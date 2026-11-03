#pragma once
#include "widgets/Widgets.hpp"
#include "core/Devices.hpp"  // s002: the device model + its journal
#include "core/Feed.hpp"     // s002: the seam discovery comes through

#include "netdiag/Checks.hpp"   // milestone 5: the checks

#include <glibmm/dispatcher.h>
#include <mutex>
#include <thread>

#include <gtkmm/applicationwindow.h>
#include <giomm/menu.h>
#include <giomm/simpleaction.h>

#include <memory>
#include <string>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// Shell -- the app window, AND the split-class exemplar.
//
// This class is split across multiple TUs by CATEGORY OF CODE, not by feature
// (CANON: "vocabulary first, file layout second" + "the header is the index").
// The split costs ~nothing while the parts are small; retrofitting it after a
// class grows to thousands of lines is a multi-session migration, so the
// structure is installed on day one. The header below IS the index -- every
// method carries a trailing `// category: ...` tag naming the file its body
// lives in. Read the tag, open that file.
//
// Vocabulary
// ----------
//   Zones    -- construction of named regions: the window frame, header,
//               sidebar+stack, the pattern pages, the menu model.
//               File: src/Shell_zones.cpp
//   Bindings -- what's wired to what: Gio actions, the recents action group,
//               accelerators.  File: src/Shell_bindings.cpp
//   Handlers -- slot bodies (on_*): what a user action actually does.
//               File: src/Shell_handlers.cpp
//   Helpers  -- reusable sync/orchestration called from zones/bindings/handlers.
//               File: src/Shell_helpers.cpp
//   Glue     -- what stays in Shell.cpp: the ctor + build_ui() (the ordered
//               construct -> name/register -> bind pass).
//
// Finding things
// --------------
//   "Where is the window built?"        -> Shell_zones.cpp    (build_shell)
//   "Where are actions registered?"     -> Shell_bindings.cpp (bind_actions)
//   "What does 'Dump registry' do?"     -> Shell_handlers.cpp (on_dump_registry)
//   "Where does the recents list sync?" -> Shell_helpers.cpp  (note_recent / rebuild_*)
//   "Where do discovered devices land?" -> Shell_helpers.cpp  (drain_feed)
//   "Where does Ctrl+Z go?"             -> Shell_bindings.cpp (the window's undo keys)
//                                          + Shell_handlers.cpp (on_undo / on_redo)
// ─────────────────────────────────────────────────────────────────────────────
namespace netscope {

class AboutWindow;
class ShortcutsDialog;

class Shell : public Gtk::ApplicationWindow {
public:
    Shell();
    ~Shell() override;

    void build_ui();                             // category: glue

private:
    // ── zones ──────────────────────────────────────────────────────────────
    void build_shell();                          // category: zone: window + sidebar + stack
    void build_pages();                          // category: zone: the pattern pages
    Glib::RefPtr<Gio::Menu> build_menu();        // category: zone: hamburger model

    // ── bindings ───────────────────────────────────────────────────────────
    void bind_actions();                         // category: bindings: actions + recents group

    // ── handlers ───────────────────────────────────────────────────────────
    void on_pick_folder();                       // category: handler: folder chooser -> remember it
    void on_recent_open(const std::string& p);   // category: handler: open a recent (parameterised)
    void on_recent_clear();                      // category: handler: empty the recents list
    void on_dump_registry();                     // category: handler: print the live widget tree
    void on_about();                             // category: handler: open the About window (dialog-lifetime exemplar)
    void on_shortcuts();   // handler: open the keyboard reference (renders from core::shortcut_registry)
    // s002 -- the Devices page. Every choice on it goes through the journal
    // (core/Undo.hpp); nothing on it writes the model any other way.
    void on_undo();                              // category: handler: Ctrl+Z / menu / button
    void on_redo();                              // category: handler: Ctrl+Shift+Z, Ctrl+Y / menu / button
    void on_device_favourite();                  // category: handler: star / unstar the selected device
    void on_device_hide();                       // category: handler: hide / show the selected device
    void on_device_rename();                     // category: handler: the nickname field, on Enter / leaving it
    void on_show_hidden();                       // category: handler: the "Show hidden" switch (a view, not a choice)
    void on_device_diagnose();                   // category: handler: run the checks off the main thread
    void on_diagnose_done();                     // category: handler: main thread -- show the findings

    // ── helpers ────────────────────────────────────────────────────────────
    void note_recent(const std::string& path);   // category: helper: push onto the list + persist
    void rebuild_recents_menu();                  // category: helper: repaint the submenu
    void rebuild_recents_page();                  // category: helper: repaint the live list page
    std::string recents_file() const;             // category: helper: the XDG path (UI-side resolution)
    void build_devices_page();                    // category: zone: the Devices page (s002)
    void drain_feed();                            // category: helper: main thread -- inbox -> network
    void queue_devices_repaint();                 // category: helper: one repaint per idle, after the writes
    void repaint_devices();                       // category: helper: the list + the detail fields from the model
    void select_device(const std::string& id, bool keys);  // category: helper: select (+ keyboard) after a repaint
    void update_undo_ui();                        // category: helper: Undo / Redo say what they would do, grey when nothing
    bool focus_is_text() const;                   // category: helper: the keyboard is in a text box (its own Ctrl+Z)

    // Titlebar: brand logo (start) + hamburger (end).
    widgets::Button     m_logo_button;   // frameless brand mark; opens About
    widgets::MenuButton m_menu_button;

    // Body: sidebar | stack (each pattern is a stack page).
    widgets::Box          m_body;      // horizontal
    widgets::StackSidebar m_sidebar;
    widgets::Stack        m_stack;

    // The recents "live page": a label + a scrolling list rebuilt on change.
    widgets::Box            m_recents_page;   // vertical
    widgets::ScrolledWindow m_recents_scroll;
    widgets::ListBox        m_recents_list;

    // Recents state (core data; UI holds it + resolves the path -- the seam).
    std::vector<std::string>        m_recents;
    std::string                     m_recents_file;
    Glib::RefPtr<Gio::Menu>         m_recents_menu;   // live submenu, rebuilt in place
    Glib::RefPtr<Gio::SimpleAction> m_recents_clear;  // held to toggle enabled state

    // s002 -- the device model. Declaration order matters: the store before
    // the journal that holds a reference to it.
    core::Network       m_net;                  // what discovery says (the feed writes it)
    core::Choices       m_choices;              // what Scott chose (the journal writes it)
    core::DeviceJournal m_journal{m_choices};
    std::string         m_choices_file;         // XDG data dir / netscope / choices.json
    std::string        m_sel;                  // the selected device's id ("" = none)
    bool               m_show_hidden = false;  // a view setting, not a model choice
    std::string        m_feed_note;            // the source's last word ("Avahi isn't running")
    bool               m_painting = false;     // a repaint sets fields: not a user choice
    bool               m_repaint_queued = false;
    bool               m_keys_to_list = false; // after the repaint, give the list the keyboard
    widgets::Box         m_devices_page;
    widgets::ListBox     m_device_list;
    widgets::Label       m_device_facts;       // the selected device, in full
    widgets::Entry       m_device_name;        // nickname
    widgets::Button      m_fav_button;
    widgets::Button      m_hide_button;
    widgets::CheckButton m_show_hidden_check;
    widgets::Button      m_diag_button;
    widgets::Label       m_diag_said;          // the findings, one plain line each
    widgets::Button    m_undo_button;
    widgets::Button    m_redo_button;
    widgets::Label     m_undo_said;            // "Undid: Hide “RASPI”"
    Glib::RefPtr<Gio::Menu>         m_edit_menu;   // Undo / Redo items, re-labelled live
    Glib::RefPtr<Gio::SimpleAction> m_act_undo, m_act_redo;

    // The About window: a hide-on-close singleton, built lazily on first open.
    std::unique_ptr<AboutWindow>    m_about;
    std::unique_ptr<ShortcutsDialog> m_shortcuts;   // shortcut registry's GTK consumer (same lifetime stone)

    // The feed (s002). The source is declared LAST so it is destroyed FIRST:
    // its thread is joined before the inbox and dispatcher it pushes to go.
    // ~Shell stops it explicitly as well.
    core::Inbox                   m_inbox;
    Glib::Dispatcher              m_feed_ready;   // emitted on the source's thread
    std::unique_ptr<core::Source> m_source;

    // Diagnose (milestone 5): one run at a time, on its own thread; the result
    // comes back through a dispatcher. Diagnosing is not a choice: no step.
    // The thread is declared after the dispatcher so it is joined first.
    Glib::Dispatcher                  m_diag_ready;
    std::mutex                        m_diag_mu;
    std::vector<netdiag::Finding>     m_diag_findings;   // guarded by m_diag_mu
    std::string                       m_diag_for;        // the device they're about
    std::string                       m_diag_name;
    std::jthread                      m_diag_thread;
};

}  // namespace netscope
