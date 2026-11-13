// SPDX-License-Identifier: MIT
// The size a constant buffer is bound at, by SET_CONSTANT_BUFFER_SELECTOR_A for a draw or a QMD's CONSTANT_BUFFER_SIZE for
// a launch: whole 256-byte units, at most NVK_MAX_CBUF_SIZE, as NVK binds every constant buffer before Turing (nvk_cmd_draw.c
// align(size, nvk_min_cbuf_alignment), 256 below TURING_A). On GM20B a buffer bound off a 256-byte multiple is not read
// right through c[bank][offset] operands even below its size: the split frontier bound at its 64 bytes left the glass
// shape's per-draw offsets zero (T-v4c: bound at 64 bytes black, at 256 clean, the twin reading only
// c[2][0x0..0x3c] of the 64 bytes its prologue wrote). A binding's memory is allocated in the same units, so the rounding
// never reaches past it.
#pragma once
#include <algorithm>
#include <cstdint>

namespace raw {

constexpr uint32_t kCbufUnit = 256, kCbufMaxBytes = 0x10000;

constexpr uint32_t cbufBindBytes(uint32_t bytes)
{
    return std::min((bytes + kCbufUnit - 1) & ~(kCbufUnit - 1), kCbufMaxBytes);
}

// `bytes` itself where `exact`: HW Test's bench binding a program's data off the unit (Device::probeExactCbufs) to measure
// what the chip reads of it
constexpr uint32_t cbufBindBytes(uint32_t bytes, bool exact)
{
    return exact ? bytes : cbufBindBytes(bytes);
}

}  // namespace raw
