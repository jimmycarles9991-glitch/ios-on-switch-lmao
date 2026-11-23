// SPDX-License-Identifier: MIT
// raw GM20B path: colour targets of formats the GM20B renders none of (runtime/format_standin.h), rendered into an
// RGBA32Float stand-in. The load states fill the stand-in with the texture's texels as floats (RGB9E5Float's decoded from
// its words); each store state writes the stand-in's texels into the texture's words, as an M4 Max stores a drawn texel:
// A1BGR5Unorm and ABGR4Unorm each channel's nearest half, a tie to even, clamped to [0, 1] (a NaN 0), then its nearest code;
// RGB9E5Float each channel clamped to [0, 65408] (a NaN 0), the largest's exponent shared, every mantissa truncated. A
// clear goes into the stand-in as floats these give back (formatStandInClear). The extended-range formats' are
// pack/xr_target.metal's.
#include <metal_stdlib>
using namespace metal;

struct Out {
    float4 position [[position]];
};

vertex Out fmt_vert(uint vid [[vertex_id]])
{
    Out o;
    o.position = float4((vid & 1) ? 1.0 : -1.0, (vid & 2) ? 1.0 : -1.0, 0.0, 1.0);
    return o;
}

fragment float4 fmt_load_frag(Out in [[stage_in]], texture2d<float, access::read> t [[texture(0)]])
{
    return t.read(uint2(in.position.xy));
}

static float3 rgb9e5Floats(uint w)
{
    return ldexp(float3(uint3(w, w >> 9, w >> 18) & 511u), int3(int(w >> 27) - 24));
}

fragment float4 fmt_load_rgb9e5_frag(Out in [[stage_in]], texture2d<uint, access::read> t [[texture(0)]])
{
    return float4(rgb9e5Floats(t.read(uint2(in.position.xy)).r), 1.0f);
}

static uint drawnCode(float v, float max)
{
    const float h = float(half(v));
    return h > 0.0f ? uint(rint(min(h, 1.0f) * max)) : 0u;
}

static float4 texel(Out in, texture2d<float, access::read> s)
{
    return s.read(uint2(in.position.xy));
}

static uint abgr4Word(float4 v)
{
    return drawnCode(v.r, 15) << 12 | drawnCode(v.g, 15) << 8 | drawnCode(v.b, 15) << 4 | drawnCode(v.a, 15);
}

// BGRA4Unorm (NIL's B4G4R4A4_UNORM): blue in the low four bits, alpha in the high
static uint bgra4Word(float4 v)
{
    return drawnCode(v.a, 15) << 12 | drawnCode(v.r, 15) << 8 | drawnCode(v.g, 15) << 4 | drawnCode(v.b, 15);
}

static uint a1bgr5Word(float4 v)
{
    return drawnCode(v.r, 31) << 11 | drawnCode(v.g, 31) << 6 | drawnCode(v.b, 31) << 1 | drawnCode(v.a, 1);
}

// Each channel clamped to [0, 65408] (a NaN 0), the largest's exponent shared, every mantissa truncated, or rounded to
// nearest where `rounded`
static uint rgb9e5Encode(float3 v, bool rounded)
{
    float3 c;
    for (int k = 0; k < 3; k++)
        c[k] = v[k] > 0.0f ? min(v[k], 65408.0f) : 0.0f;
    const float m = max(c.r, max(c.g, c.b));
    int e = m > 0.0f ? max(0, int(as_type<uint>(m) >> 23 & 255) - 127 + 16) : 0;
    if (rounded && floor(ldexp(m, 24 - e) + 0.5f) >= 512.0f)
        e++;
    const float3 scaled = ldexp(c, int3(24 - e));
    const uint3 mantissa = uint3(rounded ? floor(scaled + 0.5f) : floor(scaled));
    return mantissa.r | mantissa.g << 9 | mantissa.b << 18 | uint(e) << 27;
}

fragment uint fmt_store_abgr4_frag(Out in [[stage_in]], texture2d<float, access::read> s [[texture(0)]])
{
    return abgr4Word(texel(in, s));
}

fragment uint fmt_store_bgra4_frag(Out in [[stage_in]], texture2d<float, access::read> s [[texture(0)]])
{
    return bgra4Word(texel(in, s));
}

fragment uint fmt_store_a1bgr5_frag(Out in [[stage_in]], texture2d<float, access::read> s [[texture(0)]])
{
    return a1bgr5Word(texel(in, s));
}

fragment uint fmt_store_rgb9e5_frag(Out in [[stage_in]], texture2d<float, access::read> s [[texture(0)]])
{
    return rgb9e5Encode(texel(in, s).rgb, false);
}

constant ushort kSrgbFirst[640] = {
#include "xr_srgb_encode.h"
};

// A multisampled target's stand-in (runtime/format_standin.h bySamples): each sample filled from and stored into
// that sample of the texture, the fragment run once a sample; a function a sample count, as the pack keys an own state
// by its function's name
typedef texture2d_ms<float, access::read> Samples;

#define FMT_PER_SAMPLE(count)                                                                                      \
    fragment float4 fmt_load_ms##count##_frag(Out in [[stage_in]], uint k [[sample_id]], Samples t [[texture(0)]]) \
    {                                                                                                              \
        return t.read(uint2(in.position.xy), k);                                                                   \
    }                                                                                                              \
    fragment float4 fmt_load_rgb9e5_ms##count##_frag(Out in [[stage_in]], uint k [[sample_id]],                    \
                                                     texture2d_ms<uint, access::read> t [[texture(0)]])            \
    {                                                                                                              \
        return float4(rgb9e5Floats(t.read(uint2(in.position.xy), k).r), 1.0f);                                     \
    }                                                                                                              \
    fragment uint fmt_store_abgr4_ms##count##_frag(Out in [[stage_in]], uint k [[sample_id]],                      \
                                                   Samples s [[texture(0)]])                                       \
    {                                                                                                              \
        return abgr4Word(s.read(uint2(in.position.xy), k));                                                        \
    }                                                                                                              \
    fragment uint fmt_store_bgra4_ms##count##_frag(Out in [[stage_in]], uint k [[sample_id]],                      \
                                                   Samples s [[texture(0)]])                                       \
    {                                                                                                              \
        return bgra4Word(s.read(uint2(in.position.xy), k));                                                        \
    }                                                                                                              \
    fragment uint fmt_store_a1bgr5_ms##count##_frag(Out in [[stage_in]], uint k [[sample_id]],                     \
                                                    Samples s [[texture(0)]])                                      \
    {                                                                                                              \
        return a1bgr5Word(s.read(uint2(in.position.xy), k));                                                       \
    }                                                                                                              \
    fragment uint fmt_store_rgb9e5_ms##count##_frag(Out in [[stage_in]], uint k [[sample_id]],                     \
                                                    Samples s [[texture(0)]])                                      \
    {                                                                                                              \
        return rgb9e5Encode(s.read(uint2(in.position.xy), k).rgb, false);                                          \
    }

FMT_PER_SAMPLE(2)
FMT_PER_SAMPLE(4)

// RGB9E5Float's resolve (runtime/attachment_target.h formatResolveState), of its multisampled stand-in into a
// one-sample one: an M4 Max's tile holds each sample as the word a draw stores (mantissas truncated), and its resolve is
// the samples' decoded mean rounded to the nearest word, which the store keeps as it is. The count comes in buffer 0
fragment float4 fmt_resolve_rgb9e5_frag(Out in [[stage_in]], Samples s [[texture(0)]], constant uint &n [[buffer(0)]])
{
    const uint2 at = uint2(in.position.xy);
    float3 sum = 0.0f;
    for (uint k = 0; k < n; k++)
        sum += rgb9e5Floats(rgb9e5Encode(s.read(at, k).rgb, false));
    return float4(rgb9e5Floats(rgb9e5Encode(sum / float(n), true)), 1.0f);
}

// The 2D word mip states' buffer 0: the level above's size and the level's
struct Step {
    uint2 above, size;
};

// Word mip states (runtime/mip.h wordMipState, wordMip3dState; format_standin.h formatMipState): a level of a
// format whose words the GM20B renders but cannot filter, each texel the word an Apple M4 Max's
// generateMipmapsForTexture: writes (m2v bind/mipmap_filter model.rs, checked word for word against an M4 Max under
// macOS 27.2.1: harness/miporacle.mm). Every step is in integers, so no GPU's float rounding or fused multiply-add moves
// a word: the taps are the M4's f32 coordinates (its correctly rounded reciprocal of the level's size, each product
// rounded to nearest even, the fraction rounded half up to 1/256), emulated; each field sums its taps' codes, decoded
// steps or values on Metal's grid for the format.
constant ushort kSrgbDecode[640] = {
#include "xr_srgb_decode.h"
};

constant uint kXrTies[32] = {
    0x55555556u, 0x55555555u, 0x55555555u, 0x55555555u, 0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x55555555u, 0x55555555u, 0x55555555u, 0x95555555u,
    0xaaaaaaaau, 0xaaaaaaaau, 0xaaaaaaaau, 0x2aaaaaaau, 0x33333333u, 0x33333333u, 0x33333333u, 0x33333333u,
};
constant uint kGridTies1[1] = {
    0x00000000u,
};
constant uint kGridTies15[1] = {
    0x000000ffu,
};
constant uint kGridTies31[1] = {
    0x5555ff00u,
};
constant ushort kSrgb8Steps[256] = {
    0,    1,    2,    4,    5,    6,    7,    9,    10,   11,   12,   14,   15,   16,   18,   20,   21,   23,   25,
    27,   29,   31,   33,   35,   37,   40,   42,   45,   48,   50,   53,   56,   59,   62,   66,   69,   72,   76,
    79,   83,   87,   91,   95,   99,   103,  107,  112,  116,  121,  126,  131,  136,  141,  146,  151,  156,  162,
    168,  173,  179,  185,  191,  197,  204,  210,  216,  223,  230,  237,  244,  251,  258,  265,  273,  280,  288,
    296,  304,  312,  320,  329,  337,  346,  354,  363,  372,  381,  390,  400,  409,  419,  428,  438,  448,  458,
    469,  479,  490,  500,  511,  522,  533,  544,  555,  567,  578,  590,  602,  614,  626,  639,  651,  664,  676,
    689,  702,  715,  728,  742,  755,  769,  783,  797,  811,  825,  840,  854,  869,  884,  899,  914,  929,  945,
    960,  976,  992,  1008, 1024, 1041, 1057, 1074, 1091, 1108, 1125, 1142, 1159, 1177, 1195, 1213, 1231, 1249, 1267,
    1286, 1304, 1323, 1342, 1361, 1381, 1400, 1420, 1440, 1459, 1480, 1500, 1520, 1541, 1562, 1582, 1603, 1625, 1646,
    1668, 1689, 1711, 1733, 1755, 1778, 1800, 1823, 1846, 1869, 1892, 1916, 1939, 1963, 1987, 2011, 2035, 2059, 2084,
    2109, 2133, 2159, 2184, 2209, 2235, 2260, 2286, 2312, 2339, 2365, 2392, 2419, 2446, 2473, 2500, 2527, 2555, 2583,
    2611, 2639, 2668, 2696, 2725, 2754, 2783, 2812, 2841, 2871, 2901, 2931, 2961, 2991, 3022, 3052, 3083, 3114, 3146,
    3177, 3209, 3240, 3272, 3304, 3337, 3369, 3402, 3435, 3468, 3501, 3535, 3568, 3602, 3636, 3670, 3705, 3739, 3774,
    3809, 3844, 3879, 3915, 3950, 3986, 4022, 4059, 4095,
};
constant ushort kSrgb8First[256] = {
    0,    1,    2,    4,    5,    6,    7,    9,    10,   11,   12,   14,   15,   16,   18,   19,   21,   23,   24,
    26,   28,   30,   32,   34,   37,   39,   42,   44,   47,   49,   52,   55,   58,   61,   64,   68,   71,   74,
    78,   82,   85,   89,   93,   97,   102,  106,  110,  115,  119,  124,  129,  134,  139,  144,  149,  154,  160,
    165,  171,  177,  183,  189,  195,  201,  207,  214,  220,  227,  234,  241,  248,  255,  262,  270,  277,  285,
    293,  300,  308,  317,  325,  333,  342,  350,  359,  368,  377,  386,  395,  405,  414,  424,  434,  444,  454,
    464,  474,  485,  495,  506,  517,  528,  539,  550,  562,  573,  585,  597,  609,  621,  633,  645,  658,  670,
    683,  696,  709,  722,  736,  749,  763,  777,  790,  804,  819,  833,  848,  862,  877,  892,  907,  922,  938,
    953,  969,  985,  1001, 1017, 1033, 1049, 1066, 1083, 1100, 1117, 1134, 1151, 1169, 1186, 1204, 1222, 1240, 1259,
    1277, 1296, 1314, 1333, 1352, 1371, 1391, 1410, 1430, 1450, 1470, 1490, 1510, 1531, 1552, 1572, 1593, 1615, 1636,
    1657, 1679, 1701, 1723, 1745, 1767, 1790, 1812, 1835, 1858, 1881, 1904, 1928, 1951, 1975, 1999, 2023, 2048, 2072,
    2097, 2121, 2146, 2172, 2197, 2222, 2248, 2274, 2300, 2326, 2352, 2379, 2406, 2433, 2460, 2487, 2514, 2542, 2569,
    2597, 2626, 2654, 2682, 2711, 2740, 2769, 2798, 2827, 2857, 2886, 2916, 2946, 2977, 3007, 3038, 3068, 3099, 3130,
    3162, 3193, 3225, 3257, 3289, 3321, 3354, 3386, 3419, 3452, 3485, 3518, 3552, 3586, 3620, 3654, 3688, 3722, 3757,
    3792, 3827, 3862, 3897, 3933, 3969, 4005, 4041, 4077,
};

struct VolumeStep {
    uint2 above, size;
    uint aboveDepth, depth, plane;
};

// The value p 2^e rounded to nearest even at 24 significant bits (a remainder below p where `sticky`): its mantissa, e moved
static uint nearest24(ulong p, bool sticky, thread int &e)
{
    const int top = 63 - int(clz(p));
    if (top <= 23) {
        e -= 23 - top;
        return uint(p << (23 - top));
    }
    const int s = top - 23;
    ulong m = p >> s;
    const ulong rest = p & ((1ul << s) - 1), mid = 1ul << (s - 1);
    if (rest > mid || (rest == mid && (sticky || (m & 1))))
        m++;
    e += s;
    if (m == 1ul << 24) {
        m >>= 1;
        e++;
    }
    return uint(m);
}

struct Axis {
    uint x0, x1, q;  // the two source texels and the second's weight, of 256
};

// Destination texel i of `small` from `big` along one axis, as the M4 computes it in f32: u = (i + 1/2) RNE(1 / small),
// c = u big - 1/2, each operation rounded to nearest even, x0 = floor(c), the fraction rounded half up to 1/256
static Axis axisTap(uint i, uint big, uint small)
{
    if (big == small)
        return {0, 0, 0};
    const ulong one = 1ul << 62;
    int e = -62;
    const uint r = nearest24(one / small, one % small != 0, e);
    e -= 1;
    const uint u = nearest24(ulong(2 * i + 1) * r, false, e);
    const uint c = nearest24(ulong(u) * big, false, e);
    // c >= 1/2 and below 2^14, so its quantum is at most 2^-10: the half's subtraction is exact
    const uint sh = uint(-e);
    const ulong d = ulong(c) - (1ul << (sh - 1));
    uint x0 = uint(d >> sh);
    uint q = uint(((d & ((1ul << sh) - 1)) * 256 + (1ul << (sh - 1))) >> sh);
    if (q == 256) {
        x0++;
        q = 0;
    }
    return {x0, min(x0 + 1, big - 1), q};
}

static int msb(ulong v)
{
    return 63 - int(clz(v));
}

// The grid rounding of the sum s (code units of 2^-24) of codes times a code's m, on k fraction bits: half up to the
// grid, then the nearest code, a half up where the tie table's bit for the code below is set (Metal's f32 step there)
static uint gridCode(ulong s, uint k, uint m, constant uint *ties, uint lo, uint hi)
{
    const ulong q = (s + (1ul << (23 - k))) >> (24 - k);
    const ulong unit = ulong(m) << k;
    uint code = uint(q / unit);
    const ulong fraction = q % unit;
    if (fraction > unit / 2 || (fraction == unit / 2 && (ties[code / 32] >> (code % 32) & 1)))
        code++;
    return clamp(code, lo, hi);
}

// Each term, a code times its weight, floored to 2^(shift - 24) code before the sum
static ulong floored(thread const uint *v, thread const uint *n, uint taps, uint shift)
{
    ulong s = 0;
    for (uint k = 0; k < taps; k++)
        s += (ulong(v[k]) * n[k]) >> shift << shift;
    return s;
}

static uint xrColour(thread const uint *c, thread const uint *n, uint taps)
{
    return gridCode(floored(c, n, taps, 11), 4, 1, kXrTies, 0, 1023);
}

static uint xrAlpha(thread uint *c, thread const uint *n, uint taps)
{
    for (uint k = 0; k < taps; k++)
        c[k] = clamp(c[k], 384u, 894u);
    return gridCode(floored(c, n, taps, 9), 3, 1, kXrTies, 384, 894);
}

// An extended-range sRGB colour: its codes' signed decode steps (of 1/8192), each term floored to 1/512 step, the sum
// rounded half up, then the code whose encode range holds its magnitude, toward its sign
static uint xrSrgb(thread const uint *c, thread const uint *n, uint taps)
{
    long t = 0;
    for (uint k = 0; k < taps; k++) {
        const int o = int(c[k]) - 384;
        const long d = long(kSrgbDecode[min(uint(o < 0 ? -o : o), 639u)]);
        t += ((o < 0 ? -d : d) * long(n[k])) >> 15;
    }
    const long q = (t + 256) >> 9;
    const uint i = uint(min(q < 0 ? -q : q, long(1) << 20));
    uint m = 0;
    for (uint step = 512; step; step >>= 1)
        if (m + step < 640 && kSrgbFirst[m + step] <= i)
            m += step;
    return q >= 0 ? 384 + min(m, 639u) : 384 - min(m, 384u);
}

// An 8-bit sRGB colour: its bytes' twelve-bit linear steps, each term floored to 2^-12 step, the sum rounded half up,
// then the byte whose encode range holds the step
static uint srgb8(thread const uint *b, thread const uint *n, uint taps)
{
    ulong s = 0;
    for (uint k = 0; k < taps; k++)
        s += (ulong(kSrgb8Steps[b[k] & 255]) * n[k]) >> 12 << 12;
    const uint i = uint(min((s + (1ul << 23)) >> 24, 4095ul));
    uint m = 0;
    for (uint step = 128; step; step >>= 1)
        if (kSrgb8First[m + step] <= i)
            m += step;
    return m;
}

// A unorm field of `bits` bits filtered as the code of `wide` bits its bits replicate to (each code times m)
static uint widened(thread const uint *c, thread const uint *n, uint taps, uint wide, uint m, uint den,
                    constant uint *ties)
{
    uint v[8];
    for (uint k = 0; k < taps; k++)
        v[k] = c[k] * m;
    return gridCode(floored(v, n, taps, wide), 12 - wide, m, ties, 0, den);
}

// One RGB9E5Float channel's half from its taps, four a plane (each m 2^z weighed n 2^-24): an M4's filter sums each
// plane's terms at 10 fraction bits and 7 guard bits under its largest term's top bit, taken as the sum of its factors'
// top bits, each term truncated; adds the planes exactly; rounds half away from zero, subnormals kept
static uint rgb9e5Half(thread const uint *m, thread const int *z, thread const uint *n, uint taps)
{
    constexpr int none = -(1 << 30);
    ulong s = 0;
    int sc = none;
    for (uint first = 0; first < taps; first += 4) {
        int e = none;
        for (uint k = first; k < first + 4; k++)
            if (m[k] && n[k])
                e = max(e, msb(m[k]) + z[k] + msb(n[k]) - 24);
        if (e == none)
            continue;
        const int plane = e - 17;
        ulong p = 0;
        for (uint k = first; k < first + 4; k++) {
            if (!m[k] || !n[k])
                continue;
            const int sh = z[k] - 24 - plane;
            const ulong t = ulong(m[k]) * n[k];
            p += sh >= 0 ? t << sh : (-sh < 64 ? t >> -sh : 0);
        }
        // Summed at the coarser plane's scale: that sum's top bit lies at least 17 bits over it, so the finer sum's bits
        // under it never reach the half's rounding bit
        if (plane > sc) {
            s = (plane - sc < 64 ? s >> (plane - sc) : 0) + p;
            sc = plane;
        } else {
            s += sc - plane < 64 ? p >> (sc - plane) : 0;
        }
    }
    if (!s)
        return 0;
    int ulp = max(msb(s) + sc, -14) - 10;
    const int drop = ulp - sc;
    ulong r = drop > 0 ? (drop < 64 ? s >> drop : 0) + (drop <= 64 ? (s >> (drop - 1)) & 1 : 0) : s << -drop;
    if (r == 2048) {
        r >>= 1;
        ulp++;
    }
    if (!r)
        return 0;
    const int x = msb(r) + ulp;
    if (x > 15)
        return 0x7c00;
    if (x < -14)
        return uint(r);
    return uint(x + 15) << 10 | (uint(r) & 0x3ff);
}

// RGB9E5Float's word from its channels' half answers: the shared exponent of the largest (at least -16 before its bias of
// 16, at most 31), each mantissa floored, at most 511
static uint rgb9e5Word(uint3 h)
{
    const uint mx = max(h.x, max(h.y, h.z));
    if (!mx)
        return 0;
    const uint em = mx >> 10, mm = em ? (mx & 0x3ff) | 0x400 : mx & 0x3ff;
    const int zm = em ? int(em) - 25 : -24;
    const int es = min(max(msb(mm) + zm, -16) + 16, 31);
    uint word = uint(es) << 27;
    for (uint c = 0; c < 3; c++) {
        const uint e = h[c] >> 10, m = e ? (h[c] & 0x3ff) | 0x400 : h[c] & 0x3ff;
        const int sh = (e ? int(e) - 25 : -24) - es + 24;
        const uint v = sh >= 0 ? m << sh : (-sh < 32 ? m >> -sh : 0);
        word |= min(v, 511u) << (9 * c);
    }
    return word;
}

static uint4 packBgr10(thread const uint4 *w, thread const uint *n, uint taps)
{
    uint b[8], g[8], r[8];
    for (uint k = 0; k < taps; k++)
        b[k] = w[k].x & 1023, g[k] = w[k].x >> 10 & 1023, r[k] = w[k].x >> 20 & 1023;
    return uint4(0xC0000000u | xrColour(r, n, taps) << 20 | xrColour(g, n, taps) << 10 | xrColour(b, n, taps), 0, 0, 0);
}

static uint4 packBgr10Srgb(thread const uint4 *w, thread const uint *n, uint taps)
{
    uint b[8], g[8], r[8];
    for (uint k = 0; k < taps; k++)
        b[k] = w[k].x & 1023, g[k] = w[k].x >> 10 & 1023, r[k] = w[k].x >> 20 & 1023;
    return uint4(0xC0000000u | xrSrgb(r, n, taps) << 20 | xrSrgb(g, n, taps) << 10 | xrSrgb(b, n, taps), 0, 0, 0);
}

// BGRA10_XR read as RGBA16Uint: blue's lane first, each code in its lane's top ten bits, the low six 0
static uint4 packBgra10(thread const uint4 *w, thread const uint *n, uint taps)
{
    uint b[8], g[8], r[8], a[8];
    for (uint k = 0; k < taps; k++)
        b[k] = w[k].x >> 6, g[k] = w[k].y >> 6, r[k] = w[k].z >> 6, a[k] = w[k].w >> 6;
    return uint4(xrColour(b, n, taps), xrColour(g, n, taps), xrColour(r, n, taps), xrAlpha(a, n, taps)) << 6;
}

static uint4 packBgra10Srgb(thread const uint4 *w, thread const uint *n, uint taps)
{
    uint b[8], g[8], r[8], a[8];
    for (uint k = 0; k < taps; k++)
        b[k] = w[k].x >> 6, g[k] = w[k].y >> 6, r[k] = w[k].z >> 6, a[k] = w[k].w >> 6;
    return uint4(xrSrgb(b, n, taps), xrSrgb(g, n, taps), xrSrgb(r, n, taps), xrAlpha(a, n, taps)) << 6;
}

static uint4 packSrgb8(thread const uint4 *w, thread const uint *n, uint taps)
{
    uint b[8];
    for (uint k = 0; k < taps; k++)
        b[k] = w[k].x;
    return uint4(srgb8(b, n, taps), 0, 0, 0);
}

static uint4 packAbgr4(thread const uint4 *w, thread const uint *n, uint taps)
{
    uint out = 0;
    for (uint shift = 0; shift < 16; shift += 4) {
        uint c[8];
        for (uint k = 0; k < taps; k++)
            c[k] = w[k].x >> shift & 15;
        out |= widened(c, n, taps, 8, 17, 15, kGridTies15) << shift;
    }
    return uint4(out, 0, 0, 0);
}

static uint4 packA1bgr5(thread const uint4 *w, thread const uint *n, uint taps)
{
    uint c[8];
    for (uint k = 0; k < taps; k++)
        c[k] = w[k].x & 1;
    uint out = widened(c, n, taps, 10, 1023, 1, kGridTies1);
    for (uint shift = 1; shift < 16; shift += 5) {
        for (uint k = 0; k < taps; k++)
            c[k] = w[k].x >> shift & 31;
        out |= widened(c, n, taps, 10, 33, 31, kGridTies31) << shift;
    }
    return uint4(out, 0, 0, 0);
}

static uint4 packRgb9e5(thread const uint4 *w, thread const uint *n, uint taps)
{
    uint3 h;
    int z[8];
    for (uint k = 0; k < taps; k++)
        z[k] = int(w[k].x >> 27) - 24;
    for (uint c = 0; c < 3; c++) {
        uint m[8];
        for (uint k = 0; k < taps; k++)
            m[k] = w[k].x >> (9 * c) & 511;
        h[c] = rgb9e5Half(m, z, n, taps);
    }
    return uint4(rgb9e5Word(h), 0, 0, 0);
}

#define WORD_MIP(name, pack, out, take)                                                                         \
    fragment out fmt_mip_##name##_frag(Out in [[stage_in]], texture2d<uint, access::read> above [[texture(0)]], \
                                       constant Step &step [[buffer(0)]])                                       \
    {                                                                                                           \
        const uint2 p = uint2(in.position.xy);                                                                  \
        const Axis ax = axisTap(p.x, step.above.x, step.size.x), ay = axisTap(p.y, step.above.y, step.size.y);  \
        uint4 w[4];                                                                                             \
        uint n[4];                                                                                              \
        for (uint k = 0; k < 4; k++) {                                                                          \
            w[k] = above.read(uint2(k & 1 ? ax.x1 : ax.x0, k & 2 ? ay.x1 : ay.x0));                             \
            n[k] = (k & 1 ? ax.q : 256 - ax.q) * (k & 2 ? ay.q : 256 - ay.q) * 256;                             \
        }                                                                                                       \
        return pack(w, n, 4) take;                                                                              \
    }

// An M4's trilinear weights (of 2^-24, on 2^-16): each product of an x and a z weight exact, its share on y's second row
// rounded half down, the first row the rest
static void volumeWeights(Axis ax, Axis ay, Axis az, thread uint *n)
{
    for (uint k = 0; k < 8; k++) {
        if (k & 2)
            continue;
        const uint xz = (k & 1 ? ax.q : 256 - ax.q) * (k & 4 ? az.q : 256 - az.q), second = (xz * ay.q + 127) >> 8;
        n[k | 2] = second << 8;
        n[k] = (xz - second) << 8;
    }
}

#define WORD_MIP3D(name, pack, out, take)                                                                          \
    fragment out fmt_mip_3d_##name##_frag(Out in [[stage_in]], texture3d<uint, access::read> above [[texture(0)]], \
                                          constant VolumeStep &step [[buffer(0)]])                                 \
    {                                                                                                              \
        const uint2 p = uint2(in.position.xy);                                                                     \
        const Axis ax = axisTap(p.x, step.above.x, step.size.x), ay = axisTap(p.y, step.above.y, step.size.y),     \
                   az = axisTap(step.plane, step.aboveDepth, step.depth);                                          \
        uint4 w[8];                                                                                                \
        uint n[8];                                                                                                 \
        for (uint k = 0; k < 8; k++)                                                                               \
            w[k] = above.read(uint3(k & 1 ? ax.x1 : ax.x0, k & 2 ? ay.x1 : ay.x0, k & 4 ? az.x1 : az.x0));         \
        volumeWeights(ax, ay, az, n);                                                                              \
        return pack(w, n, 8) take;                                                                                 \
    }

WORD_MIP(bgr10_xr, packBgr10, uint, .x)
WORD_MIP(bgr10_xr_srgb, packBgr10Srgb, uint, .x)
WORD_MIP(bgra10_xr, packBgra10, uint4, )
WORD_MIP(bgra10_xr_srgb, packBgra10Srgb, uint4, )
WORD_MIP(rgb9e5, packRgb9e5, uint, .x)
WORD_MIP(srgb8, packSrgb8, uint, .x)
WORD_MIP(abgr4, packAbgr4, uint, .x)
WORD_MIP(a1bgr5, packA1bgr5, uint, .x)
WORD_MIP3D(bgr10_xr, packBgr10, uint, .x)
WORD_MIP3D(bgr10_xr_srgb, packBgr10Srgb, uint, .x)
WORD_MIP3D(bgra10_xr, packBgra10, uint4, )
WORD_MIP3D(bgra10_xr_srgb, packBgra10Srgb, uint4, )
WORD_MIP3D(srgb8, packSrgb8, uint, .x)
WORD_MIP3D(abgr4, packAbgr4, uint, .x)
WORD_MIP3D(a1bgr5, packA1bgr5, uint, .x)
WORD_MIP3D(rgb9e5, packRgb9e5, uint, .x)
