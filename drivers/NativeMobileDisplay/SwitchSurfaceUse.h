// SPDX-License-Identifier: BSD-2-Clause-Patent
// IOSurface's use count, bound per IPSW by kits/SwitchABI (IOSurface's use_count_*): the count of the surface's shared
// entry that IOSurfaceIsInUse reads, as IOSurfaceIncrementUseCount moves it (category 0). A decoder's pool writes a
// buffer again only once it reads not in use.
#pragma once
#include <stdint.h>
#include "OriginalIOSurface.h"

class OSObject;

extern "C" void switch_surface_use_increment(OSObject *surface, uint32_t category);
extern "C" void switch_surface_use_decrement(OSObject *surface, uint32_t category);
extern "C" uint32_t switch_surface_use_count(OSObject *surface);

namespace switch_display {
constexpr uint8_t kProhibitUseCount = 1u << 5;

// kIOSurfaceProhibitUseCount: IOSurface logs and ignores every change of such a surface's count.
inline bool surfaceUseCounted(const OSObject *surface)
{
    return (reinterpret_cast<const uint8_t *>(surface)[OriginalIOSurface::useCountFlags] & kProhibitUseCount) == 0;
}
}  // namespace switch_display
