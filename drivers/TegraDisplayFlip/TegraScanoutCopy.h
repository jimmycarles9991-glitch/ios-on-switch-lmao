// SPDX-License-Identifier: BSD-2-Clause-Patent
// The GPU frame copy for the hardware-rotated, multi-buffered scanout (NativeLegacyDisplay): the display hands each swapped IOSurface to the GPU driver, whose copy engine writes it, pitch-linear
// to block-linear, into a scanout buffer window A neither scans nor may take (TegraScanoutRing.h); the display flips window A to it
// once the GPU says it is done, taken at the next frame boundary. C and C++ (drivers/TegraGM20B includes it as well).
//
// Destination layout, the one window A column-scans (NativeScanout.h; hekate _di_winA_block; DC SURFACE_KIND BLOCK, BLOCK_HEIGHT 4):
// 32-bit B8G8R8A8 pixels, 1280 x 720, GOBs of 64 bytes x 8 lines (the Fermi/Maxwell GOB, byte x of line y at ((x & 32) << 3) |
// ((y & 6) << 5) | ((x & 16) << 1) | ((y & 1) << 4) | (x & 15)), 16 GOBs stacked per block (block height log2 4, 128 lines), blocks
// row-major, 80 per block row (5120 bytes / 64). 720 lines use 6 block rows; the buffer is 6 x 655360 = 0x3C0000 bytes. On the
// Maxwell copy engine that is DST_MEMORY_LAYOUT BLOCKLINEAR with block width 1 GOB, height 16 GOBs, depth 1, Fermi (8-line) GOBs,
// width 5120 bytes, height 720, depth 1, origin 0,0; the source PITCH with pitch sourceStride, 5120-byte lines, 720 of them.
// The GPU's view of the target must be plain (kind 0, uncompressed): the controller reads the bytes as they land in DRAM.
#pragma once
#include <stdint.h>

#define TEGRA_SCANOUT_COPY_VERSION      5u
#define SWITCH_SCANOUT_MAX_DEPENDENCIES 32u
struct switch_gpu_backing_token;
// flags: no copy; the completion only says the compositor's GPU work on the source is done (direct scanout: the display
// scans the source itself, through the SMMU). target, targetBytes, targetPitch and blockHeightLog2 are ignored.
#define TEGRA_SCANOUT_COPY_FENCE_ONLY 1u
// flags: the source is block-linear (tegra_gm20b_surface_layout): GOBs of 64 bytes x 8 lines, blocks 1 GOB wide and
// 2^sourceBlockHeightLog2 GOBs high, width x 4 bytes (whole GOBs) across.
#define TEGRA_SCANOUT_COPY_SOURCE_BLOCK 2u
// flags, with FENCE_ONLY: every job that wrote the source since the display began scanning it in place ended External
// (switch_gpu_backing_scanout), so a sole GM20B producer's own completion may stand for the present: the GPU driver
// queues no job and its interrupt pass completes the offer once the producer's writes are in DRAM.
#define TEGRA_SCANOUT_COPY_PRODUCER_WATCH 4u
// Tiled IOSurfaces (the GPU renders them block-linear, the display scans them as such): laid out by the GPU driver's one
// rule (drivers/TegraGM20B/TegraGM20BLayout.h, NIL's, its block height lowered to fit the allocation:
// TegraGM20BLayoutFit.h) in the surface's own pages, under the generic 16Bx2 PTE kind; rows of width x bytes per pixel
// rounded up to whole GOBs (bytes per row is ignored).
#define TEGRA_SCANOUT_TILED_KIND 0xfeu

struct TegraScanoutCopy {
    uint32_t version;        // TEGRA_SCANOUT_COPY_VERSION
    uint32_t width, height;  // pixels: 1280 x 720
    uint64_t sequence;       // increases by one per offer, from 1
    void *display;           // NativeLegacyDisplay: pass back to native_legacy_gpu_complete
    // The swapped IOSurface's memory (an IOMemoryDescriptor *): pitch-linear, 32-bit pixels, row y at byte y * sourceStride, from
    // offset 0. Retained by the display from the offer until native_legacy_gpu_complete; retain it yourself to cache a GPU mapping
    // of it beyond that. Not prepared (wired) by the display. The compositor rendered it on the GPU: the copy must be ordered after
    // that work (semaphore acquires on the producing channels), and the display does no CPU wait or cache maintenance for it.
    void *source;
    uint64_t sourceStride;  // bytes per row, >= width * 4 (5120 or padded)
    uint64_t sourceBytes;   // the surface's allocation, >= sourceStride * height
    // The block-linear target: physically contiguous, below 4 GiB, 128 KiB aligned, targetBytes long. At most three addresses ever
    // occur (the boot framebuffer 0xf5a00000 and the display's other two buffers); window A is not scanning it and cannot
    // take it while the copy runs, and the display writes it neither on the CPU nor with another copy until this one completes.
    uint64_t target;
    uint64_t targetBytes;      // 0x3C0000
    uint32_t targetPitch;      // 5120 bytes per line of pixels (80 GOBs across)
    uint32_t blockHeightLog2;  // 4: 16 GOBs per block
    uint32_t flags;            // TEGRA_SCANOUT_COPY_FENCE_ONLY (version 2)
    // TEGRA_SCANOUT_COPY_SOURCE_BLOCK: the source layout's block height, log2 GOBs (version 5)
    uint32_t sourceBlockHeightLog2;
    // The first page of another IOSurface the present also waits for the writers of, 0 for none (version 3): window B's
    // surface, when the GPU renders it and its change goes to the screen with this frame.
    uint64_t alsoWritten;
    // Typed producers remain retained by the caller until admission; the GPU driver retains accepted dependencies.
    const struct switch_gpu_backing_token *dependencies;
    uint32_t dependencyCount;
};

#ifdef __cplusplus
extern "C" {
#endif
// Provided by the GPU driver (weak in NativeLegacyDisplay: absent, every frame is copied on the CPU). Called under the display's
// work-loop gate in thread context (usually the compositor's thread inside its swap submission): queue the copy and return, never
// wait for the GPU here. 0: accepted, and native_legacy_gpu_complete follows exactly once for it. Anything else: declined (GPU
// not up, channel busy or dead, source not importable), no completion; the display copies that frame on the CPU. One offer is
// outstanding at a time: the next comes only after the previous one's completion. The copy waits only for work its producers
// had already submitted at the offer.
int tegra_gm20b_scanout_copy(const struct TegraScanoutCopy *copy);
// Provided by the GPU driver (weak in NativeLegacyDisplay): a present with no copy that completes once the work the writers
// of the IOSurface whose first page is `pa` had submitted is done (window B's surface, changing alone). The same contract
// as tegra_gm20b_scanout_copy otherwise: one offer outstanding per display, native_legacy_gpu_complete once for it.
int tegra_gm20b_scanout_fence(void *display, uint64_t pa);
// Provided by NativeLegacyDisplay: the offer's copy is done (status 0: every byte of the target is in DRAM, the copy engine's writes
// flushed past the GPU L2 before the semaphore release that raised the completion) or failed (status != 0: the target may hold a
// partial frame; the display does not flip to it). Thread context with interrupts on (a producer's work loop or work item; its
// interrupt filter calls tgm_present_filter_callout's fn instead); it records the status, writes window A's flip for a frame the
// display armed (an IOSimpleLock and six registers, SwitchCompletionFlipDisplay.h) and signals the display's work loop, and never
// sleeps.
void native_legacy_gpu_complete(void *display, int status);
// Provided by the GPU driver: fn(ctx, 0) runs in the GPU nonstall interrupt's primary filter (interrupts off, under a
// simple lock) when an accepted watched offer's producer (TEGRA_SCANOUT_COPY_PRODUCER_WATCH) is complete with its
// writes visible, before its native_legacy_gpu_complete, which follows with status 0 as ever. At most once per offer,
// never after that offer's completion; offers the producer's own pass cannot finish (a present job, a semantic wait
// in front) never call it. fn only stores to memory and registers under a simple lock. Registered once.
void tgm_present_filter_callout(void (*fn)(void *ctx, int status), void *ctx);
// status: the compositor's rendering of the source never finished (its channel faulted or hung and was killed): the source
// holds a partial frame, and the display shows nothing of it, neither through the copy nor the CPU.
#define TEGRA_SCANOUT_SOURCE_LOST (-5)
// Provided by the GPU driver (weak in NativeLegacyDisplay): whether the display's accepted offer is still the GPU driver's, its
// completion not yet delivered. The driver owns an accepted offer's deadline: 1 s after the offer it names the channel and
// seqno the copy waits for, kills a client's hung channel (the offer then completes, TEGRA_SCANOUT_SOURCE_LOST when the
// killed work was the frame's) or panics. The display's own deadline only catches a completion lost between the two: it
// waits while this says 1, up to ten of its 1 s deadlines. Any context; never waits.
int tegra_gm20b_scanout_outstanding(const void *display);
// Provided by the GPU driver (weak in NativeLegacyDisplay): whether the IOSurface whose first page is `pa` holds
// block-linear pixels. A surface's layout is decided once, at its first GPU import, and holds while any GPU client
// imports it; a CPU lock never changes what its pages hold. Thread context; never waits for the GPU.
int tegra_gm20b_surface_tiled(uint64_t pa);
// Provided by the GPU driver (weak in NativeLegacyDisplay): the display scans the IOSurface whose first page is `pa` in
// place from now until tegra_gm20b_surface_unhold. Its layout, once decided, then holds even with no GPU client importing
// it any more: the last import's release moves a block-linear surface's texels back to rows in place, under a window
// that still scans it block-linear (device boot 10, after backboardd's crash). *tiled: the layout held. Null: nothing
// to hold yet (no GPU client decided its layout). Thread context; never waits for the GPU.
void *tegra_gm20b_surface_hold(uint64_t pa, int *tiled);
void tegra_gm20b_surface_unhold(void *hold);
// Each vsync the display sends the compositor (only while it has vsync on), from the display's work loop: its stamp and
// interval (mach absolute time) and lead, the microseconds from the stamp to the frame end (0: sent at V_BLANK). The GPU
// holds app work off the GPU ahead of the vblank the frame started there aims for.
void tegra_gm20b_compositor_vsync(uint64_t stamp, uint32_t lead_us, uint64_t interval);
#ifdef __cplusplus
}
#endif
