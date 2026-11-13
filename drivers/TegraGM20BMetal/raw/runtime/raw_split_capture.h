// SPDX-License-Identifier: MIT
// DEVELOPMENT: the render server's split-draw capture (debug switch split-capture, RawMetalGlassProbe.mm). For the first
// kSplitCaptureDraws draws the split plan takes after an arming, what the bench's pack kind replays as one quad
// (hw_test_app/bench/bench_pack.cpp, tools/benchloop/capture.py): the state's key, the target, the draw's box and
// vertices, the root push words and every uniform block it binds, at its residual offset (its address & 255, which the
// replay keeps so the block takes the same residual push word). Blocks and vertices are copied as the CPU wrote them at
// encode. A record is a little-endian blob logged as hex lines of at most kSplitCaptureLine characters: per arming at
// most 32 records of 20,808 bytes, 1.3 MiB of hex.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace raw {

constexpr uint32_t kSplitCaptureDraws = 32, kSplitCaptureBlocks = 8, kSplitCaptureBlockBytes = 2048,
                   kSplitCaptureVertexBytes = 4096, kSplitCaptureChunk = 400;
constexpr uint64_t kSplitCaptureMs = 60000;
constexpr size_t kSplitCaptureLine = 900;
constexpr uint32_t kSplitCaptureMagic = 0x50414353, kSplitCaptureVersion = 1;

struct SplitCaptureBlock {
    uint32_t stage = 0, slot = 0, offset = 0;
    std::vector<uint8_t> bytes;

    bool operator==(const SplitCaptureBlock &o) const
    {
        return stage == o.stage && slot == o.slot && offset == o.offset && bytes == o.bytes;
    }
};

struct SplitCaptureRecord {
    uint64_t key = 0;
    uint32_t ca = 0, format = 0, width = 0, height = 0;
    uint32_t box[4] = {};
    uint32_t primitive = 0, first = 0, count = 0, stride = 0;  // the draw's Metal primitive and vertices
    uint32_t push[32] = {};
    std::vector<uint8_t> vertices;
    std::vector<SplitCaptureBlock> blocks;

    bool operator==(const SplitCaptureRecord &o) const
    {
        return key == o.key && ca == o.ca && format == o.format && width == o.width && height == o.height &&
               std::equal(box, box + 4, o.box) && primitive == o.primitive && first == o.first && count == o.count &&
               stride == o.stride && std::equal(push, push + 32, o.push) && vertices == o.vertices &&
               blocks == o.blocks;
    }
};

namespace capture {

inline void word(std::vector<uint8_t> &out, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        out.push_back(uint8_t(v >> (8 * i)));
}

inline void bytes(std::vector<uint8_t> &out, const std::vector<uint8_t> &v)
{
    word(out, uint32_t(v.size()));
    out.insert(out.end(), v.begin(), v.end());
}

struct Reader {
    const uint8_t *at, *end;

    bool word(uint32_t &v)
    {
        if (end - at < 4)
            return false;
        v = uint32_t(at[0]) | uint32_t(at[1]) << 8 | uint32_t(at[2]) << 16 | uint32_t(at[3]) << 24;
        at += 4;
        return true;
    }

    bool bytes(std::vector<uint8_t> &v, uint32_t most)
    {
        uint32_t n;
        if (!word(n) || n > most || end - at < ptrdiff_t(n))
            return false;
        v.assign(at, at + n);
        at += n;
        return true;
    }
};

}  // namespace capture

// magic, version, key (low, high), ca, format, width, height, box[4], primitive, first, count, stride, push[32], then
// the vertices and each block (stage, slot, offset, bytes) as a byte count and the bytes
inline std::vector<uint8_t> splitCaptureBlob(const SplitCaptureRecord &r)
{
    std::vector<uint8_t> out;
    for (uint32_t v :
         {kSplitCaptureMagic, kSplitCaptureVersion, uint32_t(r.key), uint32_t(r.key >> 32), r.ca, r.format, r.width,
          r.height, r.box[0], r.box[1], r.box[2], r.box[3], r.primitive, r.first, r.count, r.stride})
        capture::word(out, v);
    for (uint32_t v : r.push)
        capture::word(out, v);
    capture::bytes(out, r.vertices);
    capture::word(out, uint32_t(r.blocks.size()));
    for (const SplitCaptureBlock &b : r.blocks) {
        capture::word(out, b.stage);
        capture::word(out, b.slot);
        capture::word(out, b.offset);
        capture::bytes(out, b.bytes);
    }
    return out;
}

inline bool splitCaptureRecord(const uint8_t *blob, size_t size, SplitCaptureRecord &r)
{
    capture::Reader in{blob, blob + size};
    uint32_t magic, version, low, high, blocks;
    if (!in.word(magic) || magic != kSplitCaptureMagic || !in.word(version) || version != kSplitCaptureVersion ||
        !in.word(low) || !in.word(high))
        return false;
    r = SplitCaptureRecord();
    r.key = uint64_t(high) << 32 | low;
    for (uint32_t *v : {&r.ca, &r.format, &r.width, &r.height, &r.box[0], &r.box[1], &r.box[2], &r.box[3], &r.primitive,
                        &r.first, &r.count, &r.stride})
        if (!in.word(*v))
            return false;
    for (uint32_t &v : r.push)
        if (!in.word(v))
            return false;
    if (!in.bytes(r.vertices, kSplitCaptureVertexBytes) || !in.word(blocks) || blocks > kSplitCaptureBlocks)
        return false;
    r.blocks.resize(blocks);
    for (SplitCaptureBlock &b : r.blocks)
        if (!in.word(b.stage) || !in.word(b.slot) || !in.word(b.offset) || !in.bytes(b.bytes, kSplitCaptureBlockBytes))
            return false;
    return in.at == in.end;
}

// `splitcap <serial> key=<key> fmt=<format> <w>x<h> box=<x0>,<y0>-<x1>,<y1> bytes=<n> parts=<p>`, then each part
// `splitcap <serial>.<i> <hex>`
inline std::vector<std::string> splitCaptureLines(const SplitCaptureRecord &r, uint32_t serial)
{
    const std::vector<uint8_t> blob = splitCaptureBlob(r);
    const size_t parts = (blob.size() + kSplitCaptureChunk - 1) / kSplitCaptureChunk;
    char head[160];
    snprintf(head, sizeof head, "splitcap %u key=%016llx fmt=%u %ux%u box=%u,%u-%u,%u bytes=%zu parts=%zu", serial,
             (unsigned long long)r.key, r.format, r.width, r.height, r.box[0], r.box[1], r.box[2], r.box[3],
             blob.size(), parts);
    std::vector<std::string> lines{head};
    for (size_t p = 0; p < parts; p++) {
        char prefix[32];
        snprintf(prefix, sizeof prefix, "splitcap %u.%zu ", serial, p);
        std::string line = prefix;
        const size_t from = p * kSplitCaptureChunk, to = std::min(blob.size(), from + kSplitCaptureChunk);
        for (size_t i = from; i < to; i++) {
            char hex[3];
            snprintf(hex, sizeof hex, "%02x", blob[i]);
            line += hex;
        }
        lines.push_back(line);
    }
    return lines;
}

// The record a capture's lines (a header and its parts, in order) hold
inline bool splitCaptureFromLines(const std::vector<std::string> &lines, SplitCaptureRecord &r)
{
    std::vector<uint8_t> blob;
    for (size_t i = 1; i < lines.size(); i++) {
        const size_t space = lines[i].rfind(' ');
        if (space == std::string::npos)
            return false;
        for (size_t c = space + 1; c + 1 < lines[i].size(); c += 2)
            blob.push_back(uint8_t(std::stoul(lines[i].substr(c, 2), nullptr, 16)));
    }
    return splitCaptureRecord(blob.data(), blob.size(), r);
}

}  // namespace raw
