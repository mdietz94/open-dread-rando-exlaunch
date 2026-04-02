#pragma once

#include "common.hpp"
#include "dread_types.hpp"

namespace odr::romfs {
    void InstallHooks(functionOffsets* offsets);
    void* OpenAndReadFile(const char* path);

    /** The offset of "profile0" in the string bank. "profile1" and "profile2" immediately proceed. */
    const int STRINGBANK_PROFILE0 = 917;
}