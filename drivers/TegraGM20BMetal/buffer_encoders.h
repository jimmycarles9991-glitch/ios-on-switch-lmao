// SPDX-License-Identifier: MIT
// A function's encoders at every buffer index it declares (newArgumentEncoderWithBufferIndex:), which only its AIR
// states: the shader compiler answers them from the translator (RequestKind::Reflection of one stage tagged "b",
// compile_request.h bufferEncoders; bridge nmb_offline_function_buffer_encoders), keyed by the function's AIR, its
// constants and whether it was made with constant values (made without, a buffer a constant gates is absent). The blob
// is a u32 count of buffers, then each one's u32 index, a flag byte (1 an argument buffer, 2 refused) and either its
// refusal, NUL-terminated, or a u32 count of layouts (the buffer's, then those its pointers point to), each a u64
// encodedLength, u64 alignment, u32 run count and its runs (argument_layout.h ArgumentRun).
#pragma once
#include <cstdio>
#include <string>
#include <vector>
#include "argument_layout.h"
#include "function_tags.h"
#include "metallib.h"

namespace gm20b {

struct FlatArgumentLayout {
    uint64_t encodedLength = 0, alignment = 1;
    std::vector<ArgumentRun> runs;
};

struct BufferEncoder {
    uint32_t index = 0;
    bool argumentBuffer = false;
    std::string refusal;  // Metal makes no encoder here: why; empty: `layouts` holds it
    std::vector<FlatArgumentLayout> layouts;
};

using BufferEncoders = std::vector<BufferEncoder>;

constexpr uint8_t kBufferEncoderArgumentBuffer = 1, kBufferEncoderRefused = 2;
constexpr uint32_t kBufferEncoderLimit = 31, kBufferEncoderLayoutLimit = 1u << 16, kBufferEncoderRunLimit = 1u << 16;

inline std::string bufferEncodersKey(const AirHash &air, const std::string &constants, bool specialized)
{
    std::string key;
    char b[4];
    for (uint8_t x : air) {
        snprintf(b, sizeof b, "%02x", x);
        key += b;
    }
    return key + " | " + constants + (specialized ? " | specialized" : "");
}

inline std::string encodeBufferEncoders(const BufferEncoders &encoders)
{
    std::string out;
    const auto put = [&out](const void *v, size_t n) {
        out.append(static_cast<const char *>(v), n);
    };
    const uint32_t count = uint32_t(encoders.size());
    put(&count, sizeof count);
    for (const BufferEncoder &e : encoders) {
        const uint8_t flags =
            (e.argumentBuffer ? kBufferEncoderArgumentBuffer : 0) | (e.refusal.empty() ? 0 : kBufferEncoderRefused);
        put(&e.index, sizeof e.index);
        put(&flags, sizeof flags);
        if (!e.refusal.empty()) {
            put(e.refusal.c_str(), e.refusal.size() + 1);
            continue;
        }
        const uint32_t layouts = uint32_t(e.layouts.size());
        put(&layouts, sizeof layouts);
        for (const FlatArgumentLayout &l : e.layouts) {
            const uint32_t runs = uint32_t(l.runs.size());
            put(&l.encodedLength, sizeof l.encodedLength);
            put(&l.alignment, sizeof l.alignment);
            put(&runs, sizeof runs);
            if (runs)
                put(l.runs.data(), runs * sizeof(ArgumentRun));
        }
    }
    return out;
}

// False where `blob` is not one whole answer: counts past their limits, indices out of order, a refusal with layouts
inline bool decodeBufferEncoders(const std::string &blob, BufferEncoders &out)
{
    out.clear();
    TagReader r(reinterpret_cast<const uint8_t *>(blob.data()), blob.size());
    uint32_t count = 0;
    if (!r.bytes(&count, sizeof count) || count > kBufferEncoderLimit)
        return false;
    out.resize(count);
    for (uint32_t i = 0; i < count; i++) {
        BufferEncoder &e = out[i];
        uint8_t flags = 0;
        if (!r.bytes(&e.index, sizeof e.index) || !r.u8(flags) || flags > 3 || e.index >= kBufferEncoderLimit ||
            (i && e.index <= out[i - 1].index))
            return false;
        e.argumentBuffer = flags & kBufferEncoderArgumentBuffer;
        if (flags & kBufferEncoderRefused) {
            if (!r.cstr(e.refusal) || e.refusal.empty())
                return false;
            continue;
        }
        uint32_t layouts = 0;
        if (!r.bytes(&layouts, sizeof layouts) || !layouts || layouts > kBufferEncoderLayoutLimit)
            return false;
        e.layouts.resize(layouts);
        for (FlatArgumentLayout &l : e.layouts) {
            uint32_t runs = 0;
            if (!r.bytes(&l.encodedLength, sizeof l.encodedLength) || !r.bytes(&l.alignment, sizeof l.alignment) ||
                !r.bytes(&runs, sizeof runs) || runs > kBufferEncoderRunLimit)
                return false;
            l.runs.resize(runs);
            if (runs && !r.bytes(l.runs.data(), runs * sizeof(ArgumentRun)))
                return false;
        }
    }
    return r.done();
}

// The answer at buffer `index`; null where the function declares no buffer there
inline const BufferEncoder *bufferEncoderAt(const BufferEncoders &encoders, uint32_t index)
{
    for (const BufferEncoder &e : encoders)
        if (e.index == index)
            return &e;
    return nullptr;
}

}  // namespace gm20b
