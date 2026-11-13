// SPDX-License-Identifier: BSD-2-Clause-Patent
// mpeg2gen: makes the MPEG-1/2 test data from FFmpeg (not part of the build; run by hand when the data must change).
//   * reads elementary streams encoded once by FFmpeg (below) and splits them into the configuration (the first
//     sequence header and its extensions, an 'esds' DecoderSpecificInfo) and one sample per picture (with the sequence
//     and GOP headers before it);
//   * runs FFmpeg's trace_headers bitstream filter over every sample and records, per picture, what NVDEC's setup
//     takes from the headers (coding type, f_codes and full_pel, intra_dc_precision, structure, top_field_first,
//     frame_pred_frame_dct, concealment vectors, q_scale_type, intra_vlc_format, alternate_scan, progressive_frame)
//     and its slice count, and per clip the size, progressive_sequence and the quantiser matrices in force;
//   * writes ../MPEG2Clip.h and ../MPEG2Trace.h.
// FFmpeg's pixels are not recorded: MPEG-1/2's inverse DCT is specified to an accuracy (IEEE 1180), not bit for bit.
// Clips (FFmpeg 8.1.2, testsrc2):
//   A 176x144 MPEG-2 Main at 25, 18 frames: 2 B-pictures between references, both matrices loaded, alternate scan,
//     intra VLC table, non-linear quantiser scale, 10-bit intra DC, a sequence display extension
//       ffmpeg -f lavfi -i testsrc2=size=176x144:rate=25 -frames:v 18 -c:v mpeg2video -threads 1 -bf 2 -g 9 -qscale:v 14
//              -qmax 28 -intra_matrix <kIntra> -inter_matrix <kInter> -alternate_scan 1 -intra_vlc 1
//              -non_linear_quant 1 -dc 10 -seq_disp_ext always -f mpeg2video m2A.m2v
//   B 192x96 MPEG-2 interlaced (field DCT and field prediction in frame pictures, bottom field first), 12 frames
//       ffmpeg -f lavfi -i testsrc2=size=192x96:rate=25 -frames:v 12 -c:v mpeg2video -threads 1 -bf 1 -g 6 -qscale:v 10
//              -flags +ildct+ilme -top 0 -f mpeg2video m2B.m2v
//   C 176x144 MPEG-1, 15 frames: 2 B-pictures, a loaded intra matrix
//       ffmpeg -f lavfi -i testsrc2=size=176x144:rate=25 -frames:v 15 -c:v mpeg1video -threads 1 -bf 2 -g 8
//              -qscale:v 14 -intra_matrix <kInter> -f mpeg1video m2C.m2v
// trace_headers parses MPEG-1 as MPEG-2 (CBS has one syntax for both). Homebrew's FFmpeg 8.1.2 needs hevcgen.c's
// libx265.216 stand-in on DYLD_LIBRARY_PATH.
// Build and run (Homebrew FFmpeg; from this directory):
//   clang -std=c11 -O1 -I/opt/homebrew/opt/ffmpeg/include mpeg2gen.c -L/opt/homebrew/opt/ffmpeg/lib -lavcodec -lavutil
//   ./a.out <dir with m2A.m2v m2B.m2v m2C.m2v> ..
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavutil/log.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    MAX_PICTURES = 32,
    FIELDS = 16
};

static const char *const kFields[FIELDS] = {
    "picture_coding_type",
    "f_code[0][0]",
    "f_code[0][1]",
    "f_code[1][0]",
    "f_code[1][1]",
    "full_pel_forward_vector",
    "full_pel_backward_vector",
    "intra_dc_precision",
    "picture_structure",
    "top_field_first",
    "frame_pred_frame_dct",
    "concealment_motion_vectors",
    "q_scale_type",
    "intra_vlc_format",
    "alternate_scan",
    "progressive_frame",
};

static const uint8_t kZigzag[64] = {
    0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48,
    41, 34, 27, 20, 13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23,
    30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
};

typedef struct {
    int field[FIELDS], slices;
} Picture;

typedef struct {
    uint8_t *data;
    size_t size, config, start[MAX_PICTURES + 1];
    int pictures, width, height, progressive, mpeg2, traced;
    int intra[64], inter[64];
    Picture trace[MAX_PICTURES];
} Clip;

static Clip *gTracing;

static void logLine(void *avcl, int level, const char *fmt, va_list args)
{
    (void)avcl, (void)level;
    if (!gTracing)
        return;
    char line[512], name[128], bits[128];
    vsnprintf(line, sizeof(line), fmt, args);
    Clip *c = gTracing;
    if (strstr(line, "Picture Header") && !strchr(line, '=')) {
        if (c->traced < MAX_PICTURES) {
            Picture *p = &c->trace[c->traced++];
            memset(p, 0, sizeof(*p));
            p->field[1] = p->field[2] = p->field[3] = p->field[4] = -1;
            p->field[8] = 3;  // MPEG-1: frame pictures with frame prediction
            p->field[10] = p->field[15] = 1;
        }
        return;
    }
    int pos, value;
    if (sscanf(line, "%d %127s %127s = %d", &pos, name, bits, &value) != 4)
        return;
    Picture *p = c->traced ? &c->trace[c->traced - 1] : NULL;
    int index;
    if (!strcmp(name, "horizontal_size_value"))
        c->width = value;
    else if (!strcmp(name, "vertical_size_value"))
        c->height = value;
    else if (!strcmp(name, "progressive_sequence"))
        c->progressive = value, c->mpeg2 = 1;
    else if (!strcmp(name, "load_intra_quantiser_matrix") && !value && !p)
        for (int i = 0; i < 64; i++)
            c->intra[i] = -1;
    else if (sscanf(name, "intra_quantiser_matrix[%d]", &index) == 1 && index >= 0 && index < 64)
        c->intra[kZigzag[index]] = value;
    else if (sscanf(name, "non_intra_quantiser_matrix[%d]", &index) == 1 && index >= 0 && index < 64)
        c->inter[kZigzag[index]] = value;
    else if (!strcmp(name, "slice_vertical_position") && p)
        p->slices++;
    else if (p) {
        for (int i = 0; i < FIELDS; i++)
            if (!strcmp(name, kFields[i]))
                p->field[i] = value;
        if (!strcmp(name, "forward_f_code"))
            p->field[1] = p->field[2] = value;
        if (!strcmp(name, "backward_f_code"))
            p->field[3] = p->field[4] = value;
    }
}

static int load(Clip *c, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    fseek(f, 0, SEEK_END);
    c->size = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    c->data = malloc(c->size);
    const size_t read = fread(c->data, 1, c->size, f);
    fclose(f);
    if (read != c->size)
        return -1;
    // A sample starts with the sequence and GOP headers right before its picture; the first picture's are the
    // configuration.
    size_t headers = SIZE_MAX;
    for (size_t i = 0; i + 4 <= c->size && c->pictures < MAX_PICTURES; i++) {
        if (c->data[i] || c->data[i + 1] || c->data[i + 2] != 1)
            continue;
        const uint8_t code = c->data[i + 3];
        if (code == 0xb3 || code == 0xb8) {
            if (headers == SIZE_MAX)
                headers = i;
        } else if (code == 0x00) {
            if (!c->pictures)
                c->config = i;
            c->start[c->pictures] = c->pictures && headers != SIZE_MAX ? headers : i;
            c->pictures++;
            headers = SIZE_MAX;
        }
    }
    c->start[c->pictures] = c->size;
    return c->pictures ? 0 : -1;
}

static int trace(Clip *c)
{
    const AVBitStreamFilter *filter = av_bsf_get_by_name("trace_headers");
    AVBSFContext *bsf = NULL;
    if (!filter || av_bsf_alloc(filter, &bsf) < 0)
        return -1;
    bsf->par_in->codec_type = AVMEDIA_TYPE_VIDEO;
    bsf->par_in->codec_id = AV_CODEC_ID_MPEG2VIDEO;
    gTracing = c;
    if (av_bsf_init(bsf) < 0)
        return -1;
    AVPacket *packet = av_packet_alloc(), *out = av_packet_alloc();
    for (int i = 0; i < c->pictures; i++) {
        size_t from = i ? c->start[i] : 0;  // the first sample's configuration is traced with it
        av_new_packet(packet, (int)(c->start[i + 1] - from));
        memcpy(packet->data, c->data + from, (size_t)packet->size);
        av_bsf_send_packet(bsf, packet);
        while (av_bsf_receive_packet(bsf, out) == 0)
            av_packet_unref(out);
    }
    gTracing = NULL;
    av_packet_free(&packet);
    av_packet_free(&out);
    av_bsf_free(&bsf);
    return c->traced == c->pictures ? 0 : -1;
}

static void bytes(FILE *f, const char *name, const uint8_t *p, size_t n)
{
    fprintf(f, "static const uint8_t %s[%zu] = {", name, n);
    for (size_t i = 0; i < n; i++)
        fprintf(f, "%s0x%02x,", i % 20 ? "" : "\n    ", p[i]);
    fprintf(f, "\n};\n");
}

static void matrix(FILE *f, const char *name, const int *m)
{
    fprintf(f, "static const int %s[64] = {", name);
    for (int i = 0; i < 64; i++)
        fprintf(f, "%s%d,", i % 16 ? " " : "\n    ", m[i]);
    fprintf(f, "\n};\n");
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: mpeg2gen <clip dir> <out dir>\n");
        return 2;
    }
    av_log_set_level(AV_LOG_TRACE);
    av_log_set_callback(logLine);
    static Clip clips[3];
    const char *names[3] = {"A", "B", "C"};
    char path[1024];
    for (int i = 0; i < 3; i++) {
        for (int k = 0; k < 64; k++)
            clips[i].intra[k] = -1, clips[i].inter[k] = -1;
        snprintf(path, sizeof(path), "%s/m2%s.m2v", argv[1], names[i]);
        if (load(&clips[i], path) || trace(&clips[i])) {
            fprintf(stderr, "clip %s failed\n", names[i]);
            return 1;
        }
    }
    snprintf(path, sizeof(path), "%s/MPEG2Clip.h", argv[2]);
    FILE *f = fopen(path, "w");
    fprintf(f, "// Generated by test/mpeg2gen/mpeg2gen.c from FFmpeg-encoded clips (see there). Do not edit.\n"
               "#ifndef MPEG2_CLIP_H\n#define MPEG2_CLIP_H\n#include <stddef.h>\n#include <stdint.h>\n\n");
    for (int i = 0; i < 3; i++) {
        char name[32];
        snprintf(name, sizeof(name), "kMPEG2Clip%s", names[i]);
        bytes(f, name, clips[i].data, clips[i].size);
        fprintf(f, "static const size_t %sConfig = %zu;\nstatic const size_t %sPictures[%d] = {", name, clips[i].config,
                name, clips[i].pictures + 1);
        for (int v = 0; v <= clips[i].pictures; v++)
            fprintf(f, "%s%zu,", v % 10 ? " " : "\n    ", clips[i].start[v]);
        fprintf(f, "\n};\n");
    }
    fprintf(f, "\n#endif\n");
    fclose(f);
    snprintf(path, sizeof(path), "%s/MPEG2Trace.h", argv[2]);
    f = fopen(path, "w");
    fprintf(f, "// Generated by test/mpeg2gen/mpeg2gen.c: FFmpeg's trace_headers per picture. Do not edit.\n"
               "#ifndef MPEG2_TRACE_H\n#define MPEG2_TRACE_H\n\n"
               "// coding type, f_code[0][0], [0][1], [1][0], [1][1], full_pel forward, backward, intra_dc_precision,\n"
               "// structure, top_field_first, frame_pred_frame_dct, concealment, q_scale_type, intra_vlc_format,\n"
               "// alternate_scan, progressive_frame; then the slices\n"
               "typedef struct {\n    int field[16], slices;\n} MPEG2TracePicture;\n\n");
    for (int i = 0; i < 3; i++) {
        const Clip *c = &clips[i];
        fprintf(f, "static const int kMPEG2Trace%sSize[4] = {%d, %d, %d, %d};  // width, height, progressive, MPEG-2\n",
                names[i], c->width, c->height, c->mpeg2 ? c->progressive : 1, c->mpeg2);
        char name[48];
        snprintf(name, sizeof(name), "kMPEG2Trace%sIntra", names[i]);
        matrix(f, name, c->intra);
        snprintf(name, sizeof(name), "kMPEG2Trace%sInter", names[i]);
        matrix(f, name, c->inter);
        fprintf(f, "static const MPEG2TracePicture kMPEG2Trace%s[%d] = {\n", names[i], c->pictures);
        for (int v = 0; v < c->pictures; v++) {
            fprintf(f, "    {{");
            for (int k = 0; k < FIELDS; k++)
                fprintf(f, "%s%d", k ? ", " : "", c->trace[v].field[k]);
            fprintf(f, "}, %d},\n", c->trace[v].slices);
        }
        fprintf(f, "};\n");
    }
    fprintf(f, "\n#endif\n");
    fclose(f);
    printf("clip A %d, B %d, C %d pictures\n", clips[0].pictures, clips[1].pictures, clips[2].pictures);
    return 0;
}
