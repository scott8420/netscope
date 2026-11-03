#pragma once
// netmount::Mounter -- GIO mounts (milestone 6). No GTK: giomm only.
//
// Everything is asynchronous on the calling thread's main context (the GTK
// main loop in the app), so the UI never waits and no thread is needed: GIO
// and gvfs already do the waiting.
//
// Passwords: the caller passes a Gio::MountOperation. The app passes a
// Gtk::MountOperation, whose dialog offers "Remember forever"; gvfs then files
// the password in GNOME Keyring and finds it there next time -- the way
// Nautilus does it, so NetScope holds no password and no libsecret code.
//
// Every outcome arrives on a later turn of the loop, never inside the call.
//
// Lifetime: callbacks never fire after the Mounter is gone (an alive token,
// plus a Cancellable cancelled in the destructor).
#include "netmount/Places.hpp"

#include <giomm/cancellable.h>
#include <giomm/mountoperation.h>
#include <giomm/volumemonitor.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace netscope::netmount {

struct Outcome {
    bool        ok = false;
    bool        cancelled = false;   // the person dismissed the password dialog: say nothing loud
    std::string says;                // GIO's own sentence when it failed
};

class Mounter {
public:
    using Done   = std::function<void(const Outcome&)>;
    using Listed = std::function<void(const std::vector<Share>&, const Outcome&)>;

    Mounter();
    ~Mounter();
    Mounter(const Mounter&) = delete;
    Mounter& operator=(const Mounter&) = delete;

    // The shares at a server root ("smb://raspi.local/"). Mounts the browse
    // location first when GIO says it isn't mounted. Admin shares ($) dropped.
    void list_shares(const std::string& root, const Glib::RefPtr<Gio::MountOperation>& op, Listed done);
    void mount(const std::string& uri, const Glib::RefPtr<Gio::MountOperation>& op, Done done);
    void unmount(const std::string& uri, const Glib::RefPtr<Gio::MountOperation>& op, Done done);

    // The roots of everything mounted right now, as URIs. Cheap: the volume
    // monitor's own list, no round trip.
    std::vector<std::string> mount_roots() const;
    bool is_mounted(const std::string& uri) const { return netmount::is_mounted(uri, mount_roots()); }

    // Any mount came or went (ours, Nautilus's, an eject from the sidebar).
    void set_on_change(std::function<void()> f) { m_changed = std::move(f); }

private:
    void enumerate(const std::string& root, const Glib::RefPtr<Gio::MountOperation>& op, Listed done, bool may_mount);

    Glib::RefPtr<Gio::VolumeMonitor> m_monitor;
    Glib::RefPtr<Gio::Cancellable>   m_cancel;
    std::shared_ptr<bool>            m_alive;
    std::function<void()>            m_changed;
};

}  // namespace netscope::netmount
