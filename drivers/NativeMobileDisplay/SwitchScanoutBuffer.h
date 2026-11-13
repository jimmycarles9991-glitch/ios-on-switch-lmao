// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stddef.h>
#include "../TegraGM20B/SwitchGpuBacking.h"

switch_gpu_backing *switch_display_scanout_buffer(size_t bytes);
