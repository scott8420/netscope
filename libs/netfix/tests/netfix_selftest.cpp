// netfix_selftest -- the link-local settings arithmetic, headless. No bus.
#include "netfix/LinkLocal.hpp"

#include <glib.h>

#include <iostream>
#include <string>

namespace {
int g_pass = 0, g_fail = 0;
void check(const std::string& name, bool ok, const std::string& detail = "") {
    (ok ? g_pass : g_fail)++;
    std::cout << (ok ? "  PASS  " : "  FAIL  ") << name;
    if (!detail.empty()) std::cout << "  (" << detail << ")";
    std::cout << "\n";
}
GVariant* parse(const char* text) {
    GError* e = nullptr;
    GVariant* v = g_variant_parse(G_VARIANT_TYPE("a{sa{sv}}"), text, nullptr, nullptr, &e);
    if (!v) { std::cerr << "parse: " << e->message << "\n"; g_error_free(e); }
    return v ? g_variant_ref_sink(v) : nullptr;
}
std::string print(GVariant* v) {
    gchar* s = g_variant_print(v, TRUE);
    std::string out = s;
    g_free(s);
    return out;
}
}  // namespace

int main() {
    namespace nf = netscope::netfix;
    std::cout << "netfix_selftest -- ipv4.link-local through NetworkManager\n";

    check("version: 1.46.0 >= 1.40", nf::version_at_least("1.46.0", 1, 40));
    check("version: 1.40.0 >= 1.40", nf::version_at_least("1.40.0", 1, 40));
    check("version: 1.38.6 < 1.40", !nf::version_at_least("1.38.6", 1, 40));
    check("version: 2.0 >= 1.40", nf::version_at_least("2.0", 1, 40));
    check("version: junk is not", !nf::version_at_least("", 1, 40));

    // A Wi-Fi profile as GetSettings returns it: no secrets, method auto.
    GVariant* wifi = parse(
        "{'connection': {'id': <'Home WiFi'>, 'uuid': <'1b2f-x'>, 'type': <'802-11-wireless'>},"
        " '802-11-wireless': {'ssid': <[byte 0x48, 0x6f, 0x6d, 0x65]>, 'mode': <'infrastructure'>},"
        " '802-11-wireless-security': {'key-mgmt': <'wpa-psk'>},"
        " 'ipv4': {'method': <'auto'>, 'dns': <@au []>, 'address-data': <@aa{sv} []>},"
        " 'ipv6': {'method': <'auto'>}}");
    check("parse: the Wi-Fi fixture", wifi != nullptr);
    check("read: id", nf::id_of(wifi) == "Home WiFi");
    check("read: method auto", nf::method_of(wifi) == "auto");
    check("read: link-local unset -> default", nf::link_local_of(wifi) == nf::LL_DEFAULT);

    GVariant* on = nf::with_link_local(wifi, nf::LL_ENABLED);
    check("set: link-local enabled", nf::link_local_of(on) == nf::LL_ENABLED, print(on));
    check("set: method kept", nf::method_of(on) == "auto");
    check("set: id kept", nf::id_of(on) == "Home WiFi");
    check("set: same groups", g_variant_n_children(on) == g_variant_n_children(wifi));
    GVariant* ssid_before = g_variant_lookup_value(wifi, "802-11-wireless", nullptr);
    GVariant* ssid_after  = g_variant_lookup_value(on, "802-11-wireless", nullptr);
    check("set: Wi-Fi setting untouched", g_variant_equal(ssid_before, ssid_after));
    g_variant_unref(ssid_before);
    g_variant_unref(ssid_after);
    GVariant* v4 = g_variant_lookup_value(on, "ipv4", G_VARIANT_TYPE("a{sv}"));
    check("set: ipv4 keeps its other keys (method, dns, address-data) + link-local",
          g_variant_n_children(v4) == 4, print(v4));
    g_variant_unref(v4);
    check("set: type a{sa{sv}}", g_variant_is_of_type(on, G_VARIANT_TYPE("a{sa{sv}}")));

    GVariant* twice = nf::with_link_local(on, nf::LL_ENABLED);
    check("set twice: one link-local key, not two", g_variant_equal(twice, on));
    GVariant* off = nf::with_link_local(on, nf::LL_DEFAULT);
    check("undo: back to default", nf::link_local_of(off) == nf::LL_DEFAULT);
    check("undo: the rest as before", nf::method_of(off) == "auto" && nf::id_of(off) == "Home WiFi");

    GVariant* bare = parse("{'connection': {'id': <'wired'>}}");
    GVariant* added = nf::with_link_local(bare, nf::LL_ENABLED);
    check("no ipv4 setting: one is added", nf::link_local_of(added) == nf::LL_ENABLED &&
                                            g_variant_n_children(added) == 2, print(added));

    for (GVariant* v : {wifi, on, twice, off, bare, added}) g_variant_unref(v);

    check("address: loopback has no 169.254", nf::link_local_address("lo").empty());

    std::cout << "-----------------------------------------------\n"
              << g_pass << " pass / " << g_fail << " fail\n";
    return g_fail == 0 ? 0 : 1;
}
