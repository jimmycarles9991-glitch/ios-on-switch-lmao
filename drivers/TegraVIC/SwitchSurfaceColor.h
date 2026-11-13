// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stdint.h>
#include <string.h>

static inline uint32_t switch_surface_color_number(const char *tag, const char *prefix)
{
    const size_t length = strlen(prefix);
    if (strncmp(tag, prefix, length))
        return UINT32_MAX;
    tag += length;
    if (!*tag)
        return UINT32_MAX;
    uint32_t value = 0;
    while (*tag) {
        if (*tag < '0' || *tag > '9' || value > 25)
            return UINT32_MAX;
        value = value * 10 + (uint32_t)(*tag++ - '0');
    }
    return value && value <= 255 ? value : UINT32_MAX;
}

static inline uint32_t switch_surface_transfer_tag(const char *tag)
{
    if (!tag)
        return 0;
    if (!strcmp(tag, "ITU_R_709_2"))
        return 1;
    if (!strcmp(tag, "SMPTE_240M_1995"))
        return 7;
    if (!strcmp(tag, "Linear"))
        return 8;
    if (!strcmp(tag, "IEC_sRGB"))
        return 13;
    if (!strcmp(tag, "SMPTE_ST_2084_PQ"))
        return 16;
    if (!strcmp(tag, "SMPTE_ST_428_1"))
        return 17;
    if (!strcmp(tag, "ITU_R_2100_HLG"))
        return 18;
    return switch_surface_color_number(tag, "TransferFunction#");
}

static inline uint32_t switch_surface_primaries_tag(const char *tag)
{
    if (!tag)
        return 0;
    if (!strcmp(tag, "ITU_R_709_2"))
        return 1;
    if (!strcmp(tag, "EBU_3213"))
        return 5;
    if (!strcmp(tag, "SMPTE_C"))
        return 6;
    if (!strcmp(tag, "ITU_R_2020"))
        return 9;
    if (!strcmp(tag, "DCI_P3"))
        return 11;
    if (!strcmp(tag, "P3_D65"))
        return 12;
    if (!strcmp(tag, "P22"))
        return 22;
    return switch_surface_color_number(tag, "ColorPrimaries#");
}

static inline uint32_t switch_surface_chroma_tag(const char *tag)
{
    if (!tag)
        return 0;
    static const char *const names[] = {"Left", "Center", "TopLeft", "Top", "BottomLeft", "Bottom"};
    for (uint32_t i = 0; i < 6; ++i)
        if (!strcmp(tag, names[i]))
            return i + 1;
    return UINT32_MAX;
}

static inline int switch_surface_chroma_site(uint32_t top, uint32_t bottom, uint32_t *site)
{
    if (top > 6 || bottom > 6 || (top && bottom && top != bottom))
        return 0;
    *site = top ? top : bottom;
    return 1;
}

// A DC window's colour stage is its YCbCr matrix on co-sited (Left) chroma. Neither the window nor VIC has a transfer
// or gamut stage, so a surface's transfer and primaries never decide which of them scans it.
static inline int switch_surface_dc_siting(uint32_t top, uint32_t bottom)
{
    uint32_t chroma = 0;
    return switch_surface_chroma_site(top, bottom, &chroma) && chroma <= 1;
}
