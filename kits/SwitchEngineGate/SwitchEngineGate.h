// SPDX-License-Identifier: BSD-2-Clause-Patent
// A host1x engine (VIC, NVDEC, NVJPG) that is powered off when idle and on again at its next use, as mainline
// drm/tegra's runtime PM does for VIC and NVDEC (vic.c and nvdec.c: autosuspend 500 ms after the last job; a job holds
// the engine from its submission to its completion). A use powers the engine on if it is off (the engine's `on`:
// partition, clock and reset, the falcon rebooted and proven, its job queue rebased on the syncpoint). The last user
// leaving, or the engine's queue draining, arms a one-shot timer; if it fires with no user and nothing queued, the
// engine parks its queue and powers off (its `off`). Those two events are the only arms, so nothing polls.
//
// A power-on that fails fails that use (`on` logs which step and register); the engine stays off and the next use runs
// the normal power-on. An engine its driver found off outside the gate (lost(): a reset that did not bring it back) is
// off to the gate too, so its next use powers it on. A power-off the hardware refuses is one line and leaves the engine
// parked and unpowered; the next use powers it on and the next idle period powers it off as usual. No failure is held
// for later: nothing is counted, delayed or refused for having failed before.
// Sleep (SC7) goes through the gate too: suspend parks the engine once its queue drains and powers it off, holding new
// users until resume, which powers it on only for a user already waiting.
//
// Platform is SwitchEngineGateKernel.h in the kernel and a model in the host test: lock, unlock, sleep and wake on an
// event under the lock, arm(ms), nowUs(), log(line), and start(fire, context, args...).
#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef KERNEL
#include <libkern/libkern.h>
#else
#include <stdio.h>
#endif

#if (defined(DEVELOPMENT) && DEVELOPMENT) || (defined(DEBUG) && DEBUG)
#define SWITCH_ENGINE_GATE_STATS 1
#else
#define SWITCH_ENGINE_GATE_STATS 0
#endif

enum {
    SWITCH_ENGINE_GATE_OFF = 0,   // off: the engine is powered down
    SWITCH_ENGINE_GATE_BUSY = 1,  // off: work is queued, nothing was done
};

struct SwitchEngineGateOps {
    const char *name;  // the engine's log prefix
    void *engine;
    int (*on)(void *engine);               // 0, or a negative errno (one line saying why): the engine stays off
    int (*off)(void *engine, bool drain);  // SWITCH_ENGINE_GATE_*, or < 0: the hardware refused (parked, unpowered)
};

struct SwitchEngineGateStats {
    uint64_t gates, ungates, lastResumeUs, maxResumeUs;
};

template <class Platform> class SwitchEngineGate {
public:
    static constexpr uint32_t kIdleMs = 500;

    // The engine is up, idle and powered. False: no lock or timer, and the engine must not publish.
    template <class... Args> bool start(const SwitchEngineGateOps &ops, Args... args)
    {
        ops_ = ops;
        if (!platform_.start(&SwitchEngineGate::fired, this, args...))
            return false;
        __atomic_store_n(&lastActivityUs_, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&state_, kPowered | kTimerArmed, __ATOMIC_RELEASE);
        armTimer(platform_.nowUs(), kIdleMs);
        return true;
    }

    // Before a job is built for the engine: 0 with the engine powered and held until done(), or why it cannot run.
    int use()
    {
        uint32_t cur = __atomic_load_n(&state_, __ATOMIC_ACQUIRE);
        while ((cur & (kPowered | kBlocked)) == kPowered) {
            if (__atomic_compare_exchange_n(&state_, &cur, cur + 1, true, __ATOMIC_ACQUIRE, __ATOMIC_ACQUIRE))
                return 0;
        }
        return useSlow();
    }

    // The engine's driver found it off outside the gate (a reset that did not bring it back): the next use powers it
    // on. Takes no lock: the driver may hold its own, or the gate's, when it finds out.
    void lost()
    {
        __atomic_fetch_and(&state_, ~kPowered, __ATOMIC_RELEASE);
    }

    void done()
    {
        uint32_t cur = __atomic_load_n(&state_, __ATOMIC_ACQUIRE);
        uint64_t now = 0;
        bool stamped = false;
        for (;;) {
            const uint32_t users = cur & kUserMask;
            if (users == 1 && !stamped) {
                now = platform_.nowUs();
                __atomic_store_n(&lastActivityUs_, now, __ATOMIC_RELEASE);
                stamped = true;
            }
            const bool arm = (users == 1) && (cur & kPowered) && !(cur & (kBlocked | kTimerArmed));
            const uint32_t next = (cur - 1) | (arm ? kTimerArmed : 0);
            if (__atomic_compare_exchange_n(&state_, &cur, next, true, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                if (arm)
                    armTimer(now, kIdleMs);
                return;
            }
        }
    }

    // The gate's lock held: the engine stays as it is until release(), with no power-on: no idle power-off, and sleep
    // waits for the release. For a driver's recovery that runs part of its sequence (a falcon's reboot and proof)
    // without the lock.
    void holdLocked()
    {
        __atomic_fetch_add(&state_, 1u, __ATOMIC_ACQ_REL);
        held_++;
    }

    void release()
    {
        platform_.lock();
        const uint64_t now = platform_.nowUs();
        __atomic_store_n(&lastActivityUs_, now, __ATOMIC_RELEASE);
        bool arm = false;
        uint32_t cur = __atomic_load_n(&state_, __ATOMIC_ACQUIRE);
        for (;;) {
            const uint32_t users = cur & kUserMask;
            arm = (users == 1) && (cur & kPowered) && !(cur & (kBlocked | kTimerArmed));
            const uint32_t next = (cur - 1) | (arm ? kTimerArmed : 0);
            if (__atomic_compare_exchange_n(&state_, &cur, next, true, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
                break;
        }
        if (arm)
            __atomic_store_n(&deadlineUs_, now + kIdleUs, __ATOMIC_RELEASE);
        if (!--held_)
            platform_.wake(&held_);
        platform_.unlock();
        if (arm)
            platform_.arm(kIdleMs);
    }

    // The engine's queue drained. Takes no lock: the engine may call it under its own.
    void idle()
    {
        const uint64_t now = platform_.nowUs();
        // Sequential consistency orders storing activity before observing kBlocked against expire() setting kBlocked
        // before reading lastActivityUs_.
        __atomic_store_n(&lastActivityUs_, now, __ATOMIC_SEQ_CST);
        uint32_t cur = __atomic_load_n(&state_, __ATOMIC_SEQ_CST);
        for (;;) {
            if (!(cur & kPowered))
                return;
            if (cur & kBlocked) {
                if (cur & kIdlePending)
                    return;
                if (__atomic_compare_exchange_n(&state_, &cur, cur | kIdlePending, true, __ATOMIC_ACQ_REL,
                                                __ATOMIC_ACQUIRE))
                    return;
                continue;
            }
            if (cur & kTimerArmed)
                return;
            if (__atomic_compare_exchange_n(&state_, &cur, cur | kTimerArmed, true, __ATOMIC_ACQ_REL,
                                            __ATOMIC_ACQUIRE)) {
                armTimer(now, kIdleMs);
                return;
            }
        }
    }

    void expire()
    {
        char line[160];
        line[0] = 0;
        uint32_t rearmMs = 0;
        platform_.lock();
        if (asleep_ || !(__atomic_load_n(&state_, __ATOMIC_ACQUIRE) & kPowered)) {
            __atomic_fetch_and(&state_, ~(kTimerArmed | kIdlePending), __ATOMIC_RELEASE);
            platform_.unlock();
            return;
        }
        uint32_t cur = __atomic_fetch_or(&state_, kBlocked, __ATOMIC_SEQ_CST) | kBlocked;
        if (cur & kUserMask) {
            for (;;) {
                if (!(cur & kPowered)) {
                    __atomic_fetch_and(&state_, ~(kBlocked | kTimerArmed | kIdlePending), __ATOMIC_RELEASE);
                    platform_.unlock();
                    return;
                }
                if (!(cur & kUserMask)) {
                    rearmMs = kIdleMs;
                    const uint32_t next = (cur & ~(kBlocked | kIdlePending)) | kTimerArmed;
                    if (__atomic_compare_exchange_n(&state_, &cur, next, true, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
                        break;
                    continue;
                }
                const uint32_t next = cur & ~(kBlocked | kTimerArmed | kIdlePending);
                if (__atomic_compare_exchange_n(&state_, &cur, next, true, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
                    break;
            }
            if (rearmMs)
                __atomic_store_n(&deadlineUs_, platform_.nowUs() + static_cast<uint64_t>(rearmMs) * 1000,
                                 __ATOMIC_RELEASE);
            platform_.unlock();
            if (rearmMs)
                platform_.arm(rearmMs);
            return;
        }
        const uint64_t deadline = __atomic_load_n(&deadlineUs_, __ATOMIC_ACQUIRE);
        const uint64_t activity = __atomic_load_n(&lastActivityUs_, __ATOMIC_SEQ_CST);
        if (activity) {
            const uint64_t now = platform_.nowUs();
            // now < deadline only occurs in host tests firing the timer directly without advancing a stub clock.
            if (now >= deadline && now >= activity && now - activity < kIdleUs) {
                const uint64_t remUs = kIdleUs - (now - activity);
                const uint32_t remMs = static_cast<uint32_t>((remUs + 999) / 1000);
                rearmMs = remMs ? remMs : 1;
                __atomic_store_n(&deadlineUs_, now + static_cast<uint64_t>(rearmMs) * 1000, __ATOMIC_RELEASE);
                cur = __atomic_load_n(&state_, __ATOMIC_RELAXED);
                while (!__atomic_compare_exchange_n(&state_, &cur, (cur & ~(kBlocked | kIdlePending)) | kTimerArmed,
                                                    true, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
                }
                platform_.unlock();
                platform_.arm(rearmMs);
                return;
            }
        }
        if (__atomic_load_n(&state_, __ATOMIC_ACQUIRE) & kPowered) {
            const int result = ops_.off(ops_.engine, false);
            if (result == SWITCH_ENGINE_GATE_OFF) {
                __atomic_store_n(&lastActivityUs_, 0, __ATOMIC_RELAXED);
                __atomic_fetch_and(&state_, ~(kPowered | kBlocked | kTimerArmed | kIdlePending), __ATOMIC_RELEASE);
                count(line, sizeof(line));
            } else if (result < 0) {
                __atomic_store_n(&lastActivityUs_, 0, __ATOMIC_RELAXED);
                __atomic_fetch_and(&state_, ~(kPowered | kBlocked | kTimerArmed | kIdlePending), __ATOMIC_RELEASE);
                snprintf(line, sizeof(line),
                         "%s: idle power-off refused by the hardware (%d); parked unpowered, the next use powers it on",
                         ops_.name, result);
            } else {
                cur = __atomic_load_n(&state_, __ATOMIC_ACQUIRE);
                for (;;) {
                    const bool idleRaced = (cur & kIdlePending) != 0;
                    rearmMs = idleRaced ? kIdleMs : 0;
                    uint32_t next = cur & ~(kBlocked | kIdlePending);
                    if (idleRaced)
                        next |= kTimerArmed;
                    else
                        next &= ~kTimerArmed;
                    if (__atomic_compare_exchange_n(&state_, &cur, next, true, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
                        break;
                }
                if (rearmMs)
                    __atomic_store_n(&deadlineUs_, platform_.nowUs() + static_cast<uint64_t>(rearmMs) * 1000,
                                     __ATOMIC_RELEASE);
            }
        } else {
            __atomic_fetch_and(&state_, ~(kBlocked | kTimerArmed | kIdlePending), __ATOMIC_RELEASE);
        }
        platform_.unlock();
        if (rearmMs)
            platform_.arm(rearmMs);
        if (line[0])
            platform_.log(line);
    }

    void suspend()
    {
        platform_.lock();
        asleep_ = true;
        __atomic_fetch_or(&state_, kBlocked, __ATOMIC_SEQ_CST);
        while (held_)
            platform_.sleep(&held_);
        if (__atomic_load_n(&state_, __ATOMIC_ACQUIRE) & kPowered)
            (void)ops_.off(ops_.engine, true);
        __atomic_store_n(&lastActivityUs_, 0, __ATOMIC_RELAXED);
        __atomic_fetch_and(&state_, ~(kPowered | kTimerArmed | kIdlePending), __ATOMIC_RELEASE);
        platform_.unlock();
    }

    // The engine's `on` restores what its own resume could not (its address space).
    void resume()
    {
        platform_.lock();
        asleep_ = false;
        if (__atomic_load_n(&state_, __ATOMIC_ACQUIRE) & kUserMask)
            (void)powerOn(0);
        bool arm = false;
        uint32_t cur = __atomic_load_n(&state_, __ATOMIC_ACQUIRE);
        for (;;) {
            arm = (cur & kPowered) && !(cur & kUserMask) && !(cur & kTimerArmed);
            const uint32_t next = (cur & ~(kBlocked | kIdlePending)) | (arm ? kTimerArmed : 0);
            if (__atomic_compare_exchange_n(&state_, &cur, next, true, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
                break;
        }
        if (arm)
            __atomic_store_n(&deadlineUs_, platform_.nowUs() + kIdleUs, __ATOMIC_RELEASE);
        platform_.wake(&asleep_);
        platform_.unlock();
        if (arm)
            platform_.arm(kIdleMs);
    }

    // For lines that read the engine's registers: they must not while it is off.
    bool powered() const
    {
        return (__atomic_load_n(&state_, __ATOMIC_RELAXED) & kPowered) != 0;
    }

    SwitchEngineGateStats stats() const
    {
        return stats_;
    }

    Platform &platform()
    {
        return platform_;
    }

private:
    static constexpr uint64_t kIdleUs = static_cast<uint64_t>(kIdleMs) * 1000;
    static constexpr uint32_t kPowered = 1u << 31;
    static constexpr uint32_t kBlocked = 1u << 30;
    static constexpr uint32_t kTimerArmed = 1u << 29;
    static constexpr uint32_t kIdlePending = 1u << 28;
    static constexpr uint32_t kUserMask = kIdlePending - 1;

    static void fired(void *gate)
    {
        static_cast<SwitchEngineGate *>(gate)->expire();
    }

    void armTimer(uint64_t now, uint32_t ms)
    {
        __atomic_store_n(&deadlineUs_, now + static_cast<uint64_t>(ms) * 1000, __ATOMIC_RELEASE);
        platform_.arm(ms);
    }

    int useSlow()
    {
        platform_.lock();
        while (asleep_)
            platform_.sleep(&asleep_);
        const int error = powerOn(1);
        platform_.unlock();
        return error;
    }

    int powerOn(uint32_t addUsers)
    {
        if (__atomic_load_n(&state_, __ATOMIC_ACQUIRE) & kPowered) {
            if (addUsers)
                __atomic_fetch_add(&state_, addUsers, __ATOMIC_ACQ_REL);
            return 0;
        }
#if SWITCH_ENGINE_GATE_STATS
        const uint64_t began = platform_.nowUs();
#endif
        if (const int error = ops_.on(ops_.engine))
            return error;
        uint32_t cur = __atomic_load_n(&state_, __ATOMIC_RELAXED);
        while (!__atomic_compare_exchange_n(&state_, &cur, (cur | kPowered) + addUsers, true, __ATOMIC_ACQ_REL,
                                            __ATOMIC_RELAXED)) {
        }
#if SWITCH_ENGINE_GATE_STATS
        const uint64_t us = platform_.nowUs() - began;
        stats_.ungates++;
        stats_.lastResumeUs = us;
        stats_.maxResumeUs = us > stats_.maxResumeUs ? us : stats_.maxResumeUs;
#endif
        return 0;
    }

    // The DEVELOPMENT line for a power-off: cycles so far, the last and the longest power-on.
    void count(char *line, size_t size)
    {
#if SWITCH_ENGINE_GATE_STATS
        stats_.gates++;
        snprintf(line, size, "%s: idle power off: gates=%llu ungates=%llu resume=%lluus max=%lluus", ops_.name,
                 static_cast<unsigned long long>(stats_.gates), static_cast<unsigned long long>(stats_.ungates),
                 static_cast<unsigned long long>(stats_.lastResumeUs),
                 static_cast<unsigned long long>(stats_.maxResumeUs));
#else
        (void)line, (void)size;
#endif
    }

    Platform platform_;
    SwitchEngineGateOps ops_ = {};
    SwitchEngineGateStats stats_ = {};
    uint64_t lastActivityUs_ = 0, deadlineUs_ = 0;
    uint32_t state_ = 0, held_ = 0;
    bool asleep_ = false;
};
