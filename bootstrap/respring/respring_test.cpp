// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "respring.h"

#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace {

struct SwitchRespringState {
    bool frontboard = true;
    bool finish_on_frontboard = true;
    bool finish_on_fallback = true;
    bool backboard_exits = false;
    bool springboard_exits_after_kickstart = false;
    int watch_error = 0;
    int kickstart_error = 0;
    unsigned frontboard_requests = 0;
    std::vector<unsigned> waits;
    std::vector<std::string> kickstarts;
};

SwitchRespringState switch_state;

int switch_watch(int queue, SwitchRespringServices &)
{
    EXPECT_EQ(queue, 7);
    return switch_state.watch_error;
}

bool switch_frontboard()
{
    ++switch_state.frontboard_requests;
    return switch_state.frontboard;
}

int switch_wait(int queue, SwitchRespringServices &services, unsigned seconds)
{
    EXPECT_EQ(queue, 7);
    switch_state.waits.push_back(seconds);
    if (seconds == 3 && switch_state.backboard_exits)
        services[0].pid = 0;
    if (seconds == 0 && switch_state.springboard_exits_after_kickstart && !switch_state.kickstarts.empty()) {
        services[0].pid = 0;
        services[1].pid = 0;
    }
    if ((seconds == 3 && switch_state.finish_on_frontboard) || (seconds == 5 && switch_state.finish_on_fallback)) {
        services[0].pid = 0;
        services[1].pid = 0;
    }
    return services[0].pid || services[1].pid ? ETIMEDOUT : 0;
}

int switch_kickstart(const char *job)
{
    switch_state.kickstarts.emplace_back(job);
    return switch_state.kickstart_error;
}

class SwitchRespringTest : public testing::Test {
  protected:
    void SetUp() override { switch_state = SwitchRespringState{}; }

    int restart() { return switch_respring(7, services, operations); }

    SwitchRespringServices services = {{
        {"/usr/libexec/backboardd", "com.apple.backboardd", 23},
        {"/System/Library/CoreServices/SpringBoard.app/SpringBoard", "com.apple.SpringBoard", 45},
    }};
    const SwitchRespringOperations operations = {switch_watch, switch_frontboard, switch_wait, switch_kickstart};
};

TEST_F(SwitchRespringTest, FrontBoardSuccessNeedsObservedExit)
{
    EXPECT_EQ(restart(), 0);
    EXPECT_EQ(switch_state.frontboard_requests, 1u);
    EXPECT_EQ(switch_state.waits, (std::vector<unsigned>{3}));
    EXPECT_TRUE(switch_state.kickstarts.empty());
}

TEST_F(SwitchRespringTest, FallbackRestartsOnlyServicesThatRemain)
{
    switch_state.finish_on_frontboard = false;
    switch_state.backboard_exits = true;
    EXPECT_EQ(restart(), 0);
    EXPECT_EQ(switch_state.kickstarts, (std::vector<std::string>{"com.apple.SpringBoard"}));
    EXPECT_EQ(switch_state.waits, (std::vector<unsigned>{3, 0, 0, 5}));
}

TEST_F(SwitchRespringTest, FallbackDrainsRelatedServiceExitBeforeSecondKickstart)
{
    switch_state.frontboard = false;
    switch_state.springboard_exits_after_kickstart = true;
    EXPECT_EQ(restart(), 0);
    EXPECT_EQ(switch_state.kickstarts, (std::vector<std::string>{"com.apple.backboardd"}));
}

TEST_F(SwitchRespringTest, ReportsUnobservableServicesAndFallbackErrors)
{
    services[1].pid = 0;
    EXPECT_EQ(restart(), ENOENT);
    EXPECT_EQ(switch_state.frontboard_requests, 0u);

    services[0].pid = 0;
    EXPECT_EQ(restart(), ENOENT);
    EXPECT_EQ(switch_state.frontboard_requests, 0u);

    services[1].pid = 45;
    switch_state.watch_error = EPERM;
    EXPECT_EQ(restart(), EPERM);
    EXPECT_EQ(switch_state.frontboard_requests, 0u);

    switch_state.watch_error = 0;
    switch_state.frontboard = false;
    switch_state.kickstart_error = EIO;
    EXPECT_EQ(restart(), EIO);
    EXPECT_EQ(switch_state.kickstarts, (std::vector<std::string>{"com.apple.SpringBoard"}));

    switch_state.kickstart_error = 0;
    switch_state.finish_on_fallback = false;
    EXPECT_EQ(restart(), ETIMEDOUT);
}

} // namespace
