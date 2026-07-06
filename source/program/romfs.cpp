#include "romfs.hpp"

#include <nn.hpp>
#include "lib.hpp"
#include "cJSON.h"
#include "discovery.hpp"

typedef struct
{
    u64 crc;
    char const *replacement;
} stringList;

/* Function ptr to dread's crc function. */
u64 (*crc64)(char const *str, u64 size) = NULL;

stringList *g_stringList = NULL;
size_t g_stringListSize = 0;

CStrId* g_stringBank = NULL;

/** Create a string instance */
void (*create_string_instance) (CStrId* value, const char* str, uint len, u64 crc, bool storeInPool) = NULL;

/* Takes in a pointer to string and if found in the list, is replaced with the desired string. */
void replaceString(const char **str)
{
    /* Hash the string for quicker comparison. */
    u64 crc = crc64(*str, strlen(*str));

    /* Attempt to find matching hash in our list. */
    for(size_t i = 0; i < g_stringListSize; i++)
    {
        /* If the string matches, replace the string. */
        if(crc == g_stringList[i].crc)
            *str = g_stringList[i].replacement;
    }
}

/* Allocates a buffer and reads from the specified file.
 * If the path is not a valid file entry, immediately returns NULL.
 * Caller is expected to free any non-NULL return value.
 *
 * IMPORTANT (fs discipline): every nn::fs Result is checked and the file
 * handle is closed on EVERY path that reaches OpenFile success. Dread
 * manages the "rom" mount itself and nn::fs::Unmount ABORTS the whole
 * process (2002-6455, FileNotClosed) if any accessor on the mount is
 * still open — a single leaked handle here bricks the game at the next
 * unmount (observed at the game's exit-time Unmount("rom")). The
 * original upstream version also read `entryType` UNINITIALIZED when
 * GetEntryType failed, and called GetFileSize/ReadFile/CloseFile on an
 * UNINITIALIZED handle when OpenFile failed. */
void* odr::romfs::OpenAndReadFile(const char *path)
{
    nn::fs::DirectoryEntryType entryType = (nn::fs::DirectoryEntryType)-1;
    if (nn::fs::GetEntryType(&entryType, path) != 0) {
        return NULL;
    }
    if (entryType != nn::fs::DirectoryEntryType_File) {
        return NULL;
    }

    nn::fs::FileHandle fileHandle = {};
    if (nn::fs::OpenFile(&fileHandle, path, nn::fs::OpenMode_Read) != 0) {
        return NULL;
    }

    long int size = 0;
    if (nn::fs::GetFileSize(&size, fileHandle) != 0 || size < 0) {
        nn::fs::CloseFile(fileHandle);
        return NULL;
    }

    u8 *fileBuf = (u8 *)malloc(size + 1);
    if (fileBuf == NULL) {
        nn::fs::CloseFile(fileHandle);
        return NULL;
    }

    if (nn::fs::ReadFile(fileHandle, 0, fileBuf, size) != 0) {
        nn::fs::CloseFile(fileHandle);
        free(fileBuf);
        return NULL;
    }

    nn::fs::CloseFile(fileHandle);
    fileBuf[size] = '\0';
    return fileBuf;
}

/* Parses rom:/replacements.json and populates g_stringList.
 * If replacements.json does not exist, logs a warning and returns without modifying values. */
void populateStringReplacementList()
{
    /* Read contents of replacements.json in romfs. Allocates a heap buffer for the file contents and returns it. */
    char *fileBuf = (char *)odr::romfs::OpenAndReadFile("rom:/replacements.json");
    if (fileBuf == NULL) {
        const char* msg = "[LogWarn/0] rom:/replacements.json does not exist!";
        svcOutputDebugString(msg, strlen(msg));
        return;
    }

    /* Parse json from file buffer. */
    cJSON *json = cJSON_Parse(fileBuf);
    if(json == NULL) {
        free(fileBuf);
        return;
    }

    /* Get the "replacements" object within the json. */
    const cJSON *replacementList = cJSON_GetObjectItem(json, "replacements");

    /* Get number of elements in the array, for later use. */
    g_stringListSize = cJSON_GetArraySize(replacementList);

    /* Allocate space for the list of strings to replace. */
    g_stringList = (stringList *)malloc(g_stringListSize * sizeof(stringList));

    size_t i = 0;
    const cJSON *itemObject;

    /* Iterate over array contents and extract strings. */
    cJSON_ArrayForEach(itemObject, replacementList)
    {
        /* Extract the strings using the relevant method and add them to the list. */
        if(cJSON_IsString(itemObject))
        {
            char *fileStr = cJSON_GetStringValue(itemObject);
            char *replacementFileStr = (char *)malloc(strlen(fileStr) + strlen("rom:/") + 1);
            strcpy(replacementFileStr, "rom:/");
            replacementFileStr = strcat(replacementFileStr, fileStr);
            g_stringList[i].crc = crc64(fileStr, strlen(fileStr));
            g_stringList[i].replacement = replacementFileStr;
        }
        else if(cJSON_IsObject(itemObject))
        {
            char const *str = cJSON_GetItemName(itemObject);
            g_stringList[i].crc = crc64(str, strlen(str));
            g_stringList[i].replacement = cJSON_GetStringValue(itemObject->child);
        }
        i++;
    }

    /* Free the buffer allocated by OpenAndReadFile, since we're done with it. */
    free(fileBuf);
}

/* Sets the save slot names based on the string in "rom:/RDVHASH". 
 * The file must contain at most a 253-character string (called "hash"). 
 * If rom:/RDVHASH is not a valid file, writes a warning to console and returns without modifying strings.
 * If rom:/RDVHASH has a string that is too long, writes a warning to console and returns without modifying strings.
 *
 * The following strings are changed:
 *
 *  - "profile0" -> "hash_0"
 *  - "profile1" -> "hash_1"
 *  - "profile2" -> "hash_2" */
void setSeedSaveProfile()
{
    char *seedHash = (char*)odr::romfs::OpenAndReadFile("rom:/RDVHASH");
    if (seedHash == NULL) {
        const char* msg = "[LogWarn/0] rom:/RDVHASH does not exist!";
        svcOutputDebugString(msg, strlen(msg));
        return;
    }

    int len = strlen(seedHash);
    if (len > 253)
    {
        const char* msg = "[LogWarn/0] requested slot name in rom:/RDVHASH must be shorter than 254 characters!";
        svcOutputDebugString(msg, strlen(msg));
        return;
    }

    if (len > 0)
    {
        char slotName[256];
        u64 crc;
        len += 2;

        sprintf(slotName, "%s_0", seedHash);
        crc = crc64(slotName, len);
        create_string_instance(&g_stringBank[odr::romfs::STRINGBANK_PROFILE0], slotName, len, crc, true);

        slotName[len-1] = '1';
        crc = crc64(slotName, len);
        create_string_instance(&g_stringBank[odr::romfs::STRINGBANK_PROFILE0+1], slotName, len, crc, true);

        slotName[len-1] = '2';
        crc = crc64(slotName, len);
        create_string_instance(&g_stringBank[odr::romfs::STRINGBANK_PROFILE0+2], slotName, len, crc, true);
    }
    free(seedHash);
    return;
}

HOOK_DEFINE_TRAMPOLINE(ForceRomfs) {
    /* Define the callback for when the function is called. Don't forget to make it static and name it Callback. */
    static void Callback(void *CFilePathStrIdOut, const char *path, u8 flags) {

        /* Just in case the path is NULL, pass it down to the real implementation, since we don't support replacing NULL paths anyway. */
        if(path == NULL)
        {
            Orig(CFilePathStrIdOut, path, flags);
            return;
        }

        /* Replace string if we have it in our list before passing to the real implementation. */
        replaceString(&path);
        Orig(CFilePathStrIdOut, path, flags);
    }
};

/* Hook romfs mounting. Pass the arguments down to the real implementation so romfs is mounted as normal. */
/* Once romfs is mounted, we can read files from it in order to populate our string replacement list. */
HOOK_DEFINE_TRAMPOLINE(RomMounted) {
    static Result Callback(char const *path, void *romCache, unsigned long cacheSize) {
        Result res = Orig(path, romCache, cacheSize);
        populateStringReplacementList();
        setSeedSaveProfile();
        /* Read + cache rom:/ap_config.json HERE, on the game thread, in the
         * same just-mounted window the two reads above use. The discovery
         * worker consumes only the in-memory cache — it must NEVER touch
         * nn::fs itself: the game unmounts "rom" on its own schedule (at
         * least at process exit), and an open accessor from another thread
         * at that instant aborts the process (2002-6455 FileNotClosed). */
        dread::ap::CacheBridgeHostFromRomfs();
        return res;
    }
};

void odr::romfs::InstallHooks(functionOffsets* offsets) {
    RomMounted::InstallAtFuncPtr(nn::fs::MountRom);
    ForceRomfs::InstallAtOffset(offsets->CFilePathStrIdCtor);
    crc64 = (u64 (*)(char const *, u64))exl::util::modules::GetTargetOffset(offsets->crc64);
    create_string_instance = (void (*)(CStrId* value, const char* str, uint len, u64 crc, bool storeInPool)) exl::util::modules::GetTargetOffset(offsets->FindOrCreateStringInstance);
    g_stringBank = (CStrId*)exl::util::modules::GetTargetOffset(offsets->StaticStringBank);
}