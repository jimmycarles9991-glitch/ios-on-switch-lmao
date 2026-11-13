// SPDX-License-Identifier: MIT
// The MAXWELL_B methods the raw path owns: it sets them itself per pass, per draw, or once at init with its own addresses, so a
// state's record never carries them. One rule for the pack generator (pack/mkpack.py reads the ranges below) and the runtime's
// capture of a state (raw_capture.cpp).
#pragma once
#include <cstdint>

namespace raw {

struct MethodRange {
    uint32_t begin, end;
};

// clang-format off
constexpr MethodRange kOwnedMethods[] = {
    {0x0000, 0x0200},
    {0x0800, 0x0A00},
    {0x0A00, 0x0A18},
    {0x0C00, 0x0C08},
    {0x0D6C, 0x0D90},
    {0x0E00, 0x0E0C},
    {0x0FE0, 0x0FFC},
    {0x1970, 0x1974},
    {0x1644, 0x1648},
    {0x1118, 0x111C},
    {0x1434, 0x143C},
    {0x10F8, 0x10FC},
    {0x121C, 0x1220},
    {0x1538, 0x153C},
    {0x19E0, 0x1A00},
    {0x1F00, 0x1F80},
    {0x1608, 0x1610},
    {0x1614, 0x161C},
    {0x1574, 0x1580},
    {0x155C, 0x1568},
    {0x0F84, 0x0F8C},
    {0x1B00, 0x1B10},
    {0x2380, 0x2600},
    {0x3400, 0x4000},
    // the depth/stencil target and its clears (the pass's), and the depth/stencil state (the encoder's: raw_zeta.h)
    {0x0D90, 0x0D94},
    {0x0DA0, 0x0DA4},
    {0x0F54, 0x0F60},
    {0x111C, 0x1120},
    {0x1208, 0x120C},
    {0x1228, 0x1234},
    {0x12CC, 0x12D0},
    {0x12E8, 0x12EC},
    {0x130C, 0x1310},
    {0x1380, 0x13A0},
    {0x1598, 0x15A8},
    {0x179C, 0x17A0},
    {0x19CC, 0x19D0},
    // SET_PATCH: no raw draw is of patches, and the generator's probe left it as its command buffers' history had it
    {0x0DCC, 0x0DD0},
    // shader local memory (raw_slm.h): the area the runtime sizes for its programs, never the probe process's address
    {0x0790, 0x07A4},
    // cache invalidates and flushes: actions, which the raw path issues itself (a submission's prologue, a barrier), though NVK
    // leaves the last one's data behind as if it were state
    {0x021C, 0x0220},
    {0x0DA4, 0x0DA8},
    {0x0F74, 0x0F78},
    {0x1144, 0x1148},
    {0x1288, 0x128C},
    {0x1338, 0x133C},
    {0x1424, 0x142C},
    // the sample layout, SET_ANTI_ALIAS and SET_ANTI_ALIAS_SAMPLE_POSITIONS: the pass's (raw_samples.h), as NVK sets them from its
    // attachments' sample count
    {0x11E0, 0x11F0},
    {0x15D0, 0x15D4},
    // a program's register count and the slot its bound textures read handles from: its record's (PackProgram), set with the
    // program, so a state's methods hold no program's
    {0x204C, 0x2050},
    {0x214C, 0x2150},
    {0x2608, 0x260C},
    // the iterated blend (raw_fetch_free.h): SET_ITERATED_BLEND and its pass count, the program's loads, its constants, the
    // per-target blend switch and the common blend block it takes alpha from, a rung-3 fetch-free form's
    {0x0DD0, 0x0DD8},
    {0x0DF0, 0x0DF8},
    {0x1480, 0x1500},
    {0x12E4, 0x12E8},
    {0x133C, 0x1354},
    {0x1358, 0x135C},
};
// clang-format on

// The vertex streams' locations (SET_VERTEX_STREAM_A_LOCATION_A/B, 0x1c04 + 0x10 j and 0x1c08 + 0x10 j)
constexpr MethodRange kVertexStreams = {0x1C00, 0x1D00};

constexpr bool ownedMethod(uint32_t m)
{
    if (m >= kVertexStreams.begin && m < kVertexStreams.end && ((m & 0xF) == 4 || (m & 0xF) == 8))
        return true;
    for (const MethodRange &r : kOwnedMethods)
        if (m >= r.begin && m < r.end)
            return true;
    return false;
}

}  // namespace raw
