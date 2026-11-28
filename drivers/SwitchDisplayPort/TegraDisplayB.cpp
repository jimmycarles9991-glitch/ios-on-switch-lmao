// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2018 naehrwert
// Copyright (c) 2018-2025 CTCaer
// Copyright (C) 2012 Avionic Design GmbH
// Copyright (C) 2012 NVIDIA CORPORATION
// Display controller B; see TegraDisplayB.h. kInit is display controller A's base configuration from hekate
// (github.com/CTCaer/hekate, GPL-2.0-only): bdk/display/di.inl _di_dc_init_config, as TegraDisplayPower.cpp has it,
// without the DSI parts. The rest is Linux drivers/gpu/drm/tegra/dc.c and dc.h: tegra_crtc_atomic_enable,
// tegra_dc_set_timings (reference-to-sync 1/1), tegra_dc_commit_state (shift clock 1:1), tegra_dc_setup_window for each
// window, SOR_ENABLE(1) = DC_DISP_DISP_WIN_OPTIONS bit 26. Power: the DISB partition (19), DISP2 clock and reset 26, MC
// client hot reset DCB (MC 0x200 bit 3, Linux memory/tegra/tegra210.c), the MBIST workaround (clk-tegra210.c
// tegra210_generic_mbist_war, LVL2_CLK_GATE_OVRA bit 2).
#include "TegraDisplayB.h"

#include <IOKit/IOLib.h>
#include "../TegraDisplayFlip/SwitchDisplayPriority.h"
#include "../TegraSMMU/TegraSMMU.h"

#include "../../kits/SwitchCAR/SwitchCAR.h"
#include "../../kits/SwitchCAR/SwitchPMC.h"
#include "../../kits/SwitchMMIO/SwitchMMIOKernel.h"

namespace {
constexpr uint64_t kDisplayBPA = 0x54240000;
constexpr unsigned kClockDisp2 = 26, kPartitionDisb = 19;
constexpr uint32_t kMcHotResetControl = 0x200, kMcHotResetStatus = 0x204, kMcHotResetDcb = 1u << 3;
constexpr uint32_t kLvl2ClockGateOverrideA = 0xf8, kOverrideDisb = 1u << 2;
constexpr uint32_t kStateAccessReadActive = 1u << 0, kStateAccessWriteActive = 1u << 2;
constexpr unsigned kWindows = switch_external::kWindows;

enum : uint32_t {
    DC_CMD_DISPLAY_COMMAND_OPTION0 = 0x031,
    DC_CMD_DISPLAY_COMMAND = 0x032,
    DC_CMD_DISPLAY_POWER_CONTROL = 0x036,
    DC_CMD_INT_STATUS = 0x037,
    DC_CMD_INT_MASK = 0x038,
    DC_CMD_INT_ENABLE = 0x039,
    DC_CMD_STATE_ACCESS = 0x040,
    DC_CMD_STATE_CONTROL = 0x041,
    DC_CMD_DISPLAY_WINDOW_HEADER = 0x042,
    DC_CMD_REG_ACT_CONTROL = 0x043,
    DC_COM_CRC_CONTROL = 0x300,
    DC_COM_PIN_OUTPUT_POLARITY1 = 0x307,
    DC_COM_PIN_OUTPUT_POLARITY3 = 0x309,
    DC_DISP_DISP_WIN_OPTIONS = 0x402,
    DC_DISP_DISP_MEM_HIGH_PRIORITY = 0x403,
    DC_DISP_DISP_MEM_HIGH_PRIORITY_TIMER = 0x404,
    DC_DISP_DISP_TIMING_OPTIONS = 0x405,
    DC_DISP_REF_TO_SYNC = 0x406,
    DC_DISP_SYNC_WIDTH = 0x407,
    DC_DISP_BACK_PORCH = 0x408,
    DC_DISP_ACTIVE = 0x409,
    DC_DISP_FRONT_PORCH = 0x40a,
    DC_DISP_DISP_CLOCK_CONTROL = 0x42e,
    DC_DISP_DISP_INTERFACE_CONTROL = 0x42f,
    DC_DISP_DISP_COLOR_CONTROL = 0x430,
    DC_DISP_DC_MCCIF_FIFOCTRL = 0x480,
    DC_DISP_BLEND_BACKGROUND_COLOR = 0x4e4,
    DC_WINC_H_FILTER_P00 = 0x601,
    DC_WINC_CSC_YOF = 0x611,
    DC_WINC_V_FILTER_P00 = 0x619,
    DC_WIN_WIN_OPTIONS = 0x700,
    DC_WIN_BYTE_SWAP = 0x701,
    DC_WIN_BUFFER_CONTROL = 0x702,
    DC_WIN_COLOR_DEPTH = 0x703,
    DC_WIN_POSITION = 0x704,
    DC_WIN_SIZE = 0x705,
    DC_WIN_PRESCALED_SIZE = 0x706,
    DC_WIN_H_INITIAL_DDA = 0x707,
    DC_WIN_V_INITIAL_DDA = 0x708,
    DC_WIN_DDA_INC = 0x709,
    DC_WIN_LINE_STRIDE = 0x70a,
    DC_WIN_BUF_STRIDE = 0x70b,
    DC_WIN_UV_BUF_STRIDE = 0x70c,
    DC_WIN_BUFFER_ADDR_MODE = 0x70d,
    DC_WIN_DV_CONTROL = 0x70e,
    DC_WIN_BLEND_LAYER_CONTROL = 0x716,
    DC_WIN_BLEND_MATCH_SELECT = 0x717,
    DC_WIN_BLEND_NOMATCH_SELECT = 0x718,
    DC_WINBUF_START_ADDR = 0x800,
    DC_WINBUF_START_ADDR_U = 0x802,
    DC_WINBUF_ADDR_H_OFFSET = 0x806,
    DC_WINBUF_ADDR_V_OFFSET = 0x808,
    DC_WINBUF_SURFACE_KIND = 0x80b,
    DC_WINBUF_START_ADDR_HI = 0x80d,
    DC_WINBUF_START_ADDR_HI_U = 0x80f,
    DC_WINBUF_CDE_CONTROL = 0x82f,
    DC_WINBUF_AD_UFLOW_STATUS = 0xbca,  // window A's direct aperture; B and C follow 0x200 words apart
};

constexpr uint32_t kGeneralActReq = 1u << 0, kGeneralUpdate = 1u << 8;
constexpr uint32_t kFrameEnd = 1u << 1, kContinuousDisplay = 1u << 5, kSor1Enable = 1u << 26;
constexpr uint32_t kPowerAll = 0x50155;  // PW0..PW4, PM0, PM1
constexpr uint32_t kWinEnable = 1u << 30, kB8G8R8A8 = 12, kDdaOne = 0x1000;
constexpr uint32_t kHFilterTaps[16] = {0x00008000, 0x3e087ce1, 0x3b117ac1, 0x591b73aa, 0x57256d9a, 0x552f668b,
                                       0x73385e8b, 0x72435583, 0x714c4c8b, 0x70554393, 0x715e389b, 0x71662faa,
                                       0x536d25ba, 0x55731bca, 0x387a11d9, 0x3c7c08f1};

constexpr uint32_t actRequest(unsigned window)
{
    return 1u << (1 + window);
}

constexpr uint32_t kAllWindows = actRequest(0) | actRequest(1) | actRequest(2);

bool sameWindowMemory(const switch_display_memory_window &x, const switch_display_memory_window &y)
{
    return x.active == y.active && x.peak_bytes_per_second == y.peak_bytes_per_second &&
           x.required_buffering_bytes == y.required_buffering_bytes &&
           x.latency_buffering_bytes == y.latency_buffering_bytes;
}

bool sameMemoryPlan(const switch_display_memory_head &a, const switch_display_memory_window (&windows)[kWindows],
                    uint64_t peakBytes)
{
    if (a.peak_bytes_per_second != peakBytes)
        return false;
    for (unsigned w = 0; w < kWindows; ++w) {
        if (!sameWindowMemory(a.windows[w], windows[w]))
            return false;
    }
    return true;
}

bool sameFrameMemory(const switch_display_memory_window (&a)[kWindows], uint64_t peakA,
                     const switch_display_memory_window (&b)[kWindows], uint64_t peakB)
{
    if (peakA != peakB)
        return false;
    for (unsigned w = 0; w < kWindows; ++w) {
        if (!sameWindowMemory(a[w], b[w]))
            return false;
    }
    return true;
}

struct Reg {
    uint32_t index, value;
};

constexpr Reg kInit[] = {
    {DC_CMD_STATE_ACCESS, 1},  // Read active, write assembly. DCB has a single window writer.
    {DC_CMD_REG_ACT_CONTROL, 0},
    {DC_CMD_STATE_CONTROL, kGeneralUpdate},
    {DC_CMD_STATE_CONTROL, kGeneralActReq},
    {DC_DISP_DC_MCCIF_FIFOCTRL, 0},
    {DC_DISP_DISP_MEM_HIGH_PRIORITY, SwitchDisplayPriority::kThresholds},
    {DC_DISP_DISP_MEM_HIGH_PRIORITY_TIMER, SwitchDisplayPriority::kDelays},
    {DC_CMD_DISPLAY_WINDOW_HEADER, 0xf0},
    {DC_WIN_WIN_OPTIONS, 0},
    {DC_WIN_DV_CONTROL, 0},
    {DC_DISP_DISP_COLOR_CONTROL, 8},
    {DC_DISP_DISP_INTERFACE_CONTROL, 0},
    {DC_COM_PIN_OUTPUT_POLARITY1, 0},
    {DC_COM_PIN_OUTPUT_POLARITY3, 0},
    {DC_DISP_BLEND_BACKGROUND_COLOR, 0},
    {DC_COM_CRC_CONTROL, 0},
    {DC_WIN_BLEND_LAYER_CONTROL, (1u << 24) | 255},
    {DC_CMD_DISPLAY_COMMAND_OPTION0, 0},
    {DC_DISP_DISP_WIN_OPTIONS, 0},
    {DC_CMD_DISPLAY_COMMAND, 0},
    {DC_CMD_STATE_CONTROL, 0x1f00},
    {DC_CMD_STATE_CONTROL, 0x1f},
};
}  // namespace

uint32_t TegraDisplayB::read(uint32_t word) const
{
    return switch_mmio_read32(base, word * 4u);
}

void TegraDisplayB::write(uint32_t word, uint32_t value)
{
    switch_mmio_write32(base, word * 4u, value);
}

void TegraDisplayB::select(unsigned window)
{
    write(DC_CMD_DISPLAY_WINDOW_HEADER, 1u << (4 + window));
}

// One UPDATE write, then one ACT_REQ write naming the same windows, so they all switch at the same frame boundary
// (TRM 24.6.28).
void TegraDisplayB::commit(uint32_t update)
{
#if defined(__aarch64__) && !defined(SWITCH_MMIO_HOST_MODEL)
    __asm__ volatile("dsb sy" ::: "memory");
#endif
    write(DC_CMD_STATE_CONTROL, update << 8);
    write(DC_CMD_STATE_CONTROL, update);
}

bool TegraDisplayB::powerUp(char *why, size_t size)
{
    if (base == nullptr) {
        base = switch_mmio_map(kDisplayBPA, 0x40000);
    }
    if (!switch_pmc_power_up(kPartitionDisb, kClockDisp2, kClockDisp2, nullptr)) {
        snprintf(why, size, "the DISB power partition did not come up");
        return false;
    }
    // tegra210_generic_mbist_war for DISB: the partition's RAMs initialise only with their second-level clock gate
    // overridden for a moment after the partition comes up.
    switch_car_rmw(kLvl2ClockGateOverrideA, 0, kOverrideDisb);
    IODelay(1);
    switch_car_rmw(kLvl2ClockGateOverrideA, kOverrideDisb, 0);
    IODelay(1);
    tegra_mc_hot_reset(kMcHotResetControl, kMcHotResetDcb, false);
    emcRefusedLogged = false;
    for (const Reg &reg : kInit) {
        write(reg.index, reg.value);
    }
    return true;
}

void TegraDisplayB::forgetWindows()
{
    for (unsigned w = 0; w < kWindows; ++w) {
        windows[w] = {};
        csc[w][0] = csc[w][1] = -1;
    }
    latchedMemory = {};
    requestedMemory = {};
    memoryPending = false;
    requested = 0;
}

void TegraDisplayB::setMode(const struct switch_dp_mode &mode)
{
    memory = {1,
              mode.pixel_khz * 1000u,
              mode.hactive,
              mode.vactive,
              switch_dp_mode_htotal(&mode),
              switch_dp_mode_vtotal(&mode),
              0,
              0,
              {}};
    forgetWindows();
    emcRefusedLogged = false;
    for (unsigned w = 0; w < kWindows; ++w) {
        select(w);
        write(DC_WIN_WIN_OPTIONS, 0);
        // WINC filter tap registers are unshadowed (TRM 24.5.4) and must be programmed while the window is off.
        for (uint32_t i = 0; i < 16u; ++i) {
            write(DC_WINC_H_FILTER_P00 + i, kHFilterTaps[i]);
            write(DC_WINC_V_FILTER_P00 + i, 128u - 8u * i);
        }
        write(DC_WIN_BUFFER_CONTROL, 0);
        write(DC_WIN_COLOR_DEPTH, kB8G8R8A8);
        write(DC_WIN_BYTE_SWAP, 0);
        write(DC_WIN_POSITION, 0);
        write(DC_WIN_SIZE, (uint32_t(mode.vactive) << 16) | mode.hactive);
        write(DC_WIN_PRESCALED_SIZE, (uint32_t(mode.vactive) << 16) | (mode.hactive * 4u));
        write(DC_WIN_H_INITIAL_DDA, 0);
        write(DC_WIN_V_INITIAL_DDA, 0);
        write(DC_WIN_DDA_INC, (kDdaOne << 16) | kDdaOne);
        write(DC_WIN_BUF_STRIDE, 0);
        write(DC_WIN_UV_BUF_STRIDE, 0);
        write(DC_WIN_BUFFER_ADDR_MODE, 0);
        write(DC_WINBUF_ADDR_H_OFFSET, 0);
        write(DC_WINBUF_ADDR_V_OFFSET, 0);
        write(DC_WINBUF_START_ADDR_HI, 0);
        write(DC_WINBUF_START_ADDR_HI_U, 0);
        write(DC_WINBUF_CDE_CONTROL, 0);
#if DEVELOPMENT || DEBUG
        write(DC_WINBUF_AD_UFLOW_STATUS + 0x200u * w, 0);
#endif
    }
    write(DC_DISP_DISP_CLOCK_CONTROL, 0);
    write(DC_DISP_DISP_TIMING_OPTIONS, 1u);
    write(DC_DISP_REF_TO_SYNC, (1u << 16) | 1u);
    write(DC_DISP_SYNC_WIDTH, (static_cast<uint32_t>(mode.vsync) << 16) | mode.hsync);
    write(DC_DISP_BACK_PORCH, (static_cast<uint32_t>(mode.vback) << 16) | mode.hback);
    write(DC_DISP_FRONT_PORCH, (static_cast<uint32_t>(mode.vfront) << 16) | mode.hfront);
    write(DC_DISP_ACTIVE, (static_cast<uint32_t>(mode.vactive) << 16) | mode.hactive);
    write(DC_CMD_INT_STATUS, ~0u);
    write(DC_CMD_INT_ENABLE, 0);
    frameInterruptEnabled = false;
    write(DC_CMD_INT_MASK, kFrameEnd);
    write(DC_CMD_DISPLAY_COMMAND, kContinuousDisplay);
    write(DC_CMD_DISPLAY_POWER_CONTROL, kPowerAll);
    commit(kGeneralActReq | kAllWindows);
}

void TegraDisplayB::enableSor(bool on)
{
    const uint32_t value = on ? kSor1Enable : 0u;
    write(DC_CMD_STATE_ACCESS, kStateAccessReadActive);
    write(DC_DISP_DISP_WIN_OPTIONS, value);
    write(DC_CMD_STATE_ACCESS, kStateAccessReadActive | kStateAccessWriteActive);
    write(DC_DISP_DISP_WIN_OPTIONS, value);
    write(DC_CMD_STATE_ACCESS, kStateAccessReadActive);
}

void TegraDisplayB::reserveMemory(const FrameMemory &next)
{
    if (sameMemoryPlan(memory, next.windows, next.peakBytes))
        return;
    memory.peak_bytes_per_second = next.peakBytes;
    for (unsigned w = 0; w < kWindows; ++w)
        memory.windows[w] = next.windows[w];
    if (const int err = switch_emc_display_memory_reserve(&memory); err == 0) {
        emcRefusedLogged = false;
    } else if (!emcRefusedLogged) {
        emcRefusedLogged = true;
        IOLog("SwitchDisplayPort: DCB memory plan refused (%d); continuing without EMC reservation\n", err);
    }
}

// The window's assembly registers for `next`, the window selected. Only what differs from the assembly copy is written,
// the start addresses always, the options last.
void TegraDisplayB::writeWindow(unsigned window, const switch_external::WindowRegisters &next)
{
    WindowCache &cache = windows[window];
    auto &old = cache.written;
    if (!next.on) {
        write(DC_WIN_WIN_OPTIONS, 0);
        old.on = false;
        old.options = 0;
        return;
    }
    if (next.csc) {
        for (uint32_t i = 0; i < 8u; ++i)
            write(DC_WINC_CSC_YOF + i, next.cscWords[i]);
        csc[window][0] = next.matrix;
        csc[window][1] = next.fullRange;
    }
    const bool all = !cache.valid;
    const auto put = [&](uint32_t word, uint32_t value, uint32_t was) {
        if (all || value != was)
            write(word, value);
    };
    put(DC_WIN_COLOR_DEPTH, next.colorDepth, old.colorDepth);
    put(DC_WIN_BYTE_SWAP, next.byteSwap, old.byteSwap);
    put(DC_WIN_POSITION, next.position, old.position);
    put(DC_WIN_SIZE, next.size, old.size);
    put(DC_WIN_PRESCALED_SIZE, next.prescaled, old.prescaled);
    put(DC_WIN_H_INITIAL_DDA, next.hInitial, old.hInitial);
    put(DC_WIN_V_INITIAL_DDA, next.vInitial, old.vInitial);
    put(DC_WIN_DDA_INC, next.dda, old.dda);
    put(DC_WIN_LINE_STRIDE, next.lineStride, old.lineStride);
    put(DC_WIN_BLEND_LAYER_CONTROL, next.blend, old.blend);
    put(DC_WIN_BLEND_MATCH_SELECT, next.blendSelect, old.blendSelect);
    put(DC_WIN_BLEND_NOMATCH_SELECT, next.blendSelect, old.blendSelect);
    put(DC_WINBUF_ADDR_H_OFFSET, next.hOffset, old.hOffset);
    put(DC_WINBUF_ADDR_V_OFFSET, next.vOffset, old.vOffset);
    put(DC_WINBUF_SURFACE_KIND, next.kind, old.kind);
    put(DC_WINBUF_START_ADDR_U, next.startU, old.startU);
    write(DC_WINBUF_START_ADDR, next.start);
    put(DC_WIN_WIN_OPTIONS, next.options, old.options);
    old = next;
    cache.valid = true;
}

void TegraDisplayB::present(const Frame &frame)
{
    for (unsigned w = 0; w < kWindows; ++w)
        if (frame.window[w].csc && windowState(w).live)
            panic("SwitchDisplayPort: DCB window %c's CSC written while it may be scanning (matrix %d range %d)",
                  'A' + w, csc[w][0], csc[w][1]);
    FrameMemory nextMem = {};
    for (unsigned w = 0; w < kWindows; ++w)
        nextMem.windows[w] = frame.memory[w];
    nextMem.peakBytes = frame.peakBytes;
    if (!sameFrameMemory(nextMem.windows, nextMem.peakBytes, latchedMemory.windows, latchedMemory.peakBytes) ||
        !sameMemoryPlan(memory, nextMem.windows, nextMem.peakBytes)) {
        FrameMemory both = nextMem;
        for (unsigned w = 0; w < kWindows; ++w)
            switch_display::mergeMemoryWindow(both.windows[w], latchedMemory.windows[w]);
        if (latchedMemory.peakBytes > both.peakBytes)
            both.peakBytes = latchedMemory.peakBytes;
        reserveMemory(both);
        memoryPending = !sameMemoryPlan(memory, nextMem.windows, nextMem.peakBytes);
    } else {
        memoryPending = false;
    }
    uint32_t request = 0;
    for (unsigned w = 0; w < kWindows; ++w) {
        const auto &next = frame.window[w];
        WindowCache &cache = windows[w];
        cache.expectOn = next.on;
        cache.expectStart = next.start;
        if (!next.on && !cache.live && !cache.written.on)
            continue;
        select(w);
        writeWindow(w, next);
        request |= actRequest(w);
    }
    requested = request;
    requestedMemory = nextMem;
    if (request)
        commit(request);
    frameInterrupt(true);
}

// Nothing writes a window while its request is out, so a boundary that took the request shows what it named.
bool TegraDisplayB::latch()
{
    if (requested & read(DC_CMD_STATE_CONTROL))
        return false;
    for (unsigned w = 0; w < kWindows; ++w) {
        if (!(requested & actRequest(w)))
            continue;
        select(w);
        WindowCache &cache = windows[w];
        if (cache.expectOn) {
            const uint32_t start = read(DC_WINBUF_START_ADDR);
            if (start != cache.expectStart)
                panic("SwitchDisplayPort: DCB window %c scans %#x after its request latched, not %#x", 'A' + w, start,
                      cache.expectStart);
        } else if (const uint32_t options = read(DC_WIN_WIN_OPTIONS); options & kWinEnable) {
            panic("SwitchDisplayPort: DCB window %c still on (options %#x) after its request to turn off latched",
                  'A' + w, options);
        }
        cache.live = cache.expectOn;
    }
    requested = 0;
    latchedMemory = requestedMemory;
    if (memoryPending) {
        memoryPending = false;
        reserveMemory(latchedMemory);
    }
    return true;
}

switch_external::WindowState TegraDisplayB::windowState(unsigned window) const
{
    const WindowCache &cache = windows[window];
    const bool requestedOn = (requested & actRequest(window)) && cache.expectOn;
    return {cache.live || requestedOn, csc[window][0], csc[window][1]};
}

void TegraDisplayB::windowsOff()
{
    for (unsigned w = 0; w < kWindows; ++w) {
        select(w);
        writeWindow(w, {});
        windows[w].expectOn = false;
    }
    requested = kAllWindows;
    requestedMemory = {};
    memoryPending = true;
    commit(kAllWindows);
}

void TegraDisplayB::releaseBandwidth()
{
    if (!memory.peak_bytes_per_second)
        return;
    reserveMemory({});
}

void TegraDisplayB::frameInterrupt(bool enable)
{
    if (enable == frameInterruptEnabled)
        return;
    frameInterruptEnabled = enable;
    if (enable) {
        write(DC_CMD_INT_STATUS, kFrameEnd);
        write(DC_CMD_INT_ENABLE, kFrameEnd);
    } else {
        write(DC_CMD_INT_ENABLE, 0);
        write(DC_CMD_INT_STATUS, kFrameEnd);
        (void)read(DC_CMD_INT_STATUS);
    }
}

uint32_t TegraDisplayB::takeInterrupt()
{
    const uint32_t cause = read(DC_CMD_INT_STATUS);
    write(DC_CMD_INT_STATUS, cause);
    (void)read(DC_CMD_INT_STATUS);
    return cause;
}

void TegraDisplayB::powerDown()
{
    if (base == nullptr || !switch_car_clock_enabled(kClockDisp2)) {
        return;
    }
    write(DC_CMD_INT_ENABLE, 0);
    frameInterruptEnabled = false;
    forgetWindows();
    emcRefusedLogged = false;
    write(DC_CMD_INT_MASK, 0);
    write(DC_CMD_STATE_ACCESS, kStateAccessReadActive | kStateAccessWriteActive);
    write(DC_DISP_DISP_WIN_OPTIONS, 0);
    write(DC_CMD_DISPLAY_COMMAND, 0);
    write(DC_CMD_DISPLAY_POWER_CONTROL, 0);
    write(DC_CMD_STATE_ACCESS, kStateAccessReadActive);
#if DEVELOPMENT || DEBUG
    IOLog("SwitchDisplayPort: DCB UFLOW_STATUS at stop: window A %#x, B %#x, C %#x\n", read(DC_WINBUF_AD_UFLOW_STATUS),
          read(DC_WINBUF_AD_UFLOW_STATUS + 0x200u), read(DC_WINBUF_AD_UFLOW_STATUS + 0x400u));
#endif
    tegra_mc_hot_reset(kMcHotResetControl, kMcHotResetDcb, true);
    uint32_t settled = 0;
    for (uint32_t i = 0; i < 1000 && settled < 5; ++i) {
        if (!(tegra_smmu_read(kMcHotResetControl) & kMcHotResetDcb)) {
            IOLog("SwitchDisplayPort: DCB MC hot reset request cleared during flush\n");
            break;
        }
        settled = (tegra_smmu_read(kMcHotResetStatus) & kMcHotResetDcb) ? settled + 1 : 0;
        if (settled < 5)
            IODelay(10);
    }
    if (settled < 5)
        IOLog("SwitchDisplayPort: DCB MC hot reset flush timed out (STATUS %#x)\n", tegra_smmu_read(kMcHotResetStatus));
    switch_car_reset_assert(kClockDisp2);
    releaseBandwidth();
    switch_car_clock_disable(kClockDisp2);
    (void)switch_pmc_partition_off(kPartitionDisb);
}
