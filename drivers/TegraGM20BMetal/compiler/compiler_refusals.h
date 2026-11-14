// SPDX-License-Identifier: MIT
// The refusals a compiler build has made itself, beside the store (compiler_service.mm). The store is named by the stack
// key and the pack's rules, which the compiler's own code is not part of: a refusal is this build's once this build made
// it (at its compile), and one another build stored is made again once at the background class (warm). Each line of
// <store directory>/refusals-<build> is one of this build's names, 64 hex digits. Compiles on the pool's workers and the
// warm-up record names at once: a refusal is rare, so one mutex.
#pragma once
#include <dlfcn.h>
#include <mach-o/loader.h>
#include <array>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <set>
#include <string>

namespace switch_compiler {

// The LC_UUID of the image holding `address`, in hex: its build. "" where it has none
inline std::string imageBuild(const void *address)
{
    Dl_info info = {};
    if (!dladdr(address, &info) || !info.dli_fbase)
        return std::string();
    const auto *header = static_cast<const mach_header_64 *>(info.dli_fbase);
    const auto *command = reinterpret_cast<const load_command *>(header + 1);
    for (uint32_t i = 0; i < header->ncmds; ++i) {
        if (command->cmd == LC_UUID) {
            std::string hex;
            char byte[3];
            for (uint8_t b : reinterpret_cast<const uuid_command *>(command)->uuid) {
                snprintf(byte, sizeof byte, "%02x", b);
                hex += byte;
            }
            return hex;
        }
        command = reinterpret_cast<const load_command *>(reinterpret_cast<const uint8_t *>(command) + command->cmdsize);
    }
    return std::string();
}

class ConfirmedRefusals {
public:
    using Name = std::array<uint8_t, 32>;  // raw::Digest: a store entry's name

    // `build`: the compiler image's UUID in hex
    ConfirmedRefusals(const std::string &storeDirectory, const std::string &build)
        : path_(storeDirectory + "/refusals-" + build)
    {
        if (FILE *f = fopen(path_.c_str(), "r")) {
            char line[80];
            while (fgets(line, sizeof line, f)) {
                Name name{};
                if (parse(line, name))
                    names_.insert(name);
            }
            fclose(f);
        }
    }

    bool confirmed(const Name &name) const
    {
        std::lock_guard<std::mutex> l(lock_);
        return names_.count(name) != 0;
    }

    void confirm(const Name &name)
    {
        std::lock_guard<std::mutex> l(lock_);
        if (!names_.insert(name).second)
            return;
        if (FILE *f = fopen(path_.c_str(), "a")) {
            for (uint8_t byte : name)
                fprintf(f, "%02x", byte);
            fputc('\n', f);
            fclose(f);
        }
    }

private:
    std::string path_;
    mutable std::mutex lock_;
    std::set<Name> names_;

    static bool parse(const char *line, Name &name)
    {
        for (size_t i = 0; i < name.size(); ++i) {
            unsigned byte = 0;
            if (sscanf(line + 2 * i, "%2x", &byte) != 1)
                return false;
            name[i] = uint8_t(byte);
        }
        return true;
    }
};

}  // namespace switch_compiler
