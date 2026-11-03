#include "Shell.hpp"
#include "AboutWindow.hpp"      // complete type for ~unique_ptr<AboutWindow> in ~Shell()
#include "ShortcutsDialog.hpp"  // ditto -- same singleton shape, same dtor requirement
#include "Log.hpp"
#include "netdiscover/AvahiSource.hpp"

#include <cstdlib>

// Shell.cpp -- GLUE. The ctor and the ordered build_ui() pass live here; every
// other method body lives in a category file (see the index in Shell.hpp).

namespace netscope {

Shell::Shell()
    : m_logo_button("shell.logo_button"),
      m_menu_button("shell.menu_button"),
      m_body("shell.body", Gtk::Orientation::HORIZONTAL),
      m_sidebar("shell.sidebar"),
      m_stack("shell.stack"),
      m_recents_page("shell.recents_page", Gtk::Orientation::VERTICAL),
      m_recents_scroll("shell.recents_scroll"),
      m_recents_list("shell.recents_list"),
      m_devices_page("shell.devices.page", Gtk::Orientation::VERTICAL, 8),
      m_device_list("shell.devices.list"),
      m_device_facts("shell.devices.facts"),
      m_device_name("shell.devices.name"),
      m_fav_button("shell.devices.favourite"),
      m_hide_button("shell.devices.hide"),
      m_show_hidden_check("shell.devices.show_hidden"),
      m_diag_button("shell.devices.diagnose"),
      m_diag_said("shell.devices.diag_said"),
      m_shares_button("shell.devices.shares"),
      m_share_list("shell.devices.share_list"),
      m_share_said("shell.devices.share_said"),
      m_undo_button("shell.devices.undo"),
      m_redo_button("shell.devices.redo"),
      m_undo_said("shell.devices.said") {
    set_name("shell.window");
}

Shell::~Shell() {
    // Join the feed's thread before anything it pushes to is torn down.
    if (m_source) m_source->stop();
    if (m_diag_thread.joinable()) m_diag_thread.join();   // bounded: a few seconds at most
}

// The ordered pass Scott's Controller/MainWindow shape names: construct the
// shell, build the pages, bind actions + accelerators, load persisted state,
// paint. Each step is one call into a category file.
void Shell::build_ui() {
    build_shell();     // zone
    build_pages();     // zone
    bind_actions();    // bindings

    // Load persisted recents (the UI resolves the XDG path -- the seam -- and
    // hands the core a plain string), then paint both surfaces.
    m_recents_file = recents_file();
    // m_recents filled by note_recent / load; load happens in bind_actions after
    // the action group exists so a click can route. Paint whatever's there.
    rebuild_recents_menu();
    rebuild_recents_page();

    // Start the feed last, once everything it repaints exists. Avahi
    // (milestone 4); NETSCOPE_FIXTURE=1 brings back the canned devices.
    const char* fx = std::getenv("NETSCOPE_FIXTURE");
    if (fx && *fx && std::string(fx) != "0") m_source = std::make_unique<core::FixtureSource>();
    else                                       m_source = std::make_unique<netdiscover::AvahiSource>();
    m_source->start(m_inbox);

    if (auto lg = log::get(log::Area::Shell))
        lg->info("ready -- {} widget(s) registered", 0);  // dump-registry shows the real count
}

}  // namespace netscope
