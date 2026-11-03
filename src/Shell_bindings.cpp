#include "Shell.hpp"
#include "Log.hpp"
#include "core/Recents.hpp"

#include <giomm/simpleactiongroup.h>
#include <gtkmm/eventcontrollerkey.h>
#include <glibmm/variant.h>
#include <glibmm/miscutils.h>

#include <filesystem>

// Shell_bindings.cpp -- BINDINGS. What's wired to what: the win.* actions, the
// parameterised recents action group, and (where a keystroke must survive focus)
// accelerators. No slot bodies here -- those are handlers; this file only wires.

namespace netscope {

void Shell::bind_actions() {  // bindings: actions + recents group
    // Plain win.* actions -> handler bodies (Shell_handlers.cpp).
    add_action("pick-folder",   sigc::mem_fun(*this, &Shell::on_pick_folder));
    add_action("dump-registry", sigc::mem_fun(*this, &Shell::on_dump_registry));
    add_action("about",         sigc::mem_fun(*this, &Shell::on_about));
    add_action("shortcuts",     sigc::mem_fun(*this, &Shell::on_shortcuts));
    add_action("quit",          [this]() { close(); });

    // Recents: a "recents" action group with a PARAMETERISED open verb --
    // recents.open(<path>) carries each entry's path as its target, so one
    // action serves every menu row (CANON/Curvz: the proven parameterised-action
    // shape). clear is held so rebuild_recents_menu can grey it when empty.
    auto group = Gio::SimpleActionGroup::create();
    group->add_action_with_parameter(
        "open", Glib::Variant<Glib::ustring>::variant_type(),
        [this](const Glib::VariantBase& param) {
            const std::string p =
                Glib::VariantBase::cast_dynamic<Glib::Variant<Glib::ustring>>(param).get();
            on_recent_open(p);
        });
    m_recents_clear = Gio::SimpleAction::create("clear");
    m_recents_clear->signal_activate().connect(
        [this](const Glib::VariantBase&) { on_recent_clear(); });
    group->add_action(m_recents_clear);
    insert_action_group("recents", group);

    // Now that the group exists, load the persisted list (prunes stale folders).
    m_recents = core::load_recents(m_recents_file);

    // ── s002: the Devices page ──────────────────────────────────────────────
    // Undo / Redo are held so update_undo_ui can grey them; the device verbs
    // are plain actions the page's buttons name.
    m_act_undo = add_action("undo", sigc::mem_fun(*this, &Shell::on_undo));
    m_act_redo = add_action("redo", sigc::mem_fun(*this, &Shell::on_redo));
    add_action("device-favourite", sigc::mem_fun(*this, &Shell::on_device_favourite));
    add_action("device-hide",      sigc::mem_fun(*this, &Shell::on_device_hide));
    add_action("device-diagnose",  sigc::mem_fun(*this, &Shell::on_device_diagnose));
    m_diag_ready.connect(sigc::mem_fun(*this, &Shell::on_diagnose_done));
    add_action("device-fix",       sigc::mem_fun(*this, &Shell::on_device_fix));
    m_fix_ready.connect(sigc::mem_fun(*this, &Shell::on_fix_done));
    add_action("device-shares",    sigc::mem_fun(*this, &Shell::on_device_shares));
    // Milestone 6: any mount coming or going -- ours, or an eject from the
    // Nautilus sidebar -- repaints the share rows.
    m_mounter = std::make_unique<netmount::Mounter>();
    m_mounter->set_on_change([this] { repaint_shares(); });

    // The model <-> journal <-> UI wiring. Choices ask the journal whether the
    // door is open (the one-door guard); the journal reads the selection and
    // tells the UI when Undo / Redo change; either table's changes repaint.
    m_choices.set_door([this] { return m_journal.open(); });
    // Saved choices come back first (a load: not a step), then every change --
    // a verb, an undo, a redo -- is written straight back. The UI resolves
    // the XDG path; the core owns the shape (core::save_choices).
    m_choices_file = (std::filesystem::path(Glib::get_user_data_dir()) / "netscope" / "choices.json").string();
    const int restored = core::load_choices(m_choices_file, m_choices);
    if (auto lg = log::get(log::Area::Devices))
        lg->info("choices: {} restored from {}", restored, m_choices_file);
    m_choices.set_on_change([this] {
        if (!core::save_choices(m_choices_file, m_choices))
            if (auto lg = log::get(log::Area::Devices)) lg->warn("choices: save failed: {}", m_choices_file);
        queue_devices_repaint();
    });
    m_net.set_on_change([this] { queue_devices_repaint(); });
    m_journal.set_selection_source([this] {
        return m_sel.empty() ? std::vector<std::string>{} : std::vector<std::string>{m_sel};
    });
    m_journal.set_exists([this](const std::string& id) { return m_net.get(id).has_value(); });
    m_journal.set_on_change([this] { update_undo_ui(); });

    // The feed seam: the source's thread pushes and emits; the dispatcher
    // brings that to the main loop, which drains. Wired before the source starts.
    m_feed_ready.connect(sigc::mem_fun(*this, &Shell::drain_feed));
    m_inbox.set_notify([this] { m_feed_ready.emit(); });

    // ── Ctrl+Z from ANYWHERE but a text box ─────────────────────────────────
    // NOT an accelerator: a set_accels_for_action chord fires before the
    // focused widget, so it would take Ctrl+Z away from every Entry and
    // TextView (jot s016c met that with Ctrl+Delete). Instead the WINDOW
    // hears it in the BUBBLE phase -- only keys nothing else took -- and lets
    // a text box keep its own even when it had nothing to undo. Binding undo
    // to one widget's focus is the other trap: after a menu closes, or the
    // focused row is rebuilt away, the keyboard is nowhere and Ctrl+Z is dead
    // (jot s046a: "undo a delete" did nothing). The registry carries these as
    // doc-only rows for that reason.
    auto keys = Gtk::EventControllerKey::create();
    keys->signal_key_pressed().connect(
        [this](guint keyval, guint, Gdk::ModifierType state) {
            const auto mods = state & (Gdk::ModifierType::CONTROL_MASK |
                                       Gdk::ModifierType::SHIFT_MASK |
                                       Gdk::ModifierType::ALT_MASK);
            const auto CTRL  = Gdk::ModifierType::CONTROL_MASK;
            const auto SHIFT = Gdk::ModifierType::SHIFT_MASK;
            const char* act = nullptr;
            if ((keyval == GDK_KEY_z || keyval == GDK_KEY_Z) && mods == CTRL)               act = "win.undo";
            else if ((keyval == GDK_KEY_z || keyval == GDK_KEY_Z) && mods == (CTRL | SHIFT)) act = "win.redo";
            else if ((keyval == GDK_KEY_y || keyval == GDK_KEY_Y) && mods == CTRL)          act = "win.redo";
            if (!act || focus_is_text()) return false;
            if (auto lg = log::get(log::Area::Shell)) lg->info("window key -> {}", act);
            activate_action(act);   // the FULL name: "undo" alone would do nothing
            return true;
        }, false);
    add_controller(keys);
    update_undo_ui();
}

}  // namespace netscope
