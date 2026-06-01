// See discovery.hpp for the design rationale. This file ports
// smo_archipelago/switch-mod/src/ap/ApDiscovery.cpp to the exlaunch
// environment.
//
// We deliberately do NOT include <sys/socket.h> / <arpa/inet.h> here:
// the exlaunch SDK's nn::socket declarations use ``nn::socket::InAddr``
// (its own private alias), and the BSD ``in_addr`` from the system
// headers triggers a ``cannot convert 'in_addr*' to 'nn::socket::InAddr*'``
// error on InetAton. SMO sidesteps this by forward-declaring the symbols
// it needs with the BSD types at file scope; we follow the same pattern
// for ``SendTo`` / ``RecvFrom`` / ``InetAton`` / ``Poll`` etc.

#include <lib.hpp>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "discovery.hpp"
#include "json_line.hpp"

#ifndef MOD_VERSION_STRING
#define MOD_VERSION_STRING "dread-bridge-dev"
#endif

#ifndef BRIDGE_HOST_STRING
#define BRIDGE_HOST_STRING "127.0.0.1"
#endif

// File-scope BSD-style socket types, matching nn::socket's underlying ABI.
// Identical layout to the SMO Switch client (`ApClient.cpp` / `ApDiscovery.cpp`)
// which is shipping on retail HW.
struct in_addr_local { u32 s_addr; };
struct sockaddr_local {
    u8 sa_len;
    u8 sa_family;
    u16 sa_port;
    in_addr_local sa_addr;
    u8 sa_zero[8];
};
struct pollfd_local { s32 fd; short events; short revents; };

namespace nn::socket {
    s32 Socket(s32, s32, s32);
    s32 SendTo(s32, const void*, unsigned long, s32,
               const ::sockaddr_local*, u32);
    s32 RecvFrom(s32, void*, unsigned long, s32,
                 ::sockaddr_local*, u32*);
    u32 Close(s32);
    s32 Poll(::pollfd_local*, unsigned long, s32);
    u16 InetHtons(u16);
    s32 InetAton(const char*, ::in_addr_local*);
    s32 GetLastErrno();
}

namespace dread::ap {

namespace {

constexpr s32 kAfInet      = 2;
constexpr s32 kSockDgram   = 2;
constexpr s32 kIpprotoUdp  = 17;
constexpr s16 kPollIn      = 0x0001;

constexpr std::uint32_t kSweepCollectMs   = 2000;
constexpr std::uint32_t kLoopbackProbeMs  = 250;
constexpr std::uint32_t kInnerPollMs      = 50;

constexpr std::size_t kReplyBufBytes = 512;
constexpr std::uint32_t kSubnetMask  = 0xFFFFFF00u;
constexpr int           kMaxSweepHosts = 254;

std::size_t buildProbe(char* dst, std::size_t cap) {
    json::LineBuffer line;
    json::Encoder e{line};
    e.beginObject()
        .key("t").value("discover")
        .key("mod_ver").value(MOD_VERSION_STRING)
     .endObject();
    line.append('\n');
    const std::size_t take = line.size() < cap ? line.size() : cap;
    std::memcpy(dst, line.data(), take);
    return take;
}

s32 openUdpSocket() {
    return nn::socket::Socket(kAfInet, kSockDgram, kIpprotoUdp);
}

void closeSocket(s32 fd) {
    (void)nn::socket::Close(fd);
}

bool waitReadable(s32 fd, std::uint32_t timeout_ms) {
    ::pollfd_local pfd{};
    pfd.fd = fd;
    pfd.events = kPollIn;
    pfd.revents = 0;
    const s32 n = nn::socket::Poll(&pfd, 1, static_cast<s32>(timeout_ms));
    if (n <= 0) return false;
    return (pfd.revents & kPollIn) != 0;
}

bool parseReply(const char* data, std::size_t len, BridgeTarget& out) {
    char scratch[kReplyBufBytes];
    if (len > sizeof(scratch)) len = sizeof(scratch);
    std::memcpy(scratch, data, len);

    json::Reader r(scratch, len);
    if (!r.enterObject()) return false;

    bool saw_t_bridge = false;
    char host[64] = {0};
    int port = 0;

    std::string_view key;
    while (r.nextField(key)) {
        if (key == "t") {
            std::string_view t_val;
            if (!r.nextString(t_val)) return false;
            if (t_val == "bridge") saw_t_bridge = true;
        } else if (key == "host") {
            std::string_view host_val;
            if (!r.nextString(host_val)) return false;
            const std::size_t take = host_val.size() < sizeof(host) - 1
                ? host_val.size() : sizeof(host) - 1;
            std::memcpy(host, host_val.data(), take);
            host[take] = '\0';
        } else if (key == "port") {
            std::int64_t p = 0;
            if (!r.nextInt(p)) return false;
            port = static_cast<int>(p);
        } else {
            std::string_view _sv;
            std::int64_t _i;
            bool _b;
            (void)(r.isNull() || r.nextString(_sv) || r.nextInt(_i) || r.nextBool(_b));
        }
    }
    if (!saw_t_bridge || host[0] == '\0' || port <= 0 || port > 0xFFFF) {
        return false;
    }
    out.host = host;
    out.port = static_cast<std::uint16_t>(port);
    return true;
}

void makeSockAddrFromU32(u32 host_nbo, std::uint16_t port, ::sockaddr_local& out) {
    std::memset(&out, 0, sizeof(out));
    out.sa_len    = sizeof(out);
    out.sa_family = static_cast<u8>(kAfInet);
    out.sa_port   = nn::socket::InetHtons(port);
    out.sa_addr.s_addr = host_nbo;
}

bool oneProbeLiteral(s32 fd, const char* probe_data, std::size_t probe_len,
                     const char* host, std::uint16_t port,
                     std::uint32_t timeout_ms, BridgeTarget& out) {
    ::in_addr_local ia{};
    if (nn::socket::InetAton(host, &ia) == 0) return false;
    ::sockaddr_local addr{};
    addr.sa_len = sizeof(addr);
    addr.sa_family = static_cast<u8>(kAfInet);
    addr.sa_port = nn::socket::InetHtons(port);
    addr.sa_addr = ia;

    const s32 sent = nn::socket::SendTo(
        fd, probe_data, probe_len, 0, &addr, sizeof(addr));
    if (sent < 0) return false;

    if (!waitReadable(fd, timeout_ms)) return false;
    char buf[kReplyBufBytes];
    ::sockaddr_local from{};
    u32 from_len = sizeof(from);
    const s32 got = nn::socket::RecvFrom(
        fd, buf, sizeof(buf), 0, &from, &from_len);
    if (got <= 0) return false;
    return parseReply(buf, static_cast<std::size_t>(got), out);
}

bool sweepSubnet(s32 fd, const char* probe_data, std::size_t probe_len,
                 u32 seed_ip_nbo, std::uint16_t port, BridgeTarget& out) {
    auto byteswap32 = [](u32 v) -> u32 {
        return ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8) |
               ((v & 0x00FF0000u) >> 8)  | ((v & 0xFF000000u) >> 24);
    };

    const u32 seed_ho = byteswap32(seed_ip_nbo);
    const u32 mask_ho = kSubnetMask;
    const u32 net_ho  = seed_ho & mask_ho;
    const u32 bcast_ho = net_ho | ~mask_ho;

    int sent_count = 0;
    for (u32 ip_ho = net_ho + 1;
         ip_ho < bcast_ho && sent_count < kMaxSweepHosts; ++ip_ho) {
        ::sockaddr_local addr{};
        makeSockAddrFromU32(byteswap32(ip_ho), port, addr);
        const s32 n = nn::socket::SendTo(
            fd, probe_data, probe_len, 0, &addr, sizeof(addr));
        if (n >= 0) ++sent_count;
    }

    const int max_polls = static_cast<int>(kSweepCollectMs / kInnerPollMs);
    for (int i = 0; i < max_polls; ++i) {
        if (!waitReadable(fd, kInnerPollMs)) continue;
        char buf[kReplyBufBytes];
        ::sockaddr_local from{};
        u32 from_len = sizeof(from);
        const s32 got = nn::socket::RecvFrom(
            fd, buf, sizeof(buf), 0, &from, &from_len);
        if (got <= 0) continue;
        BridgeTarget t;
        if (parseReply(buf, static_cast<std::size_t>(got), t)) {
            out = t;
            return true;
        }
    }
    return false;
}

}  // namespace

bool resolveBridge(BridgeTarget& out, std::uint16_t discovery_port) {
    char probe[kReplyBufBytes];
    const std::size_t probe_len = buildProbe(probe, sizeof(probe));
    if (probe_len == 0) return false;

    // ---- Step 1: loopback (Ryujinx-on-same-host) ----
    {
        const s32 fd = openUdpSocket();
        if (fd >= 0) {
            BridgeTarget t;
            const bool ok = oneProbeLiteral(
                fd, probe, probe_len,
                "127.0.0.1", discovery_port,
                kLoopbackProbeMs, t);
            closeSocket(fd);
            if (ok) {
                out = t;
                return true;
            }
        }
    }

    // ---- Step 2: subnet sweep using BRIDGE_HOST_STRING as the seed ----
    ::in_addr_local seed_ia{};
    if (nn::socket::InetAton(BRIDGE_HOST_STRING, &seed_ia) == 0) {
        return false;
    }
    const u32 seed_nbo = seed_ia.s_addr;

    const s32 fd = openUdpSocket();
    if (fd < 0) return false;
    BridgeTarget t;
    const bool ok = sweepSubnet(fd, probe, probe_len, seed_nbo,
                                discovery_port, t);
    closeSocket(fd);
    if (ok) {
        out = t;
        return true;
    }
    return false;
}

}  // namespace dread::ap
