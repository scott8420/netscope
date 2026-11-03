// netdiag -- the SMB check (milestone 6). One SMB2 NEGOTIATE, one reply.
//
// The request offers SMB 2.0.2 to 3.0.2. 3.1.1 is left out on purpose: offering
// it obliges negotiate contexts, and the question here is only "is a file
// server answering, and how new is it?" -- not a session.
#include "netdiag/Checks.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace netscope::netdiag {

namespace {
const int kDialects[] = {0x0202, 0x0210, 0x0300, 0x0302};

void put16(std::vector<unsigned char>& b, int v) { b.push_back(v & 0xFF); b.push_back((v >> 8) & 0xFF); }
void put32(std::vector<unsigned char>& b, uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back((v >> (8 * i)) & 0xFF); }
int  get16(const std::vector<unsigned char>& b, std::size_t at) { return b[at] | (b[at + 1] << 8); }
uint32_t get32(const std::vector<unsigned char>& b, std::size_t at) {
    return uint32_t(b[at]) | uint32_t(b[at + 1]) << 8 | uint32_t(b[at + 2]) << 16 | uint32_t(b[at + 3]) << 24;
}

// Waits for fd to be ready for `events`; false on timeout or error.
bool wait_for(int fd, short events, std::chrono::milliseconds timeout) {
    pollfd p{fd, events, 0};
    return poll(&p, 1, static_cast<int>(timeout.count())) == 1;
}
}  // namespace

std::vector<unsigned char> smb2_negotiate_request() {
    std::vector<unsigned char> m;
    // SMB2 header, 64 bytes.
    m.insert(m.end(), {0xFE, 'S', 'M', 'B'});
    put16(m, 64);      // StructureSize
    put16(m, 0);       // CreditCharge
    put32(m, 0);       // Status
    put16(m, 0);       // Command: NEGOTIATE
    put16(m, 1);       // CreditRequest
    put32(m, 0);       // Flags
    put32(m, 0);       // NextCommand
    m.insert(m.end(), 8, 0);   // MessageId
    put32(m, 0);       // Reserved / ProcessId
    put32(m, 0);       // TreeId
    m.insert(m.end(), 8, 0);   // SessionId
    m.insert(m.end(), 16, 0);  // Signature
    // NEGOTIATE body.
    put16(m, 36);      // StructureSize
    put16(m, static_cast<int>(std::size(kDialects)));
    put16(m, 1);       // SecurityMode: signing enabled
    put16(m, 0);       // Reserved
    put32(m, 0);       // Capabilities
    for (int i = 0; i < 16; ++i) m.push_back(static_cast<unsigned char>(0x4E + i));  // ClientGuid: fixed, harmless
    m.insert(m.end(), 8, 0);   // ClientStartTime
    for (int d : kDialects) put16(m, d);
    // NetBIOS session header in front: 0, then a 24-bit big-endian length.
    const auto n = m.size();
    std::vector<unsigned char> out{0, static_cast<unsigned char>(n >> 16), static_cast<unsigned char>(n >> 8),
                                   static_cast<unsigned char>(n)};
    out.insert(out.end(), m.begin(), m.end());
    return out;
}

SmbReply parse_smb2_negotiate(const std::vector<unsigned char>& b) {
    SmbReply r;
    if (b.size() < 8) { r.why = "the reply was too short"; return r; }
    if (b[4] == 0xFF && b[5] == 'S' && b[6] == 'M' && b[7] == 'B') { r.smb1_only = true; return r; }
    if (!(b[4] == 0xFE && b[5] == 'S' && b[6] == 'M' && b[7] == 'B')) { r.why = "it isn't SMB"; return r; }
    if (b.size() < 4 + 64 + 6) { r.why = "the reply was cut short"; return r; }
    const uint32_t status = get32(b, 4 + 8);
    if (status != 0) {
        char hex[16];
        std::snprintf(hex, sizeof hex, "0x%08X", status);
        r.why = std::string("the server refused the negotiation (status ") + hex + ")";
        return r;
    }
    const std::size_t body = 4 + 64;
    if (get16(b, body) != 65) { r.why = "the reply's body is malformed"; return r; }
    r.smb2 = true;
    r.dialect = get16(b, body + 4);
    return r;
}

std::string dialect_name(int d) {
    switch (d) {
    case 0x0202: return "SMB 2.0.2";
    case 0x0210: return "SMB 2.1";
    case 0x0300: return "SMB 3.0";
    case 0x0302: return "SMB 3.0.2";
    case 0x0311: return "SMB 3.1.1";
    default: {
        char hex[16];
        std::snprintf(hex, sizeof hex, "0x%04X", d);
        return std::string("SMB dialect ") + hex;
    }
    }
}

Finding check_smb(const std::string& addr, int port, std::chrono::milliseconds timeout) {
    Finding r{"smb", Verdict::Ok, {}};
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(static_cast<uint16_t>(port));
    if (inet_pton(AF_INET, addr.c_str(), &sa.sin_addr) != 1 || port <= 0) {
        r.verdict = Verdict::Skip;
        r.says = "SMB: nothing to try.";
        return r;
    }
    const int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) { r.verdict = Verdict::Skip; r.says = std::string("Couldn't open a socket: ") + std::strerror(errno); return r; }
    auto fail = [&](const std::string& says) { close(fd); r.verdict = Verdict::Fail; r.says = says; return r; };

    if (connect(fd, reinterpret_cast<sockaddr*>(&sa), sizeof sa) != 0) {
        if (errno != EINPROGRESS || !wait_for(fd, POLLOUT, timeout)) return fail("SMB: couldn't connect to port " + std::to_string(port) + ".");
        int err = 0; socklen_t len = sizeof err;
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
        if (err != 0) return fail("SMB: couldn't connect: " + std::string(std::strerror(err)) + ".");
    }
    const auto req = smb2_negotiate_request();
    std::size_t sent = 0;
    while (sent < req.size()) {
        if (!wait_for(fd, POLLOUT, timeout)) return fail("SMB: the server stopped taking data.");
        const ssize_t n = send(fd, req.data() + sent, req.size() - sent, MSG_NOSIGNAL);
        if (n <= 0) return fail("SMB: the server closed the connection.");
        sent += static_cast<std::size_t>(n);
    }
    // Read the NetBIOS header, then as much of the message as it names (capped).
    std::vector<unsigned char> got;
    std::size_t want = 4;
    while (got.size() < want) {
        if (!wait_for(fd, POLLIN, timeout)) return fail("SMB: port " + std::to_string(port) + " is open but no SMB reply came.");
        unsigned char buf[1024];
        const ssize_t n = recv(fd, buf, sizeof buf, 0);
        if (n <= 0) {
            if (got.size() >= 8) break;   // enough to tell SMB1 from SMB2
            return fail("SMB: the server hung up without answering.");
        }
        got.insert(got.end(), buf, buf + n);
        if (want == 4 && got.size() >= 4)
            want = std::min<std::size_t>(4 + ((std::size_t(got[1]) << 16) | (std::size_t(got[2]) << 8) | got[3]), 4 + 4096);
    }
    close(fd);
    const auto reply = parse_smb2_negotiate(got);
    if (reply.smb2) {
        r.says = "A file server answers: " + dialect_name(reply.dialect) + ".";
        return r;
    }
    r.verdict = Verdict::Fail;
    r.says = reply.smb1_only ? "Only SMB1 answers: an old server. Modern clients, Linux included, turn SMB1 off by default."
                             : "SMB: port " + std::to_string(port) + " answered, but " + reply.why + ".";
    return r;
}

}  // namespace netscope::netdiag
