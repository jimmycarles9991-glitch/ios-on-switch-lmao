// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "SwitchNativeScanout.h"

// Source plane bases are offsets within the captured allocation. The caller keeps that snapshot through admission.
int switch_vic_scanout_convert(const SwitchNativeScanout *source, const TegraVICImage *image,
                               switch_gpu_backing *destination, uint32_t width, uint32_t height, uint32_t pitch,
                               const TegraVICOp *operation, SwitchNativeScanout *output);

constexpr uint32_t kSwitchVICPlaceReads = 2;

// Backings the job orders after without reading them: their producers' work is done when the job's is.
struct SwitchVICPlaceReads {
    switch_gpu_backing *backings[kSwitchVICPlaceReads];
    uint32_t count;
};

// switch_vic_scanout_convert into a single-plane destination the caller lays out (`layout`: format, size, pitch, rows,
// span from base[0], block linear or not); output's producer is the job's.
int switch_vic_scanout_place(const SwitchNativeScanout *source, const TegraVICImage *image,
                             switch_gpu_backing *destination, const TegraVICImage *layout, const TegraVICOp *operation,
                             const SwitchVICPlaceReads &also, SwitchNativeScanout *output);
