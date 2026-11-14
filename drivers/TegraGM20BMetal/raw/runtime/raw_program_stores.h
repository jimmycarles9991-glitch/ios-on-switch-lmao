// SPDX-License-Identifier: GPL-2.0-or-later
// Whether a program can write memory a buffer binding names: any global or generic store, atomic, reduction or surface
// write in its machine code. The opcode patterns are tools/gpu/maxwell/opcodes.txt's (yuzu's maxwell.inc,
// GPL-2.0-or-later), bits 63-48; a pattern another instruction also matches only answers yes more often. Local and shared
// stores and atomics are left out: they name no buffer.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "raw_pack.h"
#include "raw_set_reads.h"

namespace raw {

namespace stores {

constexpr setreads::Pattern kMemoryWrites[] = {
    {0xe000u, 0xa000u},  // ST
    {0xfff8u, 0xeed8u},  // STG
    {0xff00u, 0xed00u},  // ATOM
    {0xfff0u, 0xeef0u},  // ATOM (cas)
    {0xfff8u, 0xebf8u},  // RED
    {0xffe0u, 0xeb20u},  // SUST
    {0xffe0u, 0xeb40u},  // SURED
    {0xff80u, 0xea00u},  // SUATOM
};

}  // namespace stores

// `code` of `bytes`, its program after its shader program header
inline bool programWritesMemory(const uint8_t *code, size_t bytes)
{
    // four 64-bit words a group, the first the group's scheduling word
    for (size_t at = 0; at + 8 <= bytes; at += 8) {
        if (at % 32 == 0)
            continue;
        uint64_t word;
        memcpy(&word, code + at, sizeof word);
        for (const setreads::Pattern &p : stores::kMemoryWrites)
            if (setreads::matches(word, p))
                return true;
    }
    return false;
}

// The same of a program of the heap: `header` its shader program header, at its slot's 0x30 (PackProgram)
inline bool placedWritesMemory(const uint8_t *header, const PackProgram &program)
{
    constexpr uint32_t kCodeInSlot = 0x80, kHeaderBytes = 0x50;
    const uint32_t end = program.dataBytes ? program.dataOffset : program.bytes;
    return !header || programWritesMemory(header + kHeaderBytes, end > kCodeInSlot ? end - kCodeInSlot : 0);
}

}  // namespace raw
