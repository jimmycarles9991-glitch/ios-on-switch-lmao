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
// wait for the GPU here. 0: accepted, and native_legacy_gpu_complete follows exactly once for it. TEGRA_SCANOUT_SOURCE_*:
// refused, no completion, for why below: nothing of the frame is shown, the CPU copy included (LOST panics). kIOReturnCannotLock:
// a writer holds the source, and the display offers it again once the writer leaves. Anything else: declined (GPU not up, channel
// busy or dead, source not importable), no completion; VIC copies that frame, or the CPU while VIC is not up. One offer is
// outstanding at a time: the next comes only after the previous one's completion. The copy waits only for work its producers
// had already submitted at the offer.
int tegra_gm20b_scanout_copy(const struct TegraScanoutCopy *copy);
// Provided by the GPU driver (weak in NativeLegacyDisplay): a present with no copy that completes once the work the writers
// of the IOSurface whose first page is `pa` had submitted is done (window B's surface, changing alone). The same contract
// as tegra_gm20b_scanout_copy otherwise: one offer outstanding per display, native_legacy_gpu_complete once for it.
int tegra_gm20b_scanout_fence(void *display, uint64_t pa);
// Provided by NativeLegacyDisplay: the offer's copy is done (status 0: every byte of the target is in DRAM, the copy engine's writes
// flushed past the GPU L2 before the semaphore release that raised the completion) or its frame is not there (a
// TEGRA_SCANOUT_SOURCE_*: the display does not flip to it; any other status after an accepted offer panics). Thread context with interrupts on (a producer's work loop or work item; its
// interrupt filter calls tgm_present_filter_callout's fn instead); it records the status, writes window A's flip for a frame the
// display armed (an IOSimpleLock and six registers, SwitchCompletionFlipDisplay.h) and signals the display's work loop, and never
// sleeps.
void native_legacy_gpu_complete(void *display, int status);
// Provided by the GPU driver: fn(ctx, 0) runs in a GPU interrupt's primary filter (interrupts off; the GPU driver takes
// no lock there) when an accepted watched offer's producer (TEGRA_SCANOUT_COPY_PRODUCER_WATCH) is complete with its
// writes visible, before its native_legacy_gpu_complete, which follows with status 0 as ever. At most once per offer;
// a filter run that found the offer's producer visible may still be calling fn on one CPU while the offer's completion
// is delivered on another, so fn checks the offer it writes for; offers the producer's own pass cannot finish (a present
// job, a semantic wait in front) never call it. fn only stores to memory and registers under a simple lock of its own.
// Registered once.
void tgm_present_filter_owner(const void *display);
void tgm_present_filter_callout(void (*fn)(void *ctx, int status), void *ctx);
// Why a swapped frame is not there (an offer's answer or its completion's status), the GPU driver's SwitchGpuBacking.h
// WRITER_GONE, WRITER_FAILED and WRITE_REFUSED. The display shows nothing of the frame, through the copy or the CPU, and
// retires its swap unshown, the panel keeping the frame before it.
// GONE: the client whose work was to write it is gone (its channel torn down, an app's channel killed by recovery, a
// shared event its work waited on failed with its signaller). Not a display failure.
#define TEGRA_SCANOUT_SOURCE_GONE (-3)
// LOST: accepted work for it failed on the GPU (a compositor channel killed by recovery, an engine that names no cause):
// the kernel's own failure, which no frame may hide.
#define TEGRA_SCANOUT_SOURCE_LOST (-5)
// REFUSED: the write was never accepted (its submission refused, its writer promise given up); its client was told.
#define TEGRA_SCANOUT_SOURCE_REFUSED (-8)
// Provided by the GPU driver (weak in NativeLegacyDisplay): whether the display's accepted offer is still the GPU driver's, its
// completion not yet delivered. The driver owns an accepted offer's deadline: 1 s after the offer it names the channel and
// seqno the copy waits for, kills a client's hung channel (the offer then completes with the kill's TEGRA_SCANOUT_SOURCE_*
// when the killed work was the frame's) or panics. The display's own deadline, 10 s after the offer, only catches a completion
// lost between the two, and says whether this still said 1. Any context; never waits.
int tegra_gm20b_scanout_outstanding(const void *display);
// Provided by the GPU driver, debug kernels (weak in NativeLegacyDisplay): window A shows a frame no offer carried (a
// direct frame flipped from the kick, a CPU, VIC or blank frame), so the scanout read-back has no frame before the next
// offer to re-read (TegraGM20BReadbackABI.h). Under the display's gate, in thread context.
void tegra_gm20b_scanout_bypassed(void);
void tegra_gm20b_scanout_presented(uint64_t queued_ns);
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
