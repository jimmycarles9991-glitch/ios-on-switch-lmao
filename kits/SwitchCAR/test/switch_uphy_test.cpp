// kits/SwitchCAR/SwitchUPHY.h on the host: the listener runs on the announcing thread with the new state already
// visible (a lane user that reads it under its own lane lock sees "not ready" once the clamp has run), "lost" calls
// nobody, and a listener that comes after "ready" still reads it.
#include "kits/SwitchCAR/SwitchUPHY.h"

#include <gtest/gtest.h>

#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

namespace {

struct Heard {
    std::vector<bool> calls;
    std::vector<bool> seen;  // switch_uphy_ready() inside the call
};

void hear(void *context, bool ready)
{
    auto *heard = static_cast<Heard *>(context);
    heard->calls.push_back(ready);
    heard->seen.push_back(switch_uphy_ready());
}

TEST(SwitchUphy, ListenerSeesTheNewState)
{
    Heard heard;
    switch_uphy_listen(&hear, &heard);
    switch_uphy_announce(false);
    switch_uphy_announce(true);
    EXPECT_EQ(heard.calls, (std::vector<bool>{false, true}));
    EXPECT_EQ(heard.seen, heard.calls);
    EXPECT_TRUE(switch_uphy_ready());
    switch_uphy_lost();
    EXPECT_FALSE(switch_uphy_ready());
    EXPECT_EQ(heard.calls.size(), 2u) << "SC7 calls no listener";
    switch_uphy_listen(nullptr, nullptr);
}

TEST(SwitchUphy, ReadyBeforeTheListener)
{
    switch_uphy_listen(nullptr, nullptr);
    switch_uphy_announce(true);
    Heard heard;
    switch_uphy_listen(&hear, &heard);
    EXPECT_TRUE(switch_uphy_ready());
    EXPECT_TRUE(heard.calls.empty());
    switch_uphy_listen(nullptr, nullptr);
}

// The lane protocol under contention: a role thread takes the lane up under its lock only while the PLL is announced
// ready; PCIe announces "not ready", the listener clamps under the same lock, then PCIe touches the PLL. The PLL is never
// touched with the lane up.
TEST(SwitchUphy, NoCalibrationUnderALiveLane)
{
    struct Lane {
        std::mutex lock;
        bool up = false;
        std::atomic<bool> pllBusy{false};
        std::atomic<int> violations{0};
    } lane;

    switch_uphy_listen(
        [](void *context, bool ready) {
            auto *l = static_cast<Lane *>(context);
            if (!ready) {
                std::lock_guard<std::mutex> guard(l->lock);
                l->up = false;
            }
        },
        &lane);
    switch_uphy_announce(true);
    std::atomic<bool> stop{false};
    std::thread role([&] {
        while (!stop.load()) {
            {
                std::lock_guard<std::mutex> guard(lane.lock);
                if (switch_uphy_ready()) {
                    lane.up = true;
                    if (lane.pllBusy.load())
                        ++lane.violations;
                }
            }
            std::this_thread::yield();
        }
    });
    for (int i = 0; i < 2000; ++i) {
        switch_uphy_announce(false);
        lane.pllBusy = true;
        {
            std::lock_guard<std::mutex> guard(lane.lock);
            if (lane.up)
                ++lane.violations;
        }
        lane.pllBusy = false;
        switch_uphy_announce(true);
    }
    stop = true;
    role.join();
    EXPECT_EQ(lane.violations.load(), 0);
    switch_uphy_listen(nullptr, nullptr);
}

}  // namespace
