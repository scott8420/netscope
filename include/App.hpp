#pragma once
#include <gtkmm/application.h>
#include <memory>

// App -- the Gtk::Application. Thin: stands up logging in create(), builds the
// Shell on activate. (CANON: thin bootstrap -- main -> App::create() -> Shell.)
namespace netscope {

class Shell;

class App : public Gtk::Application {
public:
    static Glib::RefPtr<App> create();

protected:
    App();
    void on_activate() override;
    void on_startup() override;   // accelerators, driven from core::shortcut_registry()
    // Milestone 8: app.mount(s) and app.devices -- what the Quick Settings tile
    // calls through org.freedesktop.Application.ActivateAction when netscoped
    // needs a password the Keyring doesn't have.
    void on_mount_action(const Glib::VariantBase& uri);

private:
    Shell* m_shell = nullptr;   // owned by the application via add_window
};

}  // namespace netscope
