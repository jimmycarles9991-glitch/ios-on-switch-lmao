// SPDX-License-Identifier: BSD-2-Clause-Patent
// The framebuffer runtime property enableSBIM. iOS 27's libEDR (EDRServerEnableSBIM) sets it on the main display's
// framebuffer service, as a boolean, while EDR is engaged. AppleMobileDispH12P, J181's display driver (24B119), answers
// as follows when its display has no SBIM statistics: enabling returns kIOReturnUnsupported, and libEDR then clears
// its own SBIM flag and stops asking; disabling succeeds. DC A has no such statistics. IOMobileFramebufferLegacy's own
// setProperties takes four keys and answers kIOReturnBadArgument to any other, which libEDR retries at each engagement.
#pragma once
#include <IOKit/IOReturn.h>

namespace switch_sbim {

constexpr const char *kEnableKey = "enableSBIM";

// A setProperties dictionary of `count` keys whose enableSBIM value is `value` (-1: absent or not a boolean, else 0 or
// 1). False: not this property's dictionary, for the family to answer.
inline bool answer(unsigned count, int value, IOReturn &out)
{
    if (count != 1 || value < 0)
        return false;
    out = value ? kIOReturnUnsupported : kIOReturnSuccess;
    return true;
}

}  // namespace switch_sbim
