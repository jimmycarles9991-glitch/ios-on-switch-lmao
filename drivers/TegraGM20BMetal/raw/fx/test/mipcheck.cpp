// SPDX-License-Identifier: MIT
// raw/fx host proof (r12-cafx) for fx_mip: GPU mip chains on the raw GM20B path.
//   1. layout: fx's chain layout against Mesa NIL's own (niloracle: the NVK host twin's vkGetImageSubresourceLayout and
//      vkGetImageMemoryRequirements), every level's offset, row pitch and size, the image size;
//   2. texture header: decoded back field by field; a texture-unit address model built only from the header's fields (base,
//      level-0 block height, width, height, levels: the hardware's per-LOD block-height clamp) must put every level where NIL does;
//   3. generation: fx's push words run by the 2D engine model (twod_sim) over host memory; the chain read back through the
//      header-only address model must equal the CPU reference chain, and the stream must order its steps (no hazard) and leave
//      the chain sampleable (3D wait + texture invalidate after the last write);
//   4. teeth: the checks must fail when the stream drops the per-level wait or invalidate, when a level's block height is not
//      clamped as the hardware does, and the device self-test must fail on a 2D engine that samples without the half-texel offset;
//   5. copy into a level; 6. CPU cost of encoding.
// usage: mipcheck <nil oracle output for BGRA8> [<for RGBA16F>]
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <vector>
#include "../fx_mip.h"
#include "twod_sim.h"

using namespace raw;
static int fails = 0, checks = 0;
#define CHECK(cond, ...)                               \
    do {                                               \
        checks++;                                      \
        if (!(cond)) {                                 \
            fails++;                                   \
            printf("FAIL %s:%d ", __FILE__, __LINE__); \
            printf(__VA_ARGS__);                       \
            printf("\n");                              \
        }                                              \
    } while (0)

// ---- 2. the texture unit's view, from the header alone ----
static uint64_t bits(const uint32_t *w, unsigned hi, unsigned lo)
{
    uint64_t v = 0;
    for (unsigned b = hi + 1; b-- > lo;)
        v = v << 1 | ((w[b / 32] >> (b % 32)) & 1);
    return v;
}

struct TicView {
    uint64_t va;
    uint32_t version, yLog2, maxLevel, width, height, type, srgb, minView, maxView, normalized, border, xGobs;
};

static TicView decode(const uint32_t w[8])
{
    TicView t;
    t.va = bits(w, 63, 41) << 9 | bits(w, 79, 64) << 32;
    t.version = uint32_t(bits(w, 87, 85));
    t.xGobs = uint32_t(bits(w, 98, 96));
    t.yLog2 = uint32_t(bits(w, 101, 99));
    t.maxLevel = uint32_t(bits(w, 127, 124));
    t.width = uint32_t(bits(w, 143, 128)) + 1;
    t.srgb = uint32_t(bits(w, 150, 150));
    t.type = uint32_t(bits(w, 154, 151));
    t.border = uint32_t(bits(w, 159, 157));
    t.height = uint32_t(bits(w, 175, 160)) + 1;
    t.normalized = uint32_t(bits(w, 191, 191));
    t.minView = uint32_t(bits(w, 227, 224));
    t.maxView = uint32_t(bits(w, 231, 228));
    return t;
}

// the level's block height the hardware uses: the header's, clamped below 2x the level's height in GOBs
static uint32_t levelYLog2(const TicView &t, uint32_t l)
{
    const uint32_t h = std::max(1u, t.height >> l), gobs = (h + 7) / 8;
    uint32_t c = 0;
    while ((1u << c) < gobs)
        c++;
    return std::min(t.yLog2, c);
}

static uint64_t levelBytes(const TicView &t, uint32_t bpp, uint32_t l)
{
    const uint32_t w = std::max(1u, t.width >> l), h = std::max(1u, t.height >> l), y = levelYLog2(t, l);
    const uint64_t rowB = (uint64_t(w) * bpp + 63) / 64 * 64, rows = (h + (8u << y) - 1) / (8u << y) * (8u << y);
    return rowB * rows;
}

static uint64_t levelBase(const TicView &t, uint32_t bpp, uint32_t l)
{
    uint64_t at = 0;
    for (uint32_t i = 0; i < l; i++)
        at += levelBytes(t, bpp, i);
    return at;
}

static uint64_t ticTexel(const TicView &t, uint32_t bpp, uint32_t l, uint32_t x, uint32_t y)
{
    const uint32_t w = std::max(1u, t.width >> l), yl = levelYLog2(t, l), xb = x * bpp;
    const uint32_t gobsPerRow = (w * bpp + 63) / 64, blockRows = 8u << yl;
    const uint64_t block = uint64_t(y / blockRows) * gobsPerRow + xb / 64;
    const uint32_t inGob =
        ((xb % 64) / 32) * 256 + ((y % 8) / 2) * 64 + ((xb % 32) / 16) * 32 + (y % 2) * 16 + (xb % 16);
    return t.va + levelBase(t, bpp, l) + block * (512ull << yl) + uint64_t((y % blockRows) / 8) * 512 + inGob;
}

// ---- 1. NIL oracle ----
struct Nil {
    uint32_t w, h, l;
    uint64_t size, align;
    std::vector<uint64_t> off, row, lsize;
};

static std::vector<Nil> readNil(const char *path)
{
    std::vector<Nil> out;
    FILE *f = fopen(path, "r");
    if (!f)
        return out;
    char line[8192];
    while (fgets(line, sizeof line, f)) {
        Nil n;
        unsigned long long s, a;
        if (sscanf(line, "%u %u %u %llu %llu", &n.w, &n.h, &n.l, &s, &a) != 5)
            continue;
        n.size = s;
        n.align = a;
        const char *bar = strchr(line, '|');
        std::istringstream in(bar ? bar + 1 : "");
        std::string tok;
        while (in >> tok) {
            unsigned long long o, r, z;
            if (sscanf(tok.c_str(), "%llu,%llu,%llu", &o, &r, &z) == 3) {
                n.off.push_back(o);
                n.row.push_back(r);
                n.lsize.push_back(z);
            }
        }
        out.push_back(n);
    }
    fclose(f);
    return out;
}

static void testLayout(const char *path, uint32_t bpp, uint32_t mtl)
{
    const std::vector<Nil> nil = readNil(path);
    CHECK(!nil.empty(), "no NIL oracle lines in %s", path);
    uint32_t images = 0, levels = 0;
    for (const Nil &n : nil) {
        fx::MipChain c;
        const bool ok = fx::mipChainLayout(n.w, n.h, n.l, bpp, c);
        CHECK(ok, "layout refused %ux%ux%u", n.w, n.h, n.l);
        if (!ok)
            continue;
        images++;
        CHECK(c.bytes == n.size, "%ux%ux%u bpp %u: size fx %llu NIL %llu", n.w, n.h, n.l, bpp,
              (unsigned long long)c.bytes, (unsigned long long)n.size);
        CHECK(n.align % c.align == 0, "%ux%ux%u: NVK alignment %llu not a multiple of fx's %llu", n.w, n.h, n.l,
              (unsigned long long)n.align, (unsigned long long)c.align);
        uint32_t tic[8];
        fx::mipChainTic(c, 0x2000000000ull, 0x54e24908, false, 0, n.l, tic);
        const TicView t = decode(tic);
        for (uint32_t l = 0; l < n.l && l < n.off.size(); l++) {
            levels++;
            CHECK(c.level[l].offset == n.off[l], "%ux%ux%u level %u: offset fx %llu NIL %llu", n.w, n.h, n.l, l,
                  (unsigned long long)c.level[l].offset, (unsigned long long)n.off[l]);
            CHECK(c.level[l].rowBytes == n.row[l], "%ux%ux%u level %u: row fx %u NIL %llu", n.w, n.h, n.l, l,
                  c.level[l].rowBytes, (unsigned long long)n.row[l]);
            CHECK(uint64_t(c.level[l].rowBytes) * c.level[l].rows == n.lsize[l],
                  "%ux%ux%u level %u: size fx %llu NIL %llu", n.w, n.h, n.l, l,
                  (unsigned long long)c.level[l].rowBytes * c.level[l].rows, (unsigned long long)n.lsize[l]);
            CHECK(levelBase(t, bpp, l) == n.off[l], "%ux%ux%u level %u: header-only model %llu NIL %llu", n.w, n.h, n.l,
                  l, (unsigned long long)levelBase(t, bpp, l), (unsigned long long)n.off[l]);
        }
    }
    printf("layout (format %u, %u B/texel): %u images, %u levels against NIL\n", mtl, bpp, images, levels);
}

// ---- 2. header fields ----
static void testTic()
{
    fx::MipChain c;
    fx::mipChainLayout(776, 24, 10, 4, c);
    uint32_t w[8];
    fx::mipChainTic(c, 0x12345678000ull, 0x54e24908, true, 0, 10, w);
    TicView t = decode(w);
    CHECK(w[0] == 0x54e24908, "word 0");
    CHECK(t.va == 0x12345678000ull, "address %llx", (unsigned long long)t.va);
    CHECK(t.version == 3 && t.type == 1, "BL header, TWO_D");
    CHECK(t.yLog2 == c.yLog2 && c.yLog2 == 2, "block height %u", t.yLog2);
    CHECK(t.maxLevel == 9 && t.minView == 0 && t.maxView == 9, "levels %u %u %u", t.maxLevel, t.minView, t.maxView);
    CHECK(t.width == 776 && t.height == 24 && t.srgb == 1 && t.normalized == 1 && t.border == 7 && t.xGobs == 0,
          "fields");
    fx::mipChainTic(c, 0x12345678000ull, 0x54e24908, false, 2, 3, w);  // a view of levels 2..4
    t = decode(w);
    CHECK(t.maxLevel == 9 && t.minView == 2 && t.maxView == 4 && t.srgb == 0, "view %u %u %u", t.maxLevel, t.minView,
          t.maxView);
    fx::mipChainTic(c, 0x12345678000ull, 0x54e24908, false, 0, 1, w);  // one-level view of level 0: NIL max_mip_level 0
    t = decode(w);
    CHECK(t.maxLevel == 0 && t.maxView == 0, "single-level view %u %u", t.maxLevel, t.maxView);
}

// ---- 3. generation through the 2D engine model ----
struct Run {
    std::vector<uint8_t> pitch, chain;
    std::vector<uint32_t> push;
    fx::MipChain c;
    fx::Pitch l0;
    uint64_t chainVa = 0x2000000000ull;
};

static void fill(std::vector<uint8_t> &v, uint32_t seed)
{
    std::mt19937 r(seed);
    for (auto &b : v)
        b = uint8_t(r());
}

static bool prepare(Run &run, uint32_t w, uint32_t h, uint32_t levels, uint32_t seed)
{
    if (!fx::mipChainLayout(w, h, levels, 4, run.c))
        return false;
    run.l0 = {0x1000000000ull, (w * 4 + 255) & ~255u, w, h};
    run.pitch.assign(size_t(run.l0.pitch) * h, 0);
    fill(run.pitch, seed);
    run.chain.assign(run.c.bytes, 0xcd);
    run.push.assign(fx::twodInitWords() + fx::generateWords(run.c, 0, levels - 1) + 16, 0);
    return true;
}

static size_t emit(Run &run, uint32_t levels)
{
    Push p{run.push.data(), run.push.data() + run.push.size()};
    fx::emitTwodInit(p);
    fx::emitGenerate(p, run.c, run.chainVa, &run.l0, 0xcf, 0, levels - 1);
    return size_t(p.at - run.push.data());
}

// the CPU reference chain from level 0 (each level from the reference level above)
static std::vector<std::vector<uint8_t>> reference(const Run &run)
{
    std::vector<std::vector<uint8_t>> lv(run.c.levels);
    lv[0].resize(size_t(run.c.width) * run.c.height * 4);
    for (uint32_t y = 0; y < run.c.height; y++)
        memcpy(&lv[0][size_t(y) * run.c.width * 4], &run.pitch[size_t(y) * run.l0.pitch], run.c.width * 4);
    for (uint32_t l = 1; l < run.c.levels; l++) {
        const fx::MipLevel &u = run.c.level[l - 1], &d = run.c.level[l];
        lv[l].resize(size_t(d.width) * d.height * 4);
        fx::referenceDownsample(lv[l - 1].data(), u.width * 4, u.width, u.height, lv[l].data(), d.width * 4, d.width,
                                d.height, 4);
    }
    return lv;
}

// compares the chain the model wrote, read through the header-only address model, with the reference: bytes that differ
static uint64_t compare(const Run &run, const std::vector<std::vector<uint8_t>> &ref)
{
    uint32_t tic[8];
    fx::mipChainTic(run.c, run.chainVa, 0x54e24908, false, 0, run.c.levels, tic);
    const TicView t = decode(tic);
    uint64_t bad = 0;
    for (uint32_t l = 0; l < run.c.levels; l++) {
        const fx::MipLevel &lv = run.c.level[l];
        for (uint32_t y = 0; y < lv.height; y++)
            for (uint32_t x = 0; x < lv.width; x++) {
                const uint64_t a = ticTexel(t, 4, l, x, y) - run.chainVa;
                if (a + 4 > run.chain.size() || memcmp(&run.chain[a], &ref[l][(size_t(y) * lv.width + x) * 4], 4))
                    bad++;
                // and fx's own CPU-view addressing agrees with the header model
                if (fx::texelOffset(run.c, l, x * 4, y) != a)
                    bad++;
            }
    }
    return bad;
}

static void testGenerate()
{
    const uint32_t sizes[][2] = {{776, 24}, {180, 180}, {120, 120},   {60, 60},  {1280, 720}, {720, 1280},
                                 {97, 3},   {3, 97},    {1, 1},       {2, 1},    {1024, 1},   {1, 64},
                                 {33, 65},  {256, 8},   {1366, 1024}, {512, 512}};
    uint32_t images = 0;
    uint64_t texels = 0;
    for (auto &s : sizes) {
        Run run;
        const uint32_t levels = fx::fullLevels(s[0], s[1]);
        CHECK(prepare(run, s[0], s[1], levels, s[0] * 131 + s[1]), "prepare %ux%u", s[0], s[1]);
        const size_t n = emit(run, levels);
        CHECK(n <= run.push.size() - 16, "%ux%u: %zu words over the bound %zu", s[0], s[1], n, run.push.size() - 16);
        twodsim::Sim sim;
        sim.map(run.l0.va, run.pitch);
        sim.map(run.chainVa, run.chain);
        const bool ok = sim.run(run.push.data(), n);
        CHECK(ok, "%ux%u: model: %s", s[0], s[1], sim.error().c_str());
        CHECK(sim.launches == levels, "%ux%u: %u launches for %u levels", s[0], s[1], sim.launches, levels);
        CHECK(sim.endsSampleable, "%ux%u: not left sampleable (3D wait + texture invalidate after the last write)",
              s[0], s[1]);
        const uint64_t bad = compare(run, reference(run));
        CHECK(bad == 0, "%ux%u: %llu texels differ from the reference chain", s[0], s[1], (unsigned long long)bad);
        images++;
        for (uint32_t l = 0; l < levels; l++)
            texels += uint64_t(run.c.level[l].width) * run.c.level[l].height;
    }
    printf("generate: %u chains (%llu texels) through the 2D engine model equal the reference, ordered, sampleable\n",
           images, (unsigned long long)texels);
}

// ---- 4. teeth ----
// The device sweep's source: 24 x 10 BGRA8 texels whose components tell the four 2x2 neighbours apart at every step (a one-texel
// shift changes the result by more than 1); 24x10 -> 12x5 -> 6x2 -> 3x1 -> 1x1, odd steps on purpose
constexpr uint32_t kSweepW = 24, kSweepH = 10;

static void sweepPattern(uint8_t *pixels, uint32_t pitch)
{
    for (uint32_t y = 0; y < kSweepH; y++)
        for (uint32_t x = 0; x < kSweepW; x++) {
            uint8_t *t = pixels + size_t(y) * pitch + x * 4;
            t[0] = uint8_t(x * 10 + 7);
            t[1] = uint8_t(y * 23 + 5);
            t[2] = uint8_t((x * 37 + y * 101) & 0xff);
            t[3] = uint8_t(((x ^ y) & 1) ? 255 : 64);
        }
}

// Bytes off by more than 1, each level against the reference step from the level above as the engine made it (so one step's
// rounding is not carried), read in the Tegra or Fermi view; *first: the first one's level << 24 | y << 12 | x
static uint32_t sweepCheck(const fx::MipChain &c, const uint8_t *chainCpu, uint32_t *first, bool fermi)
{
    *first = ~0u;
    uint8_t want[kSweepW * kSweepH * 4], above[kSweepW * kSweepH * 4];
    sweepPattern(want, kSweepW * 4);
    uint32_t bad = 0;
    for (uint32_t l = 0; l < c.levels; l++) {
        const fx::MipLevel &lv = c.level[l];
        if (l) {
            const fx::MipLevel &up = c.level[l - 1];
            for (uint32_t y = 0; y < up.height; y++)
                for (uint32_t xb = 0; xb < up.width * 4; xb++)
                    above[size_t(y) * up.width * 4 + xb] = chainCpu[fx::texelOffset(c, l - 1, xb, y, fermi)];
            fx::referenceDownsample(above, up.width * 4, up.width, up.height, want, lv.width * 4, lv.width, lv.height,
                                    4);
        }
        for (uint32_t y = 0; y < lv.height; y++)
            for (uint32_t xb = 0; xb < lv.width * 4; xb++)
                if (std::abs(int(chainCpu[fx::texelOffset(c, l, xb, y, fermi)]) -
                             int(want[size_t(y) * lv.width * 4 + xb])) > 1) {
                    if (!bad)
                        *first = l << 24 | y << 12 | xb;
                    bad++;
                }
    }
    return bad;
}

// The sweep's chain generated with `sampling` on the 2D engine model, documented or as measured
static void sweepChain(fx::Sampling sampling, bool measured, fx::MipChain &c, std::vector<uint8_t> &chain)
{
    fx::mipChainLayout(kSweepW, kSweepH, fx::fullLevels(kSweepW, kSweepH), 4, c);
    std::vector<uint8_t> pitch(256 * kSweepH);
    chain.assign(c.bytes, 0);
    sweepPattern(pitch.data(), 256);
    std::vector<uint32_t> push(fx::twodInitWords() + fx::generateWords(c, 0, c.levels - 1));
    Push p{push.data(), push.data() + push.size()};
    fx::emitTwodInit(p);
    const fx::Pitch l0{0x1000000000ull, 256, kSweepW, kSweepH};
    fx::emitGenerate(p, c, 0x2000000000ull, &l0, 0xcf, 0, c.levels - 1, sampling);
    twodsim::Sim sim;
    sim.quirkCentreHalfTexel = measured;
    sim.map(l0.va, pitch);
    sim.map(0x2000000000ull, chain);
    sim.run(push.data(), size_t(p.at - push.data()));
}

static uint32_t sweepBad(fx::Sampling sampling, bool measured)
{
    fx::MipChain c;
    std::vector<uint8_t> chain;
    sweepChain(sampling, measured, c, chain);
    uint32_t first;
    return sweepCheck(c, chain.data(), &first, false);
}

static void testTeeth()
{
    // (a) the per-level 2D wait dropped: the next level reads what the last wrote before it is done
    {
        Run run;
        prepare(run, 180, 180, 8, 7);
        size_t n = emit(run, 8);
        for (size_t i = 0; i < n; i++)
            if (run.push[i] == (4u << 29 | 0u << 16 | 3u << 13 | (0x0110 >> 2)))
                run.push[i] = 4u << 29 | 3u << 13 | (0x0100 >> 2);
        twodsim::Sim sim;
        sim.map(run.l0.va, run.pitch);
        sim.map(run.chainVa, run.chain);
        CHECK(!sim.run(run.push.data(), n) && sim.error().find("hazard") != std::string::npos,
              "teeth (a): a stream without the per-level wait passed");
    }
    // (b) the texture cache invalidate dropped
    {
        Run run;
        prepare(run, 180, 180, 8, 7);
        size_t n = emit(run, 8);
        for (size_t i = 0; i < n; i++)
            if (run.push[i] == (4u << 29 | 0u << 16 | 3u << 13 | (0x0240 >> 2)))
                run.push[i] = 4u << 29 | 3u << 13 | (0x0100 >> 2);
        twodsim::Sim sim;
        sim.map(run.l0.va, run.pitch);
        sim.map(run.chainVa, run.chain);
        CHECK(!sim.run(run.push.data(), n), "teeth (b): a stream without the texture cache invalidate passed");
    }
    // (c) a writer that does not clamp each level's block height as the hardware does: the texture unit reads elsewhere
    {
        Run run;
        prepare(run, 776, 24, 10, 9);
        fx::MipChain wrong = run.c;
        uint64_t at = 0;
        for (uint32_t l = 0; l < wrong.levels; l++) {  // every level with level 0's block height, offsets accordingly
            fx::MipLevel &lv = wrong.level[l];
            lv.yLog2 = wrong.yLog2;
            lv.rows = (lv.height + (8u << lv.yLog2) - 1) / (8u << lv.yLog2) * (8u << lv.yLog2);
            lv.offset = at;
            at += uint64_t(lv.rowBytes) * lv.rows;
        }
        run.chain.assign(std::max<uint64_t>(at, run.c.bytes) + 65536, 0xcd);
        Push p{run.push.data(), run.push.data() + run.push.size()};
        fx::emitTwodInit(p);
        fx::emitGenerate(p, wrong, run.chainVa, &run.l0, 0xcf, 0, 9);
        twodsim::Sim sim;
        sim.map(run.l0.va, run.pitch);
        sim.map(run.chainVa, run.chain);
        sim.run(run.push.data(), size_t(p.at - run.push.data()));
        CHECK(compare(run, reference(run)) > 0, "teeth (c): an unclamped layout read back equal");
    }
    // (d) the device's sweep (fx::kGm20bSampling's comment): the eight samplings its render server tried at every boot that ran
    // the sweep, and the bytes below level 0 each made wrong in the Tegra view. The 2D engine model as measured gives the same
    // counts; kGm20bSampling is exact on it and on the documented engine alike, and the documented sampling is not on the device.
    {
        static const fx::Sampling kSwept[8] = {{0, 0, 0}, {0, 0, -1}, {1, 1, -1}, {1, 1, 0},
                                               {0, 1, 0}, {0, 0, 1},  {1, 0, 0},  {0, 1, -1}};
        static const uint32_t kDevice[8] = {287, 230, 287, 0, 287, 26, 230, 0};
        uint32_t counts[8];
        for (int i = 0; i < 8; i++)
            counts[i] = sweepBad(kSwept[i], true);
        CHECK(!memcmp(counts, kDevice, sizeof counts),
              "the measured model's sweep %u %u %u %u %u %u %u %u is not the device's 287 230 287 0 287 26 230 0",
              counts[0], counts[1], counts[2], counts[3], counts[4], counts[5], counts[6], counts[7]);
        CHECK(sweepBad(fx::kGm20bSampling, true) == 0 && sweepBad(fx::kGm20bSampling, false) == 0,
              "kGm20bSampling is not exact on both engines");
        CHECK(sweepBad(fx::Sampling{}, true) > 0, "teeth (d): the documented sampling passed on the measured engine");
        printf("sweep: measured model %u %u %u %u %u %u %u %u (the device's), kGm20bSampling exact\n", counts[0],
               counts[1], counts[2], counts[3], counts[4], counts[5], counts[6], counts[7]);
    }
    // the CPU's view of the chain: right in the Tegra arrangement is wrong in the Fermi one, and both are permutations
    {
        fx::MipChain c;
        std::vector<uint8_t> chain;
        sweepChain(fx::kGm20bSampling, false, c, chain);
        uint32_t f2;
        CHECK(sweepCheck(c, chain.data(), &f2, true) > 0, "the sweep's Fermi view does not tell the views apart");
        std::vector<uint8_t> seen(512, 0);
        for (uint32_t y = 0; y < 8; y++)
            for (uint32_t x = 0; x < 64; x++)
                seen[fx::gobOffsetFermi(x, y)]++, seen[fx::gobOffset(x, y)] += 2;
        CHECK(std::count(seen.begin(), seen.end(), 3) == 512, "GOB arrangements are not permutations");
    }
}

// ---- 5. copy into a level ----
static void testCopy()
{
    Run run;
    prepare(run, 180, 180, 8, 3);
    const uint32_t L = 2, sx = 5, sy = 7, dx = 3, dy = 11, w = 30, h = 20;
    std::vector<uint32_t> push(fx::copyWords() + 8);
    Push p{push.data(), push.data() + push.size()};
    fx::emitCopyToLevel(p, run.c, run.chainVa, L, run.l0, 0xcf, sx, sy, dx, dy, w, h);
    twodsim::Sim sim;
    sim.map(run.l0.va, run.pitch);
    sim.map(run.chainVa, run.chain);
    CHECK(sim.run(push.data(), size_t(p.at - push.data())), "copy: %s", sim.error().c_str());
    uint64_t bad = 0;
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++)
            bad += memcmp(&run.chain[fx::texelOffset(run.c, L, (dx + x) * 4, dy + y)],
                          &run.pitch[size_t(sy + y) * run.l0.pitch + (sx + x) * 4], 4) != 0;
    CHECK(bad == 0 && sim.endsSampleable, "copy into level %u: %llu texels differ", L, (unsigned long long)bad);
}

// ---- 6. CPU cost ----
static void bench()
{
    const uint32_t sizes[][2] = {{60, 60}, {180, 180}, {776, 24}, {1280, 720}};
    std::vector<uint32_t> push(4096);
    for (auto &s : sizes) {
        fx::MipChain c;
        const uint32_t levels = fx::fullLevels(s[0], s[1]);
        const fx::Pitch l0{0x1000000000ull, (s[0] * 4 + 255) & ~255u, s[0], s[1]};
        const int N = 200000;
        size_t words = 0;
        auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < N; i++) {
            fx::mipChainLayout(s[0], s[1], levels, 4, c);
            Push p{push.data(), push.data() + push.size()};
            fx::emitGenerate(p, c, 0x2000000000ull + uint64_t(i) * 4096, &l0, 0xcf, 0, levels - 1);
            words = size_t(p.at - push.data());
        }
        auto t1 = std::chrono::steady_clock::now();
        uint32_t tic[8];
        for (int i = 0; i < N; i++)
            fx::mipChainTic(c, 0x2000000000ull + uint64_t(i) * 4096, 0x54e24908, false, 0, levels, tic);
        auto t2 = std::chrono::steady_clock::now();
        const double gen = std::chrono::duration<double, std::nano>(t1 - t0).count() / N,
                     ticNs = std::chrono::duration<double, std::nano>(t2 - t1).count() / N;
        uint64_t traffic = uint64_t(s[0]) * s[1] * 4 * 2;  // level 0: read pitch, write chain
        for (uint32_t l = 1; l < levels; l++)
            traffic += uint64_t(c.level[l - 1].width) * c.level[l - 1].height * 4 +
                       uint64_t(c.level[l].width) * c.level[l].height * 4;
        printf("bench %4ux%-4u %2u levels: layout+generate %6.0f ns, header %4.0f ns (host), %3zu push words, chain "
               "%7llu B, "
               "GPU traffic %8llu B\n",
               s[0], s[1], levels, gen, ticNs, words, (unsigned long long)c.bytes, (unsigned long long)traffic);
    }
}

int main(int argc, char **argv)
{
    if (argc > 1)
        testLayout(argv[1], 4, 80);
    if (argc > 2)
        testLayout(argv[2], 8, 115);
    testTic();
    testGenerate();
    testTeeth();
    testCopy();
    bench();
    printf("%s: %d checks, %d failed\n", fails ? "FAIL" : "PASS", checks, fails);
    return fails ? 1 : 0;
}
