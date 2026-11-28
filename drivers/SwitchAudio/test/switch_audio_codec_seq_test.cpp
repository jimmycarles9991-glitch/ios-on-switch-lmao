// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "../SwitchCodecSeq.h"

namespace {

using namespace switch_audio;
using testing::ElementsAre;

struct FakeCodecHw {
    std::vector<std::string> log;
    uint32_t armedMs = 0;
    bool timerArmed = false;
    uint32_t biasReadyCalls = 0;
    uint32_t offCalls = 0;
    std::vector<rt5639::Route> readyRoutes;

    void record(const char *ev)
    {
        log.emplace_back(ev);
    }

    void biasStandbyBegin()
    {
        record("bias_begin");
    }

    void biasStandbyFinish()
    {
        record("bias_finish");
    }

    void biasOff()
    {
        record("bias_off");
    }

    void powerDac()
    {
        record("power_dac");
    }

    void speakerRouteUpBegin()
    {
        record("spk_up_begin");
    }

    void speakerRouteUpClassD()
    {
        record("spk_up_clsd");
    }

    void speakerRouteUpFinish()
    {
        record("spk_up_finish");
    }

    void speakerRouteDownEarly()
    {
        record("spk_down_early");
    }

    void speakerRouteDownBegin()
    {
        record("spk_down_begin");
    }

    void speakerRouteDownFinish()
    {
        record("spk_down_finish");
    }

    void headphoneRouteUpBegin()
    {
        record("hp_up_begin");
    }

    void headphoneRouteUpDepop()
    {
        record("hp_up_depop");
    }

    void headphoneRouteUpFinish()
    {
        record("hp_up_finish");
    }

    void headphoneRouteDownEarly()
    {
        record("hp_down_early");
    }

    void headphoneRouteDownBegin()
    {
        record("hp_down_begin");
    }

    void headphoneRouteDownFinish()
    {
        record("hp_down_finish");
    }

    struct Host {
        FakeCodecHw *hw;

        void armTimer(uint32_t ms) const
        {
            hw->timerArmed = true;
            hw->armedMs = ms;
            hw->log.push_back("arm:" + std::to_string(ms));
        }

        void cancelTimer() const
        {
            hw->timerArmed = false;
            hw->armedMs = 0;
            hw->record("cancel");
        }

        void onBiasReady() const
        {
            ++hw->biasReadyCalls;
            hw->record("on_bias_ready");
        }

        void onRouteReady(rt5639::Route route) const
        {
            hw->readyRoutes.push_back(route);
            hw->record(route == rt5639::kRouteSpeaker ? "on_route_spk" : "on_route_hp");
        }

        void onOff() const
        {
            ++hw->offCalls;
            hw->record("on_off");
        }
    };
};

class CodecSeqTest : public testing::Test {
protected:
    FakeCodecHw hw_;
    SwitchCodecSeq<FakeCodecHw, FakeCodecHw::Host> seq_{&hw_, FakeCodecHw::Host{&hw_}};

    void fireTimer()
    {
        ASSERT_TRUE(hw_.timerArmed);
        hw_.timerArmed = false;
        hw_.armedMs = 0;
        seq_.timerFired();
    }
};

TEST_F(CodecSeqTest, ColdSpeakerPowerUpRunsExactTimerSequence)
{
    seq_.setTarget(true, true, rt5639::kRouteSpeaker);
    seq_.reconcile();
    EXPECT_EQ(seq_.phase(), CodecPhase::kBiasUpWait);
    EXPECT_EQ(hw_.armedMs, kCodecBiasStandbyMS);
    EXPECT_FALSE(seq_.isRouteSettled());

    fireTimer();
    EXPECT_EQ(seq_.phase(), CodecPhase::kSpkUpWaitClassD);
    EXPECT_EQ(hw_.armedMs, kCodecSpeakerBiasMS);
    EXPECT_EQ(hw_.biasReadyCalls, 1u);

    fireTimer();
    EXPECT_EQ(seq_.phase(), CodecPhase::kSpkUpWaitUnmute);
    EXPECT_EQ(hw_.armedMs, kCodecSpeakerClassDMS);

    fireTimer();
    EXPECT_EQ(seq_.phase(), CodecPhase::kRouteActive);
    EXPECT_TRUE(seq_.isRouteSettled());
    EXPECT_EQ(seq_.activeRoute(), rt5639::kRouteSpeaker);
    EXPECT_FALSE(hw_.timerArmed);
    EXPECT_THAT(hw_.log,
                ElementsAre("bias_begin", "arm:25", "bias_finish", "on_bias_ready", "power_dac", "spk_up_begin",
                            "arm:15", "spk_up_clsd", "arm:15", "spk_up_finish", "on_route_spk"));
}

TEST_F(CodecSeqTest, ColdHeadphonePowerUpRuns195MsDepopSequenceNonBlocking)
{
    seq_.setTarget(true, true, rt5639::kRouteHeadphones);
    seq_.reconcile();
    EXPECT_EQ(seq_.phase(), CodecPhase::kBiasUpWait);
    EXPECT_EQ(hw_.armedMs, 25u);

    fireTimer();
    EXPECT_EQ(seq_.phase(), CodecPhase::kHpUpWaitAmp);
    EXPECT_EQ(hw_.armedMs, 15u);

    fireTimer();
    EXPECT_EQ(seq_.phase(), CodecPhase::kHpUpWaitDepop);
    EXPECT_EQ(hw_.armedMs, 180u);

    fireTimer();
    EXPECT_EQ(seq_.phase(), CodecPhase::kRouteActive);
    EXPECT_TRUE(seq_.isRouteSettled());
    EXPECT_EQ(seq_.activeRoute(), rt5639::kRouteHeadphones);
    EXPECT_FALSE(hw_.timerArmed);
    EXPECT_THAT(hw_.log, ElementsAre("bias_begin", "arm:25", "bias_finish", "on_bias_ready", "power_dac", "hp_up_begin",
                                     "arm:15", "hp_up_depop", "arm:180", "hp_up_finish", "on_route_hp"));
}

TEST_F(CodecSeqTest, UnplugDuringHeadphoneDepopCancelsAndTransitionsCleanlyToSpeaker)
{
    seq_.setTarget(true, true, rt5639::kRouteHeadphones);
    seq_.reconcile();
    fireTimer();
    fireTimer();
    ASSERT_EQ(seq_.phase(), CodecPhase::kHpUpWaitDepop);
    hw_.log.clear();

    seq_.setTarget(true, true, rt5639::kRouteSpeaker);
    seq_.reconcile();
    EXPECT_EQ(seq_.phase(), CodecPhase::kHpDownWait);
    EXPECT_EQ(hw_.armedMs, kCodecHeadphoneDownMS);

    fireTimer();
    EXPECT_EQ(seq_.phase(), CodecPhase::kSpkUpWaitClassD);
    EXPECT_EQ(hw_.armedMs, kCodecSpeakerBiasMS);

    fireTimer();
    EXPECT_EQ(seq_.phase(), CodecPhase::kSpkUpWaitUnmute);
    fireTimer();
    EXPECT_TRUE(seq_.isRouteSettled());
    EXPECT_EQ(seq_.activeRoute(), rt5639::kRouteSpeaker);
    EXPECT_THAT(hw_.log, ElementsAre("cancel", "hp_down_begin", "arm:30", "hp_down_finish", "power_dac", "spk_up_begin",
                                     "arm:15", "spk_up_clsd", "arm:15", "spk_up_finish", "on_route_spk"));
}

TEST_F(CodecSeqTest, EarlyCancelDuringPreAmpStagesSkipsDischargeWait)
{
    seq_.setTarget(true, true, rt5639::kRouteSpeaker);
    seq_.reconcile();
    fireTimer();
    ASSERT_EQ(seq_.phase(), CodecPhase::kSpkUpWaitClassD);
    hw_.log.clear();

    seq_.setTarget(true, true, rt5639::kRouteHeadphones);
    seq_.reconcile();
    EXPECT_EQ(seq_.phase(), CodecPhase::kHpUpWaitAmp);
    EXPECT_THAT(hw_.log, ElementsAre("cancel", "spk_down_early", "power_dac", "hp_up_begin", "arm:15"));

    hw_.log.clear();
    seq_.setTarget(false, false, rt5639::kRouteNone);
    seq_.reconcile();
    EXPECT_EQ(seq_.phase(), CodecPhase::kBiasOffWait);
    EXPECT_EQ(hw_.armedMs, kCodecBiasOffMS);
    EXPECT_THAT(hw_.log, ElementsAre("cancel", "hp_down_early", "bias_off", "arm:5"));

    fireTimer();
    EXPECT_EQ(seq_.phase(), CodecPhase::kOff);
    EXPECT_EQ(hw_.offCalls, 1u);
}

TEST_F(CodecSeqTest, AbortDuringSpeakerClassDWaitStillDischargesClassD)
{
    seq_.setTarget(true, true, rt5639::kRouteSpeaker);
    seq_.reconcile();
    fireTimer();
    fireTimer();
    ASSERT_EQ(seq_.phase(), CodecPhase::kSpkUpWaitUnmute);
    hw_.log.clear();

    seq_.setTarget(false, false, rt5639::kRouteNone);
    seq_.reconcile();
    EXPECT_EQ(seq_.phase(), CodecPhase::kSpkDownWait);
    EXPECT_EQ(hw_.armedMs, kCodecSpeakerDownMS);

    fireTimer();
    EXPECT_EQ(seq_.phase(), CodecPhase::kBiasOffWait);
    EXPECT_EQ(hw_.armedMs, kCodecBiasOffMS);

    fireTimer();
    EXPECT_EQ(seq_.phase(), CodecPhase::kOff);
    EXPECT_EQ(hw_.offCalls, 1u);
    EXPECT_THAT(hw_.log,
                ElementsAre("cancel", "spk_down_begin", "arm:15", "spk_down_finish", "bias_off", "arm:5", "on_off"));
}

TEST_F(CodecSeqTest, StreamRestartDuringIdleBiasOffCancelsOffAndRePowersWithoutReleasingApe)
{
    seq_.setTarget(true, true, rt5639::kRouteSpeaker);
    seq_.reconcile();
    fireTimer();
    fireTimer();
    fireTimer();
    ASSERT_TRUE(seq_.isRouteSettled());

    seq_.setTarget(false, false, rt5639::kRouteNone);
    seq_.reconcile();
    ASSERT_EQ(seq_.phase(), CodecPhase::kSpkDownWait);
    fireTimer();
    ASSERT_EQ(seq_.phase(), CodecPhase::kBiasOffWait);
    hw_.log.clear();

    seq_.setTarget(true, true, rt5639::kRouteSpeaker);
    seq_.reconcile();
    EXPECT_EQ(seq_.phase(), CodecPhase::kBiasOffWait);
    EXPECT_TRUE(hw_.log.empty());

    fireTimer();
    EXPECT_EQ(hw_.offCalls, 0u);
    EXPECT_EQ(seq_.phase(), CodecPhase::kBiasUpWait);
    EXPECT_THAT(hw_.log, ElementsAre("bias_begin", "arm:25"));
}

TEST_F(CodecSeqTest, StreamRestartDuringRouteDownFinishesDischargeThenRampsUp)
{
    seq_.setTarget(true, true, rt5639::kRouteHeadphones);
    seq_.reconcile();
    fireTimer();
    fireTimer();
    fireTimer();
    ASSERT_TRUE(seq_.isRouteSettled());

    seq_.setTarget(false, false, rt5639::kRouteNone);
    seq_.reconcile();
    ASSERT_EQ(seq_.phase(), CodecPhase::kHpDownWait);
    hw_.log.clear();

    seq_.setTarget(true, true, rt5639::kRouteHeadphones);
    seq_.reconcile();
    EXPECT_EQ(seq_.phase(), CodecPhase::kHpDownWait);

    fireTimer();
    EXPECT_EQ(seq_.phase(), CodecPhase::kHpUpWaitAmp);
    EXPECT_THAT(hw_.log, ElementsAre("hp_down_finish", "power_dac", "hp_up_begin", "arm:15"));
}

TEST_F(CodecSeqTest, BiasOnlyBootPrechargeStaysAtBiasReadyAndCancelsCleanly)
{
    seq_.setTarget(true, false, rt5639::kRouteNone);
    seq_.reconcile();
    ASSERT_EQ(seq_.phase(), CodecPhase::kBiasUpWait);

    seq_.setTarget(false, false, rt5639::kRouteNone);
    seq_.reconcile();
    EXPECT_EQ(seq_.phase(), CodecPhase::kBiasOffWait);
    fireTimer();
    EXPECT_EQ(seq_.phase(), CodecPhase::kOff);
    EXPECT_EQ(hw_.offCalls, 1u);

    hw_.log.clear();
    seq_.timerFired();
    EXPECT_TRUE(hw_.log.empty());

    seq_.setTarget(true, false, rt5639::kRouteNone);
    seq_.reconcile();
    fireTimer();
    EXPECT_EQ(seq_.phase(), CodecPhase::kBiasReady);
    EXPECT_FALSE(hw_.timerArmed);
    EXPECT_EQ(hw_.biasReadyCalls, 1u);
    EXPECT_TRUE(hw_.readyRoutes.empty());
}

}  // namespace
