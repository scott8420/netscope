#pragma once
// netafp Keyring -- AFP passwords in GNOME Keyring (milestone 12). No GTK.
//
// GVfs keeps SMB passwords in Keyring for us; AFP goes through Netatalk Client,
// so NetScope stores its own. Through libsecret, in the default collection
// (the login keyring), as a standard network password:
//
//   schema    org.gnome.keyring.NetworkPassword   (what gvfs and Seahorse use)
//   server    tc01.local
//   protocol  afp
//   user      scott        ("" = guest is never stored: nothing to keep)
//   label     "scott on tc01 (AFP, NetScope)"
//
// One item per server and user; saving again replaces it. Seahorse ("Passwords
// and Keys") lists it under Login, where it can be seen or deleted by hand.
//
// libsecret is optional at build time: without it, available() is false and
// the app keeps the password until it quits, as in milestone 10.
//
// All calls block (D-Bus to gnome-keyring; a locked keyring shows GNOME's
// unlock prompt): run them off the main thread.
#include <string>

namespace netscope::netafp::keyring {

// ── Pure ────────────────────────────────────────────────────────────────────
std::string label(const std::string& host, const std::string& user);   // "scott on tc01 (AFP, NetScope)"

// ── System ──────────────────────────────────────────────────────────────────
bool available();            // built with libsecret

struct Saved {
    bool        found = false;
    std::string user;
    std::string password;
    std::string says;        // why not, when it failed ("" when simply none saved)
};
Saved       find(const std::string& host);                       // the newest item for this server
std::string save(const std::string& host, const std::string& user,
                 const std::string& password);                    // "" = saved
std::string forget(const std::string& host);                      // every NetScope/AFP item for it; "" = ok

}  // namespace netscope::netafp::keyring
