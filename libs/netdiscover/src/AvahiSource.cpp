#include "netdiscover/AvahiSource.hpp"
#include "netdiscover/Aggregator.hpp"

#include <gio/gio.h>

#include <atomic>
#include <string>
#include <thread>
#include <vector>

namespace netscope::netdiscover {

namespace {
constexpr const char* kBus      = "org.freedesktop.Avahi";
constexpr const char* kServer   = "org.freedesktop.Avahi.Server";
constexpr const char* kBrowser  = "org.freedesktop.Avahi.ServiceBrowser";
constexpr int         kCallMs   = 5000;    // a sync setup call
constexpr int         kResolveMs = 15000;  // a sleepy Time Capsule may be slow (AFP_FINDINGS)

std::string take_error(GError*& e) {
    std::string m = e ? e->message : "unknown error";
    g_clear_error(&e);
    return m;
}
}  // namespace

struct AvahiSource::Impl {
    core::Inbox*             inbox = nullptr;
    GMainContext*            ctx   = nullptr;
    GMainLoop*               loop  = nullptr;
    GCancellable*            cancel = nullptr;
    GDBusConnection*         conn  = nullptr;
    std::vector<guint>       subs;
    std::vector<std::string> browsers;   // object paths, freed on the way out
    Aggregator               agg;
    std::thread              thread;

    void push(std::vector<core::FeedEvent> evs) {
        for (auto& e : evs) inbox->push(std::move(e));
    }
    void note(const std::string& text) {
        inbox->push(core::FeedEvent{core::FeedEvent::Kind::Note, {}, text});
    }

    // ── signals (on this thread's context) ──────────────────────────────────
    static void on_signal(GDBusConnection*, const gchar*, const gchar*, const gchar*,
                          const gchar* member, GVariant* params, gpointer self) {
        static_cast<Impl*>(self)->signal(member, params);
    }

    void signal(const std::string& member, GVariant* params) {
        if (member == "Failure") {
            const gchar* why = nullptr;
            g_variant_get(params, "(&s)", &why);
            note(std::string("Avahi browser failed: ") + (why ? why : "?"));
            return;
        }
        if (member != "ItemNew" && member != "ItemRemove") return;
        gint32 iface = 0, proto = 0;
        const gchar *name = nullptr, *type = nullptr, *domain = nullptr;
        guint32 flags = 0;
        g_variant_get(params, "(ii&s&s&su)", &iface, &proto, &name, &type, &domain, &flags);
        ServiceKey k{iface, proto, name, type, domain};
        if (member == "ItemRemove") { push(agg.removed(k)); return; }

        // ItemNew: resolve it (async, on this context), then aggregate.
        auto* key = new ServiceKey(k);
        g_dbus_connection_call(conn, kBus, "/", kServer, "ResolveService",
            g_variant_new("(iisssiu)", iface, proto, name, type, domain, -1, 0u),
            G_VARIANT_TYPE("(iissssisqaayu)"), G_DBUS_CALL_FLAGS_NONE, kResolveMs, cancel,
            &Impl::on_resolved, new std::pair<Impl*, ServiceKey*>(this, key));
    }

    static void on_resolved(GObject* src, GAsyncResult* res, gpointer data) {
        auto* p = static_cast<std::pair<Impl*, ServiceKey*>*>(data);
        Impl* self = p->first;
        ServiceKey key = *p->second;
        delete p->second;
        delete p;
        GError* err = nullptr;
        GVariant* v = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &err);
        if (!v) {
            // Cancelled on the way out, or a host that never answered: nothing to show.
            g_clear_error(&err);
            return;
        }
        gint32 iface, proto, aproto;
        const gchar *name, *type, *domain, *host, *address;
        guint16 port;
        GVariant* txt = nullptr;
        guint32 flags;
        g_variant_get(v, "(ii&s&s&s&si&sq@aayu)", &iface, &proto, &name, &type, &domain,
                      &host, &aproto, &address, &port, &txt, &flags);
        Resolved r{host ? host : "", address ? address : "", port, {}};
        GVariantIter it;
        g_variant_iter_init(&it, txt);
        while (GVariant* item = g_variant_iter_next_value(&it)) {
            gsize n = 0;
            const auto* bytes = static_cast<const char*>(g_variant_get_fixed_array(item, &n, 1));
            r.txt.emplace_back(bytes, n);
            g_variant_unref(item);
        }
        g_variant_unref(txt);
        g_variant_unref(v);
        self->push(self->agg.resolved(key, r));
    }

    static gboolean on_quit(gpointer loop) {
        g_main_loop_quit(static_cast<GMainLoop*>(loop));
        return G_SOURCE_REMOVE;
    }

    // ── the thread ──────────────────────────────────────────────────────────
    void run() {
        g_main_context_push_thread_default(ctx);
        setup();
        g_main_loop_run(loop);   // a quit queued by stop() is waiting for us
        teardown();
        g_main_context_pop_thread_default(ctx);
    }

    void setup() {
        GError* err = nullptr;
        gchar* addr = g_dbus_address_get_for_bus_sync(G_BUS_TYPE_SYSTEM, cancel, &err);
        if (addr)
            conn = g_dbus_connection_new_for_address_sync(addr,
                static_cast<GDBusConnectionFlags>(G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                                                  G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION),
                nullptr, cancel, &err);
        g_free(addr);
        if (!conn) { note("Can't reach the system bus: " + take_error(err)); return; }

        // Subscribe BEFORE any browser exists: a browser can announce before
        // its creation call returns. The type is in the signal's arguments,
        // so no path filter is needed.
        for (const char* m : {"ItemNew", "ItemRemove", "Failure"})
            subs.push_back(g_dbus_connection_signal_subscribe(conn, kBus, kBrowser, m, nullptr, nullptr,
                G_DBUS_SIGNAL_FLAGS_NONE, &Impl::on_signal, this, nullptr));

        GVariant* ver = g_dbus_connection_call_sync(conn, kBus, "/", kServer, "GetVersionString",
            nullptr, G_VARIANT_TYPE("(s)"), G_DBUS_CALL_FLAGS_NO_AUTO_START, kCallMs, cancel, &err);
        if (!ver) { note("Avahi isn't running (avahi-daemon): " + take_error(err)); return; }
        const gchar* vs = nullptr;
        g_variant_get(ver, "(&s)", &vs);
        const std::string version = vs ? vs : "?";
        g_variant_unref(ver);

        int ok = 0;
        for (const auto& type : browsed_types()) {
            GVariant* r = g_dbus_connection_call_sync(conn, kBus, "/", kServer, "ServiceBrowserNew",
                g_variant_new("(iissu)", -1, -1, type.c_str(), "local", 0u),
                G_VARIANT_TYPE("(o)"), G_DBUS_CALL_FLAGS_NONE, kCallMs, cancel, &err);
            if (!r) { note("Can't browse " + type + ": " + take_error(err)); continue; }
            const gchar* path = nullptr;
            g_variant_get(r, "(&o)", &path);
            browsers.emplace_back(path);
            g_variant_unref(r);
            ++ok;
        }
        note(version + ": browsing " + std::to_string(ok) + " service types");
    }

    void teardown() {
        if (!conn) return;
        for (const auto& path : browsers) {
            GVariant* r = g_dbus_connection_call_sync(conn, kBus, path.c_str(), kBrowser, "Free",
                nullptr, nullptr, G_DBUS_CALL_FLAGS_NONE, 1000, nullptr, nullptr);
            if (r) g_variant_unref(r);
        }
        browsers.clear();
        for (guint id : subs) g_dbus_connection_signal_unsubscribe(conn, id);
        subs.clear();
        // Let cancelled resolves deliver their callbacks (they free their keys).
        while (g_main_context_iteration(ctx, FALSE)) {}
        g_dbus_connection_close_sync(conn, nullptr, nullptr);
        g_object_unref(conn);
        conn = nullptr;
    }
};

AvahiSource::AvahiSource() : m(std::make_unique<Impl>()) {}

AvahiSource::~AvahiSource() { stop(); }

void AvahiSource::start(core::Inbox& inbox) {
    stop();
    m->inbox  = &inbox;
    m->ctx    = g_main_context_new();
    m->loop   = g_main_loop_new(m->ctx, FALSE);
    m->cancel = g_cancellable_new();
    m->thread = std::thread([this] { m->run(); });
}

void AvahiSource::stop() {
    if (!m->thread.joinable()) return;
    // Cut any sync setup call short, then queue the quit ON the loop's own
    // context: a bare g_main_loop_quit before the loop runs would be lost.
    g_cancellable_cancel(m->cancel);
    GSource* q = g_idle_source_new();
    g_source_set_callback(q, &Impl::on_quit, m->loop, nullptr);
    g_source_attach(q, m->ctx);
    g_source_unref(q);
    m->thread.join();
    g_main_loop_unref(m->loop);
    g_main_context_unref(m->ctx);
    g_object_unref(m->cancel);
    m->loop = nullptr; m->ctx = nullptr; m->cancel = nullptr;
    m->agg = Aggregator{};
}

}  // namespace netscope::netdiscover
