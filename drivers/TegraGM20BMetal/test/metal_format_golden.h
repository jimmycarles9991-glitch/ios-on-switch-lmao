// SPDX-License-Identifier: MIT
// Metal's description of each pixel format it describes (MTLPixelFormatGetInfoForDevice, Apple SPI), as m2v measured
// it on macOS 27.0.1 on an Apple M4 (build 26A434) and the Metal-to-Vulkan device answered it before it was deleted:
// third_party/m2v/crates/metal2vulkan-runtime/src/pixel_format_info.rs DESCRIBED at 153e523f3. A format of blocks
// (flags bit 0x400: `blocks`) is its block's bytes, width and height; another its texel's bytes, 1 and 1. Generated and
// clang-formatted, not edited.
#pragma once
#include <cstdint>

namespace metal_format_golden {

struct Block {
    uint16_t mtl;
    uint8_t bytes, blockW, blockH;
    bool blocks;
};

inline constexpr Block kDescribed[] = {
    {1, 1, 1, 1, false},     {10, 1, 1, 1, false},   {11, 1, 1, 1, false},    {12, 1, 1, 1, false},
    {13, 1, 1, 1, false},    {14, 1, 1, 1, false},   {20, 2, 1, 1, false},    {22, 2, 1, 1, false},
    {23, 2, 1, 1, false},    {24, 2, 1, 1, false},   {25, 2, 1, 1, false},    {26, 2, 1, 1, false},
    {30, 2, 1, 1, false},    {31, 2, 1, 1, false},   {32, 2, 1, 1, false},    {33, 2, 1, 1, false},
    {34, 2, 1, 1, false},    {40, 2, 1, 1, false},   {41, 2, 1, 1, false},    {42, 2, 1, 1, false},
    {43, 2, 1, 1, false},    {44, 2, 1, 1, false},   {53, 4, 1, 1, false},    {54, 4, 1, 1, false},
    {55, 4, 1, 1, false},    {60, 4, 1, 1, false},   {62, 4, 1, 1, false},    {63, 4, 1, 1, false},
    {64, 4, 1, 1, false},    {65, 4, 1, 1, false},   {66, 4, 1, 1, false},    {70, 4, 1, 1, false},
    {71, 4, 1, 1, false},    {72, 4, 1, 1, false},   {73, 4, 1, 1, false},    {74, 4, 1, 1, false},
    {80, 4, 1, 1, false},    {81, 4, 1, 1, false},   {84, 4, 1, 1, false},    {85, 4, 1, 1, false},
    {90, 4, 1, 1, false},    {91, 4, 1, 1, false},   {92, 4, 1, 1, false},    {93, 4, 1, 1, false},
    {94, 4, 1, 1, false},    {103, 8, 1, 1, false},  {104, 8, 1, 1, false},   {105, 8, 1, 1, false},
    {110, 8, 1, 1, false},   {112, 8, 1, 1, false},  {113, 8, 1, 1, false},   {114, 8, 1, 1, false},
    {115, 8, 1, 1, false},   {116, 8, 1, 1, false},  {123, 16, 1, 1, false},  {124, 16, 1, 1, false},
    {125, 16, 1, 1, false},  {130, 8, 4, 4, true},   {131, 8, 4, 4, true},    {132, 16, 4, 4, true},
    {133, 16, 4, 4, true},   {134, 16, 4, 4, true},  {135, 16, 4, 4, true},   {140, 8, 4, 4, true},
    {141, 8, 4, 4, true},    {142, 16, 4, 4, true},  {143, 16, 4, 4, true},   {150, 16, 4, 4, true},
    {151, 16, 4, 4, true},   {152, 16, 4, 4, true},  {153, 16, 4, 4, true},   {160, 8, 8, 4, true},
    {161, 8, 8, 4, true},    {162, 8, 4, 4, true},   {163, 8, 4, 4, true},    {164, 8, 8, 4, true},
    {165, 8, 8, 4, true},    {166, 8, 4, 4, true},   {167, 8, 4, 4, true},    {170, 8, 4, 4, true},
    {172, 8, 4, 4, true},    {174, 16, 4, 4, true},  {176, 16, 4, 4, true},   {178, 16, 4, 4, true},
    {179, 16, 4, 4, true},   {180, 8, 4, 4, true},   {181, 8, 4, 4, true},    {182, 8, 4, 4, true},
    {183, 8, 4, 4, true},    {185, 16, 4, 2, true},  {186, 16, 4, 4, true},   {187, 16, 5, 4, true},
    {188, 16, 5, 5, true},   {189, 16, 6, 5, true},  {190, 16, 6, 6, true},   {191, 16, 8, 4, true},
    {192, 16, 8, 5, true},   {193, 16, 8, 6, true},  {194, 16, 8, 8, true},   {195, 16, 10, 5, true},
    {196, 16, 10, 6, true},  {197, 16, 10, 8, true}, {198, 16, 10, 10, true}, {199, 16, 12, 10, true},
    {200, 16, 12, 12, true}, {203, 16, 4, 2, true},  {204, 16, 4, 4, true},   {205, 16, 5, 4, true},
    {206, 16, 5, 5, true},   {207, 16, 6, 5, true},  {208, 16, 6, 6, true},   {209, 16, 8, 4, true},
    {210, 16, 8, 5, true},   {211, 16, 8, 6, true},  {212, 16, 8, 8, true},   {213, 16, 10, 5, true},
    {214, 16, 10, 6, true},  {215, 16, 10, 8, true}, {216, 16, 10, 10, true}, {217, 16, 12, 10, true},
    {218, 16, 12, 12, true}, {221, 16, 4, 2, true},  {222, 16, 4, 4, true},   {223, 16, 5, 4, true},
    {224, 16, 5, 5, true},   {225, 16, 6, 5, true},  {226, 16, 6, 6, true},   {227, 16, 8, 4, true},
    {228, 16, 8, 5, true},   {229, 16, 8, 6, true},  {230, 16, 8, 8, true},   {231, 16, 10, 5, true},
    {232, 16, 10, 6, true},  {233, 16, 10, 8, true}, {234, 16, 10, 10, true}, {235, 16, 12, 10, true},
    {236, 16, 12, 12, true}, {240, 4, 2, 1, true},   {241, 4, 2, 1, true},    {250, 2, 1, 1, false},
    {252, 4, 1, 1, false},   {253, 1, 1, 1, false},  {260, 8, 1, 1, false},   {261, 1, 1, 1, false},
    {263, 4, 1, 1, false},   {300, 4, 1, 1, false},  {301, 4, 1, 1, false},   {302, 4, 1, 1, false},
    {303, 4, 1, 1, false},   {304, 4, 1, 1, false},  {313, 8, 1, 1, false},   {314, 8, 1, 1, false},
    {315, 8, 1, 1, false},   {323, 16, 1, 1, false}, {324, 16, 1, 1, false},  {325, 16, 1, 1, false},
    {500, 1, 1, 1, false},   {501, 1, 1, 1, false},  {502, 1, 1, 1, false},   {503, 1, 1, 1, false},
    {504, 2, 1, 1, false},   {505, 2, 1, 1, false},  {506, 2, 1, 1, false},   {507, 2, 1, 1, false},
    {508, 1, 1, 1, false},   {509, 1, 1, 1, false},  {510, 1, 1, 1, false},   {520, 1, 1, 1, false},
    {521, 1, 1, 1, false},   {522, 1, 1, 1, false},  {523, 1, 1, 1, false},   {524, 2, 1, 1, false},
    {525, 2, 1, 1, false},   {526, 2, 1, 1, false},  {527, 2, 1, 1, false},   {528, 1, 1, 1, false},
    {529, 1, 1, 1, false},   {530, 1, 1, 1, false},  {540, 1, 1, 1, false},   {541, 1, 1, 1, false},
    {542, 1, 1, 1, false},   {543, 2, 1, 1, false},  {544, 2, 1, 1, false},   {545, 2, 1, 1, false},
    {546, 1, 1, 1, false},   {547, 1, 1, 1, false},  {548, 1, 1, 1, false},   {550, 2, 1, 1, false},
    {551, 2, 1, 1, false},   {552, 8, 1, 1, false},  {553, 8, 1, 1, false},   {554, 4, 1, 1, false},
    {555, 4, 1, 1, false},   {556, 8, 1, 1, false},  {560, 4, 1, 1, false},   {562, 1, 1, 1, false},
    {563, 2, 1, 1, false},   {564, 2, 1, 1, false},  {565, 2, 1, 1, false},   {566, 2, 1, 1, false},
    {567, 1, 1, 1, false},   {568, 1, 1, 1, false},  {569, 1, 1, 1, false},   {570, 2, 1, 1, false},
    {571, 2, 1, 1, false},   {572, 2, 1, 1, false},  {573, 2, 1, 1, false},   {574, 2, 1, 1, false},
    {575, 2, 1, 1, false},   {576, 2, 1, 1, false},  {577, 2, 1, 1, false},   {578, 4, 1, 1, false},
    {579, 4, 1, 1, false},   {580, 2, 1, 1, false},  {581, 2, 1, 1, false},   {582, 2, 1, 1, false},
    {583, 2, 1, 1, false},   {584, 2, 1, 1, false},  {585, 2, 1, 1, false},   {586, 4, 1, 1, false},
    {587, 4, 1, 1, false},   {588, 1, 1, 1, false},  {589, 2, 1, 1, false},   {590, 2, 1, 1, false},
    {591, 2, 1, 1, false},   {592, 2, 1, 1, false},  {593, 2, 1, 1, false},   {594, 1, 1, 1, false},
    {595, 1, 1, 1, false},   {596, 1, 1, 1, false},  {597, 2, 1, 1, false},   {598, 2, 1, 1, false},
    {599, 2, 1, 1, false},   {600, 2, 1, 1, false},  {601, 2, 1, 1, false},   {602, 2, 1, 1, false},
    {603, 2, 1, 1, false},   {604, 2, 1, 1, false},  {605, 4, 1, 1, false},   {606, 2, 1, 1, false},
    {607, 2, 1, 1, false},   {608, 2, 1, 1, false},  {609, 2, 1, 1, false},   {610, 2, 1, 1, false},
    {611, 1, 1, 1, false},   {612, 1, 1, 1, false},  {613, 1, 1, 1, false},   {615, 2, 1, 1, false},
    {616, 2, 1, 1, false},   {617, 2, 1, 1, false},  {618, 2, 1, 1, false},   {619, 2, 1, 1, false},
    {620, 2, 1, 1, false},   {621, 2, 1, 1, false},  {622, 2, 1, 1, false},   {623, 4, 1, 1, false},
    {624, 2, 1, 1, false},   {625, 2, 1, 1, false},  {626, 2, 1, 1, false},   {627, 2, 1, 1, false},
    {628, 4, 1, 1, false},   {629, 4, 1, 1, false},  {630, 8, 1, 1, false},   {631, 8, 1, 1, false},
    {632, 8, 1, 1, false},   {633, 8, 1, 1, false},  {634, 8, 1, 1, false},   {635, 8, 1, 1, false},
    {636, 8, 1, 1, false},   {637, 8, 1, 1, false},  {638, 8, 1, 1, false},   {639, 8, 1, 1, false},
    {640, 8, 1, 1, false},   {641, 8, 1, 1, false},  {642, 2, 1, 1, false},   {643, 1, 1, 1, false},
    {644, 4, 1, 1, false},   {645, 2, 1, 1, false},  {651, 4, 1, 1, false},   {652, 1, 1, 1, false},
};

}  // namespace metal_format_golden
