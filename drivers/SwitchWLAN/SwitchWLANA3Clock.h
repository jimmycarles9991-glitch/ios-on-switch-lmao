/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef SWITCH_WLAN_A3_CLOCK_H
#define SWITCH_WLAN_A3_CLOCK_H

#include "SwitchWLANA3Radio.h"
#include "firmware/a3/wire.h"

struct SwitchWLANA3Job {
    switch_a3_control control;
    uint8_t payload[sizeof(switch_a3_schedule)];
    uint32_t lifetime;
#if DEVELOPMENT || DEBUG
    uint32_t trial_epoch;
    uint8_t trial_boundary;
#endif
};

int switch_wlan_a3_exchange(const SwitchWLANA3Job &job, switch_a3_reply &reply, uint64_t &before, uint64_t &after);
bool switch_wlan_a3_clock_valid(const swlan_awdl_clock_sample &sample, uint64_t now_ns);
int switch_wlan_a3_firmware_error(int status);
int switch_wlan_a3_calibrate(SwitchWLANA3Job job, const swlan_awdl_clock_sample &previous,
                             swlan_awdl_clock_sample &sample, switch_a3_reply &reply,
                             bool (*current)(uint32_t, uint32_t, uint32_t));

#endif
