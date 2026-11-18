// The app's own pixel program for the known-answer experiment, independent of the host's encoder: the four words of its
// shader data, read from constant buffer 1, written to colour 0. Encoded as NAK's sm50.rs encodes MOV (cbuf), EXIT and
// NOP; tools/benchloop/known_test.py runs these words through the host's Maxwell interpreter.
#pragma once
#include <cstdint>

namespace bench {

// A scheduling word's 21-bit control: the delay, no barriers, no reuse
constexpr uint64_t control(uint32_t delay)
{
    return delay | 7u << 5 | 7u << 8;
}

constexpr uint64_t schedule(uint32_t d0, uint32_t d1, uint32_t d2)
{
    return control(d0) | control(d1) << 21 | control(d2) << 42;
}

constexpr uint64_t movCbuf(uint32_t dst, uint32_t bank, uint32_t offset)
{
    return 0x4c98ull << 48 | 0xfull << 39 | uint64_t(bank) << 34 | uint64_t(offset / 4) << 20 | 7ull << 16 | dst;
}

constexpr uint64_t kExit = 0xe300ull << 48 | 7ull << 16 | 0xf;
constexpr uint64_t kNop = 0x50b0ull << 48 | 7ull << 16 | 0xfull << 8;

constexpr uint64_t kKnownCode[] = {
    schedule(1, 1, 1),
    movCbuf(0, 1, 0),
    movCbuf(1, 1, 4),
    movCbuf(2, 1, 8),
    schedule(15, 15, 1),
    movCbuf(3, 1, 12),
    kExit,
    kNop,
};

constexpr uint64_t kKnownKernel[] = {schedule(15, 1, 1), kExit, kNop, kNop};

// SPH v3 type 2: a pixel program (shader type 5), MRT enable as NAK sets it, SASS version 1, colour 0's RGBA written.
// Position W is read (bit 191) as NAK's from_nir always reads it: a pixel header without it traps the channel.
constexpr uint32_t kPositionW = 1u << 31;
constexpr uint32_t kKnownSph[20] = {
    2 | 3 << 5 | 5 << 10 | 1 << 14 | 1 << 17, 0, 0, 0, 0, kPositionW, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xf, 0};

// 1.0, 0.5, 0.25 and 2.0 as float bits: an rgba32float target stores them bit for bit
constexpr uint32_t kKnownData[4] = {0x3f800000, 0x3f000000, 0x3e800000, 0x40000000};
constexpr uint32_t kKnownGprs = 4;

}  // namespace bench
