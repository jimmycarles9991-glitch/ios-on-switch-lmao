// SPDX-License-Identifier: BSD-2-Clause-Patent
// A connection's job completions (TegraNVDECABI.h, SWITCH_NVDEC_COMPLETION_PORT). Each armed job reads its
// nvdec_status_s when its retirement runs (SwitchNVDECQueue.h) and sends it to the port the client registered. The
// armed job holds the status buffer's backing, so a buffer freed meanwhile stays mapped until it is read, and holds
// the record set, which outlives the connection.
#pragma once
#include <IOKit/IOUserClient.h>
#include "SwitchNVDECQueue.h"
#include "TegraNVDEC.h"

struct SwitchNVDECCompletions;

// IOUserClient's async-result statics, protected: the user client hands them over.
struct SwitchNVDECCompletionPort {
    IOReturn (*send)(OSAsyncReference64 reference, IOReturn result, io_user_reference_t *arguments, UInt32 count);
    void (*refer)(OSAsyncReference64 reference, mach_port_t port, task_t task);
};

SwitchNVDECCompletions *switch_nvdec_completions_create(const SwitchNVDECCompletionPort &port);
void switch_nvdec_completions_release(SwitchNVDECCompletions *completions);  // the connection's reference
// The registered port. It keeps one send right, the last registration's: a replaced port's right is released, and so
// is the earlier right to a port registered again. A refusal takes nothing (the MIG server releases the call's right).
IOReturn switch_nvdec_completions_listen(SwitchNVDECCompletions *completions, mach_port_t port, task_t task);
void switch_nvdec_completions_close(SwitchNVDECCompletions *completions);  // later completions are sent nowhere
// One submit's completion: empty while no port listens or when the submit names no status record (the checker refuses
// that one). -12 when every record is armed.
int switch_nvdec_completions_arm(SwitchNVDECCompletions *completions, const TegraNVDECSubmit *submit,
                                 TegraNVDECResolve resolve, void *context, SwitchNVDECCompletion *out);
void switch_nvdec_completions_disarm(const SwitchNVDECCompletion *armed);  // its submit failed
