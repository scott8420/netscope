#include "Shell.hpp"
#include "Log.hpp"

#include <gtkmm/headerbar.h>
#include <gtkmm/label.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/gestureclick.h>
#include <gtkmm/gesturedrag.h>
#include <gtkmm/eventcontrollerfocus.h>

// Shell_zones.cpp -- ZONES. Construction of the named regions: the window frame
// and header, the sidebar+stack body, the pattern pages, and the menu model.

namespace netscope {

void Shell::build_shell() {  // zone: window + sidebar + stack
    set_title("NetScope");
    set_default_size(760, 520);

    auto header = Gtk::make_managed<Gtk::HeaderBar>();

    // Brand logo at the start: a frameless button whose icon is a symbolic
    // resource resolved from the compiled-in bundle (App registers it), so this
    // one button exercises the resource pipeline. Click opens the About window
    // (the dialog-lifetime exemplar).
    m_logo_button.set_icon_name("netscope-logo-symbolic");
    m_logo_button.set_has_frame(false);
    m_logo_button.set_tooltip_text("About NetScope");
    m_logo_button.set_action_name("win.about");
    header->pack_start(m_logo_button);

    m_menu_button.set_icon_name("open-menu-symbolic");
    m_menu_button.set_menu_model(build_menu());
    header->pack_end(m_menu_button);
    set_titlebar(*header);

    // StackSidebar drives the Stack: adding a page with a title makes a sidebar
    // row automatically. That IS the Stack/StackSidebar paradigm -- no manual
    // row wiring.
    m_sidebar.set_stack(m_stack);
    m_sidebar.set_size_request(160, -1);
    m_stack.set_hexpand(true);
    m_stack.set_vexpand(true);

    m_body.append(m_sidebar);
    m_body.append(m_stack);
    set_child(m_body);
}

void Shell::build_pages() {  // zone: the pattern pages
    build_devices_page();   // s002 -- first: it is the app now
    // Page 1 -- Recents: a live list + explanation. The menu's Open-recent
    // submenu and this list are the same data, two surfaces (CANON: two
    // surfaces, one mechanism).
    auto intro = Gtk::make_managed<Gtk::Label>();
    intro->set_wrap(true);
    intro->set_xalign(0.0f);
    intro->set_margin(12);
    intro->set_markup(
        "<b>Recent menu</b> -- actions + a JSON pump, composed.\n\n"
        "Use the menu (top-right) -> \"Remember a folder...\" to add one. It "
        "appears here and under \"Open recent\", persists to a JSON file in the "
        "XDG data dir, and prunes entries whose folder has since vanished.\n\n"
        "The list is core data; the UI resolves the path and drives the menu. "
        "See docs/two-layer-seam.md and core/Recents.hpp.");
    m_recents_page.append(*intro);

    m_recents_list.set_selection_mode(Gtk::SelectionMode::NONE);
    m_recents_scroll.set_child(m_recents_list);
    m_recents_scroll.set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    m_recents_scroll.set_vexpand(true);
    m_recents_page.append(m_recents_scroll);
    m_stack.add(m_recents_page, "recents", "Recent menu");

    // Page 2 -- Registry: the debuggability payoff. The button dumps the live
    // name->widget tree to the log (also callable from gdb as registry::dump()).
    auto reg = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
    reg->set_margin(12);
    auto reg_intro = Gtk::make_managed<Gtk::Label>();
    reg_intro->set_wrap(true);
    reg_intro->set_xalign(0.0f);
    reg_intro->set_markup(
        "<b>Registry</b> -- the live address book Named&lt;W&gt; fills.\n\n"
        "Every widget here was named at construction and registered by "
        "Named&lt;W&gt;. \"Dump\" prints the live tree -- the same call is your "
        "friend in gdb when a backtrace frame is otherwise a bare pointer. See "
        "docs/widget-naming.md for why this is the LIGHT half of naming.");
    auto dump_btn = Gtk::make_managed<Gtk::Button>("Dump registry to the log");
    dump_btn->set_halign(Gtk::Align::START);
    dump_btn->set_action_name("win.dump-registry");
    reg->append(*reg_intro);
    reg->append(*dump_btn);
    m_stack.add(*reg, "registry", "Registry");
}

// ─────────────────────────────────────────────────────────────────────────────
// The Devices page (s002). The list is the join of what the network said and
// what Scott chose (core::rows). The nickname, Favourite and Hide are choices,
// and every one is an undo step (core/Undo.hpp, docs/undo.md); "Show hidden"
// is a view and is not. Today the devices come from FixtureSource through the
// real feed seam; s003 swaps in Avahi.
// ─────────────────────────────────────────────────────────────────────────────
void Shell::build_devices_page() {  // zone: the Devices page
    m_devices_page.set_margin(12);

    // Verbs.
    auto verbs = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
    m_fav_button.set_label("Favourite");
    m_fav_button.set_action_name("win.device-favourite");
    m_hide_button.set_label("Hide");
    m_hide_button.set_action_name("win.device-hide");
    verbs->append(m_fav_button);
    verbs->append(m_hide_button);
    m_diag_button.set_label("Diagnose");
    m_diag_button.set_tooltip_text("Address, route and port checks for the selected device");
    m_diag_button.set_action_name("win.device-diagnose");
    verbs->append(m_diag_button);
    m_show_hidden_check.set_label("Show hidden");
    m_show_hidden_check.signal_toggled().connect(sigc::mem_fun(*this, &Shell::on_show_hidden));
    verbs->append(m_show_hidden_check);
    auto spacer = Gtk::make_managed<Gtk::Box>();
    spacer->set_hexpand(true);
    verbs->append(*spacer);
    m_undo_button.set_label("Undo");
    m_undo_button.set_action_name("win.undo");
    m_redo_button.set_label("Redo");
    m_redo_button.set_action_name("win.redo");
    verbs->append(m_undo_button);
    verbs->append(m_redo_button);
    m_devices_page.append(*verbs);

    // The list.
    auto scroll = Gtk::make_managed<Gtk::ScrolledWindow>();
    scroll->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    scroll->set_vexpand(true);
    scroll->set_min_content_height(140);   // the share rows below must not squeeze it away
    scroll->set_child(m_device_list);
    m_device_list.set_selection_mode(Gtk::SelectionMode::SINGLE);
    m_device_list.signal_row_selected().connect([this](Gtk::ListBoxRow* row) {
        if (m_painting || !row) return;
        m_sel = row->get_name();
        queue_devices_repaint();   // the detail follows the selection
    });
    m_devices_page.append(*scroll);

    // The detail: the facts, then the nickname.
    m_device_facts.set_xalign(0.0f);
    m_device_facts.set_selectable(true);
    m_device_facts.set_text("Looking for devices\u2026");
    m_devices_page.append(m_device_facts);

    auto detail = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    auto nl = Gtk::make_managed<Gtk::Label>("Nickname");
    nl->set_valign(Gtk::Align::CENTER);
    m_device_name.set_hexpand(true);
    m_device_name.set_valign(Gtk::Align::CENTER);
    m_device_name.signal_activate().connect(sigc::mem_fun(*this, &Shell::on_device_rename));
    auto leave = Gtk::EventControllerFocus::create();
    leave->signal_leave().connect(sigc::mem_fun(*this, &Shell::on_device_rename));
    m_device_name.add_controller(leave);
    detail->append(*nl);
    detail->append(m_device_name);
    m_devices_page.append(*detail);

    // Milestone 6 -- shares: a button, the rows, a line for what happened.
    auto shares = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    m_shares_button.set_label("Shares");
    m_shares_button.set_tooltip_text("List this device's SMB shares; mount them so they show in Files");
    m_shares_button.set_action_name("win.device-shares");
    shares->append(m_shares_button);
    m_share_said.set_xalign(0.0f);
    m_share_said.set_hexpand(true);
    m_share_said.set_wrap(true);
    m_share_said.set_selectable(true);
    shares->append(m_share_said);
    m_devices_page.append(*shares);
    m_share_list.set_selection_mode(Gtk::SelectionMode::NONE);
    m_share_list.add_css_class("boxed-list");
    m_share_list.set_visible(false);
    m_devices_page.append(m_share_list);

    m_diag_said.set_xalign(0.0f);
    m_diag_said.set_wrap(true);
    m_diag_said.set_selectable(true);
    m_devices_page.append(m_diag_said);

    m_undo_said.set_xalign(0.0f);
    m_undo_said.add_css_class("dim-label");
    m_devices_page.append(m_undo_said);

    m_stack.add(m_devices_page, "devices", "Devices");
}

Glib::RefPtr<Gio::Menu> Shell::build_menu() {  // zone: hamburger model
    auto menu = Gio::Menu::create();

    auto file = Gio::Menu::create();
    file->append("Remember a folder\u2026", "win.pick-folder");
    m_recents_menu = Gio::Menu::create();
    file->append_submenu("Open recent", m_recents_menu);
    menu->append_section(file);

    // s046: Undo / Redo, re-labelled live ("Undo Delete “Jade”") and greyed
    // when there is nothing to do -- update_undo_ui() owns this section.
    m_edit_menu = Gio::Menu::create();
    m_edit_menu->append("Undo", "win.undo");
    m_edit_menu->append("Redo", "win.redo");
    menu->append_section(m_edit_menu);

    auto diag = Gio::Menu::create();
    diag->append("Dump registry", "win.dump-registry");
    menu->append_section(diag);

    auto app = Gio::Menu::create();
    app->append("About NetScope", "win.about");
    app->append("Quit", "win.quit");
    menu->append_section(app);
    return menu;
}

}  // namespace netscope
