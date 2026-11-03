#pragma once
// netfix -- the Fix button (milestone 9). No GTK.
//
// tc01 lives on 169.254 (a self-assigned address) and this machine has none,
// so there is no route to it. Adding an address is a root-level change and
// NetScope runs as the user, so NetScope does not touch the interface. It asks
// NetworkManager, which is already privileged and lets the logged-in user edit
// their own connections over D-Bus (polkit; usually no password, else GNOME's
// prompt): set `ipv4.link-local = enabled` on the active connection. NM then
// runs link-local BESIDE DHCP -- the machine keeps its router address and
// gains a 169.254 one, on every connect, not just until the next reconnect.
//
// The same as, by hand:
//   nmcli connection modify "<conn>" ipv4.link-local enabled
//   nmcli device reapply <iface>
// Undo, by hand or `netfix_probe off`:
//   nmcli connection modify "<conn>" ipv4.link-local default
//
// The settings arithmetic is pure and tested on GVariants with no bus; the
// rest is blocking D-Bus on the system bus: callers run it off the main thread.
#include <chrono>
#include <string>

typedef struct _GVariant GVariant;

namespace netscope::netfix {

// NMSettingIP4LinkLocal (NM >= 1.40).
inline constexpr int LL_DEFAULT = 0, LL_AUTO = 1, LL_DISABLED = 2, LL_ENABLED = 3;

// Pure.
bool version_at_least(const std::string& version, int major, int minor);   // "1.46.0" >= 1.40
// A connection's settings (a{sa{sv}}) with ipv4.link-local = value; every
// other setting and key kept as it was. New reference (floating sunk), caller
// unrefs. No ipv4 setting -> one is added holding just link-local.
GVariant* with_link_local(GVariant* settings, int value);
int  link_local_of(GVariant* settings);      // the ipv4.link-local value; LL_DEFAULT when unset
std::string method_of(GVariant* settings);   // ipv4.method ("auto", "manual", ...; "" when unset)
std::string id_of(GVariant* settings);       // connection.id ("Home WiFi")

// What NetworkManager says about the connection carrying `iface` ("" = the
// primary connection -- the one with the default route).
struct Target {
    std::string nm_version;
    std::string iface;           // "wld0"
    std::string device_path;     // /org/freedesktop/NetworkManager/Devices/N
    std::string conn_path;       // /org/freedesktop/NetworkManager/Settings/N
    std::string conn_id;         // "Home WiFi"
    std::string method;          // ipv4.method
    int         link_local = -1; // ipv4.link-local
};
struct Result {
    bool        ok = false;
    std::string says;            // one plain sentence, either way
    Target      target;
    std::string address;         // the 169.254 address, once it appeared
};

// Blocking. Reads only.
Result find(const std::string& iface = "");
// Blocking: on -> link-local ENABLED, off -> DEFAULT; saved to disk, then
// reapplied to the device (or the connection re-activated when NM won't
// reapply). `interactive` lets polkit show GNOME's prompt if it wants one.
// When turning on, waits up to `wait` for the 169.254 address to appear.
Result set_link_local(const std::string& iface, bool on, bool interactive = true,
                      std::chrono::seconds wait = std::chrono::seconds(20));

// The 169.254 address on `iface` right now ("" if none). getifaddrs.
std::string link_local_address(const std::string& iface);

}  // namespace netscope::netfix
