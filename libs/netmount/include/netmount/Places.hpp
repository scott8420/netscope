#pragma once
// netmount -- where a share lives, as a GIO URI (milestone 6). Pure, no GIO.
//
// A device's shares are named by URI: "smb://raspi.local/Public/". The host is
// the advertised name, not the address: it survives a changed address, it is
// what Nautilus shows ("Public on raspi.local"), and GNOME Keyring files the
// password under it -- an address would orphan the password when it changed.
// A device with no name falls back to its first IPv4, then IPv6 in brackets.
#include "core/Devices.hpp"

#include <string>
#include <vector>

namespace netscope::netmount {

struct Share {
    std::string name;   // "Public"
    std::string uri;    // "smb://raspi.local/Public/"
    bool operator==(const Share&) const = default;
};

// The GIO scheme for a discovered service kind, or "" when GIO is not the way
// (AFP goes through Netatalk Client in milestone 7).
std::string scheme_for(const std::string& kind);

// The schemes this device can be browsed by, in a fixed order (smb first).
// ssh counts as sftp (milestone 14).
std::vector<std::string> schemes_of(const core::Sighting& s);

std::string host_for(const core::Sighting& s);
std::string root_uri(const std::string& scheme, const std::string& host);         // "smb://raspi.local/"
std::string share_uri(const std::string& scheme, const std::string& host,
                      const std::string& share);                                   // percent-escaped name

// An SFTP server is one share: the server itself ("sftp://raspi.local/").
Share sftp_share(const std::string& host);

// Shares a person never mounts: "IPC$", "print$", "ADMIN$".
bool is_admin_share(const std::string& name);

// URIs compared the way a mount is found: scheme and host case-blind, one
// trailing slash, escapes as given.
std::string normal(const std::string& uri);
bool is_mounted(const std::string& uri, const std::vector<std::string>& mount_roots);

}  // namespace netscope::netmount
