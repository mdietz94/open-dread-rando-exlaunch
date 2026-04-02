#pragma once

#include <stddef.h>

/**
 * Holds offset pointers for Dread functions that we either hook or call manually
 */
typedef struct {
    /** Calculates the CRC64 hash of a string */
    ptrdiff_t crc64;

    /** Constructor for `CFilePathStrId` class used when the game loads files */
    ptrdiff_t CFilePathStrIdCtor;

    /** Function that registers Lua libraries (e.g. the `Game` library, etc.). Hooked to register our own. */
    ptrdiff_t luaRegisterGlobals;

    /** Dread's custom lua_pcall function, used to call raw Lua code in protected mode */
    ptrdiff_t lua_pcall;

    /** The lua_CFunction for `Game.LogWarn`, which is stubbed out in vanilla Dread. */
	ptrdiff_t LogWarn;

	/** Used by the game engine to call a Lua function (by its qualified name) from C++ using varargs arguments. */
	ptrdiff_t CallFunctionWithArguments;

	// Pickups
	/** Called when the player first collides with a Pickup item. */
    ptrdiff_t OnCollectPickup;

	/** Called by the CPickableItemComponent class to play the appropriate sound for collecting a pickup. */
	ptrdiff_t PlayPickupSound;

	/** Called by the CPickableItemComponent class to show a pop-up dialog with a message for a newly-collected pickup. */
    ptrdiff_t ShowItemPickupMessage;

	// Audio
	/** Plays a sound effect with specific flags and a callback function that is invoked when the sound finishes playing. */
	ptrdiff_t PlaySoundWithCallback;

	/** A large array of common CStrId in alpha order */
	ptrdiff_t StaticStringBank;

	/** Finds a StringInstance or creates one */
	ptrdiff_t FindOrCreateStringInstance;
} functionOffsets;

typedef unsigned long long crc64_t;

struct CRntString {
	char* str;
	int length;
	void* allocator;
	bool usesMainAllocator;
	crc64_t hash;
	bool isEmpty;
};

struct CStringInstance {
	void* stringPoolEntry;
	unsigned int uses;
	CRntString string;
	bool storeInPool;
	unsigned int unknown;
};

struct CStrId {
	CStringInstance *value;
};
