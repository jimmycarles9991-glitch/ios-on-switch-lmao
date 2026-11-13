// SPDX-License-Identifier: MIT
// MAXWELL_COMPUTE_B (class b1c0) and its QMD (version 01_07, MW() fields read with drf.h's DRF_MW accessors).
#pragma once
#include "../../third_party/mesa/src/nouveau/headers/drf.h"
#include "../../third_party/mesa/src/nouveau/headers/nvidia/classes/clb1c0.h"
#include "../../third_party/mesa/src/nouveau/headers/nvidia/classes/clb1c0qmd.h"

// A QMD field's 32-bit word, and its lowest bit in that word
#define SWITCH_NVB1C0_QMD_WORD(field)  (DRF_LO(DRF_MW(field)) / 32)
#define SWITCH_NVB1C0_QMD_SHIFT(field) (DRF_LO(DRF_MW(field)) % 32)
