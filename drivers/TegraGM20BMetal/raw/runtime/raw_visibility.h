// SPDX-License-Identifier: MIT
// Metal's visibility results on the raw path (rule vis.report). M4 writes each offset an encoder set a mode at once, at
// the end of the encoder: the samples that passed (Counting) or 1 when any did (Boolean), 0 for an offset set with no
// draw after it, and an offset it never set keeps its bytes (m2v runtime/src/visibility.rs, measured). Here each mode set
// is one count of the 3D engine's samples-passed counter, NVK's occlusion query: reported as 16 bytes (count, timestamp)
// into a slot of the device's report page when the next mode is set, then copied to the offset by the copy engine once
// the encoder ends, after a wait for idle. The report never goes to the offset itself: its timestamp half would overwrite
// the next offset's result.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>
#include "raw_push.h"
#include "../../../../kits/SwitchGM20BEmit/SwitchGM20BEmitPredicate.h"

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
    bool accumulate = false;  // MTLVisibilityResultTypeAccumulate (iOS 26)
};

// nullptr where the raw path writes the result, else why m2v does
const char *visibilityRefusal(const VisibilityAsk &a);

constexpr uint32_t kVisibilitySlotBytes = 16, kVisibilitySlots = 4096;
constexpr uint32_t kVisibilityQueriesPerEncoder = 256;

class VisibilityQueries {
public:
    static constexpr uint32_t kSetWords = 8, kFinishWords = 7, kResultWords = 2 * SWITCH_GM20B_COPY_WORD_REDUCE_WORDS;

    enum class Set : uint8_t {
        Done,
        Retaken,  // Metal's validation stops it; plain Metal adds the draws since to the result, which no copy does
        Full,
    };

    // setVisibilityResultMode:offset:, `resultVa` the offset's address rounded down to 8 bytes as M4 rounds it and
    // `slotVa` a free slot of the report page (both unused for Disabled)
    Set set(Push &p, uint32_t mode, uint64_t resultVa, uint64_t slotVa);

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
        open_ = counting_ = false;
    }

private:
    struct Query {
        uint64_t resultVa, slotVa;
        uint32_t mode;
    };

    void close(Push &p);

    std::vector<Query> queries_;
    bool open_ = false, counting_ = false;
};

}  // namespace raw
