#include "NativeScanout.h"  // NativeLegacyRequest.h plus the Switch's rotated scanout
// SPDX-License-Identifier: BSD-2-Clause-Patent
// Reuse the unchanged original generic framebuffer implementation. Unknown
// backend leaves stop; this class is not yet a published system display.
#include "OriginalLegacyFramebufferABI.h"
#include <IOKit/IOLib.h>
#include <os/log.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOFilterInterruptEventSource.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IODeviceMemory.h>
#include <kern/clock.h>
#include <kern/sched_prim.h>
#include <kern/thread.h>
#include <kern/thread_call.h>
#include <sys/kdebug.h>
#include <IOKit/pwr_mgt/IOPM.h>
#include <IOKit/pwr_mgt/IOPMpowerState.h>
#include <libkern/c++/OSData.h>
#include <pexpert/pexpert.h>
#include "../ArmGIC/SwitchInterrupts.h"
#include "../../kits/SwitchDMA/SwitchDcache.h"
#include "../../kits/SwitchDMA/SwitchDMABuffer.h"
#include "../../kits/SwitchDT/SwitchDTProperty.h"
#include "../../kits/SwitchCAR/SwitchCAR.h"
#include "../../kits/SwitchMMIO/SwitchMMIOWords.h"
#include "../../kits/SwitchSysctl/SwitchSysctl.h"
#include "../TegraDisplayFlip/TegraScanoutCopy.h"
#include "../TegraDisplayFlip/TegraScanoutRing.h"
#include "../TegraDisplayFlip/SwitchFrameTimeline.h"
#include "../TegraDisplayFlip/SwitchDisplayPriority.h"
#include "../TegraSMMU/TegraSMMU.h"
#include "../TegraSMMU/TegraSMMUDomain.h"
#include "DisplaySmmuProbe.h"
#include "NativePlaneFence.h"
#include "NativePlanes.h"
#include "SwitchCompletionFlip.h"
#include "SwitchDisplayComposition.h"
#include "SwitchDisplayMemoryEnvelope.h"
#include "SwitchDisplayMemoryTiming.h"
#include "SwitchDisplayRetirement.h"
#include "SwitchScanoutBuffer.h"
#include "SwitchUnderflowAccounting.h"
#include "../TegraVIC/SwitchNativeVICImage.h"
#include "../TegraVIC/SwitchVICScanoutConvert.h"
#include "SwitchPlaneComposite.h"
#include "SwitchFrameInfo.h"
#include "SwitchFrameStall.h"
#include "SwitchInteraction.h"
#include "SwitchSBIM.h"
#include "SwitchTimedPresent.h"
#include "../SwitchSuspend/SwitchSleepSteps.h"
#include "../TegraVIC/TegraVICConfig.h"
#include "../TegraVIC/TegraVICSurface.h"
#include "../TegraVIC/SwitchNativeScanout.h"
#include "../TegraGM20B/SwitchGpuBridge.h"
#include "../TegraGM20B/SwitchGpuReady.h"
#include "../TegraHost1x/SwitchHost1xFence.h"
#include "../TegraHost1x/SwitchHost1xTokenFence.h"
#include "../TegraHost1x/TegraHost1x.h"
#include "SwitchNativePlane.h"
#include "SwitchPlaneSlot.h"
#include "TegraDisplayColor.h"
#include "TegraDisplayPower.h"
#include "VsyncPhase.h"
#include "../SwitchPower/SwitchPower.h"
#include "../SwitchPower/SwitchEMC.h"
#include "../SwitchPower/SwitchDisplayBandwidth.h"
#include "../../kits/SwitchRefusal/SwitchRefusal.h"
#include "SwitchLandscape.h"
#include "SwitchLandscapeWindows.h"
#include "SwitchSurfaceUse.h"
#if SWITCH_FRAME_TIMELINE
#include <sys/sysctl.h>
#include "NativeColumnScan.h"
#include "DisplayWindowProbeRun.h"
#include "SwitchLandscapeRunABI.h"
#include "../TegraVIC/SwitchVICTurnRun.h"
extern "C" void native_legacy_frame_end_hold(void);
extern "C" void native_legacy_frame_end_release(void);
#endif

// The last swap Core Animation offered (validateSwap, accepted or not; mach time): while swaps keep coming CA is
// animating, and TegraGM20B's idle clock never drops the GPU (native_display_last_swap_ns below).
static uint64_t gLastSwapAbs;
extern "C" IOService *native_legacy_display_allocate(void);
extern "C" bool native_legacy_gop_mapping_observed(IOService *, IOMemoryMap *, uint32_t, uint32_t);
extern "C" void native_legacy_registration_observed(IOService *);
extern "C" void native_legacy_swap_stage(unsigned);
extern "C" void native_legacy_client_enter(task_t, uint32_t);
extern "C" void native_legacy_client_observed(IOReturn, IOUserClient *);
extern "C" void native_legacy_pm_observed(unsigned, IOReturn, unsigned long);
extern "C" IOMemoryDescriptor *native_legacy_surface_descriptor(OSObject *);
// XNU ipc_kmsg.c (osfmk/ipc/switch_render_senders.h): the mach messages the render server receives, by sender
extern "C" void switch_render_server_watch(task_t);
// NOLINTNEXTLINE(readability-redundant-declaration): not every kernel configuration's headers declare it
extern "C" void proc_selfname(char *, int);
extern "C" size_t switch_render_senders_line(char *, size_t, uint64_t, unsigned);
// XNU (kernel/switch/diag/frames.c): the current run of touches, the clients' commit cadence and touch delivery, and
// each present for the touch-to-scan latency line
extern "C" void switch_frames_touch_activity(uint64_t *, uint64_t *);
extern "C" size_t switch_frames_line(bool, char *, size_t);
extern "C" void switch_frames_present(uint64_t, uint64_t, uint64_t);
extern "C" size_t switch_frames_latency_line(char *, size_t);
// XNU (kernel/switch/diag/stall_frame.c): the threads a frame waits on, snapshotted at a frame end that found a stall
extern "C" void switch_stall_frame(uint64_t);
extern "C" uint64_t native_legacy_surface_width(OSObject *), native_legacy_surface_height(OSObject *);
extern "C" uint64_t native_legacy_surface_stride(OSObject *), native_legacy_surface_allocation(OSObject *);
extern "C" uint32_t native_legacy_surface_format(OSObject *), native_legacy_surface_planes(OSObject *);
// IOSurface plane accessors (kits/SwitchABI IOSurface): offset, bytes per row, height and width of plane `plane`.
extern "C" uint64_t native_planes_surface_plane_offset(OSObject *, uint32_t),
    native_planes_surface_plane_bytes_per_row(OSObject *, uint32_t),
    native_planes_surface_plane_height(OSObject *, uint32_t), native_planes_surface_plane_width(OSObject *, uint32_t);

// TegraVIC (the IOSurfaceAccelerator Core Animation copies a rotated detached layer through): what its boot self-test proved.
// Weak defaults for a kernel without it: not ready, nothing proven.
extern "C" __attribute__((weak)) bool tegra_vic_ready(void)
{
    return false;
}

extern "C" __attribute__((weak)) const TegraVICTraits *tegra_vic_traits(void)
{
    return nullptr;
}

extern "C" void native_legacy_frame_observed(unsigned kind, uint64_t count, uint64_t minx, uint64_t miny, uint64_t maxx,
                                             uint64_t maxy);
extern "C" void native_legacy_frame_colors_observed(const uint32_t *colors, const uint64_t *counts, uint64_t other);
extern "C" void native_legacy_swap_validation_observed(bool, unsigned, const void *, const void *);
// The surface of a rejected swap, as the validator read it: width, height, format, planes, stride, allocation, descriptor length.
extern "C" void native_legacy_surface_rejected(uint64_t, uint64_t, uint32_t, uint32_t, uint64_t, uint64_t, uint64_t);
extern "C" bool native_legacy_prepare_scanout(void);
extern "C" void native_legacy_transfer_observed(IOReturn, const void *, uint64_t);
extern "C" void native_legacy_copy_timing_observed(uint64_t ticks, uint64_t bytes);
extern "C" void native_legacy_retirement_observed(IOService *);
// GPU hand-off: the frame is offered to the privileged GPU client instead of being copied; the swap retires when the
// client reports the GPU copy done (native_legacy_gpu_complete). A refused offer or a failed copy uses the CPU copy below and is counted.
extern "C" bool native_legacy_gpu_offer(void *display, IOMemoryDescriptor *, uint64_t stride, uint32_t width,
                                        uint32_t height);

// The GPU driver's copy engine instead (TegraScanoutCopy.h): weak, absent the CPU copies every frame.
// Weak default that declines every offer (the frame takes the CPU copy); drivers/TegraGM20B's strong definition replaces it at
// link time. The kernel is linked statically, and ld refuses an undefined weak reference there, so the default must exist.
extern "C" __attribute__((weak)) int tegra_gm20b_scanout_copy(const struct TegraScanoutCopy *)
{
    return -1;
}

extern "C" __attribute__((weak)) int tegra_gm20b_surface_tiled(uint64_t)
{
    return 0;
}

extern "C" __attribute__((weak)) int tegra_gm20b_scanout_fence(void *, uint64_t)
{
    return -1;
}

extern "C" __attribute__((weak)) int tegra_gm20b_scanout_outstanding(const void *)
{
    return 0;
}

extern "C" __attribute__((weak)) void *tegra_gm20b_surface_hold(uint64_t, int *tiled)
{
    *tiled = 0;
    return nullptr;
}

extern "C" __attribute__((weak)) void tegra_gm20b_surface_unhold(void *)
{}

extern "C" __attribute__((weak)) int tegra_gm20b_surface_layout(uint64_t, struct tgm_layout *)
{
    return 0;
}

extern "C" __attribute__((weak)) void tegra_gm20b_frame_take(struct SwitchFrameGpu *out)
{
    *out = {};
}

extern "C" __attribute__((weak)) void switch_sched_render_vsync(uint64_t, uint64_t)
{}

extern "C" __attribute__((weak)) void switch_sched_render_swapped(void)
{}

// XNU osfmk/kern/switch_stall.c, debug kernels: base and current priority, mode and effective QoS of a thread.
extern "C" __attribute__((weak)) void switch_thread_sched(thread_t, int *base, int *sched, int *mode, int *qos)
{
    *base = *sched = *mode = *qos = 0;
}

// XNU's text console scrolled by window A's start address (osfmk/console/video_console.c vc_tape; startConsoleTape below).
// Weak defaults for a kernel without it: the console keeps redrawing its text to scroll, as before.
extern "C" __attribute__((weak)) int vc_tape_attach(vm_offset_t, uint32_t, uint32_t, uint32_t,
                                                    void (*)(void *, uint32_t), int (*)(void *), int (*)(void *),
                                                    void *)
{
    return 1;
}

extern "C" __attribute__((weak)) void vc_tape_detach(void)
{}

// XNU (kern_exec.c) while the one-time data migration runs: the console stays on screen and the UI's frames retire undrawn
// (kickHeld), so the user watches the migration's progress lines. Weak: absent, never held.
extern "C" __attribute__((weak)) volatile int switch_console_hold;
// XNU (spec_vnops.c, temporary): ends the boot I/O census at the first client frame; it prints from a thread call.
extern "C" __attribute__((weak)) void switch_boot_io_report(void);

extern "C" __attribute__((weak)) int vc_tape_panic(void)
{
    return 0;
}

// The compositor's swapped surfaces, for a display that fetches them itself: [0] distinct surfaces seen, [1] swaps checked; for the first four, at
// [2 + 3 * k]: allocation bytes, physical segments, largest segment (KiB). The pages are wired for the count and released.
extern "C" {
volatile uint32_t native_legacy_surfaces[14];
}

static void noteSurface(OSObject *surface, IOMemoryDescriptor *memory)
{
    static OSObject *seen[4];
    native_legacy_surfaces[1] = native_legacy_surfaces[1] + 1;
    for (const OSObject *known : seen) {
        if (known == surface)
            return;
    }
    const unsigned k = native_legacy_surfaces[0];
    native_legacy_surfaces[0] = k + 1;
    if (k >= 4)
        return;
    seen[k] = surface;
    if (memory->prepare() != kIOReturnSuccess)
        return;
    uint32_t segments = 0;
    uint64_t largest = 0;
    for (IOByteCount offset = 0, length = 0; offset < memory->getLength() && segments < 100000;
         offset += length, ++segments) {
        if (!memory->getPhysicalSegment(offset, &length, kIOMemoryMapperNone) || !length)
            break;
        if (length > largest)
            largest = length;
    }
    memory->complete();
    native_legacy_surfaces[2 + 3 * k] = static_cast<uint32_t>(memory->getLength());
    native_legacy_surfaces[3 + 3 * k] = segments;
    native_legacy_surfaces[4 + 3 * k] = static_cast<uint32_t>(largest >> 10);
    // Whether a client surface could ever be scanned in place (one physically contiguous run; the DC has no SMMU on this boot chain).
    IOLog("SwitchDisplay: client surface %u: %llu bytes in %u physical segments, largest %llu KiB\n", k,
          static_cast<unsigned long long>(memory->getLength()), segments,
          static_cast<unsigned long long>(largest >> 10));
}

extern "C" void native_legacy_gpu_fallback_observed(int status);
// Swaps retired when the GPU client took the frame (before its copy finished), and copies that then failed after such a retirement.
extern "C" {
volatile uint64_t native_legacy_early_retires, native_legacy_early_failures;
}
// Display refresh tick, 60 Hz. The original family sends the compositor's vsync message (type 5)
// through native_legacy_vsync_notify, the unchanged original sender, but only a platform subclass calls it from the panel's vblank and
// that subclass is not part of this port: without it the render server never saw a vsync. Display controller A's vblank interrupt drives
// it here while at least one client has vsync notifications enabled. An interrupt that cannot be had or stays silent panics. Events: 0 armed, 1 sent, 2 idle (no client), 3 late (timer deadline passed, cadence
// restarted), 4 timer started, 5 refresh properties reported at that rate.

extern "C" void native_legacy_vsync_observed(unsigned event, unsigned hz);
// NOLINTNEXTLINE(readability-redundant-declaration): not every kernel configuration's headers declare it
extern "C" uint64_t mach_continuous_time(void);

// SwitchBrightness (TegraPanel.cpp) plays its DSI commands at DC A frame ends, from the vblank interrupt's filter below (primary interrupt
// context). This empty default keeps the display linkable without it; its strong definition wins when SwitchBrightness is linked.
extern "C" __attribute__((weak)) void tegra_panel_frame_end_filter(void)
{}

// Frame-end holds (native_legacy_frame_end_hold): TegraPanel's commands in flight and the colour unit's pending change.
static uint32_t gFrameEndHolds;

// SwitchBrightness answers true once TegraPanel holds the frame end for its commands and the ambient light sensor runs on
// its own clock. Until then FRAME_END is never masked: both would wait for frame ends that do not come.
extern "C" __attribute__((weak)) bool tegra_panel_holds_frame_end(void)
{
    return false;
}

// SwitchTouch (r17-touch): the interrupt time of the oldest touch report no swap has followed yet, taken (and cleared)
// if it is before `before`; 0 otherwise. Weak: 0 without the touch driver.
extern "C" __attribute__((weak)) uint64_t switch_touch_take_report(uint64_t)
{
    return 0;
}

// SwitchBrightness (SwitchBacklight.cpp; display power is on unless switch-no-sleep) takes the render server's display power requests;
// this default (no SwitchBrightness, or switch-no-sleep) leaves them to the original user client.
extern "C" __attribute__((weak)) bool switch_display_power_request(bool)
{
    return false;
}

// SC7 (tegra_display_suspend/resume below) puts the panel to sleep and wakes it with TegraPanel's own power sequence, the one
// SwitchBacklight uses for display power, and powers the pipeline with TegraDisplayPower.cpp. These defaults keep the display
// linkable without either object; the strong definitions win when they are linked, and the hooks panic without them.
extern "C" __attribute__((weak)) int tegra_panel_power_off(void)
{
    panic("SwitchDisplay: SC7 needs SwitchBrightness (TegraPanel)");
}

extern "C" __attribute__((weak)) int tegra_panel_power_on(int)
{
    panic("SwitchDisplay: SC7 needs SwitchBrightness (TegraPanel)");
}

extern "C" __attribute__((weak)) uint32_t tegra_display_panel_id(void)
{
    panic("SwitchDisplay: SC7 needs TegraDisplayPower.o in the build");
}

extern "C" __attribute__((weak)) void tegra_display_power_down(void)
{
    panic("SwitchDisplay: SC7 needs TegraDisplayPower.o in the build");
}

extern "C" __attribute__((weak)) void tegra_display_power_up(void)
{
    panic("SwitchDisplay: SC7 needs TegraDisplayPower.o in the build");
}

// Display controller A (hekate bdk/display/di.h, Linux drivers/gpu/drm/tegra/dc.h): 32-bit registers, word indices. INT_STATUS is write-1-
// to-clear, INT_ENABLE lets a bit latch, INT_MASK routes a latched bit to the level-high line, GIC_SPI 73 = INTID 105 (mainline
// tegra210.dtsi dc@54200000, hekate irq.h IRQ_DISPLAY). DC A's clock (CAR DISP1, L27) must run before its registers are read.
static constexpr uint64_t kDisplayAPA = 0x54200000;
static constexpr uint32_t kDcIntStatus = 0x037, kDcIntMask = 0x038, kDcIntEnable = 0x039, kFrameEndInt = 1u << 1,
                          kVblankInt = 1u << 2;
static constexpr uint32_t kDisplayAInterrupt = 105, kCarDisp1Id = 27;  // Linux TEGRA210_CLK_DISP1
// r17-touch: V_PULSE2, a raster-line interrupt, for the compositor's vsync phase (VsyncPhase.h). L4T dc_reg.h and
// tegra_dc_init_vpulse2_int (T21x): INT bit 5; DISP_SIGNAL_OPTIONS0 V_PULSE_2_ENABLE bit 19; V_PULSE2_CONTROL H position 0,
// LAST(1) (bit 8); V_PULSE2_POSITION_A START line 12:0 and END line 28:16, lines counted as L4T counts them (its V_BLANK line is
// v_ref_to_sync + v_sync_width + v_back_porch + v_active). The timing registers give those: V in bits 31:16 of REF_TO_SYNC,
// SYNC_WIDTH, BACK_PORCH, DISP_ACTIVE, FRONT_PORCH (hekate di.inl _di_dc_video_mode_config: 1, 1, 9, 1280, 10 on this panel).
// All are assembly registers: written, they latch with the next flip's GENERAL_ACT_REQ, so the phase never asks for one.
static constexpr uint32_t kVpulse2Int = 1u << 5, kDcDispSignalOptions0 = 0x400, kVpulse2Enable = 1u << 19,
                          kDcDispVpulse2Control = 0x422, kDcDispVpulse2Position = 0x423, kVpulse2Last = 1u << 8;
static constexpr uint32_t kDcDispRefToSync = 0x406, kDcDispSyncWidth = 0x407, kDcDispBackPorch = 0x408,
                          kDcDispActive = 0x409, kDcDispFrontPorch = 0x40a;
// DISPLAY_COMMAND bits 6:5: 0 stop, 1 continuous display (hekate di.h DISP_CTRL_MODE_*); an assembly register, latched by GENERAL_ACT_REQ.
static constexpr uint32_t kDcDisplayCommand = 0x032, kDcModeMask = 3u << 5, kDcModeContinuous = 1u << 5;
// Window A, for the hardware rotation (NativeScanout.h). Word indices and bits checked against L4T
// drivers/video/tegra/dc/dc_reg.h (switch-l4t-kernel-nvidia linux-dev) and hekate bdk/display/di.h. The window registers sit at words
// 0x700-0x80d, so the controller is mapped 16 KiB deep (the interrupt registers alone fit in the first page).
//   0x040 STATE_ACCESS   READ_MUX_ACTIVE bit 0 (reads see the active copy), WRITE_MUX_ACTIVE bit 2 (kept 0: writes go to the
//                        assembly copy and only an ACT_REQ moves them)
//   0x041 STATE_CONTROL  GENERAL_ACT_REQ bit 0, WIN_A_ACT_REQ bit 1, GENERAL_UPDATE bit 8, WIN_A_UPDATE bit 9; an ACT_REQ bit
//                        reads 1 until the hardware latched it
//   0x042 WINDOW_HEADER  WINDOW_A_SELECT bit 4: which window the 0x7xx/0x8xx words address
//   0x043 REG_ACT_CONTROL  WIN_ACT_CNTR_SEL_HCOUNTER(0) bit 2: 1 latches window A on the line counter (hekate's init, can tear
//                        mid-frame), 0 at the frame boundary; L4T window.c clears it whenever an update is not hsync safe
//   0x700 WIN_OPTIONS    H_DIRECTION bit 0, SCAN_COLUMN bit 4, WIN_ENABLE bit 30;  0x70a LINE_STRIDE (bytes, bits 15:0)
//   0x705 SIZE, 0x706 PRESCALED_SIZE, 0x709 DDA_INCREMENT, 0x703 COLOR_DEPTH (0xC B8G8R8A8, 0xD R8G8B8A8): left as hekate set them
//   0x800 WINBUF_START_ADDR, 0x806 ADDR_H_OFFSET (bytes), 0x808 ADDR_V_OFFSET (lines), 0x80a UFLOW_STATUS,
//   0x80b SURFACE_KIND   PITCH 0, BLOCK 2, BLOCK_HEIGHT(log2 GOBs) in bits 6:4
// INT_STATUS/INT_ENABLE WIN_A_UF_INT bit 8: window A ran out of pixels during a line.
static constexpr uint64_t kDcMapBytes = 0x4000;
static constexpr uint32_t kDcStateAccess = 0x040, kDcStateControl = 0x041, kDcWindowHeader = 0x042,
                          kDcRegActControl = 0x043;
static constexpr uint32_t kWinOptions = 0x700, kWinColorDepth = 0x703, kWinPosition = 0x704, kWinSize = 0x705,
                          kWinPrescaled = 0x706;
static constexpr uint32_t kWinDdaIncrement = 0x709, kWinLineStride = 0x70a;
// For SC7 only (window A as it was, after the cold re-init): BUFFER_CONTROL, H/V_INITIAL_DDA, BLEND_LAYER_CONTROL (hekate di.h),
// DISP_WIN_OPTIONS (DSI_ENABLE), COM_SCRATCH_REGISTER_B (hekate's brightness duty, TegraPanel's boot level).
static constexpr uint32_t kWinBufferControl = 0x702, kWinHInitialDda = 0x707, kWinVInitialDda = 0x708,
                          kWinBlendLayer = 0x716;
static constexpr uint32_t kDcDispWinOptions = 0x402, kDcScratchB = 0x326;
static constexpr uint32_t kWinbufStartAddr = 0x800, kWinbufHOffset = 0x806, kWinbufVOffset = 0x808;
static constexpr uint32_t kWinbufSurfaceKind = 0x80b, kWinbufStartAddrHi = 0x80d;
static constexpr uint32_t kReadMuxActive = 1u << 0, kWriteMuxActive = 1u << 2, kWindowASelect = 1u << 4,
                          kWinAActHcntrSel = 1u << 2;
static constexpr uint32_t kGeneralActReq = 1u << 0, kWinAActReq = 1u << 1, kGeneralUpdate = 1u << 8,
                          kWinAUpdate = 1u << 9;
static constexpr uint32_t kHDirection = 1u << 0, kVDirection = 1u << 2, kScanColumn = 1u << 4, kWinEnable = 1u << 30,
                          kWinAUnderflowInt = 1u << 8;
// Hardware planes (r18-planes, NativePlanes.h): window B through its own register aperture (mainline dc.c tegra_plane_offset:
// DC_WIN 0x700 + i at 0xd80 + i, DC_WINBUF 0x800 + i at 0xdc0 + i, WINC 0x500 + i at 0xc00 + i; window A's DC_WIN at 0xb80 + i),
// so window A's flips, the console tape and the panic hook never find WINDOW_HEADER moved under them. STATE_CONTROL
// WIN_B_ACT_REQ bit 2 / WIN_B_UPDATE bit 10; REG_ACT_CONTROL WIN_B_ACT_HCNTR_SEL bit 4 and window C's bit 6 (hekate sets both:
// line-counter latching, which tears); INT_STATUS WIN_B_UF_INT bit 9; MC TRANSLATION_ENABLE_0 display0b bit 3 (swgroup dc).
static constexpr uint32_t kWinBBase = 0xd80, kWinbufBBase = 0xdc0, kWinABase = 0xb80;
static constexpr uint32_t kWinCBase = 0xf80;
static constexpr uint32_t kWinCActReq = 1u << 3, kWinCUpdate = 1u << 11, kWinCUnderflowInt = 1u << 10,
                          kDisplay0cTranslation = 1u << 5;
static constexpr uint32_t kWinBActReq = 1u << 2, kWinBUpdate = 1u << 10, kWinBActHcntrSel = 1u << 4,
                          kWinCActHcntrSel = 1u << 6, kWinBUnderflowInt = 1u << 9, kDisplay0bTranslation = 1u << 3;
// _di_winA_block: BLOCK | BLOCK_HEIGHT(4); the stride is the landscape line; H_OFFSET is the last byte of a line, where
// H_DIRECTION starts (L4T window.c: (x + w) * Bpp - 1 = 0x13FF, the value hekate quotes for Horizon's own block scanout).
// Portrait-native (SwitchPortrait.h): the same block layout scanned row by row, unrotated, as hekate's pitch window
// scans; kBlockScan is what the block layout adds to hekate's WIN_OPTIONS.
static constexpr uint32_t kScanBlockLog2 = 4u, kBlockSurfaceKind = 2u | (kScanBlockLog2 << 4),
                          kBlockLineStride = static_cast<uint32_t>(kNativeBlockStride),
                          kBlockHOffset = SWITCH_PORTRAIT_NATIVE ? 0 : static_cast<uint32_t>(kNativeBlockStride) - 1,
                          kBlockScan = SWITCH_PORTRAIT_NATIVE ? 0 : kScanColumn | kHDirection;
static constexpr const char *kBlockScanName =
    SWITCH_PORTRAIT_NATIVE ? "row scan of the portrait" : "column scan of the landscape";
// The pitch window hekate hands over (_di_winA_pitch): 720x1280 at 1.0x, 2880-byte lines. The rotation changes only
// WIN_OPTIONS, LINE_STRIDE, SURFACE_KIND and ADDR_H_OFFSET; everything else must already be this or the switch would scramble.
static constexpr uint32_t kPitchSize = (1280u << 16) | 720u, kPitchPrescaled = (1280u << 16) | (720u * 4),
                          kUnitDda = 0x10001000;
// The console tape (startConsoleTape): hekate's pitch window with kTapeGap spare text lines (64 bytes each: 16 portrait columns)
// after every 2880-byte row. kTapeLines positions 64 bytes apart, then the console redraws at position 0, whose window no window
// within kTapeGap lines of the end reaches (two window extents, plus slack for a multi-line scroll).
static constexpr uint32_t kTapeLineBytes = 64, kTapeGap = 45, kTapeStride = 2880 + kTapeGap * kTapeLineBytes;
static constexpr uint64_t kTapeWindow = 1279ULL * kTapeStride + 2880;
static constexpr uint32_t kTapeLines =
    static_cast<uint32_t>((kTapeWindow + kTapeLineBytes - 1) / kTapeLineBytes) + 2 * kTapeGap;
static constexpr uint64_t kTapeBytes =
    (static_cast<uint64_t>(kTapeLines - 1) * kTapeLineBytes + kTapeWindow + 0x3fff) & ~0x3fffULL;
static constexpr unsigned kTapeWaitMS = 100;  // the console's wait for a frame end: six frames

// 32-bit BGRA, plain ('BGRA') or in Apple's lossless-compressed spelling ('&BGA', one plane): the GPU that renders the surface here writes plain rows
// either way (no compression is implemented), so the bytes are the same.
static inline bool nativeBgra(uint32_t format, uint32_t planes)
{
    return (format == 0x42475241 && (planes == 0u)) || (format == 0x26424741 && planes <= 1);
}

// Panics draw into the boot framebuffer as the pitch console does (rotation 3: SwitchPanicSave.c's panic screen once the
// GUI owns the display, XNU's console before that, or SwitchDiag's own banner), which a block-linear column scan would
// show scrambled. While window A is rotated this is SwitchDiag's display panic hook (switch_display_panic_hook, every
// kernel config: called first in switch_diag_panic, before the panic screen is drawn, and again by SwitchPanicSave.c
// once the other CPUs are halted): the first call blacks the pitch area, every call puts back window A's four pitch
// registers, latched at the next frame, so the panic text lands on a readable screen. From the first call
// on, the frame-end filter and the flip leave window A alone: until debug.c halts them, the other CPUs still run this
// driver, whose next flip would scan block-linear again and whose latch check would panic over the restored registers.
// Before the first frame window A scans XNU's console tape (startConsoleTape): a panic the console still owns is drawn
// there by the console itself, which scrolls the tape without waiting; one after the console released the screen is
// SwitchPanicSave's, so window A goes back to the boot framebuffer as when rotated.
// Interrupts are off and other CPUs may be stopped: no locks, no allocation, no logging, only stores.
extern "C" void (*volatile switch_display_panic_hook)(const char *message);

namespace {
struct PanicRestore {
    SwitchMMIOWords dc;
    volatile uint64_t *pixels;
    uint64_t bytes;
    uint32_t options, stride, kind, hOffset, start;  // start: the boot framebuffer, where the panic console draws
    volatile bool armed;                             // window A is rotated: a panic must put the pitch window back
    volatile bool tape;       // window A scans the console tape: it must too, unless the console still owns the screen
    volatile bool panicking;  // a panic began: window A is the panic's
    volatile bool planes;     // window B may scan a plane, window A may blend over it: both go back (r18-planes)
    uint32_t blend[3];        // window A's BLEND_LAYER_CONTROL, MATCH and NOMATCH as hekate left them
    volatile bool landscape;  // window A may column-scan a video (SwitchLandscapeDisplay.h): its geometry goes back too
    uint32_t uiDepth;         // the UI's COLOR_DEPTH
#if SWITCH_FRAME_TIMELINE
    volatile bool columnScan;  // a display experiment (column scan, window probe) has window A in its own format
    uint32_t columnScanDepth;  // the UI's COLOR_DEPTH
#endif
};

PanicRestore gPanicRestore;

void restorePitchOnPanic(const char *message)
{
    const bool first = !gPanicRestore.panicking;
    gPanicRestore.panicking = true;
    __asm__ volatile("dmb ish" ::: "memory");
    if (gPanicRestore.armed || (gPanicRestore.tape && !vc_tape_panic())) {
        if (first) {
            volatile uint64_t *pixels = gPanicRestore.pixels;
            for (uint64_t i = 0; i < gPanicRestore.bytes / 8; ++i)
                pixels[i] = 0xff000000ff000000ULL;
        }
        const SwitchMMIOWords dc = gPanicRestore.dc;
        dc[kDcStateAccess] = dc[kDcStateAccess] & ~(kReadMuxActive | kWriteMuxActive);
        dc[kDcWindowHeader] = kWindowASelect;
        dc[kWinLineStride] = gPanicRestore.stride;
        dc[kWinbufSurfaceKind] = gPanicRestore.kind;
        dc[kWinbufHOffset] = gPanicRestore.hOffset;
        dc[kWinbufStartAddr] = gPanicRestore.start;
        dc[kWinbufStartAddrHi] = 0;
#if SWITCH_FRAME_TIMELINE
        const bool geometry = gPanicRestore.columnScan || gPanicRestore.landscape;
        const uint32_t depth = gPanicRestore.columnScan ? gPanicRestore.columnScanDepth : gPanicRestore.uiDepth;
#else
        const bool geometry = gPanicRestore.landscape;
        const uint32_t depth = gPanicRestore.uiDepth;
#endif
        if (geometry) {
            dc[kWinColorDepth] = depth;
            dc[kWinPosition] = 0;
            dc[kWinSize] = kPitchSize;
            dc[kWinPrescaled] = kPitchPrescaled;
            dc[kWinHInitialDda] = 0;
            dc[kWinVInitialDda] = 0;
            dc[kWinDdaIncrement] = kUnitDda;
            dc[kWinbufVOffset] = 0;
        }
        dc[kWinOptions] = gPanicRestore.options;
        uint32_t update = kGeneralUpdate | kWinAUpdate;
        uint32_t request = kGeneralActReq | kWinAActReq;
        TegraDisplayColor::bypassForPanic(dc);
        if (gPanicRestore.planes) {  // the panic screen over nothing: window B off, window A opaque again
            dc[kWinBBase + native_planes::kOptions] = 0;
            dc[kWinCBase + native_planes::kOptions] = 0;
            for (unsigned i = 0; i < 3; ++i)
                dc[kWinABase + native_planes::kBlendLayer + i] = gPanicRestore.blend[i];
            update |= kWinBUpdate | kWinCUpdate;
            request |= kWinBActReq | kWinCActReq;
        }
        __asm__ volatile("dsb sy" ::: "memory");
        dc[kDcStateControl] = update;
        dc[kDcStateControl] = request;
    } else if (gPanicRestore
                   .planes) {  // the plane probe covers the screen before the first frame: off, the console shows
        const SwitchMMIOWords dc = gPanicRestore.dc;
        dc[kWinBBase + native_planes::kOptions] = 0;
        dc[kWinCBase + native_planes::kOptions] = 0;
        TegraDisplayColor::bypassForPanic(dc);
        __asm__ volatile("dsb sy" ::: "memory");
        dc[kDcStateControl] = kGeneralUpdate | kWinBUpdate | kWinCUpdate;
        dc[kDcStateControl] = kGeneralActReq | kWinBActReq | kWinCActReq;
    }
    (void)message;
}

inline uint64_t counterNow()
{
    // NOLINTNEXTLINE(misc-const-correctness): the asm writes it
    uint64_t ticks = 0;
    __asm__ volatile("isb\n\tmrs %0, cntpct_el0" : "=r"(ticks));
    return ticks;
}

// The MAIR attribute byte the kernel's translation of address resolves to (PAR_EL1 after AT S1E1R, as XNU's mmu_kvtop):
// 0xff write-back cacheable, 0x44 non-cacheable (write-combined), 0x00/0x04 device; 0x100 when the page is not mapped.
// Interrupts are off around it so no handler's own AT replaces PAR_EL1 in between.
uint32_t mairAttribute(const void *address)
{
    const boolean_t enabled = ml_set_interrupts_enabled(FALSE);
    // NOLINTNEXTLINE(misc-const-correctness): the asm writes it
    uint64_t par = 0;
    __asm__ volatile("at s1e1r, %1\n\tisb\n\tmrs %0, par_el1" : "=r"(par) : "r"(address) : "memory");
    ml_set_interrupts_enabled(enabled);
    return (par & 1) ? 0x100 : static_cast<uint32_t>(par >> 56);
}

// Steady-state timing (NativeLegacyDisplay): one mean/max/count per stage, in timebase ticks.
struct Span {
    uint64_t sum, max;
    uint32_t count;

    void add(uint64_t ticks)
    {
        sum += ticks;
        if (ticks > max)
            max = ticks;
        ++count;
    }

    uint64_t meanUs() const
    {
        return count ? ticksToUs(sum / count) : 0;
    }

    uint64_t maxUs() const
    {
        return ticksToUs(max);
    }

    static uint64_t ticksToUs(uint64_t ticks)
    {
        uint64_t ns = 0;
        absolutetime_to_nanoseconds(ticks, &ns);
        return ns / 1000;
    }
};

// Frame-time distribution (r6-lows, stats only): fixed bins, no allocation, written under the work-loop gate. 1 ms bins up to
// 64 ms, 4 ms to 128, 8 ms to 256, 64 ms to 1024, then one bin for the rest. A quantile reads back its bin's upper edge, capped
// at the largest value seen, so it is at most 1 ms high below 64 ms (presents are whole frames, 16.7 ms apart).
struct FrameHist {
    static constexpr unsigned kBins = 64 + 16 + 16 + 12 + 1;
    uint32_t bins[kBins];
    uint32_t count;
    uint64_t maxUs;

    static unsigned binOf(uint64_t us)
    {
        const uint64_t ms = us / 1000;
        if (ms < 64)
            return static_cast<unsigned>(ms);
        if (ms < 128)
            return 64 + static_cast<unsigned>((ms - 64) / 4);
        if (ms < 256)
            return 80 + static_cast<unsigned>((ms - 128) / 8);
        if (ms < 1024)
            return 96 + static_cast<unsigned>((ms - 256) / 64);
        return kBins - 1;
    }

    static uint64_t upperUs(unsigned bin)
    {
        if (bin < 64)
            return (bin + 1) * 1000ULL;
        if (bin < 80)
            return (64 + (bin - 63) * 4) * 1000ULL;
        if (bin < 96)
            return (128 + (bin - 79) * 8) * 1000ULL;
        if (bin < 108)
            return (256 + (bin - 95) * 64) * 1000ULL;
        return UINT64_MAX;
    }

    void add(uint64_t us)
    {
        ++bins[binOf(us)];
        ++count;
        if (us > maxUs)
            maxUs = us;
    }

    // The permille-th quantile (500 median, 990 p99, 999 p99.9), in microseconds.
    uint64_t quantileUs(unsigned permille) const
    {
        if (!count)
            return 0;
        const uint64_t rank = (static_cast<uint64_t>(count) * permille + 999) / 1000;
        uint64_t seen = 0;
        for (unsigned bin = 0; bin < kBins; ++bin) {
            seen += bins[bin];
            if (seen >= rank) {
                const uint64_t upper = upperUs(bin);
                return upper < maxUs ? upper : maxUs;
            }
        }
        return maxUs;
    }
};

// The display's frame-time histograms, kept out of the object: with iPadOS 27's framebuffer family (0x1de0 bytes) as its
// base, the instance would pass the 16 KiB a kalloc_type class may have (KALLOC_SAFE_ALLOC_SIZE).
struct FrameHists {
    FrameHist presentGap, latency, touchScan;
    SwitchInteraction interaction;
};
}

// NOLINTNEXTLINE(misc-use-internal-linkage): OSDefineMetaClassAndStructors needs the class's external linkage (its kalloc type view)
class NativeLegacyDisplay : public IOMobileFramebufferLegacy {
    // NOLINTNEXTLINE(misc-explicit-constructor): IOKit's structors macro declares the metaclass constructor
    OSDeclareDefaultStructors(NativeLegacyDisplay) IOMemoryMap *bootMapping = nullptr;
    bool primary = false;
    bool logicalPM = false;
    bool transferred = false;
    // The colour unit's state (tables, matrices, the pending change) is kept out of the object: inside, it took the instance
    // past the 16 KiB a kalloc_type class may have (KALLOC_SAFE_ALLOC_SIZE). Made in start, freed in free.
    TegraDisplayColor *color = nullptr;
    switch_frame_info::State *frameInfo = nullptr;  // out of the object for the same reason
    bool blanked = false;  // a blank swap's black is the colour unit's until the next flip latches

    // Every request the validator accepted and the family has not retired yet. A client may submit swap N+1 while swap N is still on the GPU
    // (int-131: swap 3 was validated during swap 2's GPU copy and one remembered pointer lost swap 2, which then never retired and left
    // every wait on it busy), so more than one can be pending.
    // r14-probefix: 16 entries, and entries of swaps the family no longer holds (cancelled or discarded: it never asks this
    // driver to retire those) are dropped under the gate (pruneValidated) before the table fills. Full, the oldest entry goes,
    // not entry 0 (which may be the pending head: unvalidated, it would never be taken again and the screen would freeze).
    // The frame timeline (SwitchFrameTimeline.h), debug kernels: the vsync the swap came after, the GPU's completion of its
    // present, the compositor's submissions for it, every process's new-surface work, and how the swapping thread and the
    // CPU clock stood. Times are microseconds from the swap's submission (kNotSeen: none), kept small because the
    // accepted-swap table, the frame marks and the taken swap hold 25 of these inside the class.
    static constexpr int32_t kNotSeen = INT32_MIN;

    struct FrameTimeline {
        int32_t vsyncUs = kNotSeen, doneUs = kNotSeen, firstUs = kNotSeen, lastUs = kNotSeen;
        uint32_t execUs, importUs, bindUs, allocUs, cleanUs, cleanKiB;
        uint16_t submits, others, imports, binds, allocs, frees, mhzVsync, mhzSwap;
        int8_t basePri, schedPri, mode, qos;
    };

    static int32_t usFrom(uint64_t fromNs, uint64_t toNs)
    {
        const int64_t us = (static_cast<int64_t>(toNs) - static_cast<int64_t>(fromNs)) / 1000;
        if (us > INT32_MAX)
            return INT32_MAX;
        if (us <= INT32_MIN)
            return INT32_MIN + 1;
        return static_cast<int32_t>(us);
    }

    static uint32_t us32(uint64_t ns)
    {
        return ns / 1000 > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(ns / 1000);
    }

    static uint16_t sat16(uint64_t n)
    {
        return n > 0xffff ? 0xffff : static_cast<uint16_t>(n);
    }

    static uint64_t ticksNs(uint64_t ticks)
    {
        uint64_t ns = 0;
        absolutetime_to_nanoseconds(ticks, &ns);
        return ns;
    }

    static constexpr unsigned kValidated = 16;
    const OriginalLegacySwapRequest *validated[kValidated] = {};
    uint64_t validatedAt[kValidated] = {};  // when each was first accepted (mach time): the client's submission
    FrameTimeline validatedTl[kValidated] = {};
    uint32_t validatedEvicted = 0, validatedStale = 0;

    bool isValidated(const OriginalLegacySwapRequest *request) const
    {
        if (!request)
            return false;
        for (const auto *entry : validated) {
            if (entry == request)
                return true;
        }
        return false;
    }

    void removeValidated(const OriginalLegacySwapRequest *request)
    {
        frameInfo->tickets.retire(request);
        for (auto &entry : validated) {
            if (entry == request)
                entry = nullptr;
        }
    }

    // True when the request is new (not a second validation of one already pending).
    bool addValidated(const OriginalLegacySwapRequest *request, uint64_t now)
    {
        if (isValidated(request))
            return false;
        unsigned slot = 0;
        while (slot < kValidated && validated[slot])
            ++slot;
        if (slot == kValidated) {
            slot = 0;
            for (unsigned i = 1; i < kValidated; ++i) {
                if (validatedAt[i] < validatedAt[slot])
                    slot = i;
            }
            __atomic_fetch_add(&validatedEvicted, 1u, __ATOMIC_RELAXED);
        }
        if (validated[slot])
            frameInfo->tickets.retire(validated[slot]);
        validated[slot] = request;
        validatedAt[slot] = now;
        uint32_t id = 0;
        uint64_t requested = 0;
        const auto *bytes = reinterpret_cast<const unsigned char *>(request);
        memcpy(&id, bytes + swapId, sizeof(id));
        memcpy(&requested, bytes + swapRequestedPresentation, sizeof(requested));
        frameInfo->tickets.accept(request, id, requested, now);
        return true;
    }

    // Under the gate: entries older than a second whose swap is neither in the family's queue, nor its current swap, nor in
    // flight here (a younger one may be a swap validateSwap just accepted that the family has not queued yet).
    void pruneValidated()
    {
        const OriginalLegacySwapRequest *current = nullptr;
        memcpy(&current, reinterpret_cast<const unsigned char *>(this) + swapQueueCurrent,
               sizeof(const OriginalLegacySwapRequest *));
        uint64_t second = 0;
        nanoseconds_to_absolutetime(1000000000ULL, &second);
        const uint64_t now = mach_absolute_time();
        for (unsigned i = 0; i < kValidated; ++i) {
            const OriginalLegacySwapRequest *&entry = validated[i];
            if (!entry || now - validatedAt[i] < second || entry == current || entry == gpuRequest ||
                (copyQueued && entry == job.request))
                continue;
            bool queued = false;
            for (const OriginalLegacySwapRequest *r = pendingSwapRequest(); r && !queued; r = queuedAfter(r))
                queued = r == entry;
            if (queued)
                continue;
            frameInfo->tickets.retire(entry);
            entry = nullptr;
            ++validatedStale;
        }
        if (const uint32_t evicted = __atomic_exchange_n(&validatedEvicted, 0u, __ATOMIC_RELAXED)) {
            IOLog("SwitchDisplay: the accepted-swap table was full: %u entries replaced (%u stale ones dropped so "
                  "far)\n",
                  evicted, validatedStale);
        }
    }

    // The request queued behind this one in the family's tail queue (each request's next at swapNext; NativeLegacySwapQueue.h
    // activateOnlyPendingSwap), or none.
    static const OriginalLegacySwapRequest *queuedAfter(const OriginalLegacySwapRequest *request)
    {
        const OriginalLegacySwapRequest *next = nullptr;
        if (request) {
            memcpy(&next, reinterpret_cast<const unsigned char *>(request) + swapNext,
                   sizeof(const OriginalLegacySwapRequest *));
        }
        return next;
    }

    uint64_t submittedAt(const OriginalLegacySwapRequest *request) const
    {
        for (unsigned i = 0; i < kValidated; ++i) {
            if (request && validated[i] == request)
                return validatedAt[i];
        }
        return 0;
    }

    FrameTimeline timelineOf(const OriginalLegacySwapRequest *request) const
    {
        for (unsigned i = 0; i < kValidated; ++i) {
            if (request && validated[i] == request)
                return validatedTl[i];
        }
        return {};
    }

    const OriginalLegacySwapRequest *gpuRequest = nullptr;
    uint64_t gpuLength = 0;
    bool gpuBypass = false;
    // The swap of the frame the GPU client is copying already retired.
    bool gpuRetired = false;
    // GPU frame copy (double-buffered hardware rotation only; TegraScanoutCopy.h): the GPU driver's copy
    // engine writes the swapped surface into the buffer window A does not scan, and its completion flips window A to it, as the CPU
    // copy's end does. gpuScanout: the outstanding offer (gpuRequest) is such a copy, not the sheng presenter's. The completion comes
    // through gpuDoneSource from any context; one not in by its deadline that the GPU driver no longer holds panics.
    bool gpuCopy = false, gpuScanout = false, gpuPresentDeferred = false;
    uint64_t gpuSequence = 0, gpuOfferedAt = 0;
    IOMemoryDescriptor *gpuSource = nullptr;  // retained from the offer to the completion
    const OriginalLegacySwapRequest *gpuPresented =
        nullptr;  // a completed copy's swap, until it is presented (after SC7: at the resume)
    IOInterruptEventSource *gpuDoneSource = nullptr;
    IOTimerEventSource *gpuDeadline = nullptr;
    int gpuStatus = 0;
    bool gpuDonePending = false;  // written by native_legacy_gpu_complete before it signals gpuDoneSource
    uint64_t gpuDoneAt = 0;       // the frame timeline: when it came
    static constexpr unsigned kGpuCopyDeadlineMS = 1000;
    // The GPU driver's verdict on a late offer (a named panic, or a hung client killed) comes within a second of its own
    // deadline; an offer it still holds after this many of ours is one it lost track of.
    static constexpr unsigned kGpuCopyDeadlines = 10;
    unsigned gpuDeadlines = 0;
    uint64_t gpuLostFrames = 0;
    // The refresh tick runs on the family's work loop, the loop of the command gate that registers and removes the vsync clients, so the
    // client queue (vsyncClientList) never changes while the sender walks it.
    IOTimerEventSource *vsyncTimer = nullptr;
    uint64_t vsyncInterval = 0, vsyncNext = 0;
    unsigned vsyncHz = 0;
    bool vsyncArmed = false;
    unsigned vsyncSilent = 0;  // DC A frame ends without a V_BLANK while armed (watchVsync)
    static constexpr unsigned kVsyncSilentFrames = 30;
    // DC A interrupt, on the same work loop. FRAME_END is unmasked while something waits for a frame end (frameEndNeeded, a
    // frame-end hold) and masked at the first frame end with nothing waiting, so an idle display wakes no core. Every kick
    // unmasks it, since all display work starts there. V_BLANK is unmasked only while a vsync client is registered. The
    // filter clears both in INT_STATUS, which lowers the line, and hands them to the action; the timer only checks once,
    // 250 ms after the start, that frame ends arrive.
    IOFilterInterruptEventSource *vblankSource = nullptr;
    IOMemoryMap *dcMap = nullptr;
    SwitchMMIOWords dc;
    uint32_t dcEnable = 0, dcMask = 0;  // INT_ENABLE and INT_MASK as found, restored on stop
    // INT_MASK is written from outside the gate too (frame-end holds), so every write of it takes this lock.
    IOSimpleLock *intMaskLock = nullptr;
    bool intMaskLive = false;     // dc mapped and clocked: a hold may write INT_MASK
    bool scanoutStopped = false;  // display power off (setScanout): no frame ends or vblanks are expected
    uint32_t dcPending = 0;       // INT_STATUS bits from the filter, for the action
    uint64_t vblankTime = 0, vblankContinuous = 0, vblankLast = 0, frameEnds = 0;
    // r17-touch: the compositor's vsync phase (VsyncPhase.h, the pacing line's "phase"). While its lead is in effect the vsync
    // goes out at V_PULSE2, programmed `lead` before the frame end; otherwise at V_BLANK as before, and at most one goes out a
    // frame. Never booted: V_BLANK carries the vsync until V_PULSE2 has reached the CPU once; if it has not after
    // kPhaseProbeFlips latched flips with it programmed, one line and the phase is off for the boot. Once it has, a silent one
    // panics as a silent V_BLANK does. The phase runs on the frame marks, which every kernel keeps (the stats are debug only).
    VsyncPhase phase;
    uint64_t vpulseTime = 0, vpulseContinuous = 0, deliveredFrame = ~0ULL;
    uint64_t vsyncSentAt = 0;  // the frame timeline: the last vsync sent (its stamp), and the CPU clock then
    uint32_t vsyncSentKhz = 0;
    int64_t feAfterVblankNs = 0;  // the frame end's time after V_BLANK's, measured
    int32_t phaseCorrection = 0;  // lines, from the measured lead against the asked-for one
    uint32_t vblankLine = 0, totalLines = 0, phaseLine = 0, phaseAsked = 0, measuredLeadUs = 0;
    uint32_t deliveredSource = 0;  // the last vsync's: 0 at V_BLANK, else V_PULSE2's line
    unsigned phaseProbe = 0, vpulseSilent = 0;
    unsigned phaseSettle =
        0;  // latched flips to wait after a new line before measuring it (it latches with the next flip)
    bool vpulseProgrammed = false, vpulseSeen = false, vpulseThisFrame = false, phaseFailed = false;
    static constexpr unsigned kPhaseProbeFlips = 5;
    // Hardware rotation (NativeScanout.h). The CPU stops rotating: each client frame is
    // swizzled, unrotated, into the block-linear extent of the boot framebuffer and window A column-scans it. The first such
    // frame switches window A (four registers, latched at a frame boundary); the frame ends after it confirm the latch, read the
    // active registers back, and watch window A for underflow. Kick and frame ends both run under the family's work-loop gate.
    bool hwRotate = false;                // armed at start for the primary, rotated (v_rotate 3) panel
    IOMemoryMap *blockMapping = nullptr;  // the boot framebuffer's kNativeBlockBytes, write-combined
    bool rotated = false, rotationLatched = false;
    uint64_t rotationRequested = 0;  // frameEnds when the ACT_REQ went out
    uint32_t pitchOptions = 0, pitchStride = 0, pitchKind = 0, pitchHOffset = 0,
             pitchActControl = 0;    // active values before the switch
    unsigned underflowRun = 0;       // consecutive frame ends with WIN_A_UF_INT latched, once latched
    bool rotationAnnounced = false;  // the latch line is logged once, not again after every SC7 resume
    static constexpr unsigned kLatchFrames = 4, kUnderflowFrames = 30;
    // Multiple buffering (only with the hardware rotation). Three block-linear
    // surfaces (two if no third could be had): 0 is the boot framebuffer's (blockMapping), 1 and 2 physically contiguous buffers
    // below 4 GiB. A frame is swizzled into one window A neither scans nor may take (ring.target()), then window A's start
    // address flips to it, latched at the next frame boundary (frame-counter latching, set by startRotation). A frame finished
    // while an older flip is still out replaces that flip (latest frame wins, TegraScanoutRing.h): it scans out at the next
    // boundary and the older frame is not shown. With every buffer held (one scanned, one asked for, one replaced and not yet
    // known to be untaken) the next frame's kick is deferred to the frame end that settles it, so a surface is written only after
    // the controller has stopped reading it. The first frame goes to surface 1 together with the switch to the rotated layout.
    // Kick, flip and frame ends all run under the family's work-loop gate.
    bool doubleBuffer = false;
    static constexpr unsigned kScanBuffers = TegraScanoutRing::kMaxBuffers;
    static constexpr int kNoBuffer = TegraScanoutRing::kNone;
    switch_dma_buffer scanBuffers[kScanBuffers] = {};  // surfaces 1 and 2's pages ([0] unused: the boot framebuffer)
    IOMemoryMap *scanMappings[kScanBuffers] = {};      // their write-combined views
    uint64_t scanAddresses[kScanBuffers] = {};         // their physical addresses ([0]: the boot framebuffer's)
    TegraScanoutRing ring;                             // which surface is scanned, asked for, replaced, drawn
    // The direct-scanout gate's probe (DisplaySmmuProbe.h): once, kProbeAfterFrames frame ends
    // after window A first scanned a client frame, with nothing in flight, DC A's CRC unit and the display's SMMU domain (ASID
    // TEGRA_SMMU_ASID_DISPLAY on the "dc" swgroup) prove window A reads the scanned buffer through an IOVA slot; then all of
    // it is undone and one line says what was found. Swaps wait meanwhile (about 12 frames). Nothing scans through the SMMU
    // after it: direct scanout is the next stage.
    static constexpr uint32_t kDcCrcControl = 0x300, kDcCrcLatched = 0x329, kCrcOn = (1u << 3) | (1u << 2) | (1u << 0);
    static constexpr uint32_t kDisplay0aTranslation = 1u << 1;  // client display0a (id 1) in TRANSLATION_ENABLE_0
    static constexpr uint64_t kProbeSlot = 0x10000000ULL;  // below DRAM (0x80000000): only a translation reaches it
    static constexpr unsigned kProbeTables = switch_window_caps::kDomainTables, kProbeAfterFrames = 600;
    switch_dma_buffer probeTables = {};
    uint64_t probeAt = 0;
    TegraSMMUDomain probeDomain;
    DisplaySmmuProbe probe;
    bool probeRan = false, probeUnderflow = false;
    bool probeRequest = false;  // a probe START_ADDR request (to the slot, or back) not yet seen taken
    // r14-probefix: the probe holds swaps at most kProbeHoldMs (it needs about 20 frame ends, 330 ms); past that, or with the
    // display going off, it is undone and the swaps go (one line).
    static constexpr unsigned kProbeHoldMs = 600;
    uint64_t probeHoldSince = 0;
    int probeSurface = kNoBuffer;
    uint32_t probePixel = 0, probeSwgroupWas = 0;
    char probeLine[160] = {};
    // Direct scanout (r14-direct), on once this boot's SMMU probe passed: the probe's domain
    // stays attached, and a full-frame BGRA swap whose rows window A can fetch (stride a multiple of 64, one 4 MiB slot) is
    // scanned in place from an SMMU slot (ring entries kSlot0..): a fence-only GPU present waits for the compositor's rendering
    // of it, then window A flips to the slot in pitch layout. Core Animation recycles a framebuffer when a later swap completes
    // ("swap %u completed, recycling surface 0x%x from swap %u"), so from then on every swap, direct or copied, retires when its
    // frame is seen on screen, with every older one (latchRetire); a frame replaced before it showed retires with the one that
    // replaced it. A slot keeps its surface's descriptor retained and prepared until another surface needs the slot and it is
    // neither scanned nor asked for. Window A underflowing kDirectUnderflowFrames in a row from a slot turns direct off (one
    // line); swaps keep retiring at their latch.
    static constexpr int kSlot0 = static_cast<int>(TegraScanoutRing::kMaxBuffers);
    static constexpr unsigned kSlots = TegraScanoutRing::kMaxEntries - TegraScanoutRing::kMaxBuffers,
                              kDirectUnderflowFrames = 3;
    static constexpr uint64_t kSlotSpan = 4ULL << 20;

    // A slot keeps what window A may scan from it: the surface itself, its memory wired and its layout held
    // (tegra_gm20b_surface_hold), until a later frame's slot replaces it and the slot is dropped. Its client may die meanwhile.
    struct DirectSlot {
        IOMemoryDescriptor *memory;
        OSObject *surface;
        void *layout;
        uint64_t stride, used;
        uint32_t pages;
        bool tiled;  // the GPU renders it block-linear (TegraScanoutCopy.h): window A scans it so
        SwitchNativeScanout *read = nullptr;
        uint64_t allocationId = 0, allocationGeneration = 0, sourceOffset = 0;
    };

    DirectSlot slots[kSlots] = {};
    uint64_t slotUses = 0, takeSeq = 0;
    bool directOn = false, directAttached = false, latchRetire = false;
    int gpuDirect = kNoBuffer, gpuPresentedEntry = kNoBuffer;  // the slot entry a fence-only present is for

    struct Taken {
        const OriginalLegacySwapRequest *request;
        uint64_t seq;
    };

    Taken takenSwaps[8] = {};  // swaps presented or in flight, not yet retired (latchRetire)
    const OriginalLegacySwapRequest *flipRequest = nullptr;  // the swap the next flip shows (noteTaken)
    uint64_t entrySeq[TegraScanoutRing::kMaxEntries] = {};   // the take sequence of the swap each entry shows
    uint64_t entryPendingSeq[TegraScanoutRing::kMaxEntries] = {};
    switch_display::Retirement scanoutRetirement;
    uint32_t stDirect = 0;
    // --- Hardware planes (r18-planes; NativePlanes.h has the rules and their sources) ------------------------------------
    // Core Animation's detached layer (a video under the UI) is scanned by window B through the display's SMMU domain, from
    // one of kPlaneSlots IOVA slots above the direct-scanout ones, while window A shows the composited UI blending over it
    // (premultiplied). The domain comes up at the start, before backboardd: the plane probe (DisplaySmmuProbe's stages on
    // window B, which covers the screen with a frozen copy of it for about 20 frames) proves window B reads through it; only
    // then are planes on (planesOn), and only then may XNU start backboardd without CA_DISABLE_DETACHED
    // (native_legacy_detach_ready below). Window B's register sets move through DC A as native_planes::PlaneLatch follows them;
    // a set is a slot index (kOff: window off). A swap retires when the latch that shows it (or a newer one) is seen.
    static constexpr unsigned kPlaneSlots = 8;
    static constexpr uint64_t kPlaneSlotSpan = switch_window_caps::kPlaneSlotSpan,
                              kPlaneSlot0 = kProbeSlot + static_cast<uint64_t>(kSlots) * kSlotSpan;
    static constexpr unsigned kPlaneUnderflowFrames = 3;
    // An overlay's picture producer, a recorded turn's request waits (src and dst) and its surface's writer promises, for
    // each of two overlays.
    static constexpr unsigned kPlaneOverlayDependencies = 3 + SWITCH_GPU_BACKING_PROMISES,
                              kPlaneDependencies = 2 * kPlaneOverlayDependencies;

    using PlaneSlot = switch_display::PlaneSlot;

    PlaneSlot planeSlots[kPlaneSlots] = {};
    uint32_t planeSlotUses = 0;

    using PlaneSet = switch_display::Composition;
    using Window = switch_landscape::Window;

    using PlaneCommits = switch_display::Compositions<PlaneSet>;
    PlaneSet *planeSets = nullptr;
    PlaneCommits planeLatch;
    bool planesOn = false, planesAttached = false, planeProbeRan = false, planeProbing = false;
    uint64_t planeProbeAt = 0;
    // The plane change the next window A flip carries (the swap being taken), and the one each ring entry carried.
    bool planeNextSet = false;
    PlaneSet planeNext = {};
    bool entryPlaneSet[TegraScanoutRing::kMaxEntries] = {};
    PlaneSet *entryPlane = nullptr;
    bool planeHeldSet = false;  // a window B change waiting for the frame end that settles the last two (PlaneLatch)
    PlaneSet planeHeld = {};
    int planeCsc[2][2] = {{-1, -1}, {-1, -1}};
    bool planeFiltersWritten[2] = {}, planeBlending = false;
    unsigned planeUnderflowRun = 0;
    uint32_t planeBlendSaved[3] = {};
    // Stats (the pacing line): window B flips written, of them swaps that changed only window B (no UI composite at all),
    // B sets seen latched, new slot mappings, detached layers not shown (planes off, or a layer more than window B takes),
    // window B underflow frames, ready-mask mismatches at a kick, overlay sets VIC composited into one plane.
    uint32_t stPlaneFlips = 0, stPlaneOnly = 0, stPlaneLatched = 0, stPlaneMaps = 0, stPlaneDropped = 0,
             stPlaneUnderflows = 0, stPlaneUnready = 0, stPlaneComposited = 0;
    // Of the placements, a decoder's own block-linear picture (SwitchNativePlane.h): static, the instance is at the size
    // a kalloc_type class may have.
    static uint32_t stPlaneNative;
    bool kickDeferred = false;                  // a swap is waiting for a frame end (or for the SC7 resume)
    uint64_t flipRequested = 0, flipWaits = 0;  // frameEnds when the last flip went out; swaps that waited for a buffer
    IOTimerEventSource *flipTimer =
        nullptr;  // a flip's deadline: a lost DC A interrupt would otherwise stall the screen
    static constexpr unsigned kFlipDeadlineMS = 100;
    bool settleArmed = false;  // flipTimer is armed to settle requests the stopped scanout cannot take (settleStopped)
    // XNU's console tape (startConsoleTape), from start until the first client frame scans: the pages, their write-combined view
    // and physical address, and whether the console draws there (window A scans it; read by the filter and the console's wait).
    switch_dma_buffer tapeBuffer = {};
    IOMemoryMap *tapeMapping = nullptr;
    uint32_t tapeAddress = 0;
    volatile bool tapeAttached = false;
    bool tapeStrideSet = false;  // the console's first show() set LINE_STRIDE (under XNU's console lock)
    bool tapeSettle = false;
    uint64_t tapeSettleRequested = 0;  // a kick waits for the console's last scroll to latch
    uint64_t tapeFrames = 0;           // frame ends while attached: the console's wait event and count
    bool pitchSaved = false;           // pitch* below hold hekate's window (checkPitchWindow)
    // SC7 (tegra_display_suspend/resume): while hwSuspended the DC's clocks are off, so nothing may touch its registers; swaps are
    // deferred to the resume. Window A's assembly registers, the interrupt enables and masks, and whether the scanout ran (the
    // panel was lit) are kept for the cold re-init.
    bool hwSuspended = false, resumeLit = false;
    static constexpr uint32_t
        kWindowAState[] = {kWinColorDepth,     kWinPosition,     kWinSize,           kWinPrescaled,     kWinHInitialDda,
                           kWinVInitialDda,    kWinDdaIncrement, kWinLineStride,     kWinBufferControl, kWinBlendLayer,
                           kWinbufSurfaceKind, kWinbufStartAddr, kWinbufStartAddrHi, kWinbufHOffset,    kWinbufVOffset,
                           kWinOptions};  // WIN_OPTIONS last: the window is enabled once the rest is in place
    uint32_t savedWindow[sizeof(kWindowAState) / sizeof(kWindowAState[0])] = {};
    uint32_t savedDispWinOptions = 0, savedActControl = 0, savedScratchB = 0, savedIntEnable = 0, savedIntMask = 0;
    // Off-gate copy (hardware rotation only): the kick hands a surface swap's GPU wait, scans and
    // swizzle to copyLoop's thread and returns, so neither the compositor's submitting thread nor the family's gate (and with it the
    // vblank action: vsync notifications, flip latches) waits for them; copyFinished, back under the gate, presents the frame and
    // retires the swap. asyncCopy 2 also retires it when the copy starts, as the GPU hand-off does: the compositor renders its next frame, into its other surface, while this one is copied. The next swap still waits for the copy
    // (transferred). job is written under the gate before the hand-off and by the copy thread until copyFinished takes it back.
    unsigned asyncCopy = 0;
    IOWorkLoop *copyLoop = nullptr;
    IOInterruptEventSource *copySource = nullptr;

    struct CopyJob {
        const OriginalLegacySwapRequest *request;
        IOMemoryMap *source, *target;
        bool ownedSource, retired, read, tiled;
        uint64_t stride, allocation;
        uint32_t width, height;
        IOReturn status;
        uint64_t gpu, cache, copy, readTicks;
        uint32_t attribute;
        int swizzled;  // the copy thread's results
    };

    CopyJob job = {};
#include "SwitchSourceWaitDisplay.h"
#include "SwitchSourceReadyDisplay.h"
    bool copyQueued = false, copyStopping = false,
         presentDeferred = false;  // under the gate; presentDeferred: finished during SC7
    // Steady-state timing: one line every 5 s from the frame-end action, the only periodic log
    // line. A swap's stages: queue (accepted to taken by a kick, flip wait included), map, GPU wait, cache op, content scans, copy, and
    // accepted to retired; client is the compositor's own time from a retire to its next submission (0 when it submitted before the
    // retire); vsync late is V_BLANK interrupt to notification sent; gpu-copy is a GPU frame copy's offer to its completion; attr is the
    // MAIR byte of the source and scanout views (mairAttribute).
    // All under the work-loop gate except stVblanks. Debug kernels only, as the frame timeline is: a release kernel keeps
    // the frame timing the vsync phase and the frame-miss hint run on (stamps, marks, presents) and none of the counters.
    static constexpr bool stats = SWITCH_FRAME_TIMELINE;
    uint64_t statsStart = 0, lastSubmit = 0, lastRetire = 0, deferredAt = 0;
    Span stQueue = {}, stFlipWait = {}, stMap = {}, stGpu = {}, stCache = {}, stScan = {}, stCopy = {}, stRetire = {},
         stInterval = {}, stClient = {}, stVsyncLate = {};
    uint32_t stSwaps = 0, stVsyncs = 0, stGpuTimeouts = 0, stNewMaps = 0, stSourceAttr = 0x100;
    uint32_t stUnderflows = 0;  // DC A frame ends with WIN_A_UF_INT latched (window A ran dry during a line)
    uint32_t stFast = 0,
             stByte = 0;  // block-linear copies: of a tiled source as it is, and by the byte loop
    uint32_t stGpuCopies = 0, stGpuDeclined = 0;
    Span stGpuCopy = {};  // GPU frame copies done, offers declined, offer to completion
    // Once per window, just before a copy, the source is read alone (readPass): next to the copy's time it shows whether the copy
    // waits on reading the surface or on writing the scanout.
    bool stReadDue = true;
    uint64_t stRead = 0;
    uint32_t stIrqBase = 0;  // switch_interrupt_count(105) at the last stats line
    uint32_t stVblanks = 0;  // V_BLANKs the filter took while vsync was armed (atomic: primary interrupt context)
    // Frame pacing (r6-lows). A present is a flip DC A latched, timed at the frame end (or V_BLANK) whose interrupt showed it, as the
    // filter stamped it: the moment the new frame starts to scan. Under the gate unless said otherwise:
    //   presentGap    present-to-present intervals under kGapUs (longer ones are idle screens or stalls: stGaps, not in the quantiles)
    //   stVb          the same intervals in whole frames (1, 2, 3, 4 or more DC A periods)
    //   latency       the compositor's swap submission (validateSwap) to that present
    //   taken         the times of the swap a kick took (noteTaken), until flip() files them under its surface (marks)
    //   stLatch       flips latched at the first frame end after the request, and later
    // From the filter (primary interrupt context, one CPU): feStamp, the last FRAME_END; the DC A frame period (fePeriod*, relaxed
    // atomics, reset by the stats line); stFeFolded, frame ends that came before the action took the previous one (the gate was
    // busy for a whole frame). stFeLate is filter to action, the work loop's delay at each frame end.
    static constexpr uint64_t kGapUs = 250000;
    // The histograms (and their quantiles in the stats line) are debug kernels' only, as the frame timeline is: a release
    // kernel never makes them.
    static constexpr bool frameHists = SWITCH_FRAME_TIMELINE;
    FrameHists *hists = nullptr;  // presentGap, latency, touchScan; from start() on, debug kernels
    uint32_t stVb[4] = {}, stGaps[2] = {}, stPresents = 0, stLatch[2] = {};
    uint64_t lastPresent = 0;
    Span stFeLate = {};
    uint64_t feStamp = 0, feLast = 0, fePeriodSum = 0, fePeriodMin = UINT64_MAX, fePeriodMax = 0;
    uint32_t fePeriodN = 0, stFeFolded = 0;
    // r6-lows (r6h): swaps validateSwap refused this window and the last refusal's reason (2: not one IOSurface in layer 0 alone,
    // a multi-layer or detached-layer swap; 3: geometry; 4: surface; see validateSwap), relaxed atomics: the client's swap path.
    // A frozen screen with CA still rendering shows here as ~300 a window and 0 swaps.
    uint32_t stRejected = 0, stRejectReason = 0;

    // r14-pacing: the second stats line, "SwitchDisplay pacing". Each frame's own times travel with the surface it is drawn into
    // (several frames can be in flight: one asked for, one being copied), mach time: its swap's submission (validateSwap), its kick,
    // of the time between them the part spent waiting for a free surface, and its flip. At its present, swap-to-scan splits into
    // queue (submission to kick: behind the previous copy, or waiting for a surface), copy (kick to flip: the GPU work the copy is
    // ordered after, i.e. the compositor's own rendering of the frame, then the copy) and latch (flip to the boundary that took it).
    // Every present more than 1.5 frames after the previous one (and under kGapUs) is charged to one cause, with the frames it cost:
    //   idle   the compositor turned vsync off since the previous present: it had nothing to draw (no stall)
    //   ca     its swap came at or after the boundary a one-frame interval would have used: the compositor was late (its CPU
    //          render: m2v replay, encode, CPU contention)
    //   gpu    the swap came in time, but kick to flip alone outlasted the time it had left: the compositor's GPU work (or GPU
    //          contention) ended after that boundary
    //   queue  in time, but waiting behind the previous copy or for a surface made it miss
    //   latch  the flip was asked for in time but taken a boundary later
    // Also: frames replaced before they were shown (latest frame wins), swaps retired unshown because a newer one was queued
    // behind them (collapsed), V_BLANKs folded into one notification, notifications more than 1 ms after their V_BLANK, and how
    // often the compositor turned vsync off.
    struct FrameMark {
        uint64_t submit, kick, waited, ready;
        FrameTimeline tl;
    };

    FrameMark taken = {}, marks[TegraScanoutRing::kMaxEntries] = {};

    //   rate   its swap came promptly after the vsync it answered (under half a frame): the frames before it had nothing new
    //          to show (a client drawing at 20 Hz: r8a's clock widget), not a late compositor (r17-touch)
    enum {
        kLongIdle,
        kLongCa,
        kLongGpu,
        kLongQueue,
        kLongLatch,
        kLongRate,
        kLongCauses
    };

    static_assert(SwitchInteraction::kCauses == kLongCauses, "the interaction line names the causes in this order");

    Span stPhaseLead = {};  // r17-touch: the measured V_PULSE2-to-frame-end lead, per frame it carried the vsync
    uint32_t phaseReplacedSeen = 0;  // stReplaced at the last present (replaced flips are phase misses)
    Span stPQueue = {}, stPWait = {}, stPCopy = {}, stPLatch = {};
    // r17-touch: for a present whose swap was submitted after a touch report the screen had not yet followed with one,
    // that report's interrupt to the swap's submission (touch->swap) and to its scan-out (touch->scan): how old the
    // touch input is when a frame that can answer it reaches the panel. A lower bound on touch-to-photon (the frame may
    // answer an older report if the app's pipeline is deeper); the slow-motion test measures the whole of it.
    Span stTouchSwap = {};
    uint32_t stReplaced = 0, stCollapsed = 0, stLong[kLongCauses] = {}, stLongFrames[kLongCauses] = {};
    uint32_t stVsyncFolded = 0, stVsyncLateN = 0, stVsyncIdle = 0;  // stVsyncFolded: atomic (the filter)
    uint32_t stFallbacks = 0;  // recoverFlip: DC A did not take a replaced flip as modelled (once a boot at most)
    bool idleSincePresent = false;
    bool statsQuiet = false;  // the last window logged had nothing on the screen change (noteStatsWindow)

    // Under the gate, from flip(): the frame just written into surface i goes out now.
    void markFlip(int i)
    {
        if (i < 0)
            return;
        marks[i] = taken;
        marks[i].ready = mach_absolute_time();
        taken = {};
    }

    // Under the gate: a flip latched at stamp (mach time); latchedAt is frameEnds - flipRequested (1: at the first frame end); mark
    // is the presented frame's own times (all 0 if not known).
    void notePresent(uint64_t stamp, uint64_t latchedAt, const FrameMark &mark)
    {
        if (!stamp)
            return;
        noteMissedFrame(stamp);
        // r17-touch: a flip replaced unshown since the last present (counted just before this one) missed its boundary.
        // noteStatsWindow zeroes stReplaced every window: a count below the one seen is a new window, not a replacement.
        if (stReplaced < phaseReplacedSeen)
            phaseReplacedSeen = 0;
        if (stReplaced != phaseReplacedSeen) {
            phaseReplacedSeen = stReplaced;
            phase.replaced(0, 0, Span::ticksToUs(stamp));
        }
        notePhasePresent(stamp, mark);
        if (stats)
            notePresentTimes(stamp, latchedAt, mark);
        lastPresent = stamp;
        idleSincePresent = false;
    }

    // Debug kernels: the present's counters for the stats line (lastPresent is still the one before).
    void notePresentTimes(uint64_t stamp, uint64_t latchedAt, const FrameMark &mark)
    {
        ++stPresents;
        ++stLatch[latchedAt <= 1 ? 0 : 1];
        if (mark.submit && stamp > mark.submit) {
            switch_frames_present(vsyncOf(mark), mark.submit, stamp);
            if (const uint64_t touch = switch_touch_take_report(mark.submit)) {
                stTouchSwap.add(mark.submit - touch);
                if (frameHists)
                    hists->touchScan.add(Span::ticksToUs(stamp - touch));
            }
            if (frameHists)
                hists->latency.add(Span::ticksToUs(stamp - mark.submit));
            if (mark.kick >= mark.submit && mark.ready >= mark.kick && stamp >= mark.ready) {
                stPQueue.add(mark.kick - mark.submit);
                stPWait.add(mark.waited);
                stPCopy.add(mark.ready - mark.kick);
                stPLatch.add(stamp - mark.ready);
            }
        }
        uint64_t intervalUs = 0;
        uint32_t periods = 0;
        unsigned cause = kLongCauses;
        frameStall.present(stamp, lastPresent && stamp > lastPresent ? stamp - lastPresent : 0, refreshTicks());
        if (lastPresent && stamp > lastPresent) {
            const uint64_t us = Span::ticksToUs(stamp - lastPresent);
            intervalUs = us;
            if (us >= kGapUs) {
                ++stGaps[us < 1000000 ? 0 : 1];
            } else {
                if (frameHists)
                    hists->presentGap.add(us);
                const uint64_t period = vsyncInterval ? Span::ticksToUs(vsyncInterval) : 16667;
                const uint64_t frames = (us + period / 2) / period;
                periods = static_cast<uint32_t>(frames);
                uint64_t bin = 3;
                if (frames <= 1) {
                    bin = 0;
                } else if (frames < 4) {
                    bin = frames - 1;
                }
                ++stVb[bin];
                if (frames >= 2) {
                    cause = noteLong(static_cast<uint32_t>(frames), mark);
                    if (SWITCH_FRAME_TIMELINE)
                        noteSlowFrame(stamp, us, static_cast<uint32_t>(frames), cause, mark);
                }
            }
        }
        if (frameHists)
            noteInteraction(stamp, intervalUs, periods, cause);
    }

    SwitchFrameStall frameStall;

    uint64_t refreshTicks() const
    {
        uint64_t period = vsyncInterval;
        if (!period)
            nanoseconds_to_absolutetime(16666667ULL, &period);
        return period;
    }

    // Debug kernels: a present inside a touch interaction window (SwitchInteraction.h), after closing an ended one.
    void noteInteraction(uint64_t stamp, uint64_t intervalUs, uint32_t periods, unsigned cause)
    {
        uint64_t began = 0;
        uint64_t lastDown = 0;
        switch_frames_touch_activity(&began, &lastDown);
        const uint64_t now = Span::ticksToUs(stamp);
        const uint64_t beganUs = Span::ticksToUs(began);
        const uint64_t lastDownUs = Span::ticksToUs(lastDown);
        SwitchInteraction &window = hists->interaction;
        const bool active = SwitchInteraction::active(now, beganUs, lastDownUs);
        if (window.isOpen() && !active)
            closeInteraction(lastDownUs);
        if (active)
            window.present(now, intervalUs, periods, cause, beganUs);
    }

    // At a frame end: a window whose run of touches ended kTailUs ago closes even when nothing presents any more.
    void checkInteraction()
    {
        if (!frameHists || !hists->interaction.isOpen())
            return;
        uint64_t began = 0;
        uint64_t lastDown = 0;
        switch_frames_touch_activity(&began, &lastDown);
        const uint64_t lastDownUs = Span::ticksToUs(lastDown);
        if (!SwitchInteraction::active(Span::ticksToUs(mach_absolute_time()), Span::ticksToUs(began), lastDownUs))
            closeInteraction(lastDownUs);
    }

    void closeInteraction(uint64_t lastDownUs)
    {
        char line[900];
        size_t at = hists->interaction.close(lastDownUs, line, sizeof(line));
        if (at + 2 < sizeof(line)) {
            line[at++] = ';';
            line[at++] = ' ';
            line[at] = 0;
            if (!switch_frames_line(true, line + at, sizeof(line) - at))
                line[at - 2] = 0;
        }
        os_log(OS_LOG_DEFAULT, "SwitchFrames interaction: %s", line);
        if (switch_frames_latency_line(line, sizeof(line)))
            os_log(OS_LOG_DEFAULT, "SwitchFrames latency: %s", line);
#if SWITCH_FRAME_TIMELINE
        const auto &totals = frameInfo->tickets.totals;
        os_log(OS_LOG_DEFAULT,
               "SwitchFrames tickets since boot: accepted %llu shown %llu unshown %llu origin-unknown %llu "
               "deadline-unknown %llu deadline-missed %llu overflow %llu",
               totals.accepted, totals.shown, totals.unshown, totals.unknownOrigin, totals.unknownDeadline,
               totals.missedDeadline, totals.overflow);
#endif
    }

    // The vsync a present's swap was drawn for (mach time), or 0 if its timeline has none.
    static uint64_t vsyncOf(const FrameMark &mark)
    {
        if (mark.tl.vsyncUs == kNotSeen || mark.tl.vsyncUs > 0)
            return 0;
        uint64_t back = 0;
        nanoseconds_to_absolutetime(static_cast<uint64_t>(-static_cast<int64_t>(mark.tl.vsyncUs)) * 1000, &back);
        return back < mark.submit ? mark.submit - back : 0;
    }

    // A present frames DC A periods after the previous one (lastPresent, not yet moved on): its cause, see above.
    unsigned noteLong(uint32_t frames, const FrameMark &mark)
    {
        uint64_t period = vsyncInterval;
        if (!period)
            nanoseconds_to_absolutetime(16666667ULL, &period);
        const uint64_t due = lastPresent + period;  // the boundary a one-frame interval would have used
        unsigned cause = 0;
        if (!mark.submit || mark.submit >= due) {
            if (idleSincePresent) {
                cause = kLongIdle;
            } else if (vsyncOf(mark) && phase.paced(Span::ticksToUs(mark.submit))) {
                cause = kLongRate;
            } else {
                cause = kLongCa;
            }
        } else if (mark.ready > due) {
            cause = mark.ready - mark.kick > due - mark.submit ? kLongGpu : kLongQueue;
        } else {
            cause = kLongLatch;
        }
        ++stLong[cause];
        stLongFrames[cause] += frames - 1;
        return cause;
    }

    // Debug kernels: one "SwitchFrame" line per present 1.5 periods or more after the one before (tools/frames reads them),
    // its points in microseconds from the vsync its swap came after (from its submission when none is known).
    static void noteSlowFrame(uint64_t stamp, uint64_t us, uint32_t frames, unsigned cause, const FrameMark &mark)
    {
        static const char *const kCause[kLongCauses] = {"idle", "ca", "gpu", "queue", "latch", "rate"};
        const FrameTimeline &tl = mark.tl;
        const bool fromVsync = (mark.submit != 0u) && tl.vsyncUs != kNotSeen;
        const uint64_t submitNs = mark.submit ? ticksNs(mark.submit) : 0;
        uint64_t origin = mark.submit ? mark.submit : stamp;
        uint64_t back = 0;
        if (fromVsync && tl.vsyncUs < 0)
            nanoseconds_to_absolutetime(static_cast<uint64_t>(-static_cast<int64_t>(tl.vsyncUs)) * 1000, &back);
        origin -= back;
        const uint64_t originNs = ticksNs(origin);
        const auto at = [originNs](uint64_t ticks) -> long long {
            return ticks ? (static_cast<long long>(ticksNs(ticks)) - static_cast<long long>(originNs)) / 1000 : -1;
        };
        const auto rel = [&](int32_t fromSubmitUs) -> long long {
            return mark.submit && fromSubmitUs != kNotSeen
                       ? (static_cast<long long>(submitNs) + static_cast<long long>(fromSubmitUs) * 1000 -
                          static_cast<long long>(originNs)) /
                             1000
                       : -1;
        };
        const char *anchor = "latch";
        if (fromVsync) {
            anchor = "vsync";
        } else if (mark.submit) {
            anchor = "swap";
        }
        char line[640];
        snprintf(line, sizeof(line),
                 "latch %llu after %u frames (%llu us), cause %s; origin %s %llu; first submit %lld, last "
                 "submit %lld (%u, exec %u us), swap %lld, offer %lld, gpu done %lld, flip %lld, latch "
                 "%lld us; other submits %u; swap thread base %d sched %d mode %d qos %d; cpu %u MHz at vsync %u at "
                 "swap; imp %u %u us, bind %u %u us, alloc %u %u us, free %u, clean %u KiB %u us",
                 stamp, frames, us, cause < kLongCauses ? kCause[cause] : "?", anchor, origin, rel(tl.firstUs),
                 rel(tl.lastUs), tl.submits, tl.execUs, at(mark.submit), at(mark.kick), rel(tl.doneUs), at(mark.ready),
                 at(stamp), tl.others, tl.basePri, tl.schedPri, tl.mode, tl.qos, tl.mhzVsync, tl.mhzSwap, tl.imports,
                 tl.importUs, tl.binds, tl.bindUs, tl.allocs, tl.allocUs, tl.frees, tl.cleanKiB, tl.cleanUs);
        os_log(OS_LOG_DEFAULT, "SwitchFrame: %s", line);  // one string: os_log drops a record whose scalars outgrow it
    }

    // A present two or more refresh periods after the one before it, right after one that came a single period
    // after its own (a full-rate animation broke cadence; a steady 30 Hz client is not a miss), and while frames keep
    // coming (gaps under kGapUs; longer ones are a screen that went still): the CPU governor goes to its top point for
    // a while (SwitchPower.h). Independent of the stats; its own stamps, under the gate.
    uint64_t missLast = 0, missFrames = 0;

    void noteMissedFrame(uint64_t stamp)
    {
        const uint64_t last = missLast;
        const uint64_t before = missFrames;
        missLast = stamp;
        missFrames = 0;
        if (!last || stamp <= last)
            return;
        const uint64_t us = Span::ticksToUs(stamp - last);
        if (us >= kGapUs)
            return;
        const uint64_t period = vsyncInterval ? Span::ticksToUs(vsyncInterval) : 16667;
        missFrames = (us + period / 2) / period;
        if (missFrames >= 2 && before == 1)
            switch_perf_hint(SWITCH_PERF_HINT_FRAME_MISS);
    }

    void writeIntMask(uint32_t clear, uint32_t set)
    {
        const IOInterruptState state = IOSimpleLockLockDisableInterrupt(intMaskLock);
        dc[kDcIntMask] = (dc[kDcIntMask] & ~clear) | set;
        IOSimpleLockUnlockEnableInterrupt(intMaskLock, state);
    }

    // Any context. A stopped or powered-down scanout gives no frame end; setScanout unmasks it at the restart. INT_ENABLE keeps
    // FRAME_END latching while it is masked, so the latched one is stale (a frame end long ago, not now) and goes first.
    void unmaskFrameEnd()
    {
        const IOInterruptState state = IOSimpleLockLockDisableInterrupt(intMaskLock);
        if (intMaskLive && !scanoutStopped && !hwSuspended && !(dc[kDcIntMask] & kFrameEndInt)) {
            dc[kDcIntStatus] = kFrameEndInt;
            dc[kDcIntMask] = dc[kDcIntMask] | kFrameEndInt;
        }
        IOSimpleLockUnlockEnableInterrupt(intMaskLock, state);
    }

    // Under the gate, at a frame end: whether a later frame end has work. The first frame ends prove the interrupt alive.
    bool frameEndNeeded()
    {
        return frameEnds < 2 || vsyncArmed || tapeAttached || tapeSettle || kickDeferred || presentDeferred ||
               ring.pending != kNoBuffer || ring.superseded != kNoBuffer || ring.drawing != kNoBuffer ||
               ring.held != kNoBuffer || (rotated && !rotationLatched) || probeRequest || probe.active() ||
               (!probeRan && (probeTables.memory != nullptr) && doubleBuffer) || (planesOn && planeLatch.requested()) ||
               planeProbe.active() || planeHeldSet || planeRecovering || landscapeState.wanted || completionFlipOut();
    }

    void maskIdleFrameEnd()
    {
        const IOInterruptState state = IOSimpleLockLockDisableInterrupt(intMaskLock);
        if (!__atomic_load_n(&gFrameEndHolds, __ATOMIC_ACQUIRE))
            dc[kDcIntMask] = dc[kDcIntMask] & ~kFrameEndInt;
        IOSimpleLockUnlockEnableInterrupt(intMaskLock, state);
    }

public:
    // TegraPanel's contract (native_legacy_frame_end_held): a hold made FRAME_END reach the CPU.
    bool frameEndRouted()
    {
        const IOInterruptState state = IOSimpleLockLockDisableInterrupt(intMaskLock);
        const bool routed = intMaskLive && ((dc[kDcIntEnable] & dc[kDcIntMask] & kFrameEndInt) != 0u);
        IOSimpleLockUnlockEnableInterrupt(intMaskLock, state);
        return routed;
    }

    void holdFrameEnd()
    {
        unmaskFrameEnd();
    }

private:
    bool vsyncWanted() const
    {
        const void *head = nullptr;
        memcpy(&head, reinterpret_cast<const unsigned char *>(this) + vsyncClientList, sizeof(head));
        return head != nullptr;
    }

    // Selector 72's registration of a vsync client ends in kickSwapQueue (slot 290), which arms the tick; the tick stops by itself once
    // the last client is removed.
    void armVsync()
    {
        if (!vsyncTimer || vsyncArmed || !vsyncWanted())
            return;
        vsyncArmed = true;
        if (vblankSource) {
            // A V_BLANK latched while it was masked is stale: drop it, then let the next one through.
            __atomic_fetch_and(&dcPending, ~kVblankInt, __ATOMIC_RELAXED);
            // Powered down for SC7: the resume's scanout restart unmasks V_BLANK for an armed tick. V_PULSE2 (r17-touch's
            // vsync phase) likewise, once programmed: a latched one from before is stale.
            if (!hwSuspended) {
                const uint32_t bits = kVblankInt | (vpulseProgrammed && !phaseFailed ? kVpulse2Int : 0);
                dc[kDcIntStatus] = bits;
                writeIntMask(0, bits);
            }
            native_legacy_vsync_observed(0, vsyncHz);
            return;
        }
        vsyncNext = mach_absolute_time() + vsyncInterval;
        vsyncTimer->wakeAtTime(vsyncNext);
        native_legacy_vsync_observed(0, vsyncHz);
    }

    static void vsyncFired(OSObject *owner, IOTimerEventSource *)
    {
        static_cast<NativeLegacyDisplay *>(owner)->vsyncTick();
    }

    void vsyncTick()
    {
        if (vblankSource) {
            // A polled panel command that overlapped the start may have restored INT_ENABLE without our bits; it is over by now.
            if (frameEnds) {
                dc[kDcIntEnable] = dc[kDcIntEnable] | kVblankInt | kFrameEndInt | (vpulseProgrammed ? kVpulse2Int : 0);
                return;
            }
            char gic[192];
            switch_interrupt_describe(kDisplayAInterrupt, gic, sizeof(gic));
            panic("SwitchDisplay: no DC A frame end in 250 ms: INT_STATUS %#x MASK %#x ENABLE %#x, %s",
                  static_cast<uint32_t>(dc[kDcIntStatus]), static_cast<uint32_t>(dc[kDcIntMask]),
                  static_cast<uint32_t>(dc[kDcIntEnable]), gic);
        }
        vsyncArmed = false;
        if (!vsyncTimer)
            return;
        if (!vsyncWanted()) {
            native_legacy_vsync_observed(2, vsyncHz);
            noteVsyncIdle();
            return;
        }
        const uint64_t now = mach_absolute_time();
        native_legacy_vsync_notify(this, now, mach_continuous_time(), vsyncInterval);
        native_legacy_vsync_observed(1, vsyncHz);
        vsyncNext += vsyncInterval;
        if (vsyncNext <= now) {
            vsyncNext = now + vsyncInterval;
            native_legacy_vsync_observed(3, vsyncHz);
        }
        vsyncArmed = true;
        vsyncTimer->wakeAtTime(vsyncNext);
    }

    // Primary interrupt context. The action is scheduled with signalInterrupt() and the filter returns false: returning
    // true on this level line makes IOFilterInterruptEventSource::disableInterruptOccurred mask it until the work loop has
    // run the action, and TegraPanel's DSI steps and the console tape need every frame end however long the gate is held.
    // The filter clears the DC's status and reads it back first, which is what signalInterrupt() requires.
    static bool vblankFilter(OSObject *owner, IOFilterInterruptEventSource *source)
    {
        NativeLegacyDisplay *self = static_cast<NativeLegacyDisplay *>(owner);
        const uint32_t status = self->dc[kDcIntStatus] & (kVblankInt | kFrameEndInt | kVpulse2Int);
        if (!status)
            return false;
        self->dc[kDcIntStatus] = status;
        (void)self->dc[kDcIntStatus];  // read back: the line is low before ArmCpu's EOI
        if (gPanicRestore.panicking)
            return false;  // window A is the panic's (restorePitchOnPanic): no flip, no DSI command
        if (status & kVblankInt) {
            self->vblankTime = mach_absolute_time();
            self->vblankContinuous = mach_continuous_time();
        }
        if (status & kVpulse2Int) {  // r17-touch: the vsync phase's line
            self->vpulseTime = mach_absolute_time();
            self->vpulseContinuous = mach_continuous_time();
        }
        // The panel's DSI command steps, timed from the frame end as hekate does (the line is already low; the last step takes ~620 us).
        // A console waiting to scroll (waitTapeFrame) wakes at every frame end while it draws on the tape.
        if (status & kFrameEndInt) {
            if (__atomic_load_n(&self->scanoutQuiescing, __ATOMIC_ACQUIRE)) {
                __atomic_fetch_add(&self->scanoutStopFrames, 1ULL, __ATOMIC_RELEASE);
                thread_wakeup(reinterpret_cast<event_t>(&self->scanoutStopFrames));
            }
            tegra_panel_frame_end_filter();
            self->color->frameEndFilter();
            if (self->tapeAttached) {
                __atomic_fetch_add(&self->tapeFrames, 1ULL, __ATOMIC_RELEASE);
                thread_wakeup((event_t)&self->tapeFrames);
            }
            // r6-lows: the frame end's time (the present stamp); debug kernels also the DC A period and a frame end the
            // action has not taken yet.
            const uint64_t now = mach_absolute_time();
            const uint64_t last = self->feLast;
            __atomic_store_n(&self->feStamp, now, __ATOMIC_RELEASE);
            self->feLast = now;
            if (NativeLegacyDisplay::stats) {
                // Only consecutive frame ends (a stopped scanout or SC7 in between is no period).
                if (last && now > last && (!self->vsyncInterval || now - last < 2 * self->vsyncInterval)) {
                    const uint64_t period = now - last;
                    __atomic_fetch_add(&self->fePeriodSum, period, __ATOMIC_RELAXED);
                    __atomic_fetch_add(&self->fePeriodN, 1u, __ATOMIC_RELAXED);
                    if (period < __atomic_load_n(&self->fePeriodMin, __ATOMIC_RELAXED))
                        __atomic_store_n(&self->fePeriodMin, period, __ATOMIC_RELAXED);
                    if (period > __atomic_load_n(&self->fePeriodMax, __ATOMIC_RELAXED))
                        __atomic_store_n(&self->fePeriodMax, period, __ATOMIC_RELAXED);
                }
                if (__atomic_load_n(&self->dcPending, __ATOMIC_ACQUIRE) & kFrameEndInt)
                    __atomic_fetch_add(&self->stFeFolded, 1u, __ATOMIC_RELAXED);
            }
        }
        // Two V_BLANKs before the action runs (the gate busy, e.g. with a copy) fold into one notification; the stats count both.
        if ((status & kVblankInt) && NativeLegacyDisplay::stats && self->vsyncArmed) {
            __atomic_fetch_add(&self->stVblanks, 1u, __ATOMIC_RELAXED);
            if (__atomic_load_n(&self->dcPending, __ATOMIC_ACQUIRE) & kVblankInt)
                __atomic_fetch_add(&self->stVsyncFolded, 1u, __ATOMIC_RELAXED);
        }
        __atomic_fetch_or(&self->dcPending, status, __ATOMIC_RELEASE);
        source->signalInterrupt();
        return false;
    }

    static void vblankFired(OSObject *owner, IOInterruptEventSource *, int)
    {
        static_cast<NativeLegacyDisplay *>(owner)->vblankTick();
    }

    void vblankTick()
    {
        takeCompletionFlip();
        const uint32_t status = __atomic_exchange_n(&dcPending, 0u, __ATOMIC_ACQUIRE);
        if (hwSuspended)
            return;  // a status taken just before the SC7 power-down: the registers are unclocked now
        const uint64_t tickFrom = landscapeRunClock();
        // r17-touch: V_PULSE2 first: it belongs to the frame whose end may be in this same action (a folded one), so its
        // vsync must count against that frame, not the next.
        if (status & kVpulse2Int)
            vpulseNotify();
        if (status & kFrameEndInt)
            frameEnds = frameEnds + 1;
        if ((status & kFrameEndInt) && stats) {
            const uint64_t stamp = __atomic_load_n(&feStamp, __ATOMIC_ACQUIRE);
            const uint64_t now = mach_absolute_time();
            if (stamp && now > stamp)
                stFeLate.add(now - stamp);
        }
        if (status & kFrameEndInt)
            notePhaseFrameEnd();
        if (status & kVblankInt)
            vblankNotify();
        // Core Animation never registers for vsync on either kind (its frame clock is a CADisplayTimer): the frame end
        // stands in for the compositor's vsync so the GPU deadline's window still opens.
        if ((status & kFrameEndInt) && !vsyncArmed)
            tegra_gm20b_compositor_vsync(__atomic_load_n(&feStamp, __ATOMIC_ACQUIRE), 0, refreshTicks());
        watchVsync(status);
        if (status & kFrameEndInt)
            underflowRead();
        if (status & kFrameEndInt) {
            columnScanFrameEnd();
            windowProbeFrameEnd();
        }
        // After the compositor's vsync, since a latched flip starts the next frame's copy right here. A flip is also looked for at
        // V_BLANK (unmasked while a vsync client is registered), in case the controller latches after its FRAME_END.
        if ((status & kFrameEndInt) && rotated) {
            checkRotation();
        } else if ((status & kVblankInt) && rotationLatched && ring.pending != kNoBuffer) {
            checkFlip(false);
        }
        if ((status & kFrameEndInt) && tapeSettle)
            checkTapeSettled();
        if (status & kFrameEndInt)
            probeFrameEnd();
        if (status & kFrameEndInt) {
            planeFrameEnd();
            landscapeFrameEnd();
            landscapeRunFrameEnd();
        }
        if (status & kFrameEndInt)
            underflowFrameEnd();
        if (status & kFrameEndInt) {
            const bool mayRequest = ring.pending == kNoBuffer && !probeRequest && (!rotated || rotationLatched) &&
                                    !columnScanBusy() && !windowProbeHolds();
            color->frameEndAction(mayRequest);
            paletteFrameEnd(mayRequest);
        }
        if ((status & kFrameEndInt) && stats) {
            if (frameStall.frameEnd(mach_absolute_time(), refreshTicks(), vsyncWanted()))
                switch_stall_frame(frameStall.lastPresent());
            checkInteraction();
            noteStatsWindow();
        }
        if (status & kFrameEndInt)
            kickAtIdleFrameEnd();
        landscapeRunCharge(tickFrom);
        if ((status & kFrameEndInt) && !frameEndNeeded() && tegra_panel_holds_frame_end())
            maskIdleFrameEnd();
    }

    // At a frame end, under the gate: after 5 s, the one stats line for the window, then a new window. The line is formatted here and
    // logged as one string: the kernel's os_log encodes scalar arguments into a 256-byte buffer (libkern/os/log.h
    // OS_LOG_BUFFER_MAX_SIZE) and drops a message whose arguments do not fit (log_encode.c, ENOMEM), which is what became of this
    // line's 33 arguments on 10-31; a string argument grows into the 1 KiB log memory instead.
    void noteStatsWindow()
    {
        const uint64_t now = mach_absolute_time();
        uint64_t window = 0;
        nanoseconds_to_absolutetime(5000000000ULL, &window);
        if (!statsStart || now - statsStart < window) {
            if (!statsStart)
                statsStart = now;
            return;
        }
        const uint64_t us = Span::ticksToUs(now - statsStart);
        const uint32_t vblanks = __atomic_exchange_n(&stVblanks, 0u, __ATOMIC_RELAXED);
        const uint32_t irqs = switch_interrupt_count(kDisplayAInterrupt) - stIrqBase;
        stIrqBase += irqs;
        // r16-idle: a window in which nothing reached the screen and nothing went wrong is logged once, marked quiet; the
        // windows after it are counted and reset as always but not logged, until a swap, a present or an error comes. A
        // lit, still screen then writes no display lines at all (it wrote two every 5 s).
        const switch_display::UnderflowAccounting::Totals counted[kUnderflowWindows] = {
            underflows.take(0), underflows.take(1), underflows.take(2)};
        const bool quiet = (stSwaps == 0u) && (stPresents == 0u) && (stUnderflows == 0u) && (stGpuTimeouts == 0u) &&
                           (stNewMaps == 0u) && (stFallbacks == 0u) && (stVsyncIdle == 0u) &&
                           ((counted[0].frames | counted[1].frames | counted[2].frames) == 0u) &&
                           (__atomic_load_n(&stRejected, __ATOMIC_RELAXED) == 0u);
        const bool logged = !(quiet && statsQuiet);
        statsQuiet = quiet;
        // Logged every time, quiet or not: the render server took frames and committed none, which is what a hung
        // client of it looks like from here.
        if (stVsyncs && !stSwaps && !stPresents) {
            os_log(OS_LOG_DEFAULT,
                   "SwitchDisplay: quiet display: %llu ms with vsync %u sent of %u vblanks and no swap or present",
                   (unsigned long long)(us / 1000), stVsyncs, vblanks);
        }
        const auto rate = [us](uint32_t n) {
            return (static_cast<uint64_t>(n) * 10000000ULL / (us ? us : 1));
        };  // per second, in tenths
        char line[960];
        size_t at = 0;
        auto add = [&](const char *format, auto... values) {
            if (at < sizeof(line))
                at += static_cast<size_t>(snprintf(line + at, sizeof(line) - at, format, values...));
        };
        const auto span = [&](const char *name, const Span &value) {
            add(" %s %llu/%llu", name, static_cast<unsigned long long>(value.meanUs()),
                static_cast<unsigned long long>(value.maxUs()));
        };
        add("%llu ms: %u DC A interrupts, %u swaps %llu.%llu/s,", static_cast<unsigned long long>(us / 1000), irqs,
            stSwaps, rate(stSwaps) / 10, rate(stSwaps) % 10);
        span("interval", stInterval);
        span("client", stClient);
        span("submit-retire", stRetire);
        span("(queue", stQueue);
        span("flip-wait", stFlipWait);
        span("map", stMap);
        span("gpu", stGpu);
        span("cache", stCache);
        span("scan", stScan);
        span("copy", stCopy);
        span("gpu-copy", stGpuCopy);
        add(" read %llu", static_cast<unsigned long long>(Span::ticksToUs(stRead)));
        add(") us mean/max; copy fast %u byte %u (cpu-copies %llu since boot) gpu %u (declined %u); vsync %u sent "
            "%llu.%llu/s of %u vblanks,",
            stFast, stByte, copyFrames, stGpuCopies, stGpuDeclined, stVsyncs, rate(stVsyncs) / 10, rate(stVsyncs) % 10,
            vblanks);
        span("late", stVsyncLate);
        add(" us; %u gpu timeouts, %u new maps, %u underflow frames; attr source %#x target %#x", stGpuTimeouts,
            stNewMaps, stUnderflows, stSourceAttr,
            hwRotate ? mairAttribute(reinterpret_cast<const void *>(drawSurface()->getVirtualAddress())) : 0x100u);
        // r6-lows frame pacing: presents (latched flips) and their intervals under 250 ms as quantiles, in whole DC A frames, and
        // the counts over 1.5x and 2x the period (2+ and 3+ frames: presents are whole frames apart); idle or stalled gaps of
        // 250 ms-1 s and over 1 s; swap submission to present; the measured DC A period; frame-end work-loop delay, frame ends
        // folded into one action, swaps that waited for a flip, flips latched at the first frame end or later.
        const auto quantiles = [&](const char *name, const FrameHist &h) {
            const uint64_t top = h.maxUs;
            add(" %s p50 %llu p99 %llu p99.9 %llu max %llu.%llu ms", name,
                static_cast<unsigned long long>(h.quantileUs(500) / 1000),
                static_cast<unsigned long long>(h.quantileUs(990) / 1000),
                static_cast<unsigned long long>(h.quantileUs(999) / 1000), static_cast<unsigned long long>(top / 1000),
                static_cast<unsigned long long>(top % 1000 / 100));
        };
        add("; present %u", stPresents);
        if (frameHists) {
            add(" (%u timed):", hists->presentGap.count);
            quantiles("interval", hists->presentGap);
        }
        add(", frames 1:%u 2:%u 3:%u 4+:%u, >1.5x %u >2x %u, gaps %u/%u;", stVb[0], stVb[1], stVb[2], stVb[3],
            stVb[1] + stVb[2] + stVb[3], stVb[2] + stVb[3], stGaps[0], stGaps[1]);
        if (frameHists)
            quantiles("swap-to-scan", hists->latency);
        {
            const uint64_t sum = __atomic_exchange_n(&fePeriodSum, 0ULL, __ATOMIC_RELAXED);
            const uint32_t n = __atomic_exchange_n(&fePeriodN, 0u, __ATOMIC_RELAXED);
            const uint64_t low = __atomic_exchange_n(&fePeriodMin, UINT64_MAX, __ATOMIC_RELAXED);
            const uint64_t high = __atomic_exchange_n(&fePeriodMax, 0ULL, __ATOMIC_RELAXED);
            uint64_t meanNs = 0;
            if (n)
                absolutetime_to_nanoseconds(sum / n, &meanNs);
            add("; frame %llu.%03llu ms (%llu-%llu us, %u)", static_cast<unsigned long long>(meanNs / 1000000),
                static_cast<unsigned long long>(meanNs / 1000 % 1000),
                static_cast<unsigned long long>(n ? Span::ticksToUs(low) : 0),
                static_cast<unsigned long long>(Span::ticksToUs(high)), n);
        }
        add(", fe-late %llu/%llu us, folded %u, flip-waited %u, latch 1st %u later %u",
            static_cast<unsigned long long>(stFeLate.meanUs()), static_cast<unsigned long long>(stFeLate.maxUs()),
            __atomic_exchange_n(&stFeFolded, 0u, __ATOMIC_RELAXED), stFlipWait.count, stLatch[0], stLatch[1]);
        add(", rejected %u (reason %u)", __atomic_exchange_n(&stRejected, 0u, __ATOMIC_RELAXED),
            __atomic_load_n(&stRejectReason, __ATOMIC_RELAXED));
        if (logged)
            os_log(OS_LOG_DEFAULT, "SwitchDisplay stats: %s", line);  // log only, never the boot console
        // r14-pacing: swap-to-scan by stage, long presents by cause (presents/frames lost), vsync delivery (see FrameMark).
        at = 0;
        add("%u presents: swap-to-scan = queue %llu/%llu (surface wait %llu/%llu) + copy %llu/%llu + latch %llu/%llu "
            "us "
            "mean/max",
            stPresents, static_cast<unsigned long long>(stPQueue.meanUs()),
            static_cast<unsigned long long>(stPQueue.maxUs()), static_cast<unsigned long long>(stPWait.meanUs()),
            static_cast<unsigned long long>(stPWait.maxUs()), static_cast<unsigned long long>(stPCopy.meanUs()),
            static_cast<unsigned long long>(stPCopy.maxUs()), static_cast<unsigned long long>(stPLatch.meanUs()),
            static_cast<unsigned long long>(stPLatch.maxUs()));
        add("; direct %u%s", stDirect, !directOn && latchRetire ? " (off)" : "");
        stDirect = 0;
        const char *buffering = "";
        if (doubleBuffer && !ring.replace)
            buffering = stFallbacks ? " (fell back to double buffering now)" : " (double-buffered)";
        add("; %u buffers%s, replaced %u, collapsed %u", doubleBuffer ? ring.buffers : 1u, buffering, stReplaced,
            stCollapsed);
        // r18-planes: window B sets written (of them by swaps that changed only window B: video frames with no UI composite),
        // seen latched, new slot mappings, detached layers not shown, window B underflow frames, ready-mask mismatches.
        const char *planesState = "-";
        if (planesOn) {
            planesState = "on";
        } else if (planesAttached) {
            planesState = "off";
        }
        add("; planes %s: flips %u (b-only %u) latched %u, maps %u (native %u), dropped %u, underflow %u, unready %u, "
            "composited %u",
            planesState, stPlaneFlips, stPlaneOnly, stPlaneLatched, stPlaneMaps, stPlaneNative, stPlaneDropped,
            stPlaneUnderflows, stPlaneUnready, stPlaneComposited);
        add("; UFLOW_STATUS A %llu (%u frames) B %llu (%u) C %llu (%u)%s",
            static_cast<unsigned long long>(counted[0].counted), counted[0].frames,
            static_cast<unsigned long long>(counted[1].counted), counted[1].frames,
            static_cast<unsigned long long>(counted[2].counted), counted[2].frames,
            counted[0].overflowed || counted[1].overflowed || counted[2].overflowed ? " overflowed" : "");
        stPlaneNative = 0;
        planeTotalsClose();
        stPlaneFlips = stPlaneOnly = stPlaneLatched = stPlaneMaps = stPlaneDropped = stPlaneComposited =
            stPlaneUnderflows = stPlaneUnready = 0;
        add("; long %u: idle %u/%u ca %u/%u gpu %u/%u queue %u/%u latch %u/%u rate %u/%u",
            stLong[kLongIdle] + stLong[kLongCa] + stLong[kLongGpu] + stLong[kLongQueue] + stLong[kLongLatch] +
                stLong[kLongRate],
            stLong[kLongIdle], stLongFrames[kLongIdle], stLong[kLongCa], stLongFrames[kLongCa], stLong[kLongGpu],
            stLongFrames[kLongGpu], stLong[kLongQueue], stLongFrames[kLongQueue], stLong[kLongLatch],
            stLongFrames[kLongLatch], stLong[kLongRate], stLongFrames[kLongRate]);
        add("; vsync folded %u late>1ms %u idle %u", __atomic_exchange_n(&stVsyncFolded, 0u, __ATOMIC_RELAXED),
            stVsyncLateN, stVsyncIdle);
        if (stTouchSwap.count) {
            add("; touch->swap %llu/%llu us mean/max,", static_cast<unsigned long long>(stTouchSwap.meanUs()),
                static_cast<unsigned long long>(stTouchSwap.maxUs()));
            if (frameHists)
                quantiles("touch->scan", hists->touchScan);
            add(" (%u)", stTouchSwap.count);
        }
        // r17-touch: the vsync phase: the lead in effect (0: at V_BLANK), the line, the lead V_PULSE2 gave, the last window's
        // response p99, margin and miss floor, and its misses this boot.
        const char *phaseState = "at V_BLANK";
        if (phaseFailed) {
            phaseState = "off";
        } else if (phaseDelivering()) {
            phaseState = "on";
        }
        add("; phase %s lead %u us (line %u, measured %llu/%llu us), p99 %u margin %u floor %u, misses %u", phaseState,
            phase.lead(), phaseLine, static_cast<unsigned long long>(stPhaseLead.meanUs()),
            static_cast<unsigned long long>(stPhaseLead.maxUs()), phase.lastP99(), phase.marginUs(), phase.floorUs(),
            phase.missCount());
        stPhaseLead = Span{};
        if (quiet)
            add("%s", "; quiet (the next quiet windows are not logged)");
        if (logged)
            os_log(OS_LOG_DEFAULT, "SwitchDisplay pacing: %s", line);  // log only, never the boot console
        // who drew in this window: Core Animation's clients send each commit to the render server as a mach message
        if (switch_render_senders_line(line, sizeof(line), us / 1000, 6) && logged) {
            os_log(OS_LOG_DEFAULT, "SwitchDisplay clients: %llu ms to the render server: %s",
                   (unsigned long long)(us / 1000), line);
        }
        if (switch_frames_line(false, line, sizeof(line)) && logged) {
            os_log(OS_LOG_DEFAULT, "SwitchDisplay commits: %llu ms: %s", (unsigned long long)(us / 1000), line);
        }
        stTouchSwap = Span{};
        if (frameHists)
            hists->touchScan = FrameHist{};
        stPQueue = stPWait = stPCopy = stPLatch = Span{};
        stReplaced = stCollapsed = stVsyncLateN = stVsyncIdle = stFallbacks = 0;
        for (auto &n : stLong)
            n = 0;
        for (auto &n : stLongFrames)
            n = 0;
        if (frameHists)
            hists->presentGap = hists->latency = FrameHist{};
        stFeLate = Span{};
        for (auto &n : stVb)
            n = 0;
        stGaps[0] = stGaps[1] = stPresents = stLatch[0] = stLatch[1] = 0;
        stQueue = stFlipWait = stMap = stGpu = stCache = stScan = stCopy = stRetire = stInterval = stClient =
            stVsyncLate = stGpuCopy = Span{};
        stSwaps = stVsyncs = stGpuTimeouts = stNewMaps = stUnderflows = stFast = stByte = stGpuCopies = stGpuDeclined =
            0;
        stSourceAttr = 0x100;
        statsStart = now;
        stReadDue = true;
        stRead = 0;
    }

    void vblankNotify()
    {
        if (!vsyncArmed)
            return;
        if (!vsyncWanted()) {
            writeIntMask(kVblankInt | kVpulse2Int, 0);  // no client: no wakeup of its own a frame
            vsyncArmed = false;
            native_legacy_vsync_observed(2, vsyncHz);
            noteVsyncIdle();
            return;
        }
        if (phaseDelivering())
            return;  // the compositor's vsync comes from V_PULSE2 now
        deliverVsync(vblankTime, vblankContinuous, 0);
    }

    // r17-touch: the vsync phase (VsyncPhase.h, the phase members above). V_PULSE2 carries the vsync once it has reached the CPU,
    // while a lead is in effect and nothing turned the phase off.
    bool phaseDelivering() const
    {
        return vpulseSeen && !phaseFailed && (phase.lead() != 0u);
    }

    void vpulseNotify()
    {
        vpulseThisFrame = true;
        vpulseSilent = 0;
        if (!vpulseSeen) {
            vpulseSeen = true;
            os_log(OS_LOG_DEFAULT, "SwitchDisplay: V_PULSE2 reached the CPU (line %u of %u): the vsync phase is on",
                   phaseLine, totalLines);
        }
        if (!vsyncArmed || !vsyncWanted() || !phaseDelivering())
            return;
        // Stamped when it goes out, as V_BLANK's is: the render server sees a fresh frame, never one already under way.
        deliverVsync(vpulseTime, vpulseContinuous, measuredLeadUs ? measuredLeadUs : phase.lead());
    }

    // One vsync a frame (a switch between V_BLANK and V_PULSE2 never gives the compositor two in one), at `stamp`; `lead` is the
    // phase it carries (0: at V_BLANK).
    void deliverVsync(uint64_t stamp, uint64_t continuous, uint32_t lead)
    {
        if (deliveredFrame == frameEnds)
            return;
        deliveredFrame = frameEnds;
        // The panel's own period between consecutive notifications; the nominal one after a pause, and when the source or
        // V_PULSE2's line just changed (the one irregular gap, period plus or minus a lead, is never passed on).
        const uint32_t source = lead ? phaseLine : 0;
        const bool moved = source != deliveredSource;
        deliveredSource = source;
        const uint64_t interval = !moved && vblankLast && stamp > vblankLast && stamp - vblankLast < 4 * vsyncInterval
                                      ? stamp - vblankLast
                                      : vsyncInterval;
        vblankLast = stamp;
        if (SWITCH_FRAME_TIMELINE) {
            __atomic_store_n(&vsyncSentAt, stamp, __ATOMIC_RELAXED);
            __atomic_store_n(&vsyncSentKhz, switch_perf_cpu_khz(), __ATOMIC_RELAXED);
            KDBG(SWITCH_KDBG_CODE(SWITCH_KDBG_VSYNC), frameEnds);
            switch_sched_render_vsync(stamp, interval);
        }
        native_legacy_vsync_notify(this, stamp, continuous, interval);
        tegra_gm20b_compositor_vsync(stamp, lead, interval);
        native_legacy_vsync_observed(1, vsyncHz);
        if (stats) {
            ++stVsyncs;
            const uint64_t late = mach_absolute_time() - stamp;
            stVsyncLate.add(late);
            if (Span::ticksToUs(late) > 1000)
                ++stVsyncLateN;
        }
        phase.vsync(Span::ticksToUs(stamp), lead);
    }

    // At each frame end: the lead V_PULSE2 gave this frame (its time to this frame end), its line corrected towards the one
    // asked for; V_BLANK-to-frame-end, which places the line; and V_PULSE2's own lost-interrupt check.
    void notePhaseFrameEnd()
    {
        const uint64_t fe = __atomic_load_n(&feStamp, __ATOMIC_ACQUIRE);
        if (fe && vblankTime) {
            const uint64_t a = fe > vblankTime ? fe - vblankTime : vblankTime - fe;
            if (Span::ticksToUs(a) < 4000)
                feAfterVblankNs = static_cast<int64_t>(Span::ticksToUs(a)) * 1000 * (fe > vblankTime ? 1 : -1);
        }
        if (vpulseThisFrame && fe > vpulseTime && vsyncInterval && fe - vpulseTime < vsyncInterval) {
            measuredLeadUs = static_cast<uint32_t>(Span::ticksToUs(fe - vpulseTime));
            if (phaseDelivering())
                stPhaseLead.add(fe - vpulseTime);
            // Off by more than two lines from the lead asked for: correct the line once (latched with the next flip).
            const uint32_t lineUs = lineNs() / 1000 ? lineNs() / 1000 : 1;
            const int32_t error = (static_cast<int32_t>(measuredLeadUs) - static_cast<int32_t>(phaseAsked)) /
                                  static_cast<int32_t>(lineUs);
            if (phaseAsked && !phaseSettle && (error > 2 || error < -2)) {
                phaseCorrection += error;
                const int32_t bound = static_cast<int32_t>(totalLines);
                if (phaseCorrection > bound) {
                    phaseCorrection = bound;
                } else if (phaseCorrection < -bound) {
                    phaseCorrection = -bound;
                }
                programPhase(phaseAsked);
            }
        }
        vpulseThisFrame = false;
        if (!phaseDelivering() || !vsyncArmed)
            return;
        if (++vpulseSilent < kVsyncSilentFrames)
            return;
        char gic[192];
        switch_interrupt_describe(kDisplayAInterrupt, gic, sizeof(gic));
        panic("SwitchDisplay: no DC A V_PULSE2 in %u frame ends with the vsync phase on: INT_STATUS %#x MASK %#x "
              "ENABLE %#x, %s",
              vpulseSilent, static_cast<uint32_t>(dc[kDcIntStatus]), static_cast<uint32_t>(dc[kDcIntMask]),
              static_cast<uint32_t>(dc[kDcIntEnable]), gic);
    }

    uint32_t lineNs() const
    {
        uint64_t ns = 0;
        if (vsyncInterval)
            absolutetime_to_nanoseconds(vsyncInterval, &ns);
        return totalLines ? static_cast<uint32_t>(ns / totalLines) : 0;
    }

    // V_PULSE2's registers for the line `lead` before the frame end (assembly copies: they latch with the next flip).
    void writePhaseRegisters()
    {
        dc[kDcDispVpulse2Control] = kVpulse2Last;
        dc[kDcDispVpulse2Position] = phaseLine | (phaseLine + 1) << 16;
        dc[kDcDispSignalOptions0] = dc[kDcDispSignalOptions0] | kVpulse2Enable;
    }

    void programPhase(uint32_t leadUs)
    {
        if (!dc || hwSuspended || phaseFailed || !totalLines || !lineNs())
            return;
        const int64_t back = (static_cast<int64_t>(leadUs) * 1000 - feAfterVblankNs) / static_cast<int64_t>(lineNs());
        int64_t line = static_cast<int64_t>(vblankLine) - back - phaseCorrection;
        if (line < 2)
            line = 2;
        if (line > static_cast<int64_t>(vblankLine) - 1)
            line = static_cast<int64_t>(vblankLine) - 1;
        phaseAsked = leadUs;
        if (vpulseProgrammed && static_cast<uint32_t>(line) == phaseLine)
            return;
        phaseLine = static_cast<uint32_t>(line);
        phaseSettle = 2;
        writePhaseRegisters();
        if (!vpulseProgrammed) {
            dc[kDcIntStatus] = kVpulse2Int;
            dc[kDcIntEnable] = dc[kDcIntEnable] | kVpulse2Int;
            if (vsyncArmed)
                writeIntMask(0, kVpulse2Int);  // else armVsync unmasks it
            vpulseProgrammed = true;
            phaseProbe = 0;
        }
    }

    // Under the gate, at each present: what the phase learned from it, the lead it now wants programmed, and the
    // never-booted check (V_PULSE2 programmed, flips latched, not one V_PULSE2).
    void notePhasePresent(uint64_t stamp, const FrameMark &mark)
    {
        if (phaseFailed || !mark.submit || !mark.ready || !vsyncOf(mark))
            return;
        if (vsyncInterval)
            phase.setPeriod(static_cast<uint32_t>(Span::ticksToUs(vsyncInterval)));
        if (phaseSettle)
            --phaseSettle;
        phase.present(Span::ticksToUs(mark.submit), Span::ticksToUs(mark.ready), Span::ticksToUs(stamp));
        if (phase.lead())
            programPhase(phase.lead());
        if (!vpulseProgrammed || vpulseSeen || ++phaseProbe < kPhaseProbeFlips)
            return;
        phaseFailed = true;
        writeIntMask(kVpulse2Int, 0);
        dc[kDcIntEnable] = dc[kDcIntEnable] & ~kVpulse2Int;
        char gic[192];
        switch_interrupt_describe(kDisplayAInterrupt, gic, sizeof(gic));
        IOLog("SwitchDisplay: vsync phase off for this boot: V_PULSE2 never reached the CPU in %u flips (line %u; "
              "SIGNAL_OPTIONS0 %#x V_PULSE2 %#x/%#x INT_STATUS %#x MASK %#x ENABLE %#x, %s); vsync stays at V_BLANK\n",
              phaseProbe, phaseLine, static_cast<uint32_t>(dc[kDcDispSignalOptions0]),
              static_cast<uint32_t>(dc[kDcDispVpulse2Control]), static_cast<uint32_t>(dc[kDcDispVpulse2Position]),
              static_cast<uint32_t>(dc[kDcIntStatus]), static_cast<uint32_t>(dc[kDcIntMask]),
              static_cast<uint32_t>(dc[kDcIntEnable]), gic);
    }

    // The compositor has no vsync client any more: it has nothing to draw until it registers again.
    void noteVsyncIdle()
    {
        if (!stats)
            return;
        ++stVsyncIdle;
        idleSincePresent = true;
    }

    // Every DC A action, under the gate. A vsync client (Core Animation is none on a J120 or a J181: its frame clock is a
    // CADisplayTimer phased by frame info, both kern_exec.c) draws nothing until its first vsync: its registration is found
    // here, at the first frame end after it, whether or not it went through kickSwapQueue (no swap comes to do it). Armed,
    // V_BLANK must come every frame: kVsyncSilentFrames frame ends without one panic, since the client would wait for ever.
    void watchVsync(uint32_t status)
    {
        if (status & kVblankInt)
            vsyncSilent = 0;
        if (!(status & kFrameEndInt))
            return;
        if (!vsyncArmed) {
            vsyncSilent = 0;
            armVsync();
            return;
        }
        if (++vsyncSilent < kVsyncSilentFrames)
            return;
        char gic[192];
        switch_interrupt_describe(kDisplayAInterrupt, gic, sizeof(gic));
        panic("SwitchDisplay: no DC A V_BLANK in %u frame ends with vsync armed: INT_STATUS %#x MASK %#x ENABLE "
              "%#x, %s",
              vsyncSilent, static_cast<uint32_t>(dc[kDcIntStatus]), static_cast<uint32_t>(dc[kDcIntMask]),
              static_cast<uint32_t>(dc[kDcIntEnable]), gic);
    }

    static bool displayClocked()
    {
        return switch_car_clock_enabled(kCarDisp1Id) && !switch_car_reset_asserted(kCarDisp1Id);
    }

    bool startVblank(IOWorkLoop *loop)
    {
        if (!displayClocked())
            panic("SwitchDisplay: DC A clock off or in reset, no vblank interrupt");
        IOService *nub = switch_interrupt_nub(kDisplayAInterrupt, false);
        IODeviceMemory *memory = IODeviceMemory::withRange(kDisplayAPA, kDcMapBytes);
        dcMap = memory ? memory->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
        OSSafeReleaseNULL(memory);
        if (!dcMap)
            panic("SwitchDisplay: cannot map DC A");
        dc = SwitchMMIOWords(static_cast<uintptr_t>(dcMap->getVirtualAddress()));
        color->attach(dc);
        vblankSource = IOFilterInterruptEventSource::filterInterruptEventSource(
            this, &NativeLegacyDisplay::vblankFired, &NativeLegacyDisplay::vblankFilter, nub, 0);
        if (!vblankSource || loop->addEventSource(vblankSource) != kIOReturnSuccess)
            panic("SwitchDisplay: cannot attach the DC A interrupt (INTID %u)", kDisplayAInterrupt);
        dcEnable = dc[kDcIntEnable];
        dcMask = dc[kDcIntMask];
        dc[kDcIntStatus] = kVblankInt | kFrameEndInt;
        dc[kDcIntEnable] = dcEnable | kVblankInt | kFrameEndInt;
        writeIntMask(~0u, (dcMask & ~kVblankInt) | kFrameEndInt);
        intMaskLive = true;
        // r17-touch: the frame's lines, for the vsync phase's V_PULSE2 line (its constants' comment).
        const auto vertical = [this](uint32_t reg) {
            return dc[reg] >> 16 & 0x1fff;
        };
        vblankLine = vertical(kDcDispRefToSync) + vertical(kDcDispSyncWidth) + vertical(kDcDispBackPorch) +
                     vertical(kDcDispActive);
        totalLines = vertical(kDcDispSyncWidth) + vertical(kDcDispBackPorch) + vertical(kDcDispActive) +
                     vertical(kDcDispFrontPorch);
        if (vblankLine < 64 || totalLines < vblankLine - vertical(kDcDispRefToSync)) {
            IOLog("SwitchDisplay: vsync phase off for this boot: DC A timing reads V_BLANK line %u of %u lines\n",
                  vblankLine, totalLines);
            phaseFailed = true;
        }
        vblankSource->enable();
        vsyncTimer->setTimeoutMS(250);
        IOLog("SwitchDisplay: vsync from the DC A vblank interrupt (INTID %u)\n", kDisplayAInterrupt);
        return true;
    }

    // Under the work-loop gate.
    void stopVblank()
    {
        if (!vblankSource)
            return;
        if (hwSuspended)
            panic("SwitchDisplay: stopped while powered down for SC7 (no tegra_display_resume)");
        stopRotation();
        vblankSource->disable();
        if (IOWorkLoop *loop = getWorkLoop())
            loop->removeEventSource(vblankSource);
        OSSafeReleaseNULL(vblankSource);
        writeIntMask(~0u, dcMask);
        const IOInterruptState state = IOSimpleLockLockDisableInterrupt(intMaskLock);
        intMaskLive = false;
        IOSimpleLockUnlockEnableInterrupt(intMaskLock, state);
        dc[kDcIntEnable] = dcEnable;
        dc[kDcIntStatus] = kVblankInt | kFrameEndInt | kVpulse2Int;
        vpulseProgrammed = vpulseSeen = false;
        color->detach();
        dc = SwitchMMIOWords();
        OSSafeReleaseNULL(dcMap);
        dcPending = 0;
    }

    void startVsync()
    {
        constexpr unsigned hz = 60;
        if (!primary || !bootMapping || vsyncTimer)
            return;
        IOWorkLoop *loop = getWorkLoop();
        if (!loop)
            return;
        IOTimerEventSource *timer = IOTimerEventSource::timerEventSource(this, &NativeLegacyDisplay::vsyncFired);
        if (!timer)
            return;
        if (loop->addEventSource(timer) != kIOReturnSuccess) {
            timer->release();
            return;
        }
        nanoseconds_to_absolutetime(1000000000ULL / hz, &vsyncInterval);
        vsyncHz = hz;
        vsyncTimer = timer;
        // A client may already have registered: arm under the gate, like the registration does. The vblank interrupt starts under the
        // gate too, so no registration slips in between.
        loop->runAction(
            [](OSObject *owner, void *a0, void *, void *, void *) -> IOReturn {
                NativeLegacyDisplay *self = static_cast<NativeLegacyDisplay *>(owner);
                if (!self->startVblank(static_cast<IOWorkLoop *>(a0)))
                    native_legacy_vsync_observed(4, self->vsyncHz);
                self->armVsync();
                return kIOReturnSuccess;
            },
            this, loop);
    }

    void stopVsync()
    {
        if (IOWorkLoop *loop = vblankSource ? getWorkLoop() : nullptr) {
            loop->runAction(
                [](OSObject *owner, void *, void *, void *, void *) -> IOReturn {
                    static_cast<NativeLegacyDisplay *>(owner)->stopVblank();
                    return kIOReturnSuccess;
                },
                this);
        }
        if (!vsyncTimer)
            return;
        vsyncTimer->cancelTimeout();
        if (IOWorkLoop *loop = getWorkLoop())
            loop->removeEventSource(vsyncTimer);
        OSSafeReleaseNULL(vsyncTimer);
        vsyncArmed = false;
    }

    // After startVsync. The rotation is only for the Switch's rotated panel (v_rotate 3, the only rotated mode
    // native_legacy_video_mode accepts); an unrotated firmware framebuffer has nothing to turn.
    void startHardwareRotation()
    {
        PE_Video video = {};
        if (!primary || !bootMapping || PE_current_console(&video) || video.v_rotate != 3)
            return;
        if (!vblankSource)
            panic("SwitchDisplay: the hardware rotation needs DC A frame ends to confirm the latch");
        blockMapping = mapScanout(bootMapping->getPhysicalAddress());
        if (!blockMapping || !blockMapping->getVirtualAddress() || blockMapping->getLength() < kNativeBlockBytes) {
            panic("SwitchDisplay: cannot map the %#llx-byte block-linear scanout at %#llx", kNativeBlockBytes,
                  (uint64_t)bootMapping->getPhysicalAddress());
        }
        hwRotate = true;
        startDoubleBuffer();
        // The memory type each CPU view of the scanout really got (MAIR byte: 0x44 write-combined, 0x00/0x04 device): the copy's
        // store rate depends on it.
        const char *buffers = "";
        if (doubleBuffer) {
            buffers = ring.buffers == 3 ? " and two more buffers (triple-buffered, latest frame wins)"
                                        : " and a second buffer (double-buffered)";
        }
        IOLog("SwitchDisplay: hardware rotation armed: block-linear %ux%u at %#llx%s, window A switches at the "
              "first client frame; "
              "memory type %#x (pitch view %#x)\n",
              kNativeBlockWidth, kNativeBlockHeight, static_cast<uint64_t>(bootMapping->getPhysicalAddress()), buffers,
              mairAttribute(reinterpret_cast<const void *>(blockMapping->getVirtualAddress())),
              mairAttribute(reinterpret_cast<const void *>(bootMapping->getVirtualAddress())));
    }

    // A kernel view of a block-linear scanout surface (kNativeBlockBytes at physical address pa), write-combined from the
    // descriptor on: for a physical descriptor IOKit takes the memory type from the descriptor's own cache mode (the default, for
    // RAM the kernel does not manage, is device memory), and the GPU driver found a map-time kIOMapWriteCombineCache alone left its
    // mappings device memory (TegraGM20BUserClient.cpp kPoolCacheMode). Device memory takes every 16-byte store as its own bus write.
    static IOMemoryMap *mapScanout(uint64_t pa, uint64_t bytes = kNativeBlockBytes)
    {
        IOAddressRange range = {pa, bytes};
        IOMemoryDescriptor *memory = IOMemoryDescriptor::withAddressRanges(
            &range, 1,
            static_cast<IOOptionBits>(kIODirectionInOut) | static_cast<IOOptionBits>(kIOMemoryTypePhysical64) |
                (static_cast<IOOptionBits>(kIOWriteCombineCache) << kIOMemoryBufferCacheShift),
            TASK_NULL);
        IOMemoryMap *map = memory ? memory->map(kIOMapAnywhere | kIOMapWriteCombineCache) : nullptr;
        OSSafeReleaseNULL(memory);
        return map;
    }

    // At start, while 3.75 MiB runs of contiguous pages are easy to find. The DC has no SMMU on this boot chain and START_ADDR_HI
    // stays 0, so each buffer is physically contiguous below 4 GiB; 128 KiB aligned (the mask's low zero bits), as a Maxwell big
    // page. Surface 1 is required (as before); without surface 2 the display double-buffers, said in one line.
    bool startScanBuffer(unsigned i, bool required)
    {
        // switch_dma_alloc zeroed the pages and cleaned them out of the kernel's cacheable alias, which nothing writes after
        // this, so no dirty line can land on a frame the write-combined mapping wrote around the cache.
        switch_dma_buffer buffer = {};
        IOMemoryMap *mapping = nullptr;
        if (switch_dma_alloc(&buffer, kNativeBlockBytes, 0x00000000fffe0000ULL) && !(buffer.pa & 0x1ffff)) {
            mapping = mapScanout(buffer.pa);
            if (mapping && (!mapping->getVirtualAddress() || mapping->getLength() < kNativeBlockBytes))
                OSSafeReleaseNULL(mapping);
        }
        if (!mapping) {
            if (required) {
                panic("SwitchDisplay: no mapped, physically contiguous %#llx-byte buffer below 4 GiB for surface %u "
                      "(got %#llx)",
                      kNativeBlockBytes, i, buffer.pa);
            }
            IOLog("SwitchDisplay: no physically contiguous %#llx-byte buffer below 4 GiB for surface %u (got %#llx): "
                  "double-buffered, a late frame delays the ones after it\n",
                  kNativeBlockBytes, i, buffer.pa);
            switch_dma_free(&buffer);
            return false;
        }
        scanBuffers[i] = buffer;
        scanMappings[i] = mapping;
        scanAddresses[i] = buffer.pa;
        return true;
    }

    void startDoubleBuffer()
    {
        startScanBuffer(1, true);
        const unsigned buffers = startScanBuffer(2, false) ? 3 : 2;
        flipTimer = IOTimerEventSource::timerEventSource(this, &NativeLegacyDisplay::flipDeadline);
        if (!flipTimer || getWorkLoop()->addEventSource(flipTimer) != kIOReturnSuccess)
            panic("SwitchDisplay: cannot add the flip deadline timer");
        doubleBuffer = true;
        ring.reset(buffers);
        // The SMMU probe's page tables (directory + tables), physically contiguous below 4 GiB; none: no probe.
        if (!switch_dma_alloc(&probeTables, static_cast<size_t>(kProbeTables) * 4096))
            IOLog("SwitchDisplay: no contiguous %u KiB for the SMMU probe's tables: probe off\n", kProbeTables * 4);
        kickDeferred = false;
    }

    // XNU's text console scrolls by window A's start address (video_console.c vc_tape) from here until the first client frame
    // scans. Its text lines are 16-pixel columns of the portrait scanout, 64 bytes of every memory row, so moving the start address
    // 64 bytes scrolls the whole screen one line, and DC A takes that at a frame boundary like a flip (frame-counter latching,
    // REG_ACT_CONTROL, as startRotation sets it): no frame shows half a scroll, and no pixel is copied. The tape is hekate's pitch
    // window with kTapeGap spare lines after every row: a scroll clears its new line there, outside the scanned window, before the
    // window moves onto it, and the console stays within kTapeGap lines of the latched window (it sleeps in waitTapeFrame until the
    // next frame end otherwise). The first frame leaves the tape through startRotation, hence the hardware rotation; checkRotation
    // lets the tape go once that latched. Without it (no -v, no contiguous memory, a kernel without vc_tape) the console scrolls as
    // before.
    void startConsoleTape()
    {
        if (!hwRotate)
            return;
        checkPitchWindow();
        // As for surface 1: switch_dma_alloc cleaned the zeroing out of the cacheable alias, which nothing writes after this.
        if (switch_dma_alloc(&tapeBuffer, kTapeBytes))
            tapeMapping = mapScanout(tapeBuffer.pa, kTapeBytes);
        if (!tapeMapping || !tapeMapping->getVirtualAddress() || tapeMapping->getLength() < kTapeBytes) {
            IOLog("SwitchDisplay: no physically contiguous %#llx-byte console tape below 4 GiB (got %#llx): the "
                  "console scrolls in software\n",
                  kTapeBytes, tapeBuffer.pa);
            OSSafeReleaseNULL(tapeMapping);
            switch_dma_free(&tapeBuffer);
            return;
        }
        tapeAddress = static_cast<uint32_t>(tapeBuffer.pa);
        tapeStrideSet = false;
        tapeAttached = true;
        dc[kDcRegActControl] = pitchActControl & ~kWinAActHcntrSel;
        dc[kDcStateAccess] = dc[kDcStateAccess] & ~(kReadMuxActive | kWriteMuxActive);
        // XNU redraws its screen on the tape, then calls tapeShow(0), under its console lock.
        if (vc_tape_attach(tapeMapping->getVirtualAddress(), kTapeStride, kTapeGap, kTapeLines, &tapeShow, &tapeLatched,
                           &tapeWait, this)) {
            // Not drawing rotated text (no -v), or no vc_tape in this kernel: nothing was shown.
            tapeAttached = false;
            dc[kDcRegActControl] = pitchActControl;
            OSSafeReleaseNULL(tapeMapping);
            switch_dma_free(&tapeBuffer);
            return;
        }
        chainPanicHook();
        gPanicRestore.tape = true;
        IOLog("SwitchDisplay: the console scrolls by window A's start address (%llu KiB tape at %#x, up to %u lines "
              "ahead of the scan)\n",
              kTapeBytes >> 10, tapeAddress, kTapeGap);
    }

    // XNU's console, under its lock, from any context, a panic included (stores only): window A to tape position line from the next
    // frame boundary. The console wrote the new line first; the barrier puts it in memory before the window can fetch it. The first
    // call also sets the tape's line stride, latched with it.
    static void tapeShow(void *owner, uint32_t line)
    {
        NativeLegacyDisplay *self = static_cast<NativeLegacyDisplay *>(owner);
        if (self->hwSuspended)
            return;  // unclocked for SC7
        const SwitchMMIOWords dc = self->dc;
        __asm__ volatile("dsb sy" ::: "memory");
        dc[kDcWindowHeader] = kWindowASelect;
        if (!self->tapeStrideSet) {
            dc[kWinLineStride] = (self->pitchStride & ~0xffffu) | kTapeStride;
            self->tapeStrideSet = true;
        }
        dc[kWinbufStartAddr] = self->tapeAddress + line * kTapeLineBytes;
        dc[kDcStateControl] = kGeneralUpdate | kWinAUpdate;
        dc[kDcStateControl] = kGeneralActReq | kWinAActReq;
    }

    // XNU's console, under its lock: DC A took the last tapeShow (its ACT_REQ is clear).
    static int tapeLatched(void *owner)
    {
        return static_cast<int>(
            static_cast<int>((static_cast<NativeLegacyDisplay *>(owner)->dc[kDcStateControl] & kWinAActReq)) == 0u);
    }

    static int tapeWait(void *owner)
    {
        return static_cast<NativeLegacyDisplay *>(owner)->waitTapeFrame();
    }

    // XNU's console before a character, without its lock, in a thread that may block (vc_tape_pace): until the next DC A frame end.
    // 0 when none will come (display power off, SC7, a panic, the tape gone): the console scrolls on. Six frames without one panic,
    // naming the GIC state, as every lost DC A interrupt does.
    int waitTapeFrame()
    {
        if (scanoutStopped || hwSuspended || gPanicRestore.panicking || !tapeAttached)
            return 0;
        // No frame end can arrive while the display's interrupt action is pending or running: IOKit keeps the level-triggered
        // DC A line disabled from the filter until vblankFired returns, so a console printing from that action (its stats line)
        // or behind it would wait on itself (the 100 ms panic below, seen on hardware). Scroll without waiting then.
        if (__atomic_load_n(&dcPending, __ATOMIC_ACQUIRE))
            return 0;
        if (const IOWorkLoop *loop = getWorkLoop(); loop && loop->onThread())
            return 0;
        const uint64_t seen = __atomic_load_n(&tapeFrames, __ATOMIC_ACQUIRE);
        uint64_t deadline = 0;
        clock_interval_to_deadline(kTapeWaitMS, kMillisecondScale, &deadline);
        assert_wait_deadline(reinterpret_cast<event_t>(&tapeFrames), THREAD_UNINT, deadline);
        // Asserted before the count is read again: a frame end in between is not missed.
        if (__atomic_load_n(&tapeFrames, __ATOMIC_ACQUIRE) != seen) {
            clear_wait(current_thread(), THREAD_AWAKENED);
            return 1;
        }
        if (thread_block(THREAD_CONTINUE_NULL) != THREAD_TIMED_OUT ||
            __atomic_load_n(&tapeFrames, __ATOMIC_ACQUIRE) != seen)
            return 1;
        if (scanoutStopped || hwSuspended || gPanicRestore.panicking || !tapeAttached)
            return 0;
        char gic[192];
        switch_interrupt_describe(kDisplayAInterrupt, gic, sizeof(gic));
        panic("SwitchDisplay: the console waited %u ms for a DC A frame end to scroll: INT_STATUS %#x MASK %#x "
              "ENABLE %#x, %s",
              kTapeWaitMS, static_cast<uint32_t>(dc[kDcIntStatus]), static_cast<uint32_t>(dc[kDcIntMask]),
              static_cast<uint32_t>(dc[kDcIntEnable]), gic);
    }

    // Under the gate, once window A no longer scans the tape: the console (released at the first frame) leaves it, its pages go.
    void stopConsoleTape()
    {
        if (!tapeAttached)
            return;
        gPanicRestore.tape = false;
        __asm__ volatile("dmb ish" ::: "memory");
        vc_tape_detach();
        tapeAttached = false;
        tapeSettle = false;
        thread_wakeup((event_t)&tapeFrames);  // a console still waiting to scroll: there is no tape now
        OSSafeReleaseNULL(tapeMapping);
        switch_dma_free(&tapeBuffer);
    }

    // Under the gate, in a kick, right after native_legacy_prepare_scanout released XNU's console: its last scroll may still wait for
    // the frame boundary, and startRotation's several stores to window A must not straddle that latch. The kick waits for the frame
    // end that shows it taken (checkTapeSettled).
    bool tapeSettling()
    {
        if (!tapeAttached || rotated || !(dc[kDcStateControl] & kWinAActReq))
            return false;
        if (!tapeSettle) {
            tapeSettle = true;
            tapeSettleRequested = frameEnds;
        }
        kickDeferred = true;
        return true;
    }

    void checkTapeSettled()
    {
        if (dc[kDcStateControl] & kWinAActReq) {
            if (frameEnds - tapeSettleRequested < kLatchFrames)
                return;
            panic("SwitchDisplay: window A never latched the console's last scroll in %u frames: STATE_CONTROL %#x "
                  "ACT_CONTROL %#x START %#x",
                  kLatchFrames, static_cast<uint32_t>(dc[kDcStateControl]), static_cast<uint32_t>(dc[kDcRegActControl]),
                  windowActive(kWinbufStartAddr));
        }
        tapeSettle = false;
        if (kickDeferred) {
            kickDeferred = false;
            kickSwapQueue();
        }
    }

    static void flipDeadline(OSObject *owner, IOTimerEventSource *)
    {
        static_cast<NativeLegacyDisplay *>(owner)->checkFlipDeadline();
    }

    // kFlipDeadlineMS after a flip, under the gate. With the scanout running, six frame ends have come by now and four settle a
    // flip (latched, or checkFlip panics): one still out means DC A's interrupt is lost or window A never latches. A frame end
    // taken by the filter but not yet handled (the gate was busy, e.g. with a copy) only moves the deadline.
    void checkFlipDeadline()
    {
        if (settleArmed) {
            settleArmed = false;
            if (!scanoutStopped) {
                flipTimer->setTimeoutMS(kFlipDeadlineMS);  // restarted before the settle: the deadline runs from now
            } else if (!hwSuspended && !gPanicRestore.panicking) {
                settleStopped();
            }
            return;
        }
        if ((ring.pending == kNoBuffer && !probeRequest && !(planesOn && planeLatch.requested()) && !planeRecovering) ||
            hwSuspended || scanoutStopped || gPanicRestore.panicking)
            return;
        if (__atomic_load_n(&dcPending, __ATOMIC_ACQUIRE) & kFrameEndInt) {
            flipTimer->setTimeoutMS(kFlipDeadlineMS);
            return;
        }
        if (planeRecovering)
            panic("SwitchDisplay: overlay recovery has no completed stop after %u ms", kFlipDeadlineMS);
        char gic[192];
        switch_interrupt_describe(kDisplayAInterrupt, gic, sizeof(gic));
        panic("SwitchDisplay: window A flip to %#x not settled in %u ms (%llu frame ends since): STATE_CONTROL %#x "
              "INT_STATUS %#x MASK %#x ENABLE %#x, %s",
              ring.pending != kNoBuffer ? surfaceAddress(ring.pending)
              : probe.onIova            ? (uint32_t)kProbeSlot
                                        : surfaceAddress(probeSurface),
              kFlipDeadlineMS, frameEnds - flipRequested, static_cast<uint32_t>(dc[kDcStateControl]),
              static_cast<uint32_t>(dc[kDcIntStatus]), static_cast<uint32_t>(dc[kDcIntMask]),
              static_cast<uint32_t>(dc[kDcIntEnable]), gic);
    }

    // Surface i's physical address (window A's START_ADDR); 0 is the boot framebuffer, also the pitch console's.
#include "SwitchScanoutAddress.h"

    // The surface window A scans when its START_ADDR is start; kNoBuffer when none is.
    int surfaceIndex(uint32_t start) const
    {
        for (unsigned i = 0; i < (doubleBuffer ? ring.buffers : 1u); ++i) {
            if (start == surfaceAddress(static_cast<int>(i)))
                return static_cast<int>(i);
        }
        if (directAttached) {
            for (unsigned k = 0; k < kSlots; ++k) {
                if (start == surfaceAddress(kSlot0 + static_cast<int>(k)))
                    return kSlot0 + static_cast<int>(k);
            }
        }
        return kNoBuffer;
    }

    // Window A's layout for entry i: a direct slot is pitch with its surface's stride, a scan buffer block-linear.
    void layoutOf(int i, uint32_t &kind, uint32_t &stride, uint32_t &hOffset) const
    {
        if (i >= kSlot0) {
            kind = slots[i - kSlot0].tiled ? kBlockSurfaceKind : 0;
            stride = slots[i - kSlot0].tiled ? kBlockLineStride : static_cast<uint32_t>(slots[i - kSlot0].stride);
            hOffset = 0;
        } else {
            kind = kBlockSurfaceKind;
            stride = kBlockLineStride;
            hOffset = kBlockHOffset;
        }
    }

    bool sameLayout(int a, int b) const
    {
        uint32_t ka = 0;
        uint32_t sa = 0;
        uint32_t ha = 0;
        uint32_t kb = 0;
        uint32_t sb = 0;
        uint32_t hb = 0;
        layoutOf(a, ka, sa, ha);
        layoutOf(b, kb, sb, hb);
        return ka == kb && sa == sb && ha == hb;
    }

    IOMemoryMap *surfaceMapping(int i) const
    {
        return i > 0 ? scanMappings[i] : blockMapping;
    }

    // Where the next block-linear frame goes: a surface window A neither scans nor may take (ring.target(), the one being drawn
    // while a copy runs), or the only one. A caller that writes it has made sure there is one (kickOnce defers otherwise).
    IOMemoryMap *drawSurface() const
    {
        const int target = doubleBuffer ? ring.target() : 0;
        return surfaceMapping(target == kNoBuffer ? 0 : target);
    }

    // The START_ADDR window A should have once everything requested has latched.
    uint32_t expectedStart() const
    {
        return doubleBuffer ? surfaceAddress(ring.pending != kNoBuffer ? ring.pending : ring.front) : surfaceAddress(0);
    }

    // Under the gate, a frame complete in drawSurface() (the caller's copy or fill): point window A at it from the next frame. A flip
    // still out (not seen taken at a frame end) is replaced: DC A takes whatever START_ADDR its assembly copy holds at the boundary,
    // so the newer frame is the one that scans out there (TegraScanoutRing.h; checkFlip sorts out a boundary that beat this store).
    // Double-buffered after a fallback (recoverFlip), a frame finished while a flip is out is held and flipped at that flip's latch.
    void flip()
    {
        if (flippedByCompletion())
            return;
        if (gPanicRestore.panicking)
            return;
        const int target = ring.target();
        if (target == kNoBuffer) {
            panic("SwitchDisplay: a frame to flip, but no scanout surface was free for it (front %d pending %d "
                  "replaced %d)",
                  ring.front, ring.pending, ring.superseded);
        }
        presentEntry(target);
    }

    // Under the gate: the frame in ring entry i (a scan buffer just written, or a direct slot whose rendering is done) goes out:
    // it replaces a flip still out only when their window layouts match, else it is held for that flip's latch.
#include "SwitchScanoutStart.h"

    // Under the gate, a request just written: checkFlipDeadline looks at it kFlipDeadlineMS from now, or settles it at once while the
    // scanout is stopped.
    void armFlipDeadline()
    {
        if (!flipTimer)
            return;
        settleArmed = scanoutStopped;
        flipTimer->setTimeoutMS(scanoutStopped ? 0 : kFlipDeadlineMS);
    }

    // Under the gate, a block-linear frame complete in drawSurface(): the first one switches window A to the rotated layout (and,
    // double-buffered, to surface 1), later ones flip when double-buffered; single-buffered they are already on screen.
    void presentBlock()
    {
        if (gPanicRestore.panicking)
            return;
        if (!rotated && &switch_boot_io_report)
            switch_boot_io_report();  // once; later calls return at once
        if (!rotated) {
            startRotation();
        } else if (doubleBuffer) {
            flip();
        }
    }

    // A frame end (atFrameEnd) or vblank while a flip is out, under the gate. Which surface window A scans (its active START_ADDR)
    // says what the boundary took: the flip asked for, a flip that was replaced but beat the replacing store (the newer one is
    // then taken at the next boundary), or nothing yet. Each latch frees the surfaces no longer scanned.
    void checkFlip(bool atFrameEnd)
    {
        const auto [control, start] = latchState();
        const int asked = ring.pending;
        const int replaced = ring.superseded;
        const int scanned = ring.front;
        const uint64_t stamp = atFrameEnd ? __atomic_load_n(&feStamp, __ATOMIC_ACQUIRE) : vblankTime;
        const auto latch = ring.latch(surfaceIndex(start), (control & (kGeneralActReq | uiRequest())) != 0u);
        if (latch == TegraScanoutRing::kTaken || latch == TegraScanoutRing::kTakenReplaced)
            releaseIdleDirectReads();
        switch (latch) {
        case TegraScanoutRing::kWaiting:
            if (!atFrameEnd || frameEnds - flipRequested < kLatchFrames)
                return;
            panic("SwitchDisplay: window %c never latched the flip to %#x in %u frames: STATE_CONTROL %#x "
                  "ACT_CONTROL %#x START %#x",
                  uiName(), surfaceAddress(asked), kLatchFrames, control, static_cast<uint32_t>(dc[kDcRegActControl]),
                  start);
        case TegraScanoutRing::kInconsistent:
            // Double-buffered already, this is the check the path had before r14: window A does not show the flip asked for.
            if (!ring.replace) {
                panic("SwitchDisplay: window %c latched START_ADDR %#x, not the flip to %#x (front %#x; "
                      "STATE_CONTROL %#x)",
                      uiName(), start, surfaceAddress(asked), surfaceAddress(scanned), control);
            }
            recoverFlip(start, control, asked, replaced, scanned);
            return;
        case TegraScanoutRing::kTaken:
            landscapeRunLatched(false, false);
            if (SWITCH_FRAME_TIMELINE)
                KDBG(SWITCH_KDBG_CODE(SWITCH_KDBG_LATCH), surfaceAddress(asked), frameEnds - flipRequested);
            if (replaced != kNoBuffer)
                ++stReplaced;  // that frame was never shown
            notePresent(stamp, frameEnds - flipRequested, marks[asked]);
            retireThrough(entrySeq[asked], stamp);
            break;
        case TegraScanoutRing::kTakenReplaced:
            notePresent(stamp, 1, marks[replaced]);  // shown for a frame after all; the newer one is still out
            retireThrough(entrySeq[replaced], stamp);
            break;
        }
        flipLatched();
    }

    // Under the gate, at the first frame end whose window A START_ADDR none of the asked-for flips explains while flips are being
    // replaced: DC A does not take a replaced START_ADDR as TegraScanoutRing.h models it. One line, then double buffering for the
    // rest of the boot, as before r14: the buffer DC A scans becomes the front, the newest complete frame is asked for again (no
    // request is out now, so it is a plain flip), a replaced buffer it does not scan stays untouched until a frame end shows a plain
    // latch, and a copy in flight finishes into its own buffer and waits for that latch. A mismatch after this panics as it did.
    void recoverFlip(uint32_t start, uint32_t control, int asked, int replaced, int scanned)
    {
        IOLog("SwitchDisplay: window A scans %#x after flips to %#x (replaced %#x, front %#x; STATE_CONTROL %#x), "
              "not as "
              "modelled: double-buffered from here (the r7q path)\n",
              start, surfaceAddress(asked), replaced == kNoBuffer ? 0u : surfaceAddress(replaced),
              surfaceAddress(scanned), control);
        const int again = ring.recover(surfaceIndex(start));
        if (again != kNoBuffer)
            writeFlip(again);
        if (stats)
            ++stFallbacks;
    }

    // --- The SMMU probe (DisplaySmmuProbe.h), under the gate ---------------------------------------------------------
    // Its I/O, as DisplaySmmuProbe::perform drives it.
    struct ProbeIo {
        NativeLegacyDisplay *d;

        void tweakPixel() const
        {
            d->probeWritePixel(d->probePixel ^ 0x00010101u);
        }

        void restorePixel() const
        {
            d->probeWritePixel(d->probePixel);
        }

        bool attach() const
        {
            return d->probeAttach();
        }

        void scan(bool iova) const
        {
            const SwitchMMIOWords regs = d->dc;
            regs[kDcStateAccess] = regs[kDcStateAccess] & ~(kReadMuxActive | kWriteMuxActive);
            regs[kDcWindowHeader] = kWindowASelect;
            regs[kWinbufStartAddr] = iova ? static_cast<uint32_t>(kProbeSlot) : d->surfaceAddress(d->probeSurface);
            regs[kDcStateControl] = kGeneralUpdate | kWinAUpdate;
            regs[kDcStateControl] = kGeneralActReq | kWinAActReq;
            // Out until a frame end shows it taken (probeFrameEnd); a lost DC A interrupt panics at the flip deadline.
            d->probeRequest = true;
            d->flipRequested = d->frameEnds;
            d->flipTimer->setTimeoutMS(kFlipDeadlineMS);
        }

        void detach() const
        {
            if (d->probe.stage == DisplaySmmuProbe::kDone && d->startDirect())
                return;               // passed: the domain stays
            if (d->planesAttached) {  // window B's planes use the domain: only the probe's slot goes
                d->probeDomain.unmap(kProbeSlot, static_cast<uint32_t>(kNativeBlockBytes / 4096));
                d->cleanDomain();
                return;
            }
            tegra_smmu_detach(TEGRA_SMMU_ASID_DISPLAY, TEGRA_SMMU_DC_ASID);
        }

        void crcOff() const
        {
            d->dc[kDcCrcControl] = 0;
            d->dc[kDcStateControl] = kGeneralUpdate;
            d->dc[kDcStateControl] = kGeneralActReq;
        }
    };

    // The scanned surface's first word (one pixel's low bits change; the write-combined view, then a barrier).
    void probeWritePixel(uint32_t value)
    {
        *reinterpret_cast<volatile uint32_t *>(surfaceMapping(probeSurface)->getVirtualAddress()) = value;
        __asm__ volatile("dsb sy" ::: "memory");
    }

    // A window other than A that DC A would fetch for (windows B and C, WIN_ENABLE in their active options).
    bool otherWindowEnabled()
    {
        static constexpr uint32_t kWindowBCSelect[] = {1u << 5, 1u << 6};
        for (const uint32_t select : kWindowBCSelect) {
            const uint32_t access = dc[kDcStateAccess] & ~(kReadMuxActive | kWriteMuxActive);
            dc[kDcStateAccess] = access | kReadMuxActive;
            dc[kDcWindowHeader] = select;
            const uint32_t options = dc[kWinOptions];
            dc[kDcWindowHeader] = kWindowASelect;
            dc[kDcStateAccess] = access;
            if (options & kWinEnable)
                return true;
        }
        return false;
    }

    // The domain: every scan surface 1:1 (read-only: DC A only reads), the slot onto the scanned surface; cleaned to memory,
    // then the "dc" swgroup pointed at it. False (probeLine says why) leaves the swgroup as it was.
    bool probeAttach()
    {
        // With planes on, the domain is the plane probe's already (window A's surfaces 1:1 in it): it is extended, not made.
        if (!planesAttached && !probeDomain.init(probeTables.va, probeTables.pa, kProbeTables)) {
            snprintf(probeLine, sizeof(probeLine), "the table buffer cannot hold a directory");
            return false;
        }
        bool ok = true;
        for (unsigned i = 0; i < ring.buffers; ++i)
            ok = ok && probeDomain.identity(surfaceAddress(static_cast<int>(i)), kNativeBlockBytes, false);
        uint64_t pages[64];
        for (uint64_t off = 0; ok && off < kNativeBlockBytes; off += sizeof(pages) / sizeof(pages[0]) * 4096) {
            uint32_t n = 0;
            for (; n < 64 && off + n * 4096ULL < kNativeBlockBytes; ++n)
                pages[n] = surfaceAddress(probeSurface) + off + n * 4096ULL;
            ok = probeDomain.map(kProbeSlot + off, pages, n, false);
        }
        if (!ok) {
            snprintf(probeLine, sizeof(probeLine), "more than %u SMMU tables needed", kProbeTables);
            return false;
        }
        uintptr_t low = 0;
        uintptr_t high = 0;
        probeDomain.takeDirty(&low, &high);
        switch_dcache_clean_invalidate(probeTables.va, static_cast<uint64_t>(kProbeTables) * 4096);
        probeSwgroupWas = tegra_smmu_read(TEGRA_SMMU_DC_ASID);
        return tegra_smmu_attach(TEGRA_SMMU_ASID_DISPLAY, probeDomain.ptb(), TEGRA_SMMU_DC_ASID,
                                 TEGRA_SMMU_TRANSLATION_ENABLE_0, kDisplay0aTranslation, probeLine,
                                 sizeof(probeLine)) == 0;
    }

    // Every DC A frame end: start the probe once things are quiet, or take its next step.
    void probeFrameEnd()
    {
#if (defined(DEVELOPMENT) && DEVELOPMENT) || (defined(DEBUG) && DEBUG)
        if (!scanoutQualification)
            return;
        if (probeRequest && !(dc[kDcStateControl] & (kGeneralActReq | kWinAActReq))) {
            probeRequest = false;
            if (!probe.active())
                probeHoldSince = 0;
            if (!probe.active() && kickDeferred) {  // the probe's last request (back to the physical address) is in
                kickDeferred = false;
                kickSwapQueue();
            }
        }
        if (!probe.active()) {
            if (probeRan || !probeTables.memory || !doubleBuffer || !rotationLatched || hwSuspended || scanoutStopped ||
                tapeAttached || gPanicRestore.panicking)
                return;
            if (!probeAt)
                probeAt = frameEnds + kProbeAfterFrames;
            if (frameEnds < probeAt || transferred || ring.pending != kNoBuffer || ring.drawing != kNoBuffer ||
                ring.held != kNoBuffer || ring.superseded != kNoBuffer)
                return;
            probeRan = true;
            probeSurface = ring.front;
            if (planesAttached &&
                (planeOnScreen() || planeHeldSet)) {  // a plane is shown: its frames would move the CRC
                probeRan = false;
                probeAt = frameEnds + 60;
                return;
            }
            if (otherWindowEnabled()) {
                IOLog("SwitchDisplay: SMMU probe not run: window B or C is enabled (it would fetch through the SMMU "
                      "too)\n");
                return;
            }
            probePixel = *reinterpret_cast<volatile uint32_t *>(surfaceMapping(probeSurface)->getVirtualAddress());
            probeUnderflow = false;
            dc[kDcCrcControl] = kCrcOn;
            dc[kDcStateControl] = kGeneralUpdate;
            dc[kDcStateControl] = kGeneralActReq;
            probe.start();
            probeHoldSince = mach_absolute_time();
            return;
        }
        const bool requestOut = (dc[kDcStateControl] & (kGeneralActReq | kWinAActReq)) != 0u;
        const DisplaySmmuProbe::Action action = probe.frameEnd(dc[kDcCrcLatched], requestOut, probeUnderflow);
        probeUnderflow = false;
        ProbeIo io = {this};
        if (probe.perform(action, io))
            finishProbe();
#endif
    }

    // The probe is over (passed, or failed and undone): its line, and the swaps it held go.
    // NOLINTNEXTLINE(misc-no-recursion): kickSwapQueue's kicking flag stops re-entry; a nested kick only loops
    void finishProbe()
    {
        if (probe.stage == DisplaySmmuProbe::kDone) {
            IOLog("SwitchDisplay: SMMU probe ok: DC A reads window A through the SMMU (ASID %u, swgroup dc, MC %#x "
                  "was "
                  "%#x, TRANSLATION_ENABLE_0 %#x): CRC %#x by physical address, 1:1 and at IOVA %#llx, %#x with one "
                  "pixel "
                  "changed; %s\n",
                  static_cast<unsigned>(TEGRA_SMMU_ASID_DISPLAY), static_cast<unsigned>(TEGRA_SMMU_DC_ASID),
                  probeSwgroupWas, tegra_smmu_read(TEGRA_SMMU_TRANSLATION_ENABLE_0), probe.base,
                  static_cast<unsigned long long>(kProbeSlot), probe.tweaked,
                  directOn ? "the domain stays: direct scanout on" : "back in bypass");
        } else {
            IOLog("SwitchDisplay: SMMU probe failed at %s: %s%s%s (CRC base %#x again %#x pixel %#x last %#x, "
                  "first latched at frame end %u of %u); undone, direct scanout stays off\n",
                  DisplaySmmuProbe::name(probe.failedAt), probe.why, probeLine[0] ? ": " : "", probeLine, probe.base,
                  probe.again, probe.tweaked, probe.seen, probe.firstLatch, probe.frames);
        }
        if (!probeRequest)
            probeHoldSince = 0;
        if (kickDeferred && !probeRequest) {  // else at the frame end that shows the last request taken
            kickDeferred = false;
            kickSwapQueue();
        }
    }

    // In a kick held by the probe: past kProbeHoldMs the probe is undone (abortProbe) and its last start-address request no
    // longer holds swaps (a later flip supersedes it; DC A takes the assembly copy at the boundary either way). True: go.
    // NOLINTNEXTLINE(misc-no-recursion): kickSwapQueue's kicking flag stops re-entry; a nested kick only loops
    bool releaseProbeHold()
    {
        uint64_t bound = 0;
        nanoseconds_to_absolutetime(static_cast<uint64_t>(kProbeHoldMs) * 1000000ULL, &bound);
        if (!probeHoldSince || mach_absolute_time() - probeHoldSince < bound)
            return false;
        if (probe.active())
            abortProbe("it held the display's swaps too long (frame ends stopped?)");
        if (probeRequest) {
            IOLog("SwitchDisplay: SMMU probe: its last start-address request was not seen taken in %u ms; swaps go "
                  "on\n",
                  kProbeHoldMs);
            probeRequest = false;
        }
        probeHoldSince = 0;
        return true;
    }

    // The display is going away or to sleep with the probe running: undo now (registers still clocked).
    // NOLINTNEXTLINE(misc-no-recursion): kickSwapQueue's kicking flag stops re-entry; a nested kick only loops
    void abortProbe(const char *reason)
    {
        if (!probe.active())
            return;
        ProbeIo io = {this};
        probe.perform(probe.fail(reason), io);
        finishProbe();
    }

    // A surface left the scan: a frame held for the flip slot goes out (double-buffered after a fallback), and a swap waiting for a
    // surface may go.
    void flipLatched()
    {
        if (blanked) {
            blanked = false;
            color->setBlank(false);
        }
        if (const int held = ring.flipHeld(); held != kNoBuffer) {
            writeFlip(held);
            // r16-idle: nothing is out any more, so the deadline has nothing to check; left armed it woke the CPU 100 ms after
            // the last flip of every burst. The next writeFlip arms it again.
        } else if (ring.pending == kNoBuffer && !probeRequest && !planeLatch.requested() && !planeHeldSet &&
                   !planeRecovering && flipTimer) {
            flipTimer->cancelTimeout();
            settleArmed = false;
        }
        if (kickDeferred) {
            kickDeferred = false;
            kickSwapQueue();
        }
    }

    // Under the gate, with the scanout stopped (display power off): no boundary takes a request until the restart, whose first
    // boundary takes the newest one. Every request written is therefore as shown as it will get: both windows' models take it now,
    // and the swaps taken up to it retire instead of waiting for a latch only the restart brings (IOMFB's swap wait gives up on a
    // swap after 5 s and cancels the queue).
    // NOLINTNEXTLINE(misc-no-recursion): kickSwapQueue's kicking flag stops re-entry; a nested kick only loops
    void settleStopped()
    {
        flipUnverifiedFromGate();
        const int asked = ring.pending;
        uint64_t seq = asked != kNoBuffer ? entrySeq[asked] : 0;
        ring.settle();
        releaseIdleDirectReads();
        if (planeLatch.requested()) {
            planeLatch.settle();
            landscapeLatched(false);
            const uint64_t planeSeq = planeSetOf(planeLatch.active).seq;
            seq = planeSeq > seq ? planeSeq : seq;
        }
        if (seq)
            retireThrough(seq, mach_absolute_time());
        if (planeHeldSet && planeLatch.mayWrite()) {
            planeHeldSet = false;
            planeFlipOnly(planeHeld);
        }
        planeReleaseIdle();
        if (asked != kNoBuffer) {
            flipLatched();
        } else if (kickDeferred) {
            kickDeferred = false;
            kickSwapQueue();
        }
    }

    // Under the gate. A read of window A's active copy: the read mux follows STATE_ACCESS bit 0 and the window
    // WINDOW_HEADER; the write mux stays on the assembly copy.
    uint32_t windowActive(uint32_t index)
    {
        const uint32_t access = dc[kDcStateAccess] & ~(kReadMuxActive | kWriteMuxActive);
        dc[kDcStateAccess] = access | kReadMuxActive;
        dc[kDcWindowHeader] = kWindowASelect;
        const uint32_t value = dc[index];
        dc[kDcStateAccess] = access;
        return value;
    }

    // Window A, read from its active copy, must be hekate's pitch window (_di_winA_pitch) or, with the console tape attached, that
    // window on the tape: the rotation and the tape reinterpret the same window and would only scramble anything else. The first
    // check keeps hekate's values for every way back (stopRotation, restorePitchWindow, the panic hook).
    void checkPitchWindow()
    {
        const uint32_t options = windowActive(kWinOptions);
        const uint32_t size = windowActive(kWinSize);
        const uint32_t prescaled = windowActive(kWinPrescaled);
        const uint32_t dda = windowActive(kWinDdaIncrement);
        const uint32_t stride = windowActive(kWinLineStride);
        const uint32_t kind = windowActive(kWinbufSurfaceKind);
        const uint32_t start = windowActive(kWinbufStartAddr);
        const uint32_t startHigh = windowActive(kWinbufStartAddrHi);
        const uint32_t hOffset = windowActive(kWinbufHOffset);
        const uint32_t vOffset = windowActive(kWinbufVOffset);
        const uint32_t depth = windowActive(kWinColorDepth);
        const uint32_t position = windowActive(kWinPosition);
        const bool boot =
            (stride & 0xffff) == 2880 && start == static_cast<uint32_t>(bootMapping->getPhysicalAddress());
        const bool tape = tapeAttached && (stride & 0xffff) == kTapeStride && start >= tapeAddress &&
                          start - tapeAddress < kTapeLines * kTapeLineBytes &&
                          (((start - tapeAddress) % kTapeLineBytes) == 0u);
        if ((options & (kWinEnable | kScanColumn | kHDirection | kVDirection)) != kWinEnable || size != kPitchSize ||
            prescaled != kPitchPrescaled || dda != kUnitDda || !(boot || tape) || (kind & 3) || startHigh || hOffset ||
            vOffset || position || (depth != 0xc && depth != 0xd)) {
            panic("SwitchDisplay: window A is not hekate's pitch window: OPTIONS %#x SIZE %#x PRESCALED %#x DDA %#x "
                  "STRIDE %#x KIND %#x "
                  "START %#x:%#x H %#x V %#x POS %#x DEPTH %#x",
                  options, size, prescaled, dda, stride, kind, startHigh, start, hOffset, vOffset, position, depth);
        }
        if (pitchSaved)
            return;
        pitchSaved = true;
        pitchOptions = options;
        pitchStride = stride;
        pitchKind = kind;
        pitchHOffset = hOffset;
        pitchActControl = dc[kDcRegActControl];
    }

    void scanoutMemoryBarrier()
    {
        __asm__ volatile("dsb sy" ::: "memory");
    }

    // From the first change to window A (the console tape or the rotation) on, a panic may have to put hekate's pitch window back
    // over the boot framebuffer (restorePitchOnPanic). The hook is chained once and stays; armed and tape say whether it acts.
    void chainPanicHook()
    {
        gPanicRestore.dc = dc;
        gPanicRestore.pixels = reinterpret_cast<volatile uint64_t *>(bootMapping->getVirtualAddress());
        gPanicRestore.bytes = bootMapping->getLength();
        gPanicRestore.options = pitchOptions;
        gPanicRestore.stride = pitchStride;
        gPanicRestore.kind = pitchKind;
        gPanicRestore.hOffset = pitchHOffset;
        gPanicRestore.start = surfaceAddress(0);
        __asm__ volatile("dmb ish" ::: "memory");  // a CPU that panics sees the fields before the hook that reads them
        switch_display_panic_hook = restorePitchOnPanic;
        __asm__ volatile("dmb ish" ::: "memory");
    }

    // Under the gate: hekate's pitch window over the boot framebuffer again, from the next frame boundary.
    void restorePitchWindow()
    {
        dc[kDcStateAccess] = dc[kDcStateAccess] & ~(kReadMuxActive | kWriteMuxActive);
        dc[kDcWindowHeader] = kWindowASelect;
        dc[kWinLineStride] = pitchStride;
        dc[kWinbufSurfaceKind] = pitchKind;
        dc[kWinbufHOffset] = pitchHOffset;
        dc[kWinbufStartAddr] = surfaceAddress(0);
        dc[kWinOptions] = pitchOptions;
        __asm__ volatile("dsb sy" ::: "memory");
        dc[kDcStateControl] = kGeneralUpdate | kWinAUpdate;
        dc[kDcStateControl] = kGeneralActReq | kWinAActReq;
        dc[kDcRegActControl] = pitchActControl;
    }

#include "SwitchScanoutRetirement.h"

    // Under the gate, before the controller is unmapped: back to hekate's pitch window over a black frame, or, from the console
    // tape, over the console's screen (it redraws it in the boot framebuffer first; the tape may be read once more, never written).
    void stopRotation()
    {
        withdrawCompletionFlip();
        columnScanAbort("the display stopped");
        windowProbeAbort("the display stopped");
        landscapeRunAbort();
        abortProbe("the display stopped");
        if (dc && !hwSuspended)
            landscapeLeaveNow("the display stopped");
        if (rotated || tapeAttached || directAttached || planesAttached)
            retireScanoutStorage();
        stopDirect();
        if (rotated) {
            gPanicRestore.armed = false;
            __asm__ volatile("dmb ish" ::: "memory");
            if (bootMapping) {
                native_legacy_fill_pixels(reinterpret_cast<volatile void *>(bootMapping->getVirtualAddress()),
                                          bootMapping->getLength(), 2880, 720, 1280, 0xff000000U);
            }
            restorePitchWindow();
            rotated = false;
            rotationLatched = false;
        } else if (tapeAttached) {
            gPanicRestore.tape = false;
            __asm__ volatile("dmb ish" ::: "memory");
            vc_tape_detach();
            restorePitchWindow();
        }
        stopConsoleTape();
        hwRotate = false;
        OSSafeReleaseNULL(blockMapping);
        doubleBuffer = false;
        kickDeferred = false;
        ring.reset(0);
        for (unsigned i = 1; i < kScanBuffers; ++i) {
            OSSafeReleaseNULL(scanMappings[i]);
            switch_dma_free(&scanBuffers[i]);
            scanAddresses[i] = 0;
        }
        if (flipTimer) {
            flipTimer->cancelTimeout();
            if (IOWorkLoop *loop = getWorkLoop())
                loop->removeEventSource(flipTimer);
            OSSafeReleaseNULL(flipTimer);
        }
    }

#include "SwitchCpuScanoutCopy.h"

public:
    // Display power (SwitchBrightness; on unless switch-no-sleep), under the work-loop gate. Off, once the panel is asleep: FRAME_END and V_BLANK
    // masked (no display interrupt, so idle cores stay in WFI) and the controller stopped at the end of the frame (hekate
    // _di_dc_video_disable_config, without its power gating, so the restart needs no re-init). DSI stays in video mode. On: continuous
    // display again, then the same interrupts as before; TegraPanel's next command waits for the first frame end and panics without it.
    bool setScanout(bool on)
    {
        if (!dc)
            return false;
        if (!on && !hwSuspended) {
            columnScanAbort("the display went off");
            windowProbeAbort("the display went off");
            landscapeRunAbort();
        }
        if (!on && !hwSuspended && planeProbe.active()) {  // r18-planes: no frame end comes once the scan stops
            PlaneProbeIo io = {this};
            planeProbe.perform(planeProbe.fail("the display went off"), io);
            planeProbeFinish();
        }
        if (!on && !hwSuspended && (probe.active() || probeRequest)) {
            // No frame end comes once the scan stops: undo the probe now, and its last request is taken when the scan
            // restarts (the assembly copy holds the physical address); the swaps it held go.
            abortProbe("the display went off");
            probeRequest = false;
            probeHoldSince = 0;
            if (kickDeferred) {
                kickDeferred = false;
                kickSwapQueue();
            }
        }
        // Powered down for SC7: the resume restarts the scanout (and wakes the panel) if the last request was on.
        if (hwSuspended) {
            resumeLit = on;
            return true;
        }
        if (on != scanoutStopped)
            return true;
        if (!on)
            writeIntMask(kVblankInt | kFrameEndInt, 0);
        // Assembly copy for both the read and the write, as the rotation code leaves it: only the ACT_REQ below moves the mode.
        dc[kDcStateAccess] = dc[kDcStateAccess] & ~(kReadMuxActive | kWriteMuxActive);
        dc[kDcDisplayCommand] = (dc[kDcDisplayCommand] & ~kDcModeMask) | (on ? kDcModeContinuous : 0);
        dc[kDcStateControl] = kGeneralUpdate;
        dc[kDcStateControl] = kGeneralActReq;
        dc[kDcStateControl] = kGeneralUpdate;
        dc[kDcStateControl] = kGeneralActReq;
        __atomic_fetch_and(&dcPending, ~(kVblankInt | kFrameEndInt), __ATOMIC_RELAXED);
        // A window underflow latched around the stop is not the rotated scanout failing (checkRotation counts runs of them).
        dc[kDcIntStatus] = kVblankInt | kFrameEndInt | uiUnderflow();
        clearUnderflowCounts();
        underflowRun = 0;
        scanoutStopped = !on;
        if (on) {
            writeIntMask(0, kFrameEndInt | (vsyncArmed ? kVblankInt : 0));
        } else {
            landscapeScanoutStopped();
            armFlipDeadline();  // what is in flight now settles
        }
        if (!on && tapeAttached)
            thread_wakeup((event_t)&tapeFrames);  // no frame end comes now: the console scrolls without one
        return true;
    }

    // SC7, step 1 (tegra_display_suspend), under the gate: whether the panel is lit, i.e. needs TegraPanel's power-off first.
    bool beginSuspend()
    {
        if (!dc || !vblankSource)
            panic("SwitchDisplay: SC7 needs the DC A interrupt");
        if (hwSuspended)
            panic("SwitchDisplay: tegra_display_suspend twice without tegra_display_resume");
        return !scanoutStopped;
    }

    // SC7, step 2, under the gate, the panel asleep: the scanout stops as for display power off, window A's assembly registers (what
    // was last requested, a flip or the rotation not latched yet included) and the interrupt state are kept, and from here nothing
    // touches the controller until the resume. The panel comes back lit if the scanout still ran now (a display power request
    // between the steps is honoured, not undone).
    void quiesceForSuspend()
    {
        suspendCompletionFlip();
        columnScanAbort("the display suspended (SC7)");
        windowProbeAbort("the display suspended (SC7)");
        landscapeRunAbort();
        abortProbe("the display suspended (SC7)");
        const uint64_t shown = suspendShownSeq();
        planesQuiesce();  // r18-planes
        resumeLit = !scanoutStopped;
        setScanout(false);
        dc[kDcStateAccess] = dc[kDcStateAccess] & ~(kReadMuxActive | kWriteMuxActive);
        dc[kDcWindowHeader] = kWindowASelect;
        for (size_t i = 0; i < sizeof(kWindowAState) / sizeof(kWindowAState[0]); ++i)
            savedWindow[i] = dc[kWindowAState[i]];
        savedDispWinOptions = dc[kDcDispWinOptions];
        savedActControl = dc[kDcRegActControl];
        savedScratchB = dc[kDcScratchB];
        savedIntEnable = dc[kDcIntEnable];
        savedIntMask = dc[kDcIntMask];
        // The requested surface is complete (flip() follows the copy); it is the one window A gets back.
        ring.settle();
        gPanicRestore.armed = false;
        __asm__ volatile("dmb ish" ::: "memory");
        vblankSource->disable();
        hwSuspended = true;
        retireSuspendShown(shown);
    }

#include "SwitchDisplayResume.h"

    // Called by the GPU client's completion (any thread): the swap is finished on the framebuffer's work loop. The GPU frame copy's
    // completion (any context, even the GPU's interrupt filter) records its status, writes a direct frame's armed window A flip
    // (SwitchCompletionFlipDisplay.h) and signals the work loop.
    void gpuComplete(int status)
    {
        status = completionFlipVerdict(status);
        if (SWITCH_FRAME_TIMELINE) {
            __atomic_store_n(&gpuDoneAt, mach_absolute_time(), __ATOMIC_RELAXED);
            KDBG(SWITCH_KDBG_CODE(SWITCH_KDBG_PRESENT_DONE), gpuSequence, status);
        }
        if (gpuDoneSource) {
            __atomic_store_n(&gpuStatus, status, __ATOMIC_RELAXED);
            __atomic_store_n(&gpuDonePending, true, __ATOMIC_RELEASE);
            writeCompletionFlip(status);
            gpuDoneSource->interruptOccurred(nullptr, nullptr, 0);
            return;
        }

        struct Args {
            NativeLegacyDisplay *display;
            int status;
        } args = {this, status};

        IOWorkLoop *loop = getWorkLoop();
        const auto action = [](OSObject *, void *a0, void *, void *, void *) -> IOReturn {
            const Args *a = static_cast<Args *>(a0);
            a->display->finishGpuSwap(a->status);
            return kIOReturnSuccess;
        };
        if (loop) {
            loop->runAction(action, this, &args);
        } else {
            finishGpuSwap(status);
        }
    }

    void free() override
    {
        forgetPrimary();
        stopCopyLoop();
        stopGpuCopy();
        stopPlaneComposite();
        stopVsync();
        if (logicalPM) {
            PMstop();
            logicalPM = false;
        }
        primary = false;
        OSSafeReleaseNULL(bootMapping);
        scanAddresses[0] = 0;
        if (hists)
            IOFreeType(hists, FrameHists);
        if (color) {
            color->free();
            IOFreeType(color, TegraDisplayColor);
        }
        if (frameInfo)
            IOFreeType(frameInfo, switch_frame_info::State);
        if (planeSets)
            IODelete(planeSets, PlaneSet, PlaneCommits::kSlots + 1);
        releasePlaneUses();
        if (entryPlane)
            IODelete(entryPlane, PlaneSet, TegraScanoutRing::kMaxEntries);
        if (intMaskLock)
            IOSimpleLockFree(intMaskLock);
        IOMobileFramebufferLegacy::free();
    }

    void stop(IOService *provider) override
    {
        forgetPrimary();
        stopCopyLoop();
        stopGpuCopy();
        stopPlaneComposite();
        stopVsync();
        transferred = false;
        for (auto &entry : validated)
            entry = nullptr;
        gpuRequest = nullptr;
        gpuRetired = false;
        if (logicalPM) {
            PMstop();
            logicalPM = false;
        }
        primary = false;
        IOMobileFramebufferLegacy::stop(provider);
        OSSafeReleaseNULL(bootMapping);
        scanAddresses[0] = 0;
    }

    // The panel is already lit by the firmware's GOP framebuffer, which is the one thing the original code's power-state word (the
    // "powered" half of its requested/powered state pair) is meant to say; the platform subclass that normally writes it is not part of
    // this port, so without it the user client's wait (selector 6) answers kIOReturnBusy forever and the compositor never presents. This
    // is the original state setter, called with the request shape the original code uses, with only the powered word named: it stores
    // each word not skipped that differs from the framebuffer's and, when one did, announces the change (the original's own virtual
    // call), which is what wakes that wait.
    void announcePoweredByFirmware()
    {
        struct StateRequest {
            uint32_t skipRequested, requested, skipPowered, powered;
            uint8_t changed;
            uint8_t pad[11];
        } request = {1, 0, 0, 1, 1, {}};

        setDisplayPowerState(&request, false);
    }

    bool start(IOService *provider) override
    {
        planeSets = IONewZero(PlaneSet, PlaneCommits::kSlots + 1);
        entryPlane = IONewZero(PlaneSet, TegraScanoutRing::kMaxEntries);
        if (!planeSets || !entryPlane)
            return false;
        for (unsigned k = 0; k <= PlaneCommits::kSlots; ++k)
            planeSets[k] = {};
        for (unsigned k = 0; k < TegraScanoutRing::kMaxEntries; ++k)
            entryPlane[k] = {};
        planeLatch.values = planeSets;
        color = IOMallocType(TegraDisplayColor);
        if (color == nullptr || !color->init()) {
            return false;
        }
        setEdrScalingEnabled(true);
        intMaskLock = IOSimpleLockAlloc();
        if (intMaskLock == nullptr) {
            return false;
        }
        frameInfo = IOMallocType(switch_frame_info::State);
        if (frameInfo == nullptr) {
            return false;
        }
        if (frameHists) {
            hists = IOMallocType(FrameHists);
            if (!hists)
                return false;
        }
        if (!startDisplay(provider))
            return false;
        startVsync();
        startHardwareRotation();
        if (IOWorkLoop *loop = hwRotate ? getWorkLoop() : nullptr) {
            loop->runAction(
                [](OSObject *owner, void *, void *, void *, void *) -> IOReturn {
                    static_cast<NativeLegacyDisplay *>(owner)->startConsoleTape();
                    return kIOReturnSuccess;
                },
                this);
        }
        startCopyLoop();
        startGpuCopy();
        startVicCopy();
        if (IOWorkLoop *loop = getWorkLoop()) {
            loop->runAction(
                [](OSObject *owner, void *, void *, void *, void *) -> IOReturn {
                    static_cast<NativeLegacyDisplay *>(owner)->initializeScanout();
                    return kIOReturnSuccess;
                },
                this);
        }
        if (primary) {
            __atomic_store_n(&primaryDisplay, this, __ATOMIC_RELEASE);
            columnScanRegister();
            dcClassRegister();
            planeTotalsRegister();
        }
        return true;
    }

    static NativeLegacyDisplay *primaryDisplay;

    void forgetPrimary()
    {
        // NOLINTNEXTLINE(misc-const-correctness): the expected value's type must be primaryDisplay's
        NativeLegacyDisplay *self = this;
        __atomic_compare_exchange_n(&primaryDisplay, &self, nullptr, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    }

    bool startDisplay(IOService *provider)
    {
        if (!IOMobileFramebufferLegacy::start(provider))
            return false;
        if (primary && bootMapping)
            announcePoweredByFirmware();
        if (!provider || !bootMapping || !primary) {
            native_legacy_pm_observed(1, kIOReturnNotReady, 0);
            IOMobileFramebufferLegacy::stop(provider);
            return false;
        }
        // Actual IOKit PM lifecycle for logical scanout scheduling only. GOP
        // provides no panel power switch: neither state advertises physical
        // power output, power control or a hardware power transition.
        static IOPMPowerState states[2] = {};
        states[0].version = states[1].version = 1;
        states[1].capabilityFlags = kIOPMDeviceUsable;
        PMinit();
        logicalPM = initialized;
        if (!logicalPM) {
            native_legacy_pm_observed(1, kIOReturnNotReady, 0);
            IOMobileFramebufferLegacy::stop(provider);
            return false;
        }
        provider->joinPMtree(this);
        IOReturn status = registerPowerDriver(this, states, 2);
        native_legacy_pm_observed(1, status, 0);
        if (status == kIOReturnSuccess) {
            status = changePowerStateToPriv(1);
            native_legacy_pm_observed(2, status, 1);
        }
        if (status != kIOReturnSuccess) {
            PMstop();
            logicalPM = false;
            IOMobileFramebufferLegacy::stop(provider);
            return false;
        }
        return true;
    }

    IOReturn setPowerState(unsigned long ordinal, IOService *device) override
    {
        if (!logicalPM)
            return IOMobileFramebufferLegacy::setPowerState(ordinal, device);
        if (ordinal > 1 || !bootMapping) {
            native_legacy_pm_observed(3, kIOReturnUnsupported, ordinal);
            return kIOReturnUnsupported;
        }
        native_legacy_pm_observed(3, kIOReturnSuccess, ordinal);
        const IOReturn status = IOMobileFramebufferLegacy::setPowerState(ordinal, device);
        native_legacy_pm_observed(4, status, ordinal);
        return status;
    }

    // The explicit offline system-client experiment publishes only the real
    // GOP mode. Unknown backend calls retain their stop-with-caller guards.
    void registerService(IOOptionBits options = 0) override
    {
        IOService::registerService(options);
        native_legacy_registration_observed(this);
    }

    bool isMainDisplay() override
    {
        return primary;
    }

    // getRefreshAttributes (slot 301) is not overridden: at 60 Hz the generic 1/60 s refresh words are already right.
    // A swap the client submitted while the previous one was still on its way (transferred) is not kicked by the family again: without the
    // re-kick after each retirement it waited for the next submission (int-143: every offer came one frame late, a still screen showed the
    // previous frame until the user touched it again). A retirement inside a kick only asks the running kick to loop.
    bool kicking = false, rekick = false;

    // NOLINTNEXTLINE(misc-no-recursion): kickSwapQueue's kicking flag stops re-entry; a nested kick only loops
    void kickSwapQueue() override
    {
        unmaskFrameEnd();
        armVsync();
        if (kicking) {
            rekick = true;
            return;
        }
        kicking = true;
        const uint64_t kickFrom = landscapeRunClock();
        do {
            rekick = false;
            kickOnce();
            gpuBypass = false;
        } while (rekick && !transferred);
        landscapeRunCharge(kickFrom);
        kicking = false;
    }

    // NOLINTNEXTLINE(misc-no-recursion): kickSwapQueue's kicking flag stops re-entry; a nested kick only loops
    void kickOnce()
    {
        if (transferred || !logicalPM || !bootMapping || !primary)
            return;
        // Retiring at the latch, the head may be a swap already on screen or asked for: the next one not yet taken goes.
        const OriginalLegacySwapRequest *request =
            consoleReleasedFrame(latchRetire ? firstUntaken() : pendingSwapRequest());
        if (!request || !isValidated(request))
            return;
        if (presentNotDue(request))
            return;
        if ((probe.active() || probeRequest) && !releaseProbeHold()) {  // window A is the SMMU probe's for a few frames
            kickDeferred = true;
            return;
        }
        if (planeProbe.active()) {  // r18-planes: window B covers the screen and the probe's CRC must not move
            kickDeferred = true;
            return;
        }
        if (columnScanBusy() || windowProbeHolds()) {  // windows A and B are a display experiment's (debug kernels)
            kickDeferred = true;
            return;
        }
        if (landscapeHoldsKicks()) {  // a change of mode goes out alone (SwitchLandscapeDisplay.h)
            kickDeferred = true;
            return;
        }
        pruneValidated();
        // r18-planes: what this swap does to each window, from the family's effective layers (a layer it does not update keeps
        // its surface). A swap that leaves window A alone (a video frame under a still UI) changes window B only: no GPU work.
        if (hwSuspended) {
            kickDeferred = true;
            return;
        }
        PlaneDecision pd;
        planeDecide(request, pd);
        if (pd.waitSlot) {
            kickDeferred = true;
            return;
        }
        if (consoleHolds()) {
            kickHeld(request, mach_absolute_time());
            return;
        }
        if (!pd.uiNew) {
            kickPlaneOnly(request, pd, mach_absolute_time());
            return;
        }
        planeNextSet = pd.planeChange;
        planeNext = pd.set;
        // Every surface is held (one scanned, one asked for, and with three a replaced one not yet known to be untaken, or a flip
        // out with two), or the controller is unclocked during SC7: the frame end that settles a latch, or the resume, kicks again.
        const bool direct = directOn && !gpuBypass && directCandidate(pd.ui);
        const bool noSurface = doubleBuffer && (direct ? ring.held != kNoBuffer || ring.superseded != kNoBuffer
                                                       : ring.target() == kNoBuffer);
        if (noSurface || hwSuspended) {
            if (noSurface && !kickDeferred) {
                flipWaits = flipWaits + 1;
                if (stats)
                    deferredAt = mach_absolute_time();
            }
            kickDeferred = true;
            return;
        }
        // r14-pacing: a newer accepted swap queued behind this one makes it stale: shown, it would hold the newer one back a frame
        // (or be replaced at once). Every accepted swap is a whole frame, so it retires unshown, as an early retire does at a
        // copy's start, and the newest one goes.
        if (!latchRetire) {
            const OriginalLegacySwapRequest *first = request;
            for (const OriginalLegacySwapRequest *next = queuedAfter(request); next && isValidated(next);
                 next = queuedAfter(request)) {
                if (!retireSwap(request, true))
                    break;
                if (stats)
                    ++stCollapsed;
                request = pendingSwapRequest();
                if (!request || !isValidated(request))
                    return;
            }
            if (request != first) {  // the newest swap goes: its own layers (planes are off here: no window B change)
                planeDecide(request, pd);
                if (!pd.uiNew) {
                    kickPlaneOnly(request, pd, mach_absolute_time());
                    return;
                }
                planeNextSet = pd.planeChange;
                planeNext = pd.set;
            }
        }
        const uint64_t kickAt = mach_absolute_time();
        // This bounded first-frame transfer preserves the original queue's
        // resource ownership. Separately gated retirement follows the copy;
        // neither path signals a hardware fence, announces a vblank or
        // claims general producer synchronization.
        // The owned GPU instrument completes its work before submission.
        // The separate original software-renderer experiment uses completed
        // synchronous CPU surfaces, with no GPU or display fence fabricated.
        // Window A's surface: the topmost full-frame BGRA layer (layer 0 alone, or layer 1 over a detached layer).
        OSObject *surface = pd.blank ? nullptr : pd.ui;
        if (!surface) {
            // Validated blank swap (all layers disabled over black): solid-fill the actual GOP.
            PE_Video blank = {};
            uint32_t bw = 0;
            uint32_t bh = 0;
            getDisplayDimensions(&bw, &bh);
            uint64_t blankLength = static_cast<uint64_t>(bw) * bh * 4;
            if (PE_current_console(&blank) || blank.v_baseAddr != bootMapping->getPhysicalAddress() ||
                !native_legacy_video_mode(blank) || native_legacy_video_width(blank) != bw ||
                native_legacy_video_height(blank) != bh ||
                !native_legacy_surface_bounds(static_cast<uint32_t>(blank.v_width),
                                              static_cast<uint32_t>(blank.v_height), blank.v_rowBytes,
                                              bootMapping->getLength(), bootMapping->getLength())) {
                native_legacy_transfer_observed(kIOReturnNotReady, request, 0);
                return;
            }
            if (!native_legacy_prepare_scanout()) {
                native_legacy_transfer_observed(kIOReturnNotReady, request, 0);
                return;
            }
            if (tapeSettling())
                return;
            // Once window A is rotated it reads the whole block-linear extent, which reaches past the pitch frame (block row 5). While
            // it scans the console tape, a blank frame leaves it as a first client frame does: block-linear, through startRotation.
            // Rotated and double-buffered with no flip out, the colour unit blacks the head from the next frame (no frame is
            // filled), until the first flip latched after it (flipLatched); with a flip out that flip would end the black
            // too early, and single-buffered frames never flip.
            const bool block = rotated || tapeAttached;
            const bool colourBlank = rotated && rotationLatched && doubleBuffer && ring.pending == kNoBuffer &&
                                     ring.held == kNoBuffer && ring.drawing == kNoBuffer &&
                                     ring.superseded == kNoBuffer;
            if (colourBlank) {
                blanked = true;
                color->setBlank(true);
                blankLength = 0;
            } else if (block ? !native_legacy_block_fill(
                                   reinterpret_cast<volatile void *>(drawSurface()->getVirtualAddress()),
                                   drawSurface()->getLength(), 0xff000000U)
                             : !native_legacy_fill_pixels(
                                   reinterpret_cast<volatile void *>(bootMapping->getVirtualAddress()),
                                   bootMapping->getLength(), blank.v_rowBytes, static_cast<uint32_t>(blank.v_width),
                                   static_cast<uint32_t>(blank.v_height), 0xff000000U)) {
                native_legacy_transfer_observed(kIOReturnBadArgument, request, 0);
                return;
            }
            __asm__ volatile("dsb sy" ::: "memory");
            noteTaken(request, kickAt);  // before the flip, which carries it to the present
            if (block && !colourBlank)
                presentBlock();
            transferred = true;
            native_legacy_frame_observed(1, 0, 0, 0, 0, 0);
            {
                const uint32_t colors[4] = {0xff000000U, 0, 0, 0};
                const uint64_t counts[4] = {static_cast<uint64_t>(bw) * bh, 0, 0, 0};
                native_legacy_frame_colors_observed(colors, counts, 0);
            }
            native_legacy_transfer_observed(kIOReturnSuccess, request, blankLength);
            finishTransfer(request);
            return;
        }
        if (!surface->metaCast("IOSurface"))
            return;
        planeShownUi = surface;
        IOMemoryDescriptor *memory = native_legacy_surface_descriptor(surface);
        PE_Video video = {};
        uint32_t width = 0;
        uint32_t height = 0;
        getDisplayDimensions(&width, &height);
        const uint64_t length = static_cast<uint64_t>(width) * height * 4;
        const uint64_t sourceStride = native_legacy_surface_stride(surface);
        const uint64_t allocation = native_legacy_surface_allocation(surface);
        if (!memory || !memory->metaCast("IOMemoryDescriptor") ||
            !native_legacy_surface_bounds(width, height, sourceStride, allocation, memory->getLength()) ||
            native_legacy_surface_width(surface) < width || native_legacy_surface_height(surface) < height ||
            !nativeBgra(native_legacy_surface_format(surface), native_legacy_surface_planes(surface)) ||
            PE_current_console(&video) || video.v_baseAddr != bootMapping->getPhysicalAddress() ||
            !native_legacy_video_mode(video) || native_legacy_video_width(video) != width ||
            native_legacy_video_height(video) != height ||
            !native_legacy_surface_bounds(static_cast<uint32_t>(video.v_width), static_cast<uint32_t>(video.v_height),
                                          video.v_rowBytes, bootMapping->getLength(), bootMapping->getLength())) {
            native_legacy_transfer_observed(kIOReturnNotReady, request, 0);
            return;
        }
        // The diagnostic renderer repaints the page over the scanout until it is told the system owns the display; the GPU path needs that as much
        // as the CPU copy (int-093: accepted swaps were GPU-copied under a page that kept drawing).
        if (!gpuBypass) {
            if (!native_legacy_prepare_scanout()) {
                native_legacy_transfer_observed(kIOReturnNotReady, request, 0);
                return;
            }
            if (tapeSettling())
                return;
        }
        noteSurface(surface, memory);
        const int blockLog2 = surfaceBlockLog2(memory);
        const bool tiled = blockLog2 >= 0;
        // A block-linear frame in other blocks than window A's reaches the screen only through the GPU copy, which reads
        // its layout: window A cannot scan it, nor a CPU copy take its bytes as they are.
        const bool asScanned = !tiled || blockLog2 == static_cast<int>(kScanBlockLog2);
        if (direct && asScanned &&
            offerDirect(request, surface, memory, sourceStride, allocation, width, height, tiled, kickAt))
            return;
        if (direct && ring.target() == kNoBuffer) {  // no slot either: the frame end that frees a surface kicks again
            kickDeferred = true;
            return;
        }
        if (gpuCopy && !gpuBypass &&
            offerGpuCopy(request, memory, sourceStride, allocation, width, height, blockLog2, kickAt))
            return;
        if (offerVicCopy(request, surface, sourceStride, allocation, width, height, blockLog2, kickAt))
            return;
        if (!asScanned) {
            native_legacy_transfer_observed(kIOReturnUnsupported, request, 0);
            return;
        }
        if (!gpuBypass && !(planeNextSet && planeFenced(planeNext)) &&
            native_legacy_gpu_offer(this, memory, sourceStride, width, height)) {
            // The GPU writes the scanout memory behind the CPU's back: the differential copy's picture of it is stale from now on.
            native_legacy_swap_stage(1);
            gpuRequest = request;
            gpuLength = length;
            transferred = true;
            // The compositor renders the next frame only after this swap retires, so retiring at the end of the copy serialized render and
            // copy (int-243: about 4.4 ms render then about 3 ms copy per frame). Retire now: the presenter holds its own reference to this
            // frame's memory, the previous frame's copy has finished (transferred held this one back), and the next swap still waits for
            // the copy because transferred stays set until finishGpuSwap.
            noteTaken(request, kickAt);
            if (!latchRetire && retireSwap(request, true)) {
                gpuRetired = true;
                native_legacy_early_retires = native_legacy_early_retires + 1;
            }
            return;
        }
        if (cpuCopyRefused(request, kickAt))
            return;
        const uint64_t mapAt = stats ? mach_absolute_time() : 0;
        bool ownedSource = true;
        IOMemoryMap *source = sourceMapping(memory, ownedSource);
        const auto releaseSource = [&] {
            if (ownedSource)
                OSSafeReleaseNULL(source);
            releaseSourceWait();
        };
        if (!source || !source->getVirtualAddress() || source->getLength() < allocation) {
            releaseSource();
            native_legacy_transfer_observed(kIOReturnNoMemory, request, 0);
            return;
        }
        if (stats)
            stMap.add(mach_absolute_time() - mapAt);
        const void *sourceAddress = reinterpret_cast<const void *>(source->getVirtualAddress());
        if (asyncCopy && hwRotate) {
            if (!native_legacy_block_layout(reinterpret_cast<volatile void *>(drawSurface()->getVirtualAddress()),
                                            drawSurface()->getLength(), sourceAddress, source->getLength(),
                                            sourceStride, width, height)) {
                releaseSource();
                native_legacy_transfer_observed(kIOReturnBadArgument, request, 0);
                return;
            }
            startCopy(request, source, ownedSource, sourceStride, allocation, width, height, tiled, kickAt);
            return;
        }
        uint64_t gpu = 0, cache = 0;
        if (const int error = holdCopySource(surface) ? readyCopySource(sourceAddress, allocation, gpu, cache) : -12) {
            releaseSource();
            native_legacy_transfer_observed(error == TEGRA_SCANOUT_SOURCE_LOST ? kIOReturnAborted : kIOReturnNotReady,
                                            request, 0);
            return;
        }
        if (stats) {
            stGpu.add(gpu);
            stCache.add(cache);
        }
        if (hwRotate
                ? !native_legacy_block_layout(reinterpret_cast<volatile void *>(drawSurface()->getVirtualAddress()),
                                              drawSurface()->getLength(), sourceAddress, source->getLength(),
                                              sourceStride, width, height)
                : !native_legacy_scanout_layout(reinterpret_cast<volatile void *>(bootMapping->getVirtualAddress()),
                                                bootMapping->getLength(), video, sourceAddress, source->getLength(),
                                                sourceStride, width, height)) {
            releaseSource();
            native_legacy_transfer_observed(kIOReturnBadArgument, request, 0);
            return;
        }
        if (!native_legacy_prepare_scanout()) {
            releaseSource();
            native_legacy_transfer_observed(kIOReturnNotReady, request, 0);
            return;
        }
        if (stats && stReadDue && hwRotate) {
            stReadDue = false;
            stRead = readPass(sourceAddress, sourceStride, width, height);
        }
        noteTaken(request, kickAt);  // before the flip, which carries it to the present
        const uint64_t copyStart = counterNow();
        uint64_t stored = UINT64_MAX;
        if (stored == UINT64_MAX && hwRotate) {
            // One unrotated swizzling pass; the first such frame switches window A under it (latched at a frame boundary;
            // single-buffered, the frame scanned while this copy runs shows the new bytes through the old pitch window once).
            // Double-buffered, the pass fills the surface window A does not scan and window A flips to it.
            const Swizzled how =
                swizzle(drawSurface(), sourceAddress, source->getLength(), sourceStride, width, height, tiled);
            if (how == kSwizzleFailed) {
                releaseSource();
                native_legacy_transfer_observed(kIOReturnBadArgument, request, 0);
                return;
            }
            if (stats)
                noteSwizzled(how);
            stored = length;
            presentBlock();
        }
        if (stored == UINT64_MAX) {
            if (!native_legacy_scanout_copy(reinterpret_cast<volatile void *>(bootMapping->getVirtualAddress()),
                                            bootMapping->getLength(), video, sourceAddress, source->getLength(),
                                            sourceStride, width, height)) {
                releaseSource();
                native_legacy_transfer_observed(kIOReturnBadArgument, request, 0);
                return;
            }
            stored = length;
        }
        __asm__ volatile("dsb sy" ::: "memory");
        {
            const uint64_t copyEnd = counterNow();
            native_legacy_copy_timing_observed(copyEnd - copyStart, stored);
            noteCopy(copyEnd - copyStart);
            if (stats) {
                stCopy.add(copyEnd - copyStart);  // the architected counter: mach time's unit on arm64
                if (stSourceAttr == 0x100)
                    stSourceAttr = mairAttribute(sourceAddress);  // the first page is mapped now: the copy read it
            }
        }
        releaseSource();
        transferred = true;
        native_legacy_transfer_observed(kIOReturnSuccess, request, length);
        finishTransfer(request);
    }

    // XNU's console keeps the screen until window A first shows a client frame, and the data migration holds it there
    // (switch_console_hold). Released (native_legacy_prepare_scanout), the console stops drawing, so no kick may release it
    // while the hold lasts.
    bool consoleHolds() const
    {
        if (rotated || &switch_console_hold == nullptr)
            return false;
        return switch_console_hold != 0;
    }

    bool holdAnnounced = false;

    // A swap kicked while the console holds the screen: nothing is drawn or flipped and the console keeps drawing; the swap
    // retires as shown (with what is still out, retiring at the latch), so Core Animation keeps its pages.
    // NOLINTNEXTLINE(misc-no-recursion): kickSwapQueue's kicking flag stops re-entry; a nested kick only loops
    void kickHeld(const OriginalLegacySwapRequest *request, uint64_t kickAt)
    {
        if (!holdAnnounced) {
            holdAnnounced = true;
            IOLog("SwitchDisplay: the console keeps the screen while the data migration runs: client frames retire "
                  "undrawn until it ends\n");
        }
        noteTaken(request, kickAt);
        retireUnshown(seqOf(request));
        if (!latchRetire) {
            transferred = true;
            finishTransfer(request, true);
            return;
        }
        rekick = true;  // the next swap may already be queued behind this one: take it in the same kick
    }

    // A swap the kick has finished with (copied, filled or handed to the GPU), before it retires: its queue time and flip wait.
    void noteTaken(const OriginalLegacySwapRequest *request, uint64_t kickAt)
    {
        take(request);
        flipRequest = request;
        // r6-lows/r14-pacing: carried to the flip of this frame (flip, markFlip), then to its present (notePresent).
        taken = {submittedAt(request), kickAt, 0, 0, timelineOf(request)};
        noteFrameInfoTaken(request);
        if (gpuRequest == request) {
            frameInfo->tickets.gpu(seqOf(request), gpuSequence);
#if SWITCH_FRAME_TIMELINE
            const auto *ticket = frameInfo->tickets.find(request);
            KDBG(SWITCH_KDBG_CODE(SWITCH_KDBG_PRESENT), gpuSequence, ticket ? ticket->id : 0,
                 ticket ? ticket->swapId : 0, seqOf(request));
#endif
        }
        if (taken.submit && stats)
            stQueue.add(kickAt > taken.submit ? kickAt - taken.submit : 0);
        if (deferredAt) {
            taken.waited = kickAt > deferredAt ? kickAt - deferredAt : 0;
            if (stats)
                stFlipWait.add(taken.waited);
            deferredAt = 0;
        }
    }

    // The kernel mapping of a surface's memory for its copy; the caller releases it.
    static IOMemoryMap *sourceMapping(IOMemoryDescriptor *memory, bool &owned)
    {
        owned = true;
        return memory->map(kIOMapAnywhere | kIOMapReadOnly);
    }

    // The block-linear pass into target: kSwizzleTiled, kSwizzleByte or kSwizzleFailed; native_legacy_block_layout (the
    // caller's check) bounds it.
    enum Swizzled {
        kSwizzleFailed,
        kSwizzleByte,
        kSwizzleTiled
    };

    // A tiled source is in window A's own blocks (kickOnce's asScanned): copied as it is.
    static Swizzled swizzle(IOMemoryMap *target, const void *source, uint64_t bytes, uint64_t stride, uint32_t width,
                            uint32_t height, bool tiled)
    {
        void *out = reinterpret_cast<void *>(target->getVirtualAddress());
        if (tiled) {
            if (width != kNativeBlockWidth || height != kNativeBlockHeight || target->getLength() < kNativeBlockBytes ||
                bytes < kNativeBlockBytes)
                return kSwizzleFailed;
            memcpy(out, source, kNativeBlockBytes);
            return kSwizzleTiled;
        }
        return native_legacy_block_copy(out, target->getLength(), source, bytes, stride, width, height)
                   ? kSwizzleByte
                   : kSwizzleFailed;
    }

    void noteSwizzled(Swizzled how)
    {
        if (how == kSwizzleTiled) {
            ++stFast;
        } else if (how == kSwizzleByte) {
            ++stByte;
        }
    }

    // The stats' read probe: every visible row of the source loaded once, 16 bytes at a time. Ticks; 0 when
    // the source is not 16-byte aligned.
    static uint64_t readPass(const void *source, uint64_t stride, uint32_t width, uint32_t height)
    {
        if ((reinterpret_cast<uintptr_t>(source) | stride) & 15)
            return 0;
        uint64_t sum = 0;
        const uint64_t start = counterNow();
        for (uint32_t y = 0; y < height; ++y) {
            const uint64_t *row = reinterpret_cast<const uint64_t *>(reinterpret_cast<uintptr_t>(source) +
                                                                     static_cast<uint64_t>(y) * stride);
            for (uint32_t x = 0; x < width / 2; x += 2)
                sum ^= row[x] + row[x + 1];
        }
        __asm__ volatile("" ::"r"(sum));  // keeps the loads
        const uint64_t end = counterNow();
        return end - start;
    }

    // After startHardwareRotation, which it needs.
    void startCopyLoop()
    {
        constexpr unsigned mode = 2;
        if (!primary || !hwRotate)
            return;
        copyLoop = IOWorkLoop::workLoop();
        copySource =
            copyLoop ? IOInterruptEventSource::interruptEventSource(this, &NativeLegacyDisplay::copyFired) : nullptr;
        if (!copySource || copyLoop->addEventSource(copySource) != kIOReturnSuccess)
            panic("SwitchDisplay: cannot start the copy thread");
        asyncCopy = mode;
        startSourceReady();
    }

    // Stop and free, outside the gate: no copy may run once the scanout and kept mappings go. Removing the source closes the copy
    // loop's gate, so it waits for a running copyWork, and with it for its copyFinished (which takes the family's gate, free here).
    void stopCopyLoop()
    {
        if (!copyLoop)
            return;
        IOWorkLoop *loop = getWorkLoop();
        if (!loop) {
            panic("SwitchDisplay: the display's work loop is gone before its copy thread "
                  "stopped");
        }
        loop->runAction(
            [](OSObject *owner, void *, void *, void *, void *) -> IOReturn {
                __atomic_store_n(&static_cast<NativeLegacyDisplay *>(owner)->copyStopping, true, __ATOMIC_RELEASE);
                return kIOReturnSuccess;
            },
            this);
        stopSourceReady();
        copyLoop->removeEventSource(copySource);
        OSSafeReleaseNULL(copySource);
        OSSafeReleaseNULL(copyLoop);
        // A hand-off the copy thread never took: drop its mapping (its request goes with the family's own stop).
        loop->runAction(
            [](OSObject *owner, void *, void *, void *, void *) -> IOReturn {
                NativeLegacyDisplay *self = static_cast<NativeLegacyDisplay *>(owner);
                if (self->copyQueued && self->job.ownedSource)
                    OSSafeReleaseNULL(self->job.source);
                self->copyQueued = false;
                self->sourceWaitQueued = false;
                self->stopPlaneConversion();
                self->stopVicCopy();
                self->cancelQueuedPlaneFence();
                self->copyPlaneJoined = self->copyPlaneReady = self->copyPixelsReady = false;
                self->releaseSourceWait();
                if (self->sourceWait) {
                    IODelete(self->sourceWait, SourceWait, 1);
                    self->sourceWait = nullptr;
                }
                self->asyncCopy = 0;
                return kIOReturnSuccess;
            },
            this);
    }

    // Under the gate, a validated surface swap mapped and bounded: hand it to the copy thread.
    void startCopy(const OriginalLegacySwapRequest *request, IOMemoryMap *source, bool ownedSource, uint64_t stride,
                   uint64_t allocation, uint32_t width, uint32_t height, bool tiled, uint64_t kickAt)
    {
        if (!holdCopySource(planeShownUi)) {
            if (ownedSource)
                source->release();
            native_legacy_transfer_observed(kIOReturnNoMemory, request, 0);
            return;
        }
        job = {};
        job.request = request;
        job.tiled = tiled;
        job.source = source;
        job.target = drawSurface();
        if (doubleBuffer)
            ring.claim(ring.target());  // the copy thread writes it until copyFinished
        job.ownedSource = ownedSource;
        job.stride = stride;
        job.allocation = allocation;
        job.width = width;
        job.height = height;
        if (stats && stReadDue) {
            stReadDue = false;
            job.read = true;
        }
        transferred = true;
        copyQueued = true;
        noteTaken(request, kickAt);
        if (asyncCopy == 2 && !latchRetire && sourceWait->read.lease && retireSwap(request, true))
            job.retired = true;
        if (!beginCopiedPlaneFence()) {
            job.status = kIOReturnNotReady;
            copyFinished();
            return;
        }
        copySource->interruptOccurred(nullptr, nullptr, 0);
    }

    static void copyFired(OSObject *owner, IOInterruptEventSource *, int)
    {
        static_cast<NativeLegacyDisplay *>(owner)->copyWork();
    }

    // The copy thread, outside the family's gate: the part of a swap that reads the surface. job is not touched after copyFinished.
    void copyWork()
    {
        if (vicCopyWork())
            return;
        if (planeFenceQueued) {
            planeFenceWork();
            if (copyQueued && !__atomic_load_n(&copyStopping, __ATOMIC_ACQUIRE))
                copySource->interruptOccurred(nullptr, nullptr, 0);
            return;
        }
        if (planeConversionQueued) {
            planeConversionWork();
            return;
        }
        if (sourceWaitQueued) {
            sourceWaitWork();
            return;
        }
        const void *source = reinterpret_cast<const void *>(job.source->getVirtualAddress());
        job.status = kIOReturnSuccess;
        if (__atomic_load_n(&copyStopping, __ATOMIC_ACQUIRE)) {
            job.status = kIOReturnAborted;
        } else if (const int error = readyCopySource(source, job.allocation, job.gpu, job.cache)) {
            job.status = error == TEGRA_SCANOUT_SOURCE_LOST ? kIOReturnAborted : kIOReturnNotReady;
        } else {
            if (!native_legacy_prepare_scanout()) {
                job.status = kIOReturnNotReady;
            } else {
                if (job.read)
                    job.readTicks = readPass(source, job.stride, job.width, job.height);
                const uint64_t start = counterNow();
                job.swizzled =
                    swizzle(job.target, source, job.source->getLength(), job.stride, job.width, job.height, job.tiled);
                if (job.swizzled == kSwizzleFailed)
                    job.status = kIOReturnBadArgument;
                __asm__ volatile("dsb sy" ::: "memory");
                const uint64_t end = counterNow();
                job.copy = end - start;
                if (stats)
                    job.attribute = mairAttribute(source);
            }
        }
        getWorkLoop()->runAction(
            [](OSObject *owner, void *, void *, void *, void *) -> IOReturn {
                static_cast<NativeLegacyDisplay *>(owner)->copyFinished();
                return kIOReturnSuccess;
            },
            this);
    }

    // Under the gate, the copy thread done with job.
    void copyFinished()
    {
        copyQueued = false;
        releaseSourceWait();
        if (job.ownedSource)
            OSSafeReleaseNULL(job.source);
        if (job.status == kIOReturnSuccess) {
            native_legacy_copy_timing_observed(job.copy, static_cast<uint64_t>(job.width) * job.height * 4);
            noteCopy(job.copy);
        }
        if (stats) {
            if (job.status != kIOReturnAborted) {
                stGpu.add(job.gpu);
                stCache.add(job.cache);
            }
            if (job.read)
                stRead = job.readTicks;
            if (job.status == kIOReturnSuccess) {
                stCopy.add(job.copy);
                noteSwizzled(static_cast<Swizzled>(job.swizzled));
            }
            if (stSourceAttr == 0x100 && job.attribute)
                stSourceAttr = job.attribute;
        }
        if (copiedPixelsFinished())
            return;
        completeCopiedJob();
    }

#include "SwitchCopiedPlanes.h"

    void completeCopiedJob()
    {
        if (__atomic_load_n(&copyStopping, __ATOMIC_ACQUIRE))
            return;
        if (job.status == kIOReturnAborted) {
            dropLostFrame(job.request, kNoBuffer);
            return;
        }
        if (job.status != kIOReturnSuccess) {
            // As a failed copy under the gate: the swap stays queued for the next kick. One retired at the start is gone, so the
            // next may go at once (a refused one is not tried again here, which could loop). Its surface holds nothing to show.
            if (doubleBuffer)
                ring.release();
            native_legacy_transfer_observed(job.status, job.request, 0);
            transferred = false;
            if (!job.retired)
                untake(job.request);
            if (job.retired)
                kickSwapQueue();
            return;
        }
        if (hwSuspended) {
            presentDeferred = true;
            return;
        }  // the controller is unclocked: finishResume presents it
        presentCopied();
    }

    // Under the gate: the copied frame to the screen, then its swap retired (or, retired at the start, the next one released).
    void presentCopied()
    {
        const OriginalLegacySwapRequest *request = job.request;
        presentBlock();
        native_legacy_transfer_observed(kIOReturnSuccess, request, static_cast<uint64_t>(job.width) * job.height * 4);
        // Retired at the start, or gone from the queue while the gate was open (the family discarded it): nothing left to retire.
        if (job.retired || pendingSwapRequest() != request) {
            transferred = false;
            kickSwapQueue();
            return;
        }
        finishTransfer(request);
    }

    // The GPU client reported its copy (status 0) or lost it. Success retires the swap like a finished copy; anything else uses the CPU copy.
    void finishGpuSwap(int status)
    {
        if (finishVicCopy(status))
            return;
        if (gpuScanout && planeFencePending) {
            finishPlaneFence(status);
            return;
        }
        if (gpuScanout) {
            finishGpuCopy(status);
            return;
        }
        const OriginalLegacySwapRequest *request = gpuRequest;
        if (!request)
            return;
        gpuRequest = nullptr;
        if (gpuRetired) {
            // The swap retired when the client took it; its copy ending only releases the next frame. A failed copy loses this frame
            // (the next one takes the CPU copy).
            gpuRetired = false;
            if (status == 0) {
                native_legacy_transfer_observed(kIOReturnSuccess, request, gpuLength);
            } else {
                native_legacy_early_failures = native_legacy_early_failures + 1;
                native_legacy_gpu_fallback_observed(status);
                gpuBypass = true;
            }
            transferred = false;
            kickSwapQueue();
            return;
        }
        if (status == 0) {
            native_legacy_transfer_observed(kIOReturnSuccess, request, gpuLength);
            finishTransfer(request, true);
            return;
        }
        native_legacy_gpu_fallback_observed(status);
        transferred = false;
        gpuBypass = true;
        kickSwapQueue();
    }

    // After startHardwareRotation. The copy goes to the buffer window A does not scan, so it needs the second
    // buffer; the GPU driver may still decline any frame (the CPU copies that one).
    void startGpuCopy()
    {
        if (!primary)
            return;
        if (!hwRotate || !doubleBuffer || !tegra_gm20b_scanout_copy) {
            IOLog("SwitchDisplay: no GPU copy: it needs the double-buffered scanout, the hardware rotation and "
                  "TegraGM20B's tegra_gm20b_scanout_copy%s\n",
                  tegra_gm20b_scanout_copy ? "" : " (not linked)");
            return;
        }
        IOWorkLoop *loop = getWorkLoop();
        gpuDoneSource = IOInterruptEventSource::interruptEventSource(this, &NativeLegacyDisplay::gpuDoneFired);
        gpuDeadline = IOTimerEventSource::timerEventSource(this, &NativeLegacyDisplay::gpuDeadlineFired);
        if (!loop || !gpuDoneSource || !gpuDeadline || loop->addEventSource(gpuDoneSource) != kIOReturnSuccess ||
            loop->addEventSource(gpuDeadline) != kIOReturnSuccess)
            panic("SwitchDisplay: cannot add the GPU copy's completion sources");
        gpuCopy = true;
    }

    // Outside the gate. The GPU would still write a buffer that goes away with the rotation, and call back: no stop with a copy out.
    void stopGpuCopy()
    {
        if (__atomic_load_n(&gpuScanout, __ATOMIC_ACQUIRE))
            panic("SwitchDisplay: stopped with GPU frame copy %llu outstanding", gpuSequence);
        IOWorkLoop *loop = getWorkLoop();
        IOEventSource *sources[] = {gpuDoneSource, gpuDeadline};
        for (IOEventSource *source : sources) {
            if (source && loop)
                loop->removeEventSource(source);
        }
        if (gpuDeadline)
            gpuDeadline->cancelTimeout();
        OSSafeReleaseNULL(gpuDoneSource);
        OSSafeReleaseNULL(gpuDeadline);
        gpuCopy = false;
    }

    // Under the gate, in the kick, with a surface free: offer the swap's surface to the GPU for the buffer window A neither scans nor
    // may take (ring.target(), the one drawSurface() names; a flip may be out for another). Declined, the caller copies it on the CPU.
    // Accepted, the swap retires now (the compositor renders its next frame, into its other surface, while the GPU copies), and
    // transferred holds the next swap until the completion has flipped window A.
    bool offerGpuCopy(const OriginalLegacySwapRequest *request, IOMemoryDescriptor *memory, uint64_t stride,
                      uint64_t allocation, uint32_t width, uint32_t height, int blockLog2, uint64_t kickAt)
    {
        if (width != kNativeBlockWidth || height != kNativeBlockHeight || !doubleBuffer)
            return false;
        const int target = ring.target();
        if (target == kNoBuffer)
            return false;
        TegraScanoutCopy copy = {};
        copy.version = TEGRA_SCANOUT_COPY_VERSION;
        copy.width = width;
        copy.height = height;
        copy.sequence = gpuSequence + 1;
        copy.display = this;
        copy.source = memory;
        copy.sourceStride = stride;
        copy.sourceBytes = allocation;
        copy.target = surfaceAddress(target);
        copy.targetBytes = kNativeBlockBytes;
        copy.targetPitch = static_cast<uint32_t>(kNativeBlockStride);
        copy.blockHeightLog2 = 4;  // kNativeBlockGobs, the DC's BLOCK_HEIGHT(4)
        copy.flags = blockLog2 >= 0 ? TEGRA_SCANOUT_COPY_SOURCE_BLOCK : 0;
        copy.sourceBlockHeightLog2 = blockLog2 >= 0 ? static_cast<uint32_t>(blockLog2) : 0;
        copy.alsoWritten = planeNextAlsoWritten();
        switch_gpu_backing_token planeTokens[kPlaneDependencies] = {};
        planeNextDependencies(copy, planeTokens);
        memory->retain();
        const int offered = offerWithCompletionFlip(copy, target);
        if (offered) {
            memory->release();
            releaseIdleDirectReads();
            if (offered == TEGRA_SCANOUT_SOURCE_LOST) {
                gpuSequence = copy.sequence;
                noteTaken(request, kickAt);
                planeNextSet = false;
                native_legacy_transfer_observed(kIOReturnAborted, request, 0);
                dropLostFrame(request, kNoBuffer);
                return true;
            }
            if (offered == kIOReturnCannotLock)
                return deferSource(request, planeShownUi);
            if (stats)
                ++stGpuDeclined;
            return false;
        }
        gpuSequence = copy.sequence;
        gpuSource = memory;
        gpuOfferedAt = mach_absolute_time();
        ring.claim(target);  // the GPU writes it until finishGpuCopy
        __atomic_store_n(&gpuScanout, true, __ATOMIC_RELEASE);
        gpuDeadlines = 0;
        gpuDeadline->setTimeoutMS(kGpuCopyDeadlineMS);
        native_legacy_swap_stage(1);
        gpuRequest = request;
        gpuLength = static_cast<uint64_t>(width) * height * 4;
        transferred = true;
        noteTaken(request, kickAt);
        if (!latchRetire && retireSwap(request, true)) {
            gpuRetired = true;
            native_legacy_early_retires = native_legacy_early_retires + 1;
        }
        return true;
    }

    static void gpuDoneFired(OSObject *owner, IOInterruptEventSource *, int)
    {
        NativeLegacyDisplay *self = static_cast<NativeLegacyDisplay *>(owner);
        if (!__atomic_exchange_n(&self->gpuDonePending, false, __ATOMIC_ACQUIRE))
            return;
        self->finishGpuSwap(__atomic_load_n(&self->gpuStatus, __ATOMIC_RELAXED));
    }

    // A completion signalled but not handled yet (the gate was busy) only moves the deadline, as does an offer the GPU driver
    // still holds: its own deadline names what the copy waits for and recovers or panics (TegraScanoutCopy.h). The
    // completion is looked for again after the driver's answer, which may come between its release and the signal.
    static void gpuDeadlineFired(OSObject *owner, IOTimerEventSource *)
    {
        NativeLegacyDisplay *self = static_cast<NativeLegacyDisplay *>(owner);
        if (!self->gpuScanout)
            return;
        if (self->vicCopyLate())
            return;
        const bool held = tegra_gm20b_scanout_outstanding(self) != 0 || self->planeFencePending;
        if (held)
            self->gpuDeadlines += 1;
        if (__atomic_load_n(&self->gpuDonePending, __ATOMIC_SEQ_CST) ||
            (held && self->gpuDeadlines < kGpuCopyDeadlines)) {
            self->gpuDeadline->setTimeoutMS(kGpuCopyDeadlineMS);
            return;
        }
        const unsigned waited = (held ? self->gpuDeadlines : self->gpuDeadlines + 1) * kGpuCopyDeadlineMS;
        if (self->planeFencePending) {
            panic("SwitchDisplay: GPU present %llu (overlay fence, take %llu) not completed in %u ms: %s",
                  self->gpuSequence, planeFencing.seq, waited,
                  held ? "the GPU driver still holds it" : "the GPU driver released it with no completion");
        }
        if (self->gpuDirect != kNoBuffer) {
            panic("SwitchDisplay: GPU present %llu (fence only, direct slot %d) not completed in %u ms: %s",
                  self->gpuSequence, self->gpuDirect - kSlot0, waited,
                  held ? "the GPU driver still holds it" : "the GPU driver released it with no completion");
        }
        panic("SwitchDisplay: GPU frame copy %llu into %#x not completed in %u ms: %s", self->gpuSequence,
              self->surfaceAddress(self->ring.drawing), waited,
              held ? "the GPU driver still holds it" : "the GPU driver released it with no completion");
    }

    // Under the gate: the GPU frame copy is done (status 0) or failed. Done, window A flips to it (after SC7: at the resume) and the swap
    // retires, or, retired at the offer, the next one may go. Failed, nothing is flipped: a swap still queued is copied on the CPU at
    // once, one retired at the offer is lost (the next frame replaces it), and the next kick uses the CPU copy either way. A source
    // whose rendering was lost (dropLostFrame) is neither shown nor copied.
    void finishGpuCopy(int status)
    {
        const OriginalLegacySwapRequest *request = gpuRequest;
        gpuDeadline->cancelTimeout();
        __atomic_store_n(&gpuScanout, false, __ATOMIC_RELEASE);
        gpuRequest = nullptr;
        OSSafeReleaseNULL(gpuSource);
        if (stats) {
            stGpuCopy.add(mach_absolute_time() - gpuOfferedAt);
            if (!status)
                ++stGpuCopies;
        }
        const int directEntry = gpuDirect;
        gpuDirect = kNoBuffer;
        if (status == TEGRA_SCANOUT_SOURCE_LOST) {
            dropLostFrame(request, directEntry);
            return;
        }
        if (status && directEntry != kNoBuffer) {
            releaseIdleDirectReads();  // the rendering never finished on the GPU: copy it on the CPU instead
            native_legacy_gpu_fallback_observed(status);
            untake(request);
            gpuBypass = true;
            transferred = false;
            kickSwapQueue();
            return;
        }
        if (status) {
            native_legacy_gpu_fallback_observed(status);
            ring.release();  // the target may hold a partial frame: never flipped to
            gpuBypass = true;
            if (gpuRetired) {
                gpuRetired = false;
                native_legacy_early_failures = native_legacy_early_failures + 1;
            }
            transferred = false;
            kickSwapQueue();
            return;
        }
        if (SWITCH_FRAME_TIMELINE) {
            taken.tl.doneUs =
                taken.submit ? usFrom(ticksNs(taken.submit), ticksNs(__atomic_load_n(&gpuDoneAt, __ATOMIC_RELAXED)))
                             : kNotSeen;
        }
        gpuPresented = request;
        gpuPresentedEntry = directEntry;
        if (hwSuspended) {
            gpuPresentDeferred = true;
            return;
        }  // the controller is unclocked: finishResume presents it
        presentGpuCopy();
    }

    // A frame whose rendering the GPU driver killed (TEGRA_SCANOUT_SOURCE_LOST): the copy's target is never flipped to, and the
    // swap retires unshown, as a frame the console holds back does; the screen keeps the frame before it.
    // NOLINTNEXTLINE(misc-no-recursion): kickSwapQueue's kicking flag stops re-entry; a nested kick only loops
    void dropLostFrame(const OriginalLegacySwapRequest *request, int directEntry)
    {
        ++gpuLostFrames;
        IOLog("SwitchDisplay: frame %llu not shown: its rendering was lost on the GPU (%llu so far)\n", gpuSequence,
              gpuLostFrames);
        if (directEntry == kNoBuffer)
            ring.release();
        releaseIdleDirectReads();
        if (latchRetire && request) {
            retireUnshown(seqOf(request));
        } else if (!latchRetire && !gpuRetired && request && pendingSwapRequest() == request) {
            retireSwap(request, true);
        }
        gpuRetired = false;
        transferred = false;
        kickSwapQueue();
    }

    void presentGpuCopy()
    {
        const OriginalLegacySwapRequest *request = gpuPresented;
        const int directEntry = gpuPresentedEntry;
        gpuPresented = nullptr;
        gpuPresentedEntry = kNoBuffer;
        if (directEntry != kNoBuffer) {
            flipRequest = request;
            presentEntry(directEntry);  // the compositor's rendering of it is done
        } else {
            presentBlock();  // the GPU's completion already put the frame in DRAM
        }
        native_legacy_transfer_observed(kIOReturnSuccess, request, gpuLength);
        if (latchRetire) {  // it retires when it is seen on screen (checkFlip)
            transferred = false;
            kickSwapQueue();
            return;
        }
        // Retired at the offer, or gone from the queue meanwhile (the family discarded it): nothing left to retire.
        if (gpuRetired || !request || pendingSwapRequest() != request) {
            gpuRetired = false;
            transferred = false;
            kickSwapQueue();
            return;
        }
        finishTransfer(request, true);
    }

    // Hands the pending head to the unchanged family's completion. Leaves transferred alone: the callers decide when the next frame may go.
    bool retireSwap(const OriginalLegacySwapRequest *request, bool force)
    {
        if (!activateOnlyPendingSwap(request))
            return false;
        untake(request);
        const uint64_t submitted = stats ? submittedAt(request) : 0;
        removeValidated(request);
        IOMobileFramebufferLegacy::completeCurrentSwap();
        native_legacy_retirement_observed(this);
        if (stats) {
            const uint64_t now = mach_absolute_time();
            if (submitted && now > submitted)
                stRetire.add(now - submitted);
            lastRetire = now;
            ++stSwaps;
        }
        return true;
    }

    // Separately gated retirement after a real scanout store, shared by copied and blank frames.
    // NOLINTNEXTLINE(misc-no-recursion): kickSwapQueue's kicking flag stops re-entry; a nested kick only loops
    void finishTransfer(const OriginalLegacySwapRequest *request, bool force = false)
    {
        // Only after the real copy and store barrier: select the actual pending head and let the unchanged family release it and wake
        // waiters. This reports memory scanout completion, not a panel vblank or hardware fence.
        if (latchRetire) {  // retiring at the latch (direct scanout on): checkFlip retires it once it is seen on screen
            // r18-planes: a frame no flip carries (a blank fill of the pitch window before the first rotation, or the single
            // buffer) is on screen now.
            if (ring.pending == kNoBuffer && ring.held == kNoBuffer && !planeLatch.requested() && !planeHeldSet)
                retireThrough(seqOf(request), mach_absolute_time());
            transferred = false;
            kickSwapQueue();
            return;
        }
        if (!retireSwap(request, force))
            return;
        // The unchanged family has retired this generation and released its current reference. Permit the next independently validated
        // single pending frame. Keep the latch set while completion can reenter this method, and whenever retirement remains disabled.
        transferred = false;
        kickSwapQueue();
    }

    // --- Direct scanout (see the members) ------------------------------------------------------------------------------
    void take(const OriginalLegacySwapRequest *request)
    {
        for (const Taken &t : takenSwaps) {
            if (t.request == request)
                return;
        }
        for (Taken &t : takenSwaps) {
            if (!t.request) {
                t = {request, ++takeSeq};
                return;
            }
        }
        takenSwaps[0] = {request, ++takeSeq};  // more than eight in flight cannot happen (the family queues fewer)
    }

    void untake(const OriginalLegacySwapRequest *request)
    {
        for (Taken &t : takenSwaps) {
            if (t.request == request)
                t = {};
        }
    }

    uint64_t seqOf(const OriginalLegacySwapRequest *request) const
    {
        for (const Taken &t : takenSwaps) {
            if (request && t.request == request)
                return t.seq;
        }
        return 0;
    }

    void noteFrameInfoTaken(const OriginalLegacySwapRequest *request)
    {
        const auto *bytes = reinterpret_cast<const unsigned char *>(request);
        uint32_t id = 0;
        uint64_t requested = 0;
        memcpy(&id, bytes + swapId, sizeof(id));
        memcpy(&requested, bytes + swapRequestedPresentation, sizeof(requested));
        frameInfo->pending.take(seqOf(request), id, requested);
        frameInfo->tickets.take(request, seqOf(request));
    }

    // Under the gate (the family's frameInfo_notify takes it again, recursively): each frame-info client's entry for the swap
    // taken as seq, on screen from shownAt (mach_absolute_time).
    void frameShown(uint64_t seq, uint64_t shownAt)
    {
        const uint64_t nowContinuous = mach_continuous_time();
        const uint64_t nowAbsolute = mach_absolute_time();
        const uint64_t continuous = switch_frame_info::continuousTime(shownAt, nowAbsolute, nowContinuous);
#if SWITCH_FRAME_TIMELINE
        bool first = false;
        const auto ticket = frameInfo->tickets.shown(seq, continuous, vsyncInterval / 2, &first);
        if (first)
            KDBG(SWITCH_KDBG_CODE(SWITCH_KDBG_FRAME_TICKET), ticket.id, ticket.swapId, ticket.take, ticket.gpu);
#endif
        switch_frame_info::Frame frame;
        if (frameInfo->pending.shown(seq, continuous, frame) && frameInfo->clients.any())
            native_legacy_frame_info_notify(this, &frame);
    }

    // The first accepted swap in the family's queue not yet taken (latchRetire: older ones wait on screen for their latch).
    const OriginalLegacySwapRequest *firstUntaken() const
    {
        for (const OriginalLegacySwapRequest *r = pendingSwapRequest(); r; r = queuedAfter(r)) {
            if (!seqOf(r))
                return r;
        }
        return nullptr;
    }

#include "SwitchRetireDisplay.h"
#include "SwitchSuspendRetire.h"
#include "SwitchTimedPresentDisplay.h"
#include "SwitchConsoleReleaseDisplay.h"

    // A swap window A can scan in place: one full-frame BGRA surface (validateSwap's single layer), its rows a multiple of 64
    // bytes, at most one slot. The portrait-native layout only: window A then scans rows unrotated, as hekate's pitch window does.
    static bool directCandidate(OSObject *surface)
    {
        if (!SWITCH_PORTRAIT_NATIVE)
            return false;
        if (!surface || !surface->metaCast("IOSurface"))
            return false;
        const uint64_t stride = native_legacy_surface_stride(surface);
        const uint64_t allocation = native_legacy_surface_allocation(surface);
        return native_legacy_surface_width(surface) == kNativeBlockWidth &&
               native_legacy_surface_height(surface) == kNativeBlockHeight &&
               nativeBgra(native_legacy_surface_format(surface), native_legacy_surface_planes(surface)) &&
               ((stride % 64) == 0u) && stride >= static_cast<uint64_t>(kNativeBlockWidth) * 4 && stride <= 0xffff &&
               allocation <= kSlotSpan && allocation >= stride * kNativeBlockHeight;
    }

#include "SwitchDirectSlots.h"
#include "SwitchDirectVisibleDisplay.h"
#include "SwitchCompletionFlipDisplay.h"
#include "SwitchCompletionFlipFilter.h"

    // The table entries just written, to memory, then the SMMU's caches for the display's ASID.
    void cleanDomain()
    {
        uintptr_t low = 0;
        uintptr_t high = 0;
        if (probeDomain.takeDirty(&low, &high))
            switch_dcache_clean_invalidate(reinterpret_cast<const void *>(low), high - low);
        tegra_smmu_flush(TEGRA_SMMU_ASID_DISPLAY);
    }

    // Under the gate, in the kick: the swap's surface into its slot, and a fence-only GPU present for it; its completion flips
    // window A to the slot (finishGpuCopy). False: the copy path takes it.
    bool offerDirect(const OriginalLegacySwapRequest *request, OSObject *surface, IOMemoryDescriptor *memory,
                     uint64_t stride, uint64_t allocation, uint32_t width, uint32_t height, bool tiled, uint64_t kickAt)
    {
        if (!gpuCopy || !gpuDoneSource)
            return false;
        const int entry = directSlotFor(surface, memory, stride, allocation, tiled);
        if (entry == kNoBuffer)
            return false;
        if (presentDirectVisible(request, entry, width, height, kickAt))
            return true;
        TegraScanoutCopy copy = {};
        copy.version = TEGRA_SCANOUT_COPY_VERSION;
        copy.width = width;
        copy.height = height;
        copy.sequence = gpuSequence + 1;
        copy.display = this;
        copy.source = memory;
        copy.sourceStride = stride;
        copy.sourceBytes = allocation;
        copy.flags = TEGRA_SCANOUT_COPY_FENCE_ONLY | producerWatch(entry);
        copy.alsoWritten = planeNextAlsoWritten();
        switch_gpu_backing_token planeTokens[kPlaneDependencies] = {};
        planeNextDependencies(copy, planeTokens);
        memory->retain();
        const int offered = offerWithCompletionFlip(copy, entry);
        if (offered) {
            memory->release();
            releaseIdleDirectReads();
            if (offered == TEGRA_SCANOUT_SOURCE_LOST) {
                gpuSequence = copy.sequence;
                noteTaken(request, kickAt);
                planeNextSet = false;
                native_legacy_transfer_observed(kIOReturnAborted, request, 0);
                dropLostFrame(request, entry);
                return true;
            }
            if (offered == kIOReturnCannotLock)
                return deferSource(request, planeShownUi);
            if (stats)
                ++stGpuDeclined;
            return false;
        }
        gpuSequence = copy.sequence;
        gpuSource = memory;
        gpuOfferedAt = mach_absolute_time();
        __atomic_store_n(&gpuScanout, true, __ATOMIC_RELEASE);
        gpuDeadlines = 0;
        gpuDeadline->setTimeoutMS(kGpuCopyDeadlineMS);
        gpuRequest = request;
        gpuLength = static_cast<uint64_t>(width) * height * 4;
        gpuDirect = entry;
        transferred = true;
        noteTaken(request, kickAt);
        if (stats)
            ++stDirect;
        return true;
    }

    // The probe passed (its domain: the scan surfaces 1:1, the slot tables): keep it attached and scan swaps in place.
    bool startDirect()
    {
        if (!gpuCopy || !gpuDoneSource || !SWITCH_PORTRAIT_NATIVE)
            return false;
        probeDomain.unmap(kProbeSlot, static_cast<uint32_t>(kNativeBlockBytes / 4096));  // the probe's own slot mapping
        if (!probeDomain.reserve(kProbeSlot, kSlots * kSlotSpan)) {
            snprintf(probeLine, sizeof(probeLine), "no SMMU tables for %u slots", kSlots);
            return false;
        }
        cleanDomain();
        directAttached = directOn = latchRetire = true;
        return true;
    }

    // The display stops: the swgroup back to bypass, every slot given back.
    void stopDirect()
    {
        stopPlanes();
        if (directAttached || planesAttached)
            tegra_smmu_detach(TEGRA_SMMU_ASID_DISPLAY, TEGRA_SMMU_DC_ASID);
        for (unsigned k = 0; k < kSlots; ++k) {
            dropSlot(k);
            if (slots[k].read) {
                IODelete(slots[k].read, SwitchNativeScanout, 1);
                slots[k].read = nullptr;
            }
        }
        directOn = directAttached = planesAttached = false;
    }

    bool captureDisplayMemory(switch_display::TimingBank bank = switch_display::TimingBank::Active)
    {
        switch_display_memory_head timing = {};
        uint32_t width = 0, height = 0;
        getDisplayDimensions(&width, &height);
        // PLLD and the DSI shift divider program this sole built-in panel mode at 78 MHz.
        if (!dc || !switch_display::memoryTiming(dc, 78000000, timing, bank) ||
            !displayMemory.configure(timing, width, height, (kBlockScan & kScanColumn) != 0)) {
            displayMemory.invalidate();
            return false;
        }
        return true;
    }

#include "SwitchLandscapeState.h"
#include "SwitchLandscapeUi.h"
#include "SwitchDisplayBandwidthDisplay.h"
#include "NativePlanesDisplay.h"
#include "SwitchUnderflowDisplay.h"
#include "SwitchLandscapeDisplay.h"
#include "SwitchNativePlaneDisplay.h"
#include "SwitchLandscapeSelection.h"
#include "SwitchPlaneConversionDisplay.h"
#include "SwitchPlaneCompositeDisplay.h"
#include "SwitchScanoutSetup.h"
#include "NativePlaneFenceDisplay.h"
#include "SwitchMediaPlaneFence.h"
#include "SwitchMediaCompletionFlipDisplay.h"
#include "SwitchPlaneFenceAdmission.h"
#include "NativePlanesTotals.h"
#include "SwitchPlaneSurfaceUseDisplay.h"
#include "SwitchVICScanoutDisplay.h"
#include "NativeColumnScanDisplay.h"
#include "SwitchDcHost1xClassProbe.h"
#include "DisplayWindowProbeDisplay.h"
#include "SwitchLandscapeRunDisplay.h"
#include "SwitchPaletteDisplay.h"
#include "SwitchPlaneAdmissionDrop.h"

    IOReturn collectCompletionReports(uint32_t, void *, uint32_t *count, uint32_t) override
    {
        // The original completion helper passes null optional outputs and
        // ignores this IOReturn. GOP exposes no hardware completion telemetry.
        if (count)
            *count = 0;
        return kIOReturnUnsupported;
    }

    bool validateSwap(const OriginalLegacySwapRequest *request) override
    {
        // Qualified original request fields are packed; use memcpy for each access. r18-planes: every swap Core Animation
        // can build from the limits this display publishes is accepted (NativePlanes.h: a refused swap is sent again every
        // frame, the r6h freezes): the UI as one full-frame, untransformed BGRA surface with real original backing memory
        // (the rule since r6), plus one detached layer window B shows exactly. Updated layers only: the family fills in the
        // ones a swap does not update after this (0xa60), and the kick reads those.
        __atomic_store_n(&gLastSwapAbs, mach_absolute_time(), __ATOMIC_RELAXED);
        const auto *bytes = reinterpret_cast<const unsigned char *>(request);
        const auto word = [&](unsigned offset) {
            uint32_t value = 0;
            memcpy(&value, bytes + offset, 4);
            return value;
        };
        OSObject *surface = nullptr;
        unsigned reason = 1;
        bool accepted = false;
        if (logicalPM && primary && bootMapping && request) {
            OSObject *surfaces[native_planes::kLayers] = {};
            memcpy(surfaces, bytes + swapSurfaces, sizeof(surfaces));
            uint32_t width = 0;
            uint32_t height = 0;
            getDisplayDimensions(&width, &height);
            const uint32_t updated = word(swapUpdated);
            const uint32_t background = word(swapBackground);
            bool options = true;
            for (unsigned offset = swapOptions; offset <= swapOptions + 8; offset += 4)
                options = options && word(offset) == 0;
            bool hasSurface[native_planes::kLayers] = {};
            bool any = false;
            bool objects = true;
            native_planes::Layer layers[native_planes::kLayers];
            for (unsigned i = 0; i < native_planes::kLayers; ++i) {
                if (!(updated & (1u << i)) || !surfaces[i])
                    continue;
                if (!surfaces[i]->metaCast("IOSurface"))
                    objects = false;
                if (!surface)
                    surface = surfaces[i];
                hasSurface[i] = any = true;
                if (objects)
                    layers[i] = planeLayerOf(bytes, i, surfaces[i]);
            }
            if (!width || !height || !options || word(swapSize) != width || word(swapSize + 4) != height) {
                reason = 3;
            } else if (!objects) {
                reason = 4;
            } else if (!any) {
                // No surface in any slot and every updated layer disabled over black: present a blank screen.
                reason = native_legacy_blank_state(updated, word(swapReady), background) ? 5 : 2;
                accepted = reason == 5;
            } else {
                reason = native_planes::checkUpdated(updated, background, hasSurface, layers, width, height);
                planeNoteRefused(reason, updated, hasSurface, layers, width, height);
                // The UI's own memory: its rows within the descriptor (a surface larger than the display, rows or lines
                // padded to the GPU's alignment, is accepted: only the visible rectangle is read).
                for (unsigned i = 0; !reason && i < native_planes::kLayers; ++i) {
                    if (!hasSurface[i] || !native_planes::isUi(layers[i], width, height))
                        continue;
                    const IOMemoryDescriptor *memory = native_legacy_surface_descriptor(surfaces[i]);
                    if (!memory || !memory->metaCast("IOMemoryDescriptor") ||
                        !native_legacy_surface_bounds(width, height, native_legacy_surface_stride(surfaces[i]),
                                                      native_legacy_surface_allocation(surfaces[i]),
                                                      memory->getLength())) {
                        reason = 4;
                        surface = surfaces[i];
                    }
                }
                if (!reason)
                    admitPlaneBandwidth(hasSurface, layers, width, height);
                accepted = (reason == 0u);
                if (reason == 4 || reason == native_planes::kFormat || reason == native_planes::kAlignment) {
                    native_legacy_surface_rejected(
                        native_legacy_surface_width(surface), native_legacy_surface_height(surface),
                        native_legacy_surface_format(surface), native_legacy_surface_planes(surface),
                        native_legacy_surface_stride(surface), native_legacy_surface_allocation(surface),
                        native_legacy_surface_descriptor(surface)
                            ? native_legacy_surface_descriptor(surface)->getLength()
                            : 0);
                }
            }
        }
        if (accepted) {
            noteSubmitted(request);
        } else {
            removeValidated(request);
        }
        if (!accepted && stats) {
            __atomic_fetch_add(&stRejected, 1u, __ATOMIC_RELAXED);
            __atomic_store_n(&stRejectReason, reason, __ATOMIC_RELAXED);
        }
        native_legacy_swap_validation_observed(accepted, reason, request, surface);
        return accepted;
    }

    // An accepted swap: remembered for the kick, and for the stats its submission time, the interval since the previous one and the
    // compositor's own time since the last retire (0 when the previous swap was still on its way).
    void noteSubmitted(const OriginalLegacySwapRequest *request)
    {
        const uint64_t now = mach_absolute_time();
        if (!addValidated(request, now))
            return;
        if (SWITCH_FRAME_TIMELINE)
            noteSwapTimeline(request);
        if (!stats)
            return;
        if (lastSubmit) {
            stInterval.add(now - lastSubmit);
            stClient.add(lastRetire > lastSubmit && now > lastRetire ? now - lastRetire : 0);
        }
        lastSubmit = now;
    }

    // Debug kernels: the newly accepted swap's timeline, on the swapping thread.
    void noteSwapTimeline(const OriginalLegacySwapRequest *request)
    {
        for (unsigned i = 0; i < kValidated; ++i) {
            if (validated[i] != request)
                continue;
            const uint64_t submitNs = ticksNs(validatedAt[i]);
            SwitchFrameGpu gpu;
            tegra_gm20b_frame_take(&gpu);
            int base = 0;
            int sched = 0;
            int mode = 0;
            int qos = 0;
            switch_thread_sched(current_thread(), &base, &sched, &mode, &qos);
            FrameTimeline &tl = validatedTl[i];
            tl.vsyncUs = kNotSeen;
            tl.doneUs = kNotSeen;
            tl.firstUs = gpu.firstSubmit ? usFrom(submitNs, gpu.firstSubmit) : kNotSeen;
            tl.lastUs = gpu.lastSubmit ? usFrom(submitNs, gpu.lastSubmit) : kNotSeen;
            tl.execUs = us32(gpu.execNs);
            tl.importUs = us32(gpu.importNs);
            tl.bindUs = us32(gpu.bindNs);
            tl.allocUs = us32(gpu.allocNs);
            tl.cleanUs = us32(gpu.cleanNs);
            tl.cleanKiB = static_cast<uint32_t>(gpu.cleanBytes >> 10);
            tl.submits = sat16(gpu.submits);
            tl.others = sat16(gpu.others);
            tl.imports = sat16(gpu.imports);
            tl.binds = sat16(gpu.binds);
            tl.allocs = sat16(gpu.allocs);
            tl.frees = sat16(gpu.frees);
            tl.mhzVsync = sat16(__atomic_load_n(&vsyncSentKhz, __ATOMIC_RELAXED) / 1000);
            tl.mhzSwap = sat16(switch_perf_cpu_khz() / 1000);
            tl.basePri = static_cast<int8_t>(base);
            tl.schedPri = static_cast<int8_t>(sched);
            tl.mode = static_cast<int8_t>(mode);
            tl.qos = static_cast<int8_t>(qos);
#if SWITCH_FRAME_TIMELINE
            const auto *ticket = frameInfo->tickets.find(request);
            KDBG(SWITCH_KDBG_CODE(SWITCH_KDBG_SWAP), ticket ? ticket->id : 0, ticket ? ticket->swapId : 0,
                 ticket ? ticket->requested : 0, frameEnds);
#endif
            switch_sched_render_swapped();
            return;
        }
    }

    // The original slot-188 caller passes a uint32_t output and consumes an
    // IOReturn. Its implementation assumes an AppleARMIODevice provider.
    // Query our actual generic provider instead; GOP supplies no dot pitch.
    IOReturn getDotPitch(uint32_t *pitch) override
    {
        if (!pitch)
            return kIOReturnBadArgument;
        *pitch = 0;
        const IOService *provider = getProvider();
        if (!provider)
            return kIOReturnNotReady;
        const OSObject *property = provider->getProperty("dot-pitch");
        if (!property)
            return kIOReturnNotFound;
        return switch_osdata_value(property, pitch) ? kIOReturnSuccess : kIOReturnBadArgument;
    }

    IOReturn supportedFrameInfo(uint32_t index, uint32_t *count, char *key, uint32_t *flags) override
    {
        if (!count || !key || !flags)
            return kIOReturnBadArgument;
        return switch_frame_info::supported(index, count, key, flags) == switch_frame_info::kOk ? kIOReturnSuccess
                                                                                                : kIOReturnBadArgument;
    }

    IOReturn changeFrameInfo(uint32_t type, void *client, uint32_t mask) override
    {
        if (type != switch_frame_info::kNotificationType) {
            switch_refusal_k(switch_refusal_key_with(switch_refusal_key("display", "frame-info-type"), type), "display",
                             "frame-info-type", "ChangeFrameInfo for notification type %u: only type %u is frame info",
                             type, switch_frame_info::kNotificationType);
            return kIOReturnBadArgument;
        }
        if (frameInfo->clients.change(client, mask) == switch_frame_info::kOk)
            return kIOReturnSuccess;
        switch_refusal("display", "frame-info-clients", "ChangeFrameInfo %#x: already %u frame-info clients", mask,
                       switch_frame_info::Clients::kMax);
        return kIOReturnNoResources;
    }

    IOReturn sendFrameInfo(void *client, const void *frame) override
    {
        switch_frame_info::Clients::Entry *entry = frameInfo->clients.find(client);
        if (!entry || !frame)
            return kIOReturnSuccess;
        uint64_t words[switch_frame_info::kEntryWords];
        const unsigned n = switch_frame_info::entry(entry->mask, *static_cast<const switch_frame_info::Frame *>(frame),
                                                    entry->lastPresented, words);
        void *owner = nullptr;
        memcpy(&owner, static_cast<const unsigned char *>(client) + frameInfoClientOwner, sizeof(owner));
        const IOReturn status = native_legacy_frame_info_enqueue(owner, words, n * sizeof(words[0]));
        if (status != kIOReturnSuccess) {
            switch_refusal("display", "frame-info-queue",
                           "frame info for swap %u dropped (%#x): the client's queue is full or gone",
                           static_cast<uint32_t>(words[1]), status);
        }
        return status;
    }

    IOReturn newUserClient(task_t task, void *security, UInt32 type, IOUserClient **client) override
    {
        native_legacy_client_enter(task, type);
        char opener[17] = {};
        proc_selfname(opener, sizeof(opener));
        if (!strcmp(opener, "backboardd"))
            switch_render_server_watch(task);  // the opener, in its own context
        const IOReturn status = IOMobileFramebufferLegacy::newUserClient(task, security, type, client);
        native_legacy_client_observed(status, status == kIOReturnSuccess && client ? *client : nullptr);
        if (status == kIOReturnSuccess && client && *client && primary && displaySleepEnabled())
            answerPowerRequests(*client);
        return status;
    }

    // Display power requests. The render server blanks the panel with IOMobileFramebufferRequestPowerChange (QuartzCore; user-client
    // selector 12, one scalar: 0 off, else on). The original client passes it to the family only when the framebuffer's power-capable
    // byte (+0x8c) is set, as a platform subclass would, and that byte also makes every swap fail while the platform power word
    // (+0x16d4) is 0, which nothing here writes; so that path stays closed (it answers kIOReturnUnsupported) and the family's powered
    // state stays as announcePoweredByFirmware left it. Instead each client's externalMethod (slot 168, as LegacyDisplayProbe's
    // interposer, which this wraps when it ran first) is given a wrapper that hands selector 12 to SwitchBrightness; the compositor stops
    // presenting while it believes the display is off. The vtable copy keeps the wrapped entry one past its end; like the probe's copies
    // it lives as long as the kernel (one per client of the main display).
    static constexpr unsigned kClientSlots = 256, kExternalMethodSlot = 168, kRequestPowerChange = 12;
    typedef IOReturn (*ExternalMethod)(IOUserClient *, uint32_t, IOExternalMethodArguments *,
                                       IOExternalMethodDispatch *, OSObject *, void *);

    static IOReturn powerExternal(IOUserClient *client, uint32_t selector, IOExternalMethodArguments *args,
                                  IOExternalMethodDispatch *dispatch, OSObject *target, void *ref)
    {
        if (selector == kRequestPowerChange && args && args->scalarInput && args->scalarInputCount == 1 &&
            switch_display_power_request(args->scalarInput[0] != 0))
            return kIOReturnSuccess;
        const uintptr_t original = (*reinterpret_cast<uintptr_t *const *>(client))[kClientSlots];
        return reinterpret_cast<ExternalMethod>(original)(client, selector, args, dispatch, target, ref);
    }

    // Unless switch-no-sleep (SwitchBrightness.h switch_display_power_enabled, the same rule): with it the clients keep their
    // original vtables, exactly as before display power existed.
    static bool displaySleepEnabled()
    {
        unsigned off = 0;
        PE_parse_boot_argn("switch-no-sleep", &off, sizeof(off));
        return off == 0;
    }

    static void answerPowerRequests(IOUserClient *client)
    {
        const uintptr_t *table = *reinterpret_cast<uintptr_t *const *>(client);
        uintptr_t *copy = static_cast<uintptr_t *>(IOMalloc((kClientSlots + 1) * sizeof(uintptr_t)));
        if (!copy)
            return;
        memcpy(copy, table, kClientSlots * sizeof(uintptr_t));
        copy[kClientSlots] = copy[kExternalMethodSlot];
        copy[kExternalMethodSlot] = reinterpret_cast<uintptr_t>(&powerExternal);
        __asm__ volatile("dmb ish" ::: "memory");
        *reinterpret_cast<uintptr_t **>(client) = copy;
    }

    bool startHardware() override
    {
        PE_Video video = {};
        IOService *provider = getProvider();
        if (!provider || PE_current_console(&video) || !native_legacy_video_mode(video))
            return false;
        IOMemoryMap *mapping = provider->mapDeviceMemoryWithIndex(0, kIOMapWriteCombineCache);
        if (!mapping)
            return false;
        bool valid = (mapping->getVirtualAddress() != 0u) && mapping->getPhysicalAddress() == video.v_baseAddr &&
                     mapping->getLength() == video.v_rowBytes * video.v_height;
        if (valid) {
            installBootDimensions(native_legacy_video_width(video), native_legacy_video_height(video));
            uint32_t width = 0;
            uint32_t height = 0;
            getDisplayDimensions(&width, &height);
            valid = width == native_legacy_video_width(video) && height == native_legacy_video_height(video);
            if (valid) {
                OSSafeReleaseNULL(bootMapping);
                bootMapping = mapping;
                scanAddresses[0] = bootMapping->getPhysicalAddress();
                mapping = nullptr;
                // The Switch boots its panel in text mode (v_display=0); it is still the one built-in display.
                primary = true;
                valid = native_legacy_gop_mapping_observed(this, bootMapping, width, height);
                if (valid && SWITCH_PORTRAIT_NATIVE)
                    publishStrideRequirements();
            }
        }
        OSSafeReleaseNULL(mapping);
        return valid;
    }

    // The compositor's framebuffer rows (QuartzCore CA::WindowServer::AppleDisplay::update_display_limits reads this IOMFB
    // property; iosurface_alignment() then rounds every framebuffer's bytesPerRow up to it, 64 by default), and VideoToolbox's
    // outputs (native_planes::kPublishedRowAlignment): 720 x 4 = 2880 becomes 3072-byte rows. The scanout copies take any
    // source stride.
    void publishStrideRequirements()
    {
        OSDictionary *requirements = OSDictionary::withCapacity(1);
        const OSNumber *horizontal = OSNumber::withNumber(native_planes::kPublishedRowAlignment, 32);
        if (requirements && horizontal && requirements->setObject("StrideLinearHorizontal", horizontal))
            setProperty("DisplayPipeStrideRequirements", requirements);
        OSSafeReleaseNULL(horizontal);
        OSSafeReleaseNULL(requirements);
        publishPlaneLimits();
    }

    // r18-planes: the limits Core Animation detaches a layer within (AppleDisplay::update_display_limits reads them once, at
    // the display's creation; native_planes::Limits has the values and why). With backboardd still under CA_DISABLE_DETACHED
    // (native_legacy_detach_ready false) nothing is detached and these change nothing.
    void publishPlaneLimits()
    {
#if SWITCH_IPSW_KIND_24B119
        constexpr native_planes::Limits limits = native_planes::kLimits24B119;
#else
        constexpr native_planes::Limits limits;
#endif

        struct Entry {
            const char *key;
            uint64_t value;
        };

        const auto publish = [this](const char *name, const Entry *entries, unsigned count, unsigned bits) {
            OSDictionary *dict = OSDictionary::withCapacity(count);
            for (unsigned i = 0; dict && i < count; ++i) {
                if (const OSNumber *n = OSNumber::withNumber(entries[i].value, bits)) {
                    dict->setObject(entries[i].key, n);
                    n->release();
                }
            }
            if (dict)
                setProperty(name, dict);
            OSSafeReleaseNULL(dict);
        };
        const Entry scaling[] = {{"RGBLayer_MinScaleFraction", limits.minScaleFraction},
                                 {"RGBLayer_MaxScale", limits.maxScale},
                                 {"YUVLayer_MinScaleFraction", limits.yuvMinScaleFraction},
                                 {"YUVLayer_MaxScale", limits.maxScale}};
        publish("IOMFBScalingLimits", scaling, 4, 32);
        // The panel's pixel clock (TegraDisplayPower.cpp: PLLD 468 MHz, 78 MHz for 60 Hz) as both clocks [I: what
        // valid_clocking_p compares them with is not reversed; A10X computes MaxVideoSrcDownscalingWidth from its own].
        const Entry source[] = {{"MaxSrcRectWidth", limits.maxSrcRectWidth},
                                {"MaxSrcRectTotal", limits.maxSrcRectTotal},
                                {"MaxSrcBufferWidth", limits.maxSrcBuffer},
                                {"MaxSrcBufferHeight", limits.maxSrcBufferHeight},
                                {"MaxVideoSrcDownscalingWidth", limits.maxSrcRectWidth},
                                {"VideoClock", 78000000},
                                {"PixelClock", 78000000},
                                {"IOMFBCompressionSupport", 0}};
        publish("IOMFBMaxSrcPixels", source, limits.compressionUnsupported ? 8 : 7, 64);
        const Entry base[] = {{"PlaneBaseAlignmentLinear", native_planes::kPublishedBaseAlignment}};
        publish("DisplayPipePlaneBaseAlignment", base, 1, 64);
        const Entry layers[] = {{"MaxNumLayers", limits.maxLayers}};
        publish("IOMFBNumLayers", layers, 1, 32);
        if (limits.packedYuvUnsupported)
            setProperty("IOMFBYUVPackedUnSupported", true);
    }

    IOReturn getGammaTable(void *table) override
    {
        return color->getGammaTable(table);
    }

    IOReturn setGammaTable(const void *table) override
    {
        return color->setGammaTable(table);
    }

    IOReturn getMatrix(uint32_t location, void *matrix) override
    {
        return color->getMatrix(location, matrix);
    }

    IOReturn setMatrix(uint32_t location, const void *matrix) override
    {
        return color->setMatrix(location, matrix);
    }

    IOReturn setContrast(const void *contrast) override
    {
        return color->setContrast(contrast);
    }

    IOReturn setColorRemapMode(uint32_t mode) override
    {
        return color->setColorRemapMode(mode);
    }

    IOReturn getColorRemapMode(uint32_t *mode) override
    {
        return color->getColorRemapMode(mode);
    }

    IOReturn setBrightnessCorrection(uint32_t value) override
    {
        return color->setBrightnessCorrection(value);
    }

    IOReturn swapSetColorMatrix(const void *matrix, uint32_t location, uint32_t flags) override
    {
        return color->swapSetColorMatrix(matrix, location, flags);
    }

    IOReturn setAccessibilityScale(uint32_t scale) override
    {
        return color->setAccessibilityScale(scale);
    }

    void setEdrFactor(float factor) override
    {
        color->setEdrFactor(factor);
    }

#if SWITCH_IPSW_KIND_24B119
    IOReturn setProperties(OSObject *properties) override
    {
        const OSDictionary *dict = OSDynamicCast(OSDictionary, properties);
        const OSBoolean *enable = dict ? OSDynamicCast(OSBoolean, dict->getObject(switch_sbim::kEnableKey)) : nullptr;
        IOReturn status = kIOReturnSuccess;
        if (dict && switch_sbim::answer(dict->getCount(), enable ? static_cast<int>(enable->isTrue()) : -1, status))
            return status;
        return IOMobileFramebufferLegacy::setProperties(properties);
    }
#endif

    // thermalmonitord's TSFD. J120's LCD takes the panel temperature for its temperature and pixel response compensation;
    // DC A has neither and the OLED panel compensates for temperature itself, so the value is taken and reported once.
    IOReturn setDisplayTemperature(const uint32_t *temperatures) override
    {
        if (!temperatures)
            return kIOReturnBadArgument;
        static bool reported;
        if (!__atomic_exchange_n(&reported, true, __ATOMIC_RELAXED)) {
            int64_t sum = 0;
            for (unsigned i = 0; i < 4; ++i)
                sum += static_cast<int32_t>(temperatures[i]);
            const int64_t hundredths = sum * 100 / 4 / 65536;
            IOLog("SwitchDisplay: display temperature %s%lld.%02lld C from thermalmonitord (TSFD); the panel "
                  "compensates for temperature itself\n",
                  hundredths < 0 ? "-" : "", (hundredths < 0 ? -hundredths : hundredths) / 100,
                  (hundredths < 0 ? -hundredths : hundredths) % 100);
        }
        return kIOReturnSuccess;
    }

    // J120's pipe does nothing when a client registers, and treats its going away as ChangeFrameInfo to no keys.
    void platformFrameInfoNotify(void *client, bool enable) override
    {
        if (!enable)
            frameInfo->clients.change(client, 0);
    }

    // DC A's timing generator, which display power (setScanout) and SC7 stop.
    bool areTimingsEnabled() const override
    {
        if (SWITCH_FRAME_TIMELINE)
            logHeldHead();
        return dc && !scanoutStopped && !hwSuspended;
    }

    // Debug kernels. IOMFB asks for the timing state when a swap wait has gone 5 s without a completion (swap_wait_gated, just
    // before it cancels the queue) and when its last client goes away: a swap at the queue head for over a second then gets one
    // line of what holds it.
    void logHeldHead() const
    {
        const OriginalLegacySwapRequest *head = pendingSwapRequest();
        if (!head)
            return;
        const uint64_t accepted = submittedAt(head);
        const uint64_t now = mach_absolute_time();
        uint64_t second = 0;
        nanoseconds_to_absolutetime(1000000000ULL, &second);
        if (accepted != 0u && now - accepted < second)
            return;
        uint32_t id = 0;
        memcpy(&id, reinterpret_cast<const unsigned char *>(head) + swapId, sizeof(id));
        const auto flag = [](bool on) {
            return on ? 1 : 0;
        };
        const char *scanout = "running";
        if (hwSuspended) {
            scanout = "suspended";
        } else if (scanoutStopped) {
            scanout = "stopped";
        }
        IOLog(
            "SwitchDisplay: swap %u held at the queue head %llu ms (accepted %d, taken %llu of %llu): transferred %d, "
            "deferred %d (probe %d, plane probe %d, column scan %d, window probe %d, tape %d, no surface %d), ring "
            "front "
            "%d pending %d "
            "replaced %d drawing %d held %d, latch retire %d, direct %d, planes %d (armed %d, held %d), gpu %d%s, "
            "copy %d%s, scanout %s, %llu frame ends, %llu since the last flip\n",
            id, accepted != 0u ? static_cast<unsigned long long>(ticksNs(now - accepted) / 1000000) : 0ULL,
            flag(isValidated(head)), static_cast<unsigned long long>(seqOf(head)),
            static_cast<unsigned long long>(takeSeq), flag(transferred), flag(kickDeferred),
            flag(probe.active() || probeRequest), flag(planeProbe.active()), flag(columnScanBusy()),
            flag(windowProbeHolds()), flag(tapeSettle), flag(ring.target() == kNoBuffer), ring.front, ring.pending,
            ring.superseded, ring.drawing, ring.held, flag(latchRetire), flag(directOn), flag(planesOn),
            planeLatch.armed, flag(planeHeldSet), flag(gpuRequest != nullptr), gpuRequest == head ? " (this swap)" : "",
            flag(copyQueued), copyQueued && job.request == head ? " (this swap)" : "", scanout,
            static_cast<unsigned long long>(frameEnds), static_cast<unsigned long long>(frameEnds - flipRequested));
    }

    // As J120's AppleCLCD and J181's UnifiedPipeline answer: the internal panel is never plugged or unplugged, and it has
    // one refresh rate.
    IOReturn platformInternalPlug() override
    {
        return kIOReturnUnsupported;
    }

    IOReturn platformInternalUnplug() override
    {
        return kIOReturnUnsupported;
    }

    void platformRefreshRates() override
    {}
};

// OSDefineMetaClassAndStructors(NativeLegacyDisplay, IOMobileFramebufferLegacy), registered under the class name of J120's
// and J181's internal display (AppleMobileDispH9G's and AppleMobileDispH12P's IOClass). iPadOS finds the internal display
// as an AppleCLCD: VideoToolbox's and MediaToolbox's FigGetAlignmentForIOSurfaceOutput match IOServiceMatching("AppleCLCD")
// for the display pipe's alignment, and the poster extensions' sandbox profiles allow iokit-get-properties on the
// iokit-registry-entry-class AppleCLCD. Neither kext is prelinked here, so the name is free.
OSDefineMetaClassWithInit(NativeLegacyDisplay, IOMobileFramebufferLegacy, ) NativeLegacyDisplay::MetaClass::MetaClass()
    : OSMetaClass("AppleCLCD", NativeLegacyDisplay::superClass, sizeof(NativeLegacyDisplay))
{}

OSDefineDefaultStructors(NativeLegacyDisplay,
                         IOMobileFramebufferLegacy) NativeLegacyDisplay *NativeLegacyDisplay::primaryDisplay;
constinit uint32_t NativeLegacyDisplay::stPlaneNative = 0;
constinit NativeLegacyDisplay::LandscapeState NativeLegacyDisplay::landscapeState;
#if SWITCH_FRAME_TIMELINE
constinit NativeLegacyDisplay::ColumnScanState NativeLegacyDisplay::columnScan;
constinit struct sysctl_oid NativeLegacyDisplay::columnScanOid = {};
constinit NativeLegacyDisplay::WindowProbeState NativeLegacyDisplay::windowProbe;
constinit NativeLegacyDisplay::LandscapeRunState NativeLegacyDisplay::landscapeRun = {};

// HW Test's landscape run (SwitchLandscapeRunABI.h): its SAMPLE and QUERY requests, from TegraVIC's kernel-test
// selector (which holds a FORCE itself, for the calling connection).
extern "C" IOReturn switch_display_landscape_run_call(const IOExternalMethodArguments *a)
{
    const auto *request = static_cast<const SwitchDisplayLandscapeRequest *>(a->structureInput);
    if (a->scalarInputCount || a->scalarOutputCount || a->structureInputSize != sizeof(*request) || !request ||
        request->magic != SWITCH_DISPLAY_LANDSCAPE_RUN_MAGIC || request->reserved ||
        a->structureOutputSize != sizeof(SwitchDisplayLandscapeReply) || !a->structureOutput)
        return kIOReturnBadArgument;
    NativeLegacyDisplay *display = __atomic_load_n(&NativeLegacyDisplay::primaryDisplay, __ATOMIC_ACQUIRE);
    if (!display)
        return kIOReturnNotReady;
    const IOReturn status =
        display->landscapeRunCall(*request, *static_cast<SwitchDisplayLandscapeReply *>(a->structureOutput));
    return status;
}

// HW Test's window probes (DisplayWindowProbe.h), from TegraVIC's kernel-test selector: the request and reply are
// DisplayWindowProbeABI.h's.
extern "C" IOReturn switch_display_window_probe_call(const IOExternalMethodArguments *a)
{
    const auto *request = static_cast<const SwitchDisplayWindowProbeRequest *>(a->structureInput);
    if (a->scalarInputCount || a->scalarOutputCount || a->structureInputSize != sizeof(*request) || !request ||
        request->magic != SWITCH_DISPLAY_WINDOW_PROBE_MAGIC || request->reserved ||
        a->structureOutputSize != sizeof(SwitchDisplayWindowProbeReply) || !a->structureOutput)
        return kIOReturnBadArgument;
    NativeLegacyDisplay *display = __atomic_load_n(&NativeLegacyDisplay::primaryDisplay, __ATOMIC_ACQUIRE);
    if (!display)
        return kIOReturnNotReady;
    SwitchDisplayWindowProbeReply reply;
    const IOReturn status = display->windowProbeCall(*request, reply);
    if (status == kIOReturnSuccess)
        memcpy(a->structureOutput, &reply, sizeof(reply));
    return status;
}
#endif

// SwitchBrightness's display power sequence (SwitchBrightness.h): from its work loop, after the panel is asleep and before it wakes.
extern "C" bool native_legacy_display_set_scanout(bool on)
{
    NativeLegacyDisplay *display = __atomic_load_n(&NativeLegacyDisplay::primaryDisplay, __ATOMIC_ACQUIRE);
    IOWorkLoop *loop = display ? display->getWorkLoop() : nullptr;
    if (!loop)
        return false;
    return loop->runAction(
               [](OSObject *owner, void *arg, void *, void *, void *) -> IOReturn {
                   return static_cast<NativeLegacyDisplay *>(owner)->setScanout(arg != nullptr) ? kIOReturnSuccess
                                                                                                : kIOReturnNotReady;
               },
               display, on ? display : nullptr) == kIOReturnSuccess;
}

// Frame-end holds, from any context, never sleeping: while one is held DC A's FRAME_END reaches the CPU (it is unmasked before
// hold returns, unless the scanout is stopped: setScanout unmasks it at the restart). Released, it is masked again at the
// first frame end with nothing else waiting.
extern "C" void native_legacy_frame_end_hold(void)
{
    __atomic_fetch_add(&gFrameEndHolds, 1u, __ATOMIC_ACQ_REL);
    if (NativeLegacyDisplay *display = __atomic_load_n(&NativeLegacyDisplay::primaryDisplay, __ATOMIC_ACQUIRE))
        display->holdFrameEnd();
}

extern "C" void native_legacy_frame_end_release(void)
{
    if (!__atomic_load_n(&gFrameEndHolds, __ATOMIC_ACQUIRE))
        panic("SwitchDisplay: frame-end hold released without a hold");
    __atomic_fetch_sub(&gFrameEndHolds, 1u, __ATOMIC_ACQ_REL);
}

extern "C" bool native_legacy_frame_end_held(void)
{
    NativeLegacyDisplay *display = __atomic_load_n(&NativeLegacyDisplay::primaryDisplay, __ATOMIC_ACQUIRE);
    return (__atomic_load_n(&gFrameEndHolds, __ATOMIC_ACQUIRE) != 0u) && (display != nullptr) &&
           display->frameEndRouted();
}

// SC7 hooks for the sleep code; contract in TegraDisplayPower.h. The panel sleeps and wakes with TegraPanel's own power sequence
// (display power's), the controller stops and restarts with setScanout, and the pipeline itself goes down and comes up cold in
// TegraDisplayPower.cpp; the steps that touch window A and the interrupt run on the display's work loop.
template <typename Body> static bool onDisplayLoop(NativeLegacyDisplay *display, Body body)
{
    bool result = false;

    struct Call {
        Body *body;
        bool *result;
    } call = {&body, &result};

    display->getWorkLoop()->runAction(
        [](OSObject *owner, void *a0, void *, void *, void *) -> IOReturn {
            const Call *c = static_cast<Call *>(a0);
            *c->result = (*c->body)(static_cast<NativeLegacyDisplay *>(owner));
            return kIOReturnSuccess;
        },
        display, &call);
    return result;
}

static NativeLegacyDisplay *sc7Display(const char *step)
{
    NativeLegacyDisplay *display = __atomic_load_n(&NativeLegacyDisplay::primaryDisplay, __ATOMIC_ACQUIRE);
    if (!display || !display->getWorkLoop())
        panic("SwitchDisplay: %s before the display started", step);
    const uint32_t panel = tegra_display_panel_id();
    if (panel != TEGRA_DISPLAY_PANEL_AMS699VC01) {
        panic("SwitchDisplay: %s: panel %#x has no cold init here (only the OLED's AMS699VC01, %#x)", step, panel,
              TEGRA_DISPLAY_PANEL_AMS699VC01);
    }
    return display;
}

extern "C" void tegra_display_suspend(void)
{
    NativeLegacyDisplay *display = sc7Display("tegra_display_suspend");
    const bool lit = onDisplayLoop(display, [](NativeLegacyDisplay *d) {
        return d->beginSuspend();
    });
    if (lit && tegra_panel_power_off())
        IOLog("SwitchDisplay: SC7 panel power-off failed: the display is not running\n");
    onDisplayLoop(display, [](NativeLegacyDisplay *d) {
        d->quiesceForSuspend();
        return true;
    });
    switch_sleep_mark(SWITCH_STEP_DISPLAY_QUIESCED);
    IOSleep(20);  // DC A stops at the end of the frame it was scanning
    tegra_display_power_down();
}

extern "C" void tegra_display_resume(void)
{
    NativeLegacyDisplay *display = sc7Display("tegra_display_resume");
    tegra_display_power_up();
    const bool lit = onDisplayLoop(display, [](NativeLegacyDisplay *d) {
        return d->finishResume();
    });
    // Its first step waits for a frame end of the restarted DC A (a lost DC A interrupt panics there, naming the GIC state).
    if (lit && tegra_panel_power_on(-1))
        IOLog("SwitchDisplay: SC7 panel power-on failed: the display is not running\n");
}

IOService *native_legacy_display_allocate(void)
{
    return new NativeLegacyDisplay;
}

extern "C" void native_legacy_gpu_complete(void *display, int status)
{
    if (display)
        static_cast<NativeLegacyDisplay *>(display)->gpuComplete(status);
}

// r18-planes, XNU kern_exec.c (native_switch_exec_environment, when it starts backboardd): may Core Animation detach layers?
// Only when the plane probe passed this boot (window B scans through the SMMU) and TegraVIC's traits have the
// IOSurfaceAccelerator quarter turns (transforms 4 and 7), 4:2:0 to RGB and a row pitch within ours (kTegraVICTraits),
// since on an internal display every rotated detached layer (here: every landscape video) is first copied through
// it into a destination Core Animation allocates, and what Core Animation does when that copy is refused is not known.
// The alignment Core Animation sizes those destinations by is VIC's from start (native_planes::kPublishedRowAlignment).
// Otherwise backboardd keeps CA_DISABLE_DETACHED=1 and nothing changes. One line per call.
extern "C" bool switch_native_surface_display_ready(void)
{
    const auto *display = __atomic_load_n(&NativeLegacyDisplay::primaryDisplay, __ATOMIC_ACQUIRE);
    return display && display->planesReady();
}

// TegraVIC's engine thread, before it records a quarter turn instead of copying it (SwitchVICTurn.h). Any context.
extern "C" bool switch_display_landscape_ready(void)
{
    const auto *display = __atomic_load_n(&NativeLegacyDisplay::primaryDisplay, __ATOMIC_ACQUIRE);
    return display && display->landscapeReady();
}

extern "C" bool native_legacy_detach_ready(void)
{
    const NativeLegacyDisplay *display = __atomic_load_n(&NativeLegacyDisplay::primaryDisplay, __ATOMIC_ACQUIRE);
    const bool planes = (display != nullptr) && display->planesReady();
    const TegraVICTraits *vic = tegra_vic_ready() ? tegra_vic_traits() : nullptr;
    const bool turns =
        (vic != nullptr) && (vic->rotations & 0x90) == 0x90 && ((vic->yccIn & TEGRA_VIC_YCC_IN_RGB) != 0u);
    const uint32_t pitch = vic ? vic->sourcePitchAlign : 0;
    const bool pitched =
        (pitch != 0u) && pitch <= native_planes::kPublishedRowAlignment && ((pitch & (pitch - 1)) == 0u);
    const bool ok = planes && turns && pitched;
    IOLog("SwitchDisplay: backboardd %s detach layers: planes %s, IOSurfaceAccelerator quarter turns %s (transforms "
          "%#x), VIC row pitch %u%s (rows %u, plane bases %u)\n",
          ok ? "may" : "may not", planes ? "on" : "off", turns ? "proven" : "not proven", vic ? vic->rotations : 0,
          pitch, pitched ? "" : " (not proven)", native_planes::kPublishedRowAlignment,
          native_planes::kPublishedBaseAlignment);
    return ok;
}

// TegraGM20B gpu_clock.c (idle clock): the last swap, in nanoseconds of uptime; 0 before the first. Any context.
extern "C" uint64_t native_display_last_swap_ns(void)
{
    uint64_t ns = 0;
    const uint64_t abs = __atomic_load_n(&gLastSwapAbs, __ATOMIC_RELAXED);
    if (abs)
        absolutetime_to_nanoseconds(abs, &ns);
    return ns;
}

#include "SwitchPlaneCompositeProbe.h"
#include "SwitchConsoleRelease.h"
