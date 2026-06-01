// Replacement for the upstream remote_api.{cpp,hpp}.
//
// Topology inversion: the Switch is now the TCP CLIENT; DreadClient binds
// on the PC. The Switch discovers the PC via UDP (see discovery.{cpp,hpp})
// and dials in over TCP. Wire is line-delimited JSON (see json_line.{cpp,hpp}).
//
// The Lua-eval RPC is preserved as one message type — the entire Randovania
// `RL.*` bootstrap layer is unchanged semantically; only the byte stream
// underneath it changed shape.

#pragma once

#include <array>
#include <functional>
#include <memory>
#include <vector>

#include "lua-5.1.5/src/lua.hpp"

typedef std::unique_ptr<std::vector<u8>> PacketBuffer;
using SendBuffer = std::vector<PacketBuffer>;

struct ClientSubscriptions {
    bool logging;
    bool multiWorld;
};

class RemoteApi {
  public:
    // Bumped from 1 to signal the bridge wire format (the PC's BridgeServer
    // also asserts compatibility on HELLO).
    static constexpr const int VERSION = 2;
    // Reserved Lua-exec source size. The wire envelope adds ~30 bytes; the
    // PC's bootstrap chunker targets <=4 KiB to stay within the 8 KiB
    // per-line cap from json_line.
    static constexpr const int BufferSize = 4096;
    static struct ClientSubscriptions clientSubs;
    using CommandBuffer = std::array<char, BufferSize>;

    // One-shot. Called from multiworld_init() in main.cpp after the game's
    // Lua VM is up. Spawns the worker thread.
    static void Init();

    // Game-thread tick. If a queued lua_exec is pending, runs the processor
    // (which calls luaL_loadbuffer + pcall + tostring), wraps the result in
    // a lua_exec_reply envelope, and pushes it onto the send queue.
    static void ProcessCommand(
        lua_State* L,
        const std::function<PacketBuffer(lua_State* L,
                                         CommandBuffer& RecvBuffer,
                                         size_t RecvBufferLength,
                                         int seq)>& processor);

    // Build + enqueue a JSON envelope for a Switch->PC push. Lua-driven
    // pushes (log, inventory, collected, received_pickups, game_state)
    // route through SendJson*. They no-op when no socket / no peer.
    static void SendLogJson(const char* level, const char* msg);
    // ``inventory_json`` is the raw JSON array Lua already composed
    // (e.g. ``[1,2,3]``); injected directly into the envelope's
    // ``"inventory":<...>`` slot.
    static void SendInventoryJson(int index, const char* inventory_json);
    // ``hex`` is the lowercase hex bitfield Lua composed.
    static void SendCollectedJson(const char* hex);
    static void SendReceivedPickupsJson(int count);
    static void SendGameStateJson(const char* scenario, bool beaten);
    static void SendLayoutUuidJson(const char* uuid);

    static bool IsConnected();
};
