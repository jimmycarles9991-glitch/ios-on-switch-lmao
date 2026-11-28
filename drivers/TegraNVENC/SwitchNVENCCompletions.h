// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <IOKit/IOUserClient.h>
#include "SwitchNVENCFlight.h"
#include "TegraNVENC.h"

struct SwitchNVENCCompletions;

struct SwitchNVENCCompletionPort {
    IOReturn (*send)(OSAsyncReference64 reference, IOReturn result, io_user_reference_t *arguments, UInt32 count);
    void (*refer)(OSAsyncReference64 reference, mach_port_t port, task_t task);
};

struct SwitchNVENCOutputSpec {
    uint32_t statusHandle, statusOffset;
    uint32_t bitsHandle, bitsOffset, bitsStartPos, bitsLimit;
};

struct SwitchNVENCOutputStatus {
    uint32_t errorStatus;
    uint32_t ucodeErrorStatus;
    uint32_t totalBitCount;
};

constexpr uint32_t kSwitchNVENCCompletionRecords = 4 * kSwitchNVENCInFlight;

bool switch_nvenc_output_spec(const TegraNVENCSubmit *submit, SwitchNVENCOutputSpec *out);
bool switch_nvenc_status_range_covered(const SwitchNVENCOutputSpec *spec, uint32_t handle, uint32_t offset,
                                       uint32_t length);
void switch_nvenc_invalidate_output(const volatile uint8_t *statusVA, const volatile uint8_t *bitsVA,
                                    uint32_t bitsStartPos, uint32_t bitsLimit, bool statusInvalidated,
                                    SwitchNVENCOutputStatus *out);
bool switch_nvenc_invalidate_spec(const SwitchNVENCOutputSpec *spec, TegraNVENCResolve resolve, void *context,
                                  bool statusInvalidated, SwitchNVENCOutputStatus *out);

SwitchNVENCCompletions *switch_nvenc_completions_create(const SwitchNVENCCompletionPort &port);
void switch_nvenc_completions_release(SwitchNVENCCompletions *completions);
IOReturn switch_nvenc_completions_listen(SwitchNVENCCompletions *completions, mach_port_t port, task_t task);
void switch_nvenc_completions_close(SwitchNVENCCompletions *completions);
int switch_nvenc_completions_arm(SwitchNVENCCompletions *completions, const TegraNVENCSubmit *submit,
                                 TegraNVENCResolve resolve, void *context, SwitchNVENCCompletion *out);
void switch_nvenc_completions_disarm(const SwitchNVENCCompletion *armed);
