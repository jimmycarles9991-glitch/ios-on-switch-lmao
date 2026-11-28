// SPDX-License-Identifier: MIT
// DEVELOPMENT: a frame-weights capture's bookkeeping (draw_times.h). Any thread asks for one (two atomics). The device's
// encoding thread, one command buffer at a time, takes a command buffer at begin(), labels its marks with no lock (a label
// belongs to the one open command buffer) and hands it over at commit. A completer reads back the committed ones the GPU
// ran. A mutex orders a command buffer's labels before its read-back on the completer's thread: it is taken at the two
// hand-overs, at begin() only while a capture is asked for or open, by a completer only while one is committed, and never
// at a draw.
#pragma once
#include "draw_times.h"

#if SWITCH_METAL_DIAGNOSTICS
namespace gm20b {

class DrawTimes {
public:
    // Any thread: the next `buffers` command buffers the device begins make a new capture
    void ask(uint32_t buffers)
    {
        asked_.store(std::min(buffers, kTimedBuffers), std::memory_order_relaxed);
        askSerial_.fetch_add(1, std::memory_order_release);
    }

    // The encoder, at each begin(): whether begin(stamp) has anything to do
    bool asking() const
    {
        return askSerial_.load(std::memory_order_acquire) != begun_ || left_ || open_;
    }

    // The encoder: the command buffer it begins, a slot of the capture while it takes command buffers. One the command
    // buffer before left open was abandoned: the device encodes one at a time.
    TimedBuffer *begin(uint32_t stamp)
    {
        std::lock_guard<gm20b::UnfairMutex> l(lock_);
        if (open_) {
            open_->state = TimedBuffer::State::Dropped;
            open_ = nullptr;
        }
        if (const uint32_t ask = askSerial_.load(std::memory_order_acquire); ask != begun_) {
            begun_ = ask;
            restart(asked_.load(std::memory_order_relaxed), stamp);
        }
        if (!left_ || taken_ == kTimedBuffers || marks_ == kTimedMarks) {
            left_ = 0;
            return nullptr;
        }
        TimedBuffer &b = buffers_[taken_];
        b = TimedBuffer{};
        b.index = taken_++;
        b.stamp = stamp;
        b.firstMark = marks_;
        b.firstPass = passes_;
        left_--;
        open_ = &b;
        return &b;
    }

    // The encoder: the ask taken with no capture made of it
    void cancel()
    {
        std::lock_guard<gm20b::UnfairMutex> l(lock_);
        begun_ = askSerial_.load(std::memory_order_acquire);
        left_ = 0;
    }

    // The encoder: the payload of the capture's marks
    uint32_t serial() const
    {
        return serial_;
    }

    uint32_t reserve(TimedBuffer &b, const TimedLabel &label)
    {
        if (marks_ == kTimedMarks) {
            b.full++;
            return kNoMark;
        }
        labels_[marks_] = label;
        b.marks++;
        return marks_++;
    }

    uint16_t pass(TimedBuffer &b, const TimedPass &p)
    {
        if (passes_ == kTimedPasses)
            return kNoPass;
        passTable_[passes_] = p;
        b.passes++;
        return uint16_t(passes_++);
    }

    // A drawn program's index, `describe()` making it the first time the capture meets `identity` (its pipeline)
    template <class Describe> uint16_t state(const void *identity, Describe &&describe)
    {
        size_t at = (uintptr_t(identity) >> 4) & (kIndexSlots - 1);
        for (size_t probe = 0; probe < kIndexSlots; probe++, at = (at + 1) & (kIndexSlots - 1)) {
            Index &slot = index_[at];
            if (slot.identity == identity)
                return slot.state;
            if (slot.identity)
                continue;
            if (states_ == kTimedStates)
                return kNoState;
            slot = {identity, uint16_t(states_)};
            stateTable_[states_] = describe();
            return uint16_t(states_++);
        }
        return kNoState;
    }

    // The encoder, once the command buffer was submitted as `seqno` on the channel of `generation`
    void commit(TimedBuffer &b, uint32_t seqno, uint64_t generation)
    {
        std::lock_guard<gm20b::UnfairMutex> l(lock_);
        if (open_ != &b)
            return;
        b.seqno = seqno;
        b.generation = generation;
        b.state = TimedBuffer::State::Committed;
        open_ = nullptr;
        committed_.fetch_add(1, std::memory_order_release);
    }

    bool committed() const
    {
        return committed_.load(std::memory_order_acquire) != 0;
    }

    // A completer: the lines of each committed command buffer the GPU ran, in order, then the capture's end once every one it
    // took is read or dropped. `ran(seqno, generation)`: 0 not yet, 1 ran, -1 lost with its channel; `stamps(stamp, start,
    // end)`: its stamps' PTIMER values, false once a later command buffer took their slot; `gpuNs(stamp)`: an earlier
    // command buffer's GPU time, -1 where it is gone.
    template <class Ran, class Stamps, class GpuNs>
    std::vector<std::string> report(const volatile TimedMark *marks, const char *process, Ran &&ran, Stamps &&stamps,
                                    GpuNs &&gpuNs)
    {
        std::lock_guard<gm20b::UnfairMutex> l(lock_);
        TimedLog log{serial_, lines_, {}};
        for (uint32_t i = 0; i < taken_; i++) {
            TimedBuffer &b = buffers_[i];
            if (b.state != TimedBuffer::State::Committed)
                continue;
            const int done = ran(b.seqno, b.generation);
            if (!done)
                break;
            committed_.fetch_sub(1, std::memory_order_relaxed);
            if (!headed_)
                head(log, process, gpuNs);
            uint64_t start = 0, end = 0;
            if (done < 0 || !stamps(b.stamp, start, end)) {
                b.state = TimedBuffer::State::Dropped;
                log.add("dropped b" + std::to_string(b.index) + (done < 0 ? " lost" : " stamps-gone"));
                continue;
            }
            lines(log, b, readTimedMarks(marks, labels_.get(), b.firstMark, b.marks, serial_, start, end), start, end);
            b.state = TimedBuffer::State::Reported;
        }
        if (headed_ && !ended_ && !left_ && !open_)
            finish(log);
        lines_ = log.next;
        return std::move(log.lines);
    }

    DrawTimesTaken taken()
    {
        std::lock_guard<gm20b::UnfairMutex> l(lock_);
        return result_;
    }

private:
    struct Index {
        const void *identity = nullptr;
        uint16_t state = 0;
    };

    static constexpr size_t kIndexSlots = 2 * kTimedStates;

    // lock_ held
    void restart(uint32_t asked, uint32_t stamp)
    {
        superseded_ = serial_ && !ended_ ? serial_ : 0;
        serial_ = serial_ + 1 ? serial_ + 1 : 1;
        askedCount_ = left_ = asked;
        firstStamp_ = stamp;
        taken_ = marks_ = passes_ = states_ = lines_ = 0;
        headed_ = ended_ = false;
        committed_.store(0, std::memory_order_relaxed);
        if (!labels_) {
            labels_.reset(new TimedLabel[kTimedMarks]);
            stateTable_.reset(new TimedState[kTimedStates]);
            passTable_.reset(new TimedPass[kTimedPasses]);
            index_.reset(new Index[kIndexSlots]);
            printed_.reset(new bool[kTimedStates]);
        }
        std::fill(index_.get(), index_.get() + kIndexSlots, Index{});
        std::fill(printed_.get(), printed_.get() + kTimedStates, false);
        result_ = DrawTimesTaken{};
        result_.serial = serial_;
    }

    template <class GpuNs> void head(TimedLog &log, const char *process, GpuNs &&gpuNs)
    {
        std::string text = std::string("capture ") + (process ? process : "-") + " asked " +
                           std::to_string(askedCount_) + " stamp " + std::to_string(firstStamp_) + " ref";
        for (uint32_t k = askedCount_; k >= 1; k--) {
            const int64_t ns = firstStamp_ > k ? gpuNs(firstStamp_ - k) : -1;
            result_.reference.push_back(ns);
            text += ns < 0 ? std::string(" -") : " " + std::to_string(ns);
        }
        if (superseded_)
            text += " supersedes c" + std::to_string(superseded_);
        log.add(text);
        headed_ = true;
    }

    // The command buffer's line (its start in PTIMER units, which places it among the frames), its passes, the states it
    // drew that the capture has not named yet, and its marks
    void lines(TimedLog &log, const TimedBuffer &b, const TimedReadback &rb, uint64_t startPtimer, uint64_t endPtimer)
    {
        const std::string at = "b" + std::to_string(b.index);
        const uint64_t gpuNs = ptimerNanoseconds(endPtimer - startPtimer);
        log.add("cb " + at + " s" + std::to_string(b.stamp) + " start " + std::to_string(startPtimer) + " gpu " +
                std::to_string(gpuNs) + " marks " + std::to_string(b.marks) + " unwritten " +
                std::to_string(rb.unwritten) + " full " + std::to_string(b.full));
        result_.buffers.push_back({b.index, b.stamp, b.marks, rb.unwritten, b.full, gpuNs});
        std::vector<std::string> entries;
        for (uint32_t q = b.firstPass; q < b.firstPass + b.passes; q++)
            entries.push_back(timedPassEntry(q, passTable_[q]));
        log.packed("passes " + at, entries);
        entries.clear();
        for (const TimedRow &r : rb.rows) {
            DrawTimesTaken::Row taken{b.index, r, {}, {}};
            if (r.label.state != kNoState) {
                taken.state = stateTable_[r.label.state];
                if (!printed_[r.label.state])
                    log.add(timedStateLine(r.label.state, taken.state));
                printed_[r.label.state] = true;
            }
            if (r.label.pass != kNoPass)
                taken.pass = passTable_[r.label.pass];
            result_.rows.push_back(taken);
            entries.push_back(timedEntry(r));
        }
        log.packed("d " + at, entries);
    }

    void finish(TimedLog &log)
    {
        uint32_t reported = 0, dropped = 0;
        for (uint32_t i = 0; i < taken_; i++) {
            if (buffers_[i].state == TimedBuffer::State::Committed)
                return;
            reported += buffers_[i].state == TimedBuffer::State::Reported;
            dropped += buffers_[i].state == TimedBuffer::State::Dropped;
        }
        log.add("end taken " + std::to_string(taken_) + " reported " + std::to_string(reported) + " dropped " +
                std::to_string(dropped) + " lines " + std::to_string(log.next + 1));
        ended_ = result_.ended = true;
    }

    gm20b::UnfairMutex lock_;
    std::atomic<uint32_t> askSerial_{0}, asked_{0}, committed_{0};
    uint32_t begun_ = 0, left_ = 0, serial_ = 0, askedCount_ = 0, superseded_ = 0, firstStamp_ = 0;
    uint32_t taken_ = 0, marks_ = 0, passes_ = 0, states_ = 0, lines_ = 0;
    bool headed_ = false, ended_ = false;
    TimedBuffer *open_ = nullptr;
    TimedBuffer buffers_[kTimedBuffers];
    std::unique_ptr<TimedLabel[]> labels_;
    std::unique_ptr<TimedState[]> stateTable_;
    std::unique_ptr<TimedPass[]> passTable_;
    std::unique_ptr<Index[]> index_;
    std::unique_ptr<bool[]> printed_;
    DrawTimesTaken result_;
};

}  // namespace gm20b
#endif
