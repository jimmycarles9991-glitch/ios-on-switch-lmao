// SPDX-License-Identifier: MIT
// See raw_capture.h.
#include "raw_capture.h"

#include <cstdio>
#include "../pack/nvdec.h"
#include "raw_pack.h"

namespace raw {
namespace {

constexpr uint32_t kMmeScratch = 0x3400;  // MAXWELL_B's MME shadow scratch: NVK's bookkeeping, not state

// A one-method incrementing header (NV's PUSH_HDR_INC)
uint32_t methodHeader(uint32_t subc, uint32_t mthd)
{
    return 1u << 29 | 1u << 16 | subc << 13 | mthd >> 2;
}

}  // namespace

Capture::Capture(Map map, ProgramAt programAt)
    : map_(std::move(map)), programAt_(std::move(programAt)), sink_(fopen("/dev/null", "w"))
{
    primed_ = std::make_unique<nvdec::Decoder>(
        [this](uint64_t va, uint64_t bytes) {
            return map_(va, bytes);
        },
        sink_);
    primed_->fullStateAtFirstDraw = false;
    primed_->programAt = [this](uint32_t offset, nvdec::ProgramInfo &out) {
        return programAt_(offset, out);
    };
}

Capture::~Capture()
{
    primed_.reset();
    if (sink_)
        fclose(sink_);
}

bool Capture::prime(const Pack &pack, const InitAddresses &here, std::string &err)
{
    const PackInitMethod *init = pack.section<PackInitMethod>(PackInit);
    const uint64_t count = pack.bytes(PackInit) / sizeof(PackInitMethod);
    if (!count) {
        err = "the pack holds no context init";
        return false;
    }
    std::vector<uint32_t> words;
    words.reserve(2 * count);
    for (uint64_t i = 0; i < count; i++) {
        words.push_back(methodHeader(init[i].subc, init[i].mthd));
        words.push_back(relocated(init[i], here));
    }
    primed_->submitBegin();
    primed_->push(words.data(), words.size());
    primed_->submitEnd();
    return true;
}

nvdec::ProbeSubmission Capture::decode(const std::vector<std::pair<const uint32_t *, uint64_t>> &segments)
{
    nvdec::ProbeSubmission s;
    // the context copied, the programs the decoder knows moved (they only grow, and are handed back below)
    std::multimap<uint64_t, nvdec::ProgramInfo> programs = std::move(primed_->programs);
    nvdec::Decoder d(*primed_);
    d.programs = std::move(programs);
    d.onDraw = [&s](nvdec::Decoder &at) {
        s.records.emplace_back();
        at.probeRecord(s.records.back());
        s.targets.insert(s.records.back().targets.begin(), s.records.back().targets.end());
    };
    d.submitBegin();
    for (const auto &segment : segments)
        d.push(segment.first, segment.second);
    d.submitEnd();
    s.draws = uint32_t(d.draws - primed_->draws);
    s.dispatches = uint32_t(d.dispatches - primed_->dispatches);
    s.clears = uint32_t(d.clears - primed_->clears);
    // the programs it learned from the driver stay known: the next capture's draws of them match by their bytes
    primed_->programs = std::move(d.programs);
    return s;
}

std::map<uint32_t, uint32_t> Capture::context() const
{
    std::map<uint32_t, uint32_t> out;
    for (uint32_t m = 0; m < kMmeScratch; m += 4)
        if (const uint32_t v = primed_->reg3d(m))
            out[m] = v;
    return out;
}

const CapturedProgram *Capture::program(const std::string &id)
{
    if (const auto it = programs_.find(id); it != programs_.end())
        return &it->second;
    const nvdec::ProgramInfo *p = primed_->programById(id);
    if (!p)
        return nullptr;
    return &(programs_[id] = {id, p->image, p->data, p->cbufTypes});
}

}  // namespace raw
