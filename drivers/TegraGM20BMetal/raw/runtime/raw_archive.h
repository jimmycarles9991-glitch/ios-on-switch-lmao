// SPDX-License-Identifier: MIT
// iOS 27's frameworks ship their binary archives as thin Mach-O GPU binaries (RenderBox's archive.metallib, IconRendering's
// binary.metallib: MH_MAGIC_64, cputype CPU_TYPE_APPLE_GPU | CPU_ARCH_ABI64, an __AIR_DATA segment). Metal opens a file of
// binaries for another GPU as an archive holding nothing for this one, and m2v takes such a binary as a slice of a universal
// file (metal2vulkan-runtime binary_archive/container.rs APPLE_GPU_CPU_TYPE) but not on its own.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace raw {

constexpr uint32_t kMachMagic64 = 0xfeedfacf, kAppleGpuCpuType = 0x01000013;

// `head`: the file's first bytes
inline bool appleGpuBinary(const uint8_t *head, size_t bytes)
{
    if (bytes < 8)
        return false;
    uint32_t magic, cpu;
    memcpy(&magic, head, 4);
    memcpy(&cpu, head + 4, 4);
    return magic == kMachMagic64 && cpu == kAppleGpuCpuType;
}

// A file m2v's binary archive wrote ("M2VBARCH", metal2vulkan-runtime binary_archive/container.rs): it holds nothing of this
// device's (raw_archive_keys.h archiveFile)
inline bool m2vArchive(const uint8_t *head, size_t bytes)
{
    return bytes >= 8 && !memcmp(head, "M2VBARCH", 8);
}

// A file's path as an archive inventory names it: on the OS volume, whichever cryptex mount or /var link it was opened through
inline std::string inventoryPath(std::string path)
{
    for (const char *mount : {"/System/Cryptexes/OS/", "/private/preboot/Cryptexes/OS/"})
        if (path.rfind(mount, 0) == 0)
            path.erase(0, strlen(mount) - 1);
    if (path.rfind("/var/", 0) == 0)
        path.insert(0, "/private");
    return path;
}

}  // namespace raw
