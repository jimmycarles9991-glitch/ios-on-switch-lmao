// SPDX-License-Identifier: MIT
#pragma once
#include "raw_pack.h"
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

// A pack of no states for a client device to open over the kernel model, in the test's temporary directory
inline std::string emptyPack()
{
    std::vector<uint8_t> file(0x400, 0);
    raw::PackHeader h = {};
    memcpy(h.magic, "GM20BPK1", 8);
    h.version = raw::kPackVersion;
    memcpy(file.data(), &h, sizeof h);
    const char *dir = getenv("TEST_TMPDIR");
    const std::string path = std::string(dir ? dir : "/tmp") + "/empty-pack.bin";
    std::ofstream(path, std::ios::binary)
        .write(reinterpret_cast<const char *>(file.data()), std::streamsize(file.size()));
    return path;
}
