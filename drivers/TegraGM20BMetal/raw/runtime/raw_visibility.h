// SPDX-License-Identifier: MIT
// Metal's visibility results on the raw path (rule vis.report). M4 writes each offset an encoder set a mode at once, at
// the end of the encoder: the samples that passed (Counting) or 1 when any did (Boolean), 0 for an offset set with no
// draw after it, and an offset it never set keeps its bytes (m2v runtime/src/visibility.rs, measured). Here each mode set
// is one count of the 3D engine's samples-passed counter, NVK's occlusion query: reported as 16 bytes (count, timestamp)
// into a slot of the device's report page when the next mode is set, then copied to the offset by the copy engine once
// the encoder ends, after a wait for idle. The report never goes to the offset itself: its timestamp half would overwrite
// the next offset's result.
//
// A pass whose visibilityResultType is Accumulate (iOS 26) adds to the offsets instead, as an M4 Max does it (macOS
// 27.2.1, measured): Counting adds the count to the offset's low word modulo 2^32 and keeps its high word, Boolean ORs 1
// into the low word where a sample passed, and an offset set with no draw keeps its bytes. Counting is the counter's low
// word reported into the offset with the report's own ADD reduction; Boolean ORs 1 in by a copy-engine release that
// render enable gates on the count, whose low word the slot holds with zeros around it as the compare's operands.
#pragma once
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <vector>
#include "raw_push.h"
#include "../../../../kits/SwitchGM20BEmit/SwitchGM20BEmitPredicate.h"
#include "../../../../kits/SwitchGM20BEmit/SwitchGM20BEmitReport.h"

namespace raw {

enum : uint32_t {
    kVisibilityDisabled = 0,
    kVisibilityBoolean = 1,
    kVisibilityCounting = 2,
};

struct VisibilityAsk {
    uint32_t mode = kVisibilityDisabled;
    uint64_t offset = 0, bufferLength = 0;
    bool rawBuffer = false;
};

// nullptr where the raw path writes the result, else the validation error Metal stops it with
const char *visibilityRefusal(const VisibilityAsk &a);

// An encoder takes as many queries as distinct offsets the device answers (maxVisibilityQueryOffset 65,528), and the
// page as many slots, so no encoder takes a slot twice. A slot: the report at +0 or, accumulating Boolean, the compare's
// operands at +0 and +16.
constexpr uint32_t kVisibilitySlotBytes = 32, kVisibilitySlots = 8192;
constexpr uint32_t kVisibilityQueriesPerEncoder = 65528 / 8 + 1;

class VisibilityQueries {
public:
    static constexpr uint32_t kSetWords = SWITCH_GM20B_SAMPLES_REPORT_WORDS + 3 * SWITCH_GM20B_RELEASE_WORD_WORDS +
                                          SWITCH_GM20B_SAMPLES_BEGIN_WORDS + SWITCH_GM20B_SAMPLES_END_WORDS;
    static constexpr uint32_t kFinishWords = 7, kResultWords = 2 * SWITCH_GM20B_COPY_WORD_REDUCE_WORDS;

    enum class Set : uint8_t {
        Done,
        // Metal's validation stops it, and an offset past maxVisibilityQueryOffset; plain Metal adds the draws since to
        // the result, which no copy does
        Retaken,
    };

    // setVisibilityResultMode:offset:, `resultVa` the offset's address rounded down to 8 bytes as M4 rounds it and
    // `slotVa` a free slot of the report page (both unused for Disabled); `accumulate`: the pass's results add up
    Set set(Push &p, uint32_t mode, uint64_t resultVa, uint64_t slotVa, bool accumulate = false);

    bool any() const
    {
        return !queries_.empty() || counting_;
    }

    size_t results() const
    {
        return queries_.size();
    }

    // The open count reported, the counter off and the 3D engine idle, ahead of finishResult
    void finish(Push &p);

    // Result `i` at its offset
    void finishResult(Push &p, size_t i) const;

    void clear()
    {
        queries_.clear();
        taken_.reset();
        open_ = counting_ = false;
    }

private:
    struct Query {
        uint64_t resultVa, slotVa;
        uint32_t mode;
        bool accumulate;
    };

    void close(Push &p);

    std::vector<Query> queries_;
    // Offsets taken, by the result's 8-byte word modulo maxVisibilityQueryOffset's span: distinct for every offset the
    // device answers in one buffer
    std::bitset<kVisibilityQueriesPerEncoder> taken_;
    bool open_ = false, counting_ = false;
};

}  // namespace raw
