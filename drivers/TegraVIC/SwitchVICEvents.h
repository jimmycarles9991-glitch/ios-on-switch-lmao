// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "TegraVICRequest.h"
#include "../TegraGM20B/SwitchGpuSharedEvent.h"

struct SwitchVICEvents;
int switch_vic_events_create(void *task, const TegraVICRequest &, void (*ready)(void *, int), void *,
                             SwitchVICEvents **);
void switch_vic_events_arm(SwitchVICEvents *);
void switch_vic_events_release(SwitchVICEvents **);
int switch_vic_events_prepare(SwitchVICEvents *);
uint32_t switch_vic_events_tokens(SwitchVICEvents *, const switch_gpu_backing_token **);
switch_gpu_event_waits *switch_vic_events_batch(SwitchVICEvents *);
int switch_vic_events_bind(SwitchVICEvents *, const switch_gpu_backing_token *);
void switch_vic_events_commit(SwitchVICEvents *);

void switch_vic_events_submission(SwitchVICEvents *, bool final, switch_gpu_event_submission *);
int switch_vic_events_stage(SwitchVICEvents *, SwitchVICEvents **);
