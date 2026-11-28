// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "SwitchNVENCCompletions.h"
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <string.h>
#include "nvidia/clc5b7.h"
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-const-variable"
#pragma clang diagnostic ignored "-Wunused-variable"
#include "nvidia/nvenc_drv.h"
#pragma clang diagnostic pop
#include "../TegraGM20B/SwitchGpuBacking.h"
#include "../../kits/SwitchDMA/SwitchDcache.h"

namespace {
struct Record {
    SwitchNVENCCompletions *owner;
    switch_gpu_backing *statusBacking;  // null: free
    switch_gpu_backing *bitsBacking;
    const volatile uint8_t *status;
    const volatile uint8_t *bits;
    uint32_t bitsStartPos;
    uint32_t bitsLimit;
};
}  // namespace

struct SwitchNVENCCompletions {
    IOLock *lock;
    SwitchNVENCCompletionPort ops;
    uint32_t references;
    mach_port_t port;
    OSAsyncReference64 reference;
    Record records[kSwitchNVENCCompletionRecords];
};

namespace {
void destroy(SwitchNVENCCompletions *completions)
{
    IOLockFree(completions->lock);
    IODelete(completions, SwitchNVENCCompletions, 1);
}

void finish(Record *record, io_user_reference_t *arguments, IOReturn result)
{
    auto *completions = record->owner;
    auto *statusBacking = record->statusBacking;
    auto *bitsBacking = record->bitsBacking;
    IOLockLock(completions->lock);
    record->statusBacking = nullptr;
    record->bitsBacking = nullptr;
    if (arguments && completions->port)
        completions->ops.send(completions->reference, result, arguments, SWITCH_NVENC_COMPLETION_ARGS);
    const bool last = !--completions->references;
    IOLockUnlock(completions->lock);
    switch_gpu_backing_release(statusBacking);
    switch_gpu_backing_release(bitsBacking);
    if (last)
        destroy(completions);
}

void completed(void *context, uint32_t fence, int result)
{
    auto *record = static_cast<Record *>(context);
    SwitchNVENCOutputStatus stat = {};
    if (!result)
        switch_nvenc_invalidate_output(record->status, record->bits, record->bitsStartPos, record->bitsLimit, false,
                                       &stat);
    io_user_reference_t arguments[SWITCH_NVENC_COMPLETION_ARGS] = {fence, stat.errorStatus, stat.ucodeErrorStatus,
                                                                   stat.totalBitCount};
    finish(record, arguments, result ? kIOReturnIOError : kIOReturnSuccess);
}
}  // namespace

bool switch_nvenc_output_spec(const TegraNVENCSubmit *submit, SwitchNVENCOutputSpec *out)
{
    if (!submit || !out)
        return false;
    const TegraNVENCEntry *statusEntry = nullptr;
    const TegraNVENCEntry *bitsEntry = nullptr;
    for (uint32_t i = 0; i < submit->entry_count && i < TEGRA_NVENC_MAX_ENTRIES; ++i) {
        const auto &e = submit->entries[i];
        if (e.kind != TEGRA_NVENC_ENTRY_RELOC)
            continue;
        if (e.method == NVC5B7_SET_OUT_ENC_STATUS)
            statusEntry = &e;
        else if (e.method == NVC5B7_SET_OUT_BITSTREAM)
            bitsEntry = &e;
    }
    if (!statusEntry || !bitsEntry)
        return false;
    uint32_t limit = 0;
    uint32_t startPos = 0;
    if (submit->codec == TEGRA_NVENC_CODEC_H264) {
        if (submit->setup_bytes < sizeof(nvenc_h264_drv_pic_setup_s))
            return false;
        nvenc_h264_drv_pic_setup_s setup;
        memcpy(&setup, submit->setup, sizeof(setup));
        limit = setup.pic_control.bitstream_buf_size;
        startPos = setup.pic_control.bitstream_start_pos;
    } else if (submit->codec == TEGRA_NVENC_CODEC_H265) {
        if (submit->setup_bytes < sizeof(nvenc_h265_drv_pic_setup_s))
            return false;
        nvenc_h265_drv_pic_setup_s setup;
        memcpy(&setup, submit->setup, sizeof(setup));
        limit = setup.pic_control.bitstream_buf_size;
        startPos = setup.pic_control.bitstream_start_pos;
    } else {
        return false;
    }
    if (!limit || startPos >= limit)
        return false;
    *out = {statusEntry->value, statusEntry->offset, bitsEntry->value, bitsEntry->offset, startPos, limit};
    return true;
}

bool switch_nvenc_status_range_covered(const SwitchNVENCOutputSpec *spec, uint32_t handle, uint32_t offset,
                                       uint32_t length)
{
    if (!spec || handle != spec->statusHandle || offset > spec->statusOffset)
        return false;
    return static_cast<uint64_t>(offset) + length >=
           static_cast<uint64_t>(spec->statusOffset) + sizeof(nvenc_pic_stat_s);
}

void switch_nvenc_invalidate_output(const volatile uint8_t *statusVA, const volatile uint8_t *bitsVA,
                                    uint32_t bitsStartPos, uint32_t bitsLimit, bool statusInvalidated,
                                    SwitchNVENCOutputStatus *out)
{
    if (!statusVA)
        return;
    if (!statusInvalidated)
        switch_dcache_invalidate(statusVA, sizeof(nvenc_pic_stat_s));
    nvenc_pic_stat_s stat;
    memcpy(&stat, const_cast<const uint8_t *>(statusVA), sizeof(stat));
    if (out)
        *out = {stat.error_status, stat.ucode_error_status, stat.total_bit_count};
    const uint64_t produced = (static_cast<uint64_t>(stat.total_bit_count) + 7) / 8;
    if (!stat.ucode_error_status && produced && bitsVA && bitsStartPos < bitsLimit) {
        const uint64_t room = static_cast<uint64_t>(bitsLimit) - bitsStartPos;
        const size_t bytes = static_cast<size_t>(produced < room ? produced : room);
        switch_dcache_invalidate(bitsVA + bitsStartPos, bytes);
    }
}

bool switch_nvenc_invalidate_spec(const SwitchNVENCOutputSpec *spec, TegraNVENCResolve resolve, void *context,
                                  bool statusInvalidated, SwitchNVENCOutputStatus *out)
{
    TegraNVENCBlock statusBlock = {};
    TegraNVENCBlock bitsBlock = {};
    if (!spec || !resolve || !resolve(context, spec->statusHandle, &statusBlock) ||
        statusBlock.size < sizeof(nvenc_pic_stat_s) ||
        spec->statusOffset > statusBlock.size - sizeof(nvenc_pic_stat_s) ||
        !resolve(context, spec->bitsHandle, &bitsBlock) || bitsBlock.size < spec->bitsLimit ||
        spec->bitsOffset > bitsBlock.size - spec->bitsLimit)
        return false;
    switch_nvenc_invalidate_output(statusBlock.va + spec->statusOffset, bitsBlock.va + spec->bitsOffset,
                                   spec->bitsStartPos, spec->bitsLimit, statusInvalidated, out);
    return true;
}

SwitchNVENCCompletions *switch_nvenc_completions_create(const SwitchNVENCCompletionPort &ops)
{
    auto *completions = IONewZero(SwitchNVENCCompletions, 1);
    if (!completions)
        return nullptr;
    completions->lock = IOLockAlloc();
    if (!completions->lock) {
        IODelete(completions, SwitchNVENCCompletions, 1);
        return nullptr;
    }
    completions->ops = ops;
    completions->references = 1;
    for (auto &record : completions->records)
        record.owner = completions;
    return completions;
}

void switch_nvenc_completions_release(SwitchNVENCCompletions *completions)
{
    if (!completions)
        return;
    IOLockLock(completions->lock);
    const mach_port_t old = completions->port;
    completions->port = MACH_PORT_NULL;
    memset(completions->reference, 0, sizeof(completions->reference));
    const bool last = !--completions->references;
    IOLockUnlock(completions->lock);
    if (old != MACH_PORT_NULL)
        IOUserClient::releaseNotificationPort(old);
    if (last)
        destroy(completions);
}

IOReturn switch_nvenc_completions_listen(SwitchNVENCCompletions *completions, mach_port_t port, task_t task)
{
    if (!completions || port == MACH_PORT_NULL)
        return kIOReturnBadArgument;
    IOLockLock(completions->lock);
    const mach_port_t old = completions->port;
    completions->port = port;
    completions->ops.refer(completions->reference, port, task);
    IOLockUnlock(completions->lock);
    if (old != MACH_PORT_NULL)
        IOUserClient::releaseNotificationPort(old);
    return kIOReturnSuccess;
}

void switch_nvenc_completions_close(SwitchNVENCCompletions *completions)
{
    if (!completions)
        return;
    IOLockLock(completions->lock);
    const mach_port_t old = completions->port;
    completions->port = MACH_PORT_NULL;
    memset(completions->reference, 0, sizeof(completions->reference));
    IOLockUnlock(completions->lock);
    if (old != MACH_PORT_NULL)
        IOUserClient::releaseNotificationPort(old);
}

int switch_nvenc_completions_arm(SwitchNVENCCompletions *completions, const TegraNVENCSubmit *submit,
                                 TegraNVENCResolve resolve, void *context, SwitchNVENCCompletion *out)
{
    *out = {};
    SwitchNVENCOutputSpec spec = {};
    TegraNVENCBlock statusBlock = {};
    TegraNVENCBlock bitsBlock = {};
    if (!completions || !resolve || !switch_nvenc_output_spec(submit, &spec) ||
        !resolve(context, spec.statusHandle, &statusBlock) || !statusBlock.backing ||
        statusBlock.size < sizeof(nvenc_pic_stat_s) ||
        spec.statusOffset > statusBlock.size - sizeof(nvenc_pic_stat_s) ||
        !resolve(context, spec.bitsHandle, &bitsBlock) || !bitsBlock.backing || bitsBlock.size < spec.bitsLimit ||
        spec.bitsOffset > bitsBlock.size - spec.bitsLimit)
        return 0;
    IOLockLock(completions->lock);
    Record *record = nullptr;
    for (uint32_t i = 0; completions->port != MACH_PORT_NULL && !record && i < kSwitchNVENCCompletionRecords; ++i)
        if (!completions->records[i].statusBacking)
            record = &completions->records[i];
    const bool listening = completions->port != MACH_PORT_NULL;
    if (record) {
        record->statusBacking = statusBlock.backing;
        record->bitsBacking = bitsBlock.backing;
        record->status = statusBlock.va + spec.statusOffset;
        record->bits = bitsBlock.va + spec.bitsOffset;
        record->bitsStartPos = spec.bitsStartPos;
        record->bitsLimit = spec.bitsLimit;
        switch_gpu_backing_retain(statusBlock.backing);
        switch_gpu_backing_retain(bitsBlock.backing);
        ++completions->references;
    }
    IOLockUnlock(completions->lock);
    if (record)
        *out = {completed, record};
    return listening && !record ? -12 : 0;
}

void switch_nvenc_completions_disarm(const SwitchNVENCCompletion *armed)
{
    if (armed && armed->done == completed)
        finish(static_cast<Record *>(armed->context), nullptr, kIOReturnSuccess);
}
