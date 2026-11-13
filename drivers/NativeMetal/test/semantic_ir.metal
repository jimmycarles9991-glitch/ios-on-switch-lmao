#include <metal_stdlib>
using namespace metal;

constant uint semantic_table_values[256] = {
    19u, 87u, 137u, 141u, 391u, 147u, 69u, 9u, 155u, 479u, 1697u, 1541u, 1359u, 1163u, 3197u, 3569u,
    4003u, 2183u, 2265u, 2813u, 5367u, 5347u, 4757u, 4121u, 7723u, 7663u, 6769u, 6293u, 10687u, 11387u, 11789u, 8609u,
    9523u, 10167u, 14441u, 16045u, 15847u, 12403u, 13541u, 19049u, 20411u, 17407u, 18369u, 17189u, 24495u, 23531u, 22493u, 21393u,
    28355u, 26919u, 25913u, 24925u, 31447u, 30595u, 29237u, 36409u, 38603u, 39887u, 32785u, 34165u, 35743u, 36507u, 46317u, 47233u,
    41555u, 42775u, 43465u, 54221u, 54727u, 57171u, 50821u, 52425u, 51803u, 61471u, 65121u, 58565u, 60815u, 59595u, 70461u, 72369u,
    67043u, 69319u, 79769u, 81853u, 79671u, 77347u, 75093u, 89561u, 87531u, 85295u, 83377u, 96725u, 94719u, 92475u, 90445u, 104673u,
    104307u, 102391u, 98473u, 113005u, 110887u, 109235u, 107045u, 121513u, 118139u, 116671u, 130177u, 128357u, 125935u, 123307u, 137373u, 134865u,
    146179u, 132327u, 134905u, 137373u, 156439u, 159043u, 161781u, 148089u, 149643u, 153487u, 154961u, 174389u, 178143u, 179419u, 167469u, 168257u,
    189587u, 192471u, 193289u, 181261u, 182535u, 187923u, 206277u, 207241u, 211995u, 196959u, 202017u, 203141u, 224719u, 228875u, 215805u, 214641u,
    217635u, 241159u, 244313u, 231805u, 231287u, 234339u, 257045u, 260249u, 248747u, 247407u, 250097u, 271381u, 276799u, 266235u, 269709u, 267041u,
    287155u, 291895u, 281065u, 285485u, 306535u, 311027u, 298853u, 295401u, 299835u, 326527u, 321857u, 317605u, 312879u, 341867u, 337245u, 332305u,
    328003u, 356519u, 353209u, 349149u, 344919u, 347139u, 375989u, 371385u, 367179u, 363087u, 392081u, 388085u, 383775u, 379675u, 407917u, 403713u,
    394963u, 423831u, 419401u, 415309u, 410951u, 441299u, 437253u, 433481u, 430043u, 458399u, 450785u, 447301u, 443919u, 472907u, 468413u, 461361u,
    489571u, 487239u, 482585u, 476477u, 505783u, 500131u, 496341u, 524121u, 518507u, 514735u, 508977u, 537173u, 532863u, 527803u, 555725u, 550753u,
    545779u, 540535u, 578089u, 583405u, 587943u, 560179u, 565413u, 570665u, 609019u, 613951u, 619009u, 591845u, 596591u, 601643u, 639517u, 643665u,
    651139u, 654183u, 627065u, 633885u, 637847u, 677571u, 683637u, 687097u, 660491u, 667407u, 670417u, 709813u, 716639u, 718427u, 695469u, 697025u
};

kernel void semantic_table(device uint *target [[buffer(0)]], uint i [[thread_position_in_grid]])
{
    target[i] = semantic_table_values[i & 255];
}

kernel void semantic_integer(device const uint *source [[buffer(0)]],
                             device uint *target [[buffer(1)]], uint i [[thread_position_in_grid]])
{
    target[i] = (source[i] * 3u + 7u) ^ (i >> 1);
}

kernel void semantic_branch(device const uint *source [[buffer(0)]],
                            device uint *target [[buffer(1)]], uint i [[thread_position_in_grid]])
{
    uint x = source[i];
    target[i] = x & 1 ? x * 5 + i : x ^ (i + 9);
}

kernel void semantic_loop(device const uint *source [[buffer(0)]],
                          device uint *target [[buffer(1)]], uint i [[thread_position_in_grid]])
{
    uint x = source[i];
    for (uint n = 0; n < (i & 7); ++n)
        x = (x << 1) ^ (n + 3);
    target[i] = x;
}

kernel void semantic_float(device const float *source [[buffer(0)]],
                           device float *target [[buffer(1)]], uint i [[thread_position_in_grid]])
{
    target[i] = fma(source[i], 1.25f, -0.125f);
}

kernel void semantic_float_round(device const float *source [[buffer(0)]],
                                 device float *target [[buffer(1)]], uint i [[thread_position_in_grid]])
{
    float product = source[i] * 1.25f;
    target[i] = product - 0.125f;
}

kernel void semantic_atan2(device float *target [[buffer(0)]], uint i [[thread_position_in_grid]])
{
    target[i] = atan2(float(i) - 32.0f, float(i) + 64.0f);
}

kernel void semantic_texture(texture2d<float, access::read> source [[texture(0)]],
                             device float4 *target [[buffer(0)]], uint i [[thread_position_in_grid]])
{
    target[i] = source.read(uint2(i, 0));
}

kernel void semantic_vector(device const uint4 *source [[buffer(0)]],
                            device uint4 *target [[buffer(1)]], uint i [[thread_position_in_grid]])
{
    uint4 x = source[i];
    target[i] = (x.wzyx << uint4(1, 2, 3, 4)) ^ uint4(x.y, x.x, i, x.z + 5);
}

kernel void semantic_half(device const half4 *source [[buffer(0)]],
                          device half4 *target [[buffer(1)]], uint i [[thread_position_in_grid]])
{
    half4 x = source[i];
    target[i] = half4(x.x + x.y, x.z * x.w, fma(x.x, x.y, x.z), half(float(x.w) * 0.0625f));
}

kernel void semantic_float_compare(device const float2 *source [[buffer(0)]],
                                   device uint *target [[buffer(1)]], uint i [[thread_position_in_grid]])
{
    float2 x = source[i];
    target[i] = uint(isnan(x.x)) | (uint(isinf(x.y)) << 1) | (uint(x.x == x.y) << 2) |
        (uint(x.x != x.y) << 3) | (uint(x.x < x.y) << 4) | (uint(x.x >= x.y) << 5);
}

kernel void semantic_convert(device const uint4 *source [[buffer(0)]],
                             device uint4 *target [[buffer(1)]], uint i [[thread_position_in_grid]])
{
    uint4 x = source[i];
    float a = float(x.x), b = float(int(x.y));
    target[i] = uint4(as_type<uint>(a), as_type<uint>(b), uint(half(a)), uint(int(b)));
}

kernel void semantic_private(device const uint4 *source [[buffer(0)]],
                             device uint *target [[buffer(1)]], uint i [[thread_position_in_grid]])
{
    uint4 x = source[i];
    uint slots[4] = {x.x, x.y, x.z, x.w};
    for (uint j = 0; j < (i & 3); ++j)
        slots[(j + 1) & 3] ^= slots[j];
    target[i] = slots[i & 3];
}

kernel void semantic_atomic(device atomic_uint *target [[buffer(0)]], uint i [[thread_position_in_grid]])
{
    atomic_fetch_add_explicit(target, i, memory_order_relaxed);
}

kernel void semantic_barrier(device uint *target [[buffer(0)]], uint i [[thread_position_in_grid]],
                             uint lane [[thread_index_in_threadgroup]])
{
    threadgroup uint values[32];
    values[lane] = i;
    threadgroup_barrier(mem_flags::mem_threadgroup);
    target[i] = values[31 - lane];
}

kernel void semantic_extended(device const float4 *source [[buffer(0)]],
                              device float4 *target [[buffer(1)]], uint i [[thread_position_in_grid]])
{
    float4 x = source[i];
    target[i] = float4(floor(x.x), rint(x.y), clamp(x.z, -0.25f, 0.75f), fabs(x.w));
}

kernel void semantic_group_min(device const uint *source [[buffer(0)]], device uint *target [[buffer(1)]],
                               uint i [[thread_position_in_grid]], uint lane [[thread_index_in_threadgroup]])
{
    threadgroup uint values[32];
    values[lane] = min(source[i], 0x10203040u);
    threadgroup_barrier(mem_flags::mem_threadgroup | mem_flags::mem_device);
    target[i] = values[31 - lane];
}

kernel void semantic_unpack_half(device const ushort4 *source [[buffer(0)]],
                                 device float4 *target [[buffer(1)]], uint i [[thread_position_in_grid]])
{
    target[i] = float4(as_type<half4>(source[i]));
}

kernel void semantic_subgroup(device uint *target [[buffer(0)]], threadgroup uint *shared [[threadgroup(0)]],
                              uint i [[thread_position_in_grid]], uint local [[thread_index_in_threadgroup]],
                              uint lane [[thread_index_in_simdgroup]], uint group [[simdgroup_index_in_threadgroup]],
                              uint width [[threads_per_simdgroup]])
{
    shared[local] = group * width + lane;
    threadgroup_barrier(mem_flags::mem_threadgroup);
    target[i] = shared[31 - local] + group * width + lane;
}
