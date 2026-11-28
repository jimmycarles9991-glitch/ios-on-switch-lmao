// SPDX-License-Identifier: BSD-2-Clause-Patent
// HW Test gpu.x1.host1x_mlock (host1x item 5, c38-audit/perf1/learn/host1x.md A3), debug kernels: whether
// ACQUIRE_MLOCK holds a second channel. Run through TegraVIC's kernel-test selector, as VIC's module lock (10, L4T
// nvhost.h) is what two VIC channels would share.
// - Control: the first channel WAITs on a gate syncpoint the CPU holds; the second raises its syncpoint at once,
//   because channels run side by side (TRM 14.6.1).
// - Locked: the first channel takes MLOCK 10, raises `held`, then waits on the gate. The second asks for MLOCK 10 and
//   then raises its syncpoint. If the lock holds, that syncpoint stays put until the CPU raises the gate and the first
//   channel lets the lock go.
// Each stream ends with an increment of its own, after anything it holds is let go: that is its job's fence, so its
// arena room is reused only once host1x has read every word.
#pragma once
#include <stdint.h>
#include "TegraHost1x.h"

#define SWITCH_HOST1X_MLOCK_PROBE_MAGIC 0x564d4c4bu  // 'VMLK'

typedef struct {
    uint32_t magic;
    int32_t status;              // 0, or why the probe could not run or settle
    uint32_t controlSideBySide;  // 1: the second channel ran while the first waited (no lock)
    uint32_t heldTaken;          // 1: the first channel took the lock and raised `held`
    uint32_t heldOff;            // 1: the second channel did not run while the lock was held
    uint32_t afterRelease;       // 1: the second channel ran once the lock was let go
    uint32_t heldUs;             // how long the second channel was watched while the lock was held
    uint32_t settled;            // 1: every stream finished without a teardown
} SwitchHost1xMlockProbeReply;

namespace switch_host1x_mlock {
constexpr uint32_t kLock = 10, kHeldUs = 20000, kRunUs = 50000;
constexpr uint32_t kHolderWords = 1 + 2 + SWITCH_HOST1X_WAIT_WORDS + 1 + 1;
constexpr uint32_t kAskerWords = 1 + 1 + 2;

inline uint32_t increment(uint32_t *w, uint32_t syncpt)
{
    w[0] = tegra_host1x_op_setclass(TEGRA_HOST1X_CLASS_HOST1X, 0, 0);
    w[1] = tegra_host1x_op_incr_syncpt(0, syncpt);
    return 2;
}

// The first channel: [ACQUIRE_MLOCK] raise `held`, WAIT on the gate, [RELEASE_MLOCK], raise `done`.
inline uint32_t holder(uint32_t *w, bool locked, uint32_t held, uint32_t gate, uint32_t gateAt, uint32_t done)
{
    uint32_t n = 0;
    if (locked)
        w[n++] = tegra_host1x_op_acquire_mlock(kLock);
    n += increment(w + n, held);
    n += tegra_host1x_op_wait_syncpt(w + n, gate, gateAt);
    if (locked)
        w[n++] = tegra_host1x_op_release_mlock(kLock);
    w[n++] = tegra_host1x_op_incr_syncpt(0, done);
    return n;
}

// The second channel: [ACQUIRE_MLOCK, RELEASE_MLOCK] then raise `after`.
inline uint32_t asker(uint32_t *w, bool locked, uint32_t after)
{
    uint32_t n = 0;
    if (locked) {
        w[n++] = tegra_host1x_op_acquire_mlock(kLock);
        w[n++] = tegra_host1x_op_release_mlock(kLock);
    }
    n += increment(w + n, after);
    return n;
}
}  // namespace switch_host1x_mlock

#ifdef __cplusplus
extern "C" {
#endif
#if DEVELOPMENT || DEBUG
int switch_host1x_mlock_probe_run(void *reply);
#endif
#ifdef __cplusplus
}
#endif
