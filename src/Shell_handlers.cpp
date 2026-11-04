#include "Shell.hpp"
#include "AboutWindow.hpp"
#include "ShortcutsDialog.hpp"
#include "Log.hpp"
#include "Registry.hpp"
#include "core/Recents.hpp"

#include <gtkmm/filedialog.h>
#include <giomm/asyncresult.h>
#include <giomm/file.h>

#include <glibmm/convert.h>
#include <filesystem>
#include <glibmm/markup.h>
#include <giomm/appinfo.h>

#include <algorithm>

// Shell_handlers.cpp -- HANDLERS. Slot bodies: what a user action actually does.
// Each is small and single-concern; orchestration it shares lives in helpers.

namespace netscope {

void Shell::on_pick_folder() {  // handler: folder chooser -> remember it
    auto dialog = Gtk::FileDialog::create();
    dialog->set_title("Pick a folder to remember");
    dialog->select_folder(*this,
        [this, dialog](const Glib::RefPtr<Gio::AsyncResult>& result) {
            try {
                auto dir = dialog->select_folder_finish(result);
                if (!dir) return;
                const std::string folder = dir->get_path();
                if (!folder.empty()) note_recent(folder);
            } catch (const Glib::Error& e) {
                if (auto lg = log::get(log::Area::Shell))
                    lg->info("pick-folder: dialog dismissed ({})", e.what());
            }
        });
}

void Shell::on_recent_open(const std::string& path) {  // handler: open a recent
    if (path.empty()) return;
    // A real app would load the project here. The seed just proves the routing:
    // a vanished folder self-prunes; a live one is promoted back to the front.
    if (!std::filesystem::exists(path)) {
        core::recents_remove(m_recents, path);
        core::save_recents(m_recents_file, m_recents);
        rebuild_recents_menu();
        rebuild_recents_page();
        if (auto lg = log::get(log::Area::Recents))
            lg->warn("open '{}': gone -- pruned", path);
        return;
    }
    if (auto lg = log::get(log::Area::Recents)) lg->info("open '{}'", path);
    note_recent(path);   // move-to-front
}

void Shell::on_recent_clear() {  // handler: empty the recents list
    m_recents.clear();
    core::save_recents(m_recents_file, m_recents);
    rebuild_recents_menu();
    rebuild_recents_page();
}

void Shell::on_dump_registry() {  // handler: print the live widget tree
    // The debuggability payoff, on demand. Same call works from a gdb prompt.
    registry::dump();
}

void Shell::on_shortcuts() {  // handler: open the keyboard reference
    // Same lifetime stone as About: build once, re-present. The window's CONTENT
    // is not written here -- it renders by walking core::shortcut_registry(),
    // the same list bind_accelerators() wires from, so the two can't drift.
    if (!m_shortcuts) m_shortcuts = std::make_unique<ShortcutsDialog>();
    m_shortcuts->show(*this);
}

void Shell::on_about() {  // handler: open the About window (dialog-lifetime exemplar)
    // Lazily build once, then re-present. Hide-on-close singleton (CANON:
    // "Lifetime shape is design") -- the X hides it, so one instance lives for
    // the session and its named children never re-register.
    if (!m_about) m_about = std::make_unique<AboutWindow>(*this);
    m_about->present();
}

// ─────────────────────────────────────────────────────────────────────────────
// s002 -- the Devices page's handlers. Each choice is ONE call into
// core::devices (which goes through the journal). None of them writes
// m_choices directly: the selftest's gate would not see a handler that did,
// so the rule is kept here by hand -- handlers call verbs.
// ─────────────────────────────────────────────────────────────────────────────
void Shell::on_undo() {  // handler: take the last step back
    const auto r = m_journal.undo();
    if (!r.done) return;
    m_sel = r.selection.empty() ? std::string{} : r.selection.front();
    m_undo_said.set_text("Undid: " + r.label);
    select_device(m_sel, true);
    if (auto lg = log::get(log::Area::Shell)) lg->info("undo: {}", r.label);
}

void Shell::on_redo() {  // handler: put the undone step back
    const auto r = m_journal.redo();
    if (!r.done) return;
    m_sel = r.selection.empty() ? std::string{} : r.selection.front();
    m_undo_said.set_text("Redid: " + r.label);
    select_device(m_sel, true);
    if (auto lg = log::get(log::Area::Shell)) lg->info("redo: {}", r.label);
}

void Shell::on_device_favourite() {  // handler: star / unstar
    if (m_sel.empty()) return;
    const bool on = !m_choices.of(m_sel).favourite;
    if (core::devices::set_favourite(m_journal, m_choices, m_net, m_sel, on)) select_device(m_sel, true);
}

void Shell::on_device_hide() {  // handler: hide / show
    if (m_sel.empty()) return;
    const bool on = !m_choices.of(m_sel).hidden;
    // Hiding takes the row off the list (unless Show hidden): the selection
    // stays on it, so Ctrl+Z right away brings it back where it was.
    if (core::devices::set_hidden(m_journal, m_choices, m_net, m_sel, on)) select_device(m_sel, true);
}

void Shell::on_device_rename() {  // handler: Enter in the nickname field, or leaving it
    if (m_painting || m_sel.empty()) return;
    core::devices::rename(m_journal, m_choices, m_net, m_sel, m_device_name.get_text());
}

void Shell::on_device_diagnose() {  // handler: the checks, off the main thread
    if (m_sel.empty() || m_diag_thread.joinable()) return;
    const auto seen = m_net.get(m_sel);
    if (!seen) return;
    const core::Sighting s = seen->sighting;
    m_diag_for  = s.id;
    m_diag_name = s.name.empty() ? s.id : s.name;
    m_diag_button.set_sensitive(false);
    m_diag_said.set_text("Checking " + m_diag_name + "\u2026");
    if (auto lg = log::get(log::Area::Devices)) lg->info("diagnose {}", s.id);
    m_diag_thread = std::jthread([this, s] {
        auto found = netdiag::diagnose(s, netdiag::local_facts());
        { std::lock_guard l(m_diag_mu); m_diag_findings = std::move(found); }
        m_diag_ready.emit();
    });
}

void Shell::on_diagnose_done() {  // handler: main thread -- the findings
    if (m_diag_thread.joinable()) m_diag_thread.join();   // it has finished: it just emitted
    std::vector<netdiag::Finding> found;
    { std::lock_guard l(m_diag_mu); found.swap(m_diag_findings); }
    std::string text;
    if (!m_fix_note.empty()) text = Glib::Markup::escape_text(m_fix_note) + "\n\n";   // milestone 9
    m_fix_note.clear();
    text += "<b>" + Glib::Markup::escape_text(m_diag_name) + "</b>";
    for (const auto& f : found) {
        const char* mark = f.verdict == netdiag::Verdict::Ok   ? "\u2713" :
                           f.verdict == netdiag::Verdict::Warn ? "!" :
                           f.verdict == netdiag::Verdict::Fail ? "\u2717" : "\u2013";
        text += std::string("\n") + mark + "  " + Glib::Markup::escape_text(f.says);
        if (auto lg = log::get(log::Area::Devices)) lg->info("  {}: {}", f.check, f.says);
    }
    m_diag_said.set_markup(text);
    m_diag_button.set_sensitive(!m_sel.empty());
    const bool offer = std::any_of(found.begin(), found.end(), [](const netdiag::Finding& f) { return f.fix == "link-local"; });
    m_fix_button.set_visible(offer);
    m_fix_button.set_sensitive(offer && !m_fix_thread.joinable());
}

// ── Milestone 9 -- the Fix button ──────────────────────────────────────────
// NetScope runs as the user and can't add an address; NetworkManager can, and
// lets the logged-in user change their own connection (polkit; GNOME prompts
// if it wants a password). See libs/netfix.
void Shell::on_device_fix() {  // handler: link-local via NM, off the main thread
    if (m_fix_thread.joinable()) return;
    m_fix_button.set_sensitive(false);
    m_diag_said.set_text("Asking NetworkManager to turn on link-local\u2026");
    if (auto lg = log::get(log::Area::Devices)) lg->info("fix: link-local on the primary connection");
    m_fix_thread = std::jthread([this] {
        auto r = netfix::set_link_local("", true);
        { std::lock_guard l(m_fix_mu); m_fix_result = std::move(r); }
        m_fix_ready.emit();
    });
}

void Shell::on_fix_done() {  // handler: main thread -- say what NM did; re-diagnose on success
    if (m_fix_thread.joinable()) m_fix_thread.join();   // it has finished: it just emitted
    netfix::Result r;
    { std::lock_guard l(m_fix_mu); r = std::move(m_fix_result); }
    if (auto lg = log::get(log::Area::Devices))
        lg->info("fix: {} ({}, {}, link-local {})", r.says, r.target.iface, r.target.conn_path, r.target.link_local);
    if (!r.ok) {
        m_diag_said.set_text(r.says);
        m_fix_button.set_sensitive(true);
        return;
    }
    m_fix_button.set_visible(false);
    m_fix_note = r.says;
    on_device_diagnose();                                   // the route line should now read "directly on <iface>"
}

void Shell::on_show_hidden() {  // handler: a view setting -- not a step
    if (m_painting) return;
    m_show_hidden = m_show_hidden_check.get_active();
    queue_devices_repaint();
}

// ── Milestone 6 -- shares ──────────────────────────────────────────────────
// Mounts are not choices (ARCHITECTURE): only Pin / Unpin touches the journal.
// GIO answers on the main loop, so these handlers start work and return; the
// answers repaint. The password dialog is GTK's own: "Remember forever" puts
// the password in GNOME Keyring through gvfs, and gvfs finds it there next time.

void Shell::on_device_shares() {  // handler: list the selected device's shares (AFP first, else SMB)
    const auto seen = m_sel.empty() ? std::nullopt : m_net.get(m_sel);
    if (!seen || !m_mounter) return;
    const auto& s = seen->sighting;
    const auto schemes = netmount::schemes_of(s);
    const bool smb = std::find(schemes.begin(), schemes.end(), "smb") != schemes.end();
    // Milestone 10: a device that speaks AFP is asked over AFP -- the Time
    // Capsule's SMB is SMB1, which current systems refuse.
    if (device_has_afp(s)) {
        if (netafp::available()) { list_afp_volumes(s); return; }
        if (!smb) { m_share_notes[s.id] = netafp::missing(); repaint_shares(); return; }
    }
    if (!smb) {
        m_share_notes[s.id] = "No AFP or SMB here. (SFTP and NFS come with a later milestone.)";
        repaint_shares();
        return;
    }
    const auto root = netmount::root_uri("smb", netmount::host_for(s));
    if (m_share_busy.count(root)) return;
    m_share_busy.insert(root);
    m_share_notes[s.id] = "Asking " + root + "…";
    repaint_shares();
    if (auto lg = log::get(log::Area::Mounts)) lg->info("shares {}: listing {}", s.id, root);
    const std::string id = s.id;
    m_mounter->list_shares(root, mount_op(),
        [this, id, root](const std::vector<netmount::Share>& found, const netmount::Outcome& o) {
            m_share_busy.erase(root);
            auto lg = log::get(log::Area::Mounts);
            if (o.ok) {
                m_shares[id] = found;
                m_share_notes[id] = found.empty() ? "No shares offered." :
                                    std::to_string(found.size()) + (found.size() == 1 ? " share." : " shares.");
                if (lg) lg->info("shares {}: {}", id, found.size());
            } else {
                m_share_notes[id] = o.cancelled ? "" : "Couldn't list shares: " + o.says;
                if (lg) lg->warn("shares {}: {}", id, o.says);
            }
            repaint_shares();
        });
}

void Shell::on_share_mount(const std::string& uri) {  // handler: mount one share
    if (netafp::is_afp_uri(uri)) { mount_afp(uri); return; }   // milestone 10
    if (!m_mounter || m_share_busy.count(uri)) return;
    m_share_busy.insert(uri);
    const std::string id = m_sel;
    m_share_notes[id] = "Mounting " + uri + "…";
    repaint_shares();
    if (auto lg = log::get(log::Area::Mounts)) lg->info("mount {}", uri);
    m_mounter->mount(uri, mount_op(), [this, id, uri](const netmount::Outcome& o) {
        m_share_busy.erase(uri);
        m_share_notes[id] = o.ok ? "Mounted: it's in Files now." : o.cancelled ? "" : "Couldn't mount: " + o.says;
        if (auto lg = log::get(log::Area::Mounts)) lg->info("mount {}: {}", uri, o.ok ? "ok" : o.says);
        repaint_shares();
    });
}

void Shell::on_share_unmount(const std::string& uri) {  // handler: unmount it
    if (netafp::is_afp_uri(uri)) { unmount_afp(uri); return; }   // milestone 10
    if (!m_mounter || m_share_busy.count(uri)) return;
    m_share_busy.insert(uri);
    const std::string id = m_sel;
    m_share_notes[id] = "Unmounting…";
    repaint_shares();
    if (auto lg = log::get(log::Area::Mounts)) lg->info("unmount {}", uri);
    m_mounter->unmount(uri, mount_op(), [this, id, uri](const netmount::Outcome& o) {
        m_share_busy.erase(uri);
        m_share_notes[id] = o.ok ? "Unmounted." : o.cancelled ? "" : "Couldn't unmount: " + o.says;
        if (auto lg = log::get(log::Area::Mounts)) lg->info("unmount {}: {}", uri, o.ok ? "ok" : o.says);
        repaint_shares();
    });
}

void Shell::mount_share(const std::string& uri) {  // handler: app.mount from the tile
    // The tile can start NetScope just to ask for a password: then discovery
    // hasn't found the device yet. Hold the request until it shows up
    // (drain_feed), or say so after 20 s -- never drop it silently.
    std::string dev;
    for (const auto& [id, c] : m_choices.all())
        if (core::devices::is_pinned(m_choices, id, uri)) { dev = id; break; }
    if (!dev.empty() && !m_net.get(dev)) {
        m_pending_mount = uri;
        m_pending_dev = dev;
        if (auto lg = log::get(log::Area::Mounts)) lg->info("mount {}: waiting for {} to be found", uri, dev);
        m_pending_timeout.disconnect();
        m_pending_timeout = Glib::signal_timeout().connect_seconds([this, dev] {
            if (m_pending_mount.empty()) return false;
            if (auto lg = log::get(log::Area::Mounts)) lg->warn("mount {}: {} not found", m_pending_mount, dev);
            m_share_notes[m_sel] = netafp::short_host(dev) + " hasn't shown up on the network, so " +
                                   "NetScope can't mount it yet. Is it awake?";
            m_pending_mount.clear();
            m_pending_dev.clear();
            repaint_shares();
            return false;
        }, 20);
        return;
    }
    if (!dev.empty()) { m_sel = dev; select_device(dev, false); }
    on_share_mount(uri);
}

void Shell::on_share_pin(const netmount::Share& share, bool on) {  // handler: pin / unpin -- a choice, an undo step
    if (m_sel.empty()) return;
    core::devices::set_pinned(m_journal, m_choices, m_net, m_sel, core::Pin{share.name, share.uri}, on);
}

void Shell::on_share_open(const std::string& uri) {  // handler: open it in the file manager
    try {
        // AFP: the FUSE mount point, not the afp:// URI (GVfs's AFP is the path that fails).
        const std::string where = netafp::is_afp_uri(uri) ? std::string(Glib::filename_to_uri(afp_point(uri))) : uri;
        Gio::AppInfo::launch_default_for_uri(where);
        if (auto lg = log::get(log::Area::Mounts)) lg->info("open {}", uri);
    } catch (const Glib::Error& e) {
        m_share_notes[m_sel] = "Couldn't open it: " + std::string(e.what());
        if (auto lg = log::get(log::Area::Mounts)) lg->warn("open {}: {}", uri, e.what());
        repaint_shares();
    }
}

}  // namespace netscope
