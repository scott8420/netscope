// netfix -- link-local through NetworkManager (milestone 9). See LinkLocal.hpp.
// Plain GDBus (C API): the a{sa{sv}} settings are easier to walk there than
// through giomm's Variant templates, and nothing here is long-lived.
#include "netfix/LinkLocal.hpp"

#include <gio/gio.h>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>

#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

namespace netscope::netfix {

namespace {

constexpr const char* NM      = "org.freedesktop.NetworkManager";
constexpr const char* NM_PATH = "/org/freedesktop/NetworkManager";
constexpr const char* PROPS   = "org.freedesktop.DBus.Properties";

struct VarUnref { void operator()(GVariant* v) const { if (v) g_variant_unref(v); } };
using Var = std::unique_ptr<GVariant, VarUnref>;
struct ErrFree { void operator()(GError* e) const { if (e) g_error_free(e); } };

std::string str_of(GVariant* v) {  // s or o
    return v ? std::string(g_variant_get_string(v, nullptr)) : std::string();
}

// The message of a D-Bus error without GIO's "GDBus.Error:org....: " prefix.
std::string plain(GError* e) {
    if (!e) return {};
    gchar* remote = g_dbus_error_get_remote_error(e);
    if (remote) g_dbus_error_strip_remote_error(e);
    std::string s = e->message ? e->message : "";
    g_free(remote);
    return s;
}

// One-off call; returns the reply tuple or null with `err` set.
Var call(GDBusConnection* bus, const char* path, const char* iface, const char* method,
         GVariant* args, const char* reply_type, bool interactive, std::string& err,
         int timeout_ms = 25000) {
    GError* e = nullptr;
    GVariant* r = g_dbus_connection_call_sync(
        bus, NM, path, iface, method, args, reply_type ? G_VARIANT_TYPE(reply_type) : nullptr,
        interactive ? G_DBUS_CALL_FLAGS_ALLOW_INTERACTIVE_AUTHORIZATION : G_DBUS_CALL_FLAGS_NONE,
        timeout_ms, nullptr, &e);
    std::unique_ptr<GError, ErrFree> hold(e);
    if (!r) err = plain(e);
    return Var(r);
}

Var prop(GDBusConnection* bus, const char* path, const char* iface, const char* name, std::string& err) {
    Var r = call(bus, path, PROPS, "Get", g_variant_new("(ss)", iface, name), "(v)", false, err, 5000);
    if (!r) return {};
    GVariant* v = nullptr;
    g_variant_get(r.get(), "(v)", &v);
    return Var(v);
}

Var lookup(GVariant* settings, const char* group, const char* key) {
    if (!settings) return {};
    Var inner(g_variant_lookup_value(settings, group, G_VARIANT_TYPE("a{sv}")));
    if (!inner) return {};
    return Var(g_variant_lookup_value(inner.get(), key, nullptr));
}

struct Bus {
    GDBusConnection* c = nullptr;
    std::string err;
    Bus() {
        GError* e = nullptr;
        c = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, &e);
        if (!c) err = e ? e->message : "no system bus";
        if (e) g_error_free(e);
    }
    ~Bus() { if (c) g_object_unref(c); }
};

// Fills t.nm_version, t.iface, t.device_path, t.conn_path, t.conn_id, t.method,
// t.link_local; and hands back the settings. "" on success, else the sentence.
std::string resolve(GDBusConnection* bus, const std::string& want, Target& t, Var& settings) {
    std::string err;
    Var ver = prop(bus, NM_PATH, NM, "Version", err);
    if (!ver)
        return "NetworkManager isn't answering on the system bus, so NetScope can't add a 169.254 address (" +
               err + ").";
    t.nm_version = str_of(ver.get());
    if (!version_at_least(t.nm_version, 1, 40))
        return "NetworkManager " + t.nm_version +
               " is too old to run link-local beside DHCP (1.40 or later is needed).";

    std::string active;   // an active-connection path
    if (want.empty()) {
        Var pc = prop(bus, NM_PATH, NM, "PrimaryConnection", err);
        // The primary connection when it is Wi-Fi or Ethernet; else (a VPN on
        // top, or the default route on something NM doesn't manage) the first
        // active Wi-Fi / Ethernet connection.
        std::vector<std::string> order;
        if (pc && str_of(pc.get()) != "/") order.push_back(str_of(pc.get()));
        if (Var all = prop(bus, NM_PATH, NM, "ActiveConnections", err))
            for (gsize i = 0; i < g_variant_n_children(all.get()); ++i) {
                Var a(g_variant_get_child_value(all.get(), i));
                order.push_back(str_of(a.get()));
            }
        for (const auto& ac : order) {
            Var devs = prop(bus, ac.c_str(), "org.freedesktop.NetworkManager.Connection.Active", "Devices", err);
            if (!devs || g_variant_n_children(devs.get()) == 0) continue;
            Var d(g_variant_get_child_value(devs.get(), 0));
            Var type = prop(bus, str_of(d.get()).c_str(), "org.freedesktop.NetworkManager.Device", "DeviceType", err);
            const guint32 ty = type ? g_variant_get_uint32(type.get()) : 0;
            if (ty != 1 && ty != 2 && ty != 20) continue;   // NM_DEVICE_TYPE_ETHERNET, _WIFI, _VETH
            active = ac;
            t.device_path = str_of(d.get());
            break;
        }
        if (active.empty())
            return "NetworkManager has no active Wi-Fi or Ethernet connection to change.";
    } else {
        Var r = call(bus, NM_PATH, NM, "GetDeviceByIpIface", g_variant_new("(s)", want.c_str()), "(o)",
                     false, err, 5000);
        if (!r) return "NetworkManager doesn't know an interface called " + want + " (" + err + ").";
        Var d(g_variant_get_child_value(r.get(), 0));
        t.device_path = str_of(d.get());
        Var ac = prop(bus, t.device_path.c_str(), "org.freedesktop.NetworkManager.Device", "ActiveConnection", err);
        active = str_of(ac.get());
        if (active.empty() || active == "/") return want + " isn't connected, so there is no connection to change.";
    }
    Var ifn = prop(bus, t.device_path.c_str(), "org.freedesktop.NetworkManager.Device", "IpInterface", err);
    t.iface = str_of(ifn.get());
    if (t.iface.empty()) t.iface = want;

    Var conn = prop(bus, active.c_str(), "org.freedesktop.NetworkManager.Connection.Active", "Connection", err);
    t.conn_path = str_of(conn.get());
    if (t.conn_path.empty() || t.conn_path == "/") return "The active connection on " + t.iface + " has no saved profile.";

    Var r = call(bus, t.conn_path.c_str(), "org.freedesktop.NetworkManager.Settings.Connection", "GetSettings",
                 nullptr, "(a{sa{sv}})", false, err, 5000);
    if (!r) return "NetworkManager wouldn't show the connection's settings (" + err + ").";
    settings = Var(g_variant_get_child_value(r.get(), 0));
    t.conn_id    = id_of(settings.get());
    t.method     = method_of(settings.get());
    t.link_local = link_local_of(settings.get());
    return {};
}

std::string curly(const std::string& s) { return "“" + s + "”"; }

}  // namespace

// ── Pure ────────────────────────────────────────────────────────────────────

bool version_at_least(const std::string& v, int major, int minor) {
    int a = 0, b = 0;
    if (std::sscanf(v.c_str(), "%d.%d", &a, &b) < 1) return false;
    return a > major || (a == major && b >= minor);
}

GVariant* with_link_local(GVariant* settings, int value) {
    GVariantBuilder outer;
    g_variant_builder_init(&outer, G_VARIANT_TYPE("a{sa{sv}}"));
    bool had_ipv4 = false;
    if (settings) {
        GVariantIter it;
        g_variant_iter_init(&it, settings);
        const gchar* group = nullptr;
        GVariant* inner = nullptr;
        while (g_variant_iter_next(&it, "{&s@a{sv}}", &group, &inner)) {
            if (std::string(group) != "ipv4") {
                g_variant_builder_add(&outer, "{s@a{sv}}", group, inner);
            } else {
                had_ipv4 = true;
                GVariantBuilder b;
                g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
                GVariantIter kit;
                g_variant_iter_init(&kit, inner);
                const gchar* key = nullptr;
                GVariant* val = nullptr;
                while (g_variant_iter_next(&kit, "{&sv}", &key, &val)) {
                    if (std::string(key) != "link-local") g_variant_builder_add(&b, "{sv}", key, val);
                    g_variant_unref(val);
                }
                g_variant_builder_add(&b, "{sv}", "link-local", g_variant_new_int32(value));
                g_variant_builder_add(&outer, "{sa{sv}}", group, &b);
            }
            g_variant_unref(inner);
        }
    }
    if (!had_ipv4) {
        GVariantBuilder b;
        g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
        g_variant_builder_add(&b, "{sv}", "link-local", g_variant_new_int32(value));
        g_variant_builder_add(&outer, "{sa{sv}}", "ipv4", &b);
    }
    return g_variant_ref_sink(g_variant_builder_end(&outer));
}

int link_local_of(GVariant* settings) {
    Var v = lookup(settings, "ipv4", "link-local");
    return v && g_variant_is_of_type(v.get(), G_VARIANT_TYPE_INT32) ? g_variant_get_int32(v.get()) : LL_DEFAULT;
}

std::string method_of(GVariant* settings) {
    Var v = lookup(settings, "ipv4", "method");
    return v && g_variant_is_of_type(v.get(), G_VARIANT_TYPE_STRING) ? str_of(v.get()) : std::string();
}

std::string id_of(GVariant* settings) {
    Var v = lookup(settings, "connection", "id");
    return v && g_variant_is_of_type(v.get(), G_VARIANT_TYPE_STRING) ? str_of(v.get()) : std::string();
}

// ── System ──────────────────────────────────────────────────────────────────

std::string link_local_address(const std::string& iface) {
    std::string out;
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0) return out;
    for (ifaddrs* i = list; i; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET) continue;
        if (!iface.empty() && iface != i->ifa_name) continue;
        const uint32_t a = ntohl(reinterpret_cast<sockaddr_in*>(i->ifa_addr)->sin_addr.s_addr);
        if ((a >> 16) != 0xA9FE) continue;   // 169.254/16
        char buf[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(i->ifa_addr)->sin_addr, buf, sizeof buf);
        out = buf;
        break;
    }
    freeifaddrs(list);
    return out;
}

Result find(const std::string& iface) {
    Result r;
    Bus bus;
    if (!bus.c) { r.says = "No system bus: " + bus.err; return r; }
    Var settings;
    r.says = resolve(bus.c, iface, r.target, settings);
    if (!r.says.empty()) return r;
    r.ok = true;
    r.address = link_local_address(r.target.iface);
    const bool on = r.target.link_local == LL_ENABLED || r.target.method == "link-local";
    r.says = "Link-local is " + std::string(on ? "on" : "off") + " for " + curly(r.target.conn_id) + " on " +
             r.target.iface + (r.address.empty() ? "." : " (" + r.address + ").");
    return r;
}

Result set_link_local(const std::string& iface, bool on, bool interactive, std::chrono::seconds wait) {
    Result r;
    Bus bus;
    if (!bus.c) { r.says = "No system bus: " + bus.err; return r; }
    Var settings;
    r.says = resolve(bus.c, iface, r.target, settings);
    if (!r.says.empty()) return r;
    Target& t = r.target;
    const std::string where = curly(t.conn_id) + " on " + t.iface;

    if (t.method == "disabled") { r.says = "IPv4 is turned off on " + where + "; NetScope won't change that."; return r; }
    const int want = on ? LL_ENABLED : LL_DEFAULT;
    std::string err;
    bool reconnected = false;
    if (t.link_local != want) {
        Var next(with_link_local(settings.get(), want));
        // Update2(a{sa{sv}} settings, u flags = TO_DISK, a{sv} args). Settings
        // without secrets: NM keeps the stored ones (Wi-Fi password untouched).
        Var u = call(bus.c, t.conn_path.c_str(), "org.freedesktop.NetworkManager.Settings.Connection", "Update2",
                     g_variant_new("(@a{sa{sv}}u@a{sv})", next.get(), 0x1u, g_variant_new_array(G_VARIANT_TYPE("{sv}"), nullptr, 0)),
                     "(a{sv})", interactive, err, 120000);   // long: a polkit prompt may be up
        if (!u) { r.says = "NetworkManager refused to change " + where + ": " + err; return r; }
        // Apply now. Reapply keeps the link up; if NM won't, re-activate.
        Var a = call(bus.c, t.device_path.c_str(), "org.freedesktop.NetworkManager.Device", "Reapply",
                     g_variant_new("(@a{sa{sv}}tu)", g_variant_new_array(G_VARIANT_TYPE("{sa{sv}}"), nullptr, 0),
                                   (guint64)0, 0u),
                     nullptr, interactive, err, 30000);
        if (!a) {
            std::string err2;
            Var act = call(bus.c, NM_PATH, NM, "ActivateConnection",
                           g_variant_new("(ooo)", t.conn_path.c_str(), t.device_path.c_str(), "/"), "(o)",
                           interactive, err2, 60000);
            if (!act) {
                r.says = "Saved link-local for " + where + ", but NetworkManager wouldn't apply it (" + err +
                         "; " + err2 + "). Reconnecting will.";
                return r;
            }
            reconnected = true;
        }
        t.link_local = want;
    }

    if (!on) {
        r.ok = true;
        r.says = "Link-local is back to the default for " + where + ".";
        return r;
    }
    const auto until = std::chrono::steady_clock::now() + wait;
    while ((r.address = link_local_address(t.iface)).empty() && std::chrono::steady_clock::now() < until)
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    if (r.address.empty()) {
        r.says = "Turned on link-local for " + where + ", but no 169.254 address appeared within " +
                 std::to_string(wait.count()) + " seconds.";
        return r;
    }
    r.ok = true;
    r.says = "Turned on link-local for " + where + (reconnected ? " (reconnected)" : "") + ": this machine is now " +
             r.address + ", so 169.254 devices are reachable. It stays on across reconnects.";
    return r;
}

}  // namespace netscope::netfix
