// SPDX-License-Identifier: MIT
// Metal's counter sampling as the identity's AGX driver answers it (AGXMetalA10 on 21H461, AGXMetalA13 on 24B119): one
// counter set, "timestamp", of one counter, "GPUTimestamp", sampled at stage boundaries alone, a sample 8 bytes.
//
// A sample here is a four-word release the GPU writes where the boundary's work has passed: payload, 0, then PTIMER (NVK's
// timestamp query report). The 3D class's SET_REPORT_SEMAPHORE at the stage's pipeline location, the compute class's after
// its encoder's waits for idle, the copy engine's semaphore of an empty launch: none waits for idle itself. So a Metal sample
// takes a record of 16 bytes, and an unwritten record (payload 0) resolves to 0.
//
// A resolved sample is nanoseconds of mach_absolute_time, the domain of the real driver's samples (its sampleTimestamps:
// answers one value for both clocks). PTIMER and the CPU's counter both count the 19.2 MHz oscillator (PTIMER 32 a tick,
// raw_stamps.h; timebase-frequency 19200000), so the rate is exact and one reading of both, an anchor, places every unit
// of one power-up of the GPU. The kernel publishes each power-up's anchor (TegraGM20BClockABI.h): a sample takes the
// newest one at or below its PTIMER value.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include "raw_push.h"
#include "raw_stamps.h"

namespace raw::counters {

constexpr uint32_t kSampleBytes = 8;
constexpr uint32_t kRecordBytes = 16;
constexpr uint64_t kMaxBufferBytes = 0x8000;
constexpr uint32_t kPayload = 1;
constexpr uint64_t kErrorValue = ~0ull;
constexpr uint32_t kTimestampWords = 8;

inline constexpr std::string_view kSetName = "timestamp";
inline constexpr std::string_view kSetDescription = "A counter set containing only a GPU timestamp.";
inline constexpr std::string_view kCounterName = "GPUTimestamp";
inline constexpr std::string_view kCounterDescription = "A timestamp in nanoseconds on the GPU.";

struct Record {
    uint32_t payload, zero;
    uint64_t units;
};

static_assert(sizeof(Record) == kRecordBytes, "the four-word report structure");

// -[AGXMTLCounterSampleBuffer initWithDevice:descriptor:error:], then initWithDevice:length:label:options:error:, in order:
// each a MTLCounterSampleBufferErrorInvalid
enum class Check : uint8_t {
    Ok,
    SetSize,
    SetName,
    CounterName,
    Length,
    Alignment
};

inline Check checkLength(uint64_t bytes)
{
    if (bytes - 1 >= kMaxBufferBytes)
        return Check::Length;
    return bytes % kSampleBytes ? Check::Alignment : Check::Ok;
}

// The descriptor's counter set of `counters` counters, named `set`, its first counter `counter`, for `sampleCount` samples
// (the length the driver takes, sampleCount << 3, wraps as its does)
inline Check checkDescriptor(uint64_t counters, std::string_view set, std::string_view counter, uint64_t sampleCount)
{
    if (counters != 1)
        return Check::SetSize;
    if (set != kSetName)
        return Check::SetName;
    if (counter != kCounterName)
        return Check::CounterName;
    return checkLength(sampleCount << 3);
}

// The driver's NSError description of a check: `found` the name it found, `bytes` the length it took
inline std::string checkMessage(Check c, std::string_view found, uint64_t bytes)
{
    const std::string length = "Invalid sample buffer length: " + std::to_string(bytes) + " B. ";
    switch (c) {
    case Check::Ok:
        return {};
    case Check::SetSize:
        return "Invalid counter set size. Expecting 1 counter";
    case Check::SetName:
        return "Invalid counter set name. Expecting: " + std::string(kSetName) + ". Found: " + std::string(found);
    case Check::CounterName:
        return "Invalid counter name. Expecting: " + std::string(kCounterName) + ". Found: " + std::string(found);
    case Check::Length:
        return length + "Expected range: " + std::to_string(kSampleBytes) + " -> " + std::to_string(kMaxBufferBytes);
    case Check::Alignment:
        return length + "Length must be aligned to " + std::to_string(kSampleBytes) + " B";
    }
    return {};
}

// resolveCounterRange:'s range: nil where it is empty or past the samples
inline bool resolvable(uint64_t location, uint64_t length, uint64_t count)
{
    return length && location <= count && length <= count - location;
}

// A blit resolve's range: one past the samples is the driver's out-of-bounds line, an empty one copies nothing
inline bool withinSamples(uint64_t location, uint64_t length, uint64_t count)
{
    return location <= count && length <= count - location;
}

// An attachment's sample index: MTLCounterDontSample (the driver keeps it as a 32-bit word) samples nothing, nor does one
// past the buffer's samples ("Invalid sample index")
enum class Index : uint8_t {
    None,
    Sample,
    Invalid
};

inline Index sampleIndex(uint64_t index, uint64_t count)
{
    if (uint32_t(index) == UINT32_MAX)
        return Index::None;
    return index < count ? Index::Sample : Index::Invalid;
}

struct Anchor {
    uint64_t units = 0, ns = 0;
    bool valid = false;
};

// A span of PTIMER units in nanoseconds, rounded toward minus infinity
inline int64_t unitsToNs(int64_t units)
{
    int64_t q = units / int64_t(kPtimerNsDen), r = units % int64_t(kPtimerNsDen);
    if (r < 0) {
        r += int64_t(kPtimerNsDen);
        q -= 1;
    }
    return q * int64_t(kPtimerNsNum) + r * int64_t(kPtimerNsNum) / int64_t(kPtimerNsDen);
}

inline uint64_t nanoseconds(const Anchor &a, uint64_t units)
{
    return a.ns + uint64_t(unitsToNs(int64_t(units - a.units)));
}

// Where a sample is taken, which engine writes it
enum class Point : uint8_t {
    VertexStart,
    VertexEnd,
    FragmentStart,
    FragmentEnd,
    ComputeStart,
    ComputeEnd,
    BlitStart,
    BlitEnd
};

inline bool isStart(Point p)
{
    return p == Point::VertexStart || p == Point::FragmentStart || p == Point::ComputeStart || p == Point::BlitStart;
}

// The 3D class's report: where every command before it has passed the stage's pipeline location (NVK's
// vk_stage_flags_to_nv9097_pipeline_location: vertex input, pre-rasterization shaders, fragment shader, all graphics)
inline uint32_t reportWord3d(Point p)
{
    const uint32_t release =
        uint32_t(NVDEF(NVB197, SET_REPORT_SEMAPHORE_D, OPERATION, RELEASE) |
                 NVDEF(NVB197, SET_REPORT_SEMAPHORE_D, RELEASE, AFTER_ALL_PRECEEDING_WRITES_COMPLETE) |
                 NVDEF(NVB197, SET_REPORT_SEMAPHORE_D, STRUCTURE_SIZE, FOUR_WORDS));
    switch (p) {
    case Point::VertexStart:
        return release | uint32_t(NVDEF(NVB197, SET_REPORT_SEMAPHORE_D, PIPELINE_LOCATION, DATA_ASSEMBLER));
    case Point::VertexEnd:
        return release | uint32_t(NVDEF(NVB197, SET_REPORT_SEMAPHORE_D, PIPELINE_LOCATION, GEOMETRY_SHADER));
    case Point::FragmentStart:
        return release | uint32_t(NVDEF(NVB197, SET_REPORT_SEMAPHORE_D, PIPELINE_LOCATION, PIXEL_SHADER));
    default:
        return release | uint32_t(NVDEF(NVB197, SET_REPORT_SEMAPHORE_D, PIPELINE_LOCATION, ALL));
    }
}

inline void emit3d(Push &p, uint64_t va, Point point)
{
    p.m(NVB197_FLUSH_PENDING_WRITES, 0);
    p.inc(NVB197_SET_REPORT_SEMAPHORE_A,
          {uint32_t(NVVAL(NVB197, SET_REPORT_SEMAPHORE_A, OFFSET_UPPER, uint32_t(va >> 32))), uint32_t(va), kPayload,
           reportWord3d(point)});
}

// The compute class's report has no pipeline location: it goes where the encoder already waited for idle
inline void emitCompute(Push &p, uint64_t va)
{
    p.inc(NVB1C0_SET_REPORT_SEMAPHORE_A,
          {uint32_t(NVVAL(NVB1C0, SET_REPORT_SEMAPHORE_A, OFFSET_UPPER, uint32_t(va >> 32))), uint32_t(va), kPayload,
           uint32_t(NVDEF(NVB1C0, SET_REPORT_SEMAPHORE_D, OPERATION, RELEASE) |
                    NVDEF(NVB1C0, SET_REPORT_SEMAPHORE_D, STRUCTURE_SIZE, FOUR_WORDS))},
          SUBC_COMPUTE);
}

// The copy engine's semaphore of a launch that moves nothing; `afterCopies`: NON_PIPELINED, after the copies before it
// landed (NVK's vkCmdWriteTimestamp2 on the copy subchannel)
inline void emitCopy(Push &p, uint64_t va, bool afterCopies)
{
    p.inc(NVB0B5_SET_SEMAPHORE_A,
          {uint32_t(NVVAL(NVB0B5, SET_SEMAPHORE_A, UPPER, uint32_t(va >> 32))), uint32_t(va), kPayload}, SUBC_COPY);
    if (afterCopies)
        p.inc(NVB0B5_LINE_LENGTH_IN, {0, 0}, SUBC_COPY);
    p.m(NVB0B5_LAUNCH_DMA,
        uint32_t((afterCopies ? NVDEF(NVB0B5, LAUNCH_DMA, DATA_TRANSFER_TYPE, NON_PIPELINED)
                              : NVDEF(NVB0B5, LAUNCH_DMA, DATA_TRANSFER_TYPE, NONE)) |
                 NVDEF(NVB0B5, LAUNCH_DMA, SEMAPHORE_TYPE, RELEASE_FOUR_WORD_SEMAPHORE) |
                 NVDEF(NVB0B5, LAUNCH_DMA, FLUSH_ENABLE, TRUE)),
        SUBC_COPY);
}

// A sample to take: its record's address and its boundary
struct Stamp {
    uint64_t va;
    Point point;
};

inline void emit(Push &p, const Stamp &s)
{
    switch (s.point) {
    case Point::ComputeStart:
    case Point::ComputeEnd:
        emitCompute(p, s.va);
        break;
    case Point::BlitStart:
    case Point::BlitEnd:
        emitCopy(p, s.va, s.point == Point::BlitEnd);
        break;
    default:
        emit3d(p, s.va, s.point);
        break;
    }
}

}  // namespace raw::counters
