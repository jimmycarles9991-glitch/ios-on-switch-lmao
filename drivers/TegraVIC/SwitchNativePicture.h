// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "SwitchNativeSurface.h"

inline const char *switch_native_picture(const TegraVICPicture &picture, const TegraVICSurfaceDesc &target,
                                         switch_gpu_backing_layout &layout, SwitchNativeSurfaceCrop &crop)
{
    constexpr uint32_t nv12Video = 0x34323076, nv12Full = 0x34323066;
    constexpr uint32_t p010Video = 0x78343230, p010Full = 0x78663230;
    const uint32_t format = target.fourcc;
    if (picture.dstX || picture.dstY || picture.width != target.width || picture.height != target.height ||
        target.planes != 2 || target.protection)
        return "native export requires one complete unprotected destination image";
    if ((picture.sampleBytes == 1 && format != nv12Video && format != nv12Full) ||
        (picture.sampleBytes == 2 && format != p010Video && format != p010Full) ||
        (picture.sampleBytes != 1 && picture.sampleBytes != 2))
        return "native sample size differs from destination format";
    if (!picture.pitch || (picture.pitch & 63) || !picture.rows || (picture.rows & 31) ||
        picture.blockHeightLog2 != 1 || (picture.chromaOffset & 255))
        return "native decoder block geometry";
    if (!picture.width || !picture.height || ((picture.cropX | picture.cropY | picture.width | picture.height) & 1))
        return "native 4:2:0 crop geometry";
    const uint64_t right = uint64_t(picture.cropX) + picture.width;
    const uint64_t bottom = uint64_t(picture.cropY) + picture.height;
    if (right * picture.sampleBytes > picture.pitch || bottom > picture.rows)
        return "native picture crop exceeds allocation geometry";
    const uint64_t luma = uint64_t(picture.pitch) * picture.rows;
    const uint64_t chroma = luma / 2;
    if (picture.chromaOffset < luma || picture.chromaOffset > picture.bytes ||
        chroma > picture.bytes - picture.chromaOffset)
        return "native picture planes exceed allocation";
    switch_gpu_backing_layout result = {};
    result.format = format;
    result.plane_count = 2;
    result.color_matrix = target.colorMatrix;
    result.transfer = target.transfer;
    result.primaries = target.primaries;
    result.chroma_top = target.chromaTop;
    result.chroma_bottom = target.chromaBottom;
    result.color_range = format == nv12Full || format == p010Full ? 2 : 1;
    result.planes[0] = {0,
                        luma,
                        picture.pitch / picture.sampleBytes,
                        picture.rows,
                        picture.pitch,
                        picture.sampleBytes,
                        SWITCH_GPU_BACKING_BLOCK_LINEAR,
                        1};
    result.planes[1] = {picture.chromaOffset,
                        chroma,
                        picture.pitch / (2 * picture.sampleBytes),
                        picture.rows / 2,
                        picture.pitch,
                        2 * picture.sampleBytes,
                        SWITCH_GPU_BACKING_BLOCK_LINEAR,
                        1};
    layout = result;
    crop = {picture.cropX, picture.cropY, picture.width, picture.height};
    return nullptr;
}
