// netscoped -- NetScope's session daemon (milestone 8). No GTK.
//
// The half of the Quick Settings tile that does things (Samson's split:
// samsond <-> the tile). It lists the shares Scott pinned (choices.json, the
// app's own file, watched for changes), says which are mounted, and mounts /
// unmounts them through libnetmount -- with no window.
//
// Passwords: gvfs looks in GNOME Keyring first. If Keyring has none, the
// daemon's mount operation declines to ask and Mount returns "password"; the
// tile then asks the app (org.freedesktop.Application.ActivateAction "mount"),
// whose GTK dialog can save it. Nothing here ever holds a password.
//
// AFP (milestone 11): an afp:// pin is mounted when its FUSE mount point
// (~/<host>/<volume>, libs/netafp) is in the kernel's mount table. Unmount
// goes through netafp.
//
// AFP Mount (milestone 12): the login comes from GNOME Keyring (the app's
// "Remember the password in Keyring"); the host is resolved through Avahi
// (tc01.local -> 169.254.192.51, its IPv4 address, as the app connects), and
// netafp mounts on a thread. No saved login, or the saved one is turned down:
// "password", and the tile opens the app. Takes ~15 s cold: the tile waits.
//
//   bus name   io.github.scott8420.NetScope.Daemon
//   object     /io/github/scott8420/NetScope/Daemon
//   interface  io.github.scott8420.NetScope.Daemon1
//     property Shares     a(sssb)  uri, name, device, mounted -- pinned order
//     property AnyMounted b
//     method   Mount(s uri) -> s   "" ok | "password" | GIO's sentence
//     method   Unmount(s uri) -> s "" ok | GIO's sentence
//     method   UnmountAll() -> as  the URIs that would not unmount
//     method   Refresh()           re-read choices.json
//   PropertiesChanged on every change.
#include "core/Devices.hpp"
#include "netmount/Mounter.hpp"
#include "netafp/Afp.hpp"
#include "netafp/Keyring.hpp"

#include <giomm.h>
#include <glibmm.h>

#include <filesystem>
#include <iostream>
#include <memory>
#include <set>
#include <thread>
#include <tuple>
#include <vector>

namespace {
using namespace netscope;

const char* kName  = "io.github.scott8420.NetScope.Daemon";
const char* kPath  = "/io/github/scott8420/NetScope/Daemon";
const char* kIface = "io.github.scott8420.NetScope.Daemon1";
const char* kXml = R"xml(<node>
  <interface name="io.github.scott8420.NetScope.Daemon1">
    <property name="Shares" type="a(sssb)" access="read"/>
    <property name="AnyMounted" type="b" access="read"/>
    <method name="Mount"><arg name="uri" type="s" direction="in"/><arg name="result" type="s" direction="out"/></method>
    <method name="Unmount"><arg name="uri" type="s" direction="in"/><arg name="result" type="s" direction="out"/></method>
    <method name="UnmountAll"><arg name="failed" type="as" direction="out"/></method>
    <method name="Refresh"/>
  </interface>
</node>)xml";

using ShareRow  = std::tuple<Glib::ustring, Glib::ustring, Glib::ustring, bool>;

struct Pinned { std::string uri, name, device; };

class Daemon {
public:
    explicit Daemon(std::string choices_file) : m_file(std::move(choices_file)) {
        reload();
        m_mounter.set_on_change([this] { changed(); });
        auto f = Gio::File::create_for_path(m_file);
        try {
            m_watch = f->monitor_file();
            m_watch->signal_changed().connect([this](const auto&, const auto&, Gio::FileMonitor::Event) {
                reload();
                changed();
            });
        } catch (const Glib::Error& e) {
            std::cerr << "netscoped: can't watch " << m_file << ": " << e.what() << "\n";
        }
    }

    void own() {
        m_node = Gio::DBus::NodeInfo::create_for_xml(kXml);
        m_owner = Gio::DBus::own_name(Gio::DBus::BusType::SESSION, kName,
            [this](const Glib::RefPtr<Gio::DBus::Connection>& c, const Glib::ustring&) {
                m_conn = c;
                m_vtable = std::make_unique<Gio::DBus::InterfaceVTable>(
                    sigc::mem_fun(*this, &Daemon::on_call), sigc::mem_fun(*this, &Daemon::on_get));
                c->register_object(kPath, m_node->lookup_interface(kIface), *m_vtable);
            },
            [](const Glib::RefPtr<Gio::DBus::Connection>&, const Glib::ustring&) {
                std::cout << "netscoped: on the session bus as " << kName << "\n";
            },
            [](const Glib::RefPtr<Gio::DBus::Connection>&, const Glib::ustring&) {
                std::cerr << "netscoped: lost the bus name -- another netscoped has it, or the session ended\n";
                std::exit(1);
            });
    }

private:
    void reload() {
        core::Choices c;
        core::load_choices(m_file, c);
        m_pins.clear();
        for (const auto& [id, ch] : c.all())
            for (const auto& p : ch.pins) m_pins.push_back({p.uri, p.name, id});
    }

    static std::string afp_point(const std::string& uri) {
        std::string host, vol;
        if (!netafp::split_uri(uri, host, vol)) return {};
        return netafp::mountpoint_for(Glib::get_home_dir(), host, vol);
    }
    // One answer for both kinds: GIO's mounts for smb://, the kernel's table for afp://.
    bool mounted(const std::string& uri, const std::vector<std::string>& roots,
                 const std::vector<netafp::Mount>& afp) const {
        if (!netafp::is_afp_uri(uri)) return netmount::is_mounted(uri, roots);
        const auto point = afp_point(uri);
        for (const auto& m : afp) if (m.point == point) return true;
        return false;
    }

    std::vector<ShareRow> rows() const {
        const auto roots = m_mounter.mount_roots();
        const auto afp = netafp::mounts();
        std::vector<ShareRow> out;
        for (const auto& p : m_pins) out.emplace_back(p.uri, p.name, p.device, mounted(p.uri, roots, afp));
        return out;
    }
    bool any_mounted() const {
        for (const auto& r : rows()) if (std::get<3>(r)) return true;
        return false;
    }

    void changed() {
        if (!m_conn) return;
        std::map<Glib::ustring, Glib::VariantBase> props{
            {"Shares", Glib::Variant<std::vector<ShareRow>>::create(rows())},
            {"AnyMounted", Glib::Variant<bool>::create(any_mounted())}};
        auto body = Glib::VariantContainerBase::create_tuple({
            Glib::Variant<Glib::ustring>::create(kIface),
            Glib::Variant<std::map<Glib::ustring, Glib::VariantBase>>::create(props),
            Glib::Variant<std::vector<Glib::ustring>>::create({})});
        try {
            m_conn->emit_signal(kPath, "org.freedesktop.DBus.Properties", "PropertiesChanged", {}, body);
        } catch (const Glib::Error& e) {
            std::cerr << "netscoped: PropertiesChanged: " << e.what() << "\n";
        }
    }

    void on_get(Glib::VariantBase& out, const Glib::RefPtr<Gio::DBus::Connection>&, const Glib::ustring&,
                const Glib::ustring&, const Glib::ustring&, const Glib::ustring& prop) {
        if (prop == "Shares") out = Glib::Variant<std::vector<ShareRow>>::create(rows());
        else if (prop == "AnyMounted") out = Glib::Variant<bool>::create(any_mounted());
    }

    static void reply_s(const Glib::RefPtr<Gio::DBus::MethodInvocation>& inv, const std::string& s) {
        inv->return_value(Glib::VariantContainerBase::create_tuple(Glib::Variant<Glib::ustring>::create(s)));
    }

    // A mount operation that never shows anything: Keyring (inside gvfs) or
    // anonymous when that's offered; otherwise decline and remember we did.
    Glib::RefPtr<Gio::MountOperation> quiet_op(std::shared_ptr<bool> needs_password) {
        auto op = Gio::MountOperation::create();
        op->signal_ask_password().connect([op, needs_password](const Glib::ustring&, const Glib::ustring&,
                                                                const Glib::ustring&, Gio::AskPasswordFlags flags) {
            if ((flags & Gio::AskPasswordFlags::ANONYMOUS_SUPPORTED) == Gio::AskPasswordFlags::ANONYMOUS_SUPPORTED &&
                !*needs_password) {
                *needs_password = true;   // if anonymous is refused, the second ask aborts
                op->set_anonymous(true);
                op->reply(Gio::MountOperationResult::HANDLED);
                return;
            }
            *needs_password = true;
            op->reply(Gio::MountOperationResult::ABORTED);
        });
        return op;
    }

    void on_call(const Glib::RefPtr<Gio::DBus::Connection>&, const Glib::ustring&, const Glib::ustring&,
                 const Glib::ustring&, const Glib::ustring& method, const Glib::VariantContainerBase& params,
                 const Glib::RefPtr<Gio::DBus::MethodInvocation>& inv) {
        if (method == "Refresh") {
            reload();
            changed();
            inv->return_value(Glib::VariantContainerBase());
            return;
        }
        if (method == "UnmountAll") {
            auto failed = std::make_shared<std::vector<Glib::ustring>>();
            const auto roots = m_mounter.mount_roots();
            const auto afp = netafp::mounts();
            std::vector<std::string> todo;
            for (const auto& p : m_pins) if (mounted(p.uri, roots, afp)) todo.push_back(p.uri);
            if (todo.empty()) {
                inv->return_value(Glib::VariantContainerBase::create_tuple(Glib::Variant<std::vector<Glib::ustring>>::create({})));
                return;
            }
            auto left = std::make_shared<std::size_t>(todo.size());
            for (const auto& uri : todo) {
                if (netafp::is_afp_uri(uri)) {
                    unmount_afp(uri, [this, inv, failed, left, uri](const std::string& r) {
                        if (!r.empty()) failed->push_back(uri);
                        if (--*left == 0)
                            inv->return_value(Glib::VariantContainerBase::create_tuple(
                                Glib::Variant<std::vector<Glib::ustring>>::create(*failed)));
                    });
                    continue;
                }
                m_mounter.unmount(uri, Gio::MountOperation::create(), [inv, failed, left, uri](const netmount::Outcome& o) {
                    if (!o.ok) failed->push_back(uri);
                    if (--*left == 0)
                        inv->return_value(Glib::VariantContainerBase::create_tuple(
                            Glib::Variant<std::vector<Glib::ustring>>::create(*failed)));
                });
            }
            return;
        }
        Glib::Variant<Glib::ustring> arg;
        params.get_child(arg, 0);
        const std::string uri = arg.get();
        if (method == "Mount" && netafp::is_afp_uri(uri)) {
            mount_afp(uri, [inv](const std::string& r) { reply_s(inv, r); });
            return;
        }
        if (method == "Unmount" && netafp::is_afp_uri(uri)) {
            std::cout << "netscoped: unmount " << uri << "\n";
            unmount_afp(uri, [inv](const std::string& r) { reply_s(inv, r); });
            return;
        }
        if (method == "Mount") {
            auto needs = std::make_shared<bool>(false);
            std::cout << "netscoped: mount " << uri << "\n";
            m_mounter.mount(uri, quiet_op(needs), [inv, needs, uri](const netmount::Outcome& o) {
                const std::string r = o.ok ? "" : (o.cancelled && *needs) ? "password" : o.says;
                std::cout << "netscoped: mount " << uri << ": " << (r.empty() ? "ok" : r) << "\n";
                reply_s(inv, r);
            });
        } else if (method == "Unmount") {
            std::cout << "netscoped: unmount " << uri << "\n";
            m_mounter.unmount(uri, Gio::MountOperation::create(), [inv](const netmount::Outcome& o) {
                reply_s(inv, o.ok ? "" : o.says);
            });
        } else {
            inv->return_dbus_error("org.freedesktop.DBus.Error.UnknownMethod", "No such method: " + method);
        }
    }

    // tc01.local -> its IPv4 address. Avahi first (what discovery uses; works
    // without nss-mdns), then the system resolver, else the name itself.
    // Blocking: called on a worker thread.
    static std::string resolve(const std::string& host) {
        try {
            auto bus = Gio::DBus::Connection::get_sync(Gio::DBus::BusType::SYSTEM);
            auto args = Glib::VariantContainerBase::create_tuple({
                Glib::Variant<gint32>::create(-1), Glib::Variant<gint32>::create(0),   // any interface, IPv4
                Glib::Variant<Glib::ustring>::create(host),
                Glib::Variant<gint32>::create(0), Glib::Variant<guint32>::create(0)});
            auto r = bus->call_sync("/", "org.freedesktop.Avahi.Server", "ResolveHostName", args,
                                    "org.freedesktop.Avahi", 5000);
            Glib::Variant<Glib::ustring> addr;
            r.get_child(addr, 4);
            if (!addr.get().empty()) return addr.get();
        } catch (const Glib::Error& e) {
            std::cerr << "netscoped: avahi can't resolve " << host << ": " << e.what() << "\n";
        }
        try {
            for (const auto& a : Gio::Resolver::get_default()->lookup_by_name(host))
                if (a->get_family() == Gio::SocketFamily::IPV4) return a->to_string();
        } catch (const Glib::Error& e) {
            std::cerr << "netscoped: can't resolve " << host << ": " << e.what() << "\n";
        }
        return host;
    }

    // "" mounted (or already was) | "password" (no saved login / refused) | the sentence.
    void mount_afp(const std::string& uri, std::function<void(const std::string&)> done) {
        const std::string point = afp_point(uri);
        if (netafp::is_mounted(point)) {
            std::cout << "netscoped: mount " << uri << ": already mounted\n";
            done("");
            return;
        }
        if (!netafp::keyring::available() || !netafp::available()) {
            std::cout << "netscoped: mount " << uri << ": password (no Keyring or AFP here; the app asks)\n";
            done("password");
            return;
        }
        if (m_afp_busy.count(uri)) { done("Already mounting -- give it a few seconds."); return; }
        m_afp_busy.insert(uri);
        std::string host, vol;
        netafp::split_uri(uri, host, vol);
        std::cout << "netscoped: mount " << uri << ": looking in Keyring\n";
        std::thread([this, uri, host, vol, point, done = std::move(done)] {
            std::string r;
            const auto saved = netafp::keyring::find(host);
            if (!saved.found) {
                r = "password";
                std::cout << "netscoped: mount " << uri << ": "
                          << (saved.says.empty() ? "nothing saved" : saved.says) << " -> password\n";
            } else {
                netafp::Login login;
                login.user = saved.user;
                login.password = saved.password;
                login.address = resolve(host);
                std::cout << "netscoped: mount " << uri << ": " << saved.user << " at " << login.address << "\n";
                const auto m = netafp::mount(login, vol, point);
                r = m.ok ? "" : m.denied ? "password" : m.says;
                std::cout << "netscoped: mount " << uri << ": " << (m.ok ? "ok" : m.says) << "\n";
            }
            Glib::MainContext::get_default()->invoke([this, r, uri, done] {
                m_afp_busy.erase(uri);
                done(r);
                changed();
                return false;
            });
        }).detach();
    }

    // netafp::unmount blocks (afp_client / fusermount): a thread, the answer
    // back on the main loop. "" = unmounted, else the sentence.
    void unmount_afp(const std::string& uri, std::function<void(const std::string&)> done) {
        const std::string point = afp_point(uri);
        std::thread([this, point, uri, done = std::move(done)] {
            const auto r = netafp::unmount(point);
            Glib::MainContext::get_default()->invoke([this, r, uri, done] {
                std::cout << "netscoped: unmount " << uri << ": " << r.says << "\n";
                done(r.ok ? std::string{} : r.says);
                changed();
                return false;
            });
        }).detach();
    }

    std::string                                   m_file;
    std::vector<Pinned>                           m_pins;
    std::set<std::string>                         m_afp_busy;   // AFP mounts under way
    netmount::Mounter                             m_mounter;
    Glib::RefPtr<Gio::FileMonitor>                m_watch;
    Glib::RefPtr<Gio::DBus::NodeInfo>             m_node;
    Glib::RefPtr<Gio::DBus::Connection>           m_conn;
    std::unique_ptr<Gio::DBus::InterfaceVTable>   m_vtable;
    guint                                         m_owner = 0;
};
}  // namespace

int main(int argc, char** argv) {
    Gio::init();
    // The app's own file (Shell_bindings.cpp resolves it the same way);
    // NETSCOPE_CHOICES overrides it for tests.
    std::string file = (std::filesystem::path(Glib::get_user_data_dir()) / "netscope" / "choices.json").string();
    if (const char* e = std::getenv("NETSCOPE_CHOICES")) file = e;
    if (argc > 1 && std::string(argv[1]) == "--help") {
        std::cout << "netscoped -- NetScope's session daemon (pinned shares, mount / unmount over D-Bus)\n"
                     "reads " << file << "\n";
        return 0;
    }
    Daemon d(file);
    d.own();
    std::cout << "netscoped: pins from " << file << "\n";
    Glib::MainLoop::create()->run();
}
