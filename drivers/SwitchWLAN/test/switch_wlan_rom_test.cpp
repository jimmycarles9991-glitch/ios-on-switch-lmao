// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "../SwitchWLANRomState.h"
#include <atomic>
#include <thread>
#include <gtest/gtest.h>

TEST(SwitchWlanRom, RetainsCaptureUntilDataIsMountedAndClaimsItOnce)
{
    int capture = 7;
    switch_wlan_rom_state state = {};
    EXPECT_TRUE(switch_wlan_rom_publish(&state, &capture));
    int duplicate = 9;
    EXPECT_FALSE(switch_wlan_rom_publish(&state, &duplicate));
    EXPECT_EQ(switch_wlan_rom_take(&state), nullptr);
    EXPECT_FALSE(switch_wlan_rom_publish(&state, &duplicate));
    __atomic_store_n(&state.switch_mounted, true, __ATOMIC_SEQ_CST);
    EXPECT_EQ(switch_wlan_rom_take(&state), &capture);
    EXPECT_EQ(switch_wlan_rom_take(&state), nullptr);
    switch_wlan_rom_state mountedFirst = {};
    __atomic_store_n(&mountedFirst.switch_mounted, true, __ATOMIC_SEQ_CST);
    EXPECT_EQ(switch_wlan_rom_take(&mountedFirst), nullptr);
    EXPECT_TRUE(switch_wlan_rom_publish(&mountedFirst, &capture));
    EXPECT_EQ(switch_wlan_rom_take(&mountedFirst), &capture);
}

TEST(SwitchWlanRom, CaptureAndMountRacingPublishOnlyOneWriter)
{
    for (unsigned i = 0; i < 100; ++i) {
        int capture = 7;
        switch_wlan_rom_state state = {};
        std::atomic<unsigned> writers = 0;
        std::thread mounted([&] {
            __atomic_store_n(&state.switch_mounted, true, __ATOMIC_SEQ_CST);
            if (switch_wlan_rom_take(&state))
                writers.fetch_add(1);
        });
        std::thread captured([&] {
            EXPECT_TRUE(switch_wlan_rom_publish(&state, &capture));
            if (switch_wlan_rom_take(&state))
                writers.fetch_add(1);
        });
        mounted.join();
        captured.join();
        EXPECT_EQ(writers.load(), 1u);
        EXPECT_EQ(switch_wlan_rom_take(&state), nullptr);
    }
}
