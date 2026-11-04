#pragma once
// netmount::nfs -- an NFS server's export list (milestone 14). No GIO, no new
// dependency: plain sockets and ONC RPC by hand.
//
// gvfs mounts an export (nfs://raspi.local/srv/photos/) but won't list them at
// nfs://raspi.local/ -- "Mount point does not exist". So NetScope asks the
// server itself, the way `showmount -e` does: the portmapper (TCP 111) says
// where mountd listens, mountd's EXPORT call says what is exported.
//
// The encoding and parsing are pure (tested headless); list_exports() is the
// one call that touches the network. It blocks (3 s per step at most): run it
// off the main thread.
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace netscope::netmount::nfs {

struct Export {
    std::string              path;     // "/srv/nfs/photos"
    std::vector<std::string> groups;   // who may mount it ("*", "192.168.1.0/24"); empty = everyone
    bool operator==(const Export&) const = default;
};

constexpr std::uint32_t kPortmap = 100000;
constexpr std::uint32_t kMountd  = 100005;
constexpr std::uint32_t kTcp     = 6;

// One RPC record over TCP: the 4-byte last-fragment mark, then the body.
std::string record(const std::string& body);
// Bodies of the two calls (AUTH_NULL).
std::string call_getport(std::uint32_t xid, std::uint32_t prog, std::uint32_t vers, std::uint32_t prot);
std::string call_export(std::uint32_t xid, std::uint32_t vers);
// Replies, defragmented (marks removed). nullopt: not a successful answer to xid.
std::optional<std::uint32_t>       parse_getport(const std::string& reply, std::uint32_t xid);
std::optional<std::vector<Export>> parse_exports(const std::string& reply, std::uint32_t xid);
// A record stream (marks included) -> the body, once the last fragment is in.
std::optional<std::string> defragment(const std::string& stream);

struct Listing {
    bool                ok = false;
    std::vector<Export> exports;
    std::string         says;   // a sentence when !ok
};
// Ask the server at `address` (an IP, v4 or v6). `who` names it in sentences.
Listing list_exports(const std::string& address, const std::string& who, int timeout_ms = 3000);

// "nfs://raspi.local/srv/nfs/photos/" -- each path part escaped, slashes kept.
std::string export_uri(const std::string& host, const std::string& path);
// What a row calls it: the last part ("photos"); "/" stays "/".
std::string export_name(const std::string& path);

}  // namespace netscope::netmount::nfs
