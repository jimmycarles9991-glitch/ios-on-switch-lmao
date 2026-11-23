// SPDX-License-Identifier: MIT
// DEVELOPMENT: the frame-weights capture (TegraGM20BMetalDrawTimes.mm asks for it, tools/devicerun/frame_weights.py reads it). In
// the next few command buffers one process begins, every draw, every pass's prologue and the work ahead of each prologue
// end in a mark: the tiled cache flushed, a wait for idle, then a 3D report release (payload and PTIMER) and a
// PS_INVOCATIONS report (the counter and PTIMER). The time from one mark to the next is what the GPU took for the work
// between them started on an idle pipe, the counter's step the fragment shader invocations it ran: each draw's cost alone,
// which ranks states. It is not its share of a normal frame, where draws overlap in the pipeline and share the tiled
// cache's tiles, and a captured command buffer runs longer than the ones around it, which the capture's line names
// (`ref`). Without the flush a binned draw's pixels land in whichever later span flushes them (stamps.h).
//
// The counter runs only while a captured command buffer does: the channel's context init leaves SET_STATISTICS_COUNTER 0
// (kits/SwitchGM20BEmit/context/context.tsv), so begin() sets PS_INVOCATIONS_ENABLE for a captured one and 0 again at the
// next command buffer it does not capture.
#pragma once
#include "lock.h"
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "../../kits/SwitchGM20B/SwitchGM20B.h"
#include "push.h"
#include "span_marks.h"
#include "stamps.h"
#include "stats.h"

namespace gm20b {

// A mark's words: the tiled cache's flush, a wait for idle, the time's release, the invocations' report
constexpr uint32_t kTimedMarkWords = 2 + kSpanMarkWords + 5;
// A pass prologue's: the mark it starts with and the one it ends with
constexpr uint32_t kTimedPassWords = SWITCH_METAL_DIAGNOSTICS ? 2 * kTimedMarkWords : 0;

#if SWITCH_METAL_DIAGNOSTICS
constexpr uint32_t kTimedMarks = 4096;   // marks a capture holds over all its command buffers
constexpr uint32_t kTimedBuffers = 16;   // command buffers a capture takes at most
constexpr uint32_t kTimedStates = 512;   // drawn programs a capture names
constexpr uint32_t kTimedPasses = 1024;  // passes a capture describes
constexpr uint32_t kTimedAskedByName = 4;
constexpr uint32_t kNoMark = UINT32_MAX;
constexpr uint16_t kNoState = UINT16_MAX, kNoPass = UINT16_MAX;
// a line's text after "SwitchMetal: fw ", under what os_log keeps of one message
constexpr size_t kTimedLineBytes = 900;

constexpr uint32_t kStatisticsPsInvocations = NVDEF(NVB197, SET_STATISTICS_COUNTER, PS_INVOCATIONS_ENABLE, TRUE);
// NVK's fragment-invocations query report (nvk_query_pool.c): the 64-bit counter and PTIMER where the pixel shader ends
constexpr uint32_t kInvocationsReport = NVDEF(NVB197, SET_REPORT_SEMAPHORE_D, OPERATION, REPORT_ONLY) |
                                        NVDEF(NVB197, SET_REPORT_SEMAPHORE_D, PIPELINE_LOCATION, PIXEL_SHADER) |
                                        NVDEF(NVB197, SET_REPORT_SEMAPHORE_D, REPORT, PS_INVOCATIONS) |
                                        NVDEF(NVB197, SET_REPORT_SEMAPHORE_D, STRUCTURE_SIZE, FOUR_WORDS) |
                                        NVDEF(NVB197, SET_REPORT_SEMAPHORE_D, FLUSH_DISABLE, TRUE);

struct TimedMark {
    StampRelease release;  // the capture's serial and PTIMER
    uint64_t invocations, ptimer;
};

static_assert(sizeof(TimedMark) == 32, "two four-word reports");

inline void emitTimedMark(Push &p, uint64_t va, uint32_t serial, bool tiledCache)
{
    if (tiledCache)
        p.m(SWITCH_NVB197_TILED_CACHE_FLUSH, 0);
    p.m(NVB197_WAIT_FOR_IDLE, 0);
    emitSpanMark(p, va, serial);
    const uint64_t counter = va + offsetof(TimedMark, invocations);
    p.inc(NVB197_SET_REPORT_SEMAPHORE_A, {uint32_t(counter >> 32) & 0xff, uint32_t(counter), 0, kInvocationsReport});
}

// The pixel program's register count a state's push words set last (build() writes SET_PIPELINE_REGISTER_COUNT(PIXEL) as an
// immediate), walked header by header so a data word is never read as one; 0 where they set none
inline uint32_t pixelRegisters(const uint32_t *words, size_t count)
{
    constexpr uint32_t kMethod = NVB197_SET_PIPELINE_REGISTER_COUNT(NVB197_SET_PIPELINE_SHADER_TYPE_PIXEL);
    uint32_t found = 0;
    for (size_t i = 0; i < count;) {
        const uint32_t h = words[i], op = h >> 29, n = h >> 16 & 0x1fff, method = (h & 0x1fff) << 2;
        const bool threeD = (h >> 13 & 7) == SUBC_3D;
        if (op == 4) {
            found = threeD && method == kMethod ? n : found;
            i++;
            continue;
        }
        if (op != 1 && op != 3 && op != 5)
            break;
        for (uint32_t k = 0; k < n && i + 1 + k < count; k++) {
            const uint32_t at = op == 1 ? method + 4 * k : op == 5 && k ? method + 4 : method;
            found = threeD && at == kMethod ? words[i + 1 + k] : found;
        }
        i += 1 + size_t(n);
    }
    return found;
}

// What ends at a mark: the command buffer's submission prologue (Start), what ran since the last mark ahead of a pass
// (Between: compute and blit encoders, the pass before's end), a pass's barrier, targets and clears (Prologue), a draw, an
// indirect draw or indirect command buffer (Indirect); the Tail runs to the command buffer's end stamp
enum class TimedKind : uint8_t {
    Start,
    Between,
    Prologue,
    Draw,
    Indirect,
    Tail
};

inline char timedKindLetter(TimedKind k)
{
    return "sbpdit"[size_t(k)];
}

struct TimedLabel {
    TimedKind kind = TimedKind::Draw;
    uint8_t topology = 0xff;  // PRIMITIVE_TOPOLOGY's value
    uint16_t state = kNoState, pass = kNoPass;
    uint32_t vertices = 0, instances = 0;
    uint32_t box = 0;  // the pixels of its scissor within its viewport, its geometry's bound where known
};

// A program a draw ran: its state's key, the form drawn (SpanForm), its fragment and vertex code in 8-byte words (a
// scheduling word in every four), its pixel program's registers and passes a pixel
struct TimedState {
    uint64_t key = 0;
    const char *name = nullptr;  // Core Animation's or the driver's own, alive as long as the process
    uint32_t fragmentWords = 0, vertexWords = 0;
    uint8_t form = 0, gprs = 0, samplePasses = 1;
};

// A pass's colour target 0 (its depth target where it has none)
struct TimedPass {
    uint32_t format = 0;
    uint16_t width = 0, height = 0;
    uint8_t samples = 1;
    bool blockLinear = false, compressed = false;
};

struct TimedBuffer {
    enum class State : uint8_t {
        Encoding,
        Committed,
        Reported,
        Dropped
    };

    State state = State::Encoding;
    uint32_t index = 0, stamp = 0, seqno = 0;
    uint64_t generation = 0;
    uint32_t firstMark = 0, marks = 0, full = 0;  // full: marks it wanted after the capture's were all taken
    uint32_t firstPass = 0, passes = 0;
};

struct TimedRow {
    TimedLabel label;
    uint64_t ns = 0;
    int64_t invocations = -1;  // -1: not counted (the first mark, the tail)
};

struct TimedReadback {
    std::vector<TimedRow> rows;
    uint32_t unwritten = 0;
};

// A command buffer's marks [first, first + count) from its start stamp to its end stamp, each the time since the last mark
// the GPU wrote before it; a mark it did not write (a pass refused after its marks were taken) gives its time to the next
inline TimedReadback readTimedMarks(const volatile TimedMark *marks, const TimedLabel *labels, uint32_t first,
                                    uint32_t count, uint32_t serial, uint64_t startPtimer, uint64_t endPtimer)
{
    std::atomic_thread_fence(std::memory_order_acquire);
    TimedReadback out;
    uint64_t prev = startPtimer, counted = 0;
    bool counting = false;
    for (uint32_t i = first; i < first + count; i++) {
        const uint64_t at = marks[i].release.ptimer, invocations = marks[i].invocations;
        if (marks[i].release.payload != serial || at < prev || at > endPtimer) {
            out.unwritten++;
            continue;
        }
        TimedRow r{labels[i], ptimerNanoseconds(at - prev), -1};
        if (counting && invocations >= counted)
            r.invocations = int64_t(invocations - counted);
        counted = invocations;
        counting = true;
        prev = at;
        out.rows.push_back(r);
    }
    TimedRow tail;
    tail.label.kind = TimedKind::Tail;
    tail.ns = ptimerNanoseconds(endPtimer >= prev ? endPtimer - prev : 0);
    out.rows.push_back(tail);
    return out;
}

// "<kind><state>:<pass>:<topology>:<vertices>x<instances>:<box>:<invocations>:<ns>" for a draw,
// "<kind>:<pass>:<invocations>:<ns>" for the rest, '-' for what is not known
inline std::string timedEntry(const TimedRow &r)
{
    const TimedLabel &l = r.label;
    char invocations[24] = "-", pass[8] = "-", out[160];
    if (r.invocations >= 0)
        snprintf(invocations, sizeof invocations, "%lld", (long long)r.invocations);
    if (l.pass != kNoPass)
        snprintf(pass, sizeof pass, "%u", l.pass);
    const char kind = timedKindLetter(l.kind);
    if (l.kind != TimedKind::Draw && l.kind != TimedKind::Indirect) {
        snprintf(out, sizeof out, "%c:%s:%s:%llu", kind, pass, invocations, (unsigned long long)r.ns);
        return out;
    }
    char state[8] = "-", topology[8] = "-";
    if (l.state != kNoState)
        snprintf(state, sizeof state, "%u", l.state);
    if (l.topology != 0xff)
        snprintf(topology, sizeof topology, "%u", l.topology);
    snprintf(out, sizeof out, "%c%s:%s:%s:%ux%u:%u:%s:%llu", kind, state, pass, topology, l.vertices, l.instances,
             l.box, invocations, (unsigned long long)r.ns);
    return out;
}

inline std::string timedPassEntry(uint32_t index, const TimedPass &p)
{
    char out[64];
    snprintf(out, sizeof out, "%u:%u/%ux%u/%u%s%s", index, p.format, p.width, p.height, p.samples,
             p.blockLinear ? "/bl" : "", p.compressed ? "+comp" : "");
    return out;
}

inline std::string timedStateLine(uint32_t index, const TimedState &s)
{
    char out[256];
    snprintf(out, sizeof out, "state %u %016llx %s fs%u vs%u r%u x%u %s", index, (unsigned long long)s.key,
             spanFormName(SpanForm(s.form)), s.fragmentWords, s.vertexWords, s.gprs, s.samplePasses,
             s.name && s.name[0] ? s.name : "-");
    return out;
}

// A capture's lines, each "c<serial>.<n> " ahead of its text, numbered so a reader tells a lost line from none
struct TimedLog {
    uint32_t serial = 0, next = 0;
    std::vector<std::string> lines;

    void add(const std::string &text)
    {
        lines.push_back("c" + std::to_string(serial) + "." + std::to_string(next++) + " " + text);
    }

    // `head` and then as many of `entries` as fit a line, line after line
    void packed(const std::string &head, const std::vector<std::string> &entries)
    {
        std::string line = head;
        for (const std::string &e : entries) {
            if (line.size() > head.size() && line.size() + 1 + e.size() > kTimedLineBytes) {
                add(line);
                line = head;
            }
            line += " " + e;
        }
        if (line.size() > head.size())
            add(line);
    }
};

// What HW Test reads back (SwitchMetalDevice switchMetalFrameWeightsTaken): the capture's command buffers and every mark of them
struct DrawTimesTaken {
    struct Buffer {
        uint32_t index = 0, stamp = 0, marks = 0, unwritten = 0, full = 0;
        uint64_t gpuNs = 0;
    };

    struct Row {
        uint32_t buffer = 0;
        TimedRow row;
        TimedState state;
        TimedPass pass;
    };

    uint32_t serial = 0;
    bool ended = false;
    std::vector<int64_t> reference;  // the GPU time of each command buffer before the capture, -1: unknown
    std::vector<Buffer> buffers;
    std::vector<Row> rows;
};

#endif

}  // namespace gm20b
