// SPDX-License-Identifier: BSD-2-Clause-Patent
// See MPEG4Syntax.h. Clause numbers are ISO/IEC 14496-2:2004.
#include "MPEG4Syntax.h"
#include <string.h>
#include "AVDBits.h"

enum {
    SHAPE_RECTANGULAR = 0,
    SPRITE_GMC = 2
};

static const uint8_t kZigzag[64] = {
    0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48,
    41, 34, 27, 20, 13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23,
    30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
};

// 6.3.3 default_intra_quant_mat and default_non_intra_quant_mat, raster order.
static const uint8_t kDefaultIntra[64] = {
    8,  17, 18, 19, 21, 23, 25, 27, 17, 18, 19, 21, 23, 25, 27, 28, 20, 21, 22, 23, 24, 26,
    28, 30, 21, 22, 23, 24, 26, 28, 30, 32, 22, 23, 24, 26, 28, 30, 32, 35, 23, 24, 26, 28,
    30, 32, 35, 38, 25, 26, 28, 30, 32, 35, 38, 41, 27, 28, 30, 32, 35, 38, 41, 45,
};
static const uint8_t kDefaultInter[64] = {
    16, 17, 18, 19, 20, 21, 22, 23, 17, 18, 19, 20, 21, 22, 23, 24, 18, 19, 20, 21, 22, 23,
    24, 25, 19, 20, 21, 22, 23, 24, 26, 27, 20, 21, 22, 23, 25, 26, 27, 28, 21, 22, 23, 24,
    26, 27, 28, 30, 22, 23, 24, 26, 27, 28, 30, 31, 23, 24, 25, 27, 28, 30, 31, 33,
};

// An expandable descriptor size (14496-1 8.3.3): up to four bytes of 7 bits.
static bool descriptor(const uint8_t *p, size_t size, size_t *pos, uint8_t tag, size_t *length)
{
    if (*pos >= size || p[(*pos)++] != tag)
        return false;
    size_t n = 0;
    for (int i = 0; i < 4; i++) {
        if (*pos >= size)
            return false;
        const uint8_t b = p[(*pos)++];
        n = n << 7 | (b & 0x7f);
        if (!(b & 0x80))
            break;
    }
    *length = n;
    return *pos + n <= size;
}

bool mpeg4_esds(const uint8_t *esds, size_t size, uint8_t *object_type, const uint8_t **dsi, size_t *dsi_size)
{
    size_t pos = 4, length = 0;  // version and flags
    if (!descriptor(esds, size, &pos, 0x03, &length) || pos + 3 > size)
        return false;
    const size_t end = pos + length;
    const uint8_t flags = esds[pos + 2];
    pos += 3;
    if (flags & 0x80)
        pos += 2;
    if (flags & 0x40)
        pos += pos < end ? 1 + esds[pos] : 0;
    if (flags & 0x20)
        pos += 2;
    if (pos > end || !descriptor(esds, end, &pos, 0x04, &length) || length < 13)
        return false;
    *object_type = esds[pos];
    const size_t configEnd = pos + length;
    pos += 13;
    *dsi = NULL;
    *dsi_size = 0;
    if (pos < configEnd && descriptor(esds, configEnd, &pos, 0x05, &length)) {
        *dsi = esds + pos;
        *dsi_size = length;
    }
    return true;
}

static void loadMatrix(AVDBits *b, uint8_t *matrix)
{
    uint8_t last = 0;
    for (int i = 0; i < 64; i++) {
        const uint8_t v = (uint8_t)avd_u(b, 8);
        if (!v) {  // 6.3.3: a 0 ends the list and the last value repeats to the end
            for (int j = i; j < 64; j++)
                matrix[kZigzag[j]] = last;
            return;
        }
        matrix[kZigzag[i]] = last = v;
    }
}

// 6.2.2 VisualObject: the verid a VOL inherits and the video signal type.
static void parseVisualObject(MPEG4VOL *vol, AVDBits *b)
{
    vol->verid = 1;
    if (avd_u(b, 1)) {
        vol->verid = (uint8_t)avd_u(b, 4);
        avd_skip(b, 3);
    }
    const uint32_t type = avd_u(b, 4);
    if ((type == 1 || type == 2) && avd_u(b, 1)) {
        avd_skip(b, 3);
        vol->full_range = avd_u(b, 1);
        vol->colour_description = avd_u(b, 1);
        if (vol->colour_description) {
            vol->colour_primaries = (uint8_t)avd_u(b, 8);
            vol->transfer = (uint8_t)avd_u(b, 8);
            vol->matrix = (uint8_t)avd_u(b, 8);
        }
    }
}

static void parseVolControl(MPEG4VOL *vol, AVDBits *b)
{
    vol->chroma_format = (uint8_t)avd_u(b, 2);
    avd_skip(b, 1);     // low_delay
    if (avd_u(b, 1)) {  // vbv_parameters: bit rate, buffer size and occupancy halves, a marker after all but one
        static const uint8_t kFields[] = {15, 15, 15, 3, 11, 15};
        for (unsigned i = 0; i < sizeof(kFields); i++) {
            avd_skip(b, kFields[i]);
            if (kFields[i] != 3)
                avd_marker(b);
        }
    }
}

// 6.2.3 VideoObjectLayer, up to scalability (the features NVDEC refuses are recorded, not parsed past).
static bool parseVOL(MPEG4VOL *vol, AVDBits *b)
{
    avd_skip(b, 1);  // random_accessible_vol
    vol->object_type = (uint8_t)avd_u(b, 8);
    uint8_t verid = vol->verid ? vol->verid : 1;
    if (avd_u(b, 1)) {
        verid = (uint8_t)avd_u(b, 4);
        avd_skip(b, 3);
    }
    if (avd_u(b, 4) == 15)  // aspect_ratio_info: extended PAR
        avd_skip(b, 16);
    vol->chroma_format = 1;
    if (avd_u(b, 1))
        parseVolControl(vol, b);
    vol->shape = (uint8_t)avd_u(b, 2);
    if (vol->shape != SHAPE_RECTANGULAR)
        return !b->bad;
    avd_marker(b);
    vol->time_resolution = (uint16_t)avd_u(b, 16);
    avd_marker(b);
    if (!vol->time_resolution)
        return false;
    vol->time_bits = 1;
    while ((1u << vol->time_bits) < vol->time_resolution)
        vol->time_bits++;
    if (avd_u(b, 1))  // fixed_vop_rate
        avd_skip(b, vol->time_bits);
    avd_marker(b);
    vol->width = (uint16_t)avd_u(b, 13);
    avd_marker(b);
    vol->height = (uint16_t)avd_u(b, 13);
    avd_marker(b);
    vol->interlaced = avd_u(b, 1);
    vol->obmc_disable = avd_u(b, 1);
    vol->sprite_enable = (uint8_t)avd_u(b, verid == 1 ? 1 : 2);
    if (vol->sprite_enable)
        return !b->bad;
    vol->not_8_bit = avd_u(b, 1);
    if (vol->not_8_bit)
        return !b->bad;
    vol->quant_type = avd_u(b, 1);
    memcpy(vol->intra_matrix, kDefaultIntra, 64);
    memcpy(vol->inter_matrix, kDefaultInter, 64);
    if (vol->quant_type) {
        if (avd_u(b, 1))
            loadMatrix(b, vol->intra_matrix);
        if (avd_u(b, 1))
            loadMatrix(b, vol->inter_matrix);
    }
    vol->quarter_sample = verid != 1 && avd_u(b, 1);
    vol->complexity_estimation = !avd_u(b, 1);
    if (vol->complexity_estimation)
        return !b->bad;
    vol->resync_marker_disable = avd_u(b, 1);
    vol->data_partitioned = avd_u(b, 1);
    if (vol->data_partitioned)
        avd_skip(b, 1);  // reversible_vlc
    if (verid != 1) {
        vol->newpred = avd_u(b, 1);
        if (vol->newpred)
            avd_skip(b, 3);
        vol->reduced_resolution = avd_u(b, 1);
    }
    vol->scalability = avd_u(b, 1);
    return !b->bad;
}

int mpeg4_parse_config(MPEG4VOL *vol, const uint8_t *data, size_t size)
{
    int found = 0;
    for (size_t at = avd_next_start_code(data, size, 0); at + 4 <= size;) {
        const uint8_t code = data[at + 3];
        const size_t next = avd_next_start_code(data, size, at + 4);
        AVDBits b = avd_bits(data + at + 4, next - at - 4);
        if (code == MPEG4_START_VOS) {
            vol->profile_level = (uint8_t)avd_u(&b, 8);
        } else if (code == MPEG4_START_VO) {
            parseVisualObject(vol, &b);
        } else if (code >= 0x20 && code <= 0x2f) {
            if (!parseVOL(vol, &b))
                return -1;
            vol->valid = true;
            found = 1;
        } else if (code == MPEG4_START_VOP) {
            break;
        }
        at = next;
    }
    return found;
}

const char *mpeg4_vol_unsupported(const MPEG4VOL *vol)
{
    return !vol->valid                               ? "no video object layer"
           : vol->shape != SHAPE_RECTANGULAR         ? "not rectangular"
           : vol->sprite_enable                      ? "sprites or global motion compensation"
           : vol->not_8_bit                          ? "not 8-bit"
           : vol->chroma_format != 1                 ? "not 4:2:0"
           : vol->interlaced                         ? "interlaced"
           : !vol->obmc_disable                      ? "overlapped block motion compensation"
           : vol->complexity_estimation              ? "complexity estimation headers"
           : vol->data_partitioned                   ? "data partitioning"
           : vol->newpred                            ? "NEWPRED"
           : vol->reduced_resolution                 ? "reduced resolution VOPs"
           : vol->scalability                        ? "scalability"
           : vol->width < 48 || vol->height < 16     ? "below NVDEC's 48x16"
           : vol->width > 2032 || vol->height > 2032 ? "beyond NVDEC's MPEG-4 2032x2032"
                                                     : NULL;
}

bool mpeg4_parse_gov(const uint8_t *data, size_t size, uint32_t *seconds)
{
    AVDBits b = avd_bits(data, size);
    const uint32_t hours = avd_u(&b, 5), minutes = avd_u(&b, 6);
    avd_marker(&b);
    *seconds = avd_u(&b, 6) + 60 * (minutes + 60 * hours);
    return !b.bad;
}

bool mpeg4_parse_vop(const MPEG4VOL *vol, const uint8_t *data, size_t size, MPEG4VOP *vop)
{
    AVDBits b = avd_bits(data, size);
    memset(vop, 0, sizeof(*vop));
    vop->coding_type = (uint8_t)avd_u(&b, 2);
    while (avd_u(&b, 1) && !b.bad)
        vop->modulo_time_base++;
    avd_marker(&b);
    vop->time_increment = avd_u(&b, vol->time_bits);
    avd_marker(&b);
    vop->coded = avd_u(&b, 1);
    vop->fcode_forward = vop->fcode_backward = 1;
    if (!vop->coded || b.bad)
        return !b.bad;
    if (vop->coding_type == MPEG4_VOP_P || (vop->coding_type == MPEG4_VOP_S && vol->sprite_enable == SPRITE_GMC))
        vop->rounding = avd_u(&b, 1);
    vop->intra_dc_vlc_thr = (uint8_t)avd_u(&b, 3);
    if (vol->interlaced) {
        vop->top_field_first = avd_u(&b, 1);
        vop->alternate_vertical_scan = avd_u(&b, 1);
    }
    vop->quant = (uint8_t)avd_u(&b, 5);
    if (vop->coding_type != MPEG4_VOP_I)
        vop->fcode_forward = (uint8_t)avd_u(&b, 3);
    if (vop->coding_type == MPEG4_VOP_B)
        vop->fcode_backward = (uint8_t)avd_u(&b, 3);
    return !b.bad && vop->quant && vop->fcode_forward && vop->fcode_backward;
}
