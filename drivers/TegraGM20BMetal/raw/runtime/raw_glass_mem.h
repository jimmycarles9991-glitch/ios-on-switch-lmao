// SPDX-License-Identifier: MIT
// DEVELOPMENT: the render server's blur-texture probe (Device::glassMem, raw_gpu.cpp). Each Core Animation command buffer
// names the textures its draws sampled that this process rendered or copied into: rebuilt (this command buffer wrote them)
// or reused (an earlier one did: its age in command buffers and the GPU-memory events since, Device::glassMemEvent), and
// what one row of each read at the command buffer's end, by the copy engine into a ring the CPU reads once the fence passed:
// black, non-finite, or for a reused one changed since the end of the command buffer that last wrote it. A texture header
// whose 32 bytes changed under the same index while its texture lived counts as moved. The ring is GPU-volatile control
// memory (an internal job's writes may sit in the L2 past its fence otherwise). The CPU fills a slot's rows with
// kGlassMemPoison before its submission, and the slot ends with a host semaphore release of its command buffer's cookie:
// a slot without the cookie (its segment never ran) or a row still poisoned (its copy never landed) is not judged.
// For a texture rebuilt in the command buffer, what its passes reached is kept (the first pass's load, the box its draws
// covered) and a second row is read outside that box: memory nothing of this command buffer wrote. A reused texture's row
// is read where its last write's command buffer read it, so that the two compare. In the owner's watch a zero row of a
// float texture a glass draw sampled is a line too, once in kGlassMemZeroEvery command buffers a texture: Settings' dark
// backdrop blurs to zero, and every such row would fill the log. A float texture a glass draw sampled is read at two more
// rows, a quarter and three quarters down it, and a zero row says whether its alpha is zero too (zero.a0) or not (zero.a),
// so a mask that keeps its data in alpha is told from an empty one. A compressed texture whose kind the copy engine reads
// as plain texels (raw_comp.h copyEngineReadsCompressed) is read through its own binding (+comp).
// It runs only while armed (GlassMemArming): disarmed, the frame path pays one relaxed load and nothing else.
#pragma once
#include "raw_lock.h"
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include "raw_glass_mem_row.h"

namespace raw {

constexpr uint32_t kGlassMemTexels = 32, kGlassMemItems = 16, kGlassMemSlots = 8, kGlassMemRows = 4,
                   kGlassMemReaches = 32;
constexpr uint32_t kGlassMemRowBytes = kGlassMemTexels * 16, kGlassMemItemBytes = kGlassMemRows * kGlassMemRowBytes;
// The slot's end: the copies' completion release (its cookie) and, 16 bytes on, the release before them: their GPU time
constexpr uint32_t kGlassMemCookieBytes = 32, kGlassMemStartAt = 16;
constexpr uint32_t kGlassMemSlotBytes = kGlassMemItems * kGlassMemItemBytes + kGlassMemCookieBytes;
constexpr uint64_t kGlassMemArmMs = 20000, kGlassMemWatchMs = 300000;
constexpr uint8_t kGlassMemPoison = 0xa5;
constexpr size_t kGlassMemLine = 900;  // os_log keeps about 1 KiB of a line
// Entries kept: a texture CA dropped without our seeing its destroy (a client's) ages out past this many
constexpr size_t kGlassMemTracked = 512;
constexpr uint64_t kGlassMemZeroEvery = 60;

enum : uint8_t {
    kGlassMemHeap = 1,       // an MTLHeap's (an arena: a purge takes its pages)
    kGlassMemPitch = 2,      // pitch-linear
    kGlassMemUnsampled = 4,  // no row taken (compressed, memoryless, multisampled, converted)
    kGlassMemAliased = 8,    // an MTLHeap texture made aliasable by the command buffer's end: its rows may be another's
    kGlassMemCompressed = 16,  // compressed memory, read through its compressible binding (the L2 decompresses)
};

struct GlassMemCb {
    uint64_t serial = 0, mach = 0, events = 0;
    uint32_t seqno = 0, count = 0, dropped = 0, reaches = 0, launches = 0;
    GlassMemItem items[kGlassMemItems];
    std::pair<uint64_t, GlassMemReach> reach[kGlassMemReaches];
};

// The value a command buffer's slot's semaphore release writes: never 0, which a slot holds before its release lands
inline uint32_t glassMemCookie(uint64_t serial)
{
    return uint32_t(serial % 0xffffffffu) + 1;
}

inline bool glassMemGlassState(const char *state)
{
    return state[0] == 'T' && state[1] == 'g';
}

struct GlassMemCounts {
    std::atomic<uint64_t> cbs{0}, rebuilt{0}, reused{0}, changed{0}, nonFinite{0}, moved{0}, black{0};
    std::atomic<uint64_t> launches{0}, probeNs{0};  // the probe's own copy-engine launches and GPU time
};

// The "T" term of a Core Animation state's name (PBGRAXm_TvrdA2Xhf_Isrc: Tvrd), four letters at most
inline void glassMemState(const char *caName, char out[5])
{
    out[0] = '-', out[1] = 0;
    const char *t = caName ? strstr(caName, "_T") : nullptr;
    if (!t)
        return;
    size_t n = 0;
    for (const char *c = t + 1; *c && n < 4 && (n == 0 || (*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9')); c++)
        out[n++] = *c;
    out[n] = 0;
}

// Who armed the probe and until when (mach time): HW Test's glass tests (`test`: every command buffer's line) for their
// own run, or the owner from HW Test's main screen (lines that found something, and the totals)
class GlassMemArming {
public:
    void arm(bool test, uint64_t until)
    {
        (test ? test_ : watch_).store(until, std::memory_order_relaxed);
    }

    void disarm(bool test)
    {
        (test ? test_ : watch_).store(0, std::memory_order_relaxed);
    }

    bool on(uint64_t now) const
    {
        return now < test_.load(std::memory_order_relaxed) || now < watch_.load(std::memory_order_relaxed);
    }

    bool every(uint64_t now) const
    {
        return now < test_.load(std::memory_order_relaxed);
    }

    uint64_t until() const
    {
        return std::max(test_.load(std::memory_order_relaxed), watch_.load(std::memory_order_relaxed));
    }

private:
    std::atomic<uint64_t> test_{0}, watch_{0};
};

// What a command buffer's end does for the probe: no ring and no copy unless it is armed, then a launch for each row
struct GlassMemTailPlan {
    bool ring = false;
    uint32_t launches = 0;
};

inline GlassMemTailPlan glassMemTailPlan(bool armed, const GlassMemCb &cb)
{
    GlassMemTailPlan plan;
    for (uint32_t i = 0; armed && i < cb.count; i++)
        if (!(cb.items[i].flags & kGlassMemUnsampled))
            plan.launches += 1 + cb.items[i].outside + 2 * cb.items[i].quarters;
    plan.ring = plan.launches > 0;
    return plan;
}

class GlassMem {
public:
    // A write into the memory at `key` by command buffer `serial`, with `events` memory events so far
    void wrote(uint64_t key, uint64_t serial, uint64_t events)
    {
        std::lock_guard<raw::Mutex> l(lock_);
        Write &w = writes_[key];
        if (!w.first)
            w.first = serial;
        w.serial = serial;
        w.events = events;
        trim();
    }

    // A pass of command buffer `serial` into the image at `image` ended: what it reached (GlassMemReach)
    void reached(uint64_t image, const GlassMemReach &pass, uint64_t serial)
    {
        const uint32_t x0 = pass.x0, y0 = pass.y0, x1 = pass.x1, y1 = pass.y1;
        std::lock_guard<raw::Mutex> l(lock_);
        open_.serial = serial;
        for (uint32_t i = 0; i < open_.reaches; i++)
            if (open_.reach[i].first == image) {
                GlassMemReach &r = open_.reach[i].second;
                r.x0 = std::min(r.x0, x0), r.y0 = std::min(r.y0, y0);
                r.x1 = std::max(r.x1, x1), r.y1 = std::max(r.y1, y1);
                return;
            }
        if (open_.reaches < kGlassMemReaches)
            open_.reach[open_.reaches++] = {image, pass};
    }

    // A draw of state `caName` sampled `item`'s texture in command buffer `serial`, header index `tic` holding bytes of
    // hash `header`: its row's index in this command buffer, or -1 (not one this process wrote, already taken, or no room)
    int sampled(const GlassMemItem &item, const char *caName, uint32_t tic, uint64_t header, uint64_t serial)
    {
        std::lock_guard<raw::Mutex> l(lock_);
        open_.serial = serial;
        auto w = writes_.find(item.key);
        if (w == writes_.end() || !w->second.serial)
            return -1;
        bool moved = false;
        if (tic) {
            Header &h = headers_[tic];
            moved = h.key == item.key && h.hash != header;
            h = {item.key, header};
        }
        char state[5];
        glassMemState(caName, state);
        const bool glass = glassMemGlassState(state);
        for (uint32_t i = 0; i < open_.count; i++)
            if (open_.items[i].key == item.key && open_.items[i].level == item.level) {
                open_.items[i].moved |= moved;
                open_.items[i].glass |= glass;
                return -1;
            }
        uint32_t at = open_.count;
        if (at == kGlassMemItems) {
            // the glass draws sample last: one takes the place of the latest item no glass draw sampled
            open_.dropped++;
            if (!glass)
                return -1;
            while (at > 0 && glassMemGlassState(open_.items[at - 1].state))
                at--;
            if (!at)
                return -1;
            at--;
        } else {
            open_.count++;
        }
        GlassMemItem &it = open_.items[at];
        it = item;
        it.moved = moved;
        it.glass = glass;
        memcpy(it.state, state, sizeof state);
        return int(at);
    }

    // A command buffer is begun: what an abandoned one sampled is no record's
    void begin()
    {
        std::lock_guard<raw::Mutex> l(lock_);
        open_ = GlassMemCb();
    }

    // Armed again: what was written while disarmed went unseen, so no texture's history holds
    void reset()
    {
        std::lock_guard<raw::Mutex> l(lock_);
        writes_.clear();
        refs_.clear();
        headers_.clear();
        open_ = GlassMemCb();
    }

    // `key` died (a view's header `tic` alone, when `tic`)
    void forget(uint64_t key, uint32_t tic = 0)
    {
        std::lock_guard<raw::Mutex> l(lock_);
        if (tic) {
            headers_.erase(tic);
            return;
        }
        for (uint32_t i = 0; i < open_.count; i++)
            if (open_.items[i].key == key)
                open_.items[i].flags |= kGlassMemUnsampled;
        writes_.erase(key);
        for (auto it = refs_.begin(); it != refs_.end();)
            it = it->first - key < 16 ? refs_.erase(it) : std::next(it);
        for (auto it = headers_.begin(); it != headers_.end();)
            it = it->second.key == key ? headers_.erase(it) : std::next(it);
    }

    // Command buffer `serial`'s record, closed at its commit: rebuilt or reused is known now that every write was seen,
    // and so where each item's rows are read
    GlassMemCb close(uint64_t serial, uint64_t mach, uint64_t events)
    {
        std::lock_guard<raw::Mutex> l(lock_);
        GlassMemCb cb = open_.serial == serial ? open_ : GlassMemCb();
        open_ = GlassMemCb();
        cb.serial = serial;
        cb.mach = mach;
        cb.events = events;
        for (uint32_t i = 0; i < cb.count; i++) {
            GlassMemItem &it = cb.items[i];
            const auto found = writes_.find(it.key);
            const Write w = found == writes_.end() ? Write() : found->second;
            it.rebuilt = w.serial == serial;
            it.fresh = w.first == serial;
            it.age = serial - w.serial;
            it.events = events - w.events;
            for (uint32_t r = 0; it.rebuilt && r < cb.reaches; r++)
                if (cb.reach[r].first == it.image)
                    it.reach = cb.reach[r].second;
            Ref &ref = refs_[it.key + it.level];
            if (it.rebuilt || !ref.placed) {
                glassMemRowsAt(it);
                if (it.rebuilt)
                    ref.inX = it.inX, ref.inY = it.inY, ref.placed = true;
            } else {
                it.inX = ref.inX, it.inY = ref.inY;
            }
            it.quarters = it.glass && floatComponentBytes(it.format) && it.height >= 4;
        }
        return cb;
    }

    // What `cb`'s rows read (`rows`: kGlassMemItemBytes an item, then the cookie): its lines (os_log's length each), none
    // when there is nothing to say and not `every`. Rows whose slot lacks the cookie are no read: '?', and count nothing.
    // `probeNs`: the GPU time of the probe's own copies, which each line names with their launches.
    std::vector<std::string> finish(const GlassMemCb &cb, const uint8_t *rows, bool every, uint64_t probeNs = 0)
    {
        std::lock_guard<raw::Mutex> l(lock_);
        uint32_t cookie;
        memcpy(&cookie, rows + uint64_t(kGlassMemItems) * kGlassMemItemBytes, sizeof cookie);
        const bool landed = cookie == glassMemCookie(cb.serial);
        if (landed) {
            counts.cbs++;
            counts.launches += cb.launches;
            counts.probeNs += probeNs;
        }
        char head[192];
        int at = snprintf(head, sizeof head, "cb %llu mach %llu seq %u ev %llu n %u ce %u gpu %lluus",
                          (unsigned long long)cb.serial, (unsigned long long)cb.mach, cb.seqno,
                          (unsigned long long)cb.events, cb.count, cb.launches, (unsigned long long)(probeNs / 1000));
        if (cb.dropped)
            snprintf(head + at, sizeof head - size_t(at), " drop %u", cb.dropped);
        std::vector<std::string> lines{std::string(head) + " |"};
        bool notable = !landed;
        for (uint32_t i = 0; i < cb.count; i++) {
            const GlassMemItem &it = cb.items[i];
            const uint8_t *row = rows + uint64_t(i) * kGlassMemItemBytes;
            const std::string verdict = landed ? verdictOf(it, row, cb.serial, notable) : "?";
            char levels[12] = "", level[8] = "", events[16] = "", reach[96] = "";
            if (it.levels > 1)
                snprintf(levels, sizeof levels, "/L%u", it.levels);
            if (it.level)
                snprintf(level, sizeof level, ".%u", it.level);
            if (!it.rebuilt && it.events)
                snprintf(events, sizeof events, "/e%llu", (unsigned long long)it.events);
            if (it.reach.load)
                snprintf(reach, sizeof reach, "/%c%u,%u-%u,%u/a%ux%u/k%ux%u", it.reach.load, it.reach.x0, it.reach.y0,
                         it.reach.x1, it.reach.y1, it.reach.areaW, it.reach.areaH, it.reach.clearW, it.reach.clearH);
            char item[256];
            snprintf(item, sizeof item, " %04llx%s:%ux%u/f%u%s/%s%s/%s/%c%llu%s%s%s%s%s:%s",
                     (unsigned long long)(it.key >> 14 & 0xffff), level, it.width, it.height, it.format, levels,
                     it.flags & kGlassMemPitch ? "pitch" : "bl",
                     it.flags & kGlassMemCompressed ? "+comp"
                     : it.flags & kGlassMemHeap     ? "+heap"
                                                    : "",
                     it.state, it.rebuilt ? 'R' : 'U', (unsigned long long)(it.rebuilt ? 0 : it.age),
                     it.fresh ? "/n" : "", reach, events, it.moved ? "/mv" : "",
                     it.flags & kGlassMemAliased ? "/al" : "", verdict.c_str());
            if (lines.back().size() + strlen(item) > kGlassMemLine)
                lines.push_back(std::string(head) + " +|");
            lines.back() += item;
        }
        if (!notable && !every)
            lines.clear();
        return lines;
    }

    GlassMemCounts counts;

private:
    struct Write {
        uint64_t serial = 0, events = 0, first = 0;
    };

    // The row read at its last write's end, where it was read (placed: by a command buffer that rebuilt it) and the last
    // command buffer whose zero row made a line
    struct Ref {
        uint64_t serial = 0, hash = 0, zeroLine = 0;
        uint32_t inX = 0, inY = 0;
        bool black = false, placed = false;
    };

    struct Header {
        uint64_t key = 0, hash = 0;
    };

    bool poisoned(const GlassMemItem &it, const uint8_t *row) const
    {
        for (uint32_t b = 0; b < it.texels * it.bpp; b++)
            if (row[b] != kGlassMemPoison)
                return false;
        return true;
    }

    // The inside row's verdict, then the outside row's after a comma (no reference: it was never meant to be written), then
    // the quarter rows' where any of the three is not ok
    std::string verdictOf(const GlassMemItem &it, const uint8_t *row, uint64_t serial, bool &notable)
    {
        std::string v = insideVerdict(it, row, serial, notable);
        if (it.flags & kGlassMemUnsampled)
            return it.unread ? v + it.unread : v;
        if (it.quarters && !poisoned(it, row)) {
            const GlassMemRow in = glassMemRow(row, it.texels, it.bpp, it.format);
            const uint8_t *q1 = row + 2 * kGlassMemRowBytes, *q3 = row + 3 * kGlassMemRowBytes;
            const GlassMemRow a = glassMemRow(q1, it.texels, it.bpp, it.format),
                              b = glassMemRow(q3, it.texels, it.bpp, it.format);
            if (in.black)
                v += in.alphaZero ? ".a0" : ".a";
            if (in.black || a.black || b.black || a.nonFinite || b.nonFinite)
                v += std::string(",q1:") + (poisoned(it, q1) ? "?" : a.verdict()) +
                     ",q3:" + (poisoned(it, q3) ? "?" : b.verdict());
        }
        if (!it.outside)
            return v;
        const uint8_t *out = row + kGlassMemRowBytes;
        if (poisoned(it, out))
            return v + ",out:?";
        const GlassMemRow r = glassMemRow(out, it.texels, it.bpp, it.format);
        if (r.nonFinite) {
            counts.nonFinite++;
            notable = true;
        }
        char at[40];
        snprintf(at, sizeof at, ",out@%u,%u:%s", it.outX, it.outY, r.nonFinite ? "nf" : r.black ? "zero" : "ok");
        return v + at;
    }

    const char *insideVerdict(const GlassMemItem &it, const uint8_t *row, uint64_t serial, bool &notable)
    {
        (it.rebuilt ? counts.rebuilt : counts.reused)++;
        if (it.moved) {
            counts.moved++;
            notable = true;
        }
        if (it.flags & kGlassMemUnsampled)
            return "-";
        if (poisoned(it, row))
            return "?";
        const GlassMemRow r = glassMemRow(row, it.texels, it.bpp, it.format);
        Ref &ref = refs_[it.key + it.level];
        if (r.nonFinite) {
            counts.nonFinite++;
            notable = true;
        }
        if (r.black && it.glass && floatComponentBytes(it.format) &&
            (!ref.zeroLine || serial - ref.zeroLine >= kGlassMemZeroEvery)) {
            ref.zeroLine = serial;
            notable = true;
        }
        if (it.rebuilt) {
            ref.serial = serial, ref.hash = r.hash, ref.black = r.black;
            return r.nonFinite ? "nf" : r.black ? "zero" : "ok";
        }
        if (r.black)
            counts.black++;
        const bool referenced = ref.serial && ref.serial == serial - it.age;
        if (referenced && ref.hash != r.hash) {
            counts.changed++;
            notable = true;
            return r.nonFinite ? "nfchg" : r.black ? "zchg" : "chg";
        }
        if (r.nonFinite)
            return "nf";
        if (r.black)
            return referenced ? "zero" : "zero?";
        return referenced ? "ok" : "ok?";
    }

    void trim()
    {
        if (writes_.size() <= kGlassMemTracked)
            return;
        auto oldest = writes_.begin();
        for (auto it = writes_.begin(); it != writes_.end(); ++it)
            if (it->second.serial < oldest->second.serial)
                oldest = it;
        const uint64_t key = oldest->first;
        writes_.erase(oldest);
        for (auto it = refs_.begin(); it != refs_.end();)
            it = it->first - key < 16 ? refs_.erase(it) : std::next(it);
    }

    raw::Mutex lock_;
    std::unordered_map<uint64_t, Write> writes_;
    std::unordered_map<uint64_t, Ref> refs_;        // by key plus level: the row read at its last write's end
    std::unordered_map<uint32_t, Header> headers_;  // by header index: whose it is and its bytes' hash
    GlassMemCb open_;
};

// The notify words HW Test reads (hw_test_app app27 ui.glass_sidebar): the probe's counts, and the pool's shape
inline uint64_t glassMemWord(const GlassMemCounts &c)
{
    const auto sat = [](uint64_t v, unsigned bits) {
        return std::min<uint64_t>(v, (1ull << bits) - 1);
    };
    return (c.cbs.load() & 0xfffff) | sat(c.changed.load(), 12) << 20 | sat(c.nonFinite.load(), 12) << 32 |
           sat(c.moved.load(), 12) << 44 | sat(c.black.load(), 8) << 56;
}

inline uint64_t glassMemPoolWord(uint64_t poolMB, uint64_t chunks, uint64_t backings, uint64_t backingMB,
                                 uint64_t compHeld, uint64_t compMade)
{
    const auto sat = [](uint64_t v, unsigned bits) {
        return std::min<uint64_t>(v, (1ull << bits) - 1);
    };
    return sat(poolMB, 12) | sat(chunks, 8) << 12 | sat(backings, 8) << 20 | sat(backingMB, 12) << 28 |
           sat(compHeld, 8) << 40 | (compMade & 0xffff) << 48;
}

}  // namespace raw
