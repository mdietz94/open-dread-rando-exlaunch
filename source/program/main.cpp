#include "lib.hpp"
#include "lib/util/modules.hpp"
#include <nn.hpp>
#include <cstring>
#include "remote_api.hpp"
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

/* This functions is called perodically from the game and calls RemoteApi::ProcessCommand with a callback function */
int multiworld_update(lua_State* outerLuaState) {
    /* Callback is executing the RecvBuffer as lua code */
    RemoteApi::ProcessCommand(outerLuaState, [](lua_State* L, RemoteApi::CommandBuffer& RecvBuffer, size_t RecvBufferLength) -> PacketBuffer {
        size_t resultSize = 0;          // length of the lua string response (without \0)
        bool outputSuccess = false;     // was the lua function call sucessfully
        PacketBuffer sendBuffer(new std::vector<u8>());               // sendBuffer to store the result. this pointer is returned

        // +1; use lua's tostring so we properly convert all types
        lua_getglobal(L, "tostring");

        // +1
        int loadResult = luaL_loadbuffer(L, RecvBuffer.data() + 5, RecvBufferLength - 5, "remote lua");

        if (loadResult == 0) {
            // -1, +1 - call the code we just loaded
            int pcallResult = exefs_lua_pcall(L, 0, 1, 0);
            // -2, +1 - call tostring with the result of that
            lua_call(L, 1, 1);

            const char* luaResult = lua_tolstring(L, 1, &resultSize);

            if (pcallResult == 0) {
                // success! top string is the entire result
                outputSuccess = true;
            }
            sendBuffer->insert(sendBuffer->begin(), luaResult, luaResult + resultSize);
        } else {
            std::string errorMessage = "error parsing buffer: " + std::to_string(loadResult);
            const char* errorAsCString = errorMessage.c_str();
            resultSize = errorMessage.size();
            sendBuffer->insert(sendBuffer->begin(), errorAsCString, errorAsCString + resultSize);
        }
        sendBuffer->insert(sendBuffer->begin(), PACKET_REMOTE_LUA_EXEC);
        sendBuffer->insert(sendBuffer->begin() + 1, outputSuccess);
        sendBuffer->insert(sendBuffer->begin() + 2, resultSize & 0xff);
        sendBuffer->insert(sendBuffer->begin() + 3, (resultSize >> 8)  & 0xff);
        sendBuffer->insert(sendBuffer->begin() + 4, (resultSize >> 16)  & 0xff);
        return sendBuffer;
    });

    // Register calling update again
    multiworld_schedule_update(outerLuaState);
    return 0;
}

PacketBuffer create_packet_from_lua_string(lua_State* L) {
    size_t resultSize = 0;          // length of the lua string response (without \0)
    PacketBuffer sendBuffer(new std::vector<u8>());  // sendBuffer to store the result. this pointer is returned

    const char* luaResult = lua_tolstring(L, 1, &resultSize);

    uint32_t resultAs32Bit = resultSize;
    u8* chars = reinterpret_cast<u8*>(&resultAs32Bit);
    // If you move the following line by one line down the compiler throws a warning?!?
    sendBuffer->insert(sendBuffer->begin(), luaResult, luaResult + resultAs32Bit);
    sendBuffer->insert(sendBuffer->begin(), chars, chars + sizeof(uint32_t));

    return sendBuffer;
}

void build_and_send_message(lua_State* L, PacketType outerType) {
    RemoteApi::SendMessage(L, outerType, [](lua_State* L, PacketType packetType) -> PacketBuffer {
        PacketBuffer sendBuffer = create_packet_from_lua_string(L);
        sendBuffer->insert(sendBuffer->begin(), (u8)packetType);
        return sendBuffer;
    });
}

/* Gets called by lua to send a log message from Game.LogWarn */
int gamelog_send(lua_State* L) {
    if (RemoteApi::clientSubs.logging) {
        build_and_send_message(L, PACKET_LOG_MESSAGE);
    }
    return 0;
}

/* Gets called by lua to send the inventory */
int inventory_send(lua_State* L) {
    if (RemoteApi::clientSubs.multiWorld) {
        build_and_send_message(L, PACKET_NEW_INVENTORY);
    }
    return 0;
}

/* Gets called by lua to send the indices of the already collected locations */
int indices_send(lua_State* L) {
    if (RemoteApi::clientSubs.multiWorld) {
        build_and_send_message(L, PACKET_COLLECTED_INDICES);
    }
    return 0;
}

/* Gets called by lua to send the received pickups from other players */
int recv_pickups_send(lua_State* L) {
    if (RemoteApi::clientSubs.multiWorld) {
        build_and_send_message(L, PACKET_RECEIVED_PICKUPS);
    }
    return 0;
}

/* Gets called by lua to send the current state (like ingame or main menu) */
int new_game_state_send(lua_State* L) {
    if (RemoteApi::clientSubs.multiWorld) {
        build_and_send_message(L, PACKET_GAME_STATE);
    }
    return 0;
}

/* Gets called by lua to get current connection state */
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
  {"Connected", is_connected},
  {NULL, NULL}
};

/* Hook asdf */

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

/* Handle version differences */
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
        offsets->CallFunctionWithArguments = 0x790; // wow that's early

        // Pickups
        offsets->OnCollectPickup = 0x9d0e28;
        offsets->PlayPickupSound = 0x9d16c4;
        offsets->ShowItemPickupMessage = 0xb5fd9c;

        // Audio
        offsets->PlaySoundWithCallback = 0xfd90d8;

        // String bank
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

        // Pickups
        offsets->OnCollectPickup = 0x9ce5c8;
        offsets->PlayPickupSound = 0x9cee64;
        offsets->ShowItemPickupMessage = 0xb5395c;

        // Audio
        offsets->PlaySoundWithCallback = 0xf975f8;

        // String bank
        offsets->StaticStringBank = 0x1cfc2d0;
        offsets->FindOrCreateStringInstance = 0x4063c;
    }
}

extern "C" void exl_main(void* x0, void* x1)
{
    functionOffsets offsets;
    /* Setup hooking enviroment. */
    exl::hook::Initialize();

    getVersionOffsets(&offsets);

    /* Install the hook at the provided function pointer. Function type is checked against the callback function. */
    /* Hook functions we care about */
    LuaRegisterGlobals::InstallAtOffset(offsets.luaRegisterGlobals);
    odr::debug::InstallHooks(&offsets);
    odr::lua::InstallFunctions(&offsets);
    odr::pickups::InstallHooks(&offsets);
    odr::romfs::InstallHooks(&offsets);

    /* Alternative install funcs: */
    /* InstallAtPtr takes an absolute address as a uintptr_t. */
    /* InstallAtOffset takes an offset into the main module. */

    /* Get the address of dread's crc64 function */
    exefs_lua_pcall = (int (*) (lua_State *L, int nargs, int nresults, int errfunc)) exl::util::modules::GetTargetOffset(offsets.lua_pcall);
}

extern "C" NORETURN void exl_exception_entry()
{
    /* TODO: exception handling */
    EXL_ABORT(0x420);
}