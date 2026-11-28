// SPDX-License-Identifier: MIT
// The shader compiler's compiled stages (NIR and NAK's work on one stage, direct_nvk.h compileStage), kept by a digest
// of everything the stage compile reads, so a stage that another state, or another form of the same state, compiles
// from the same inputs is compiled once per process. The engine (compiler_engine.h) gives its direct compile one.
// A kernel is compiled each time: its stage compile reads m2v's native program, which no digest here reads, and it is a
// small part of a kernel's compile.
#pragma once
#include <CommonCrypto/CommonDigest.h>
#include <array>
#include <cstdint>
#include <cstring>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include "compiler_times.h"
#include "../direct_nvk.h"

namespace switch_compiler {

using StageDigest = std::array<uint8_t, CC_SHA256_DIGEST_LENGTH>;

// What a stage compile reads, digested by part, so two states whose stages differ name the part
struct StageInputs {
    enum Part {
        Code,
        Specialization,
        Pipeline,
        Push,
        Targets,
        Layout,
        Compiler,
        kParts
    };

    static const char *name(int part)
    {
        static const char *const names[kParts] = {"code",    "specialization", "pipeline", "push",
                                                  "targets", "layout",         "compiler"};
        return names[part];
    }

    std::array<StageDigest, kParts> parts{};
    bool keyable = true;  // false: an input no part reads (a native program, a chained structure it does not know)

    StageDigest key() const
    {
        StageDigest out;
        CC_SHA256(parts.data(), CC_LONG(sizeof parts), out.data());
        return out;
    }
};

namespace stage_inputs {

// VkSpecializationInfo and its map entries, which this build has no Vulkan headers for
struct SpecializationEntry {
    uint32_t id, offset;
    size_t size;
};

struct Specialization {
    uint32_t count;
    const SpecializationEntry *entries;
    size_t dataSize;
    const void *data;
};

constexpr uint32_t kSubgroupSizeType = 1000225001;  // VkPipelineShaderStageRequiredSubgroupSizeCreateInfo
constexpr uint32_t kFragmentStage = 0x10;           // VK_SHADER_STAGE_FRAGMENT_BIT
constexpr size_t kPushRangeBytes = 12;              // VkPushConstantRange

class Digest {
public:
    Digest()
    {
        CC_SHA256_Init(&c_);
    }

    void bytes(const void *p, size_t n)
    {
        word(n);
        if (n)
            CC_SHA256_Update(&c_, p, CC_LONG(n));
    }

    template <class T> void word(T v)
    {
        CC_SHA256_Update(&c_, &v, CC_LONG(sizeof v));
    }

    StageDigest done()
    {
        StageDigest out;
        CC_SHA256_Final(out.data(), &c_);
        return out;
    }

private:
    CC_SHA256_CTX c_;
};

}  // namespace stage_inputs

// Every field of NvkStageInfo is read below, or makes the stage unkeyable
static_assert(sizeof(gm20b::NvkStageInfo) == 312, "a field added to NvkStageInfo is read by stageInputs");

inline StageInputs stageInputs(const gm20b::StageCompile &c, const gm20b::NvkStageInfo &info,
                               const gm20b::NakLayout &layout)
{
    using namespace stage_inputs;
    StageInputs in;
    const auto *subgroup = static_cast<const gm20b::SubgroupSize *>(info.stagePnext);
    in.keyable = !info.semantic && !info.portable && !info.portableContext && info.spirv && info.spirvBytes >= 20 &&
                 !info.layout && !info.pipelinePnext &&
                 (!subgroup || (subgroup->sType == kSubgroupSizeType && !subgroup->next));
    if (!in.keyable)
        return in;
    {
        Digest d;
        d.bytes(info.spirv, info.spirvBytes);
        d.bytes(info.entry, info.entry ? strlen(info.entry) : 0);
        d.word(info.stage);
        in.parts[StageInputs::Code] = d.done();
    }
    {
        Digest d;
        const auto *s = static_cast<const Specialization *>(info.specialization);
        d.word(uint32_t(s ? s->count : 0));
        for (uint32_t i = 0; s && i < s->count; i++) {
            d.word(s->entries[i].id);
            d.word(s->entries[i].offset);
            d.word(uint64_t(s->entries[i].size));
        }
        d.bytes(s ? s->data : nullptr, s && s->data ? s->dataSize : 0);
        in.parts[StageInputs::Specialization] = d.done();
    }
    {
        Digest d;
        d.word(info.pipelineFlags);
        d.word(uint8_t(info.sampleShading));
        d.word(uint8_t(info.split));
        d.word(info.centerFragDepth);
        d.word(uint8_t(info.texsF16));
        d.word(uint8_t(info.turned));
        d.word(subgroup ? subgroup->size : 0u);
        in.parts[StageInputs::Pipeline] = d.done();
    }
    {
        // the stage compile folds the masked words alone (nvk_switch_push.c), and so the digest reads them alone
        Digest d;
        d.word(info.constPushMask);
        for (uint32_t i = 0; i < 32; i++)
            d.word(info.constPushMask >> i & 1 ? info.constPushWords[i] : 0u);
        in.parts[StageInputs::Push] = d.done();
    }
    {
        // a stage other than a fragment's never reads them (nvk_switch_round.c, nvk_shader_compile.c's fs key)
        Digest d;
        d.word(info.stage == kFragmentStage ? info.halfTargets : 0u);
        d.word(info.stage == kFragmentStage ? info.unormTargets : 0u);
        d.bytes(info.xrPlaneBindings.data(), sizeof info.xrPlaneBindings);
        in.parts[StageInputs::Targets] = d.done();
    }
    {
        Digest d;
        d.word(layout.setCount);
        d.word(layout.createFlags);
        for (uint32_t i = 0; i < layout.setCount; i++) {
            const gm20b::NakSet &set = layout.sets[i];
            d.word(set.bindingCount);
            d.word(set.flags);
            for (uint32_t b = 0; b < set.bindingCount; b++)
                d.bytes(&set.bindings[b], sizeof set.bindings[b]);
        }
        d.bytes(layout.pushRanges, layout.pushRanges ? layout.pushRangeCount * kPushRangeBytes : 0);
        in.parts[StageInputs::Layout] = d.done();
    }
    {
        Digest d;
        d.word(uint8_t(c.ranked));
        d.word(uintptr_t(c.target));
        in.parts[StageInputs::Compiler] = d.done();
    }
    return in;
}

// The process's compiled stages, shared by the pool's workers (each with its own Direct) by one rule: an asker finds or
// adds its key's slot under `lock_`, then holds that slot's `making` for its compile, so the first asker of a key
// compiles it and a concurrent or later asker waits on `making` and takes the same Made; a failed compile is not kept
// (the next asker compiles again). A kept stage is never written again: an asker gets a copy with no owner and shares
// the Made through `held`, which outlives the stage's eviction. Lock order: a slot's `making`, then `lock_` (used,
// record, forget); `lock_` is never held across a compile or a wait, and a worker holds one slot at a time.
class CompiledStages final : public gm20b::StageMemo {
public:
    // Up to `bytes` of compiled stages: past it the least recently used goes, and a larger stage is not kept
    explicit CompiledStages(size_t bytes) : limit_(bytes)
    {}

    CompiledStages(const CompiledStages &) = delete;
    CompiledStages &operator=(const CompiledStages &) = delete;
    ~CompiledStages() = default;

    int32_t compile(const gm20b::StageCompile &c, const gm20b::NvkStageInfo &info, const gm20b::NakLayout &layout,
                    gm20b::NvkStage &out, std::shared_ptr<const void> &held) override
    {
#if SWITCH_DEBUG
        const uint64_t began = uptimeNs();

        struct Timed {
            uint64_t began;

            ~Timed()
            {
                tJobSplit.codegenNs += uptimeNs() - began;
            }
        } timed{began};
#endif
        const StageInputs inputs = stageInputs(c, info, layout);
        if (!inputs.keyable)
            return gm20b::compileStageNow(c, info, layout, out);
        const StageDigest key = inputs.key();
        std::shared_ptr<Slot> slot;
        SWITCH_JOB_COUNT(stagesAsked);
        {
            std::lock_guard<std::mutex> l(lock_);
            std::shared_ptr<Slot> &s = slots_[key];
            if (!s) {
                s = std::make_shared<Slot>();
                s->key = key;
            }
            slot = s;
        }
        std::lock_guard<std::mutex> making(slot->making);
        if (slot->made) {
            used(slot);
        } else {
            auto made = std::make_shared<Made>();
            if (const int32_t result = gm20b::compileStageNow(c, info, layout, made->stage); result != 0) {
                forget(key, slot);
                return result;
            }
            slot->made = made;
            record(key, slot, bytesOf(made->stage));
            SWITCH_JOB_COUNT(stagesMade);
        }
        out = slot->made->stage;
        out.owner = nullptr;
        held = slot->made;
        return 0;
    }

    struct Kept {
        uint64_t entries = 0, bytes = 0;
    };

    Kept kept() const
    {
        std::lock_guard<std::mutex> l(lock_);
        return {uint64_t(order_.size()), uint64_t(bytes_)};
    }

private:
    // A compiled stage the memo owns: its code, data and split live until its last holder lets go
    struct Made {
        gm20b::NvkStage stage = {};

        ~Made()
        {
            if (stage.owner)
                switch_compile_stage_finish(&stage);
        }
    };

    // One digest's compile. `making` is held for that one compile: a second worker asking the same stage (the pool's
    // workers take a burst of one pipeline's formats at once) waits on it and takes the stage, and the kernel lends a
    // mutex's owner the waiter's priority.
    struct Slot {
        std::mutex making;
        StageDigest key{};
        std::shared_ptr<const Made> made;
        size_t bytes = 0;
        std::list<std::shared_ptr<Slot>>::iterator at;  // its place in order_ once made
    };

    static size_t bytesOf(const gm20b::NvkStage &s)
    {
        // a split stage holds its per-pixel program and prologue beside the stage's
        return sizeof(Made) + sizeof(Slot) + (size_t(s.codeBytes) + s.dataBytes) * (s.split ? 3 : 1);
    }

    void used(const std::shared_ptr<Slot> &slot)
    {
        std::lock_guard<std::mutex> l(lock_);
        if (slot->bytes)
            order_.splice(order_.end(), order_, slot->at);
    }

    void record(const StageDigest &key, const std::shared_ptr<Slot> &slot, size_t bytes)
    {
        std::lock_guard<std::mutex> l(lock_);
        const auto found = slots_.find(key);
        if (found == slots_.end() || found->second != slot)
            return;
        // a stage past the whole bound is not kept at the others' cost
        if (bytes > limit_) {
            slots_.erase(found);
            return;
        }
        slot->bytes = bytes;
        slot->at = order_.insert(order_.end(), slot);
        bytes_ += bytes;
        while (bytes_ > limit_ && order_.size() > 1) {
            const std::shared_ptr<Slot> oldest = order_.front();
            order_.pop_front();
            bytes_ -= oldest->bytes;
            oldest->bytes = 0;
            slots_.erase(oldest->key);
        }
    }

    void forget(const StageDigest &key, const std::shared_ptr<Slot> &slot)
    {
        std::lock_guard<std::mutex> l(lock_);
        const auto found = slots_.find(key);
        if (found != slots_.end() && found->second == slot && !slot->bytes)
            slots_.erase(found);
    }

    const size_t limit_;
    // The digests, the order of use and the counts, held for lookups and bookkeeping only, never across a compile: the
    // pool's workers ask at once
    mutable std::mutex lock_;
    std::map<StageDigest, std::shared_ptr<Slot>> slots_;
    std::list<std::shared_ptr<Slot>> order_;  // made stages, the most recently used last
    size_t bytes_ = 0;
};

}  // namespace switch_compiler
