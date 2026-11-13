// SPDX-License-Identifier: MIT
// The MAXWELL_B registers outside raw_owned.h's that the raw path writes between a pass's last draw and the next pass's
// first, other than by binding a state: a pass's epilogue (RenderEncoder::end: the raster state and base registers put
// back, the swizzle undone) and the next one's prologue (its targets, clears and zcull region). A state segment that sets
// one of them is bound again at every pass's first draw (raw_state_segment.h HeldSegment).
#pragma once
#include <cstdint>
#include "../../../../kits/SwitchGM20B/SwitchGM20B3D.h"
#include "raw_owned.h"

namespace raw {

// clang-format off
constexpr MethodRange kPassWrites[] = {
    {NVB197_SET_POLY_OFFSET_POINT, NVB197_SET_POLY_OFFSET_POINT + 4},
    {NVB197_SET_POLY_OFFSET_LINE, NVB197_SET_POLY_OFFSET_LINE + 4},
    {NVB197_SET_POLY_OFFSET_FILL, NVB197_SET_POLY_OFFSET_FILL + 4},
    {NVB197_SET_DEPTH_BIAS, NVB197_SET_DEPTH_BIAS + 4},
    {NVB197_SET_SLOPE_SCALE_DEPTH_BIAS, NVB197_SET_SLOPE_SCALE_DEPTH_BIAS + 4},
    {NVB197_SET_DEPTH_BIAS_CLAMP, NVB197_SET_DEPTH_BIAS_CLAMP + 4},
    {NVB197_OGL_SET_CULL, NVB197_OGL_SET_CULL + 4},
    {NVB197_OGL_SET_FRONT_FACE, NVB197_OGL_SET_FRONT_FACE + 4},
    {NVB197_OGL_SET_CULL_FACE, NVB197_OGL_SET_CULL_FACE + 4},
    {NVB197_SET_FRONT_POLYGON_MODE, NVB197_SET_FRONT_POLYGON_MODE + 4},
    {NVB197_SET_BACK_POLYGON_MODE, NVB197_SET_BACK_POLYGON_MODE + 4},
    {NVB197_SET_GLOBAL_BASE_INSTANCE_INDEX, NVB197_SET_GLOBAL_BASE_INSTANCE_INDEX + 4},
    {NVB197_SET_GLOBAL_BASE_VERTEX_INDEX, NVB197_SET_GLOBAL_BASE_VERTEX_INDEX + 4},
    {NVB197_SET_VERTEX_ID_BASE, NVB197_SET_VERTEX_ID_BASE + 4},
    {NVB197_SET_VIEWPORT_COORDINATE_SWIZZLE(0), NVB197_SET_VIEWPORT_COORDINATE_SWIZZLE(16)},
    {NVB197_SET_COLOR_COMPRESSION(0), NVB197_SET_COLOR_COMPRESSION(8)},
    {NVB197_SET_COLOR_CLEAR_VALUE(0), NVB197_SET_COLOR_CLEAR_VALUE(4)},
    {NVB197_SET_CLEAR_RECT_HORIZONTAL, NVB197_SET_CLEAR_RECT_VERTICAL + 4},
    {NVB197_SET_CLEAR_SURFACE_CONTROL, NVB197_SET_CLEAR_SURFACE_CONTROL + 4},
    {NVB197_SET_RENDER_ENABLE_OVERRIDE, NVB197_SET_RENDER_ENABLE_OVERRIDE + 4},
    {NVB197_SET_Z_CLEAR_VALUE, NVB197_SET_Z_CLEAR_VALUE + 4},
    {NVB197_SET_STENCIL_CLEAR_VALUE, NVB197_SET_STENCIL_CLEAR_VALUE + 4},
    {NVB197_SET_ZT_SELECT, NVB197_SET_ZT_SELECT + 4},
    {NVB197_SET_ZT_LAYER, NVB197_SET_ZT_LAYER + 4},
    {NVB197_SET_Z_COMPRESSION, NVB197_SET_Z_COMPRESSION + 4},
    {NVB197_SET_STENCIL_COMPRESSION, NVB197_SET_STENCIL_COMPRESSION + 4},
    {NVB197_SET_ZT_SPARSE, NVB197_SET_ZT_SPARSE + 4},
    // the zcull region (raw_zcull.h): its location, storage, size and offsets, subregions, criterion and direction
    {0x0370, 0x0374},
    {0x02E8, 0x02EC},
    {0x02F8, 0x0300},
    {0x07C0, 0x07F8},
    {0x0DBC, 0x0DC0},
    {0x0DD8, 0x0DDC},
    {0x1590, 0x1594},
    {0x15C8, 0x15CC},
    {0x15FC, 0x1604},
};
// clang-format on

constexpr bool passWrites(uint32_t m)
{
    for (const MethodRange &r : kPassWrites)
        if (m >= r.begin && m < r.end)
            return true;
    return false;
}

}  // namespace raw
