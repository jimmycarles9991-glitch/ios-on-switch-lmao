// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "SwitchVICGate.h"

#if TEGRA_VIC_STATS
#include <IOKit/IOLib.h>
#include <string.h>
#include "SwitchVICGateABI.h"
#include "SwitchVICTransform.h"
#include "TegraVICBounce.h"
#include "TegraVICSurface.h"
#include "../TegraHost1x/TegraHost1x.h"

namespace {
// Arm, release, query and close come from user-client threads and take `lock`: a test's few calls a run, never on a
// frame's path. The copy's completion, on VIC's retire thread, gives the surfaces back and then publishes `done` with
// one release store; arm reads it before it reuses them.
struct Gate {
    IOLock *lock;
    const void *owner;
    uint32_t syncpt, threshold;
    bool syncptMade, armed, released;
    TegraVICSurface source, destination;
    int status;
    bool done;
};

Gate gGate;

IOLock *gateLock()
{
    IOLock *lock = __atomic_load_n(&gGate.lock, __ATOMIC_ACQUIRE);
    if (lock)
        return lock;
    IOLock *made = IOLockAlloc();
    if (made && !__atomic_compare_exchange_n(&gGate.lock, &lock, made, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        IOLockFree(made);
        return lock;
    }
    return made;
}

void completed(void *, int status)
{
    tegra_vic_surface_written(&gGate.destination);
    tegra_vic_surface_release(&gGate.source);
    tegra_vic_surface_release(&gGate.destination);
    gGate.status = status;
    __atomic_store_n(&gGate.done, true, __ATOMIC_RELEASE);
}

bool direct(const TegraVICSurfaceDesc &desc, bool source)
{
    return tegra_vic_bounce_needed(&desc, tegra_vic_pitch_align(tegra_vic_traits(), source)) == 1;
}

int lookup(const SwitchVICGateRequest &r, TegraVICSurface &source, TegraVICSurface &destination)
{
    if (tegra_vic_surface_lookup(r.source, current_task(), &source))
        return -2;
    if (tegra_vic_surface_lookup(r.destination, current_task(), &destination)) {
        tegra_vic_surface_release(&source);
        return -2;
    }
    const auto &s = source.desc, &d = destination.desc;
    if (s.fourcc != d.fourcc || s.width != d.width || s.height != d.height || !direct(s, true) || !direct(d, false)) {
        tegra_vic_surface_release(&source);
        tegra_vic_surface_release(&destination);
        return -95;
    }
    return 0;
}

int arm(const void *client, const SwitchVICGateRequest &r)
{
    if (gGate.armed && !__atomic_load_n(&gGate.done, __ATOMIC_ACQUIRE))
        return -16;
    if (!tegra_vic_ready() || !tegra_vic_traits())
        return -19;
    if (!gGate.syncptMade) {
        if (tegra_host1x_syncpt_alloc(&gGate.syncpt))
            return -12;
        gGate.syncptMade = true;
    }
    TegraVICSurface source = {}, destination = {};
    if (const int error = lookup(r, source, destination))
        return error;
    TegraVICPlan plan = {};
    plan.count = plan.readsSource = 1;
    plan.op[0].srcW = uint64_t(source.desc.width) << 16;
    plan.op[0].srcH = uint64_t(source.desc.height) << 16;
    plan.op[0].dstW = destination.desc.width;
    plan.op[0].dstH = destination.desc.height;
    gGate.source = source;
    gGate.destination = destination;
    gGate.owner = client;
    gGate.released = false;
    gGate.status = 0;
    __atomic_store_n(&gGate.done, false, __ATOMIC_RELAXED);
    gGate.threshold = tegra_host1x_syncpt_read(gGate.syncpt) + 1;
    gGate.armed = true;
    const TegraVICAcquire acquire = {gGate.syncpt, gGate.threshold};
    const int submitted =
        switch_vic_transform(&gGate.source, &gGate.destination, &plan, 0, completed, nullptr, nullptr, &acquire);
    if (submitted == 1)
        return 0;
    gGate.armed = false;
    gGate.owner = nullptr;
    tegra_vic_surface_release(&gGate.source);
    tegra_vic_surface_release(&gGate.destination);
    return submitted ? submitted : -95;
}

int moveGate()
{
    const int error = tegra_host1x_syncpt_cpu_increment(gGate.syncpt);
    if (error)
        IOLog("TegraVIC: the gate's syncpoint %u did not move to %u (%d): its copy waits for VIC's bound\n",
              gGate.syncpt, gGate.threshold, error);
    else
        gGate.released = true;
    return error;
}

int release(const void *client)
{
    if (!gGate.armed || gGate.owner != client || gGate.released)
        return -22;
    return moveGate();
}

void describe(SwitchVICGateReply &reply)
{
    const bool done = __atomic_load_n(&gGate.done, __ATOMIC_ACQUIRE);
    reply.state = !gGate.armed     ? SWITCH_VIC_GATE_IDLE
                  : done           ? SWITCH_VIC_GATE_DONE
                  : gGate.released ? SWITCH_VIC_GATE_RELEASED
                                   : SWITCH_VIC_GATE_HELD;
    reply.status = gGate.armed && done ? gGate.status : 0;
    reply.syncpt = gGate.syncptMade ? gGate.syncpt : 0;
    reply.threshold = gGate.threshold;
}
}  // namespace

IOReturn switch_vic_gate_call(const void *client, const IOExternalMethodArguments *a)
{
    const auto *request = static_cast<const SwitchVICGateRequest *>(a->structureInput);
    if (a->scalarInputCount || a->scalarOutputCount || a->structureInputSize != sizeof(*request) || !request ||
        request->magic != SWITCH_VIC_GATE_MAGIC || !a->structureOutput ||
        a->structureOutputSize != sizeof(SwitchVICGateReply) || request->op < SWITCH_VIC_GATE_ARM ||
        request->op > SWITCH_VIC_GATE_QUERY)
        return kIOReturnBadArgument;
    IOLock *lock = gateLock();
    if (!lock)
        return kIOReturnNoMemory;
    SwitchVICGateReply reply = {};
    reply.magic = SWITCH_VIC_GATE_MAGIC;
    IOLockLock(lock);
    if (request->op == SWITCH_VIC_GATE_ARM)
        reply.error = -arm(client, *request);
    else if (request->op == SWITCH_VIC_GATE_RELEASE)
        reply.error = -release(client);
    describe(reply);
    IOLockUnlock(lock);
    memcpy(a->structureOutput, &reply, sizeof(reply));
    return kIOReturnSuccess;
}

void switch_vic_gate_close(const void *client)
{
    IOLock *lock = __atomic_load_n(&gGate.lock, __ATOMIC_ACQUIRE);
    if (!lock)
        return;
    IOLockLock(lock);
    if (gGate.armed && gGate.owner == client) {
        if (!gGate.released)
            moveGate();
        gGate.owner = nullptr;
    }
    IOLockUnlock(lock);
}
#endif
