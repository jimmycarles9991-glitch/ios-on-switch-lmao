// SPDX-License-Identifier: BSD-2-Clause-Patent
// Which of DC A's windows carries what. Portrait (every composition so far): the UI on window A, the two overlays on B
// and C. Landscape (SwitchLandscape.h, behind V3): the UI on B in front, the video column-scanned on A behind it (only
// window A rotates planar formats on T210), the second overlay on C. The mode belongs to the latched composition, so
// every active-register read, ACT_REQ test and underflow bit follows what DC A scans, not what was last written.
#pragma once
#include <stdint.h>

namespace switch_landscape {

enum Window : unsigned {
    kA = 0,
    kB = 1,
    kC = 2
};

struct Windows {
    Window ui;
    Window overlay[2];
};

constexpr Windows kPortraitWindows = {kA, {kB, kC}};
constexpr Windows kLandscapeWindows = {kB, {kA, kC}};

// WIN_OPTIONS SCAN_COLUMN: each output line is a source column (NativeColumnScan.h).
constexpr uint32_t kScanColumn = 1u << 4;

constexpr const Windows &windowsOf(bool landscape)
{
    return landscape ? kLandscapeWindows : kPortraitWindows;
}

// Each window's own register apertures (mainline dc.c tegra_plane_offset: 0xa00 + 0x200 * index, DC_WIN 0x700 + i at
// + 0x180 + i, DC_WINBUF 0x800 + i at + 0x1c0 + i), and its bits in STATE_CONTROL, INT_STATUS and WINDOW_HEADER.
constexpr uint32_t controlBase(Window w)
{
    return 0xa00 + 0x200 * w;
}

constexpr uint32_t windowBase(Window w)
{
    return 0xb80 + 0x200 * w;
}

constexpr uint32_t bufferBase(Window w)
{
    return 0xbc0 + 0x200 * w;
}

constexpr uint32_t actRequest(Window w)
{
    return 1u << (1 + w);
}

constexpr uint32_t update(Window w)
{
    return 1u << (9 + w);
}

constexpr uint32_t underflow(Window w)
{
    return 1u << (8 + w);
}

constexpr uint32_t headerSelect(Window w)
{
    return 1u << (4 + w);
}

constexpr uint32_t overlayRequests(bool landscape)
{
    return actRequest(windowsOf(landscape).overlay[0]) | actRequest(windowsOf(landscape).overlay[1]);
}

constexpr uint32_t overlayUpdates(bool landscape)
{
    return update(windowsOf(landscape).overlay[0]) | update(windowsOf(landscape).overlay[1]);
}

constexpr uint32_t overlayUnderflows(bool landscape)
{
    return underflow(windowsOf(landscape).overlay[0]) | underflow(windowsOf(landscape).overlay[1]);
}

constexpr char nameOf(Window w)
{
    return static_cast<char>('A' + w);
}

static_assert(controlBase(kB) == 0xc00 && windowBase(kB) == 0xd80 && bufferBase(kB) == 0xdc0, "window B");
static_assert(controlBase(kC) == 0xe00 && windowBase(kC) == 0xf80 && bufferBase(kC) == 0xfc0, "window C");
static_assert(overlayRequests(false) == (1u << 2 | 1u << 3) && overlayUnderflows(false) == (1u << 9 | 1u << 10),
              "portrait overlays are B and C");

}  // namespace switch_landscape
