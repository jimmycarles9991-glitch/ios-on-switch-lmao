// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stdint.h>

namespace switch_mc {
constexpr uint32_t kMcEmemArbOutstandingReq = 0x094;
constexpr uint32_t kMcEmemArbMisc0 = 0x0d8;
constexpr uint32_t kMcEmemArbRing3Throttle = 0x0e4;
constexpr uint32_t kMcLatencyAllowanceGpu0 = 0x3ac;
constexpr uint32_t kMcLatencyAllowanceGpu20 = 0x3e8;
constexpr uint32_t kMcGkPtsaRate = 0x53c;
constexpr uint32_t kMcGkPtsaMin = 0x540;
constexpr uint32_t kMcGkPtsaMax = 0x544;
constexpr uint32_t kMcGk2PtsaRate = 0x610;
constexpr uint32_t kMcGk2PtsaMin = 0x614;
constexpr uint32_t kMcGk2PtsaMax = 0x618;
constexpr uint32_t kMcEmemArbOutstandingReqRing3 = 0x66c;
constexpr uint32_t kMcEmemArbNisoThrottle = 0x6b0;
constexpr uint32_t kMcEmemArbOutstandingReqNiso = 0x6b4;
constexpr uint32_t kMcPtsaGrantDecrement = 0x960;

constexpr uint32_t kGpuReadInitLa = 25u;
constexpr uint32_t kGpuReadRefClkMhz = 800u;
constexpr uint32_t kGpuWriteInitLa = 128u;
constexpr uint32_t kGpuWriteRefClkMhz = 800u;
constexpr uint32_t kLaMaxValue = 255u;
constexpr uint32_t kGpuReadMask = 0x000000ffu;
constexpr uint32_t kGpuWriteShift = 16u;
constexpr uint32_t kGpuWriteMask = 0x00ff0000u;
constexpr uint32_t kGkPtsaMin = 0x3eu;  // (-2) & 0x3f in tegra21x_la.c t21x_init_ptsa
constexpr uint32_t kGkPtsaMax = 0x00u;

struct GpuMcPlan {
    uint32_t gpusrd;
    uint32_t gpuswr;
    uint32_t ptsaMin;
    uint32_t ptsaMax;
};

struct GpuMcSnapshot {
    uint32_t gpu0;
    uint32_t gpu2;
    uint32_t gkRate;
    uint32_t gkMin;
    uint32_t gkMax;
    uint32_t gk2Rate;
    uint32_t gk2Min;
    uint32_t gk2Max;
    uint32_t outstandingReq;
    uint32_t outstandingRing3;
    uint32_t outstandingNiso;
    uint32_t arbMisc0;
    uint32_t ring3Throttle;
    uint32_t nisoThrottle;
    uint32_t grantDec;
};

inline uint32_t scaleAllowance(uint32_t initLa, uint32_t refClkMhz, uint32_t emcFreqMhz)
{
    if (!emcFreqMhz || refClkMhz <= emcFreqMhz)
        return initLa > kLaMaxValue ? kLaMaxValue : initLa;
    const uint32_t scaled = (initLa * refClkMhz) / emcFreqMhz;
    return scaled > kLaMaxValue ? kLaMaxValue : scaled;
}

inline GpuMcPlan gpuMcPlan(uint32_t dramKhz)
{
    const uint32_t emcFreqMhz = dramKhz / 1000u;
    return {
        scaleAllowance(kGpuReadInitLa, kGpuReadRefClkMhz, emcFreqMhz),
        scaleAllowance(kGpuWriteInitLa, kGpuWriteRefClkMhz, emcFreqMhz),
        kGkPtsaMin,
        kGkPtsaMax,
    };
}

template <class Registers> void programGpuMc(Registers &regs, uint32_t dramKhz)
{
    const GpuMcPlan plan = gpuMcPlan(dramKhz);
    const uint32_t cur = uint32_t(regs[kMcLatencyAllowanceGpu0 / 4]);
    regs[kMcLatencyAllowanceGpu0 / 4] =
        (cur & ~(kGpuReadMask | kGpuWriteMask)) | plan.gpusrd | (plan.gpuswr << kGpuWriteShift);
    regs[kMcGkPtsaMin / 4] = plan.ptsaMin;
    regs[kMcGkPtsaMax / 4] = plan.ptsaMax;
}

template <class Registers> GpuMcSnapshot captureGpuMc(Registers &regs)
{
    return {
        uint32_t(regs[kMcLatencyAllowanceGpu0 / 4]),
        uint32_t(regs[kMcLatencyAllowanceGpu20 / 4]),
        uint32_t(regs[kMcGkPtsaRate / 4]),
        uint32_t(regs[kMcGkPtsaMin / 4]),
        uint32_t(regs[kMcGkPtsaMax / 4]),
        uint32_t(regs[kMcGk2PtsaRate / 4]),
        uint32_t(regs[kMcGk2PtsaMin / 4]),
        uint32_t(regs[kMcGk2PtsaMax / 4]),
        uint32_t(regs[kMcEmemArbOutstandingReq / 4]),
        uint32_t(regs[kMcEmemArbOutstandingReqRing3 / 4]),
        uint32_t(regs[kMcEmemArbOutstandingReqNiso / 4]),
        uint32_t(regs[kMcEmemArbMisc0 / 4]),
        uint32_t(regs[kMcEmemArbRing3Throttle / 4]),
        uint32_t(regs[kMcEmemArbNisoThrottle / 4]),
        uint32_t(regs[kMcPtsaGrantDecrement / 4]),
    };
}
}  // namespace switch_mc
