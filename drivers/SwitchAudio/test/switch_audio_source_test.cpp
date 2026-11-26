// SwitchAudio's driver and plug-in invariants: clean integer/even PLLA clock tree across all 10 SFC rates,
// SwitchAudioFormat.h single-source register words, ASoC unmute ordering, PZ4 power sequencing, S32 ABI v9,
// hybrid headphone staging, and isolated capture path.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <iterator>
#include <regex>
#include <string>
#include <type_traits>

#include "../SwitchAudioAbi.h"
#include "../SwitchAudioFormat.h"
#include "bazel/testing/source_text.h"
#include "switch_audio_format_harness.h"
#include "switch_audio_words.h"

namespace {

using switch_audio_linux::Fraction;
using switch_ios::test::Between;
using switch_ios::test::SourceText;
using testing::HasSubstr;
using testing::Not;

std::string Driver()
{
    std::string text;
    for (const char *file :
         {"../SwitchAudioRegisters.h", "../SwitchAudioDriver.h", "../SwitchAudioAhub.h", "../SwitchAudio.cpp",
          "../SwitchAudioClocks.cpp", "../SwitchAudioStream.cpp", "../SwitchAudioCodec.cpp", "../SwitchAudioSfc.cpp",
          "../SwitchAudioMixer.cpp", "../SwitchAudioMvc.cpp", "../SwitchAudioOpe.cpp"})
        text += SourceText(file);
    return text;
}

static_assert(SWITCH_AUDIO_VERSION == 9u);
static_assert(SWITCH_AUDIO_MAX_RATES == 10u);
static_assert(std::is_same_v<switch_audio_sample_t, int32_t>);
static_assert(SWITCH_AUDIO_RING_FRAMES * switch_audio_linux::kChannels * sizeof(switch_audio_sample_t) == 512 * 1024);
static_assert(SWITCH_AUDIO_IN_RING_FRAMES * sizeof(switch_audio_sample_t) == 64 * 1024);

struct ClockFamily {
    uint32_t index;
    Fraction plla;
    uint32_t i2sDivider, extern1Divider;
    double hz;
};

// Both link families from the 38.4 MHz oscillator: PLLA_OUT0 = PLLA / 7.5 = 1024 fs (49.152 / 45.1584 MHz), MCLK =
// PLLA_OUT0 / 4 (even 50% duty), BCLK = PLLA_OUT0 / 16 (even 50% duty), < 1 ppm frequency error, and exact Q32 ticks.
TEST(SwitchAudioSource, ClockFamiliesDivideToTheirRates)
{
    for (const ClockFamily &family : {ClockFamily{0, Fraction(2949121875, 8), 30, 6, 48000.031},
                                      ClockFamily{1, Fraction(1354753125, 4), 30, 6, 44100.037}}) {
        uint32_t f[7];
        neon_h_clock(family.index, f);
        const uint32_t rate = f[0], m = f[1], p = f[3], out0x2 = f[5], perFs = f[6];
        const uint64_t nEff = neon_h_clock_n_eff(family.index);
        const Fraction plla(38400000 * (__int128)nEff, 8192 * (__int128)m * p);
        EXPECT_TRUE(plla == family.plla) << rate;
        const Fraction pllaOut0 = plla * Fraction(2) / Fraction(out0x2);
        const uint32_t i2s = neon_h_i2s_divider(perFs), extern1 = neon_h_extern1_divider(perFs);
        EXPECT_EQ(i2s, family.i2sDivider) << rate;
        EXPECT_EQ(extern1, family.extern1Divider) << rate;
        const Fraction bclk = pllaOut0 / (Fraction(i2s, 2) + Fraction(1));
        const Fraction mclk = pllaOut0 / (Fraction(extern1, 2) + Fraction(1));
        const Fraction fs = bclk / Fraction(neon_h_bclk_fs());
        EXPECT_TRUE(mclk == fs * Fraction(256)) << rate;
        EXPECT_TRUE(pllaOut0 == fs * Fraction(perFs)) << rate;
        EXPECT_EQ(std::round((fs.value() - family.hz) * 1000), 0.0) << fs.value();
        EXPECT_LT(std::fabs(fs.value() - rate) / rate, 1e-6) << rate;
        EXPECT_EQ(neon_h_clock_millihz(38400000, family.index), (uint64_t)(fs * Fraction(1000)).floor());
        for (uint64_t hostHz : {19200000ull, 24000000ull}) {
            const uint64_t q32 = neon_h_clock_ticks_per_frame_q32(hostHz, 38400000, family.index);
            EXPECT_EQ(q32, (uint64_t)(Fraction((__int128)hostHz) / fs * Fraction((__int128)1 << 32)).floor()) << hostHz;
        }
    }

    constexpr uint32_t kRates[] = {48000u, 44100u, 96000u, 88200u, 32000u, 24000u, 22050u, 16000u, 11025u, 8000u};
    for (uint32_t clientRate : kRates) {
        const uint32_t idx = neon_h_is_44k_family(clientRate) ? 1u : 0u;
        uint32_t f[7];
        neon_h_clock(idx, f);
        const Fraction plla(38400000 * (__int128)neon_h_clock_n_eff(idx), 8192 * (__int128)f[1] * f[3]);
        const Fraction linkFs = plla * Fraction(2) / Fraction(f[5]) / Fraction(f[6]);
        const Fraction clientFs = linkFs * Fraction(clientRate, f[0]);
        for (uint64_t hostHz : {19200000ull, 24000000ull}) {
            const uint64_t got = neon_h_client_ticks_per_frame_q32(hostHz, 38400000, idx, clientRate);
            const uint64_t expect =
                (uint64_t)(Fraction((__int128)hostHz) / clientFs * Fraction((__int128)1 << 32)).floor();
            EXPECT_EQ(got, expect) << "rate=" << clientRate << " hostHz=" << hostHz;
        }
    }
}

TEST(SwitchAudioSource, DriverRunsTheClockTableThroughSwitchCar)
{
    const std::string driver = Driver();
    for (const char *line : {
             "constexpr switch_audio_clock kClocks[2] = {SWITCH_AUDIO_CLOCK_48000, SWITCH_AUDIO_CLOCK_44100};",
             "switch_car_write(CLK_SOURCE_I2S0, switch_audio_i2s_divider(clock->out0_per_fs));",
             "switch_car_write(CLK_SOURCE_EXTERN1, switch_audio_extern1_divider(clock->out0_per_fs));",
             "const uint32_t out0Div = clock->out0_x2 - 2;",
             "return switch_audio_client_ticks_per_frame_q32(hz, refHz, *clock, clientRate);",
             "tegra_mc_hot_reset(kMcHotResetCtrl1, kMcHotResetApe,",
         })
        EXPECT_THAT(driver, HasSubstr(line));
    for (const char *gone : {"kPllaM", "kPllaN =", "kPllaSdm", "kI2sDiv", "kExtern1Div", "kWatchdogMS"})
        EXPECT_THAT(driver, Not(HasSubstr(gone)));
    const std::string setRate = Between(driver, "IOReturn SwitchAudio::setRate(uint32_t rate)", "\n}\n");
    const size_t idle = setRate.find("codecStep(kCodecIdleNow)"), change = setRate.find("&SwitchAudio::rateGated");
    ASSERT_NE(idle, std::string::npos);
    ASSERT_NE(change, std::string::npos);
    EXPECT_LT(idle, change);
    EXPECT_THAT(Between(driver, "IOReturn SwitchAudio::rateGated(", "\n}\n"), HasSubstr("if (self->control->state) {"));
}

TEST(SwitchAudioSource, DriverProgramsTheHeaderWords)
{
    const std::string d = Driver();
    for (const char *line : {
             "switch_mmio_write32(ahub, ADMAIF_TX_CIF, switch_audio_admaif_cif(SWITCH_AUDIO_OUT_CHANNELS));",
             "switch_mmio_write32(ahub, ADMAIF_RX_CIF, switch_audio_admaif_cif(SWITCH_AUDIO_IN_CHANNELS));",
             "switch_mmio_write32(ahub, I2S_RX_CIF, switch_audio_i2s_rx_cif());",
             "switch_mmio_write32(ahub, I2S_TX_CIF, switch_audio_i2s_tx_cif());",
             "switch_mmio_write32(ahub, I2S_CTRL, kI2sCtrl);",
             "switch_mmio_write32(ahub, I2S_TIMING, kI2sTiming);",
         })
        EXPECT_THAT(d, HasSubstr(line));
    const std::string codec = SourceText("../SwitchRT5639.cpp");
    for (const char *line : {
             "update(I2S1_SDP, SWITCH_AUDIO_RT5640_SDP_MASK, SWITCH_AUDIO_RT5640_SDP);",
             "update(ADDA_CLK1, SWITCH_AUDIO_RT5640_ADDA_MASK, SWITCH_AUDIO_RT5640_ADDA);",
         })
        EXPECT_THAT(codec, HasSubstr(line));
    EXPECT_THAT(d, HasSubstr("kI2sCtrl = switch_audio_i2s_ctrl(), kI2sTiming = switch_audio_i2s_timing()"));
    for (const char *gone : {"kPack16", "constexpr uint32_t cif(", "sizeof(int16_t)", "int16_t *"})
        EXPECT_THAT(d, Not(HasSubstr(gone)));
    const std::regex writes(R"(switch_mmio_write32\(ahub, (?:ADMAIF|I2S)_[RT]X_CIF,)");
    EXPECT_EQ(std::distance(std::sregex_iterator(d.begin(), d.end(), writes), std::sregex_iterator()), 4);
}

TEST(SwitchAudioSource, CodecUnmutedOnlyWhileTheI2sClocks)
{
    const std::string d = Driver();
    const std::string request = Between(d, "IOReturn SwitchAudio::request(uint32_t selector)", "\n}\n");
    const size_t powerUp = request.find("codecStep(kCodecPowerUp)"), pause = request.find("codecStep(kCodecPause)");
    const size_t hardware = request.find("const IOReturn result = gate->runAction(&SwitchAudio::requestGated");
    const size_t unmute = request.find("codecStep(kCodecUnmute)");
    ASSERT_NE(hardware, std::string::npos);
    EXPECT_LT(powerUp, hardware);
    EXPECT_LT(pause, hardware);
    EXPECT_NE(unmute, std::string::npos);
    EXPECT_GT(unmute, hardware);
    EXPECT_THAT(Between(d, "void SwitchAudio::codecPowerUp()", "\n}\n"), Not(HasSubstr("mute(false)")));
    const std::string sleep = Between(d, "void SwitchAudio::systemSleep(bool entering)", "} else {");
    EXPECT_LT(sleep.find("codecStep(kCodecMute)"), sleep.find("&SwitchAudio::sleepGated"));
}

TEST(SwitchAudioSource, CodecPowerPadDrivenBeforeItsFirstRead)
{
    const std::string gpio = SourceText("../../TegraGPIO/TegraGPIO.cpp");
    EXPECT_THAT(Between(gpio, "constexpr OutputMux kOutputMux[] = {", "};"),
                HasSubstr("{TEGRA_GPIO(Z, 4), 0x28c, PULL_DOWN | 1}"));
    const std::string present = Between(SourceText("../SwitchRT5639.cpp"), "bool SwitchRT5639::present()", "\n}\n");
    const size_t raise = present.find("tegra_gpio_output(kLdo1Enable, 1)");
    ASSERT_NE(raise, std::string::npos);
    EXPECT_LT(raise, present.find("tegra_i2c_read("));
    const std::string start = Between(Driver(), "bool SwitchAudio::start(IOService *provider)", "\n}\n");
    EXPECT_LT(start.find("SwitchRT5639::present()"), start.find("tegraInit()"));
}

TEST(SwitchAudioSource, AbiIsS32)
{
    const std::string driver = Driver();
    const std::string plugin = SourceText("../hal/SwitchAudioPlugIn.c");
    EXPECT_THAT(driver, HasSubstr("sizeof(switch_audio_sample_t)"));
    EXPECT_THAT(plugin, Not(HasSubstr("int16_t")));
    EXPECT_THAT(plugin,
                HasSubstr("switch_audio_ring_mix(gOut, SWITCH_AUDIO_RING_FRAMES, 2, first, src, count, mixGain)"));
    EXPECT_THAT(plugin, HasSubstr("switch_audio_ring_read_s32(main, gIn, SWITCH_AUDIO_IN_RING_FRAMES,"));
}

TEST(SwitchAudioSource, JackIsTheCodecDevice)
{
    const std::string plugin = SourceText("../hal/SwitchAudioPlugIn.c");
    EXPECT_THAT(plugin, HasSubstr("#define kCodecUID   \"Codec\""));
    EXPECT_THAT(plugin, HasSubstr("kJackIsConnected = 'jack'"));
    EXPECT_THAT(
        plugin,
        HasSubstr(
            "REPLY(UInt32, (jackState() & (scopeIn ? SwitchAudioJackMicrophone : SwitchAudioJackHeadphones)) != 0);"));
    EXPECT_THAT(plugin, HasSubstr("gHost->PropertiesChanged(gHost, kCodec, n, changed);"));
    EXPECT_THAT(plugin,
                HasSubstr("IOServiceAddInterestNotification(gJackPort, service, kIOGeneralInterest, jackChanged"));
    EXPECT_THAT(
        plugin,
        HasSubstr("REPLY(AudioObjectPropertyElement, o == kCodecVolume ? 1 : kAudioObjectPropertyElementMain);"));
    EXPECT_THAT(plugin, HasSubstr("IOConnectCallScalarMethod(gConnect, SwitchAudioSetHeadphoneLevel, &attenuation, 1"));
    EXPECT_THAT(plugin, Not(HasSubstr("SwitchAudioMic")));
    EXPECT_THAT(plugin, Not(HasSubstr("kAudioDevicePropertyDataSources")));

    const std::string driver = Driver();
    EXPECT_THAT(driver, HasSubstr("tegraCapture = tegra;"));
    EXPECT_THAT(Between(driver, "void SwitchAudio::publishJack()", "\n}\n"),
                HasSubstr("messageClients(kIOMessageServicePropertyChange);"));
    const std::string capture = Between(driver, "void SwitchAudio::captureOff()", "\n}\n");
    EXPECT_THAT(capture, Not(HasSubstr("panic(")));
    EXPECT_THAT(capture, HasSubstr("tegraCapture = false;"));
    EXPECT_THAT(driver, HasSubstr("if (!captureProven && played > 2 * static_cast<uint64_t>(kOutPeriodFrames)) {"));

    const std::string codec = SourceText("../SwitchRT5639.cpp");
    EXPECT_THAT(codec, HasSubstr("void SwitchRT5639::setHeadphoneAnalog(uint8_t regVal, bool muted)"));
    EXPECT_THAT(codec, HasSubstr("write(DAC1_DIG_VOL, kDacHeadphone);"));
    EXPECT_THAT(codec, Not(HasSubstr("0x0101")));
}

TEST(SwitchAudioSource, LongPlaybackShortCapturePeriods)
{
    const std::string d = Driver();
    EXPECT_THAT(d, HasSubstr("constexpr unsigned kOutPeriods = 4, kInPeriods = 8;"));
    EXPECT_THAT(d, HasSubstr("((kOutPeriods - 1) << 28) | (5u << 20) | 1u"));
    EXPECT_THAT(d, HasSubstr("control->period_frames = kInPeriodFrames;"));
    EXPECT_THAT(d, Not(HasSubstr("kPeriodFrames ")));
    constexpr uint32_t kOutPeriod = SWITCH_AUDIO_RING_FRAMES / 4, kLead = SWITCH_AUDIO_LEAD_FRAMES;
    constexpr uint32_t kLargestIoBuffer = 16384;
    static_assert(kLead + kLargestIoBuffer < SWITCH_AUDIO_RING_FRAMES - kOutPeriod);
}

TEST(SwitchAudioSource, CapturePathFollowsInput)
{
    constexpr uint32_t out = SwitchAudioOutputRunning, in = SwitchAudioInputRunning;
    EXPECT_EQ(neon_h_transition(0, out), (uint32_t)SWITCH_AUDIO_HW_START);
    EXPECT_EQ(neon_h_transition(0, in), (uint32_t)(SWITCH_AUDIO_HW_START | SWITCH_AUDIO_CAPTURE_START));
    EXPECT_EQ(neon_h_transition(out, out | in), (uint32_t)SWITCH_AUDIO_CAPTURE_START);
    EXPECT_EQ(neon_h_transition(out | in, out), (uint32_t)SWITCH_AUDIO_CAPTURE_STOP);
    EXPECT_EQ(neon_h_transition(out | in, in), 0u);
    EXPECT_EQ(neon_h_transition(in, 0), (uint32_t)(SWITCH_AUDIO_CAPTURE_STOP | SWITCH_AUDIO_HW_STOP));
    EXPECT_EQ(neon_h_transition(out, 0), (uint32_t)SWITCH_AUDIO_HW_STOP);
    EXPECT_EQ(neon_h_transition(out, out), 0u);

    constexpr uint64_t kNone = SWITCH_AUDIO_NO_CAPTURE;
    EXPECT_EQ(neon_h_capture_frame(1000, kNone, 5000, SWITCH_AUDIO_IN_RING_FRAMES), -1);
    EXPECT_EQ(neon_h_capture_frame(999, 1000, 5000, SWITCH_AUDIO_IN_RING_FRAMES), -1);
    EXPECT_EQ(neon_h_capture_frame(1000, 1000, 5000, SWITCH_AUDIO_IN_RING_FRAMES), 0);
    EXPECT_EQ(neon_h_capture_frame(4999, 1000, 5000, SWITCH_AUDIO_IN_RING_FRAMES), 3999);
    EXPECT_EQ(neon_h_capture_frame(5000, 1000, 5000, SWITCH_AUDIO_IN_RING_FRAMES), -1);
    EXPECT_EQ(neon_h_capture_frame(1000 + SWITCH_AUDIO_IN_RING_FRAMES + 7, 1000, 1000 + 2 * SWITCH_AUDIO_IN_RING_FRAMES,
                                   SWITCH_AUDIO_IN_RING_FRAMES),
              7);

    const std::string d = Driver();
    const std::string start = Between(d, "void SwitchAudio::tegraStart()", "\n}\n");
    for (const char *capture : {"ADMAIF_RX_ENABLE", "I2S_TX_ENABLE", "programChannel(kChIn)"})
        EXPECT_THAT(start, Not(HasSubstr(capture))) << "playback start powers no capture: " << capture;
    EXPECT_THAT(Between(d, "void SwitchAudio::codecPowerUp()", "\n}\n"), Not(HasSubstr("powerCapture")));
    EXPECT_THAT(Between(d, "void SwitchAudio::captureStart()", "\n}\n"), HasSubstr("programChannel(kChIn);"));
    EXPECT_THAT(Between(d, "void SwitchAudio::tegraStop()", "\n}\n"), HasSubstr("captureStop();"));
    const std::string request = Between(d, "IOReturn SwitchAudio::request(uint32_t selector)", "\n}\n");
    EXPECT_THAT(request, HasSubstr("codecStep(kCodecCaptureOn)"));
    EXPECT_THAT(request, HasSubstr("codecStep(kCodecCaptureOff)"));
    EXPECT_THAT(d, HasSubstr("const uint32_t actions = switch_audio_transition(was, c->state);"));
    EXPECT_THAT(Between(d, "void SwitchAudio::noteJack()", "\n}\n"), HasSubstr("if (!captureCodecOn || !headset)"));

    const std::string plugin = SourceText("../hal/SwitchAudioPlugIn.c");
    EXPECT_THAT(plugin, HasSubstr("const Boolean want = gCodecUsers > 0 && gCodecInActive && gControl;"));
    EXPECT_THAT(Between(plugin, "static OSStatus StartIO(", "\n}\n"), Not(HasSubstr("SwitchAudioStartInput")));
    EXPECT_THAT(plugin, HasSubstr("switch_audio_ring_read_s32(main, gIn, SWITCH_AUDIO_IN_RING_FRAMES,"));
}

TEST(SwitchAudioSource, HdaReturnsDmaOwnershipBeforeReusingMemoryOrResetting)
{
    const std::string source = SourceText("../SwitchHDA.cpp");
    const auto ordered = [](const std::string &body, const char *first, const char *second) {
        const size_t before = body.find(first), after = body.find(second);
        ASSERT_NE(before, std::string::npos) << first;
        ASSERT_NE(after, std::string::npos) << second;
        EXPECT_LT(before, after) << first << " before " << second;
    };
    const auto body = [&](const char *signature) { return Between(source, signature, "\n}\n"); };
    EXPECT_THAT(source, Not(HasSubstr("panic(")));
    const std::string stop = body("bool SwitchHDA::stop()");
    EXPECT_THAT(stop, HasSubstr("kBar + stream, kRun, 0, 40"));
    ordered(stop, "coreWrite(stream,", "switch_mmio_wait32");
    ordered(stop, "switch_mmio_wait32", "control->state = 0;");
    const std::string commands = body("bool SwitchHDA::stopCommands()");
    EXPECT_THAT(commands, HasSubstr("kBar + CORBCTL, kRun, 0, 1000"));
    EXPECT_THAT(commands, HasSubstr("kBar + RIRBCTL, kRun, 0, 1000"));
    ordered(commands, "field(RIRBCTL,", "switch_mmio_wait32");
    ordered(body("bool SwitchHDA::reset()"), "if (!stop() || !stopCommands())", "coreWrite(GCTL, 0);");
    const std::string start = body("IOReturn SwitchHDA::start()");
    EXPECT_THAT(start, Not(HasSubstr("verb(")));
    EXPECT_THAT(start, Not(HasSubstr("programFormat(")));
    ordered(start, "if (!stop())", "bzero(");
    ordered(start, "if (!stop())", "coreWrite(stream, kStreamReset);");
    ordered(body("void SwitchHDA::disconnect()"), "stop();", "stopCommands();");
    ordered(body("void SwitchHDA::sleep(bool entering)"), "disconnect();", "power(false);");
    const std::string release = body("void SwitchHDA::release()");
    ordered(release, "disconnect();", "power(false);");
    ordered(release, "power(false);", "tegra_smmu_detach");
}

TEST(SwitchAudioSource, SleepWaitsForTheExternalPresenceTransactionBeforeReturning)
{
    const std::string source = SourceText("../SwitchDisplayAudio.cpp");
    const std::string sleep = Between(source, "IOReturn SwitchAudio::displaySleepGated(", "\n}\n");
    EXPECT_THAT(sleep, HasSubstr("clock_interval_to_deadline("));
    EXPECT_THAT(sleep, HasSubstr("while (self->displayPresencePending)"));
    EXPECT_THAT(sleep, HasSubstr("commandSleep(&self->displayPresencePending, deadline, THREAD_UNINT)"));
    EXPECT_THAT(sleep, HasSubstr("THREAD_TIMED_OUT"));
    EXPECT_THAT(sleep, HasSubstr("displayAudio.sleep(true)"));
    EXPECT_LT(sleep.find("commandSleep("), sleep.find("displayAudio.sleep(true)"));
    const std::string complete = Between(source, "IOReturn SwitchAudio::displayConnectGated(", "\n}\n");
    EXPECT_THAT(complete, HasSubstr("displayPresencePending = false"));
    EXPECT_THAT(complete, HasSubstr("displayAudio.sleep(true)"));
    EXPECT_THAT(complete, HasSubstr("commandWakeup(&self->displayPresencePending, false)"));
    EXPECT_LT(complete.find("displayPresencePending = false"), complete.find("commandWakeup("));
    EXPECT_LT(complete.find("displayAudio.sleep(true)"), complete.find("commandWakeup("));
}

}  // namespace
