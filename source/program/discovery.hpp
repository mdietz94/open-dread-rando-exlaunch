// Runtime bridge discovery via UDP.
//
// Every (re)connect cycle, the worker thread calls resolveBridge() before
// TCP connect. Probe order:
//
//   1. UDP probe -> 127.0.0.1:17776 — covers Ryujinx running on the same
//      host as DreadClient. 250 ms timeout.
//   2. UDP unicast sweep over the BRIDGE_HOST_STRING /24 subnet — every
//      .1..254 host on (seed & 0xFFFFFF00) gets one probe, fired as a tight
//      burst on a single socket. ~2 s collect window (bumped from SMO's 1 s
//      for Wi-Fi headroom on real hardware). First valid reply wins.
//
// Replaces the old 255.255.255.255 broadcast (silently dropped on travel
// routers / mesh repeaters / IGMP-snooping switches). The /24 unicast
// sweep is the production-tested replacement (see smo_archipelago).
//
// On success, fills `out` with the bridge's advertised TCP host:port.
// On total failure, returns false; the caller retries with the exponential
// backoff in the outer worker loop.
//
// Ported from smo_archipelago/switch-mod/src/ap/ApDiscovery.{hpp,cpp} with
// the DiscoveryReport debug-overlay machinery stripped (no on-Switch UI).

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace dread::ap {

#ifdef DISCOVERY_PORT_VALUE
inline constexpr std::uint16_t kDefaultDiscoveryPort = DISCOVERY_PORT_VALUE;
#else
inline constexpr std::uint16_t kDefaultDiscoveryPort = 17776;
#endif

struct BridgeTarget {
    std::string host;
    std::uint16_t port = 17777;
};

bool resolveBridge(
    BridgeTarget& out,
    std::uint16_t discovery_port = kDefaultDiscoveryPort);

}  // namespace dread::ap
