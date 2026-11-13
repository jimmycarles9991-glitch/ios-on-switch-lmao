// SPDX-License-Identifier: MIT
#include "kits/SwitchGM20B/SwitchGM20BCaps.h"

uint32_t switch_gm20b_caps_c_count(void);

uint32_t switch_gm20b_caps_c_count(void)
{
    return sizeof(switch_gm20b_formats) / sizeof(switch_gm20b_formats[0]);
}
