// netmount -- URIs for shares. Pure.
#include "netmount/Places.hpp"

#include <algorithm>
#include <cctype>

namespace netscope::netmount {

namespace {
std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}
bool has_service(const core::Sighting& s, const std::string& kind) {
    return std::any_of(s.services.begin(), s.services.end(), [&](const auto& v) { return v.kind == kind; });
}
}  // namespace

std::string scheme_for(const std::string& kind) {
    if (kind == "smb")  return "smb";
    if (kind == "sftp") return "sftp";
    if (kind == "nfs")  return "nfs";
    return "";
}

std::vector<std::string> schemes_of(const core::Sighting& s) {
    std::vector<std::string> out;
    for (const char* k : {"smb", "sftp", "nfs"})
        if (has_service(s, k)) out.push_back(scheme_for(k));
    return out;
}

std::string host_for(const core::Sighting& s) {
    if (!s.id.empty()) return lower(s.id);
    for (const auto& a : s.addresses) if (a.find(':') == std::string::npos) return a;
    for (const auto& a : s.addresses) return "[" + a + "]";
    return "";
}

std::string root_uri(const std::string& scheme, const std::string& host) {
    return scheme + "://" + host + "/";
}

std::string share_uri(const std::string& scheme, const std::string& host, const std::string& share) {
    static const char* hex = "0123456789ABCDEF";
    std::string esc;
    for (unsigned char c : share) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || c == '$') esc += static_cast<char>(c);
        else { esc += '%'; esc += hex[c >> 4]; esc += hex[c & 15]; }
    }
    return root_uri(scheme, host) + esc + "/";
}

bool is_admin_share(const std::string& name) {
    return !name.empty() && name.back() == '$';
}

std::string normal(const std::string& uri) {
    const auto sep = uri.find("://");
    if (sep == std::string::npos) return uri;
    const auto path = uri.find('/', sep + 3);
    std::string head = lower(uri.substr(0, path));
    std::string rest = path == std::string::npos ? "/" : uri.substr(path);
    while (rest.size() > 1 && rest.back() == '/') rest.pop_back();
    if (rest.back() != '/') rest += '/';
    return head + rest;
}

bool is_mounted(const std::string& uri, const std::vector<std::string>& roots) {
    const auto want = normal(uri);
    return std::any_of(roots.begin(), roots.end(), [&](const auto& r) { return normal(r) == want; });
}

}  // namespace netscope::netmount
