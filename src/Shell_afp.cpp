// Shell -- milestone 10: AFP volumes through Netatalk Client (libs/netafp).
//
// AFP volumes are rows in the same Shares list as SMB (afp://tc01.local/Data/),
// so Pin, Mount, Unmount and Open work the same. What differs:
//   * the login is NetScope's own small dialog (GVfs's AFP login is the path
//     that fails against the Time Capsule). Milestone 12: a login is looked up
//     in memory, then in GNOME Keyring (netafp/Keyring.hpp), and only then
//     asked; the dialog's "Remember in Keyring" saves it once it has worked,
//     so netscoped -- and the tile -- can mount with no window;
//   * the work blocks (libafpsl, afp_client), so each job runs on its own
//     thread and hands a continuation back through m_afp_ready;
//   * a volume is mounted at ~/<host>/<volume> ("~/tc01/Data").
// Mounts are not choices: no journal steps (ARCHITECTURE).
#include "Shell.hpp"
#include "Log.hpp"
#include "netafp/Keyring.hpp"

#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/checkbutton.h>
#include <gtkmm/entry.h>
#include <gtkmm/grid.h>
#include <gtkmm/label.h>
#include <gtkmm/window.h>
#include <glibmm/main.h>
#include <glibmm/markup.h>
#include <glibmm/miscutils.h>
#include <giomm/appinfo.h>
#include <giomm/file.h>

namespace netscope {

namespace {
// Where to connect: the first IPv4 address (no mDNS lookup needed), else the name.
std::string afp_address(const core::Sighting& s) {
    for (const auto& a : s.addresses)
        if (a.find('.') != std::string::npos && a.find(':') == std::string::npos) return a;
    return s.id;
}
}  // namespace

bool Shell::device_has_afp(const core::Sighting& s) const {  // helper
    for (const auto& svc : s.services) if (svc.kind == "afp") return true;
    return false;
}

std::string Shell::afp_point(const std::string& uri) const {  // helper: ~/tc01/Data, "" if not an AFP volume
    std::string host, vol;
    if (!netafp::split_uri(uri, host, vol)) return {};
    return netafp::mountpoint_for(Glib::get_home_dir(), host, vol);
}

void Shell::run_afp(std::function<std::function<void()>()> work) {  // helper: work off the main thread, continuation on it
    // Finished threads are joined here (each has already handed back its continuation).
    for (auto it = m_afp_jobs.begin(); it != m_afp_jobs.end();) {
        if (it->done && *it->done) { it->thread.join(); it = m_afp_jobs.erase(it); }
        else ++it;
    }
    auto done = std::make_shared<std::atomic<bool>>(false);
    m_afp_jobs.push_back({std::jthread([this, work = std::move(work), done] {
        auto then = work();
        { std::lock_guard l(m_afp_mu); m_afp_then.push_back(std::move(then)); }
        *done = true;
        m_afp_ready.emit();
    }), done});
}

void Shell::on_afp_ready() {  // handler: main thread -- run what the AFP threads handed back
    std::vector<std::function<void()>> todo;
    { std::lock_guard l(m_afp_mu); todo.swap(m_afp_then); }
    for (auto& f : todo) if (f) f();
}

void Shell::ask_afp_login(const std::string& host, const std::string& why,
                          std::function<void(const netafp::Login&)> then) {  // helper: NetScope's login dialog
    if (m_afp_dialog) { m_afp_dialog->present(); return; }
    auto win = std::make_unique<Gtk::Window>();
    win->set_name("shell.afp_login");
    win->set_title("Connect to " + netafp::short_host(host));
    win->set_transient_for(*this);
    win->set_modal(true);
    win->set_resizable(false);
    win->set_hide_on_close(true);

    auto box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
    box->set_margin(18);
    auto head = Gtk::make_managed<Gtk::Label>();
    head->set_markup("<b>" + Glib::Markup::escape_text(netafp::short_host(host)) + "</b> wants a user name and password (AFP).");
    head->set_xalign(0.0f);
    box->append(*head);
    if (!why.empty()) {
        auto w = Gtk::make_managed<Gtk::Label>(why);
        w->set_xalign(0.0f);
        w->add_css_class("error");
        box->append(*w);
    }
    auto grid = Gtk::make_managed<Gtk::Grid>();
    grid->set_row_spacing(6);
    grid->set_column_spacing(12);
    auto ul = Gtk::make_managed<Gtk::Label>("User");
    ul->set_xalign(1.0f);
    auto user = Gtk::make_managed<Gtk::Entry>();
    user->set_name("shell.afp_login.user");
    const auto known = m_afp_users.find(host);
    user->set_text(known != m_afp_users.end() ? known->second : std::string(Glib::get_user_name()));
    user->set_hexpand(true);
    auto pl = Gtk::make_managed<Gtk::Label>("Password");
    pl->set_xalign(1.0f);
    auto pass = Gtk::make_managed<Gtk::Entry>();
    pass->set_name("shell.afp_login.password");
    pass->set_visibility(false);
    pass->set_input_purpose(Gtk::InputPurpose::PASSWORD);
    grid->attach(*ul, 0, 0);
    grid->attach(*user, 1, 0);
    grid->attach(*pl, 0, 1);
    grid->attach(*pass, 1, 1);
    box->append(*grid);
    const bool keyring = netafp::keyring::available();
    auto remember = Gtk::make_managed<Gtk::CheckButton>("Remember the password in Keyring");
    remember->set_name("shell.afp_login.remember");
    remember->set_active(keyring);
    remember->set_sensitive(keyring);
    remember->set_visible(keyring);
    box->append(*remember);
    auto hint = Gtk::make_managed<Gtk::Label>(keyring
        ? "Empty user name = guest. Remembered, the Quick Settings tile mounts with no window."
        : "Empty user name = guest. NetScope keeps the password until it quits.");
    hint->add_css_class("dim-label");
    hint->set_xalign(0.0f);
    box->append(*hint);

    auto buttons = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
    buttons->set_halign(Gtk::Align::END);
    auto cancel = Gtk::make_managed<Gtk::Button>("Cancel");
    auto connect = Gtk::make_managed<Gtk::Button>("Connect");
    connect->set_name("shell.afp_login.connect");
    connect->add_css_class("suggested-action");
    buttons->append(*cancel);
    buttons->append(*connect);
    box->append(*buttons);
    win->set_child(*box);

    auto finish = [this, host](bool ok, netafp::Login login, std::function<void(const netafp::Login&)> then) {
        if (ok) m_afp_users[host] = login.user;
        m_afp_from_keyring.erase(host);   // typed, not saved: a refusal is about what was typed
        if (m_afp_dialog) m_afp_dialog->set_visible(false);
        // Destroy the window after its own handler has returned.
        Glib::signal_idle().connect_once([this] { m_afp_dialog.reset(); });
        if (ok && then) then(login);
    };
    connect->signal_clicked().connect([this, host, finish, user, pass, remember, then] {
        m_afp_remember[host] = remember->get_active();
        netafp::Login l;
        l.user = user->get_text();
        l.password = pass->get_text();
        finish(true, std::move(l), then);
    });
    pass->signal_activate().connect([connect] { connect->activate(); });
    cancel->signal_clicked().connect([finish] { finish(false, {}, nullptr); });
    win->signal_close_request().connect([finish] { finish(false, {}, nullptr); return true; }, false);

    m_afp_dialog = std::move(win);
    m_afp_dialog->present();
    // A filled-in user name (the last one, or the system's) wants only the password.
    (!user->get_text().empty() ? static_cast<Gtk::Widget*>(pass) : static_cast<Gtk::Widget*>(user))->grab_focus();
}

void Shell::with_afp_login(const std::string& host, const std::string& why,
                           std::function<void(const netafp::Login&)> then) {  // helper: memory -> Keyring -> dialog
    if (why.empty()) {
        const auto known = m_afp_logins.find(host);
        if (known != m_afp_logins.end()) { then(known->second); return; }
        if (netafp::keyring::available()) {
            run_afp([this, host, then] {
                auto saved = netafp::keyring::find(host);
                return std::function<void()>([this, host, then, saved] {
                    if (auto lg = log::get(log::Area::Mounts))
                        lg->info("afp {}: keyring {}", host, saved.found ? "has " + saved.user
                                                            : saved.says.empty() ? std::string("has nothing") : saved.says);
                    if (saved.found) {
                        netafp::Login l;
                        l.user = saved.user;
                        l.password = saved.password;
                        m_afp_users[host] = saved.user;
                        m_afp_from_keyring.insert(host);
                        then(l);
                        return;
                    }
                    ask_afp_login(host, "", then);
                });
            });
            return;
        }
    }
    ask_afp_login(host, why, then);
}

void Shell::afp_login_worked(const std::string& host, const netafp::Login& login) {  // helper
    m_afp_logins[host] = login;
    const auto r = m_afp_remember.find(host);
    if (r == m_afp_remember.end() || !netafp::keyring::available()) return;   // from memory or Keyring: nothing new
    const bool keep = r->second;
    m_afp_remember.erase(r);
    const std::string id = m_sel;
    run_afp([this, host, login, keep, id] {
        const std::string err = keep ? netafp::keyring::save(host, login.user, login.password)
                                     : netafp::keyring::forget(host);
        return std::function<void()>([this, host, login, keep, id, err] {
            if (auto lg = log::get(log::Area::Mounts))
                lg->info("afp {}: keyring {} {}", host, keep ? "save" : "forget", err.empty() ? "ok" : err);
            if (!keep || login.user.empty()) return;
            m_share_notes[id] += err.empty() ? " Password saved in Keyring." : " " + err;
            repaint_shares();
        });
    });
}

// The sentence for a refusal: say when it was Keyring's password that failed.
static std::string refused(const std::string& says, bool from_keyring) {
    return from_keyring ? says + " (the password saved in Keyring)" : says;
}

void Shell::list_afp_volumes(const core::Sighting& s, const std::string& why) {  // helper: login -> volumes
    const std::string id = s.id, host = s.id, address = afp_address(s);
    const std::string root = "afp://" + host + "/";
    with_afp_login(host, why, [this, id, host, address, root](const netafp::Login& entered) {
        netafp::Login login = entered;
        login.address = address;
        m_share_busy.insert(root);
        m_share_notes[id] = "Logging in to " + netafp::short_host(host) + "…";
        repaint_shares();
        if (auto lg = log::get(log::Area::Mounts)) lg->info("afp {}: listing volumes at {} as {}", id, address,
                                                            login.user.empty() ? "guest" : login.user);
        run_afp([this, id, host, root, login] {
            auto r = netafp::list_volumes(login);
            return std::function<void()>([this, id, host, root, login, r] {
                m_share_busy.erase(root);
                if (auto lg = log::get(log::Area::Mounts)) lg->info("afp {}: {}", id, r.says);
                if (r.ok) {
                    afp_login_worked(host, login);
                    std::vector<netmount::Share> found;
                    for (const auto& v : r.volumes) found.push_back({v, netafp::volume_uri(host, v)});
                    m_shares[id] = found;
                    m_share_notes[id] = r.says;
                } else {
                    m_afp_logins.erase(host);
                    m_share_notes[id] = r.says;
                    if (r.denied) {
                        const bool kr = m_afp_from_keyring.erase(host) > 0;
                        repaint_shares();
                        if (const auto seen = m_net.get(id)) list_afp_volumes(seen->sighting, refused(r.says, kr));   // ask again
                        return;
                    }
                }
                repaint_shares();
            });
        });
    });
}

void Shell::mount_afp(const std::string& uri, const std::string& why) {  // helper: mount one AFP volume
    std::string host, vol;
    if (!netafp::split_uri(uri, host, vol) || m_share_busy.count(uri)) return;
    const std::string id = m_sel;
    const auto seen = m_net.get(id);
    if (!seen) {
        m_share_notes[id] = netafp::short_host(host) + " isn't on the network right now.";
        repaint_shares();
        return;
    }
    const std::string address = afp_address(seen->sighting);
    const std::string point = afp_point(uri);
    auto go = [this, id, uri, host, vol, address, point](netafp::Login login) {
        login.address = address;
        m_share_busy.insert(uri);
        m_share_notes[id] = "Mounting " + vol + " (Netatalk Client takes about 15 seconds)…";
        repaint_shares();
        if (auto lg = log::get(log::Area::Mounts)) lg->info("afp mount {} at {}", uri, point);
        run_afp([this, id, uri, host, vol, login, point] {
            auto r = netafp::mount(login, vol, point);
            return std::function<void()>([this, id, uri, host, login, point, r] {
                m_share_busy.erase(uri);
                if (auto lg = log::get(log::Area::Mounts)) lg->info("afp mount {}: {}", uri, r.says);
                if (r.ok) {
                    m_share_notes[id] = "Mounted at " + point + ": it's in Files now.";
                    afp_login_worked(host, login);
                } else if (r.denied) {
                    m_afp_logins.erase(host);
                    const bool kr = m_afp_from_keyring.erase(host) > 0;
                    m_share_notes[id] = r.says;
                    repaint_shares();
                    mount_afp(uri, refused(r.says, kr));   // ask again
                    return;
                } else {
                    m_share_notes[id] = "Couldn't mount: " + r.says;
                }
                repaint_shares();
            });
        });
    };
    with_afp_login(host, why, go);
}

void Shell::unmount_afp(const std::string& uri) {  // helper
    const std::string point = afp_point(uri);
    if (point.empty() || m_share_busy.count(uri)) return;
    const std::string id = m_sel;
    m_share_busy.insert(uri);
    m_share_notes[id] = "Unmounting…";
    repaint_shares();
    run_afp([this, id, uri, point] {
        auto r = netafp::unmount(point);
        return std::function<void()>([this, id, uri, r] {
            m_share_busy.erase(uri);
            if (auto lg = log::get(log::Area::Mounts)) lg->info("afp unmount {}: {}", uri, r.says);
            m_share_notes[id] = r.ok ? "Unmounted." : "Couldn't unmount: " + r.says;
            repaint_shares();
        });
    });
}

}  // namespace netscope
