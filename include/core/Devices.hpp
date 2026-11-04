#pragma once
// core::Devices -- the device model (s002). GTK-free. Replaces Cairn's Stones.
//
// TWO TABLES, ONE DEVICE. What the network says and what Scott chose are kept
// apart, because they have different owners and different undo rules:
//
//   * Network  -- what discovery reported: name, model, addresses, services,
//                 online or stale. Written ONLY by the feed (core/Feed.hpp).
//                 Not a user choice, so never an undo step.
//   * Choices  -- what Scott chose about a device: nickname, favourite,
//                 hidden. Meets the journal's contract; every write goes
//                 through Journal::run (the one door) and the selftest walks
//                 the verb table.
//
// Kept apart, an undo of "Rename tc01" puts back the old nickname and never
// rolls back an address the feed delivered in between. A device's row is the
// join of the two (rows()), computed, never stored.
//
// The id is the advertised host name, lower-case ("tc01.local"): stable across
// address changes, which is the whole point on a network where tc01's address
// is self-assigned.
#include "core/Undo.hpp"

#include <chrono>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace netscope::core {

using DeviceId = std::string;

// ── what discovery reports ─────────────────────────────────────────────────
struct Service {
    std::string kind;    // "afp", "smb", "ssh", "sftp", "nfs", "http", "device-info", ...
    int         port = 0;
    bool operator==(const Service&) const = default;
};

struct Sighting {
    DeviceId                 id;          // "tc01.local"
    std::string              name;        // as advertised: "tc01"
    std::string              model;       // from device-info if any: "TimeCapsule8,119"
    std::vector<std::string> addresses;   // IPv4 / IPv6, as reported
    std::vector<Service>     services;
    bool operator==(const Sighting&) const = default;
};

enum class Presence { Online, Stale };

struct Seen {
    Sighting sighting;
    Presence presence = Presence::Online;
    bool operator==(const Seen&) const = default;
};

// The discovered table. Not journalled: the feed is its only writer.
class Network {
public:
    // A device announced (or re-announced): it is online with these facts.
    void seen(const Sighting& s);
    // A device withdrew or timed out: kept, marked stale. Returns false if unknown.
    bool lost(const DeviceId& id);
    // It is part of another device now (milestone 13): off the list entirely.
    bool forget(const DeviceId& id);

    std::optional<Seen>   get(const DeviceId& id) const;
    std::vector<DeviceId> ids() const;
    std::size_t           count() const { return m_items.size(); }

    void set_on_change(std::function<void()> f) { m_changed = std::move(f); }

private:
    std::map<DeviceId, Seen> m_items;
    std::function<void()>    m_changed;
};

// ── what Scott chose ───────────────────────────────────────────────────────
// A share Scott pinned (milestone 7): listed without browsing, and what the
// Quick Settings tile will offer. Kept by URI; the name is what GIO called it.
struct Pin {
    std::string name;   // "pub"
    std::string uri;    // "smb://raspi.local/pub/"
    bool operator==(const Pin&) const = default;
};

struct Choice {
    std::string      nickname;          // "" = show the advertised name
    bool             favourite = false; // sorts to the top
    bool             hidden    = false; // off the list unless "Show hidden"
    std::vector<Pin> pins;              // in the order pinned
    bool operator==(const Choice&) const = default;
};

class Choices {
public:
    using Id    = DeviceId;
    using Value = Choice;

    // ── the journal's contract ──────────────────────────────────────────────
    // A device with no choices has no record: get() is nullopt, and putting a
    // default Choice erases it, so "nothing chosen" has one shape.
    std::optional<Choice> get(const Id& id) const;
    void put(const Id& id, const std::optional<Choice>& v);

    Choice of(const Id& id) const { return get(id).value_or(Choice{}); }
    std::size_t count() const { return m_items.size(); }
    const std::map<Id, Choice>& all() const { return m_items; }

    // ── the one-door guard (same shape as the journal's other stores) ──────
    void set_door(std::function<bool()> open) { m_door = std::move(open); }
    int  strays() const { return m_strays; }
    // Restoring saved choices at startup is not a user choice: say so, by scope.
    struct Loading {
        explicit Loading(Choices& c) : m_c(c) { ++m_c.m_loading; }
        ~Loading() { --m_c.m_loading; }
        Choices& m_c;
    };

    void set_on_change(std::function<void()> f) { m_changed = std::move(f); }

private:
    std::map<Id, Choice>  m_items;
    int                   m_strays = 0;
    int                   m_loading = 0;
    std::function<bool()> m_door;
    std::function<void()> m_changed;
};

using DeviceJournal = Journal<Choices>;

// ── persistence: the choices pump (s002, milestone 3) ──────────────────────
// On disk: { "version": 1, "devices": { "tc01.local": { "nickname": "Basement",
// "favourite": true, "pins": [ { "name": "pub", "uri": "smb://..." } ] } } }. Fields at their default are omitted (Folio's
// lesson: don't rewrite what didn't change). Encode and decode sit together in
// Devices.cpp so they can't drift. The UI resolves the path (XDG) and hands it
// in. A load is not a user choice: it runs inside Choices::Loading and is not
// an undo step. First run, unreadable or unparseable -> nothing loaded, no throw.
int  load_choices(const std::string& file, Choices& into);   // how many loaded
bool save_choices(const std::string& file, const Choices& from);   // atomic: tmp + rename

// ── the join: one row per device, in list order ────────────────────────────
struct Row {
    DeviceId    id;
    std::string title;      // nickname, else the advertised name
    std::string advertised; // the advertised name (shown beside a nickname)
    std::string model;
    std::string address;    // the first address, "" if none
    std::string services;   // "AFP, SMB, SSH" -- display kinds, de-duplicated
    Presence    presence = Presence::Online;
    bool        favourite = false;
    bool        hidden    = false;
};

// Favourites first, then online before stale, then by title (case-blind).
// Hidden devices are left out unless show_hidden.
std::vector<Row> rows(const Network& net, const Choices& ch, bool show_hidden);

// "afp" -> "AFP", "device-info" -> "" (bookkeeping, not something to connect to).
std::string service_label(const std::string& kind);
// The detail line's longer label: "Printer (IPP)", "Scanner (eSCL)", ...
std::string service_detail(const std::string& kind);

// ── the verbs: every user choice the Devices page offers ───────────────────
namespace devices {
// Empty or whitespace-only clears the nickname. Same text: no step.
bool rename(DeviceJournal& j, Choices& c, const Network& n, const DeviceId& id, const std::string& nickname);
bool set_favourite(DeviceJournal& j, Choices& c, const Network& n, const DeviceId& id, bool on);
bool set_hidden(DeviceJournal& j, Choices& c, const Network& n, const DeviceId& id, bool on);
// Milestone 7. Pinning a pinned URI, or unpinning one that isn't: no step.
bool set_pinned(DeviceJournal& j, Choices& c, const Network& n, const DeviceId& id, const Pin& share, bool on);
bool is_pinned(const Choices& c, const DeviceId& id, const std::string& uri);

// The verb table: every verb, by name, runnable on a fixture. The selftest
// walks it (one door, undo restores, redo re-applies). A verb the page offers
// and this table lacks is the gap the gate cannot see -- add it here in the
// same change that adds the button.
struct Verb {
    std::string name;
    std::function<void(DeviceJournal&, Choices&, const Network&, const DeviceId& first)> run;
};
const std::vector<Verb>& verbs();
}  // namespace devices

}  // namespace netscope::core
