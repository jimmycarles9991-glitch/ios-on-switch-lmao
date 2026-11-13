// SPDX-License-Identifier: MIT
#include "../SwitchGM20B.h"
#include "SwitchGM20BCaps.h"

uint32_t switch_gm20b_kernel_check(void);

uint32_t switch_gm20b_kernel_check(void)
{
    return switch_gm20b_formats[0].tic0 ^ NVB197_WAIT_FOR_IDLE ^ SWITCH_GM20B_LIMIT_COLOR_TARGETS;
}
