// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef SWITCH_WLAN_KEY_STATE_H
#define SWITCH_WLAN_KEY_STATE_H

namespace swlan::wpa {

class InstalledKeys {
public:
    void record(bool pairwise, bool accepted)
    {
        (pairwise ? pairwise_ : group_) = accepted;
        failed_ |= !accepted;
    }

    void fail()
    {
        failed_ = true;
    }

    bool failed() const
    {
        return failed_;
    }

    bool ready() const
    {
        return pairwise_ && group_ && !failed_;
    }

private:
    bool pairwise_ = false, group_ = false, failed_ = false;
};

}
#endif
