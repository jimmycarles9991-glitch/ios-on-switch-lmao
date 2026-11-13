// SPDX-License-Identifier: MIT
// drivers/TegraGM20B/TegraGM20BLayout.h on the host: the one block-linear layout every GPU user of the tree takes is NIL's.
// kNil holds what NVK answers for a VK_IMAGE_TILING_OPTIMAL 2D image of one level (raw/fx/test/niloracle.c over the NVK host
// twin and the mock GM20B: vkGetImageMemoryRequirements' size, vkGetImageSubresourceLayout's row pitch and size), for texel
// sizes 1 to 16 bytes and heights on each side of every block-height step. raw/fx's mip chains take their level 0 from the
// same rule, and the display's tiled scan (TegraScanoutCopy.h) reads what it lays out.
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "../../TegraDisplayFlip/TegraScanoutCopy.h"
#include "../raw/fx/fx_image.h"
#include "../raw/fx/fx_mip.h"
#include "TegraGM20BLayout.h"

namespace {

struct NilImage {
    uint32_t width, height, bpp;
    uint64_t bytes, rowPitch, levelBytes;
};

// niloracle <dylib> <9 R8 | 16 RG8 | 44 BGRA8 | 97 RGBA16F | 109 RGBA32F> WxHx1 ...
const NilImage kNil[] = {
    {1, 1, 1, 4096, 64, 512},
    {2, 2, 1, 4096, 64, 512},
    {3, 5, 1, 4096, 64, 512},
    {7, 7, 1, 4096, 64, 512},
    {8, 8, 1, 4096, 64, 512},
    {9, 9, 1, 4096, 64, 1024},
    {10, 10, 1, 4096, 64, 1024},
    {11, 11, 1, 4096, 64, 1024},
    {12, 16, 1, 4096, 64, 1024},
    {16, 17, 1, 4096, 64, 2048},
    {21, 21, 1, 4096, 64, 2048},
    {22, 22, 1, 4096, 64, 2048},
    {31, 33, 1, 4096, 64, 4096},
    {32, 32, 1, 4096, 64, 2048},
    {42, 42, 1, 4096, 64, 4096},
    {43, 43, 1, 4096, 64, 4096},
    {64, 64, 1, 4096, 64, 4096},
    {65, 65, 1, 16384, 128, 16384},
    {85, 85, 1, 16384, 128, 16384},
    {86, 86, 1, 16384, 128, 16384},
    {87, 90, 1, 16384, 128, 16384},
    {100, 60, 1, 8192, 128, 8192},
    {127, 127, 1, 16384, 128, 16384},
    {128, 128, 1, 16384, 128, 16384},
    {129, 129, 1, 49152, 192, 49152},
    {180, 180, 1, 49152, 192, 49152},
    {255, 257, 1, 98304, 256, 98304},
    {256, 256, 1, 65536, 256, 65536},
    {300, 200, 1, 81920, 320, 81920},
    {342, 154, 1, 98304, 384, 98304},
    {45, 18, 1, 4096, 64, 2048},
    {640, 480, 1, 327680, 640, 327680},
    {720, 1280, 1, 983040, 768, 983040},
    {1280, 720, 1, 983040, 1280, 983040},
    {736, 1280, 1, 983040, 768, 983040},
    {776, 24, 1, 28672, 832, 26624},
    {1024, 1, 1, 8192, 1024, 8192},
    {1, 1024, 1, 65536, 64, 65536},
    {1366, 1024, 1, 1441792, 1408, 1441792},
    {2048, 1536, 1, 3145728, 2048, 3145728},
    {2500, 1500, 1, 3932160, 2560, 3932160},
    {4096, 4096, 1, 16777216, 4096, 16777216},
    {520, 520, 1, 368640, 576, 368640},
    {5, 300, 1, 24576, 64, 24576},
    {1000, 3, 1, 8192, 1024, 8192},
    {33, 65, 1, 8192, 64, 8192},
    {97, 3, 1, 4096, 128, 1024},
    {1, 1, 2, 4096, 64, 512},
    {2, 2, 2, 4096, 64, 512},
    {3, 5, 2, 4096, 64, 512},
    {7, 7, 2, 4096, 64, 512},
    {8, 8, 2, 4096, 64, 512},
    {9, 9, 2, 4096, 64, 1024},
    {10, 10, 2, 4096, 64, 1024},
    {11, 11, 2, 4096, 64, 1024},
    {12, 16, 2, 4096, 64, 1024},
    {16, 17, 2, 4096, 64, 2048},
    {21, 21, 2, 4096, 64, 2048},
    {22, 22, 2, 4096, 64, 2048},
    {31, 33, 2, 4096, 64, 4096},
    {32, 32, 2, 4096, 64, 2048},
    {42, 42, 2, 8192, 128, 8192},
    {43, 43, 2, 8192, 128, 8192},
    {64, 64, 2, 8192, 128, 8192},
    {65, 65, 2, 24576, 192, 24576},
    {85, 85, 2, 24576, 192, 24576},
    {86, 86, 2, 24576, 192, 24576},
    {87, 90, 2, 24576, 192, 24576},
    {100, 60, 2, 16384, 256, 16384},
    {127, 127, 2, 32768, 256, 32768},
    {128, 128, 2, 32768, 256, 32768},
    {129, 129, 2, 81920, 320, 81920},
    {180, 180, 2, 98304, 384, 98304},
    {255, 257, 2, 196608, 512, 196608},
    {256, 256, 2, 131072, 512, 131072},
    {300, 200, 2, 163840, 640, 163840},
    {342, 154, 2, 180224, 704, 180224},
    {45, 18, 2, 4096, 128, 4096},
    {640, 480, 2, 655360, 1280, 655360},
    {720, 1280, 2, 1884160, 1472, 1884160},
    {1280, 720, 2, 1966080, 2560, 1966080},
    {736, 1280, 2, 1884160, 1472, 1884160},
    {776, 24, 2, 53248, 1600, 51200},
    {1024, 1, 2, 16384, 2048, 16384},
    {1, 1024, 2, 65536, 64, 65536},
    {1366, 1024, 2, 2818048, 2752, 2818048},
    {2048, 1536, 2, 6291456, 4096, 6291456},
    {2500, 1500, 2, 7766016, 5056, 7766016},
    {4096, 4096, 2, 33554432, 8192, 33554432},
    {520, 520, 2, 696320, 1088, 696320},
    {5, 300, 2, 24576, 64, 24576},
    {1000, 3, 2, 16384, 2048, 16384},
    {33, 65, 2, 16384, 128, 16384},
    {97, 3, 2, 4096, 256, 2048},
    {1, 1, 4, 4096, 64, 512},
    {2, 2, 4, 4096, 64, 512},
    {3, 5, 4, 4096, 64, 512},
    {7, 7, 4, 4096, 64, 512},
    {8, 8, 4, 4096, 64, 512},
    {9, 9, 4, 4096, 64, 1024},
    {10, 10, 4, 4096, 64, 1024},
    {11, 11, 4, 4096, 64, 1024},
    {12, 16, 4, 4096, 64, 1024},
    {16, 17, 4, 4096, 64, 2048},
    {21, 21, 4, 4096, 128, 4096},
    {22, 22, 4, 4096, 128, 4096},
    {31, 33, 4, 8192, 128, 8192},
    {32, 32, 4, 4096, 128, 4096},
    {42, 42, 4, 12288, 192, 12288},
    {43, 43, 4, 12288, 192, 12288},
    {64, 64, 4, 16384, 256, 16384},
    {65, 65, 4, 40960, 320, 40960},
    {85, 85, 4, 49152, 384, 49152},
    {86, 86, 4, 49152, 384, 49152},
    {87, 90, 4, 49152, 384, 49152},
    {100, 60, 4, 28672, 448, 28672},
    {127, 127, 4, 65536, 512, 65536},
    {128, 128, 4, 65536, 512, 65536},
    {129, 129, 4, 147456, 576, 147456},
    {180, 180, 4, 196608, 768, 196608},
    {255, 257, 4, 393216, 1024, 393216},
    {256, 256, 4, 262144, 1024, 262144},
    {300, 200, 4, 311296, 1216, 311296},
    {342, 154, 4, 360448, 1408, 360448},
    {45, 18, 4, 8192, 192, 6144},
    {640, 480, 4, 1310720, 2560, 1310720},
    {720, 1280, 4, 3686400, 2880, 3686400},
    {1280, 720, 4, 3932160, 5120, 3932160},
    {736, 1280, 4, 3768320, 2944, 3768320},
    {776, 24, 4, 102400, 3136, 100352},
    {1024, 1, 4, 32768, 4096, 32768},
    {1, 1024, 4, 65536, 64, 65536},
    {1366, 1024, 4, 5636096, 5504, 5636096},
    {2048, 1536, 4, 12582912, 8192, 12582912},
    {2500, 1500, 4, 15433728, 10048, 15433728},
    {4096, 4096, 4, 67108864, 16384, 67108864},
    {520, 520, 4, 1351680, 2112, 1351680},
    {5, 300, 4, 24576, 64, 24576},
    {1000, 3, 4, 32768, 4032, 32256},
    {33, 65, 4, 24576, 192, 24576},
    {97, 3, 4, 4096, 448, 3584},
    {1, 1, 8, 4096, 64, 512},
    {2, 2, 8, 4096, 64, 512},
    {3, 5, 8, 4096, 64, 512},
    {7, 7, 8, 4096, 64, 512},
    {8, 8, 8, 4096, 64, 512},
    {9, 9, 8, 4096, 128, 2048},
    {10, 10, 8, 4096, 128, 2048},
    {11, 11, 8, 4096, 128, 2048},
    {12, 16, 8, 4096, 128, 2048},
    {16, 17, 8, 4096, 128, 4096},
    {21, 21, 8, 8192, 192, 6144},
    {22, 22, 8, 8192, 192, 6144},
    {31, 33, 8, 16384, 256, 16384},
    {32, 32, 8, 8192, 256, 8192},
    {42, 42, 8, 24576, 384, 24576},
    {43, 43, 8, 24576, 384, 24576},
    {64, 64, 8, 32768, 512, 32768},
    {65, 65, 8, 73728, 576, 73728},
    {85, 85, 8, 90112, 704, 90112},
    {86, 86, 8, 90112, 704, 90112},
    {87, 90, 8, 90112, 704, 90112},
    {100, 60, 8, 53248, 832, 53248},
    {127, 127, 8, 131072, 1024, 131072},
    {128, 128, 8, 131072, 1024, 131072},
    {129, 129, 8, 278528, 1088, 278528},
    {180, 180, 8, 376832, 1472, 376832},
    {255, 257, 8, 786432, 2048, 786432},
    {256, 256, 8, 524288, 2048, 524288},
    {300, 200, 8, 622592, 2432, 622592},
    {342, 154, 8, 704512, 2752, 704512},
    {45, 18, 8, 12288, 384, 12288},
    {640, 480, 8, 2621440, 5120, 2621440},
    {720, 1280, 8, 7372800, 5760, 7372800},
    {1280, 720, 8, 7864320, 10240, 7864320},
    {736, 1280, 8, 7536640, 5888, 7536640},
    {776, 24, 8, 200704, 6208, 198656},
    {1024, 1, 8, 65536, 8192, 65536},
    {1, 1024, 8, 65536, 64, 65536},
    {1366, 1024, 8, 11206656, 10944, 11206656},
    {2048, 1536, 8, 25165824, 16384, 25165824},
    {2500, 1500, 8, 30769152, 20032, 30769152},
    {4096, 4096, 8, 134217728, 32768, 134217728},
    {520, 520, 8, 2662400, 4160, 2662400},
    {5, 300, 8, 24576, 64, 24576},
    {1000, 3, 8, 65536, 8000, 64000},
    {33, 65, 8, 40960, 320, 40960},
    {97, 3, 8, 8192, 832, 6656},
    {1, 1, 16, 4096, 64, 512},
    {2, 2, 16, 4096, 64, 512},
    {3, 5, 16, 4096, 64, 512},
    {7, 7, 16, 4096, 128, 1024},
    {8, 8, 16, 4096, 128, 1024},
    {9, 9, 16, 4096, 192, 3072},
    {10, 10, 16, 4096, 192, 3072},
    {11, 11, 16, 4096, 192, 3072},
    {12, 16, 16, 4096, 192, 3072},
    {16, 17, 16, 8192, 256, 8192},
    {21, 21, 16, 12288, 384, 12288},
    {22, 22, 16, 12288, 384, 12288},
    {31, 33, 16, 32768, 512, 32768},
    {32, 32, 16, 16384, 512, 16384},
    {42, 42, 16, 45056, 704, 45056},
    {43, 43, 16, 45056, 704, 45056},
    {64, 64, 16, 65536, 1024, 65536},
    {65, 65, 16, 139264, 1088, 139264},
    {85, 85, 16, 180224, 1408, 180224},
    {86, 86, 16, 180224, 1408, 180224},
    {87, 90, 16, 180224, 1408, 180224},
    {100, 60, 16, 102400, 1600, 102400},
    {127, 127, 16, 262144, 2048, 262144},
    {128, 128, 16, 262144, 2048, 262144},
    {129, 129, 16, 540672, 2112, 540672},
    {180, 180, 16, 737280, 2880, 737280},
    {255, 257, 16, 1572864, 4096, 1572864},
    {256, 256, 16, 1048576, 4096, 1048576},
    {300, 200, 16, 1228800, 4800, 1228800},
    {342, 154, 16, 1409024, 5504, 1409024},
    {45, 18, 16, 24576, 768, 24576},
    {640, 480, 16, 5242880, 10240, 5242880},
    {720, 1280, 16, 14745600, 11520, 14745600},
    {1280, 720, 16, 15728640, 20480, 15728640},
    {736, 1280, 16, 15073280, 11776, 15073280},
    {776, 24, 16, 397312, 12416, 397312},
    {1024, 1, 16, 131072, 16384, 131072},
    {1, 1024, 16, 65536, 64, 65536},
    {1366, 1024, 16, 22413312, 21888, 22413312},
    {2048, 1536, 16, 50331648, 32768, 50331648},
    {2500, 1500, 16, 61440000, 40000, 61440000},
    {4096, 4096, 16, 268435456, 65536, 268435456},
    {520, 520, 16, 5324800, 8320, 5324800},
    {5, 300, 16, 49152, 128, 49152},
    {1000, 3, 16, 131072, 16000, 128000},
    {33, 65, 16, 73728, 576, 73728},
    {97, 3, 16, 16384, 1600, 12800},
};

TEST(Layout, IsNils)
{
    for (const NilImage &n : kNil) {
        tgm_layout l;
        ASSERT_TRUE(tgm_layout_tiled(n.width, n.height, n.bpp, &l)) << n.width << "x" << n.height << " bpp " << n.bpp;
        EXPECT_EQ(l.row_bytes, n.rowPitch) << n.width << "x" << n.height << " bpp " << n.bpp;
        EXPECT_EQ(uint64_t(l.row_bytes) * l.rows, n.levelBytes) << n.width << "x" << n.height << " bpp " << n.bpp;
        EXPECT_EQ(l.bytes, n.bytes) << n.width << "x" << n.height << " bpp " << n.bpp;
    }
}

TEST(Layout, IsRawFxLevelZero)
{
    for (const NilImage &n : kNil) {
        tgm_layout l;
        raw::fx::MipChain c;
        ASSERT_TRUE(tgm_layout_tiled(n.width, n.height, n.bpp, &l));
        ASSERT_TRUE(raw::fx::mipChainLayout(n.width, n.height, 1, n.bpp, c));
        EXPECT_EQ(l.y_log2, c.yLog2);
        EXPECT_EQ(l.row_bytes, c.level[0].rowBytes);
        EXPECT_EQ(l.rows, c.level[0].rows);
        EXPECT_EQ(l.bytes, c.bytes);
        EXPECT_EQ(l.align, c.align);
        for (uint32_t y : {0u, n.height / 2, n.height - 1})
            for (uint32_t xB : {0u, n.width * n.bpp / 2, n.width * n.bpp - 1})
                EXPECT_EQ(tgm_layout_offset(&l, xB, y), raw::fx::texelOffset(c, 0, xB, y));
    }
}

// NIL's own answers (raw/fx/test/nilimage.c over the NVK host twin's exported NIL): a one-level image's size, alignment, level
// 0's row stride and block depth, the header of a view of all of it at 0x100000, and the FNV-1a hash of the image after
// nil_copy_linear_to_tiled of the pattern (k * 131 + k / 251 + 7)
struct NilView {
    uint32_t dim, width, height, depth, bpp;
    bool view2d;
    uint64_t bytes, align;
    uint32_t rowBytes, zLog2;
    uint32_t header[8];
    uint64_t hash;
    uint32_t layers;
    bool cube;
    uint64_t arrayStride;
};

// (one image a line: nilimage's output, in its order)
// clang-format off
const NilView kNilViews[] = {
    {1, 1024, 1, 1, 4, false, 32768, 4096, 4096, 0, {0x58d24908, 0x00100000, 0x00600000, 0x00070000, 0xe00003ff, 0x80000000, 0x03000000, 0},
     0x12598af5ff51af25, 1, false, 32768},
    {3, 17, 17, 17, 4, false, 98304, 8192, 128, 4, {0x58d24908, 0x00100000, 0x00600000, 0x00070100, 0xe1000010, 0x80100010, 0x03000000, 0},
     0xb7b73f69140b8432, 1, false, 98304},
    {3, 17, 17, 17, 4, true, 53248, 4096, 128, 0, {0x58d24908, 0x00100000, 0x00600000, 0x00070000, 0xe1000010, 0x80100010, 0x03000000, 0},
     0xbf352aae0dc31cb6, 1, false, 52224},
    {3, 33, 33, 33, 8, false, 614400, 8192, 320, 4, {0x58d7ff83, 0x00100000, 0x00600000, 0x00070100, 0xe1000020, 0x80200020, 0x03000000, 0},
     0xa3ae3ecb66f064aa, 1, false, 614400},
    {3, 5, 300, 2, 4, false, 40960, 4096, 64, 1, {0x58d24908, 0x00100000, 0x00600000, 0x00070040, 0xe1000004, 0x8001012b, 0x03000000, 0},
     0xe8066eb5bd6c7d71, 1, false, 38912},
    {3, 64, 64, 64, 4, false, 1048576, 8192, 256, 4, {0x58d24908, 0x00100000, 0x00600000, 0x00070100, 0xe100003f, 0x803f003f, 0x03000000, 0},
     0x4a69c3a125f58076, 1, false, 1048576},
    {3, 1, 1, 11, 16, false, 8192, 8192, 64, 4, {0x58d7ff81, 0x00100000, 0x00600000, 0x00070100, 0xe1000000, 0x800a0000, 0x03000000, 0},
     0xfad2be41f8779e65, 1, false, 8192},
    {3, 129, 70, 6, 2, false, 184320, 4096, 320, 3, {0x7012491b, 0x00100000, 0x00600000, 0x000700c0, 0xe1000080, 0x80050045, 0x03000000, 0},
     0xe0c2d6f8c35d1a2d, 1, false, 184320},
    {2, 2, 2, 1, 4, false, 4096, 4096, 64, 0, {0x58d24908, 0x00100000, 0x00600000, 0x00070000, 0xe2800001, 0x80020001, 0x03000000, 0},
     0x0cc5a288dca2dce5, 3, false, 512},
    {2, 64, 64, 1, 4, false, 98304, 4096, 256, 0, {0x58d24908, 0x00100000, 0x00600000, 0x00070018, 0xe180003f, 0x8000003f, 0x03000000, 0},
     0xe26af6459727f0d1, 6, true, 16384},
    {2, 32, 32, 1, 8, false, 98304, 4096, 256, 0, {0x58d7ff83, 0x00100000, 0x00600000, 0x00070010, 0xe400001f, 0x8001001f, 0x03000000, 0},
     0x42cfeb0b5268b305, 12, true, 8192},
    {2, 300, 200, 1, 4, false, 1245184, 8192, 1216, 0, {0x58d24908, 0x00100000, 0x00600000, 0x00070020, 0xe280012b, 0x800300c7, 0x03000000, 0},
     0x0669d097886e5521, 4, false, 311296},
    {2, 128, 128, 1, 2, false, 196608, 8192, 256, 0, {0x7012491b, 0x00100000, 0x00600000, 0x00070020, 0xe180007f, 0x8000007f, 0x03000000, 0},
     0x2814460b41d6dfd5, 6, true, 32768},
    {2, 33, 9, 1, 16, false, 49152, 4096, 576, 0, {0x58d7ff81, 0x00100000, 0x00600000, 0x00070000, 0xe2800020, 0x80040008, 0x03000000, 0},
     0x670c8a7e7b780e8e, 5, false, 9216},
};
// clang-format on

uint32_t viewType(const NilView &n)
{
    if (n.dim != 2)
        return n.dim == 1 ? raw::fx::kTexOneD : raw::fx::kTexThreeD;
    return n.cube ? (n.layers > 6 ? raw::fx::kTexCubeArray : raw::fx::kTexCube) : raw::fx::kTexTwoDArray;
}

TEST(Layout, OneThreeDimensionalArrayAndCubeImagesAreNils)
{
    for (const NilView &n : kNilViews) {
        raw::fx::MipChain c;
        ASSERT_TRUE(n.dim == 1   ? raw::fx::mipChainLayout(n.width, n.height, 1, n.bpp, c)
                    : n.dim == 3 ? raw::fx::volumeLayout(n.width, n.height, n.depth, 1, n.bpp, n.view2d, c)
                                 : raw::fx::arrayLayout(n.width, n.height, n.layers, 1, n.bpp, c))
            << n.width << "x" << n.height << "x" << n.depth << "x" << n.layers;
        EXPECT_EQ(c.bytes, n.bytes) << n.width << "x" << n.height << "x" << n.depth << "x" << n.layers;
        EXPECT_EQ(c.align, n.align) << n.width << "x" << n.height << "x" << n.depth << "x" << n.layers;
        EXPECT_EQ(c.arrayStride, n.arrayStride) << n.width << "x" << n.height << "x" << n.depth << "x" << n.layers;
        EXPECT_EQ(c.level[0].rowBytes, n.rowBytes);
        EXPECT_EQ(c.zLog2, n.zLog2);
        uint32_t header[8];
        raw::fx::mipChainTic(c, 0x100000, n.header[0], false, 0, 1, header, viewType(n));
        for (int w = 0; w < 8; w++)
            EXPECT_EQ(header[w], n.header[w])
                << n.width << "x" << n.height << "x" << n.depth << "x" << n.layers << " word " << w;
        const size_t row = size_t(n.width) * n.bpp, plane = row * n.height;
        std::vector<uint8_t> linear(plane * n.depth * n.layers), tiled(c.bytes, 0);
        for (size_t k = 0; k < linear.size(); k++)
            linear[k] = uint8_t(k * 131 + k / 251 + 7);
        const uint32_t origin[3] = {0, 0, 0}, size[3] = {n.width, n.height, n.depth};
        if (n.dim == 3)
            raw::fx::writeVolume(c, tiled.data(), 0, origin, size, linear.data(), row, plane);
        else
            for (uint32_t l = 0; l < n.layers; l++)
                raw::fx::writeLevel(c, tiled.data() + l * c.arrayStride, 0, 0, 0, n.width, n.height,
                                    linear.data() + l * plane, row);
        uint64_t hash = 0xcbf29ce484222325ull;
        for (uint8_t b : tiled)
            hash = (hash ^ b) * 0x100000001b3ull;
        EXPECT_EQ(hash, n.hash) << n.width << "x" << n.height << "x" << n.depth << "x" << n.layers;
        if (n.dim == 3) {
            std::vector<uint8_t> back(linear.size(), 0);
            raw::fx::readVolume(c, tiled.data(), 0, origin, size, back.data(), row, plane);
            EXPECT_EQ(back, linear);
        }
    }
}

// NIL's headers of views of part of a 2D image (nilimage view: an image of `levels` levels and `layers` layers at 0x100000, the
// view's type, its first level and level count, its first layer and layer count)
struct NilPartView {
    uint32_t width, height, layers, levels, bpp, type, baseLevel, viewLevels, baseLayer, viewLayers;
    uint32_t header[8];
};

// clang-format off
const NilPartView kNilPartViews[] = {
    {17, 17, 3, 1, 4, raw::fx::kTexTwoD, 0, 1, 1, 1,
     {0x58d24908, 0x00101000, 0x00600000, 0x00070008, 0xe0800010, 0x80000010, 0x03000000, 0x00000000}},
    {17, 17, 3, 1, 4, raw::fx::kTexTwoDArray, 0, 1, 1, 2,
     {0x58d24908, 0x00101000, 0x00600000, 0x00070008, 0xe2800010, 0x80010010, 0x03000000, 0x00000000}},
    {64, 64, 6, 1, 4, raw::fx::kTexTwoD, 0, 1, 2, 1,
     {0x58d24908, 0x00108000, 0x00600000, 0x00070018, 0xe080003f, 0x8000003f, 0x03000000, 0x00000000}},
    {64, 64, 6, 1, 4, raw::fx::kTexTwoDArray, 0, 1, 0, 6,
     {0x58d24908, 0x00100000, 0x00600000, 0x00070018, 0xe280003f, 0x8005003f, 0x03000000, 0x00000000}},
    {64, 64, 12, 1, 8, raw::fx::kTexCube, 0, 1, 6, 6,
     {0x58d7ff83, 0x00130000, 0x00600000, 0x00070018, 0xe180003f, 0x8000003f, 0x03000000, 0x00000000}},
    {64, 64, 12, 1, 8, raw::fx::kTexCubeArray, 0, 1, 0, 12,
     {0x58d7ff83, 0x00100000, 0x00600000, 0x00070018, 0xe400003f, 0x8001003f, 0x03000000, 0x00000000}},
    {128, 128, 1, 8, 4, raw::fx::kTexTwoD, 1, 7, 0, 1,
     {0x58d24908, 0x00100000, 0x00600000, 0x70070020, 0xe080007f, 0x8000007f, 0x03000000, 0x00000071}},
    {128, 128, 1, 8, 4, raw::fx::kTexTwoD, 3, 1, 0, 1,
     {0x58d24908, 0x00100000, 0x00600000, 0x70070020, 0xe080007f, 0x8000007f, 0x03000000, 0x00000033}},
    {300, 200, 1, 9, 4, raw::fx::kTexTwoDArray, 2, 3, 0, 1,
     {0x58d24908, 0x00100000, 0x00600000, 0x80070020, 0xe280012b, 0x800000c7, 0x03000000, 0x00000042}},
};
// clang-format on

// raw::Device::createPartView's header: the part's layout (a chain's levels, an array's layers from its first one's address)
TEST(Layout, PartViewHeadersAreNils)
{
    for (const NilPartView &n : kNilPartViews) {
        raw::fx::MipChain c;
        ASSERT_TRUE(n.levels > 1 ? raw::fx::mipChainLayout(n.width, n.height, n.levels, n.bpp, c)
                                 : raw::fx::arrayLayout(n.width, n.height, n.layers, 1, n.bpp, c));
        c.layers = n.viewLayers;
        uint32_t header[8];
        raw::fx::mipChainTic(c, 0x100000 + n.baseLayer * c.arrayStride, n.header[0], false, n.baseLevel, n.viewLevels,
                             header, n.type);
        for (int w = 0; w < 8; w++)
            EXPECT_EQ(header[w], n.header[w]) << n.width << "x" << n.height << "x" << n.layers << " levels " << n.levels
                                              << " view type " << n.type << " word " << w;
    }
}

// NIL's multisampled 2D images (nilimage ms): size, alignment, row stride (in samples), block height, the header of a view of
// it at 0x100000 and the FNV-1a hash after nil_copy_linear_to_tiled of the pattern over its extent in samples
struct NilMultisampled {
    uint32_t width, height, bpp, samples;
    uint64_t bytes, align;
    uint32_t rowBytes, yLog2;
    uint32_t header[8];
    uint64_t hash;
};

// clang-format off
const NilMultisampled kNilMultisampled[] = {
    {720, 1280, 4, 4, 14745600, 8192, 5760, 4,
     {0x58d24908, 0x00100000, 0x00600000, 0x00070020, 0xe08002cf, 0x800004ff, 0x03000000, 0x00000200}, 0x915559f6e8bb5d65},
    {720, 1280, 2, 4, 7372800, 8192, 2880, 4,
     {0x7012491b, 0x00100000, 0x00600000, 0x00070020, 0xe08002cf, 0x800004ff, 0x03000000, 0x00000200}, 0x585d28ea07b715d6},
    {1280, 720, 4, 2, 7864320, 8192, 10240, 4,
     {0x58d24908, 0x00100000, 0x00600000, 0x00070020, 0xe08004ff, 0x800002cf, 0x03000000, 0x00000500}, 0x304b7a0c46c0a706},
    {64, 64, 8, 2, 65536, 4096, 1024, 3,
     {0x58d7ff83, 0x00100000, 0x00600000, 0x00070018, 0xe080003f, 0x8000003f, 0x03000000, 0x00000500}, 0xf7657a78fe778072},
    {33, 9, 4, 4, 8192, 4096, 320, 0,
     {0x58d24908, 0x00100000, 0x00600000, 0x00070000, 0xe0800020, 0x80000008, 0x03000000, 0x00000200}, 0x8675d8eb1b7f81ee},
    {100, 50, 16, 8, 819200, 4096, 6400, 3,
     {0x58d7ff81, 0x00100000, 0x00600000, 0x00070018, 0xe0800063, 0x80000031, 0x03000000, 0x00000400}, 0x1f0f460a8643026e},
    {720, 1280, 4, 1, 3686400, 8192, 2880, 4,
     {0x58d24908, 0x00100000, 0x00600000, 0x00070020, 0xe08002cf, 0x800004ff, 0x03000000, 0x00000000}, 0x5694e54830c071ae},
};
// clang-format on

TEST(Layout, MultisampledImagesAreNils)
{
    for (const NilMultisampled &n : kNilMultisampled) {
        raw::fx::MipChain c;
        ASSERT_TRUE(raw::fx::msLayout(n.width, n.height, n.samples, n.bpp, c)) << n.width << "x" << n.height;
        EXPECT_EQ(c.bytes, n.bytes) << n.width << "x" << n.height << " x" << n.samples;
        EXPECT_EQ(c.align, n.align) << n.width << "x" << n.height << " x" << n.samples;
        EXPECT_EQ(c.level[0].rowBytes, n.rowBytes) << n.width << "x" << n.height << " x" << n.samples;
        EXPECT_EQ(c.yLog2, n.yLog2) << n.width << "x" << n.height << " x" << n.samples;
        uint32_t header[8];
        raw::fx::mipChainTic(c, 0x100000, n.header[0], false, 0, 1, header);
        for (int w = 0; w < 8; w++)
            EXPECT_EQ(header[w], n.header[w]) << n.width << "x" << n.height << " x" << n.samples << " word " << w;
        const uint32_t wSa = n.width << c.sxLog2, hSa = n.height << c.syLog2;
        std::vector<uint8_t> linear(size_t(wSa) * n.bpp * hSa), tiled(c.bytes, 0);
        for (size_t k = 0; k < linear.size(); k++)
            linear[k] = uint8_t(k * 131 + k / 251 + 7);
        raw::fx::writeLevel(c, tiled.data(), 0, 0, 0, wSa, hSa, linear.data(), size_t(wSa) * n.bpp);
        uint64_t hash = 0xcbf29ce484222325ull;
        for (uint8_t b : tiled)
            hash = (hash ^ b) * 0x100000001b3ull;
        EXPECT_EQ(hash, n.hash) << n.width << "x" << n.height << " x" << n.samples;
    }
}

// A 1D image is NIL's 2D layout of one row (Tiling::choose leaves a 1D image one GOB tall, where a 2D image of height 1 clamps
// to), under a header of TEXTURE_TYPE ONE_D (nvb097_fill_image_view_desc for ViewType::_1D); nothing else in the header moves
TEST(Layout, OneDHeaderDiffersFromTwoDInItsTypeAlone)
{
    raw::fx::MipChain c;
    ASSERT_TRUE(raw::fx::mipChainLayout(1024, 1, 1, 4, c));
    EXPECT_EQ(c.yLog2, 0);
    uint32_t one[8], two[8];
    raw::fx::mipChainTic(c, 0x100000, 0x7017ff8f, false, 0, 1, one, raw::fx::kTexOneD);
    raw::fx::mipChainTic(c, 0x100000, 0x7017ff8f, false, 0, 1, two);
    EXPECT_EQ(one[4] >> 23 & 0xf, 0u);  // TEXTURE_TYPE, bits 154:151
    EXPECT_EQ(two[4] >> 23 & 0xf, 1u);
    EXPECT_EQ(one[4] & 0xffff, 1023u);  // WIDTH_MINUS_ONE
    EXPECT_EQ(one[5] & 0xffff, 0u);     // HEIGHT_MINUS_ONE
    for (int w : {0, 1, 2, 3, 5, 6, 7})
        EXPECT_EQ(one[w], two[w]) << w;
    EXPECT_EQ(one[4] & ~(0xfu << 23), two[4] & ~(0xfu << 23));
}

TEST(Layout, RefusesWhatNilDoesNotTile)
{
    tgm_layout l;
    EXPECT_FALSE(tgm_layout_tiled(0, 4, 4, &l));
    EXPECT_FALSE(tgm_layout_tiled(4, 0, 4, &l));
    EXPECT_FALSE(tgm_layout_tiled(16385, 4, 4, &l));
    for (uint32_t bpp : {0u, 3u, 6u, 12u, 32u})
        EXPECT_FALSE(tgm_layout_tiled(64, 64, bpp, &l)) << bpp;
}

// The display's contract for a tiled framebuffer: 16-GOB blocks, rows of width x 4 bytes in whole GOBs, the frame inside the
// pitch allocation, GOBs in the arrangement DC A scans (TegraScanoutCopy.h's byte formula).
TEST(Layout, IsTheDisplaysTiledFramebuffer)
{
    tgm_layout l;
    ASSERT_TRUE(tgm_layout_tiled(720, 1280, 4, &l));
    EXPECT_EQ(l.y_log2, 4);  // the DC's BLOCK_HEIGHT(4), NativeScanout.h kNativeBlockGobs
    EXPECT_EQ(l.row_bytes, 720u * 4);
    EXPECT_LE(l.bytes, 3932160u);
    for (uint32_t y = 0; y < 8; y++)
        for (uint32_t x = 0; x < 64; x++)
            EXPECT_EQ(tgm_layout_gob_offset(x, y),
                      ((x & 32) << 3) | ((y & 6) << 5) | ((x & 16) << 1) | ((y & 1) << 4) | (x & 15));
}

// Every byte of the image lands at its own offset inside the layout, and the CPU's copies through it give back the bytes.
TEST(Layout, CopiesRoundTrip)
{
    for (const NilImage &n : kNil) {
        if (uint64_t(n.width) * n.height * n.bpp > (1u << 18))
            continue;
        tgm_layout l;
        ASSERT_TRUE(tgm_layout_tiled(n.width, n.height, n.bpp, &l));
        const uint64_t pitch = uint64_t(n.width) * n.bpp + 5;
        std::vector<uint8_t> src(pitch * n.height), back(src.size()), tiled(l.bytes);
        for (size_t i = 0; i < src.size(); i++)
            src[i] = uint8_t(i * 131 + (i >> 9));
        tgm_layout_write(&l, tiled.data(), 0, 0, n.width, n.height, src.data(), pitch);
        tgm_layout_read(&l, tiled.data(), 0, 0, n.width, n.height, back.data(), pitch);
        for (uint32_t y = 0; y < n.height; y++)
            ASSERT_EQ(0, memcmp(&src[y * pitch], &back[y * pitch], size_t(n.width) * n.bpp))
                << n.width << "x" << n.height << " bpp " << n.bpp << " row " << y;
        std::vector<bool> seen(uint64_t(l.row_bytes) * l.rows);
        for (uint32_t y = 0; y < n.height; y++)
            for (uint32_t xB = 0; xB < n.width * n.bpp; xB++) {
                const uint64_t at = tgm_layout_offset(&l, xB, y);
                ASSERT_LT(at, seen.size());
                ASSERT_FALSE(seen[at]) << xB << "," << y;
                seen[at] = true;
            }
    }
}

TEST(Layout, CopiesOfASubRectangleTouchOnlyIt)
{
    tgm_layout l;
    ASSERT_TRUE(tgm_layout_tiled(300, 200, 4, &l));
    std::vector<uint8_t> tiled(l.bytes, 0xee), src(37 * 4 * 23, 0x5a);
    tgm_layout_write(&l, tiled.data(), 13, 101, 37, 23, src.data(), 37 * 4);
    for (uint32_t y = 0; y < 200; y++)
        for (uint32_t x = 0; x < 300; x++) {
            const bool inside = x >= 13 && x < 50 && y >= 101 && y < 124;
            EXPECT_EQ(tiled[tgm_layout_offset(&l, x * 4, y)], inside ? 0x5a : 0xee) << x << "," << y;
        }
}

}  // namespace
