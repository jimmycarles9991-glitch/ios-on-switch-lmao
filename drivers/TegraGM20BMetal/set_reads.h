// SPDX-License-Identifier: GPL-2.0-or-later
// Which of a state's set-0 entries a program reads from the set's memory (PackPipeline::memoryEntries), from its machine
// code: the words it reads at the constant buffer slot it binds set 0 at, and every entry where it reads the set through
// the root table's address of it (gpu.cpp writes set 0's address at root 0x38) or at an offset it computes. The
// opcode patterns of instructions with a constant buffer operand are tools/gpu/maxwell/opcodes.txt's (yuzu's maxwell.inc,
// GPL-2.0-or-later), bits 63-48; a pattern another instruction also matches only marks more entries.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "pack.h"
#include "set_entries.h"

namespace gm20b {

namespace setreads {

struct Pattern {
    uint16_t mask, value;
};

// An operand c[bank][offset]: bank in bits 34-38, offset / 4 in bits 20-33
constexpr Pattern kCbufOperand[] = {
    {0xfff8u, 0x4c00u}, {0xfff8u, 0x53f0u}, {0xfff8u, 0x4bf0u}, {0xfff8u, 0x4c70u}, {0xfff0u, 0x5370u},
    {0xfff0u, 0x4b70u}, {0xfff8u, 0x4c50u}, {0xfff8u, 0x4c80u}, {0xff80u, 0x4900u}, {0xfff0u, 0x4b80u},
    {0xfff8u, 0x4ca8u}, {0xfff8u, 0x4cb0u}, {0xfff8u, 0x4c58u}, {0xfff8u, 0x4c88u}, {0xfff0u, 0x53a0u},
    {0xfff0u, 0x4ba0u}, {0xff80u, 0x5180u}, {0xff80u, 0x4980u}, {0xfff8u, 0x4c30u}, {0xfff8u, 0x4c60u},
    {0xfff8u, 0x4c68u}, {0xff00u, 0x4800u}, {0xfff0u, 0x4bb0u}, {0xfe80u, 0x7a80u}, {0xf880u, 0x6080u},
    {0xf880u, 0x7080u}, {0xfe80u, 0x7880u}, {0xfe80u, 0x7c80u}, {0xfe80u, 0x7e80u}, {0xfff8u, 0x4cb8u},
    {0xfff8u, 0x4ce0u}, {0xfff8u, 0x4c10u}, {0xfff0u, 0x4cc0u}, {0xfff0u, 0x5340u}, {0xfff0u, 0x4b40u},
    {0xff80u, 0x5200u}, {0xff80u, 0x4a00u}, {0xff80u, 0x5280u}, {0xff80u, 0x4a80u}, {0xfff8u, 0x4c20u},
    {0xfff8u, 0x4c38u}, {0xfff8u, 0x4c18u}, {0xfff0u, 0x4b50u}, {0xfff0u, 0x4b60u}, {0xfc00u, 0x1800u},
    {0xfff0u, 0x4bd0u}, {0xfff8u, 0x4c40u}, {0xfe00u, 0x0200u}, {0xfff8u, 0x4c98u}, {0xfff8u, 0xebe0u},
    {0xfff8u, 0x4ce8u}, {0xfff8u, 0x4c08u}, {0xfff0u, 0x53c0u}, {0xfff0u, 0x4bc0u}, {0xfff8u, 0x4cf0u},
    {0xfff8u, 0x4c90u}, {0xfff8u, 0x4ca0u}, {0xfff8u, 0x4c48u}, {0xfff8u, 0x4c28u}, {0xff80u, 0x5100u},
    {0xfe00u, 0x4e00u},
};

// LDC: bank in bits 36-40, its address register Ra in bits 8-15 (RZ: 255) plus the immediate in bits 20-35, the load's
// size in bits 48-50, an indexed mode in bits 44-45
constexpr Pattern kLdc = {0xfff8u, 0xef90u};

constexpr uint32_t kRootSetAddress = 0x38, kRootSetAddressEnd = 0x40, kRegisterZero = 255;

inline uint32_t field(uint64_t word, unsigned at, unsigned bits)
{
    return uint32_t(word >> at) & ((1u << bits) - 1);
}

inline bool matches(uint64_t word, Pattern p)
{
    return (uint16_t(word >> 48) & p.mask) == p.value;
}

inline uint32_t ldcBytes(uint32_t size)
{
    constexpr uint32_t kBytes[8] = {1, 1, 2, 2, 4, 8, 16, 16};
    return kBytes[size & 7];
}

inline uint32_t entryBytes(const PackEntry &e)
{
    return e.kind == EntryUbo || e.kind == EntryColorTable || e.kind == EntrySpillTable ? 16 : 4;
}

}  // namespace setreads

// The words of a mask of a set's `count` entries: entry k is bit k % 32 of word k / 32
constexpr uint32_t entryMaskWords(uint32_t count)
{
    return count > 32 ? (count + 31) / 32 : 1;
}

// The entries of `entries` program code `code` reads from set 0's memory, ORed into `read` (entryMaskWords of them),
// `setSlot` the constant buffer slot it binds set 0 at (0: none)
inline void setMemoryReads(const uint8_t *code, size_t bytes, uint32_t setSlot, const SetEntries &entries,
                           uint32_t *read)
{
    using namespace setreads;
    const uint32_t count = entries.size();
    bool all = false;
    const auto readAll = [&] {
        for (uint32_t k = 0; k < count; k++)
            read[k / 32] |= 1u << k % 32;
        all = true;
    };
    const auto reads = [&](uint32_t bank, uint32_t offset, uint32_t width, bool computed) {
        if (bank == 0 && (computed || (offset < kRootSetAddressEnd && offset + width > kRootSetAddress)))
            return readAll();
        if (!setSlot || bank != setSlot)
            return;
        if (computed)
            return readAll();
        for (uint32_t k = 0; k < count; k++)
            if (offset < entries[k].offset + entryBytes(entries[k]) && offset + width > entries[k].offset)
                read[k / 32] |= 1u << k % 32;
    };
    // four 64-bit words a group, the first the group's scheduling word
    for (size_t at = 0; at + 8 <= bytes && !all; at += 8) {
        if (at % 32 == 0)
            continue;
        uint64_t word;
        memcpy(&word, code + at, sizeof word);
        if (matches(word, kLdc)) {
            const bool computed = field(word, 8, 8) != kRegisterZero || field(word, 44, 2);
            reads(field(word, 36, 5), field(word, 20, 16), ldcBytes(field(word, 48, 3)), computed);
            continue;
        }
        for (const Pattern &p : kCbufOperand)
            if (matches(word, p)) {
                reads(field(word, 34, 5), field(word, 20, 14) * 4, 4, false);
                break;
            }
    }
}

// The entries of `entries` a program reads from set 0's memory, ORed into `read` (entryMaskWords of them): its constant
// buffer types `cbufType[0..cbufs)` name the slot it binds set 0 at; its bound texture instructions read their handles at
// slot `boundTextures - 1` (0: none); `code` is its machine code, after its shader program header
inline void programSetReads(const uint8_t *code, size_t bytes, const uint8_t *cbufType, uint32_t cbufs,
                            uint8_t boundTextures, const SetEntries &entries, uint32_t *read)
{
    uint32_t setSlot = 0;
    for (uint32_t c = 1; c < cbufs; c++)
        if (cbufType[c] == CbufSet)
            setSlot = c;
    setMemoryReads(code, bytes, setSlot, entries, read);
    if (boundTextures && boundTextures <= cbufs && cbufType[boundTextures - 1] == CbufSet)
        for (uint32_t k = 0; k < entries.size(); k++)
            if (entries[k].kind != EntryUbo && entries[k].kind != EntryColorTable && entries[k].kind != EntrySpillTable)
                read[k / 32] |= 1u << k % 32;
}

// The same of a program of the heap: `header` its shader program header, at its slot's 0x30 (PackProgram)
inline void placedSetReads(const uint8_t *header, const PackProgram &program, const SetEntries &entries, uint32_t *read)
{
    constexpr uint32_t kCodeInSlot = 0x80, kHeaderBytes = 0x50;
    const uint32_t end = program.dataBytes ? program.dataOffset : program.bytes;
    programSetReads(header + kHeaderBytes, end > kCodeInSlot ? end - kCodeInSlot : 0, program.cbufType, program.cbufs,
                    program.boundTextures, entries, read);
}

// placedSetReads of the first 32 entries alone: Pipeline::memoryEntries, which only a draw of 32 or fewer reads
inline uint32_t placedSetReads(const uint8_t *header, const PackProgram &program, const SetEntries &entries)
{
    uint32_t read[entryMaskWords(kSetEntriesMax)] = {};
    placedSetReads(header, program, entries, read);
    return read[0];
}

}  // namespace gm20b
