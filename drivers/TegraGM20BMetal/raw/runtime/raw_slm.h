// SPDX-License-Identifier: MIT
// Shader local memory for the raw channel: the threads' scratch and the call/return stack's spill, sized as NVK sizes it
// (nvk_slm_area_ensure). NVK sets it from its queue's state at a submission, which neither the pack's context init (NVK's
// queue creation) nor a captured command buffer carries, so the raw path sets it itself before a program that asks for it
// runs, for the pack's programs and kernels and runtime-made records alike. A GM20B pixel program whose header asks for local
// memory the channel was never given traps at its bind (device boot 7: DS_HWW_ESR 0xa0000040, SPH6_ERR at M_PIPE_STAGE PS).
// The methods are the runtime's (raw_owned.h): a probe's command buffers carry its own process's area.
#pragma once
#include <cstdint>
#include <cstring>
#include <vector>
#include "raw_push.h"

namespace raw {

constexpr uint32_t kSlmWarpsPerTpc = 64;  // GM20B: 64 warps an MP, one MP a TPC (nvkmd_darwin's device info)

constexpr uint64_t slmAlign(uint64_t v, uint64_t a)
{
    return (v + a - 1) & ~(a - 1);
}

// What a warp of a program needs: its threads' local memory and its CRS spill; 0: none
constexpr uint64_t slmPerWarp(uint64_t perThread, uint64_t crsPerWarp)
{
    return perThread || crsPerWarp ? slmAlign(perThread * 32 + crsPerWarp, 0x200) : 0;
}

// A render program's header (SPHv3, either type): SHADER_LOCAL_MEMORY_LOW_SIZE 55:32, HIGH_SIZE 87:64, CRS_SIZE 119:96
constexpr uint64_t slmPerWarpOfHeader(const uint32_t sph[4])
{
    return slmPerWarp((sph[1] & 0xffffff) | uint64_t(sph[2] & 0xffffff) << 32, sph[3] & 0xffffff);
}

inline uint64_t slmPerWarpOfHeader(const uint8_t *header)
{
    uint32_t sph[4];
    memcpy(sph, header, sizeof sph);
    return slmPerWarpOfHeader(sph);
}

// A program's header is its stage's (SPH_TYPE 4:0, SHADER_TYPE 13:10): VTG and VERTEX (1, 1), PS and PIXEL (2, 5). One that is
// not would trap at its bind as a header asking for more than the channel has does.
inline bool headerIsStage(const std::vector<uint8_t> &image, bool pixel)
{
    uint32_t w0 = 0;
    if (image.size() < 80)
        return false;
    memcpy(&w0, image.data(), sizeof w0);
    return (w0 & 0x1f) == (pixel ? 2u : 1u) && ((w0 >> 10) & 0xf) == (pixel ? 5u : 1u);
}

// A kernel's QMD (QMDV01_07): SHADER_LOCAL_MEMORY_LOW_SIZE 1463:1440, HIGH_SIZE 1495:1472, CRS_SIZE 1527:1504. Read
// bytewise: the packed pack records hold their QMDs at even offsets (PackKernel's at 18).
inline uint64_t slmPerWarpOfQmd(const void *qmd)
{
    uint32_t w[3];
    memcpy(w, static_cast<const uint8_t *>(qmd) + 45 * sizeof(uint32_t), sizeof w);
    return slmPerWarp((w[0] & 0xffffff) | uint64_t(w[1] & 0xffffff) << 32, w[2] & 0xffffff);
}

struct SlmArea {
    uint64_t perWarp = 0, perTpc = 0, bytes = 0;
};

constexpr SlmArea slmArea(uint64_t perWarp, uint32_t tpcs)
{
    SlmArea a;
    a.perWarp = perWarp;
    a.perTpc = slmAlign(perWarp * kSlmWarpsPerTpc, 0x8000);
    a.bytes = slmAlign(a.perTpc * tpcs, 0x20000);
    return a;
}

constexpr uint32_t kSlmWords = 18;

// The area at `va` for the 3D class (subchannel 0) and the compute class (1), as nvk_queue_slm_state_update sets it, after the
// work before is idle: a submission before may still run with the area it had
inline void emitSlm(Push &p, uint64_t va, const SlmArea &a)
{
    constexpr uint32_t kWaitForIdle = 0x0110, kComputeNonThrottledA = 0x02e4, kComputeThrottledA = 0x02f0,
                       kLocalMemoryA = 0x0790;
    p.m(kWaitForIdle, 0);
    p.inc(kLocalMemoryA,
          {uint32_t(va >> 32), uint32_t(va), uint32_t(a.bytes >> 32), uint32_t(a.bytes), uint32_t(a.perWarp)});
    p.inc(kLocalMemoryA, {uint32_t(va >> 32), uint32_t(va)}, SUBC_COMPUTE);
    p.inc(kComputeNonThrottledA, {uint32_t(a.perTpc >> 32), uint32_t(a.perTpc), 0xff}, SUBC_COMPUTE);
    p.inc(kComputeThrottledA, {uint32_t(a.perTpc >> 32), uint32_t(a.perTpc), 0xff}, SUBC_COMPUTE);
}

}  // namespace raw
