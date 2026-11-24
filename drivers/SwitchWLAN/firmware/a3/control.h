/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef SWITCH_A3_CONTROL_H
#define SWITCH_A3_CONTROL_H
#include <stdint.h>

void switch_a3_control_initialize(void *wlc);
void switch_a3_control_up(void);
void switch_a3_control_down(void);
uint32_t switch_a3_control_features(void);
void switch_a3_control_retired(void);
void switch_a3_control_fault(int error);
struct switch_a3_timer;
struct switch_a3_owner;
void switch_a3_control_timer_fault(int error, const struct switch_a3_timer *timer, const struct switch_a3_owner *owner);
#if DEVELOPMENT || DEBUG
void switch_a3_control_record(uint16_t reason, int error, const struct switch_a3_owner *owner,
                               const struct switch_a3_timer *timer, const uint64_t *actual, uint32_t channels);
#else
#define switch_a3_control_record(...) ((void)0)
#endif
int32_t switch_a3_control_request(const void *input, uint32_t input_length, void *output, uint32_t output_length);
int32_t switch_a3_control_deadline(void *output, uint32_t output_length);

#endif
