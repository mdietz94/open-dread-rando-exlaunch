#include "lib.hpp"
#include "lib/util/modules.hpp"
#include <nn.hpp>
#include <cstring>
#include <cstdio>
#include "remote_api.hpp"
#include "json_line.hpp"
#include "lua-5.1.5/src/lua.hpp"
#include "dread_types.hpp"
#include "debug_hooks.hpp"
#include "lua_helper.hpp"
#include "item_pickups.hpp"
#include "romfs.hpp"

/* The main executable's pcall, so we get proper error handling. */
int (*exefs_lua_pcall) (lua_State *L, int nargs, int nresults, int errfunc) = NULL;

void multiworld_schedule_update(lua_State* L) {
    lua_getglobal(L, "Game");
    lua_getfield(L, -1, "AddGUISF");

    lua_pushinteger(L, 0);
    lua_pushstring(L, "RemoteLua.Update");
    lua_pushstring(L, "");

    lua_call(L, 3, 0);
    lua_pop(L, 1);
}

int multiworld_init(lua_State* L) {
    RemoteApi::Init();
    multiworld_schedule_update(L);
    return 0;
}

/* Game-thread tick. Runs any pending lua_exec, wraps the result as a
 * `lua_exec_reply` JSON envelope, and enqueues it for the worker thread. */
int multiworld_update(lua_State* outerLuaState) {
    RemoteApi::ProcessCommand(outerLuaState,
        [](lua_State* L, RemoteApi::CommandBuffer& RecvBuffer,
           size_t RecvBufferLength, int seq) -> PacketBuffer {
            size_t resultSize = 0;
            bool outputSuccess = false;
            const char* luaResult = "";

            // +1; use lua's tostring so we properly convert all types
            lua_getglobal(L, "tostring");

            // +1
            int loadResult = luaL_loadbuffer(
                L, RecvBuffer.data(), RecvBufferLength, "remote lua");

            if (loadResult == 0) {
                int pcallResult = exefs_lua_pcall(L, 0, 1, 0);
                lua_call(L, 1, 1);
                luaResult = lua_tolstring(L, 1, &resultSize);
                if (pcallResult == 0) outputSuccess = true;
            } else {
                static const char kErr[] = "error parsing buffer";
                luaResult = kErr;
                resultSize = sizeof(kErr) - 1;
            }

            // Build the lua_exec_reply line. We use LineBuffer directly
            // (rather than the Encoder helper) because the JSON envelope
            // is tiny + fixed-shape and the result string already lives
            // on Lua's stack — avoiding a second copy is worth the
            // hand-rolled emit.
            dread::ap::json::LineBuffer line;
            dread::ap::json::Encoder e{line};
            e.beginObject()
                .key("t").value("lua_exec_reply")
                .key("seq").value(static_cast<std::int64_t>(seq))
                .key("ok").value(outputSuccess)
                .key("result").value(std::string_view(luaResult, resultSize))
             .endObject();

            PacketBuffer pb(new std::vector<u8>());
            pb->reserve(line.size() + 1);
            pb->insert(pb->end(),
                       reinterpret_cast<const u8*>(line.data()),
                       reinterpret_cast<const u8*>(line.data()) + line.size());
            pb->push_back(static_cast<u8>('\n'));
            return pb;
        });

    multiworld_schedule_update(outerLuaState);
    return 0;
}

/* RemoteLua.SendLog(level, msg) */
int gamelog_send(lua_State* L) {
    const char* level = lua_tostring(L, 1);
    const char* msg = lua_tostring(L, 2);
    if (msg == nullptr) {
        // Legacy single-arg call: treat lone string as msg with level=info.
        msg = level;
        level = "info";
    }
    RemoteApi::SendLogJson(level ? level : "info", msg ? msg : "");
    return 0;
}

/* RemoteLua.SendInventory(indexInt, jsonArrStr) */
int inventory_send(lua_State* L) {
    int index = static_cast<int>(lua_tointeger(L, 1));
    const char* inv = lua_tostring(L, 2);
    RemoteApi::SendInventoryJson(index, inv ? inv : "[]");
    return 0;
}

/* RemoteLua.SendIndices(hexStr) */
int indices_send(lua_State* L) {
    const char* hex = lua_tostring(L, 1);
    RemoteApi::SendCollectedJson(hex ? hex : "");
    return 0;
}

/* RemoteLua.SendReceivedPickups(countInt) */
int recv_pickups_send(lua_State* L) {
    int count = static_cast<int>(lua_tointeger(L, 1));
    RemoteApi::SendReceivedPickupsJson(count);
    return 0;
}

/* RemoteLua.SendNewGameState(scenarioStr, beatenBool) */
int new_game_state_send(lua_State* L) {
    const char* scenario = lua_tostring(L, 1);
    bool beaten = lua_toboolean(L, 2);
    RemoteApi::SendGameStateJson(scenario ? scenario : "", beaten);
    return 0;
}

/* RemoteLua.SendLayoutUuid(uuidStr) — pushed once after bootstrap so the
 * PC can fix up the connection's device_id (HELLO is fired before Lua
 * land is up). Bootstrap can also re-push if Init.sLayoutUUID changes
 * after a save load. */
int layout_uuid_send(lua_State* L) {
    const char* uuid = lua_tostring(L, 1);
    RemoteApi::SendLayoutUuidJson(uuid ? uuid : "");
    return 0;
}

int is_connected(lua_State* L) {
    lua_pushboolean(L, RemoteApi::IsConnected());
    return 1;
}


static const luaL_Reg multiworld_lib[] = {
  {"Init", multiworld_init},
  {"Update", multiworld_update},
  {"SendLog", gamelog_send},
  {"SendInventory", inventory_send},
  {"SendIndices", indices_send},
  {"SendReceivedPickups", recv_pickups_send},
  {"SendNewGameState", new_game_state_send},
  {"SendLayoutUuid", layout_uuid_send},
  {"Connected", is_connected},
  {NULL, NULL}
};

HOOK_DEFINE_TRAMPOLINE(LuaRegisterGlobals) {
    static void Callback(lua_State* L) {
        Orig(L);

        nn::oe::DisplayVersion dispVer;
        nn::oe::GetDisplayVersion(&dispVer);
        lua_pushstring(L, dispVer.displayVersion);
        lua_setglobal(L, "GameVersion");

        lua_pushcfunction(L, luaopen_debug);
        lua_pushstring(L, "debug");
        lua_call(L, 1, 0);

        luaL_register(L, "RemoteLua", multiworld_lib);

        lua_pushinteger(L, RemoteApi::VERSION);
        lua_setfield(L, -2, "Version");

        lua_pushinteger(L, RemoteApi::BufferSize);
        lua_setfield(L, -2, "BufferSize");

        odr::debug::InstallLuaLibrary(L);
        odr::pickups::InstallLuaLibrary(L);
    }
};

void getVersionOffsets(functionOffsets *offsets)
{
    nn::oe::DisplayVersion dispVer;
    nn::oe::GetDisplayVersion(&dispVer);

    if(strcmp(dispVer.displayVersion, "2.1.0") == 0)
    {
        offsets->crc64 = 0x1570;
        offsets->CFilePathStrIdCtor = 0x166C8;
        offsets->luaRegisterGlobals = 0x010aed50;
        offsets->lua_pcall = 0x010a3a80;
        offsets->LogWarn = 0x1094820;
        offsets->CallFunctionWithArguments = 0x790;

        offsets->OnCollectPickup = 0x9d0e28;
        offsets->PlayPickupSound = 0x9d16c4;
        offsets->ShowItemPickupMessage = 0xb5fd9c;

        offsets->PlaySoundWithCallback = 0xfd90d8;

        offsets->StaticStringBank = 0x1d552d0;
        offsets->FindOrCreateStringInstance = 0x406dc;
    }
    else /* 1.0.0 - 2.0.0 */
    {
        offsets->crc64 = 0x1570;
        offsets->CFilePathStrIdCtor = 0x16624;
        offsets->luaRegisterGlobals = 0x106ce90;
        offsets->lua_pcall = 0x1061bc0;
        offsets->LogWarn = 0x1052a70;
        offsets->CallFunctionWithArguments = 0x790;

        offsets->OnCollectPickup = 0x9ce5c8;
        offsets->PlayPickupSound = 0x9cee64;
        offsets->ShowItemPickupMessage = 0xb5395c;

        offsets->PlaySoundWithCallback = 0xf975f8;

        offsets->StaticStringBank = 0x1cfc2d0;
        offsets->FindOrCreateStringInstance = 0x4063c;
    }
}

extern "C" void exl_main(void* x0, void* x1)
{
    functionOffsets offsets;
    exl::hook::Initialize();
    getVersionOffsets(&offsets);

    LuaRegisterGlobals::InstallAtOffset(offsets.luaRegisterGlobals);
    odr::debug::InstallHooks(&offsets);
    odr::lua::InstallFunctions(&offsets);
    odr::pickups::InstallHooks(&offsets);
    odr::romfs::InstallHooks(&offsets);

    exefs_lua_pcall = (int (*) (lua_State *L, int nargs, int nresults, int errfunc))
        exl::util::modules::GetTargetOffset(offsets.lua_pcall);
}

extern "C" NORETURN void exl_exception_entry()
{
    EXL_ABORT(0x420);
}
