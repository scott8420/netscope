#include "AboutWindow.hpp"
#include "widgets/Widgets.hpp"
#include "Registry.hpp"

#include <gtkmm/box.h>
#include <gtkmm/headerbar.h>
#include <gtkmm/enums.h>

namespace netscope {

AboutWindow::AboutWindow(Gtk::Window& parent) {
    set_name("shell.about");
    registry::add("shell.about", this);

    set_title("About NetScope");
    set_modal(true);
    set_resizable(false);
    set_default_size(380, 340);
    set_transient_for(parent);
    set_hide_on_close(true);   // singleton: X hides, never destructs mid-session

    // Raw Gtk::HeaderBar, matching Shell's headerbar (NetScope doesn't wrap it --
    // widen on demand, and nothing addresses it).
    auto* hb = Gtk::make_managed<Gtk::HeaderBar>();
    hb->set_show_title_buttons(true);
    set_titlebar(*hb);

    auto* page = Gtk::make_managed<widgets::Box>(
        widgets::unregistered, "shell.about.page", Gtk::Orientation::VERTICAL);
    page->set_spacing(10);
    page->set_margin(24);
    page->set_halign(Gtk::Align::CENTER);

    // Hero logo: symbolic icon from the gresource bundle, recoloured to the
    // theme foreground by GTK. Proof the resource pipeline resolved.
    auto* logo = Gtk::make_managed<widgets::Image>(widgets::unregistered, "shell.about.logo");
    logo->set_from_icon_name("netscope-logo-symbolic");
    logo->set_pixel_size(88);
    page->append(*logo);

    auto* name = Gtk::make_managed<widgets::Label>(widgets::unregistered, "shell.about.name");
    name->set_markup("<span size='xx-large' weight='bold'>NetScope</span>");
    page->append(*name);

    auto* tagline = Gtk::make_managed<widgets::Label>(
        widgets::unregistered, "shell.about.tagline", "Find, diagnose and connect to what is on your network.");
    tagline->add_css_class("dim-label");
    page->append(*tagline);

    auto* close_btn = Gtk::make_managed<widgets::Button>(
        widgets::unregistered, "shell.about.close", "Close");
    close_btn->add_css_class("suggested-action");
    close_btn->set_halign(Gtk::Align::CENTER);
    close_btn->set_margin_top(8);
    close_btn->signal_clicked().connect([this]() { close(); });
    page->append(*close_btn);

    set_child(*page);
}

AboutWindow::~AboutWindow() {
    registry::remove(this);
}

}  // namespace netscope
