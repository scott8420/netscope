// netafp Keyring -- see Keyring.hpp.
#include "netafp/Keyring.hpp"
#include "netafp/Afp.hpp"

#ifdef NETAFP_HAVE_LIBSECRET
#include <libsecret/secret.h>
#endif

namespace netscope::netafp::keyring {

std::string label(const std::string& host, const std::string& user) {
    return (user.empty() ? std::string("guest") : user) + " on " + short_host(host) + " (AFP, NetScope)";
}

#ifdef NETAFP_HAVE_LIBSECRET

namespace {
const SecretSchema* schema() { return SECRET_SCHEMA_COMPAT_NETWORK; }

std::string take_error(GError* e) {
    std::string s = e && e->message ? e->message : "unknown error";
    if (e) g_error_free(e);
    return "Keyring: " + s;
}

// Search for this server's AFP items; caller frees with g_list_free_full(.., g_object_unref).
GList* search(const std::string& host, GError** err) {
    GHashTable* attrs = g_hash_table_new(g_str_hash, g_str_equal);
    g_hash_table_insert(attrs, (gpointer)"server", (gpointer)host.c_str());
    g_hash_table_insert(attrs, (gpointer)"protocol", (gpointer)"afp");
    GList* found = secret_password_searchv_sync(schema(), attrs,
        static_cast<SecretSearchFlags>(SECRET_SEARCH_ALL | SECRET_SEARCH_UNLOCK), nullptr, err);
    g_hash_table_unref(attrs);
    return found;
}
}  // namespace

bool available() { return true; }

Saved find(const std::string& host) {
    Saved out;
    GError* err = nullptr;
    GList* items = search(host, &err);
    if (err) { out.says = take_error(err); return out; }
    // Newest first: a re-save replaces, but a second user name is its own item.
    SecretRetrievable* best = nullptr;
    guint64 best_t = 0;
    for (GList* l = items; l; l = l->next) {
        auto* r = SECRET_RETRIEVABLE(l->data);
        const guint64 t = secret_retrievable_get_modified(r);
        if (!best || t > best_t) { best = r; best_t = t; }
    }
    if (best) {
        GHashTable* a = secret_retrievable_get_attributes(best);
        if (const char* u = static_cast<const char*>(g_hash_table_lookup(a, "user"))) out.user = u;
        g_hash_table_unref(a);
        SecretValue* v = secret_retrievable_retrieve_secret_sync(best, nullptr, &err);
        if (err) {
            out.says = take_error(err);
        } else if (v) {
            gsize n = 0;
            const gchar* p = secret_value_get(v, &n);
            out.password.assign(p, n);
            out.found = !out.user.empty();
            secret_value_unref(v);
        }
    }
    g_list_free_full(items, g_object_unref);
    return out;
}

std::string save(const std::string& host, const std::string& user, const std::string& password) {
    if (user.empty()) return {};   // guest: nothing to keep
    GHashTable* attrs = g_hash_table_new(g_str_hash, g_str_equal);
    g_hash_table_insert(attrs, (gpointer)"server", (gpointer)host.c_str());
    g_hash_table_insert(attrs, (gpointer)"protocol", (gpointer)"afp");
    g_hash_table_insert(attrs, (gpointer)"user", (gpointer)user.c_str());
    GError* err = nullptr;
    const bool ok = secret_password_storev_sync(schema(), attrs, SECRET_COLLECTION_DEFAULT,
                                                label(host, user).c_str(), password.c_str(), nullptr, &err);
    g_hash_table_unref(attrs);
    if (!ok || err) return take_error(err);
    return {};
}

std::string forget(const std::string& host) {
    GHashTable* attrs = g_hash_table_new(g_str_hash, g_str_equal);
    g_hash_table_insert(attrs, (gpointer)"server", (gpointer)host.c_str());
    g_hash_table_insert(attrs, (gpointer)"protocol", (gpointer)"afp");
    GError* err = nullptr;
    secret_password_clearv_sync(schema(), attrs, nullptr, &err);   // false = nothing there, not an error
    g_hash_table_unref(attrs);
    if (err) return take_error(err);
    return {};
}

#else  // no libsecret

bool available() { return false; }
Saved find(const std::string&) { return {}; }
std::string save(const std::string&, const std::string&, const std::string&) {
    return "NetScope was built without libsecret, so it can't use Keyring.";
}
std::string forget(const std::string&) { return {}; }

#endif

}  // namespace netscope::netafp::keyring
