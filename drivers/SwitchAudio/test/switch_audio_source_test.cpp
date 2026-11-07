// SwitchAudio's driver and plug-in use the S32 link as switch_audio_format_test checks it: the driver programs
// exactly SwitchAudioFormat.h's words (no hand-written duplicates), its I2S clock is 47999.878 or 44099.854 Hz on PLLA
// (the header's clock table through its divider and rate helpers), the ABI and plug-in are S32 throughout, and the codec
// is unmuted only while the I2S clocks run.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <iterator>
#include <optional>
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

// The ABI: version 6 is S32_LE samples at one of rates[]; periods stay whole ADMA buffers and the rings DMA-able
// (playback 65536 frames of stereo S32: 512 KiB; capture 16384 mono: 64 KiB).
static_assert(SWITCH_AUDIO_VERSION == 7u);
static_assert(std::is_same_v<switch_audio_sample_t, int32_t>);
static_assert(SWITCH_AUDIO_RING_FRAMES * switch_audio_linux::kChannels * sizeof(switch_audio_sample_t) == 512 * 1024);
static_assert(SWITCH_AUDIO_IN_RING_FRAMES * sizeof(switch_audio_sample_t) == 64 * 1024);

struct ClockFamily {
    uint32_t index;
    Fraction plla;  // clk-tegra210b01.c pll_a_freq_table's entry for the 38.4 MHz reference
    uint32_t i2sDivider, extern1Divider;
    double hz;  // the frame rate it really makes
};

// Both families from the 38.4 MHz oscillator: PLLA from the T210B01 table, PLLA_OUT0 a whole 7.1 ratio of it, the
// bit clock 64 fs and MCLK 256 fs exact divisions of PLLA_OUT0, and the frame time the plug-in is told.
TEST(SwitchAudioSource, ClockFamiliesDivideToTheirRates)
{
    for (const ClockFamily &family : {ClockFamily{0, Fraction(737278125, 2), 30, 6, 47999.878},
                                      ClockFamily{1, Fraction(564478125, 2), 38, 8, 44099.854}}) {
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
        const Fraction bclk = pllaOut0 / (Fraction(i2s, 2) + Fraction(1));  // 7.1 divider: ratio = value / 2 + 1
        const Fraction mclk = pllaOut0 / (Fraction(extern1, 2) + Fraction(1));
        const Fraction fs = bclk / Fraction(neon_h_bclk_fs());
        EXPECT_TRUE(mclk == fs * Fraction(256)) << rate;
        EXPECT_TRUE(pllaOut0 == fs * Fraction(perFs)) << rate;
        EXPECT_EQ(std::round((fs.value() - family.hz) * 1000), 0.0) << fs.value();
        EXPECT_LT(std::fabs(fs.value() - rate) / rate, 5e-6) << rate;
        EXPECT_EQ(neon_h_clock_millihz(38400000, family.index), (uint64_t)(fs * Fraction(1000)).floor());
        for (uint64_t hostHz : {19200000ull, 24000000ull}) {
            const uint64_t q32 = neon_h_clock_ticks_per_frame_q32(hostHz, 38400000, family.index);
            EXPECT_EQ(q32, (uint64_t)(Fraction((__int128)hostHz) / fs * Fraction((__int128)1 << 32)).floor()) << hostHz;
        }
    }
}

TEST(SwitchAudioSource, DriverRunsTheClockTable)
{
    const std::string driver = SourceText("../SwitchAudio.cpp");
    for (const char *line :
         {"constexpr switch_audio_clock kClocks[SWITCH_AUDIO_MAX_RATES] = {SWITCH_AUDIO_CLOCK_48000, "
          "SWITCH_AUDIO_CLOCK_44100};",
          "switch_mmio_write32(car, CLK_SOURCE_I2S0, switch_audio_i2s_divider(clock->out0_per_fs));",
          "switch_mmio_write32(car, CLK_SOURCE_EXTERN1, switch_audio_extern1_divider(clock->out0_per_fs));",
          "const uint32_t out0Div = clock->out0_x2 - 2;",
          "return switch_audio_clock_ticks_per_frame_q32(hz, refHz, *clock);"})
        EXPECT_THAT(driver, HasSubstr(line));
    for (const char *gone : {"kPllaM", "kPllaN =", "kPllaSdm", "kI2sDiv", "kExtern1Div", "kWatchdogMS"})
        EXPECT_THAT(driver, Not(HasSubstr(gone)));
    // A rate change powers the codec down before its MCLK changes, and only between streams.
    const std::string setRate = Between(driver, "IOReturn setRate(uint32_t rate)", "\n    }\n");
    const size_t idle = setRate.find("codecStep(kCodecIdleNow)"), change = setRate.find("&SwitchAudio::rateGated");
    ASSERT_NE(idle, std::string::npos);
    ASSERT_NE(change, std::string::npos);
    EXPECT_LT(idle, change);
    EXPECT_THAT(Between(driver, "static IOReturn rateGated(", "\n    }\n"), HasSubstr("if (self->control->state) {"));
}

TEST(SwitchAudioSource, DriverProgramsTheHeaderWords)
{
    const std::string d = SourceText("../SwitchAudio.cpp");
    for (const char *line :
         {"switch_mmio_write32(ahub, ADMAIF_TX_CIF, switch_audio_admaif_cif(SWITCH_AUDIO_OUT_CHANNELS));",
          "switch_mmio_write32(ahub, ADMAIF_RX_CIF, switch_audio_admaif_cif(SWITCH_AUDIO_IN_CHANNELS));",
          "switch_mmio_write32(ahub, I2S_RX_CIF, switch_audio_i2s_rx_cif());",
          "switch_mmio_write32(ahub, I2S_TX_CIF, switch_audio_i2s_tx_cif());",
          "switch_mmio_write32(ahub, I2S_CTRL, kI2sCtrl);", "switch_mmio_write32(ahub, I2S_TIMING, kI2sTiming);"})
        EXPECT_THAT(d, HasSubstr(line));
    const std::string codec = SourceText("../SwitchRT5639.cpp");
    for (const char *line : {"update(I2S1_SDP, SWITCH_AUDIO_RT5640_SDP_MASK, SWITCH_AUDIO_RT5640_SDP);",
                             "update(ADDA_CLK1, SWITCH_AUDIO_RT5640_ADDA_MASK, SWITCH_AUDIO_RT5640_ADDA);"})
        EXPECT_THAT(codec, HasSubstr(line));
    EXPECT_THAT(d, HasSubstr("kI2sCtrl = switch_audio_i2s_ctrl(), kI2sTiming = switch_audio_i2s_timing()"));
    for (const char *gone : {"kPack16", "constexpr uint32_t cif(", "sizeof(int16_t)", "int16_t *"})
        EXPECT_THAT(d, Not(HasSubstr(gone)));
    // Nothing else writes the CIF registers.
    const std::regex writes(R"(switch_mmio_write32\(ahub, (?:ADMAIF|I2S)_[RT]X_CIF,)");
    EXPECT_EQ(std::distance(std::sregex_iterator(d.begin(), d.end(), writes), std::sregex_iterator()), 4);
}

// The RT5639, the I2S slave, is unmuted only once the I2S clocks run and muted before they stop, at a stream's start
// and stop and at system sleep (ASoC's order): its amplifiers never carry the frame clock starting or stopping.
TEST(SwitchAudioSource, CodecUnmutedOnlyWhileTheI2sClocks)
{
    const std::string d = SourceText("../SwitchAudio.cpp");
    const std::string request = Between(d, "IOReturn request(uint32_t selector)", "\n    }\n");
    const size_t powerUp = request.find("codecStep(kCodecPowerUp)"), pause = request.find("codecStep(kCodecPause)");
    const size_t hardware = request.find("const IOReturn result = gate->runAction(&SwitchAudio::requestGated");
    const size_t unmute = request.find("codecStep(kCodecUnmute)");
    ASSERT_NE(hardware, std::string::npos);
    EXPECT_LT(powerUp, hardware);
    EXPECT_LT(pause, hardware);
    EXPECT_NE(unmute, std::string::npos);
    EXPECT_GT(unmute, hardware);
    EXPECT_THAT(Between(d, "void codecPowerUp()", "\n    }\n"), Not(HasSubstr("mute(false)")));
    const std::string sleep = Between(d, "void systemSleep(bool entering)", "} else {");
    EXPECT_LT(sleep.find("codecStep(kCodecMute)"), sleep.find("&SwitchAudio::sleepGated"));
}

TEST(SwitchAudioSource, AbiIsS32)
{
    const std::string driver = SourceText("../SwitchAudio.cpp");
    const std::string plugin = SourceText("../hal/SwitchAudioPlugIn.c");
    EXPECT_THAT(driver, HasSubstr("sizeof(switch_audio_sample_t)"));
    EXPECT_THAT(plugin, Not(HasSubstr("int16_t")));
    EXPECT_THAT(plugin, HasSubstr("switch_audio_ring_mix(gOut, SWITCH_AUDIO_RING_FRAMES, 2, first, main, frames,"));
    EXPECT_THAT(plugin, HasSubstr("switch_audio_s32_to_float(gIn["));
}

// The jack as VirtualAudio's Device_Codec_Aspen takes it: a "Codec" device whose 'jack' answers in the output scope
// (Wired Headphones) and the input scope (Wired Microphone) from the first query, changed by PropertiesChanged on that
// device when the kernel's interest message comes; the headphone volume a level control on output element 1, sent to
// the codec. The kernel captures, publishes the jack's state and turns a capture channel that never delivered off
// rather than panic.
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
    EXPECT_THAT(plugin, Not(HasSubstr("kAudioDevicePropertyDataSources")));  // no built-in microphone

    const std::string driver = SourceText("../SwitchAudio.cpp");
    EXPECT_THAT(driver, HasSubstr("tegraCapture = tegra;"));
    EXPECT_THAT(Between(driver, "void publishJack()", "\n    }\n"),
                HasSubstr("messageClients(kIOMessageServicePropertyChange);"));
    const std::string capture = Between(driver, "void captureOff()", "\n    }\n");
    EXPECT_THAT(capture, Not(HasSubstr("panic(")));
    EXPECT_THAT(capture, HasSubstr("tegraCapture = false;"));
    EXPECT_THAT(driver, HasSubstr("if (!captureProven && played > 2 * kOutPeriodFrames) {"));

    const std::string codec = SourceText("../SwitchRT5639.cpp");
    EXPECT_THAT(codec, HasSubstr("write(DAC1_DIG_VOL, headphoneDac());"));
    EXPECT_THAT(codec, Not(HasSubstr("write(DAC1_DIG_VOL, kDacHeadphone);")));
}

// Playback's ADMA period is long (its interrupt only clears what played) and bounded by the plug-in's write-ahead;
// capture's stays short, the microphone's latency.
TEST(SwitchAudioSource, LongPlaybackShortCapturePeriods)
{
    const std::string d = SourceText("../SwitchAudio.cpp");
    EXPECT_THAT(d, HasSubstr("constexpr unsigned kOutPeriods = 4, kInPeriods = 8;"));
    EXPECT_THAT(d, HasSubstr("((kOutPeriods - 1) << 28) | (5u << 20) | 1u"));
    EXPECT_THAT(d, HasSubstr("control->period_frames = kInPeriodFrames;"));
    EXPECT_THAT(d, Not(HasSubstr("kPeriodFrames ")));
    constexpr uint32_t kOutPeriod = SWITCH_AUDIO_RING_FRAMES / 4, kLead = 2 * SWITCH_AUDIO_PERIOD_FRAMES;
    constexpr uint32_t kLargestIoBuffer = 16384;
    static_assert(kLead + kLargestIoBuffer < SWITCH_AUDIO_RING_FRAMES - kOutPeriod);
    const std::string plugin = SourceText("../hal/SwitchAudioPlugIn.c");
    EXPECT_THAT(plugin, HasSubstr("switch_audio_s32_to_float(gIn[at])"));
}

// The capture path runs only while input does: playback alone (the speaker, or headphones on the Codec device) never
// starts ADMA channel 1, I2S TX or the codec's ADC and microphone bias.
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

    // Capture ring frame 0 holds the sample time the session started at; outside [anchor, in_frames) is silence.
    constexpr uint64_t kNone = SWITCH_AUDIO_NO_CAPTURE;
    EXPECT_EQ(neon_h_capture_frame(1000, kNone, 5000, SWITCH_AUDIO_IN_RING_FRAMES), -1);
    EXPECT_EQ(neon_h_capture_frame(999, 1000, 5000, SWITCH_AUDIO_IN_RING_FRAMES), -1);
    EXPECT_EQ(neon_h_capture_frame(1000, 1000, 5000, SWITCH_AUDIO_IN_RING_FRAMES), 0);
    EXPECT_EQ(neon_h_capture_frame(4999, 1000, 5000, SWITCH_AUDIO_IN_RING_FRAMES), 3999);
    EXPECT_EQ(neon_h_capture_frame(5000, 1000, 5000, SWITCH_AUDIO_IN_RING_FRAMES), -1);
    EXPECT_EQ(neon_h_capture_frame(1000 + SWITCH_AUDIO_IN_RING_FRAMES + 7, 1000, 1000 + 2 * SWITCH_AUDIO_IN_RING_FRAMES,
                                   SWITCH_AUDIO_IN_RING_FRAMES),
              7);

    const std::string d = SourceText("../SwitchAudio.cpp");
    const std::string start = Between(d, "    void tegraStart()", "\n    }\n");
    for (const char *capture : {"ADMAIF_RX_ENABLE", "I2S_TX_ENABLE", "programChannel(kChIn)"})
        EXPECT_THAT(start, Not(HasSubstr(capture))) << "playback start powers no capture: " << capture;
    EXPECT_THAT(Between(d, "    void codecPowerUp()", "\n    }\n"), Not(HasSubstr("powerCapture")));
    EXPECT_THAT(Between(d, "    void captureStart()", "\n    }\n"), HasSubstr("programChannel(kChIn);"));
    EXPECT_THAT(Between(d, "    void tegraStop()", "\n    }\n"), HasSubstr("captureStop();"));
    const std::string request = Between(d, "IOReturn request(uint32_t selector)", "\n    }\n");
    EXPECT_THAT(request, HasSubstr("codecStep(kCodecCaptureOn)"));
    EXPECT_THAT(request, HasSubstr("codecStep(kCodecCaptureOff)"));
    EXPECT_THAT(d, HasSubstr("const uint32_t actions = switch_audio_transition(was, c->state);"));
    EXPECT_THAT(Between(d, "    void noteJack()", "\n    }\n"), HasSubstr("if (!captureCodecOn || !headset)"));

    const std::string plugin = SourceText("../hal/SwitchAudioPlugIn.c");
    EXPECT_THAT(plugin, HasSubstr("const Boolean want = gCodecUsers > 0 && gCodecInActive && gControl;"));
    EXPECT_THAT(Between(plugin, "static OSStatus StartIO(", "\n}\n"), Not(HasSubstr("SwitchAudioStartInput")));
    EXPECT_THAT(plugin, HasSubstr("switch_audio_capture_frame(t, anchor, captured, SWITCH_AUDIO_IN_RING_FRAMES)"));
}

}  // namespace
