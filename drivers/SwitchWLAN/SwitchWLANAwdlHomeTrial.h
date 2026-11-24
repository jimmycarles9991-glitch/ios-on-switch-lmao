// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef SWITCH_WLAN_AWDL_HOME_TRIAL_H
#define SWITCH_WLAN_AWDL_HOME_TRIAL_H

#include <stdint.h>

namespace swlan {
struct SwitchAwdlHomeTrial {
    enum class Phase { Off, Armed, Waiting, Applying, Active };
    enum class Action { None, Request, Install, Start, Timeout, Finish };

    Phase phase = Phase::Off;
    uint64_t deadlineNs = 0;

    void arm()
    {
        phase = Phase::Armed;
        deadlineNs = 0;
    }

    void cancel()
    {
        phase = Phase::Off;
        deadlineNs = 0;
    }

    Action advance(uint64_t nowNs, bool enabled, bool committed, bool eligible, bool applied)
    {
        if (phase == Phase::Off)
            return Action::None;
        if (phase == Phase::Armed) {
            if (!enabled || !committed)
                return Action::None;
            phase = Phase::Waiting;
            deadlineNs = nowNs + 10000000000ULL;
            return Action::Request;
        }
        if (nowNs >= deadlineNs)
            return phase == Phase::Active ? Action::Finish : Action::Timeout;
        if (phase == Phase::Waiting && enabled && committed && eligible) {
            phase = Phase::Applying;
            return Action::Install;
        }
        if (phase == Phase::Applying && enabled && committed && applied) {
            phase = Phase::Active;
            deadlineNs = nowNs + 60000000000ULL;
            return Action::Start;
        }
        return Action::None;
    }
};
}
#endif
