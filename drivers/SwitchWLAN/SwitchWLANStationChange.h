// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef SWITCH_WLAN_STATION_CHANGE_H
#define SWITCH_WLAN_STATION_CHANGE_H

namespace swlan {

class StationChanges {
public:
    StationChanges(int (*prepare)(), void (*resume)()) : prepare_(prepare), resume_(resume)
    {}

    int enter()
    {
        if (depth_) {
            ++depth_;
            return 0;
        }
        const int result = prepare_();
        if (!result)
            ++depth_;
        return result;
    }

    void leave()
    {
        if (depth_ && !--depth_)
            resume_();
    }

    void reset()
    {
        depth_ = 0;
    }

    class Scope {
    public:
        explicit Scope(StationChanges &state) : state_(state), error_(state.enter())
        {}

        Scope(const Scope &) = delete;
        Scope &operator=(const Scope &) = delete;

        ~Scope()
        {
            if (!error_)
                state_.leave();
        }

        explicit operator bool() const
        {
            return !error_;
        }

    private:
        StationChanges &state_;
        const int error_;
    };

private:
    int (*prepare_)();
    void (*resume_)();
    unsigned depth_ = 0;
};

}
#endif
