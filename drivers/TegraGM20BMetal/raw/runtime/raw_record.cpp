// SPDX-License-Identifier: MIT
// See raw_record.h. Each step follows the pack/mkpack.py or pack/layouts.py function it is named after: they are one rule, and a
// change to one is a change to the other (test/recordcheck fails otherwise). What the probe's capture gathers of a state is made
// into its record by raw_record_parts.h.
#include "raw_record.h"

#include <cstdio>
#include "../pack/probe_record.h"
#include "raw_record_parts.h"
#include "raw_residual.h"
#include "raw_srgb_table.h"

namespace raw {
namespace {

constexpr uint32_t kInterlockControl = 0x1224;                                   // SET_PIXEL_SHADER_INTERLOCK_CONTROL
constexpr uint32_t kRegisterCountVertex = 0x204c, kRegisterCountPixel = 0x214c;  // SET_PIPELINE_REGISTER_COUNT
constexpr uint32_t kTextureType3D = 2;   // TEXHEAD's TEXTURE_TYPE (word 4 bits 26:23)
constexpr uint32_t kTagBuffer = 0xC0DE;  // pack/probe.mm's payload tag

std::string hex(uint64_t v)
{
    char b[24];
    snprintf(b, sizeof b, "%llx", static_cast<unsigned long long>(v));
    return b;
}

uint32_t valueOf(const std::map<uint32_t, uint32_t> &regs, uint32_t m)
{
    const auto it = regs.find(m);
    return it == regs.end() ? 0 : it->second;
}

// mkpack.py interlocked: the storage form, in one draw or in a scratch between m2v's copy in and copy out
bool interlocked(const nvdec::ProbeSubmission &s)
{
    for (const nvdec::ProbeRecord &r : s.records)
        if (valueOf(r.regs, kInterlockControl) & 3)
            return true;
    return false;
}

// mkpack.py skip_reason
bool skipReason(const nvdec::ProbeSubmission &s, std::string &why)
{
    why.clear();
    if (s.draws != 1 && !s.dispatches && interlocked(s))
        why = "storage form through a scratch, m2v copying the color in and out around it";
    else if (s.draws != 1)
        why = std::to_string(s.draws) + " draws";
    else if (s.dispatches)
        why = std::to_string(s.dispatches) + " dispatches";
    else if (s.records.empty())
        why = "no record";
    if (!why.empty())
        return true;
    std::string stages;
    for (const auto &[stage, program] : s.records[0].programs) {
        if (!program.known) {
            why = "unknown program";
            return true;
        }
        stages += (stages.empty() ? "" : ", ") + std::to_string(stage);
    }
    if (s.records[0].programs.size() != 2 || !s.records[0].programs.count(1) || !s.records[0].programs.count(5)) {
        why = "stages [" + stages + "]";
        return true;
    }
    return false;
}

// layouts.py tic_kind
Tex textureKind(const nvdec::ProbeRecord::Header &t, const std::map<std::string, uint32_t> &targets, uint8_t &slot)
{
    const uint32_t width = (t.w[4] & 0xFFFF) + 1, height = (t.w[5] & 0xFFFF) + 1;
    if (width >= 16 && width < 32 && height == width - 8) {
        slot = uint8_t(width - 16);
        return Tex::Slot;
    }
    if (const auto it = targets.find(t.name); it != targets.end()) {
        slot = uint8_t(it->second);
        return Tex::Fetch;
    }
    slot = kNoSlot;
    return (t.w[4] >> 23 & 0xF) == kTextureType3D ? Tex::Texture3D : Tex::Unknown;
}

// layouts.py tsc_kind: probe.mm tags fragment sampler slot s with max LOD 1 + s/2 (nearest mip filter), linear filters when s&1
// and a repeating U when s&2; a header without such a tag is a constexpr sampler's
Smp samplerKind(const std::array<uint32_t, 8> &t, uint8_t &slot)
{
    const uint32_t lod = t[2] >> 12 & 0xFFF, mag = t[1] & 7, u = t[0] & 7;
    if (lod % 256 || lod < 256 || lod > 256 * 8 || (mag != 1 && mag != 2)) {
        return Smp::Constexpr;
    }
    const uint32_t s = 2 * (lod / 256 - 1) + (mag == 2 ? 1 : 0);
    if (s > 15 || u != (s & 2 ? 0u : 2u))
        return Smp::Derived;
    slot = uint8_t(s);
    return Smp::Slot;
}

// layouts.py buffer_entry's colour table: a descriptor of its size over its words (as many as the capture read)
bool isColorTable(const std::vector<uint32_t> &pw, uint32_t size)
{
    static const std::vector<uint32_t> table = [] {
        std::vector<uint32_t> w(kColorTableBytes / 4);
        colorTableWords(w.data());
        return w;
    }();
    return size == kColorTableBytes && !pw.empty() && pw.size() <= table.size() &&
           std::equal(pw.begin(), pw.end(), table.begin());
}

// layouts.py layout: set 0's entries; false: a problem (`why`)
bool layout(const nvdec::ProbeSubmission &s, uint32_t index, std::vector<LayoutEntry> &out, std::string &why)
{
    const nvdec::ProbeRecord &r = s.records[0];
    const std::vector<uint32_t> &words = r.sets[0].words;
    for (size_t o = 0; o < words.size();) {
        if (o % 4 == 0) {
            if (const auto p = r.setPointers[0].find(uint32_t(o * 4)); p != r.setPointers[0].end()) {
                const std::vector<uint32_t> &pw = p->second.words;
                const uint32_t stage = pw.size() > 1 ? pw[0] >> 8 & 0xFF : 0;
                if (o + 2 < words.size() && isColorTable(pw, words[o + 2])) {
                    LayoutEntry e;
                    e.offset = uint32_t(o * 4);
                    e.colorTable = true;
                    out.push_back(e);
                    o += 4;
                    continue;
                }
                if (pw.size() < 2 || pw[0] >> 16 != kTagBuffer || pw[1] != index || (stage != 1 && stage != 2)) {
                    why = "untagged pointer at " + std::to_string(o * 4) + " (" + hex(pw.empty() ? 0 : pw[0]) + ")";
                    return false;
                }
                LayoutEntry e;
                e.offset = uint32_t(o * 4);
                e.buffer = true;
                e.stage = uint8_t(stage);
                e.slot = uint8_t(pw[0] & 0xFF);
                out.push_back(e);
                o += 4;
                continue;
            }
        }
        const uint32_t w = words[o];
        if (w) {
            LayoutEntry e;
            e.offset = uint32_t(o * 4);
            if (const uint32_t tic = w & 0xFFFFF) {
                const auto t = r.tic.find(tic);
                e.tex = t == r.tic.end() ? Tex::Unknown : textureKind(t->second, s.targets, e.texSlot);
            }
            const uint32_t tsc = w >> 20;
            const auto smp = r.tsc.find(tsc);
            if (tsc == 0)
                e.smp = Smp::None;  // NVK's reserved sampler (nvk_device.c): the handle names no sampler
            else
                e.smp = smp == r.tsc.end() ? Smp::Constexpr : samplerKind(smp->second, e.smpSlot);
            // m2v binds a slot the state samples by comparison with its compare header (TEXSAMP0 DEPTH_COMPARE, bit 9)
            e.smpCompare = e.smp == Smp::Slot && (smp->second[0] >> 9 & 1);
            if (e.smp == Smp::Constexpr && smp != r.tsc.end()) {
                e.headerKnown = true;
                e.header = smp->second;
            }
            if (e.tex == Tex::Unknown) {
                why = "unidentified handle at " + std::to_string(o * 4) + ": " + hex(w);
                return false;
            }
            out.push_back(e);
        }
        o++;
    }
    return true;
}

// mkpack.py load_sizes: by set-0 byte, the range bound for the payload of a run whose draw is one record (missing: 0)
std::map<uint32_t, uint32_t> boundSizes(const nvdec::ProbeSubmission *s)
{
    std::map<uint32_t, uint32_t> out;
    if (!s || s->draws != 1 || s->dispatches || s->records.empty() || s->records[0].sets[0].words.empty())
        return out;
    const std::vector<uint32_t> &w = s->records[0].sets[0].words;
    for (size_t o = 0; o + 3 < w.size(); o += 4)
        if (s->records[0].setPointers[0].count(uint32_t(o * 4)))
            out[uint32_t(o * 4)] = w[o + 2];
    return out;
}

// mkpack.py attach_residuals
std::map<uint32_t, uint32_t> residualWords(const nvdec::ProbeSubmission *plain, const nvdec::ProbeSubmission *bound)
{
    if (!plain || !bound || bound->draws != 1 || bound->dispatches || bound->records.empty() || plain->records.empty())
        return {};
    const auto &was = plain->records[0].pushWords, &now = bound->records[0].pushWords;
    return residualWordsByValue({was.begin(), was.end()}, {now.begin(), now.end()});
}

// mkpack.py vertex_streams: the Metal vertex buffer slot and stride of the first stream; false: a stream fed by no vertex slot
bool vertexStream(const nvdec::ProbeRecord &r, uint8_t &slot, uint16_t &stride, std::string &why)
{
    slot = 0xFF;
    stride = 0;
    for (const auto &[j, stream] : r.streams) {
        const uint32_t w0 = stream.words.empty() ? 0 : stream.words[0];
        if (w0 >> 16 != kTagBuffer || (w0 >> 8 & 0xFF) != 1) {
            why = "vertex stream " + std::to_string(j) + " not a vertex slot";
            return false;
        }
        if (slot == 0xFF) {
            slot = uint8_t(w0 & 0xFF);
            stride = uint16_t(stream.format & 0xFFF);
        }
    }
    return true;
}

}  // namespace

bool makeRecord(const ProbeRuns &runs, const PackRules &rules, const ProgramLookup &program, RuntimeRecord &out,
                std::string &why)
{
    out = {};
    const nvdec::ProbeSubmission &s = *runs.bytes256;
    if (skipReason(s, why))
        return false;
    const nvdec::ProbeRecord &r = s.records[0];
    if (valueOf(r.regs, kInterlockControl) & 3) {
        why = "storage form: capture lacks qualified storage attachment metadata";
        return false;
    }
    RecordParts parts;
    for (int k = 0; k < 2; k++)
        if (!(parts.programs[k] = program(r.programs.at(k ? 5 : 1).id))) {
            why = "unknown program";
            return false;
        }
    if (!programsBindable(parts, why) || !layout(s, runs.index, parts.layout, why))
        return false;
    parts.sizes16 = boundSizes(runs.bytes16);
    parts.sizes4096 = boundSizes(runs.bytes4096);
    parts.residuals = residualWords(runs.bytes256, runs.bound);
    std::string streamWhy;
    const bool streams = vertexStream(r, parts.vertexSlot, parts.vertexStride, streamWhy);
    parts.regs = r.regs;
    for (const auto &[offset, word] : r.pushWords)
        parts.push[offset] = word;
    const std::vector<uint32_t> &root = r.root.words;
    parts.setBytes = r.sets[0].words.empty() || root.size() <= 0x38 / 4 + 2 ? 0 : uint16_t(root[0x38 / 4 + 2]);
    if (!assembleRecord(parts, runs.key, rules, out, why))
        return false;
    out.programs[0].gprs = uint8_t(valueOf(r.regs, kRegisterCountVertex));
    out.programs[1].gprs = uint8_t(valueOf(r.regs, kRegisterCountPixel));
    if (!streams) {
        why = streamWhy;
        return false;
    }
    return true;
}

}  // namespace raw
