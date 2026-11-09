// SPDX-License-Identifier: BSD-2-Clause-Patent
// NVDEC's pictures into VideoToolbox's buffers on VIC (TegraVICPresent.cpp), for TegraNVDEC's Present method.
//
// NVDEC writes NV12 in Tegra block linear (16Bx2 sectors, GOB height 2) into its own buffers; VideoToolbox's clients
// want pitch-linear '420v'/'420f' IOSurfaces. VIC copies one into the other (the 4:2:0 pass-through,
// read as block linear: TegraVICTraits.blockLinearIn), so the CPU neither de-swizzles nor touches either buffer, and
// the IOSurface is in DRAM, with no CPU line of it left dirty or stale, when the call returns: a GPU that does not
// snoop the CPU's caches reads what VIC wrote.
//
// Synchronous on the caller's thread (the decoder's, which sleeps on VIC's syncpoint interrupt), serialised with the
// IOSurfaceAccelerator jobs by the engine lock. Only the destination rows the picture covers are mapped and kept
// coherent, so a tile of a grid image costs its own rows, not the canvas. The IOSurface is looked up in `task` (the
// caller's), as Apple's scaler looks surfaces up. A refusal (why set) costs no engine time.
#ifndef TEGRA_VIC_PRESENT_H
#define TEGRA_VIC_PRESENT_H
#include <kern/task.h>
#include <stdint.h>

typedef struct {
    const uint64_t *pages;  // the picture's memory, 4 KiB pages in order (an NVDEC buffer's page list)
    uint32_t pageCount;
    uint64_t bytes;
    uint32_t pitch, rows;      // bytes per row (whole 64-byte GOBs), luma rows (whole 32-row blocks pairs)
    uint32_t chromaOffset;     // the CbCr plane, 256-byte aligned, rows / 2 rows of pitch bytes
    uint32_t blockHeightLog2;  // GOBs a block: NVDEC's GOB_2 is 1
    uint32_t cropX, cropY;     // the displayed window's corner (even), in samples
    uint32_t width, height;    // its size (even); clipped to the destination's
    uint32_t dstX, dstY;       // where in the destination it goes (even; dstX a multiple of 256 bytes but 0)
    uint32_t surfaceID;        // a '420v' or '420f' IOSurface of `task`; with 16-bit samples 'x420' or 'xf20'
    uint32_t sampleBytes;      // 1, or 2 for 16-bit samples (NVDEC's Main 10, P010)
} TegraVICPicture;

#ifdef __cplusplus
extern "C" {
#endif
// VIC is published (V3) and copies block-linear 4:2:0 exactly (kTegraVICTraits).
bool tegra_vic_present_ready(void);
void tegra_vic_present_start(void);  // V3: presents are taken from here on
// 0 when the picture is in the IOSurface; -19 VIC not ready, -22 bad surface ID, -95 a shape VIC does not take, -12 no
// memory, -60 the job did not finish; `why` says which (static text).
int tegra_vic_present(const TegraVICPicture *picture, task_t task, const char **why);
#ifdef __cplusplus
}
#endif
#endif
