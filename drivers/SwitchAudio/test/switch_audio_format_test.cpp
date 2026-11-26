// SwitchAudio's S32 link, AHUB/codec helper words, and float32 <-> S32 conversions (NEON vs scalar bit-exact).
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

// FCVTNS on float32 x * (gain * 2^31): round to nearest even, saturated; NaN is 0.
int64_t fcvtns31(float x, float gain = 1.0f)
{
    const float v = x * (gain * 2147483648.0f);
    if (std::isnan(v))
        return 0;
    if (v >= 2147483648.0f)
        return kInt32Max;
    if (v <= -2147483648.0f)
        return kInt32Min;
    const int64_t r = (int64_t)std::rintf(v);
    return std::clamp(r, kInt32Min, kInt32Max);
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

TEST(SwitchAudioFormat, AdmaifAndAhubWordsAreL4tS32)
{
    for (uint32_t channels : {1u, 2u}) {
        EXPECT_EQ(neon_h_admaif_cif(channels), linux_::admaif("S32_LE", channels));
        EXPECT_EQ(neon_h_ahub_cif(channels), linux_::admaif("S32_LE", channels));
    }
    EXPECT_EQ(linux_::admaif("S16_LE", 2), (1u << 30) | 0x00113300u);
}

TEST(SwitchAudioFormat, I2sWordsAreL4tS32)
{
    const linux_::I2s i2s = linux_::i2s(32, kChannels, kRate);
    EXPECT_EQ(neon_h_i2s_ctrl() & 7, i2s.bitSize);
    EXPECT_EQ(neon_h_i2s_ctrl() & ~7u, (31u << 24) | (1u << 10));
    EXPECT_EQ(neon_h_i2s_timing(), i2s.timing);
    EXPECT_EQ(i2s.i2sClock, 3072000u);
    EXPECT_EQ(neon_h_bclk_fs() * kRate, i2s.i2sClock);
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
    EXPECT_EQ(codec->bclkFs, neon_h_bclk_fs());
    EXPECT_FALSE(linux_::rt5640(32, kChannels, kRate, 256 * kRate).has_value());
}

TEST(SwitchAudioFormat, FullScaleSaturationAndTiesToEven)
{
    const float inf = std::numeric_limits<float>::infinity(), nan = std::numeric_limits<float>::quiet_NaN();
    const std::vector<float> src = {1.0f,
                                    -1.0f,
                                    0.5f,
                                    -0.5f,
                                    0.0f,
                                    -0.0f,
                                    2.0f,
                                    -2.0f,
                                    nan,
                                    inf,
                                    -inf,
                                    std::ldexp(1.0f, -31),
                                    0.999999f,
                                    std::ldexp(0.5f, -31),
                                    std::ldexp(1.5f, -31),
                                    std::ldexp(2.5f, -31),
                                    std::ldexp(-1.5f, -31),
                                    std::ldexp(-2.5f, -31)};
    const std::vector<int32_t> neon = mix(std::vector<int32_t>(src.size(), 0), src, 1.0f, kNeon);
    const std::vector<int32_t> scalar = mix(std::vector<int32_t>(src.size(), 0), src, 1.0f, kScalar);
    EXPECT_EQ(neon, scalar);
    EXPECT_EQ(std::vector<int32_t>(neon.begin(), neon.begin() + 8),
              (std::vector<int32_t>{INT32_MAX, INT32_MIN, 1 << 30, -(1 << 30), 0, 0, INT32_MAX, INT32_MIN}));
    EXPECT_EQ(std::vector<int32_t>(neon.begin() + 8, neon.begin() + 11),
              (std::vector<int32_t>{0, INT32_MAX, INT32_MIN}));
    EXPECT_EQ(neon[11], 1);
    EXPECT_EQ(std::vector<int32_t>(neon.begin() + 13, neon.end()), (std::vector<int32_t>{0, 2, 2, -2, -2}));
    for (size_t i = 0; i < src.size(); ++i)
        EXPECT_EQ(neon[i], fcvtns31(src[i])) << "sample " << i;
}

TEST(SwitchAudioFormat, ContextsSumWithSaturation)
{
    const std::vector<float> a = {0.25f, 0.75f, -0.75f, 0.5f, -1.0f, 0.1f};
    const std::vector<float> b = {0.25f, 0.75f, -0.75f, -0.5f, -1.0f, -0.1f};
    const std::vector<int32_t> once = mix(std::vector<int32_t>(6, 0), a);
    const std::vector<int32_t> twice = mix(once, b);
    for (size_t i = 0; i < a.size(); ++i)
        EXPECT_EQ(twice[i], saturate(fcvtns31(a[i]) + fcvtns31(b[i]))) << i;
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
            expect[i] = saturate(dst[i] + fcvtns31(src[i], gain));
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
                    expect[at * channels + c] += fcvtns31(src[i * channels + c]);
            }
            EXPECT_EQ(std::vector<int64_t>(ring.begin(), ring.end()), expect)
                << "first " << first << " frames " << frames;
        }
    }
}

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
            expect[2 * i] = saturate(dst[2 * i] + fcvtns31(src[4 * i], gain));
            expect[2 * i + 1] = saturate(dst[2 * i + 1] + fcvtns31(src[4 * i + 2], gain));
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
                expect[at * 2] += fcvtns31(src[i * 4]);
                expect[at * 2 + 1] += fcvtns31(src[i * 4 + 2]);
            }
            EXPECT_EQ(std::vector<int64_t>(ring.begin(), ring.end()), expect)
                << "first " << first << " frames " << frames;
        }
    }
}

TEST(SwitchAudioFormat, CaptureVectorizedAndRingReadMatchScalar)
{
    EXPECT_EQ(neon_h_s32_to_float(INT32_MIN), -1.0f);
    EXPECT_EQ(neon_h_s32_to_float(1 << 30), 0.5f);
    EXPECT_EQ(neon_h_s32_to_float(0), 0.0f);
    std::mt19937_64 rng(5);
    std::uniform_int_distribution<int32_t> codec(-(1 << 23), (1 << 23) - 1);
    for (int i = 0; i < 2000; ++i) {
        const int32_t v24 = codec(rng);
        const int32_t sample = (int32_t)((uint32_t)v24 << 8);
        const float x = neon_h_s32_to_float(sample);
        ASSERT_EQ((double)x, v24 / 8388608.0) << v24;
        ASSERT_EQ(mix({0}, {x})[0], sample) << v24;
    }

    for (uint32_t n : {0u, 1u, 3u, 4u, 5u, 15u, 1025u}) {
        std::vector<int32_t> src(n);
        for (int32_t &s : src)
            s = (int32_t)((uint32_t)codec(rng) << 8);
        std::vector<float> neon(n), scalar(n);
        neon_h_s32_to_float_buf(neon.data(), src.data(), n);
        scalar_h_s32_to_float_buf(scalar.data(), src.data(), n);
        EXPECT_EQ(neon, scalar) << "n=" << n;
    }

    constexpr uint32_t ringFrames = 16;
    std::vector<int32_t> ring(ringFrames);
    for (uint32_t i = 0; i < ringFrames; ++i)
        ring[i] = (int32_t)((i + 1) << 24);
    std::vector<float> neon(24, 9.0f), scalar(24, 9.0f);
    // Anchor at 100, captured up to 118, reading [97, 121): 3 leading zeros, 18 wrapped samples, 3 trailing zeros.
    neon_h_ring_read_s32(neon.data(), ring.data(), ringFrames, 97, 24, 100, 118);
    scalar_h_ring_read_s32(scalar.data(), ring.data(), ringFrames, 97, 24, 100, 118);
    EXPECT_EQ(neon, scalar);
    for (uint32_t i = 0; i < 3; ++i)
        EXPECT_EQ(neon[i], 0.0f);
    for (uint32_t i = 0; i < 18; ++i)
        EXPECT_EQ(neon[3 + i], neon_h_s32_to_float(ring[i % ringFrames]));
    for (uint32_t i = 21; i < 24; ++i)
        EXPECT_EQ(neon[i], 0.0f);

    // Unanchored capture reads silence.
    neon_h_ring_read_s32(neon.data(), ring.data(), ringFrames, 100, 8, ~(uint64_t)0, 200);
    for (uint32_t i = 0; i < 8; ++i)
        EXPECT_EQ(neon[i], 0.0f);
}

TEST(SwitchAudioFormat, AhubAndStagingHelpers)
{
    EXPECT_EQ(neon_h_sfc_rate_index(8000u), 0);
    EXPECT_EQ(neon_h_sfc_rate_index(44100u), 6);
    EXPECT_EQ(neon_h_sfc_rate_index(48000u), 7);
    EXPECT_EQ(neon_h_sfc_rate_index(64000u), -1);
    EXPECT_EQ(neon_h_sfc_rate_index(96000u), 10);
    EXPECT_EQ(neon_h_is_44k_family(44100u), 1);
    EXPECT_EQ(neon_h_is_44k_family(88200u), 1);
    EXPECT_EQ(neon_h_is_44k_family(48000u), 0);
    EXPECT_EQ(neon_h_is_44k_family(96000u), 0);

    EXPECT_EQ(neon_h_mvc_linear_db_q8(0), 0);
    EXPECT_EQ(neon_h_mvc_linear_db_q8(-6000), -6 * 256);
    EXPECT_EQ(neon_h_mvc_linear_db_q8(-1500), -384);
    EXPECT_EQ(neon_h_mvc_linear_db_q8(-200000), -120 * 256);
    EXPECT_EQ(neon_h_mvc_linear_db_q8(60000), 40 * 256);

    EXPECT_EQ(neon_h_mbdrc_pack_thresh(180u, 120u, 24u, 12u), 0x0c1878b4u);
    EXPECT_EQ(neon_h_mbdrc_fast_factor(0x3000u, 0x0800u), 0x30000800u);

    uint8_t reg = 0;
    int32_t mvc = 0;
    neon_h_hp_stage_split(0u, &reg, &mvc);
    EXPECT_EQ(reg, 8u);
    EXPECT_EQ(mvc, 0);
    neon_h_hp_stage_split(750u, &reg, &mvc);
    EXPECT_EQ(reg, 8u);
    EXPECT_EQ(mvc, -750);
    neon_h_hp_stage_split(1500u, &reg, &mvc);
    EXPECT_EQ(reg, 9u);
    EXPECT_EQ(mvc, 0);
    neon_h_hp_stage_split(46500u, &reg, &mvc);
    EXPECT_EQ(reg, 39u);
    EXPECT_EQ(mvc, 0);
    neon_h_hp_stage_split(63750u, &reg, &mvc);
    EXPECT_EQ(reg, 39u);
    EXPECT_EQ(mvc, -17250);

    // Exact frames from ticks_per_frame_q32 over 10 hours at 19.2 MHz without 64-bit overflow.
    const uint64_t tpf48 = neon_h_clock_ticks_per_frame_q32(19200000ull, 38400000ull, 0u);
    const uint64_t ticks10h = 19200000ull * 36000ull;
    const uint64_t frames = neon_h_frames_from_ticks_q32(ticks10h, tpf48);
    const uint64_t expect = static_cast<uint64_t>(((__uint128_t)ticks10h << 32) / tpf48);
    EXPECT_EQ(frames, expect);
}

}  // namespace
