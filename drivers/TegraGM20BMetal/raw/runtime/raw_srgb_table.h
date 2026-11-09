// SPDX-License-Identifier: MIT
// The colour table metal2vulkan's fragment programs read to write a float to an 8-bit sRGB colour target as the byte Metal stores
// (metal2vulkan lower/arith/srgb.rs color_write_table, WriteSource::ColorWriteTable): 256 entries of four words, entry b holding
// byte b's first index in Metal's twelve-bit encode table, the bits of the f32 nearest its linear value, and byte b - 1's.
// The raw path keeps its own copy (Device::colorTable_) where the record names the table (EntryColorTable).
#pragma once
#include <cstdint>

namespace raw {

// Metal's twelve-bit encode: byte b from index kSrgbEncodeFirst[b] on (the exact product x * 4095 rounded to nearest even)
constexpr uint16_t kSrgbEncodeFirst[256] = {
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

// The f32 nearest each byte's linear value, decode(b / 255) of the sRGB transfer, as bits
constexpr uint32_t kSrgbLinear[256] = {
    0x00000000, 0x399f22b4, 0x3a1f22b4, 0x3a6eb40e, 0x3a9f22b4, 0x3ac6eb61, 0x3aeeb40e, 0x3b0b3e5d, 0x3b1f22b4,
    0x3b33070a, 0x3b46eb61, 0x3b5b518e, 0x3b70f18f, 0x3b83e1c6, 0x3b8fe616, 0x3b9c87fd, 0x3ba9c9b6, 0x3bb7ad6f,
    0x3bc6354a, 0x3bd56360, 0x3be539c1, 0x3bf5ba71, 0x3c0373b6, 0x3c0c6153, 0x3c15a705, 0x3c1f45be, 0x3c293e6b,
    0x3c3391f7, 0x3c3e4149, 0x3c494d44, 0x3c54b6c9, 0x3c607eb4, 0x3c6ca5df, 0x3c792d22, 0x3c830aa9, 0x3c89af9f,
    0x3c9085dc, 0x3c978dc6, 0x3c9ec7c2, 0x3ca63433, 0x3cadd37d, 0x3cb5a602, 0x3cbdac21, 0x3cc5e63a, 0x3cce54ac,
    0x3cd6f7d5, 0x3cdfd010, 0x3ce8ddba, 0x3cf2212d, 0x3cfb9ac3, 0x3d02a56a, 0x3d0798dd, 0x3d0ca7e6, 0x3d11d2af,
    0x3d171964, 0x3d1c7c30, 0x3d21fb3c, 0x3d2796b2, 0x3d2d4ebb, 0x3d332381, 0x3d39152b, 0x3d3f23e4, 0x3d454fd2,
    0x3d4b991d, 0x3d51ffec, 0x3d588468, 0x3d5f26b6, 0x3d65e6fd, 0x3d6cc563, 0x3d73c20e, 0x3d7add24, 0x3d810b65,
    0x3d84b793, 0x3d88732e, 0x3d8c3e48, 0x3d9018f4, 0x3d940344, 0x3d97fd49, 0x3d9c0715, 0x3da020ba, 0x3da44a4a,
    0x3da883d6, 0x3daccd6f, 0x3db12727, 0x3db5910f, 0x3dba0b38, 0x3dbe95b3, 0x3dc33090, 0x3dc7dbe0, 0x3dcc97b4,
    0x3dd1641d, 0x3dd6412b, 0x3ddb2eee, 0x3de02d76, 0x3de53cd4, 0x3dea5d18, 0x3def8e51, 0x3df4d090, 0x3dfa23e5,
    0x3dff885e, 0x3e027f06, 0x3e05427f, 0x3e080ea2, 0x3e0ae377, 0x3e0dc104, 0x3e10a753, 0x3e13966a, 0x3e168e51,
    0x3e198f0f, 0x3e1c98ac, 0x3e1fab30, 0x3e22c6a1, 0x3e25eb07, 0x3e29186a, 0x3e2c4ed0, 0x3e2f8e42, 0x3e32d6c5,
    0x3e362862, 0x3e39831f, 0x3e3ce703, 0x3e405417, 0x3e43ca60, 0x3e4749e6, 0x3e4ad2af, 0x3e4e64c3, 0x3e520029,
    0x3e55a4e7, 0x3e595305, 0x3e5d0a89, 0x3e60cb7a, 0x3e6495df, 0x3e6869be, 0x3e6c471f, 0x3e702e07, 0x3e741e7e,
    0x3e78188b, 0x3e7c1c33, 0x3e8014bf, 0x3e822039, 0x3e84308b, 0x3e8645b8, 0x3e885fc3, 0x3e8a7eb0, 0x3e8ca281,
    0x3e8ecb3b, 0x3e90f8df, 0x3e932b72, 0x3e9562f6, 0x3e979f6f, 0x3e99e0e0, 0x3e9c274c, 0x3e9e72b6, 0x3ea0c321,
    0x3ea31890, 0x3ea57307, 0x3ea7d288, 0x3eaa3716, 0x3eaca0b6, 0x3eaf0f68, 0x3eb18332, 0x3eb3fc15, 0x3eb67a14,
    0x3eb8fd34, 0x3ebb8576, 0x3ebe12de, 0x3ec0a56e, 0x3ec33d2a, 0x3ec5da14, 0x3ec87c30, 0x3ecb2380, 0x3ecdd008,
    0x3ed081ca, 0x3ed338c9, 0x3ed5f508, 0x3ed8b68a, 0x3edb7d52, 0x3ede4963, 0x3ee11abf, 0x3ee3f169, 0x3ee6cd65,
    0x3ee9aeb5, 0x3eec955b, 0x3eef815c, 0x3ef272b8, 0x3ef56974, 0x3ef86593, 0x3efb6716, 0x3efe6e00, 0x3f00bd2b,
    0x3f02460c, 0x3f03d1a5, 0x3f055ff7, 0x3f06f104, 0x3f0884cd, 0x3f0a1b54, 0x3f0bb499, 0x3f0d509f, 0x3f0eef65,
    0x3f1090ef, 0x3f12353d, 0x3f13dc50, 0x3f15862a, 0x3f1732cc, 0x3f18e237, 0x3f1a946e, 0x3f1c4970, 0x3f1e0140,
    0x3f1fbbde, 0x3f21794d, 0x3f23398c, 0x3f24fc9f, 0x3f26c285, 0x3f288b41, 0x3f2a56d2, 0x3f2c253c, 0x3f2df67f,
    0x3f2fca9c, 0x3f31a194, 0x3f337b6a, 0x3f35581d, 0x3f3737b0, 0x3f391a24, 0x3f3aff7a, 0x3f3ce7b2, 0x3f3ed2cf,
    0x3f40c0d2, 0x3f42b1bc, 0x3f44a58e, 0x3f469c49, 0x3f4895ef, 0x3f4a9280, 0x3f4c91ff, 0x3f4e946c, 0x3f5099c9,
    0x3f52a216, 0x3f54ad56, 0x3f56bb88, 0x3f58ccaf, 0x3f5ae0cc, 0x3f5cf7df, 0x3f5f11ea, 0x3f612eef, 0x3f634eee,
    0x3f6571e9, 0x3f6797e0, 0x3f69c0d5, 0x3f6becca, 0x3f6e1bbf, 0x3f704db5, 0x3f7282ae, 0x3f74baab, 0x3f76f5ae,
    0x3f7933b6, 0x3f7b74c6, 0x3f7db8de, 0x3f800000,
};

constexpr uint32_t kColorTableBytes = 256 * 16;

inline void colorTableWords(uint32_t out[256 * 4])
{
    for (uint32_t b = 0; b < 256; b++) {
        out[4 * b] = kSrgbEncodeFirst[b];
        out[4 * b + 1] = kSrgbLinear[b];
        out[4 * b + 2] = b ? kSrgbLinear[b - 1] : 0;
        out[4 * b + 3] = 0;
    }
}

}  // namespace raw
