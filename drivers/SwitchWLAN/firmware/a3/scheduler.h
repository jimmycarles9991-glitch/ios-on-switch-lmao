/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef SWITCH_A3_SCHEDULER_H
#define SWITCH_A3_SCHEDULER_H
#include "wire.h"
#include "fault.h"

int switch_a3_scheduler_validate(void *wlc, const struct switch_a3_schedule *schedule, int allow_empty);
int switch_a3_scheduler_quiesce(void);
int switch_a3_scheduler_home_ready(void *wlc);
uint16_t switch_a3_scheduler_fault_reason(void);
const struct switch_a3_owner *switch_a3_scheduler_owner(void);
int switch_a3_scheduler_install(void *wlc, const struct switch_a3_schedule *schedule);
int switch_a3_scheduler_start(void *wlc, uint16_t *chanspec);
int switch_a3_scheduler_run(void *wlc, uint32_t session, uint32_t epoch, uint32_t generation,
                            void (*changed)(void *, int), void *context);
int switch_a3_scheduler_stop(void);
int switch_a3_scheduler_admit(void *wlc, uint16_t chanspec, uint64_t *until);
void switch_a3_scheduler_observed(void *wlc);

#endif
