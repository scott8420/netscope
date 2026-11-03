// netmount_probe -- shares and mounts with no window.
//
//   netmount_probe list    smb://raspi.local/
//   netmount_probe mount   smb://raspi.local/Public/
//   netmount_probe unmount smb://raspi.local/Public/
//   netmount_probe mounts
//
// Passwords: anonymous when the server allows it; otherwise it asks on the
// terminal (NETMOUNT_USER sets the user name; NETMOUNT_PASS skips the prompt
// for scripts; NETMOUNT_DEBUG=1 prints what the server asked for). The probe never saves a
// password -- the app's dialog is where "Remember forever" lives.
#include "netmount/Mounter.hpp"

#include <giomm/init.h>
#include <glibmm/main.h>

#include <cstdlib>
#include <iostream>
#include <unistd.h>

int main(int argc, char** argv) {
    Gio::init();
    const std::string verb = argc > 1 ? argv[1] : "";
    const std::string uri  = argc > 2 ? argv[2] : "";
    if (verb != "mounts" && (uri.empty() || (verb != "list" && verb != "mount" && verb != "unmount"))) {
        std::cerr << "usage: netmount_probe list|mount|unmount <uri>   |   netmount_probe mounts\n";
        return 2;
    }
    auto loop = Glib::MainLoop::create();
    netscope::netmount::Mounter m;
    int rc = 0;

    auto op = Gio::MountOperation::create();
    int asks = 0;
    op->signal_ask_password().connect([op, &asks](const Glib::ustring& msg, const Glib::ustring& user,
                                           const Glib::ustring& domain, Gio::AskPasswordFlags flags) {
        std::cout << "asked: " << msg.raw() << "\n";
        if (++asks > 2) {   // gvfs asks again after a wrong password; the dialog would, too
            std::cout << "  wrong user or password twice -- giving up\n";
            op->reply(Gio::MountOperationResult::ABORTED);
            return;
        }
        if (std::getenv("NETMOUNT_DEBUG"))
            std::cout << "  flags=" << static_cast<int>(flags) << " user='" << user.raw() << "' domain='" << domain.raw() << "'\n";
        if ((flags & Gio::AskPasswordFlags::ANONYMOUS_SUPPORTED) == Gio::AskPasswordFlags::ANONYMOUS_SUPPORTED &&
            !std::getenv("NETMOUNT_USER")) {
            op->set_anonymous(true);
            op->reply(Gio::MountOperationResult::HANDLED);
            return;
        }
        const char* u = std::getenv("NETMOUNT_USER");
        op->set_username(u ? u : std::string(user));
        if (!domain.empty()) op->set_domain(domain);
        if ((flags & Gio::AskPasswordFlags::NEED_PASSWORD) == Gio::AskPasswordFlags::NEED_PASSWORD) {
            const char* pw = std::getenv("NETMOUNT_PASS");   // for scripts; never saved
            if (!pw) pw = getpass("password: ");
            op->set_password(pw ? pw : "");
        }
        op->set_password_save(Gio::PasswordSave::NEVER);
        op->reply(Gio::MountOperationResult::HANDLED);
    });

    auto report = [&](const netscope::netmount::Outcome& o) {
        std::cout << (o.ok ? "ok" : (o.cancelled ? "cancelled" : "failed: " + o.says)) << "\n";
        rc = o.ok ? 0 : 1;
        loop->quit();
    };
    if (verb == "mounts") {
        for (const auto& r : m.mount_roots()) std::cout << r << "\n";
        return 0;
    }
    if (verb == "list") {
        m.list_shares(uri, op, [&](const std::vector<netscope::netmount::Share>& shares, const netscope::netmount::Outcome& o) {
            for (const auto& s : shares)
                std::cout << "share " << s.name << "  " << s.uri << (m.is_mounted(s.uri) ? "  (mounted)" : "") << "\n";
            report(o);
        });
    } else if (verb == "mount") {
        m.mount(uri, op, report);
    } else {
        m.unmount(uri, op, report);
    }
    loop->run();
    return rc;
}
