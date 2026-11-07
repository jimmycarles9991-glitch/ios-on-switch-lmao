// SPDX-License-Identifier: MIT
// See raw_capture.h.
#include "raw_capture.h"

#include <cstdio>
#include "../pack/nvdec.h"
#include "raw_owned.h"
#include "raw_pack.h"

namespace raw {
namespace {

constexpr uint32_t kMethods = 0x10000;  // a subchannel's method space, bytes
constexpr int kVertexStage = 1, kPixelStage = 5;

// A one-method incrementing header (NV's PUSH_HDR_INC)
uint32_t methodHeader(uint32_t subc, uint32_t mthd)
{
    return 1u << 29 | 1u << 16 | subc << 13 | mthd >> 2;
}

CapturedProgram program(nvdec::Decoder &d, int stage)
{
    CapturedProgram p;
    if (const nvdec::ProgramInfo *known = d.boundProgram(stage, p.id)) {
        p.image = known->image;
        p.data = known->data;
        p.cbufTypes = known->cbufTypes;
    }
    return p;
}

}  // namespace

Capture::Capture(std::function<const uint8_t *(uint64_t va, uint64_t bytes)> map)
    : decoder_(std::make_unique<nvdec::Decoder>(std::move(map), fopen("/dev/null", "w")))
{
    decoder_->fullStateAtFirstDraw = false;
    decoder_->onDraw = [this](nvdec::Decoder &d) {
        record(d);
    };
}

Capture::~Capture() = default;

bool Capture::prime(const Pack &pack, std::string &err)
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
        words.push_back(init[i].data);
    }
    decoder_->submitBegin();
    decoder_->push(words.data(), words.size());
    decoder_->submitEnd();
    context_.resize(kMethods / 4);
    for (uint32_t m = 0; m < kMethods; m += 4)
        context_[m / 4] = decoder_->reg3d(m);
    return true;
}

void Capture::set(uint32_t subc, uint32_t mthd, uint32_t value)
{
    const uint32_t words[2] = {methodHeader(subc, mthd), value};
    decoder_->submitBegin();
    decoder_->push(words, 2);
    decoder_->submitEnd();
    if (subc == 0)
        context_[mthd / 4] = value;
}

std::vector<CapturedState> Capture::decode(const std::vector<std::pair<const uint32_t *, uint64_t>> &segments)
{
    drawn_.clear();
    decoder_->submitBegin();
    for (const auto &s : segments)
        decoder_->push(s.first, s.second);
    decoder_->submitEnd();
    return std::move(drawn_);
}

void Capture::record(nvdec::Decoder &d)
{
    CapturedState s;
    for (uint32_t m = 0; m < kMethods; m += 4)
        if (!ownedMethod(m) && d.reg3d(m) != context_[m / 4])
            s.registers.push_back({m, d.reg3d(m)});
    s.vertex = program(d, kVertexStage);
    s.pixel = program(d, kPixelStage);
    d.pushWords(s.pushMask, s.push);
    drawn_.push_back(std::move(s));
}

}  // namespace raw
