#include "Shell.hpp"
#include "core/Launch.hpp"
#include "core/Recents.hpp"
#include "Log.hpp"

#include <gtkmm/label.h>
#include <gtkmm/box.h>
#include <gtkmm/listboxrow.h>
#include <gtkmm/editable.h>
#include <gtkmm/textview.h>
#include <glibmm/main.h>
#include <giomm/menuitem.h>
#include <glibmm/miscutils.h>
#include <glibmm/variant.h>
#include <glibmm/markup.h>
#include <gtk/gtk.h>   // GtkMountOperation: gtkmm 4 has no wrapper

#include <filesystem>

// Shell_helpers.cpp -- HELPERS. Reusable orchestration called from zones,
// bindings, and handlers: the recents seam (path resolution, persist, and the
// two-surface repaint).

namespace netscope {

std::string Shell::recents_file() const {  // helper: XDG path (UI-side resolution)
    // The UI resolves the XDG dir (Glib here) and hands the core a plain path --
    // the core stays GTK-free (docs/two-layer-seam.md).
    return (std::filesystem::path(Glib::get_user_data_dir()) / "netscope" / "recent.json")
        .string();
}

void Shell::note_recent(const std::string& path) {  // helper: push + persist + repaint
    if (path.empty()) return;
    core::recents_add(m_recents, path);
    core::save_recents(m_recents_file, m_recents);
    rebuild_recents_menu();
    rebuild_recents_page();
}

void Shell::rebuild_recents_menu() {  // helper: repaint the submenu
    if (!m_recents_menu) return;
    m_recents_menu->remove_all();

    // Display name = folder basename; identical basenames get their parent dir
    // appended so they're distinguishable (Curvz's disambiguation).
    std::vector<std::string> names(m_recents.size());
    for (std::size_t i = 0; i < m_recents.size(); ++i)
        names[i] = std::filesystem::path(m_recents[i]).filename().string();
    for (std::size_t i = 0; i < names.size(); ++i)
        for (std::size_t j = i + 1; j < names.size(); ++j)
            if (names[i] == names[j]) {
                const std::string pi =
                    std::filesystem::path(m_recents[i]).parent_path().filename().string();
                const std::string pj =
                    std::filesystem::path(m_recents[j]).parent_path().filename().string();
                if (!pi.empty()) names[i] += "  (" + pi + ")";
                if (!pj.empty()) names[j] += "  (" + pj + ")";
            }

    for (std::size_t i = 0; i < m_recents.size(); ++i) {
        auto item = Gio::MenuItem::create(names[i], "");
        item->set_action_and_target(
            "recents.open", Glib::Variant<Glib::ustring>::create(m_recents[i]));
        m_recents_menu->append_item(item);
    }

    auto clear_section = Gio::Menu::create();
    clear_section->append("Clear recent", "recents.clear");
    m_recents_menu->append_section("", clear_section);
    if (m_recents_clear) m_recents_clear->set_enabled(!m_recents.empty());
}

void Shell::rebuild_recents_page() {  // helper: repaint the live list page
    // Clear existing rows.
    while (auto* row = m_recents_list.get_row_at_index(0))
        m_recents_list.remove(*row);

    if (m_recents.empty()) {
        auto* empty = Gtk::make_managed<Gtk::Label>("(nothing remembered yet)");
        empty->set_xalign(0.0f);
        empty->set_margin(12);
        m_recents_list.append(*empty);
        return;
    }
    for (const auto& p : m_recents) {
        auto* lbl = Gtk::make_managed<Gtk::Label>(p);
        lbl->set_xalign(0.0f);
        lbl->set_margin(8);
        lbl->set_selectable(true);
        m_recents_list.append(*lbl);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// s002 -- the Devices page's helpers.
// ─────────────────────────────────────────────────────────────────────────────

// The main-thread half of the feed seam. The source's thread pushed into the
// inbox and emitted the dispatcher; here, on the main loop, take everything
// queued and apply it. The network's on_change queues the one repaint.
void Shell::drain_feed() {  // helper
    const auto events = m_inbox.drain();
    const int changed = core::apply(m_net, events);
    for (const auto& e : events) {
        auto lg = log::get(log::Area::Devices);
        if (e.kind == core::FeedEvent::Kind::Note) {
            m_feed_note = e.note;
            if (lg) lg->info("note: {}", e.note);
            queue_devices_repaint();
        } else if (lg) {
            lg->info("{} {}", e.kind == core::FeedEvent::Kind::Seen ? "seen" : e.kind == core::FeedEvent::Kind::Merged ? "merged" : "lost", e.sighting.id);
        }
    }
    if (changed > 0 && m_sel.empty()) {
        // First arrival: select the top row so the keyboard has somewhere to be.
        const auto rs = core::rows(m_net, m_choices, m_show_hidden);
        if (!rs.empty()) select_device(rs.front().id, false);
    }
    // A mount the tile asked for before its device was found (mount_share).
    if (!m_pending_mount.empty() && m_net.get(m_pending_dev)) {
        const std::string uri = m_pending_mount;
        m_pending_mount.clear();
        m_pending_timeout.disconnect();
        if (auto lg = log::get(log::Area::Mounts)) lg->info("mount {}: {} found, mounting", uri, m_pending_dev);
        Glib::signal_idle().connect_once([this, uri] { mount_share(uri); });
    }
}

// One repaint per idle, AFTER the writes that caused it. A burst of feed
// events, or a verb, would otherwise rebuild the list per write -- and a row
// rebuilt under a live click or a focused row is torn down (jot s045).
void Shell::queue_devices_repaint() {  // helper
    if (m_repaint_queued) return;
    m_repaint_queued = true;
    Glib::signal_idle().connect_once([this]() {
        m_repaint_queued = false;
        repaint_devices();
    });
}

namespace {
std::string esc(const std::string& s) { return Glib::Markup::escape_text(s); }

std::string join(const std::vector<std::string>& v, const char* sep) {
    std::string o;
    for (const auto& x : v) { if (!o.empty()) o += sep; o += x; }
    return o;
}
}  // namespace

// The list and the detail fields, from the model. Setting a field here is not
// a user choice: m_painting keeps the field's own signal from writing it back.
void Shell::repaint_devices() {  // helper
    m_painting = true;
    while (auto* c = m_device_list.get_first_child()) m_device_list.remove(*c);
    Gtk::ListBoxRow* chosen = nullptr;
    for (const auto& r : core::rows(m_net, m_choices, m_show_hidden)) {
        auto row = Gtk::make_managed<Gtk::ListBoxRow>();
        row->set_name(r.id);   // the row carries its device's id
        auto box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 10);
        box->set_margin(6);

        auto star = Gtk::make_managed<Gtk::Label>(r.favourite ? "\u2605" : "");
        star->set_size_request(16, -1);
        box->append(*star);

        auto text = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
        text->set_hexpand(true);
        auto title = Gtk::make_managed<Gtk::Label>();
        title->set_xalign(0.0f);
        title->set_markup("<b>" + esc(r.title) + "</b>" + (r.hidden ? "  <i>(hidden)</i>" : ""));
        text->append(*title);
        std::vector<std::string> sub;
        if (r.title != r.advertised) sub.push_back(r.advertised);
        if (!r.model.empty())        sub.push_back(r.model);
        if (!r.services.empty())     sub.push_back(r.services);
        auto subtitle = Gtk::make_managed<Gtk::Label>(join(sub, "  \u00b7  "));
        subtitle->set_xalign(0.0f);
        subtitle->add_css_class("dim-label");
        text->append(*subtitle);
        box->append(*text);

        auto addr = Gtk::make_managed<Gtk::Label>(r.address.empty() ? "no address" : r.address);
        addr->add_css_class("dim-label");
        box->append(*addr);
        auto pres = Gtk::make_managed<Gtk::Label>(r.presence == core::Presence::Online ? "online" : "stale");
        pres->set_size_request(56, -1);
        pres->set_xalign(1.0f);
        if (r.presence == core::Presence::Stale) pres->add_css_class("dim-label");
        box->append(*pres);

        if (r.presence == core::Presence::Stale || r.hidden) box->set_opacity(0.6);
        row->set_child(*box);
        m_device_list.append(*row);
        if (r.id == m_sel) chosen = row;
    }
    if (chosen) m_device_list.select_row(*chosen);
    else        m_device_list.unselect_all();

    // The detail: everything the network said about the selected device.
    const auto seen = m_sel.empty() ? std::nullopt : m_net.get(m_sel);
    const auto c    = m_choices.of(m_sel);
    m_device_name.set_sensitive(seen.has_value());
    m_fav_button.set_sensitive(seen.has_value());
    m_hide_button.set_sensitive(seen.has_value());
    m_diag_button.set_sensitive(seen.has_value() && !m_diag_thread.joinable());
    if (m_sel != m_diag_for && !m_diag_thread.joinable()) m_diag_said.set_text("");   // findings belong to one device
    m_fav_button.set_label(c.favourite ? "Unfavourite" : "Favourite");
    m_hide_button.set_label(c.hidden ? "Show" : "Hide");
    if (seen) {
        const auto& s = seen->sighting;
        std::vector<std::string> svcs;
        for (const auto& v : s.services) {
            const auto l = core::service_detail(v.kind);
            if (!l.empty()) svcs.push_back(l + " " + std::to_string(v.port));
        }
        m_device_facts.set_markup(
            "<b>" + esc(s.name) + "</b>  <tt>" + esc(s.id) + "</tt>  \u00b7  " +
            (seen->presence == core::Presence::Online ? "online" : "stale") + "\n" +
            "Model: "     + esc(s.model.empty() ? "unknown" : s.model) + "\n" +
            "Addresses: " + esc(s.addresses.empty() ? "none reported" : join(s.addresses, ", ")) + "\n" +
            "Services: "  + esc(svcs.empty() ? "none" : join(svcs, ", ")));
    } else {
        // Nothing selected: say what the feed last said, so an empty list explains itself.
        const std::string looking = m_feed_note.empty() ? "Looking for devices\u2026" : m_feed_note;
        m_device_facts.set_text(m_net.count() == 0 ? looking : "Nothing selected.  " + m_feed_note);
    }
    // Never overwrite a field the user is typing in.
    if (!m_device_name.has_focus() || !seen) m_device_name.set_text(c.nickname);
    m_device_name.set_placeholder_text(seen ? seen->sighting.name : std::string{});
    m_show_hidden_check.set_active(m_show_hidden);
    repaint_shares();
    m_painting = false;

    if (m_keys_to_list) {
        m_keys_to_list = false;
        if (chosen) chosen->grab_focus();
        else        m_device_list.grab_focus();
    }
}

// Select a device (and, with `keys`, give its row the keyboard) once the
// repaint the last write queued has run.
void Shell::select_device(const std::string& id, bool keys) {  // helper
    m_sel = id;
    m_keys_to_list = m_keys_to_list || keys;
    queue_devices_repaint();
}

// Undo / Redo say what they would do, and grey when there is nothing: the
// actions (so buttons and menu items follow), the button tooltips, and the
// menu's labels -- "Undo Delete “Jade”" names the step before you take it.
void Shell::update_undo_ui() {  // helper
    if (m_act_undo) m_act_undo->set_enabled(m_journal.can_undo());
    if (m_act_redo) m_act_redo->set_enabled(m_journal.can_redo());
    const std::string u = m_journal.undo_label(), r = m_journal.redo_label();
    m_undo_button.set_tooltip_text(u.empty() ? "Nothing to undo" : "Undo " + u + "  (Ctrl+Z)");
    m_redo_button.set_tooltip_text(r.empty() ? "Nothing to redo" : "Redo " + r + "  (Ctrl+Shift+Z)");
    if (m_edit_menu) {
        m_edit_menu->remove_all();
        m_edit_menu->append(u.empty() ? "Undo" : "Undo " + u, "win.undo");
        m_edit_menu->append(r.empty() ? "Redo" : "Redo " + r, "win.redo");
    }
}

// Is the keyboard in something that types? Then Ctrl+Z is that box's own,
// whether or not it had anything to undo.
bool Shell::focus_is_text() const {  // helper
    for (const Gtk::Widget* w = get_focus(); w; w = w->get_parent())
        if (dynamic_cast<const Gtk::TextView*>(w) || dynamic_cast<const Gtk::Editable*>(w)) return true;
    return false;
}

// ── Milestone 6 -- shares ──────────────────────────────────────────────────

Glib::RefPtr<Gio::MountOperation> Shell::mount_op() {  // helper
    // GTK's dialog: user / password / domain, Anonymous when the server allows
    // it, and Forget / Remember until logout / Remember forever.
    // gtkmm 4 doesn't wrap GtkMountOperation, so make it in C and hand it to
    // giomm as the GMountOperation it is (we own the one reference).
    GMountOperation* op = gtk_mount_operation_new(GTK_WINDOW(gobj()));
    return Glib::wrap(op, false);
}

std::string Shell::shares_key(const core::Sighting& s) const {  // helper
    if (device_has_afp(s) && netafp::available()) return "afp://" + s.id + "/";
    return "shares:" + s.id;   // one press gathers SMB, SFTP and NFS
}

void Shell::repaint_shares() {  // helper: the selected device's shares + their state
    while (auto* c = m_share_list.get_first_child()) m_share_list.remove(*c);
    const auto seen = m_sel.empty() ? std::nullopt : m_net.get(m_sel);
    bool has_shares = false;
    std::string root;
    if (seen) {
        has_shares = device_has_afp(seen->sighting) || !netmount::schemes_of(seen->sighting).empty();
        root = shares_key(seen->sighting);
    }
    m_shares_button.set_sensitive(has_shares && !m_share_busy.count(root));
    // Milestone 15: what the buttons would open, said in their tooltips.
    const auto url = seen ? core::launch::web_url(seen->sighting) : std::string{};
    const auto ssh = seen ? core::launch::ssh_args(seen->sighting) : std::vector<std::string>{};
    m_web_button.set_sensitive(!url.empty());
    m_web_button.set_tooltip_text(url.empty() ? "This device advertises no web page" : "Open " + url + " in your browser");
    m_ssh_button.set_sensitive(!ssh.empty());
    m_ssh_button.set_tooltip_text(ssh.empty() ? "This device advertises no ssh"
                                              : "ssh " + join(ssh, " ") + " in a terminal");
    const auto note = m_share_notes.find(m_sel);
    m_share_said.set_text(note == m_share_notes.end() ? std::string{} : note->second);

    // Pinned shares first (milestone 7: listed without browsing), then what
    // the last Shares press found that isn't pinned.
    std::vector<netmount::Share> list;
    if (seen)
        for (const auto& p : m_choices.of(m_sel).pins) list.push_back({p.name, p.uri});
    if (const auto it = m_shares.find(m_sel); seen && it != m_shares.end())
        for (const auto& sh : it->second)
            if (!core::devices::is_pinned(m_choices, m_sel, sh.uri)) list.push_back(sh);
    if (list.empty() || !m_mounter) {
        m_share_list.set_visible(false);
        return;
    }
    const auto roots = m_mounter->mount_roots();
    const auto afp_mounts = netafp::mounts();   // milestone 10: FUSE mounts, from the kernel's table
    for (const auto& sh : list) {
        bool mounted = false;
        if (netafp::is_afp_uri(sh.uri)) {
            const auto point = afp_point(sh.uri);
            for (const auto& m : afp_mounts) if (m.point == point) mounted = true;
        } else {
            mounted = netmount::is_mounted(sh.uri, roots);
        }
        const bool busy    = m_share_busy.count(sh.uri) > 0;
        auto row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
        row->set_margin(6);
        auto name = Gtk::make_managed<Gtk::Label>();
        const bool is_pin = core::devices::is_pinned(m_choices, m_sel, sh.uri);
        name->set_markup(std::string(is_pin ? "\U0001F4CC " : "") + "<b>" + Glib::Markup::escape_text(sh.name) + "</b>  <tt>" +
                         Glib::Markup::escape_text(sh.uri) + "</tt>");
        name->set_xalign(0.0f);
        name->set_hexpand(true);
        name->set_ellipsize(Pango::EllipsizeMode::MIDDLE);
        row->append(*name);
        auto state = Gtk::make_managed<Gtk::Label>(busy ? "…" : mounted ? "mounted" : "");
        state->add_css_class("dim-label");
        row->append(*state);
        const bool pinned = core::devices::is_pinned(m_choices, m_sel, sh.uri);
        auto pin = Gtk::make_managed<Gtk::Button>(pinned ? "Unpin" : "Pin");
        pin->set_tooltip_text(pinned ? "Stop listing it here without a search" : "Keep it listed here, and in the Quick Settings tile");
        const netmount::Share share = sh;
        pin->signal_clicked().connect([this, share, pinned] { on_share_pin(share, !pinned); });
        row->append(*pin);
        auto act = Gtk::make_managed<Gtk::Button>(mounted ? "Unmount" : "Mount");
        act->set_sensitive(!busy);
        const std::string uri = sh.uri;
        act->signal_clicked().connect([this, uri, mounted] {
            if (mounted) on_share_unmount(uri); else on_share_mount(uri);
        });
        row->append(*act);
        auto open = Gtk::make_managed<Gtk::Button>("Open");
        open->set_sensitive(mounted && !busy);
        open->set_tooltip_text("Open in Files");
        open->signal_clicked().connect([this, uri] { on_share_open(uri); });
        row->append(*open);
        m_share_list.append(*row);
    }
    m_share_list.set_visible(true);
}

}  // namespace netscope
