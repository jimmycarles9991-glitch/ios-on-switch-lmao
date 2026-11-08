// SPDX-License-Identifier: BSD-2-Clause-Patent
// r14-sdcq: the SD command queue (SD Physical Layer 6.00, 4.19) as the SD driver (NativeSwitchBlock.cpp) drives it: its
// state and the order of its commands, apart from the controller registers (the `Bus`), so the host can run it against a
// model card that enforces the spec's rules (tests/native_sd_cq_harness.cpp).
//
// Sequential CQ mode, reads only. In Voluntary mode the card marks tasks ready in its Task Status register and the host
// has to read it with CMD13 until one is (4.19.6: "at least one task ready within 250 ms"; nothing signals it), so an
// idle queue would need a timer. In Sequential mode (Performance Enhancement byte 262 = 03h) the host assigns a group of
// tasks at once from Task ID 0 with an empty queue (CMD44 then CMD45 for each), then executes them with CMD46 in Task ID
// order "without polling for ready state" (4.19.1.2): the card starts each one's data when it has it (within 100 ms,
// 4.19.7) and the controller's transfer-complete interrupt ends it. Every task's address reaches the card before the
// first CMD46, so it can fetch them while earlier ones move: the overlap a queue is for, with nothing polled.
//
// While CQ is enabled CMD17/18/24/25 are illegal (Table 4-74: only CMD0/12/13, CMD43-47, CMD32/33/38, CMD48/49/58/59,
// CMD6 and CMD19 are legal), so the driver leaves CQ mode before any legacy command (writes, lone reads after its queue
// ran dry, proofs) and when its queue runs dry. Every CMD49 is read back with CMD48. r18-sdcq: an error no longer keeps
// the queue off for the boot (r8c: "command queue off until reboot (CMD44 failed) after 16 tasks in 1 groups": those 16
// were setup's test group, so the first group of the data path failed and nothing went through the queue again). The
// group runner (`run`) recovers (`recover`: CMD13 while the card still holds the failed command's B-type status bits, then
// CMD43), retries the group once, reads it the legacy way if that fails too, and pauses the queue after kStrikes groups in
// a row with errors, longer each time; only a proof that differs (or a card that had to be power-cycled out of CQ mode
// kMaxStuck times) stops it for the boot (`fail`).
//
// r18-sdcq (r8d): r8d's card (the same SanDisk SR01T) failed the first CMD44 of every group assigned right after another
// group in the same session, and only those: groups = enters + errors at every stats line (839 = 406 + 433), each the
// same line, "CMD44 task 0 step 3 ... CMD13 status 0x00400900 (ILLEGAL) state 4, tasks 0x00000000; previous CMD46;
// CMD43 step 0", whatever the group's size (6 to 31 tasks). The card did not answer CMD44 (ILLEGAL_COMMAND in the next
// R1, clear condition B), so for it Task ID 0 was still taken (4.19.5: no response for "Task ID already exists") although
// every task had been executed and none showed ready; every group after a CMD43 (a session's opening one, or a
// recovery's) went through. 6.00 4.19.1.2 has the host empty the queue itself, "Queue shall be empty before assigning
// Tasks [Host may abort entire queue of tasks queued earlier]"; only 9.10 4.19.7 adds that a completed task's
// information "shall be cleared and the Task ID can be reused". So each group's assignment now starts with CMD43
// (abort the entire queue: no error on an empty one, 9.10 4.19.8), R1b, its busy waited out by interrupt, unless the
// queue was just emptied (a session's opening CMD43, a recovery's): one command per group, where r8d paid a CMD44
// timeout, two CMD13, a CMD43 and the group's assignment again. And a group whose retry went through the queue no
// longer counts toward a pause (r8d: 74 pauses, the queue skipped for 273k group starts, with every retry good): only
// groups read the legacy way do.
//
// r16-sdcq: r7z's card (SanDisk SR01T, 2024-11) took CMD49 03h with a clean R1 and its CMD48 read-back did not show 03h.
// What the engine did not do, against the spec and both Linux kernels that drive SD CQ: (1) the cache first. 4.19.1:
// "Cache Feature (Refer to Section 4.17) should be enabled before enabling Command Queue"; 4.16.2: A2 performance is
// measured with both; Linux's mmc_sd_init_card enables the cache (byte 260) before anything else, and the Raspberry Pi
// kernel (rpi-6.12.y sd.c sd_parse_ext_reg_perf) does not even count a queue as supported without the cache and Power
// Off Notification. (2) A lazy card: one read-back right after the CMD49's busy. The Raspberry Pi kernel re-reads the
// byte for 10 ms (sd_ops.c mmc_sd_cmdq_switch, commit 45fa95bc: "cards may have a lazy init/deinit sequence"; "make sure
// the register write actually stuck, as some cards claim support but never report back"). Now every control byte this
// engine writes is read back up to kReadBacks times, a settle() between (~10 ms in all): each read-back is a CMD48, a
// command waited for by interrupt; the settle is a sleep, not a spin. (3) Which bit did not take: Enable CQ (bit 0) and
// CQ Mode (bit 1) are separate (Table 5-30), so the read-back is kept (lastRead) for the driver's one line.
//
// The cache (byte 260) is on only inside a CQ session, because 4.19.1 asks for it there, never for writes: enter() turns
// it on right before 262 = 03h, and leaving CQ mode turns it off right after 262 = 00h. The queue carries reads only and
// every write is a legacy command, out of CQ mode, so every write finds the cache off and completes, as before, once the
// card's busy says it is programmed: nothing written is ever held in the cache, no synchronize has anything to flush, and
// a power cycle at any point loses no completed write. A cache that will not go off (cacheOn after a failed disable) is
// the one case left: every write is then flushed before it completes (commitWrite: byte 261 = 01h, the card busy on DAT0
// until done, then the bit reads 0: 4.17), write-through either way, and a flush that fails fails only that write. (SD has
// no per-write forced programming: CMD23's argument is all block count, 4.15 / Table 4-24; bit 24 is eMMC's.) Nothing
// here panics. The bytes this engine ever writes: 260 with 00h or 01h, 261 with 01h, 262 with 00h or 03h (Bus::control
// refuses anything else).
#pragma once

#include <stdint.h>

#include "SwitchSdCard.h"

struct SwitchCqTask {
    uint64_t lba;
    uint32_t sectors;
};

// r18-sdcq: one failed command of a CQ session and what the recovery read right after it, for the driver's line.
// `previous` is the command the card got before the failed one: an unanswered command's ILLEGAL_COMMAND / COM_CRC_ERROR
// (Table 4-42, clear condition B: "Always related to the previous command") shows in the next response, so a failed
// command's own R1 can blame the one before, and CMD13 right after a command that got no answer names it.
struct SwitchCqError {
    uint8_t command;   // 44/45 (assignment), 46 (a task's data), 49 (an enable), 48 (a read-back that never showed the
                       // byte: step -1), 43 (the abort that opens a session went unanswered)
    uint8_t task;      // its task id (CMD44/45/46)
    uint8_t previous;  // the command index the card got before it
    uint8_t value;     // the byte the last read-back showed (command 48)
    uint8_t attempt;   // 1 or 2 within its group
    uint8_t tasks;     // tasks in that attempt
    uint8_t outcome;   // SwitchCqEngine::Recovery
    int step;          // the bus's code; 0: answered with error bits in r1; -1: a read-back that never showed the value
    uint32_t r1;       // its response (0 when none came)
    uint32_t irq;      // the controller's interrupt status for it (the driver's; 0 on the host)
    int statusStep;    // CMD13 (card status) right after
    uint32_t cardStatus;
    int tasksStep;  // CMD13 with bit 15 (the Task Status Register, 4.10.3), only while in CQ mode; -1 not sent
    uint32_t taskStatus;
    int abortStep;  // CMD43 abort entire queue; -1 not sent
    uint32_t abortR1;
};

// Bus: int command(uint32_t index, uint32_t argument, bool busy, uint32_t *r1): CMD43 (busy) / CMD44 / CMD45, 0 or a
// nonzero controller error (no response, CRC, timeout; *r1 then holds a response if one came, else 0); int control(
// uint32_t byte, bool write, uint8_t *value, uint32_t *r1): CMD49 of *value into performance-function byte `byte` (260,
// 261 or 262) / CMD48 of that byte into *value, 0 or nonzero (a controller error or an R1 error; *r1 the response); void
// settle(): about a millisecond (only between read-backs of a byte the card has not shown the written value in yet, or
// probes of a queue the card has not opened yet); r18-sdcq: int status(bool tasks, uint32_t *r1): CMD13 (bit 15 with
// `tasks`: the Task Status Register in CQ mode, a stuff bit out of it), 0 or a controller error; uint8_t lastCommand():
// the index of the last command the card got, whoever sent it; uint32_t lastIrq(): the controller's interrupt status for
// the last command (0 if the bus has none).
template <typename Bus> struct SwitchCqEngine {
    enum State : uint8_t {
        kOff,
        kIdle,
        kOn
    };

    // r18-sdcq: what a recovery left (SwitchCqError::outcome): the card in CQ mode with its queue empty (a group may be
    // assigned at once), out of CQ mode (idle), or stuck in it (the driver power-cycles it).
    enum Recovery : uint8_t {
        kInPlace,
        kOut,
        kStuck
    };

    // r18-sdcq: a chain's proof (the driver's compareChain) and how a group ended.
    enum Proof : uint8_t {
        kProofMatches,
        kProofDiffers,
        kProofUnproven,
        kProofBroken
    };

    enum Outcome : uint8_t {
        kGroupQueued,
        kGroupRetried,
        kGroupLegacy,
        kGroupStopped
    };

    static constexpr uint32_t kR1Errors = 0xfdf90008u;  // the driver's R1_ERRORS
    // r18-sdcq: COM_CRC_ERROR and ILLEGAL_COMMAND, clear condition B: they describe the command before the one whose
    // response carries them (Table 4-42; 4.6.1: an illegal or CRC-failed command gets no response).
    static constexpr uint32_t kR1PreviousErrors = 0x00c00000u;
    static constexpr uint8_t kEnabled = kSdCqEnable | kSdCqSequential, kDisabled = 0;
    // Read-backs of a control byte before the card is taken not to have applied a write (a settle() before each but the
    // first, ~10 ms in all: the Raspberry Pi kernel's mmc_sd_cmdq_switch allows 10 ms for the CQ byte too). r18-sdcq: the
    // same bound for the CMD43 that opens a session.
    static constexpr unsigned kReadBacks = 10;
    static constexpr unsigned kMaxDepth = 32;
    // r18-sdcq: the backoff. kStrikes groups in a row read the legacy way (r8d: not those whose retry went through the
    // queue) pause the queue for pauseLength groups (the reads that would have started one go the legacy way, counted
    // per group, no clock), kPauseMin doubling to kPauseMax; kForgive groups in a row through the queue bring the pause
    // back to kPauseMin. kMaxStuck power cycles out of CQ mode stop it for the boot.
    static constexpr unsigned kStrikes = 3, kPauseMin = 16, kPauseMax = 4096, kForgive = 64, kMaxStuck = 3;
    State state = kOff;
    uint8_t depth = 0;
    unsigned queued = 0, next = 0;  // tasks assigned in the current group, and the next one to execute
    const char *why = nullptr;      // the latest failure's reason (r18-sdcq: the stop's, once stopped)
    bool stuck = false;             // the card would not leave CQ mode: only a power cycle gets it out
    bool stopped = false;           // r18-sdcq: off for the boot (fail): a proof differed, or kMaxStuck power cycles
    // r18-sdcq (r8d): the card's queue was just emptied by an answered CMD43 (a session's opening one, a recovery's), so
    // the next group is assigned without another; any group assigned clears it (its finished tasks may stay taken).
    bool emptied = false;
    // r16-sdcq: the card's cache. useCache: the card offers one and it has not refused it, so each CQ session turns it on
    // first. cacheOn: it may be enabled (set before the CMD49 that enables it; cleared only by a read-back that shows it
    // off, or by a power cycle), so a write must be flushed before it completes (commitWrite).
    bool useCache = false, cacheOn = false;
    // r16-sdcq: the last value each control byte read back (260, 261, 262) and how many read-backs the last check took;
    // what the last enables read back (the cleanup after a failure reads the bytes again), and in how many read-backs.
    uint8_t lastRead[3] = {};
    unsigned lastReads = 0;
    uint8_t cacheRead = 0, cqRead = 0;
    unsigned cacheReads = 0, cqReads = 0;
    uint64_t enters = 0, leaves = 0, groups = 0, tasks = 0, settles = 0, flushes = 0;
    // r18-sdcq: the last error and the recovery's counts: errors recovered, groups whose retry went through the queue,
    // groups read the legacy way after errors, groups skipped by a pause, pauses, power cycles out of CQ mode, CMD43
    // probes that opening a session took beyond the first.
    SwitchCqError error = {};
    uint64_t recoveries = 0, retried = 0, legacyGroups = 0, skipped = 0, pauses = 0, powerCycles = 0, probes = 0;
    uint64_t groupAborts = 0;  // r18-sdcq (r8d): CMD43s that emptied the queue between two groups of a session
    unsigned strikes = 0, clean = 0, pauseLeft = 0, pauseLength = kPauseMin;
    bool pausedNow = false;  // the last group started a pause (for the driver's line)

    // The card offers a queue of `queueDepth` (2..32) and the driver may use it; r16-sdcq: `cache`, it offers a cache too.
    void start(uint8_t queueDepth, bool cache)
    {
        depth = queueDepth > kMaxDepth ? kMaxDepth : queueDepth;
        state = depth >= 2 ? kIdle : kOff;
        useCache = cache && state == kIdle;
    }

    // r18-sdcq: setup's test group done: the counts start again, so the data path's lines count its own groups only.
    void resetCounts()
    {
        enters = leaves = groups = tasks = settles = recoveries = probes = 0;
    }

    // The card was powered off and initialized again (system sleep, or the driver's answer to `stuck`): CQ mode and the
    // cache (Cache Enable is 0 after power-up) did not survive it; the next session turns the cache on again. r18-sdcq: a
    // card power-cycled out of CQ mode gets the queue back (kMaxStuck times a boot).
    void powerCycled()
    {
        if (state == kOn)
            state = kIdle;
        if (stuck) {
            stuck = false;
            if (++powerCycles >= kMaxStuck && !stopped) {
                stopped = true;
                why = "the card stayed in CQ mode three times";
            }
            if (!stopped && depth >= 2)
                state = kIdle;
        }
        queued = next = 0;
        cacheOn = false;
    }

    bool usable() const
    {
        return state != kOff;
    }

    // r18-sdcq: the backoff's gate, once per read that could start a group: false (counted) while a pause lasts.
    bool admit()
    {
        if (!pauseLeft)
            return true;
        --pauseLeft;
        ++skipped;
        return false;
    }

    // r16-sdcq: Cache Enable (byte 260 = 01h), out of CQ mode, read back. False when the card does not show it on: then
    // 00h is written and read back, so cacheOn stays only for a card that might still have taken it. r18-sdcq: *answered,
    // whether the card answered the CMD49 (only a card that answered and then did not show it on has refused it).
    bool enableCache(Bus &bus, bool *answered = nullptr)
    {
        if (answered)
            *answered = false;
        if (state == kOn)
            return false;
        cacheOn = true;
        uint8_t value = kSdCacheEnable;
        uint32_t r1 = 0;
        const bool written = !bus.control(kSdPerfCacheControl, true, &value, &r1);
        if (answered)
            *answered = written;
        const bool taken = written && readBack(bus, kSdPerfCacheControl, 1, kSdCacheEnable);
        cacheRead = lastRead[0];
        cacheReads = lastReads;
        if (taken)
            return true;
        (void)disableCache(bus);
        return false;
    }

    // r16-sdcq: Flush Cache (byte 261 = 01h): the card busy on DAT0 while it commits its cache (at most 1 s: 4.17), then
    // the bit reads 0. Out of CQ mode only (early SanDisk A2 cards never finished a flush in CQ mode: Linux quirks.h).
    // True with nothing to flush. A CMD49 that failed proves nothing (bit 0 also reads 0 when no flush was asked for): it
    // is sent once more (asking for a flush twice is harmless), and only one the card answered counts.
    bool flush(Bus &bus)
    {
        if (!cacheOn)
            return true;
        if (state == kOn || stuck)
            return false;
        ++flushes;
        uint8_t value = kSdFlushCache;
        uint32_t r1 = 0;
        if (bus.control(kSdPerfFlushControl, true, &value, &r1)) {
            value = kSdFlushCache;
            if (bus.control(kSdPerfFlushControl, true, &value, &r1))
                return false;
        }
        return readBack(bus, kSdPerfFlushControl, kSdFlushCache, 0);
    }

    // r16-sdcq: the cache off (byte 260 = 00h, resent once if its CMD49 failed on the bus, read back), out of CQ mode. No
    // flush first: nothing written while it was on has completed without one (commitWrite). False when the card does not
    // show it off: cacheOn stays, and writes are flushed. Only a CMD49 the card answered makes a 00h read-back mean off
    // (its busy lasts until it is done, 5.7.2.2; a lazy enable reads 00h too). A card stuck in CQ mode is left alone: its
    // power cycle clears it.
    bool disableCache(Bus &bus)
    {
        if (!cacheOn)
            return true;
        if (state == kOn || stuck)
            return false;
        uint8_t value = 0;
        uint32_t r1 = 0;
        if (bus.control(kSdPerfCacheControl, true, &value, &r1)) {
            value = 0;
            if (bus.control(kSdPerfCacheControl, true, &value, &r1))
                return false;
        }
        if (!readBack(bus, kSdPerfCacheControl, 1, 0))
            return false;
        cacheOn = false;
        return true;
    }

    // r16-sdcq: what a legacy write needs before it completes: nothing while the cache is off (every session's end turns
    // it off), a flush while it may be on. False: the write is not known to be on flash (the driver fails it).
    bool commitWrite(Bus &bus)
    {
        return !cacheOn || flush(bus);
    }

    // Into CQ mode (from kIdle): CMD49 03h, read back 03h (lazily: kReadBacks). r16-sdcq: the cache on first while the
    // card offers it (4.19.1); a card that answers the enable and will not show it on keeps it off for the boot and still
    // gets its try at the queue. r18-sdcq: then CMD43, abort the entire queue, before the first CMD44: "Queue shall be
    // empty before assigning Tasks [Host may abort entire queue of tasks queued earlier]" (6.00 4.19.1.2), and on an empty
    // queue the card answers it without error (9.10 4.19.8), R1b, its busy waited out ("not assign another task until
    // CMD43 is completed"). So no task a card kept from the last session (whose Task ID 0 the next CMD44 would find
    // taken: no response, 4.19.5) survives into this one, and the answer proves the card is in CQ mode (class 1 commands
    // get none out of it, 4.19), the 03h read-back alone only that the byte took. A card slow to open its queue gets
    // kReadBacks probes, a settle() between; an answer carrying only B-type bits is the previous probe's (no answer).
    // False with `error` set: out of CQ mode again (idle), or stuck (the driver power-cycles it). Never off for the boot.
    bool enter(Bus &bus)
    {
        if (state == kOn)
            return true;
        if (state != kIdle)
            return false;
        if (useCache && !cacheOn) {
            bool answered = false;
            if (!enableCache(bus, &answered) && answered)
                useCache = false;
        }
        uint8_t value = kEnabled;
        uint32_t r1 = 0;
        uint8_t before = bus.lastCommand();
        if (const int step = bus.control(kSdPerfCqControl, true, &value, &r1)) {
            note(49, 0, before, step, r1, bus);
            return abandon(bus, "CMD49 enabling the queue failed");
        }
        const bool taken = readBack(bus, kSdPerfCqControl, 3, kEnabled);
        cqRead = lastRead[2];
        cqReads = lastReads;
        if (!taken) {
            note(48, 0, 49, -1, 0, bus);
            error.value = cqRead;
            return abandon(bus, "the CQ control byte does not read back 03h");
        }
        int step = 0;
        r1 = 0;
        for (unsigned attempt = 0; attempt < kReadBacks; ++attempt) {
            if (attempt) {
                bus.settle();
                ++settles;
                ++probes;
            }
            before = bus.lastCommand();
            r1 = 0;
            step = bus.command(43, switchSdCmd43(kSdAbortQueue, 0), true, &r1);
            if (!step && !(r1 & kR1Errors & (attempt ? ~kR1PreviousErrors : ~0u)))
                break;
            if (!step)
                step = 30;
        }
        if (step) {
            note(43, 0, before, step, r1, bus);
            return abandon(bus, "the card does not answer CMD43 in CQ mode");
        }
        state = kOn;
        queued = next = 0;
        emptied = true;
        ++enters;
        return true;
    }

    // Out of CQ mode (from kOn): CMD49 00h, read back 00h, then (r16-sdcq) the cache off, so the legacy commands that
    // follow (writes among them) find it off. True when the card is out of CQ mode (a cache that would not go off leaves
    // cacheOn: writes are then flushed); false when it stays in (stuck: the driver power-cycles the card, the only way
    // left, and its power cycle clears the cache). r18-sdcq: tasks still queued (a caller's bug) are aborted first, and a
    // stuck card is not off for the boot: the power cycle gives the queue back (powerCycled).
    bool leave(Bus &bus)
    {
        if (state != kOn)
            return true;
        if (queued != next) {
            uint32_t r1 = 0;
            (void)bus.command(43, switchSdCmd43(kSdAbortQueue, 0), true, &r1);
            queued = next = 0;
        }
        return quit(bus, "the card stays in CQ mode");
    }

    // Assigns tasks 0..n-1 (n <= depth) to an empty queue: CMD44 (read, id, blocks) then CMD45 (start block) for each.
    // r18-sdcq: false at the first command that fails, with `error` set and the engine still in CQ mode for `recover`
    // (the run's next step); a group that cannot be assigned at all (a bug in the caller) stops the queue (fail).
    // r18-sdcq (r8d): CMD43 first (abort the entire queue, R1b), unless the queue was just emptied: a card may keep a
    // finished group's Task IDs taken until the host aborts the queue (6.00 4.19.1.2; the header's r8d paragraph).
    bool submit(Bus &bus, const SwitchCqTask *t, unsigned n)
    {
        if (state != kOn || queued != next)
            return fail(bus, "tasks assigned to a busy queue"), false;
        if (!n || n > depth)
            return fail(bus, "a group larger than the queue"), false;
        for (unsigned i = 0; i < n; ++i) {
            if (!t[i].sectors || t[i].sectors > 0xffff || t[i].lba > 0xffffffffULL)
                return fail(bus, "a task the queue cannot take"), false;
        }
        queued = next = 0;
        if (!emptied) {
            uint32_t r1 = 0;
            const uint8_t before = bus.lastCommand();
            const int step = bus.command(43, switchSdCmd43(kSdAbortQueue, 0), true, &r1);
            if (step || (r1 & kR1Errors)) {
                note(43, 0, before, step, r1, bus);
                why = "CMD43 before a group failed";
                return false;
            }
            ++groupAborts;
        }
        emptied = false;
        for (unsigned i = 0; i < n; ++i) {
            uint32_t r1 = 0;
            uint8_t before = bus.lastCommand();
            int step = bus.command(44, switchSdCmd44(true, static_cast<uint8_t>(i), t[i].sectors), false, &r1);
            if (step || (r1 & kR1Errors)) {
                note(44, static_cast<uint8_t>(i), before, step, r1, bus);
                why = "CMD44 failed";
                return false;
            }
            ++queued;  // a CMD45 that fails leaves at most this one half-assigned: the recovery's CMD43 covers it
            r1 = 0;
            before = bus.lastCommand();
            step = bus.command(45, static_cast<uint32_t>(t[i].lba), false, &r1);
            if (step || (r1 & kR1Errors)) {
                note(45, static_cast<uint8_t>(i), before, step, r1, bus);
                why = "CMD45 failed";
                return false;
            }
        }
        ++groups;
        tasks += n;
        return true;
    }

    // The CMD46 argument of the next task, in Task ID order; executed() once its data is in.
    bool pending() const
    {
        return state == kOn && next < queued;
    }

    uint32_t argument() const
    {
        return switchSdTaskArgument(static_cast<uint8_t>(next));
    }

    void executed()
    {
        ++next;
    }

    bool empty() const
    {
        return next == queued;
    }

    // r18-sdcq: after a command of a session failed (`error` holds it). CMD13 first, while the card still holds the
    // B-type bits of a command it did not answer (they show in the next response, then clear: Table 4-42), so its status
    // says what went wrong, and CMD13 invalidates a half-assigned CMD44 (4.19.5); in CQ mode CMD13 with bit 15 too (the
    // Task Status Register, 4.10.3; out of CQ mode bit 15 is a stuff bit). Then CMD43, abort the entire queue (4.19.1.2:
    // "Host shall abort all queued tasks when there are errors in Sequential CQ Mode"), R1b, its busy waited out. An
    // answered CMD43 leaves the card in CQ mode with an empty queue (only CMD0 or CMD49 00h leave it: 9.10 Table
    // 4.19.4-1): kInPlace. Unanswered (the card is out of CQ mode, or the bus failed): out of CQ mode as leave() goes
    // (kOut), or kStuck. Nothing here turns the queue off.
    Recovery recover(Bus &bus)
    {
        ++recoveries;
        error.tasksStep = error.abortStep = -1;
        error.taskStatus = error.abortR1 = 0;
        error.cardStatus = 0;
        error.statusStep = bus.status(false, &error.cardStatus);
        Recovery r = kOut;
        if (stuck) {
            r = kStuck;
        } else if (state == kOn) {
            error.tasksStep = bus.status(true, &error.taskStatus);
            error.abortStep = bus.command(43, switchSdCmd43(kSdAbortQueue, 0), true, &error.abortR1);
            queued = next = 0;
            if (!error.abortStep && !(error.abortR1 & kR1Errors)) {
                r = kInPlace;
                emptied = true;
            } else {
                r = quit(bus, "the card stays in CQ mode") ? kOut : kStuck;
            }
        }
        error.outcome = r;
        return r;
    }

    // An error that ends the queue for the boot (r18-sdcq: a proof that differed, setup's test group, a caller's bug):
    // abort what is queued (CMD43), leave CQ mode, off for the boot. True when the card is out of CQ mode. `maybeOn`: the
    // card may be in CQ mode although the engine never saw it get there.
    // r16-sdcq: the cache off with the queue; a card stuck in CQ mode is left to the driver's power cycle, which clears
    // it (no completed write is in it: commitWrite).
    bool fail(Bus &bus, const char *reason, bool maybeOn = false)
    {
        why = reason;
        stopped = true;
        const bool wasOn = state == kOn || maybeOn;
        state = kOff;
        bool out = true;
        if (wasOn) {
            if (queued != next) {
                uint32_t r1 = 0;
                (void)bus.command(43, switchSdCmd43(kSdAbortQueue, 0), true, &r1);
            }
            queued = next = 0;
            out = off(bus);
            stuck = !out;
        }
        useCache = false;
        if (out)
            (void)disableCache(bus);
        return out;
    }

    // r18-sdcq: one group of read chains, chain i as task i, the way the driver runs one (Host is the driver's or the
    // host test's). Into CQ mode (enter), every task assigned (submit), each executed in order while the previous one's
    // requests complete; while `proving`, the requests are held until, out of CQ mode, each chain's blocks read again the
    // legacy way match (Host::prove). An error is recovered (recover) and the chains not executed yet are assigned again
    // once (in CQ mode if the recovery left it there, else entering it again); if that fails too, or the card had to be
    // power-cycled, they go the legacy way (from chain 0 while proving: no queued byte of a group with an error is ever
    // unproven) and the queue stays on. A proof that differs stops the queue for the boot and the whole group is read
    // again the legacy way. Every chain's requests complete exactly once, and the caller's earlier batch too (before the
    // group's first command moves data): the last chain is left for the caller (its completions run while its next command
    // moves data), as before. *proven: chains whose proof matched.
    //
    // Host: void beginTask(unsigned chain, uint32_t argument) (the task's CMD46 issued), int endTask(unsigned chain,
    // uint32_t *r1) (waited for: 0, or the step that failed, the controller recovered and a data command stopped by
    // CMD12), void beginLegacy(unsigned chain) / void endLegacy(unsigned chain) (the chain as a legacy command), void
    // complete(unsigned chain), void completeEarlier() (the batch before the group, if any), uint8_t prove(unsigned
    // chain) (a Proof; Broken: the chain completes with an error), void recovered(const SwitchCqError &) (after each
    // recovery, before what follows it: the driver's line), void powerCycle() (the card stays in CQ mode: power it off
    // and initialize it again; the engine's powerCycled follows).
    template <typename Host>
    Outcome run(Bus &bus, Host &host, const SwitchCqTask *t, unsigned n, bool proving, unsigned *proven)
    {
        enum : int {
            kEarlier = -1,
            kNothing = -2
        };

        int owed = kEarlier;  // whose completions are still to run: the caller's batch, a chain, or nothing
        const auto pay = [&] {
            if (owed == kEarlier) {
                host.completeEarlier();
            } else if (owed >= 0) {
                host.complete(static_cast<unsigned>(owed));
            }
            owed = kNothing;
        };
        *proven = 0;
        unsigned done = 0;  // chains 0..done-1 have their task's data
        bool errors = false;
        bool legacy = false;
        for (unsigned attempt = 0; !legacy; ++attempt) {
            const unsigned m = n - done;
            bool ok = enter(bus) && submit(bus, t + done, m);
            while (ok && done < n) {
                const uint8_t before = bus.lastCommand();
                const uint8_t task = static_cast<uint8_t>(next);
                host.beginTask(done, argument());
                pay();
                uint32_t r1 = 0;
                if (const int step = host.endTask(done, &r1)) {
                    note(46, task, before, step, r1, bus);
                    why = "a task's data failed";
                    ok = false;
                    break;
                }
                executed();
                if (!proving)
                    owed = static_cast<int>(done);
                ++done;
            }
            if (ok)
                break;
            legacy = true;
            if (stopped || (state == kOff && !stuck))
                break;  // a caller's bug stopped it: nothing to recover
            errors = true;
            error.attempt = static_cast<uint8_t>(attempt + 1);
            error.tasks = static_cast<uint8_t>(m);
            const Recovery r = recover(bus);
            host.recovered(error);
            if (r != kStuck && attempt == 0) {
                legacy = false;  // once more through the queue
            } else if (r == kStuck || (r == kInPlace && !quit(bus, "the card stays in CQ mode"))) {
                host.powerCycle();
                powerCycled();
            }
        }
        if (!legacy && proving) {
            // Out of CQ mode for the legacy reads, then each chain compared, its requests still held.
            if (!leave(bus)) {
                note(48, 0, 49, -1, 0, bus);
                error.value = lastRead[2];
                error.attempt = 1;
                error.tasks = static_cast<uint8_t>(n);
                errors = legacy = true;
                (void)recover(bus);
                host.recovered(error);
                host.powerCycle();
                powerCycled();
            } else {
                unsigned matched = 0;
                bool differs = false;
                for (unsigned i = 0; i < n && !differs; ++i) {
                    const uint8_t proof = host.prove(i);
                    if (proof == kProofMatches)
                        ++matched;
                    if (proof == kProofDiffers)
                        differs = true;
                }
                if (differs) {
                    (void)fail(bus, "a task differs from its legacy read");
                    legacy = true;
                } else {
                    *proven = matched;
                    for (unsigned i = 0; i + 1 < n; ++i)
                        host.complete(i);
                    owed = static_cast<int>(n - 1);
                }
            }
        }
        if (legacy) {
            if (stuck) {  // a caller's bug stopped the queue and the card would not leave CQ mode
                host.powerCycle();
                powerCycled();
            }
            for (unsigned i = proving ? 0 : done; i < n; ++i) {
                host.beginLegacy(i);
                pay();
                host.endLegacy(i);
                owed = static_cast<int>(i);
            }
        }
        Outcome outcome = kGroupQueued;
        if (stopped) {
            outcome = kGroupStopped;
        } else if (legacy) {
            outcome = kGroupLegacy;
        } else if (errors) {
            outcome = kGroupRetried;
        }
        if (outcome == kGroupLegacy)
            ++legacyGroups;
        if (outcome == kGroupRetried)
            ++retried;
        ended(outcome == kGroupLegacy);
        return outcome;
    }

    // r18-sdcq: chains the queue does not take (fewer than a group from legacy mode needs), each the legacy way, in the
    // run's order of completions (the caller's earlier batch while the first one's command moves data, the last left to
    // the caller). Every chain taken for a group runs, whatever the queue decides.
    template <typename Host> static void runLegacy(Host &host, unsigned n)
    {
        for (unsigned i = 0; i < n; ++i) {
            host.beginLegacy(i);
            if (i) {
                host.complete(i - 1);
            } else {
                host.completeEarlier();
            }
            host.endLegacy(i);
        }
    }

    // r18-sdcq: a failed command into `error` (the driver's setup notes its test tasks' CMD46 here too).
    void note(uint8_t command, uint8_t task, uint8_t previous, int step, uint32_t r1, Bus &bus)
    {
        error = {};
        error.command = command;
        error.task = task;
        error.previous = previous;
        error.step = step;
        error.r1 = r1;
        error.irq = bus.lastIrq();
        error.tasksStep = error.abortStep = -1;
    }

private:
    // r18-sdcq: the backoff's count at a group's end: `failed`, it went the legacy way (r8d: a group whose retry went
    // through the queue is no strike).
    void ended(bool failed)
    {
        pausedNow = false;
        if (!failed) {
            strikes = 0;
            if (clean < kForgive) {
                ++clean;
                if (clean == kForgive)
                    pauseLength = kPauseMin;
            }
            return;
        }
        clean = 0;
        if (stopped || ++strikes < kStrikes)
            return;
        strikes = 0;
        pauseLeft = pauseLength;
        pauseLength = pauseLength * 2 > kPauseMax ? kPauseMax : pauseLength * 2;
        ++pauses;
        pausedNow = true;
    }

    // A session's end after an error (enter's own), as leave() ends one: false (and stuck) when the card stays in CQ
    // mode, else idle.
    bool abandon(Bus &bus, const char *reason)
    {
        why = reason;
        state = kOn;
        queued = next = 0;
        (void)quit(bus, "the card stays in CQ mode");
        return false;
    }

    // Out of CQ mode from kOn with an empty queue (leave, recover, abandon): idle with the cache off (r16-sdcq: a cache
    // that will not go off is left alone from here, no session turns it on again, and writes are flushed while it may be
    // on), or stuck (kOff until the driver's power cycle: powerCycled).
    bool quit(Bus &bus, const char *reason)
    {
        if (!off(bus)) {
            why = reason;
            state = kOff;
            stuck = true;
            return false;
        }
        state = kIdle;
        queued = next = 0;
        ++leaves;
        if (useCache && !disableCache(bus))
            useCache = false;
        return true;
    }

    // r16-sdcq: CMD48 of `byte` until (value & mask) == want, at most kReadBacks times; a settle() before each but the
    // first. The last value read in lastRead.
    bool readBack(Bus &bus, uint32_t byte, uint8_t mask, uint8_t want)
    {
        for (unsigned attempt = 0; attempt < kReadBacks; ++attempt) {
            if (attempt) {
                bus.settle();
                ++settles;
            }
            uint8_t value = static_cast<uint8_t>(~want);
            uint32_t r1 = 0;
            if (bus.control(byte, false, &value, &r1))
                continue;
            lastRead[byte - kSdPerfCacheControl] = value;
            lastReads = attempt + 1;
            if ((value & mask) == want)
                return true;
        }
        lastReads = kReadBacks;
        return false;
    }

    // CMD49 00h, then read back until the enable bit is clear. False when it never is. r16-sdcq: a CMD49 that failed on
    // the bus is sent once more (00h is idempotent): a lazy card that has not shown its enable yet reads 00h either way,
    // so only a write the card answered makes that 00h mean out (if the second fails too, read back anyway: the card may
    // have taken one).
    bool off(Bus &bus)
    {
        uint8_t value = kDisabled;
        uint32_t r1 = 0;
        if (bus.control(kSdPerfCqControl, true, &value, &r1)) {
            value = kDisabled;
            (void)bus.control(kSdPerfCqControl, true, &value, &r1);
        }
        return readBack(bus, kSdPerfCqControl, kSdCqEnable, 0);
    }
};
