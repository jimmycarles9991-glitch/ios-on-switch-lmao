// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "TegraVIC.h"
#include "SwitchVICQueue.h"
#include "../TegraGM20B/SwitchGpuBacking.h"

constexpr uint32_t kSwitchVICConfigStride = 4096;
constexpr uint32_t kSwitchVICConfigBytes = SwitchVICQueue::kSlots * kSwitchVICConfigStride;

bool switch_vic_jobs_start(const TegraVICBuffer &configs, uint32_t syncpt, uint32_t completed);
// New jobs wait until switch_vic_jobs_resume. drain: once the queued ones are done; else only if none is queued and no
// cancellation is under way.
bool switch_vic_jobs_park(bool drain);
void switch_vic_jobs_resume(bool ready, uint32_t completed);

// A stalled job's cancellation (TegraVICCancel.cpp). Stalled: the first failed fence of a stall, on host1x's work loop;
// new jobs wait from then until the cancellation has renumbered the queue.
void switch_vic_job_stalled(uint64_t fence, int error, uint64_t elapsedUs, int producers);
// The engine has stopped: every queued batch the syncpoint has not reached fails, recorded as failed, and its
// completion (a display frame's conversion) gets the failure. Returns the last job's fence, through which the caller
// then releases the syncpoint.
uint32_t switch_vic_jobs_cancel(uint32_t reached);
// The engine runs again (or does not: VIC then refuses work). Once the cancelled jobs have ended, the queue is
// renumbered from the syncpoint and new jobs go on.
void switch_vic_jobs_recovered(bool ready);
// Sleep: returns once no cancellation is under way.
void switch_vic_jobs_settle();

extern "C" int switch_vic_wait(uint32_t fence);
// switch_gpu_backing_token_filter_status for VIC's tokens (SwitchGpuBacking.h).
extern "C" int switch_vic_token_filter_status(const switch_gpu_backing_token *token);
int switch_vic_submit_batch(const TegraVICJob *jobs, uint32_t count, const TegraVICAcquire *acquire,
                            switch_gpu_backing_use *uses, uint32_t useCount, void (*completion)(void *, int),
                            void *context, uint32_t *fence);

// Supplied ownership leases remain held through commit and final completion of the conversion.
int switch_vic_submit_owned(const TegraVICJob *jobs, uint32_t count, const TegraVICAcquire *acquire,
                            switch_gpu_backing_use *uses, uint32_t useCount, switch_gpu_cpu_lease *const *leases,
                            uint32_t leaseCount, void (*completion)(void *, int), void *context, uint32_t *fence);

struct SwitchVICEvents;

struct SwitchVICOwnership {
    switch_gpu_backing_use *uses;
    uint32_t useCount;
    const switch_gpu_backing_guard *guards;
    uint32_t guardCount;
    switch_gpu_cpu_lease *const *leases;
    uint32_t leaseCount;
};

int switch_vic_submit_checked(const TegraVICJob *jobs, uint32_t count, const TegraVICAcquire *acquire,
                              const SwitchVICOwnership &ownership, void (*completion)(void *, int), void *context,
                              uint32_t *fence, SwitchVICEvents *events = nullptr);
// written: the batch's token, by which a reader of what it wrote learns the batch's failure.
int switch_vic_submit_checked(const TegraVICJob *jobs, uint32_t count, const TegraVICAcquire *acquire,
                              const SwitchVICOwnership &ownership, void (*completion)(void *, int), void *context,
                              uint32_t *fence, SwitchVICEvents *events, switch_gpu_backing_token *written);
// Debug kernels: which check of switch_vic_submit_checked refused the last job it refused.
const char *switch_vic_last_refusal();
