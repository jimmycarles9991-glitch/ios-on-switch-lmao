// SPDX-License-Identifier: BSD-2-Clause-Patent
// TegraNVENCDirect.cpp: a submit whose source surface holds a native NVDEC picture NVENC reads in place.
#pragma once
#include "TegraNVENC.h"
#include "../TegraVIC/SwitchNativeSurface.h"

enum : uint32_t {
    TEGRA_NVENC_DIRECT_HANDLE = 0xfffffff0u,  // the native picture, in the patched submit's relocations
};

struct TegraNVENCDirect {
    TegraVICSurface surface;
    SwitchNativeSurfaceView view;
    void *map;
    TegraNVENCBlock block;  // the picture's backing in NVENC's address space
    switch_gpu_backing_token token;
    switch_gpu_backing_use uses[2];  // the transaction's: it reads them again at commit or abort, after begin returned
    switch_gpu_backing_transaction transaction;
    TegraNVENCOrder order;  // for tegra_nvenc_run
};

// 0 with `patched` the submit reading the native picture (TEGRA_NVENC_DIRECT_HANDLE) and `direct` holding it; -95 when
// the source is no native picture NVENC reads as it is (`why` says which, or null for no native picture at all).
int tegra_nvenc_direct_prepare(const TegraNVENCSubmit *submit, task_t task, TegraNVENCDirect *direct,
                               TegraNVENCSubmit *patched, const char **why);
void tegra_nvenc_direct_release(TegraNVENCDirect *direct);
