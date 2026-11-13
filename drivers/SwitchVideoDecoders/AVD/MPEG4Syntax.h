// SPDX-License-Identifier: BSD-2-Clause-Patent
// MPEG-4 Part 2 (ISO/IEC 14496-2) headers as far as NVDEC's picture setup needs them: the visual object sequence,
// visual object and video object layer headers (6.2.2-6.2.3) out of an 'esds' DecoderSpecificInfo or a sample, and a
// VOP header up to vop_fcode_backward (6.2.5). NVDEC parses the VOP header again itself and decodes the macroblocks.
// Plain C, for the host tests.
#ifndef MPEG4_SYNTAX_H
#define MPEG4_SYNTAX_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    MPEG4_VOP_I = 0,
    MPEG4_VOP_P = 1,
    MPEG4_VOP_B = 2,
    MPEG4_VOP_S = 3
};

enum {
    MPEG4_START_VOS = 0xb0,
    MPEG4_START_USER_DATA = 0xb2,
    MPEG4_START_GOV = 0xb3,
    MPEG4_START_VO = 0xb5,
    MPEG4_START_VOP = 0xb6
};

typedef struct {
    bool valid;
    uint8_t profile_level, verid, object_type, chroma_format, shape, sprite_enable;
    bool colour_description, full_range;
    uint8_t colour_primaries, transfer, matrix;
    uint16_t width, height, time_resolution;
    uint8_t time_bits;
    bool interlaced, obmc_disable, not_8_bit, quant_type, quarter_sample, complexity_estimation;
    bool resync_marker_disable, data_partitioned, newpred, reduced_resolution, scalability;
    uint8_t intra_matrix[64], inter_matrix[64];  // raster order, the defaults (6.3.3) unless loaded
} MPEG4VOL;

typedef struct {
    uint8_t coding_type;  // MPEG4_VOP_*
    bool coded;
    uint32_t modulo_time_base;
    uint32_t time_increment;
    bool rounding;
    uint8_t intra_dc_vlc_thr, quant, fcode_forward, fcode_backward;
    bool top_field_first, alternate_vertical_scan;
} MPEG4VOP;

// The ES_Descriptor of an 'esds' atom's payload (version and flags first, ISO/IEC 14496-1 7.2.6.5):
// objectTypeIndication and the DecoderSpecificInfo bytes. false when malformed.
bool mpeg4_esds(const uint8_t *esds, size_t size, uint8_t *object_type, const uint8_t **dsi, size_t *dsi_size);

// Every header in `data` (start codes and all) into `vol`, which keeps what came before (a VOL needs its VO's verid
// and colour). Returns 1 when a VOL was parsed, 0 when none was there, -1 when one was malformed.
int mpeg4_parse_config(MPEG4VOL *vol, const uint8_t *data, size_t size);

// Why NVDEC does not take this VOL, or NULL: rectangular 8-bit 4:2:0 progressive Simple or Advanced Simple (no GMC,
// data partitioning, NEWPRED, reduced resolution or scalability), 48x16 to 2032x2032 (NVDEC 2.0's MPEG-4 range).
const char *mpeg4_vol_unsupported(const MPEG4VOL *vol);

// A group_of_vop header's time_code (6.2.4) in seconds, the time base the next VOP's modulo_time_base counts from;
// false when malformed.
bool mpeg4_parse_gov(const uint8_t *data, size_t size, uint32_t *seconds);

// The VOP header after a 00 00 01 B6 start code; false when malformed.
bool mpeg4_parse_vop(const MPEG4VOL *vol, const uint8_t *data, size_t size, MPEG4VOP *vop);

#endif
