// Replacement for the upstream remote_api.cpp.
// See remote_api.hpp for the design overview.

#include <lib.hpp>

#include "remote_api.hpp"

#include "static_thread.hpp"
#include "event_helper.hpp"
#include "discovery.hpp"
#include "json_line.hpp"

#include <nn.hpp>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>

#ifndef MOD_VERSION_STRING
#define MOD_VERSION_STRING "dread-bridge-dev"
#endif

/**
 * Lifecycle:
 *
 *   1. multiworld_init (game-thread, Lua-driven) calls RemoteApi::Init.
 *   2. Init spawns SocketSpawnThread. PrepareThread() initializes the mutex
 *      and the nn::socket pool, then nn::nifm.
 *   3. Outer worker loop:
 *        resolveBridge() -> tcp_connect() -> sendHello() -> inner poll loop
 *        On disconnect: exponential backoff (1, 2, 5, 10, 30s cap), restart.
 *   4. Inner poll loop:
 *        nn::socket::Poll on the connected socket;
 *        on POLLIN drain bytes into g_inbound and pop \n-terminated lines;
 *        for each line, dispatch by "t" (lua_exec / ping / kick / hello_ack).
 *        For lua_exec: stash src into g_pendingLuaSrc + g_pendingSeq, flip
 *        g_readyForGameThread (game thread's ProcessCommand runs the lua,
 *        builds lua_exec_reply, enqueues).
 *        Pump the send queue (drain via Send + MSG_DONTWAIT).
 *
 * Concurrency: g_sendBuffer is guarded by sendBufferLock (game-thread
 * pushes via Send*Json + worker pulls in pumpOutbound). g_readyForGameThread
 * is atomic — worker sets, game thread clears.
 *
 * NEVER block the worker on the game thread or vice versa.
 */

struct ClientSubscriptions RemoteApi::clientSubs;

namespace {

// Socket / nifm constants — match Nintendo's bsd:u (FreeBSD-derived).
constexpr s32 kAfInet      = AF_INET;
constexpr s32 kSockStream  = SOCK_STREAM;
constexpr s32 kIpprotoTcp  = IPPROTO_TCP;
constexpr s16 kPollIn      = 0x0001;
constexpr s16 kPollErr     = 0x0008;
constexpr s16 kPollHup     = 0x0010;

// Backoff schedule (ms) — matches SMO's 1/2/5/10/30s cap.
constexpr std::uint32_t kBackoffSchedule[] = { 1000, 2000, 5000, 10000, 30000 };
constexpr std::size_t   kBackoffCount = sizeof(kBackoffSchedule) / sizeof(kBackoffSchedule[0]);

constexpr s32 kPollTickMs = 50;
constexpr std::size_t kInboundCap = 16 * 1024;

constexpr unsigned long DefaultTcpAutoBufferSizeMax = 192 * 1024;
constexpr unsigned long MinTransferMemorySize       = 2 * DefaultTcpAutoBufferSizeMax + (128 * 1024);
constexpr unsigned long MinSocketAllocatorSize      = 128 * 1024;
constexpr unsigned long SocketAllocatorSize         = MinSocketAllocatorSize;
constexpr unsigned long TransferMemorySize          = MinTransferMemorySize;
constexpr unsigned long SocketPoolSize              = SocketAllocatorSize + TransferMemorySize;

// State -------------------------------------------------------------

static s32 g_socket = -1;
static std::atomic<bool> g_readyForGameThread{false};
static std::atomic<bool> g_socketConnected{false};

// Inbound: raw byte buffer + write position. We append into here from Recv
// and pop newline-delimited lines.
static u8 g_inbound[kInboundCap];
static std::size_t g_inboundLen = 0;

// Pending lua_exec from PC — stashed by worker, read by game thread.
static RemoteApi::CommandBuffer g_pendingLuaSrc;
static std::size_t g_pendingLuaLen = 0;
static int g_pendingSeq = 0;

static shimmer::util::StaticThread<0x10000> SocketSpawnThread;
static nn::os::MutexType g_sendBufferLock;
static SendBuffer g_sendBuffer;

// Helpers -----------------------------------------------------------

void enqueue(PacketBuffer&& buf) {
    nn::os::LockMutex(&g_sendBufferLock);
    g_sendBuffer.push_back(std::move(buf));
    nn::os::UnlockMutex(&g_sendBufferLock);
}

PacketBuffer make_buffer_from_line(const dread::ap::json::LineBuffer& line) {
    PacketBuffer buf(new std::vector<u8>());
    buf->reserve(line.size() + 1);
    buf->insert(buf->end(),
                reinterpret_cast<const u8*>(line.data()),
                reinterpret_cast<const u8*>(line.data()) + line.size());
    buf->push_back(static_cast<u8>('\n'));
    return buf;
}

void sendHello(const char* layout_uuid_or_empty) {
    nn::oe::DisplayVersion dispVer;
    nn::oe::GetDisplayVersion(&dispVer);
    dread::ap::json::LineBuffer line;
    dread::ap::json::Encoder e{line};
    e.beginObject()
        .key("t").value("hello")
        .key("mod_ver").value(MOD_VERSION_STRING)
        .key("dread_ver").value(dispVer.displayVersion)
        .key("layout_uuid").value(layout_uuid_or_empty)
        .key("device_id").value("")  // PC synthesizes from peer IP if empty
     .endObject();
    enqueue(make_buffer_from_line(line));
}

bool tcp_connect(const dread::ap::BridgeTarget& target) {
    g_socket = nn::socket::Socket(kAfInet, kSockStream, kIpprotoTcp);
    if (g_socket < 0) {
        g_socket = -1;
        return false;
    }

    nn::socket::InAddr ia{};
    if (nn::socket::InetAton(target.host.c_str(), &ia) == 0) {
        nn::socket::Close(g_socket);
        g_socket = -1;
        return false;
    }
    ::sockaddr_in addr{};
    addr.sin_family = static_cast<u8>(kAfInet);
    addr.sin_port = nn::socket::InetHtons(target.port);
    addr.sin_addr.s_addr = ia.addr;

    const u32 rc = nn::socket::Connect(
        g_socket, reinterpret_cast<::sockaddr*>(&addr), sizeof(addr));
    if (rc != 0) {
        nn::socket::Close(g_socket);
        g_socket = -1;
        return false;
    }

    int keepalive = 1;
    nn::socket::SetSockOpt(g_socket, SOL_SOCKET, SO_KEEPALIVE,
                           &keepalive, sizeof(keepalive));
    g_inboundLen = 0;
    g_socketConnected.store(true);
    return true;
}

void close_socket() {
    g_socketConnected.store(false);
    if (g_socket >= 0) {
        nn::socket::Close(g_socket);
        g_socket = -1;
    }
    g_inboundLen = 0;
    // Drop any pending replies; the PC's bridge will reissue on reconnect.
    nn::os::LockMutex(&g_sendBufferLock);
    g_sendBuffer.clear();
    nn::os::UnlockMutex(&g_sendBufferLock);
    g_readyForGameThread.store(false);
}

bool drain_into_inbound() {
    if (g_inboundLen >= kInboundCap) {
        // Buffer full and we still haven't seen a newline — corrupt stream.
        // Reset and continue; a future complete line still reads cleanly.
        g_inboundLen = 0;
    }
    const std::size_t want = kInboundCap - g_inboundLen;
    const s32 got = nn::socket::Recv(
        g_socket, g_inbound + g_inboundLen, want, MSG_DONTWAIT);
    if (got == 0) return false;  // peer FIN
    if (got < 0) return true;    // EWOULDBLOCK — try later
    g_inboundLen += static_cast<std::size_t>(got);
    return true;
}

// Locate the next \n in g_inbound. Returns std::size_t(-1) if absent.
std::size_t find_newline() {
    for (std::size_t i = 0; i < g_inboundLen; ++i) {
        if (g_inbound[i] == '\n') return i;
    }
    return static_cast<std::size_t>(-1);
}

void consume_line(std::size_t nl_idx) {
    if (nl_idx + 1 >= g_inboundLen) {
        g_inboundLen = 0;
    } else {
        std::memmove(g_inbound, g_inbound + nl_idx + 1,
                     g_inboundLen - nl_idx - 1);
        g_inboundLen -= nl_idx + 1;
    }
}

void enqueue_pong(std::int64_t ts_ms) {
    dread::ap::json::LineBuffer line;
    dread::ap::json::Encoder e{line};
    e.beginObject()
        .key("t").value("pong")
        .key("ts_ms").value(ts_ms)
     .endObject();
    enqueue(make_buffer_from_line(line));
}

// Dispatch one decoded JSON line.
void dispatch_line(char* buf, std::size_t len) {
    dread::ap::json::Reader r(buf, len);
    if (!r.enterObject()) return;

    // First pass: find the type.
    std::string_view t_val;
    char saved[16 * 1024];  // copy for second pass since readString mutates
    if (len > sizeof(saved)) return;
    std::memcpy(saved, buf, len);

    std::string_view key;
    while (r.nextField(key)) {
        if (key == "t") {
            if (!r.nextString(t_val)) return;
        } else {
            std::string_view _sv;
            std::int64_t _i;
            bool _b;
            (void)(r.isNull() || r.nextString(_sv) || r.nextInt(_i) || r.nextBool(_b));
        }
    }

    if (t_val == "lua_exec") {
        // Re-parse for seq + src on the saved copy.
        dread::ap::json::Reader r2(saved, len);
        if (!r2.enterObject()) return;
        int seq = 0;
        std::size_t src_len = 0;
        while (r2.nextField(key)) {
            if (key == "seq") {
                std::int64_t v = 0;
                if (!r2.nextInt(v)) return;
                seq = static_cast<int>(v);
            } else if (key == "src") {
                std::string_view src;
                if (!r2.nextString(src)) return;
                src_len = src.size();
                if (src_len > g_pendingLuaSrc.size()) {
                    src_len = g_pendingLuaSrc.size();
                }
                std::memcpy(g_pendingLuaSrc.data(), src.data(), src_len);
            } else {
                std::string_view _sv;
                std::int64_t _i;
                bool _b;
                (void)(r2.isNull() || r2.nextString(_sv) || r2.nextInt(_i) || r2.nextBool(_b));
            }
        }
        g_pendingLuaLen = src_len;
        g_pendingSeq = seq;
        g_readyForGameThread.store(true);
        return;
    }

    if (t_val == "ping") {
        // Re-parse for ts_ms on the saved copy.
        dread::ap::json::Reader r2(saved, len);
        if (!r2.enterObject()) return;
        std::int64_t ts_ms = 0;
        while (r2.nextField(key)) {
            if (key == "ts_ms") {
                if (!r2.nextInt(ts_ms)) return;
            } else {
                std::string_view _sv;
                std::int64_t _i;
                bool _b;
                (void)(r2.isNull() || r2.nextString(_sv) || r2.nextInt(_i) || r2.nextBool(_b));
            }
        }
        enqueue_pong(ts_ms);
        return;
    }

    if (t_val == "hello_ack") {
        // We don't reject on ok=false here — the PC may want to log the
        // mismatch but still keep the wire up. The worker loop runs until
        // POLLHUP; the user will see "rejected" in the PC's bridge log.
        RemoteApi::clientSubs.logging = true;
        RemoteApi::clientSubs.multiWorld = true;
        return;
    }

    if (t_val == "kick") {
        // Soft tear-down: close the socket; outer worker loop will redial.
        // We don't drop g_pendingLuaSrc here — the next reconnect's bootstrap
        // will reinstall whatever was in flight.
        close_socket();
        return;
    }

    // Unknown type — ignore (forward-compatible).
}

void pump_outbound() {
    nn::os::LockMutex(&g_sendBufferLock);
    for (auto& pb : g_sendBuffer) {
        if (pb->empty()) continue;
        const s32 ret = nn::socket::Send(
            g_socket, pb->data(), pb->size(), MSG_DONTWAIT);
        if (ret <= 0) continue;
        if (static_cast<std::size_t>(ret) >= pb->size()) {
            pb->clear();
        } else {
            pb->erase(pb->begin(), pb->begin() + ret);
        }
    }
    g_sendBuffer.erase(
        std::remove_if(g_sendBuffer.begin(), g_sendBuffer.end(),
                       [](const auto& pb) { return pb->size() == 0; }),
        g_sendBuffer.end());
    nn::os::UnlockMutex(&g_sendBufferLock);
}

void prepare_thread() {
    nn::os::InitializeMutex(&g_sendBufferLock, false, 0);
    void* pool = aligned_alloc(0x4000, SocketPoolSize);
    R_ABORT_UNLESS(nn::socket::Initialize(
        pool, SocketPoolSize, SocketAllocatorSize, 14));
}

void worker_main(void*) {
    SocketSpawnThread.SetName("DreadBridgeWorker");
    prepare_thread();

    // Network up — required for nn::socket on a clean boot. nn::nifm is
    // safe to call from this worker thread; SMO does it from the frame
    // thread in main.cpp's GameSystem::init hook, but the worker also
    // works (Dread's game thread doesn't itself use sockets, so there's
    // no race for the one-shot Initialize).
    nn::nifm::Initialize();
    nn::nifm::SubmitNetworkRequestAndWait();

    std::size_t backoff_idx = 0;
    while (true) {
        dread::ap::BridgeTarget target;
        if (!dread::ap::resolveBridge(target)) {
            svcSleepThread(
                static_cast<s64>(kBackoffSchedule[backoff_idx]) * 1'000'000ll);
            if (backoff_idx + 1 < kBackoffCount) ++backoff_idx;
            continue;
        }

        if (!tcp_connect(target)) {
            svcSleepThread(
                static_cast<s64>(kBackoffSchedule[backoff_idx]) * 1'000'000ll);
            if (backoff_idx + 1 < kBackoffCount) ++backoff_idx;
            continue;
        }

        // Connection established — reset backoff.
        backoff_idx = 0;

        // First message: HELLO. layout_uuid is empty until the bootstrap
        // pushes RL.SendLayoutUuid() — the worker thread can't read Lua.
        sendHello("");

        // Inner poll loop.
        while (g_socketConnected.load()) {
            ::pollfd pfd{};
            pfd.fd = g_socket;
            pfd.events = kPollIn;
            pfd.revents = 0;
            const s32 n = nn::socket::Poll(&pfd, 1, kPollTickMs);
            if (n < 0) { close_socket(); break; }
            if (n > 0) {
                if (pfd.revents & (kPollErr | kPollHup)) {
                    close_socket();
                    break;
                }
                if (pfd.revents & kPollIn) {
                    if (!drain_into_inbound()) {
                        close_socket();
                        break;
                    }
                    while (true) {
                        const std::size_t nl = find_newline();
                        if (nl == static_cast<std::size_t>(-1)) break;
                        // Copy into a writable stack buffer for the Reader.
                        // 8 KiB matches the wire's per-line cap.
                        char line_buf[8 * 1024 + 1];
                        const std::size_t take =
                            nl < sizeof(line_buf) - 1 ? nl : sizeof(line_buf) - 1;
                        std::memcpy(line_buf, g_inbound, take);
                        line_buf[take] = '\0';
                        consume_line(nl);
                        dispatch_line(line_buf, take);
                    }
                }
            }
            pump_outbound();
        }
        // Loop back to discovery + reconnect.
    }
}

}  // namespace

// Public API --------------------------------------------------------

void RemoteApi::Init() {
    R_ABORT_UNLESS(SocketSpawnThread.Create(worker_main));
    SocketSpawnThread.Start();
}

void RemoteApi::ProcessCommand(
    lua_State* L,
    const std::function<PacketBuffer(lua_State* L,
                                     CommandBuffer& RecvBuffer,
                                     size_t RecvBufferLength,
                                     int seq)>& processor) {
    if (!g_readyForGameThread.load()) return;
    PacketBuffer buffer = processor(L, g_pendingLuaSrc, g_pendingLuaLen,
                                    g_pendingSeq);
    if (buffer) {
        enqueue(std::move(buffer));
    }
    g_readyForGameThread.store(false);
}

bool RemoteApi::IsConnected() {
    return g_socketConnected.load();
}

// Outbound JSON helpers (game thread / Lua-driven) -------------------

void RemoteApi::SendLogJson(const char* level, const char* msg) {
    if (!g_socketConnected.load()) return;
    if (!clientSubs.logging) return;
    dread::ap::json::LineBuffer line;
    dread::ap::json::Encoder e{line};
    e.beginObject()
        .key("t").value("log")
        .key("level").value(level ? level : "info")
        .key("msg").value(msg ? msg : "")
     .endObject();
    enqueue(make_buffer_from_line(line));
}

void RemoteApi::SendInventoryJson(int index, const char* inventory_json) {
    if (!g_socketConnected.load()) return;
    if (!clientSubs.multiWorld) return;
    // Build manually so we can splice ``inventory_json`` (which is already
    // a valid JSON array literal e.g. ``[1,2,3]``) verbatim. The Encoder
    // doesn't expose a "raw" value helper; using LineBuffer directly is
    // the path of least pain.
    dread::ap::json::LineBuffer line;
    line.append("{\"t\":\"inventory\",\"index\":", 25);
    char tmp[24];
    std::snprintf(tmp, sizeof(tmp), "%d", index);
    line.append(tmp, std::strlen(tmp));
    line.append(",\"inventory\":", 13);
    if (inventory_json) {
        line.append(inventory_json, std::strlen(inventory_json));
    } else {
        line.append("[]", 2);
    }
    line.append('}');
    enqueue(make_buffer_from_line(line));
}

void RemoteApi::SendCollectedJson(const char* hex) {
    if (!g_socketConnected.load()) return;
    if (!clientSubs.multiWorld) return;
    dread::ap::json::LineBuffer line;
    dread::ap::json::Encoder e{line};
    e.beginObject()
        .key("t").value("collected")
        .key("hex").value(hex ? hex : "")
     .endObject();
    enqueue(make_buffer_from_line(line));
}

void RemoteApi::SendReceivedPickupsJson(int count) {
    if (!g_socketConnected.load()) return;
    if (!clientSubs.multiWorld) return;
    dread::ap::json::LineBuffer line;
    dread::ap::json::Encoder e{line};
    e.beginObject()
        .key("t").value("received_pickups")
        .key("count").value(static_cast<std::int64_t>(count))
     .endObject();
    enqueue(make_buffer_from_line(line));
}

void RemoteApi::SendGameStateJson(const char* scenario, bool beaten) {
    if (!g_socketConnected.load()) return;
    if (!clientSubs.multiWorld) return;
    dread::ap::json::LineBuffer line;
    dread::ap::json::Encoder e{line};
    e.beginObject()
        .key("t").value("game_state")
        .key("scenario").value(scenario ? scenario : "")
        .key("beaten").value(beaten)
     .endObject();
    enqueue(make_buffer_from_line(line));
}

void RemoteApi::SendLayoutUuidJson(const char* uuid) {
    if (!g_socketConnected.load()) return;
    dread::ap::json::LineBuffer line;
    dread::ap::json::Encoder e{line};
    e.beginObject()
        .key("t").value("layout_uuid")
        .key("value").value(uuid ? uuid : "")
     .endObject();
    enqueue(make_buffer_from_line(line));
}
