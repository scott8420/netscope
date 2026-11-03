// netmount::Mounter -- GIO mounts. See the header.
#include "netmount/Mounter.hpp"

#include <giomm/error.h>
#include <giomm/file.h>
#include <giomm/fileenumerator.h>
#include <giomm/mount.h>
#include <glibmm/main.h>

namespace netscope::netmount {

namespace {
Outcome from_error(const Glib::Error& e) {
    Outcome o;
    if (const auto* g = dynamic_cast<const Gio::Error*>(&e)) {
        if (g->code() == Gio::Error::FAILED_HANDLED || g->code() == Gio::Error::CANCELLED) {
            o.cancelled = true;
            o.says = "Cancelled.";
            return o;
        }
    }
    o.says = e.what();
    return o;
}
bool not_mounted(const Glib::Error& e) {
    const auto* g = dynamic_cast<const Gio::Error*>(&e);
    return g && g->code() == Gio::Error::NOT_MOUNTED;
}
}  // namespace

Mounter::Mounter()
    : m_monitor(Gio::VolumeMonitor::get()),
      m_cancel(Gio::Cancellable::create()),
      m_alive(std::make_shared<bool>(true)) {
    std::weak_ptr<bool> alive = m_alive;
    auto changed = [this, alive](const Glib::RefPtr<Gio::Mount>&) {
        if (alive.lock() && m_changed) m_changed();
    };
    m_monitor->signal_mount_added().connect(changed);
    m_monitor->signal_mount_removed().connect(changed);
    m_monitor->signal_mount_changed().connect(changed);
}

Mounter::~Mounter() {
    m_alive.reset();
    m_cancel->cancel();
}

std::vector<std::string> Mounter::mount_roots() const {
    std::vector<std::string> out;
    for (const auto& m : m_monitor->get_mounts())
        if (auto root = m->get_root()) out.push_back(root->get_uri());
    return out;
}

void Mounter::list_shares(const std::string& root, const Glib::RefPtr<Gio::MountOperation>& op, Listed done) {
    enumerate(root, op, std::move(done), true);
}

void Mounter::enumerate(const std::string& root, const Glib::RefPtr<Gio::MountOperation>& op, Listed done,
                        bool may_mount) {
    std::weak_ptr<bool> alive = m_alive;
    auto file = Gio::File::create_for_uri(root);
    file->enumerate_children_async(
        [this, alive, file, root, op, done, may_mount](Glib::RefPtr<Gio::AsyncResult>& res) {
            if (!alive.lock()) return;
            Glib::RefPtr<Gio::FileEnumerator> en;
            try {
                en = file->enumerate_children_finish(res);
            } catch (const Glib::Error& e) {
                if (may_mount && not_mounted(e)) {
                    // The server's share list is itself a mount (gvfs "smb-browse").
                    file->mount_enclosing_volume(op,
                        [this, alive, file, root, op, done](Glib::RefPtr<Gio::AsyncResult>& r2) {
                            if (!alive.lock()) return;
                            try {
                                file->mount_enclosing_volume_finish(r2);
                            } catch (const Glib::Error& e2) {
                                done({}, from_error(e2));
                                return;
                            }
                            enumerate(root, op, done, false);
                        },
                        m_cancel);
                    return;
                }
                done({}, from_error(e));
                return;
            }
            // Read the whole listing in batches, then report once.
            auto shares = std::make_shared<std::vector<Share>>();
            auto next = std::make_shared<std::function<void()>>();
            *next = [this, alive, en, root, done, shares, next] {
                en->next_files_async(
                    [this, alive, en, root, done, shares, next](Glib::RefPtr<Gio::AsyncResult>& r3) {
                        if (!alive.lock()) { *next = nullptr; return; }
                        std::vector<Glib::RefPtr<Gio::FileInfo>> batch;
                        try {
                            batch = en->next_files_finish(r3);
                        } catch (const Glib::Error& e3) {
                            *next = nullptr;
                            done({}, from_error(e3));
                            return;
                        }
                        if (batch.empty()) {
                            *next = nullptr;   // break the self-reference
                            done(*shares, Outcome{true, false, {}});
                            return;
                        }
                        for (const auto& info : batch) {
                            const std::string name = info->get_name();
                            if (name.empty() || is_admin_share(name)) continue;
                            // gvfs gives each share as a child of the browse root; its
                            // target URI is the share's own location when it says one.
                            std::string uri = info->get_attribute_string("standard::target-uri");
                            if (uri.empty()) uri = en->get_child(info)->get_uri();
                            if (!uri.empty() && uri.back() != '/') uri += '/';
                            shares->push_back({info->get_display_name().empty() ? name : std::string(info->get_display_name()), uri});
                        }
                        (*next)();
                    },
                    m_cancel, 64);
            };
            (*next)();
        },
        m_cancel, "standard::name,standard::display-name,standard::target-uri,standard::type");
}

void Mounter::mount(const std::string& uri, const Glib::RefPtr<Gio::MountOperation>& op, Done done) {
    std::weak_ptr<bool> alive = m_alive;
    auto file = Gio::File::create_for_uri(uri);
    file->mount_enclosing_volume(op,
        [alive, file, done](Glib::RefPtr<Gio::AsyncResult>& res) {
            if (!alive.lock()) return;
            try {
                file->mount_enclosing_volume_finish(res);
            } catch (const Glib::Error& e) {
                const auto* g = dynamic_cast<const Gio::Error*>(&e);
                if (g && g->code() == Gio::Error::ALREADY_MOUNTED) { done({true, false, {}}); return; }
                done(from_error(e));
                return;
            }
            done({true, false, {}});
        },
        m_cancel);
}

void Mounter::unmount(const std::string& uri, const Glib::RefPtr<Gio::MountOperation>& op, Done done) {
    Glib::RefPtr<Gio::Mount> mine;
    const auto want = normal(uri);
    for (const auto& m : m_monitor->get_mounts())
        if (auto root = m->get_root(); root && normal(root->get_uri()) == want) { mine = m; break; }
    std::weak_ptr<bool> alive = m_alive;
    if (!mine) {
        // Answer on the next turn of the loop, like every other outcome here.
        Glib::signal_idle().connect_once([alive, done] {
            if (alive.lock()) done({false, false, "It isn't mounted."});
        });
        return;
    }
    mine->unmount(op,
        [alive, mine, done](Glib::RefPtr<Gio::AsyncResult>& res) {
            if (!alive.lock()) return;
            try {
                mine->unmount_finish(res);
            } catch (const Glib::Error& e) {
                done(from_error(e));
                return;
            }
            done({true, false, {}});
        },
        m_cancel);
}

}  // namespace netscope::netmount
