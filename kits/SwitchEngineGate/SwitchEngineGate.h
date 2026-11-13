// SPDX-License-Identifier: BSD-2-Clause-Patent
// A host1x engine (VIC, NVDEC, NVJPG) that is powered off when idle and on again at its next use, as mainline
// drm/tegra's runtime PM does for VIC and NVDEC (vic.c and nvdec.c: autosuspend 500 ms after the last job; a job holds
// the engine from its submission to its completion). A use powers the engine on if it is off (the engine's `on`:
// partition, clock and reset, the falcon rebooted and proven, its job queue rebased on the syncpoint). The last user
// leaving, or the engine's queue draining, arms a one-shot timer; if it fires with no user and nothing queued, the
// engine parks its queue and powers off (its `off`). Those two events are the only arms, so nothing polls.
//
// A power-on that fails wedges the engine for the boot (`on` logs why; every later use is refused). A power-off the
// hardware refuses turns gating off for the boot, with one line, and the next use powers the engine on as usual.
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
    int (*on)(void *engine);               // 0, or a negative errno: the engine refuses work from now on
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
        setPowered(true);
        if (!platform_.start(&SwitchEngineGate::fired, this, args...))
            return false;
        platform_.arm(kIdleMs);
        return true;
    }

    // Before a job is built for the engine: 0 with the engine powered and held until done(), or why it cannot run.
    int use()
    {
        platform_.lock();
        while (asleep_ && !failed_)
            platform_.sleep(&asleep_);
        const int error = failed_ ? -19 : powerOn();
        if (!error)
            users_++;
        platform_.unlock();
        return error;
    }

    void done()
    {
        platform_.lock();
        const bool last = --users_ == 0;
        platform_.unlock();
        if (last)
            platform_.arm(kIdleMs);
    }

    // The engine's queue drained. Takes no lock: the engine may call it under its own.
    void idle()
    {
        platform_.arm(kIdleMs);
    }

    void expire()
    {
        char line[160];
        line[0] = 0;
        platform_.lock();
        if (powered_ && gating_ && !users_ && !asleep_ && !failed_) {
            const int result = ops_.off(ops_.engine, false);
            if (result == SWITCH_ENGINE_GATE_OFF) {
                setPowered(false);
                count(line, sizeof(line));
            } else if (result < 0) {
                setPowered(false);
                gating_ = false;
                snprintf(line, sizeof(line), "%s: idle power-off refused by the hardware (%d); powered from here on",
                         ops_.name, result);
            }
        }
        platform_.unlock();
        if (line[0])
            platform_.log(line);
    }

    void suspend()
    {
        platform_.lock();
        asleep_ = true;
        if (powered_)
            (void)ops_.off(ops_.engine, true);
        setPowered(false);
        platform_.unlock();
    }

    // `usable`: the engine's own resume (its address space) came back.
    void resume(bool usable)
    {
        platform_.lock();
        asleep_ = false;
        failed_ = failed_ || !usable;
        if (!failed_ && users_)
            (void)powerOn();
        platform_.wake(&asleep_);
        platform_.unlock();
    }

    // For lines that read the engine's registers: they must not while it is off.
    bool powered() const
    {
        return __atomic_load_n(&powered_, __ATOMIC_RELAXED);
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
    static void fired(void *gate)
    {
        static_cast<SwitchEngineGate *>(gate)->expire();
    }

    void setPowered(bool on)
    {
        __atomic_store_n(&powered_, on, __ATOMIC_RELAXED);
    }

    int powerOn()
    {
        if (powered_)
            return 0;
#if SWITCH_ENGINE_GATE_STATS
        const uint64_t began = platform_.nowUs();
#endif
        const int error = ops_.on(ops_.engine);
        if (error) {
            failed_ = true;
            return error;
        }
        setPowered(true);
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
    uint32_t users_ = 0;
    bool powered_ = false, gating_ = true, asleep_ = false, failed_ = false;
};
