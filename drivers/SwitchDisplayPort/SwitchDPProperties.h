// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "SwitchDPModes.h"

class OSDictionary;
class OSArray;
class OSString;

OSString *switch_dp_edid_uuid(const uint8_t *edid);
OSDictionary *switch_dp_timing_element(const struct switch_dp_mode *mode, uint32_t id, OSArray *colors);
OSDictionary *switch_dp_color_element();
