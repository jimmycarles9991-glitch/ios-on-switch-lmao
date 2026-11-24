// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef SWITCH_WLAN_RECOVERY_H
#define SWITCH_WLAN_RECOVERY_H

#include <stdint.h>

namespace swlan {
class SwitchRadioRecovery {
public:
    void request()
    {
        requested_ = true;
    }

    void reset()
    {
        requested_ = false;
    }

    bool requested() const
    {
        return requested_;
    }

    bool canSubmit(bool up, bool gone, bool halted) const
    {
        return canService(up, gone, halted) && !requested_;
    }

    static bool canService(bool up, bool gone, bool halted)
    {
        return up && !gone && !halted;
    }

    static bool canReset(bool ioctlBusy)
    {
        return !ioctlBusy;
    }

    bool watchdogDue(bool up, bool gone, bool halted, uint64_t now, uint64_t scheduled) const
    {
        return up && (gone || halted || requested_ || now >= scheduled);
    }

private:
    bool requested_ = false;
};
}
#endif
