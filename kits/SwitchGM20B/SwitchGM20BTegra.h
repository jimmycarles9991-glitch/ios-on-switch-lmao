// SPDX-License-Identifier: MIT
// GM20B methods NVIDIA's open class headers do not name, in their naming style with a SWITCH_ prefix.
//
// 3D: the tiled cache (Maxwell's binning rasterizer) and the pixel-ordered texture data cache invalidate, as deko3d has
// them (source/maxwell/engine_3d.def, gpu_3d_base.cpp, MIT) for NVN on the Tegra X1.
// Host: Tegra's host1x syncpoint methods, as nvgpu has them (include/nvgpu/hw/gm20b/hw_pbdma_gm20b.h
// pbdma_syncpoint{a,b}, gk20a/fifo_gk20a.c, MIT). SYNCPOINTB's index is 8 bits: syncpoints 0-255 only.
#pragma once

#define SWITCH_NVB197_SET_TILED_CACHE_ENABLE           0x0f60
#define SWITCH_NVB197_SET_TILED_CACHE_TILE_SIZE        0x0f64
#define SWITCH_NVB197_SET_TILED_CACHE_TILE_SIZE_WIDTH  15 : 0
#define SWITCH_NVB197_SET_TILED_CACHE_TILE_SIZE_HEIGHT 31 : 16
#define SWITCH_NVB197_SET_TILED_CACHE_CONFIG_0         0x0f68
#define SWITCH_NVB197_SET_TILED_CACHE_CONFIG_1         0x0f6c
#define SWITCH_NVB197_SET_TILED_CACHE_CONFIG_3         0x0f70
// After PIXEL_SHADER_BARRIER: invalidates the texture data cache once the earlier pixels have written (NVN's
// ORDER_FRAGMENTS barrier). The NVIDIA headers' _NO_WFI form acts when the front end reads it, ahead of those pixels.
#define SWITCH_NVB197_INVALIDATE_TEXTURE_DATA_CACHE_PIXEL_ORDERED 0x0f74
#define SWITCH_NVB197_TILED_CACHE_BARRIER                         0x0f7c
#define SWITCH_NVB197_TILED_CACHE_FLUSH                           0x0f80
#define SWITCH_NVB197_SET_TILED_CACHE_CONFIG_2                    0x1108

#define SWITCH_NVB06F_SYNCPOINTA                 0x0070
#define SWITCH_NVB06F_SYNCPOINTA_PAYLOAD         31 : 0
#define SWITCH_NVB06F_SYNCPOINTB                 0x0074
#define SWITCH_NVB06F_SYNCPOINTB_OPERATION       0 : 0
#define SWITCH_NVB06F_SYNCPOINTB_OPERATION_WAIT  0x00000000
#define SWITCH_NVB06F_SYNCPOINTB_OPERATION_INCR  0x00000001
#define SWITCH_NVB06F_SYNCPOINTB_WAIT_SWITCH     4 : 4
#define SWITCH_NVB06F_SYNCPOINTB_WAIT_SWITCH_DIS 0x00000000
#define SWITCH_NVB06F_SYNCPOINTB_WAIT_SWITCH_EN  0x00000001
#define SWITCH_NVB06F_SYNCPOINTB_SYNCPT_INDEX    15 : 8
