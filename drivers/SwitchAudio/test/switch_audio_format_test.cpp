// SwitchAudio's S32 link: the APE and codec register words for S32_LE samples from memory to the RT5639,
// and the plug-in's float32 <-> S32 conversions, as shipped (drivers/SwitchAudio/SwitchAudioFormat.h,
// hal/SwitchAudioConvert.h, compiled as C through switch_audio_format_harness.c).
//
// Register words are re-derived from the Linux sources' formulas for that stream, independently of the header
// (switch_audio_words.h). switch_audio_source_test checks the driver programs exactly those words and keeps its I2S
// clock at 47999.878 Hz on PLLA.
//
// Conversions: full scale 2^31 with saturation (+1.0 -> INT32_MAX, -1.0 -> INT32_MIN, NaN -> 0), two IO contexts sum
// with saturation, ring wrap-around in two runs, J181's four speaker channels folded to the stereo ring (channels 0 and
// 2), capture back to float exactly for 24-bit samples; the NEON build (as on the device) and the scalar fallback agree
// bit for bit.
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <random>
#include <vector>

#include "switch_audio_format_harness.h"
#include "switch_audio_words.h"

namespace {

namespace linux_ = switch_audio_linux;
using linux_::kChannels;
using linux_::kRate;
constexpr int64_t kInt32Max = INT32_MAX, kInt32Min = INT32_MIN;

// FCVTZS #31 on a float32: x * 2^31 rounded toward zero, saturated; NaN is 0.
int64_t fcvtzs31(float x)
{
    if (std::isnan(x))
        return 0;
    const double v = (double)x * 2147483648.0;
    if (v >= 2147483648.0)
        return kInt32Max;
    if (v <= -2147483648.0)
        return kInt32Min;
    return (int64_t)v;
}

int32_t saturate(int64_t v)
{
    return (int32_t)std::clamp(v, kInt32Min, kInt32Max);
}

struct Build {
    void (*mix)(int32_t *, const float *, uint32_t, float);
    int (*neon)();
};

const Build kNeon{neon_h_mix, neon_h_neon};
const Build kScalar{scalar_h_mix, scalar_h_neon};

std::vector<int32_t> mix(std::vector<int32_t> dst, const std::vector<float> &src, float gain = 1.0f,
                         const Build &build = kNeon)
{
    build.mix(dst.data(), src.data(), (uint32_t)src.size(), gain);
    return dst;
}

TEST(SwitchAudioFormat, AdmaifWordsAreL4tS32)
{
    for (uint32_t channels : {1u, 2u})
        EXPECT_EQ(neon_h_admaif_cif(channels), linux_::admaif("S32_LE", channels));
    // S16_LE, for contrast: PACK16 with 16-bit audio and client bits.
    EXPECT_EQ(linux_::admaif("S16_LE", 2), (1u << 30) | 0x00113300u);
}

TEST(SwitchAudioFormat, I2sWordsAreL4tS32)
{
    const linux_::I2s i2s = linux_::i2s(32, kChannels, kRate);
    EXPECT_EQ(neon_h_i2s_ctrl() & 7, i2s.bitSize);
    EXPECT_EQ(neon_h_i2s_ctrl() & ~7u, (31u << 24) | (1u << 10));  // fsync 31, master, LRCK
    EXPECT_EQ(neon_h_i2s_timing(), i2s.timing);
    EXPECT_EQ(i2s.i2sClock, 3072000u);
    EXPECT_EQ(neon_h_bclk_fs() * kRate, i2s.i2sClock);
    // Playback CIF (AXBAR RX, XBAR -> DAP): 2/2 channels, RX FIFO threshold 3 (rx_fifo_th default); capture CIF (AXBAR
    // TX, DAP -> XBAR): stereo_to_mono channel 0, audio 1 channel, client 2.
    linux_::Cif rx;
    rx.threshold = 3, rx.audioChannels = 2, rx.clientChannels = 2, rx.audioBits = rx.clientBits = linux_::bits(32);
    EXPECT_EQ(neon_h_i2s_rx_cif(), linux_::cif(rx));
    linux_::Cif tx;
    tx.audioChannels = 1, tx.clientChannels = 2, tx.audioBits = tx.clientBits = linux_::bits(32), tx.stereoConv = 0;
    EXPECT_EQ(neon_h_i2s_tx_cif(), linux_::cif(tx));
}

TEST(SwitchAudioFormat, CodecWordsAreRt5640TwentyFourBit)
{
    const std::optional<linux_::Rt5640> codec = linux_::rt5640(24, kChannels, kRate, 256 * kRate);
    ASSERT_TRUE(codec.has_value());
    EXPECT_EQ(neon_h_rt5640_sdp_mask(), codec->sdpMask);
    EXPECT_EQ(neon_h_rt5640_sdp(), codec->sdp);
    EXPECT_EQ(neon_h_rt5640_adda_mask(), codec->addaMask);
    EXPECT_EQ(neon_h_rt5640_adda(), codec->adda);
    EXPECT_EQ(neon_h_codec_bits(), 24u);
    // The codec's BCLK/LRCK ratio is the Tegra's bit clock; it has no 32-bit width (S32 would be -EINVAL).
    EXPECT_EQ(codec->bclkFs, neon_h_bclk_fs());
    EXPECT_FALSE(linux_::rt5640(32, kChannels, kRate, 256 * kRate).has_value());
}

TEST(SwitchAudioFormat, FullScaleAndSaturation)
{
    const float inf = std::numeric_limits<float>::infinity(), nan = std::numeric_limits<float>::quiet_NaN();
    const std::vector<float> src = {
        1.0f, -1.0f, 0.5f, -0.5f, 0.0f, -0.0f, 2.0f, -2.0f, nan, inf, -inf, std::ldexp(1.0f, -31), 0.999999f};
    const std::vector<int32_t> got = mix(std::vector<int32_t>(src.size(), 0), src);
    EXPECT_EQ(std::vector<int32_t>(got.begin(), got.begin() + 8),
              (std::vector<int32_t>{INT32_MAX, INT32_MIN, 1 << 30, -(1 << 30), 0, 0, INT32_MAX, INT32_MIN}));
    EXPECT_EQ(std::vector<int32_t>(got.begin() + 8, got.begin() + 11), (std::vector<int32_t>{0, INT32_MAX, INT32_MIN}));
    EXPECT_EQ(got[11], 1);
    for (size_t i = 0; i < src.size(); ++i)
        EXPECT_EQ(got[i], fcvtzs31(src[i])) << "sample " << i;
}

TEST(SwitchAudioFormat, ContextsSumWithSaturation)
{
    const std::vector<float> a = {0.25f, 0.75f, -0.75f, 0.5f, -1.0f, 0.1f};
    const std::vector<float> b = {0.25f, 0.75f, -0.75f, -0.5f, -1.0f, -0.1f};
    const std::vector<int32_t> once = mix(std::vector<int32_t>(6, 0), a);
    const std::vector<int32_t> twice = mix(once, b);
    for (size_t i = 0; i < a.size(); ++i)
        EXPECT_EQ(twice[i], saturate(fcvtzs31(a[i]) + fcvtzs31(b[i]))) << i;
    EXPECT_EQ(std::vector<int32_t>(twice.begin(), twice.begin() + 5),
              (std::vector<int32_t>{1 << 30, INT32_MAX, INT32_MIN, 0, INT32_MIN}));
}

TEST(SwitchAudioFormat, Gain)
{
    EXPECT_EQ(mix(std::vector<int32_t>(4, 0), {1.0f, -1.0f, 0.5f, 0.25f}, 0.5f),
              (std::vector<int32_t>{1 << 30, -(1 << 30), 1 << 29, 1 << 28}));
    EXPECT_EQ(mix(std::vector<int32_t>(3, 7), {0.9f, -0.9f, 0.3f}, 0.0f), (std::vector<int32_t>{7, 7, 7}));
}

TEST(SwitchAudioFormat, EveryLengthNeonMatchesScalar)
{
    std::mt19937_64 rng(17);
    std::uniform_real_distribution<double> sample(-1.3, 1.3), unit(0.0, 1.0);
    std::uniform_int_distribution<int64_t> any(kInt32Min, kInt32Max - 1);
    EXPECT_EQ(kNeon.neon(), 1);
    EXPECT_EQ(kScalar.neon(), 0);
    std::vector<uint32_t> lengths;
    for (uint32_t n = 0; n < 13; ++n)
        lengths.push_back(n);
    lengths.push_back(1023);
    lengths.push_back(2048);
    for (uint32_t n : lengths) {
        std::vector<float> src(n);
        for (float &s : src)
            s = (float)sample(rng);
        std::vector<int32_t> dst(n);
        for (int32_t &d : dst)
            d = (int32_t)any(rng);
        const float gain = (float)unit(rng);
        const std::vector<int32_t> neon = mix(dst, src, gain, kNeon);
        const std::vector<int32_t> scalar = mix(dst, src, gain, kScalar);
        EXPECT_EQ(neon, scalar) << "length " << n;
        std::vector<int32_t> expect(n);
        for (uint32_t i = 0; i < n; ++i)
            expect[i] = saturate(dst[i] + fcvtzs31(src[i] * gain));
        EXPECT_EQ(neon, expect) << "length " << n;
    }
}

TEST(SwitchAudioFormat, RingWrap)
{
    constexpr uint32_t ringFrames = 16, channels = 2;
    for (uint64_t first : {0ull, 5ull, 12ull, 15ull, 16ull, 16ull * 1000 + 13}) {
        for (uint32_t frames : {0u, 1u, 3u, 4u, 11u, 16u, 20u}) {
            std::vector<int32_t> ring(ringFrames * channels, 0);
            std::vector<float> src(std::max(1u, frames * channels), 0.0f);
            for (uint32_t i = 0; i < frames * channels; ++i)
                src[i] = (float)((i + 1) / 64.0) * (i % 2 ? 1.0f : -1.0f);
            const uint32_t run = neon_h_ring_mix(ring.data(), ringFrames, channels, first, src.data(), frames, 1.0f);
            const uint32_t total = std::min(frames, ringFrames);
            EXPECT_EQ(run, std::min(total, ringFrames - (uint32_t)(first % ringFrames)))
                << "first " << first << " frames " << frames;
            std::vector<int64_t> expect(ringFrames * channels, 0);
            for (uint32_t i = 0; i < total; ++i) {
                const uint64_t at = (first + i) % ringFrames;
                for (uint32_t c = 0; c < channels; ++c)
                    expect[at * channels + c] += fcvtzs31(src[i * channels + c]);
            }
            EXPECT_EQ(std::vector<int64_t>(ring.begin(), ring.end()), expect)
                << "first " << first << " frames " << frames;
        }
    }
}

// J181's speaker frames are left, left, right, right: channels 0 and 2 reach the ring, with the gain, saturating.
TEST(SwitchAudioFormat, FoldEveryLengthNeonMatchesScalar)
{
    std::mt19937_64 rng(181);
    std::uniform_real_distribution<double> sample(-1.3, 1.3), unit(0.0, 1.0);
    std::uniform_int_distribution<int64_t> any(kInt32Min, kInt32Max - 1);
    for (uint32_t frames : {0u, 1u, 2u, 3u, 4u, 5u, 7u, 8u, 9u, 1023u, 2048u}) {
        std::vector<float> src(std::max(1u, frames * 4));
        for (float &s : src)
            s = (float)sample(rng);
        if (frames > 2)
            src[4 * 2 + 2] = std::numeric_limits<float>::quiet_NaN();
        std::vector<int32_t> dst(std::max(1u, frames * 2));
        for (int32_t &d : dst)
            d = (int32_t)any(rng);
        const float gain = (float)unit(rng);
        std::vector<int32_t> neon = dst, scalar = dst;
        neon_h_fold(neon.data(), src.data(), frames, gain);
        scalar_h_fold(scalar.data(), src.data(), frames, gain);
        EXPECT_EQ(neon, scalar) << "frames " << frames;
        std::vector<int32_t> expect = dst;
        for (uint32_t i = 0; i < frames; ++i) {
            expect[2 * i] = saturate(dst[2 * i] + fcvtzs31(src[4 * i] * gain));
            expect[2 * i + 1] = saturate(dst[2 * i + 1] + fcvtzs31(src[4 * i + 2] * gain));
        }
        EXPECT_EQ(neon, expect) << "frames " << frames;
    }
}

TEST(SwitchAudioFormat, FoldRingWrap)
{
    constexpr uint32_t ringFrames = 16;
    for (uint64_t first : {0ull, 5ull, 12ull, 15ull, 16ull, 16ull * 1000 + 13}) {
        for (uint32_t frames : {0u, 1u, 3u, 4u, 11u, 16u, 20u}) {
            std::vector<int32_t> ring(ringFrames * 2, 0);
            std::vector<float> src(std::max(1u, frames * 4), 0.0f);
            for (uint32_t i = 0; i < frames; ++i) {
                src[4 * i] = src[4 * i + 1] = (float)((i + 1) / 64.0);
                src[4 * i + 2] = src[4 * i + 3] = -(float)((i + 1) / 32.0);
            }
            const uint32_t run = neon_h_ring_fold(ring.data(), ringFrames, first, src.data(), frames, 1.0f);
            const uint32_t total = std::min(frames, ringFrames);
            EXPECT_EQ(run, std::min(total, ringFrames - (uint32_t)(first % ringFrames)))
                << "first " << first << " frames " << frames;
            std::vector<int64_t> expect(ringFrames * 2, 0);
            for (uint32_t i = 0; i < total; ++i) {
                const uint64_t at = (first + i) % ringFrames;
                expect[at * 2] += fcvtzs31(src[i * 4]);
                expect[at * 2 + 1] += fcvtzs31(src[i * 4 + 2]);
            }
            EXPECT_EQ(std::vector<int64_t>(ring.begin(), ring.end()), expect)
                << "first " << first << " frames " << frames;
        }
    }
}

TEST(SwitchAudioFormat, CaptureBackToFloat)
{
    EXPECT_EQ(neon_h_s32_to_float(INT32_MIN), -1.0f);
    EXPECT_EQ(neon_h_s32_to_float(1 << 30), 0.5f);
    EXPECT_EQ(neon_h_s32_to_float(0), 0.0f);
    std::mt19937_64 rng(5);
    std::uniform_int_distribution<int32_t> codec(-(1 << 23), (1 << 23) - 1);
    for (int i = 0; i < 2000; ++i) {
        const int32_t v24 = codec(rng);  // the codec's 24 bits, top-aligned in the slot
        const int32_t sample = (int32_t)((uint32_t)v24 << 8);
        const float x = neon_h_s32_to_float(sample);
        ASSERT_EQ((double)x, v24 / 8388608.0) << v24;  // exact
        ASSERT_EQ(mix({0}, {x})[0], sample) << v24;    // and back
    }
}

}  // namespace
