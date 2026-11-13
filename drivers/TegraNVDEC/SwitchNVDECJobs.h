// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stdint.h>

struct switch_dma_buffer;
struct switch_gpu_backing_token;

bool switch_nvdec_jobs_start(const switch_dma_buffer &setup, uint32_t syncpt, uint32_t completed);
// New jobs wait until switch_nvdec_jobs_resume. drain: once the queued ones are done; else only if none is queued and
// no cancellation is under way.
bool switch_nvdec_jobs_park(bool drain);
// Idle power gating (TegraNVDEC.cpp): a submission holds the powered engine (on again first if it was off), and the
// queue draining starts the idle timer.
int switch_nvdec_power_use();
void switch_nvdec_power_done();
void switch_nvdec_power_idle();
void switch_nvdec_jobs_resume(bool ready, uint32_t completed);
// A job's fence or bridge could not be armed.
[[noreturn]] void switch_nvdec_job_fault(uint32_t fence, int error, uint64_t elapsedUs);

// A stalled job's cancellation (TegraNVDECCancel.cpp). Stalled: the first failed fence of a stall, on host1x's work
// loop; new jobs wait from then until the cancellation has renumbered the queue.
void switch_nvdec_job_stalled(uint32_t fence, int error, uint64_t elapsedUs);
// The engine has stopped: every queued job the syncpoint has not reached fails, recorded as failed. Returns the last
// job's fence, through which the caller then releases the syncpoint.
uint32_t switch_nvdec_jobs_cancel(uint32_t reached);
// The engine runs again (or does not: NVDEC then refuses work). Once the cancelled jobs have ended, the queue is
// renumbered from the syncpoint and new jobs go on.
void switch_nvdec_jobs_recovered(bool ready);
// Sleep: returns once no cancellation is under way.
void switch_nvdec_jobs_settle();
// switch_gpu_backing_token_filter_status for NVDEC's tokens (SwitchGpuBacking.h).
extern "C" int switch_nvdec_token_filter_status(const switch_gpu_backing_token *token);
