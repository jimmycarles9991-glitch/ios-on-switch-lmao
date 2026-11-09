// SPDX-License-Identifier: MIT
// third_party/mesa/src/nouveau/compiler/nak_nir_algebraic.py: where NAK leaves out metal2vulkan's fp32 denormal flush (a
// bcsel of integer ops giving a denormal's signed zero). SM50's FTZ instructions flush each float source and each result to
// a zero of its sign, as this Mac's FPCR.FZ does, which models them here: a flushed source under fneg or fabs reaches an
// FTZ instruction as the unflushed one does, for every denormal and every second operand class; a flush of what an FTZ
// instruction computes, or of a widened half, changes nothing.
#include <gtest/gtest.h>

#pragma STDC FENV_ACCESS ON

#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <vector>

namespace {

float asFloat(uint32_t u)
{
    float f;
    memcpy(&f, &u, 4);
    return f;
}

uint32_t bitsOf(float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

// metal2vulkan's flush as NIR has it: (x & 0x7f800000) == 0 ? x & 0x80000000 : x
uint32_t flushed(uint32_t x)
{
    return (x & 0x7f800000) == 0 ? x & 0x80000000 : x;
}

bool denormal(uint32_t x)
{
    return (x & 0x7f800000) == 0 && (x & 0x7fffff);
}

bool same(uint32_t a, uint32_t b)
{
    return a == b || (std::isnan(asFloat(a)) && std::isnan(asFloat(b)));
}

// FPCR.FZ on for the scope: AArch64 flushes denormal sources and results of float arithmetic, comparisons included, to
// zeros of their sign
class FlushToZero {
public:
    FlushToZero()
    {
        asm volatile("mrs %0, fpcr" : "=r"(saved_));
        const uint64_t fz = saved_ | (1ull << 24);
        asm volatile("msr fpcr, %0" ::"r"(fz) : "memory");
    }

    ~FlushToZero()
    {
        asm volatile("msr fpcr, %0" ::"r"(saved_) : "memory");
    }

    FlushToZero(const FlushToZero &) = delete;
    FlushToZero &operator=(const FlushToZero &) = delete;

private:
    uint64_t saved_;
};

// One value of each class an operand takes: zeros, denormals, normals near the bounds, ordinary values, infinities and NaNs
std::vector<uint32_t> operands()
{
    std::vector<uint32_t> v;
    for (uint32_t s : {0u, 0x80000000u})
        for (uint32_t m :
             {0x0u, 0x1u, 0x7fffffu, 0x800000u, 0x800001u, 0x3f000000u, 0x3f800000u, 0x7f800000u, 0x7fc00000u})
            v.push_back(s | m);
    return v;
}

// Every denormal and both zeros
template <typename F> void forEachFlushable(F f)
{
    for (uint32_t s : {0u, 0x80000000u})
        for (uint32_t m = 0; m < 0x800000; m++)
            f(s | m);
}

using Source = std::function<uint32_t(uint32_t)>;

std::vector<std::pair<const char *, Source>> modifiers()
{
    return {{"fneg",
             [](uint32_t x) {
                 return x ^ 0x80000000u;
             }},
            {"fabs", [](uint32_t x) {
                 return x & 0x7fffffffu;
             }}};
}

// Each binary FTZ instruction the rules name, with the operand in either position
std::vector<std::pair<const char *, std::function<uint32_t(float, float)>>> binaries()
{
    return {
        {"fadd",
         [](float a, float b) {
             return bitsOf(a + b);
         }},
        {"fmul",
         [](float a, float b) {
             return bitsOf(a * b);
         }},
        {"fmin",
         [](float a, float b) {
             return bitsOf(std::fmin(a, b));
         }},
        {"fmax",
         [](float a, float b) {
             return bitsOf(std::fmax(a, b));
         }},
        {"feq",
         [](float a, float b) {
             return uint32_t(a == b);
         }},
        {"fneu",
         [](float a, float b) {
             return uint32_t(a != b);
         }},
        {"flt",
         [](float a, float b) {
             return uint32_t(a < b);
         }},
        {"fge",
         [](float a, float b) {
             return uint32_t(a >= b);
         }},
    };
}

TEST(NakFp32Flush, AFlushedSourceUnderFnegOrFabsReachesAnFtzInstructionAsItsSourceDoes)
{
    const FlushToZero ftz;
    const std::vector<uint32_t> others = operands();
    const uint32_t fmaOthers[] = {0x0u, 0x80000001u, 0x800000u, 0x3f800000u, 0xbf000000u, 0x7f800000u, 0x7fc00000u};
    uint64_t checked = 0, differ = 0;
    for (const auto &modifier : modifiers()) {
        const Source &m = modifier.second;
        forEachFlushable([&](uint32_t a) {
            const float kept = asFloat(m(a)), cut = asFloat(m(flushed(a)));
            for (const auto &binary : binaries())
                for (uint32_t b : others) {
                    const auto &op = binary.second;
                    const float o = asFloat(b);
                    differ += !same(op(kept, o), op(cut, o)) + !same(op(o, kept), op(o, cut));
                    checked += 2;
                }
            for (uint32_t b : fmaOthers)
                for (uint32_t c : fmaOthers) {
                    const float x = asFloat(b), y = asFloat(c);
                    differ += !same(bitsOf(std::fma(kept, x, y)), bitsOf(std::fma(cut, x, y)));
                    differ += !same(bitsOf(std::fma(x, kept, y)), bitsOf(std::fma(x, cut, y)));
                    differ += !same(bitsOf(std::fma(x, y, kept)), bitsOf(std::fma(x, y, cut)));
                    checked += 3;
                }
            // fsat, f2i32 and f2u32: SM50 saturates and converts the flushed source
            const float sk = std::fmin(std::fmax(kept + 0.0f, 0.0f), 1.0f),
                        sc = std::fmin(std::fmax(cut + 0.0f, 0.0f), 1.0f);
            differ += !same(bitsOf(sk), bitsOf(sc)) + (int32_t(kept) != int32_t(cut));
            checked += 2;
        });
    }
    EXPECT_GT(checked, 1000000000u);
    EXPECT_EQ(differ, 0u);
}

TEST(NakFp32Flush, NoFtzResultIsADenormal)
{
    const FlushToZero ftz;
    volatile uint32_t tiny = 1;
    ASSERT_EQ(bitsOf(asFloat(tiny) * 1.0f), 0u);
    const std::vector<uint32_t> others = operands();
    uint64_t found = 0;
    // Sums and products landing in the denormal range from every denormal and each operand class, and from pairs of
    // normals one ulp apart around the smallest normals
    forEachFlushable([&](uint32_t a) {
        const float x = asFloat(a);
        for (uint32_t b : others) {
            const float y = asFloat(b);
            for (float r : {x + y, x * y, std::fma(x, y, y), std::fmin(x, y), std::fmax(x, y)})
                found += denormal(bitsOf(r));
        }
    });
    for (uint32_t m = 0x800000; m < 0x1800000; m++) {
        const float x = asFloat(m), y = asFloat(m + 1);
        for (float r : {y - x, x - y, x * 0.5f, std::fma(x, -0.5f, x * 0.25f), std::fma(y, 1.0f, -x)})
            found += denormal(bitsOf(r));
    }
    EXPECT_EQ(found, 0u);
}

TEST(NakFp32Flush, NoWidenedHalfIsADenormal)
{
    uint32_t found = 0;
    for (uint32_t h = 0; h < 65536; h++) {
        _Float16 x;
        const uint16_t bits = uint16_t(h);
        memcpy(&x, &bits, 2);
        const uint32_t w = bitsOf(float(x));
        found += denormal(w) || flushed(w) != w;
    }
    EXPECT_EQ(found, 0u);
}

}  // namespace
