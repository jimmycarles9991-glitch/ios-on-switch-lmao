// SPDX-License-Identifier: MIT
// raw/fx host oracle: Mesa NIL's layout, nil_image_view_descriptor's texture header
// for a view of the whole image, and where nil_copy_linear_to_tiled puts a pattern (an FNV-1a hash of the image's bytes).
// test/raw_layout_test.cpp holds its lines and checks fx's layout, header and writes against them.
// usage: nilimage <dim 1|2|3> WxHxDxL <bytes a texel 1|2|4|8|16> <2d-view 0|1> <view: plain|cube>...
// prints per image: "dim W H D L bpp view2d view | size align array_stride | level offset row_stride y_log2 z_log2 ... |
// header words | hash"
//    or: nilimage view WxHxL <levels> <bpp> <2d|2da|cube|cubea> <base level> <levels> <base layer> <layers>...
// prints per view of a 2D image (array) of that many levels: its header at 0x100000, the image's base
//    or: nilimage ms WxH <bpp> <samples>...
// prints per multisampled image: "WxH bpp samples layout | size align | row_stride y_log2 | header words | hash | storage
// header words" (the hash of the pattern copied in over its extent in samples; the storage header NVK binds the image as a
// storage image with)
//    or: nilimage mip <bgra8|rgba8|rgba16f> WxHxL...
// prints per chain: "W H L size align | offset,row_stride,level_size ..."
// HAVE_FUNC_ATTRIBUTE_PACKED must match NIL's nv_device_info enum layout.
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nil.h"

static enum pipe_format formatOf(uint32_t bpp)
{
    switch (bpp) {
    case 1:
        return PIPE_FORMAT_R8_UNORM;
    case 2:
        return PIPE_FORMAT_R16_UNORM;
    case 4:
        return PIPE_FORMAT_R8G8B8A8_UNORM;
    case 8:
        return PIPE_FORMAT_R16G16B16A16_FLOAT;
    default:
        return PIPE_FORMAT_R32G32B32A32_FLOAT;
    }
}

// Views of part of a 2D image of `levels` levels and L layers: 8 arguments a view
static int views(const struct nv_device_info *dev, int n, char **v)
{
    for (int a = 0; a + 7 < n; a += 8) {
        uint32_t w = 0, h = 0, layers = 1;
        if (sscanf(v[a], "%ux%ux%u", &w, &h, &layers) < 2)
            return 2;
        const uint32_t levels = (uint32_t)atoi(v[a + 1]), bpp = (uint32_t)atoi(v[a + 2]),
                       baseLevel = (uint32_t)atoi(v[a + 4]), viewLevels = (uint32_t)atoi(v[a + 5]),
                       baseLayer = (uint32_t)atoi(v[a + 6]), viewLayers = (uint32_t)atoi(v[a + 7]);
        const char *type = v[a + 3];
        const struct nil_format f = nil_format(formatOf(bpp));
        struct nil_image_init_info info;
        memset(&info, 0, sizeof info);
        info.dim = NIL_IMAGE_DIM_2D;
        info.format = f;
        info.extent_px.width = w;
        info.extent_px.height = h;
        info.extent_px.depth = 1;
        info.extent_px.array_len = layers;
        info.levels = levels;
        info.samples = 1;
        info.modifier = DRM_FORMAT_MOD_INVALID;
        struct nil_image img;
        if (!nil_image_init(dev, &img, &info)) {
            printf("%ux%ux%u %u refused\n", w, h, layers, levels);
            continue;
        }
        struct nil_view view;
        memset(&view, 0, sizeof view);
        view.view_type = !strcmp(type, "2da")     ? NIL_VIEW_TYPE_2D_ARRAY
                         : !strcmp(type, "cube")  ? NIL_VIEW_TYPE_CUBE
                         : !strcmp(type, "cubea") ? NIL_VIEW_TYPE_CUBE_ARRAY
                                                  : NIL_VIEW_TYPE_2D;
        view.access = NIL_VIEW_ACCESS_TEXTURE;
        view.format = f;
        view.base_level = baseLevel;
        view.num_levels = viewLevels;
        view.base_array_layer = baseLayer;
        view.array_len = viewLayers;
        view.swizzle[0] = PIPE_SWIZZLE_X;
        view.swizzle[1] = PIPE_SWIZZLE_Y;
        view.swizzle[2] = PIPE_SWIZZLE_Z;
        view.swizzle[3] = PIPE_SWIZZLE_W;
        const struct nil_descriptor desc = nil_image_view_descriptor(dev, &img, &view, 0x100000);
        printf("%ux%ux%u %u %u %s %u %u %u %u |", w, h, layers, levels, bpp, type, baseLevel, viewLevels, baseLayer,
               viewLayers);
        for (int k = 0; k < 8; k++)
            printf(" %08x", desc.bits[k]);
        printf("\n");
    }
    return 0;
}

// Multisampled 2D images, one level: 3 arguments an image
static int multisampled(const struct nv_device_info *dev, int n, char **v)
{
    for (int a = 0; a + 2 < n; a += 3) {
        uint32_t w = 0, h = 0;
        if (sscanf(v[a], "%ux%u", &w, &h) != 2)
            return 2;
        const uint32_t bpp = (uint32_t)atoi(v[a + 1]), samples = (uint32_t)atoi(v[a + 2]);
        const struct nil_format f = nil_format(formatOf(bpp));
        struct nil_image_init_info info;
        memset(&info, 0, sizeof info);
        info.dim = NIL_IMAGE_DIM_2D;
        info.format = f;
        info.extent_px.width = w;
        info.extent_px.height = h;
        info.extent_px.depth = 1;
        info.extent_px.array_len = 1;
        info.levels = 1;
        info.samples = samples;
        info.modifier = DRM_FORMAT_MOD_INVALID;
        struct nil_image img;
        if (!nil_image_init(dev, &img, &info)) {
            printf("%ux%u %u %u refused\n", w, h, bpp, samples);
            continue;
        }
        struct nil_view view;
        memset(&view, 0, sizeof view);
        view.view_type = NIL_VIEW_TYPE_2D;
        view.access = NIL_VIEW_ACCESS_TEXTURE;
        view.format = f;
        view.num_levels = 1;
        view.array_len = 1;
        view.swizzle[0] = PIPE_SWIZZLE_X;
        view.swizzle[1] = PIPE_SWIZZLE_Y;
        view.swizzle[2] = PIPE_SWIZZLE_Z;
        view.swizzle[3] = PIPE_SWIZZLE_W;
        const struct nil_descriptor desc = nil_image_view_descriptor(dev, &img, &view, 0x100000);
        view.access = NIL_VIEW_ACCESS_STORAGE;
        const struct nil_descriptor storage = nil_image_view_descriptor(dev, &img, &view, 0x100000);
        printf("%ux%u %u %u %u | %" PRIu64 " %u | %u %u |", w, h, bpp, samples, (unsigned)img.sample_layout, img.size_B,
               img.align_B, img.levels[0].row_stride_B, img.levels[0].tiling.y_log2);
        for (int k = 0; k < 8; k++)
            printf(" %08x", desc.bits[k]);
        const struct nil_Extent4D_Bytes level = nil_extent4d_px_to_B(img.extent_px, f, img.sample_layout);
        const size_t row = level.width, bytes = row * level.height;
        uint8_t *linear = malloc(bytes), *tiled = calloc(1, img.size_B);
        for (size_t k = 0; k < bytes; k++)
            linear[k] = (uint8_t)(k * 131 + k / 251 + 7);
        const struct nil_Offset4D_Bytes at = {0, 0, 0, 0};
        nil_copy_linear_to_tiled(tiled, level, linear, row, bytes, at, level, &img.levels[0].tiling);
        uint64_t hash = 0xcbf29ce484222325ull;
        for (uint64_t k = 0; k < img.size_B; k++)
            hash = (hash ^ tiled[k]) * 0x100000001b3ull;
        printf(" | %016" PRIx64 " |", hash);
        for (int k = 0; k < 8; k++)
            printf(" %08x", storage.bits[k]);
        printf("\n");
        free(linear);
        free(tiled);
    }
    return 0;
}

static int mip(const struct nv_device_info *dev, const char *format, int n, char **v)
{
    const enum pipe_format pf = !strcmp(format, "bgra8")     ? PIPE_FORMAT_B8G8R8A8_UNORM
                                : !strcmp(format, "rgba8")   ? PIPE_FORMAT_R8G8B8A8_UNORM
                                : !strcmp(format, "rgba16f") ? PIPE_FORMAT_R16G16B16A16_FLOAT
                                                             : PIPE_FORMAT_NONE;
    if (pf == PIPE_FORMAT_NONE)
        return 2;
    for (int a = 0; a < n; a++) {
        unsigned w, h, levels;
        char tail;
        if (sscanf(v[a], "%ux%ux%u%c", &w, &h, &levels, &tail) != 3 || !w || !h || !levels)
            return 2;
        const struct nil_image_init_info info = {
            .dim = NIL_IMAGE_DIM_2D,
            .format = nil_format(pf),
            .extent_px = {.width = w, .height = h, .depth = 1, .array_len = 1},
            .levels = levels,
            .samples = 1,
            .modifier = DRM_FORMAT_MOD_INVALID,
        };
        struct nil_image img;
        if (!nil_image_init(dev, &img, &info)) {
            fprintf(stderr, "NIL refused %s %s\n", format, v[a]);
            return 1;
        }
        printf("%u %u %u %" PRIu64 " %u |", w, h, levels, img.size_B, img.align_B);
        for (unsigned l = 0; l < levels; l++)
            printf(" %" PRIu64 ",%u,%" PRIu64, nil_image_level_layer_offset_B(&img, l, 0), img.levels[l].row_stride_B,
                   nil_image_level_size_B(&img, l));
        printf("\n");
    }
    return 0;
}

int main(int argc, char **argv)
{
    const bool viewMode = argc > 1 && !strcmp(argv[1], "view"), msMode = argc > 1 && !strcmp(argv[1], "ms"),
               mipMode = argc > 1 && !strcmp(argv[1], "mip");
    if (argc < 2 || (!viewMode && !msMode && !mipMode && (argc - 1) % 5) ||
        (viewMode && (argc < 10 || (argc - 2) % 8)) || (msMode && (argc < 5 || (argc - 2) % 3)) ||
        (mipMode && argc < 4)) {
        fprintf(
            stderr,
            "usage: %s (<dim> WxHxDxL <bpp> <view2d> <plain|cube>)...\n"
            "       %s view (WxHxL <levels> <bpp> <2d|2da|cube|cubea> <base level> <levels> <base layer> <layers>)...\n"
            "       %s ms (WxH <bpp> <samples>)...\n"
            "       %s mip <bgra8|rgba8|rgba16f> WxHxL...\n",
            argv[0], argv[0], argv[0], argv[0]);
        return 2;
    }
    struct nv_device_info dev;
    memset(&dev, 0, sizeof dev);
    dev.type = NV_DEVICE_TYPE_SOC;
    dev.chipset = 0x12b;
    dev.sm = 53;
    dev.cls_copy = 0xb0b5;
    dev.cls_eng2d = 0x902d;
    dev.cls_eng3d = 0xb197;
    dev.cls_compute = 0xb1c0;
    if (mipMode)
        return mip(&dev, argv[2], argc - 3, argv + 3);
    if (msMode)
        return multisampled(&dev, argc - 2, argv + 2);
    if (viewMode)
        return views(&dev, argc - 2, argv + 2);
    for (int a = 1; a + 4 < argc; a += 5) {
        const uint32_t dim = (uint32_t)atoi(argv[a]), bpp = (uint32_t)atoi(argv[a + 2]),
                       view2d = (uint32_t)atoi(argv[a + 3]), cube = !strcmp(argv[a + 4], "cube");
        uint32_t w = 0, h = 0, d = 0, layers = 1;
        if (sscanf(argv[a + 1], "%ux%ux%ux%u", &w, &h, &d, &layers) < 3)
            return 2;
        const struct nil_format f = nil_format(formatOf(bpp));
        struct nil_image_init_info info;
        memset(&info, 0, sizeof info);
        info.dim = dim == 1 ? NIL_IMAGE_DIM_1D : dim == 2 ? NIL_IMAGE_DIM_2D : NIL_IMAGE_DIM_3D;
        info.format = f;
        info.extent_px.width = w;
        info.extent_px.height = h;
        info.extent_px.depth = dim == 3 ? d : 1;
        info.extent_px.array_len = layers;
        info.levels = 1;
        info.samples = 1;
        info.usage = view2d ? NIL_IMAGE_USAGE_2D_VIEW_BIT : 0;
        info.modifier = DRM_FORMAT_MOD_INVALID;
        struct nil_image img;
        if (!nil_image_init(&dev, &img, &info)) {
            printf("%u %u %u %u %u %u %u %s refused\n", dim, w, h, d, layers, bpp, view2d, argv[a + 4]);
            continue;
        }
        printf("%u %u %u %u %u %u %u %s | %" PRIu64 " %u %" PRIu64 " |", dim, w, h, d, layers, bpp, view2d, argv[a + 4],
               img.size_B, img.align_B, img.array_stride_B);
        for (uint32_t l = 0; l < img.num_levels; l++)
            printf(" %" PRIu64 " %u %u %u", img.levels[l].offset_B, img.levels[l].row_stride_B,
                   img.levels[l].tiling.y_log2, img.levels[l].tiling.z_log2);
        struct nil_view view;
        memset(&view, 0, sizeof view);
        view.view_type = dim == 1             ? NIL_VIEW_TYPE_1D
                         : dim == 3           ? NIL_VIEW_TYPE_3D
                         : cube && layers > 6 ? NIL_VIEW_TYPE_CUBE_ARRAY
                         : cube               ? NIL_VIEW_TYPE_CUBE
                         : layers > 1         ? NIL_VIEW_TYPE_2D_ARRAY
                                              : NIL_VIEW_TYPE_2D;
        view.access = NIL_VIEW_ACCESS_TEXTURE;
        view.format = f;
        view.num_levels = 1;
        view.array_len = layers;
        view.swizzle[0] = PIPE_SWIZZLE_X;
        view.swizzle[1] = PIPE_SWIZZLE_Y;
        view.swizzle[2] = PIPE_SWIZZLE_Z;
        view.swizzle[3] = PIPE_SWIZZLE_W;
        const struct nil_descriptor desc = nil_image_view_descriptor(&dev, &img, &view, 0x100000);
        printf(" |");
        for (int k = 0; k < 8; k++)
            printf(" %08x", desc.bits[k]);
        const size_t row = (size_t)w * bpp, plane = row * h, planes = dim == 3 ? d : 1;
        uint8_t *linear = malloc(plane * planes * layers), *tiled = calloc(1, img.size_B);
        for (size_t k = 0; k < plane * planes * layers; k++)
            linear[k] = (uint8_t)(k * 131 + k / 251 + 7);
        const struct nil_Extent4D_Bytes level = {w * bpp, h, (uint32_t)planes, 1};
        const struct nil_Offset4D_Bytes at = {0, 0, 0, 0};
        for (uint32_t l = 0; l < layers; l++)
            nil_copy_linear_to_tiled(tiled + l * img.array_stride_B, level, linear + l * plane * planes, row, plane, at,
                                     level, &img.levels[0].tiling);
        uint64_t hash = 0xcbf29ce484222325ull;
        for (uint64_t k = 0; k < img.size_B; k++)
            hash = (hash ^ tiled[k]) * 0x100000001b3ull;
        printf(" | %016" PRIx64 "\n", hash);
        free(linear);
        free(tiled);
    }
    return 0;
}
