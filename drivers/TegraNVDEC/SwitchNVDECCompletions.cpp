// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "SwitchNVDECCompletions.h"
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <string.h>
#include "nvidia/clc5b0.h"
#include "nvidia/nvdec_drv.h"
#include "../TegraGM20B/SwitchGpuBacking.h"
#include "../../kits/SwitchDMA/SwitchDcache.h"

namespace {
// A connection's submits are serialized, so at most one waits for queue room beside the queue's own jobs.
constexpr uint32_t kRecords = 2 * SwitchNVDECQueue::kSlots;

struct Record {
    SwitchNVDECCompletions *owner;
    switch_gpu_backing *backing;  // null: free
    const volatile uint8_t *status;
    size_t statusBytes;
};
}  // namespace

struct SwitchNVDECCompletions {
    IOLock *lock;
    SwitchNVDECCompletionPort ops;
    uint32_t references;
    mach_port_t port;
    OSAsyncReference64 reference;
    Record records[kRecords];
};

namespace {
void drop(SwitchNVDECCompletions *completions)
{
    if (__atomic_sub_fetch(&completions->references, 1u, __ATOMIC_ACQ_REL))
        return;
    IOLockFree(completions->lock);
    IODelete(completions, SwitchNVDECCompletions, 1);
}

void finish(Record *record, io_user_reference_t *arguments, IOReturn result)
{
    auto *completions = record->owner;
    auto *backing = record->backing;
    IOLockLock(completions->lock);
    record->backing = nullptr;
    // Never blocks: the kernel's sends bypass the port's queue limit (MACH_SEND_ALWAYS).
    if (arguments && completions->port)
        completions->ops.send(completions->reference, result, arguments, SWITCH_NVDEC_COMPLETION_ARGS);
    IOLockUnlock(completions->lock);
    switch_gpu_backing_release(backing);
    drop(completions);
}

void completed(void *context, uint32_t fence, int result)
{
    auto *record = static_cast<Record *>(context);
    nvdec_status_s status;
    switch_dcache_invalidate(record->status, record->statusBytes);
    memcpy(&status, const_cast<const uint8_t *>(record->status), sizeof(status));
    io_user_reference_t arguments[SWITCH_NVDEC_COMPLETION_ARGS] = {fence, status.error_status,
                                                                   status.mbs_correctly_decoded};
    finish(record, arguments, result ? kIOReturnIOError : kIOReturnSuccess);
}

const TegraNVDECEntry *statusEntry(const TegraNVDECSubmit &submit)
{
    for (uint32_t i = 0; i < submit.entry_count && i < TEGRA_NVDEC_MAX_ENTRIES; ++i)
        if (submit.entries[i].method == NVC5B0_SET_NVDEC_STATUS_OFFSET &&
            submit.entries[i].kind == TEGRA_NVDEC_ENTRY_RELOC)
            return &submit.entries[i];
    return nullptr;
}
}  // namespace

SwitchNVDECCompletions *switch_nvdec_completions_create(const SwitchNVDECCompletionPort &ops)
{
    auto *completions = IONewZero(SwitchNVDECCompletions, 1);
    if (!completions)
        return nullptr;
    completions->lock = IOLockAlloc();
    if (!completions->lock) {
        IODelete(completions, SwitchNVDECCompletions, 1);
        return nullptr;
    }
    completions->ops = ops;
    completions->references = 1;
    for (auto &record : completions->records)
        record.owner = completions;
    return completions;
}

void switch_nvdec_completions_release(SwitchNVDECCompletions *completions)
{
    if (!completions)
        return;
    switch_nvdec_completions_close(completions);
    drop(completions);
}

IOReturn switch_nvdec_completions_listen(SwitchNVDECCompletions *completions, mach_port_t port, task_t task)
{
    if (!completions || port == MACH_PORT_NULL)
        return kIOReturnBadArgument;
    IOLockLock(completions->lock);
    const mach_port_t old = completions->port;
    __atomic_store_n(&completions->port, port, __ATOMIC_RELEASE);
    completions->ops.refer(completions->reference, port, task);
    IOLockUnlock(completions->lock);
    // Each registration brings a send right of its own: the replaced port's, or this call's second one to the port
    // already kept, goes here. No send uses it after the unlock, since a completion reads the port under the lock.
    if (old != MACH_PORT_NULL)
        IOUserClient::releaseNotificationPort(old);
    return kIOReturnSuccess;
}

void switch_nvdec_completions_close(SwitchNVDECCompletions *completions)
{
    IOLockLock(completions->lock);
    const mach_port_t old = completions->port;
    __atomic_store_n(&completions->port, MACH_PORT_NULL, __ATOMIC_RELEASE);
    memset(completions->reference, 0, sizeof(completions->reference));
    IOLockUnlock(completions->lock);
    if (old != MACH_PORT_NULL)
        IOUserClient::releaseNotificationPort(old);
}

int switch_nvdec_completions_arm(SwitchNVDECCompletions *completions, const TegraNVDECSubmit *submit,
                                 TegraNVDECResolve resolve, void *context, SwitchNVDECCompletion *out)
{
    *out = {};
    if (!completions || __atomic_load_n(&completions->port, __ATOMIC_ACQUIRE) == MACH_PORT_NULL)
        return 0;
    const TegraNVDECEntry *entry = submit ? statusEntry(*submit) : nullptr;
    TegraNVDECBlock block = {};
    if (!entry || !resolve(context, entry->value, &block) || !block.backing || block.size < sizeof(nvdec_status_s) ||
        entry->offset > block.size - sizeof(nvdec_status_s))
        return 0;
    // nvdec_status_s is 56 bytes in a 256-byte-aligned slot; rounding up to a full 64-byte line lets
    // switch_dcache_invalidate issue dc ivac instead of dc civac on the tail line.
    const size_t statusBytes =
        !(entry->offset & (SWITCH_DCACHE_LINE - 1)) && entry->offset + SWITCH_DCACHE_LINE <= block.size
            ? SWITCH_DCACHE_LINE
            : sizeof(nvdec_status_s);
    IOLockLock(completions->lock);
    Record *record = nullptr;
    for (uint32_t i = 0; completions->port != MACH_PORT_NULL && !record && i < kRecords; ++i)
        if (!completions->records[i].backing)
            record = &completions->records[i];
    const bool listening = completions->port != MACH_PORT_NULL;
    if (record) {
        record->backing = block.backing;
        record->status = block.va + entry->offset;
        record->statusBytes = statusBytes;
        switch_gpu_backing_retain(block.backing);
        __atomic_fetch_add(&completions->references, 1u, __ATOMIC_RELAXED);
    }
    IOLockUnlock(completions->lock);
    if (record)
        *out = {completed, record};
    return listening && !record ? -12 : 0;
}

void switch_nvdec_completions_disarm(const SwitchNVDECCompletion *armed)
{
    if (armed && armed->done == completed)
        finish(static_cast<Record *>(armed->context), nullptr, kIOReturnSuccess);
}
