// SPDX-License-Identifier: BSD-2-Clause-Patent
// mpeg4gen: makes the MPEG-4 Part 2 test data from FFmpeg (not part of the build; run by hand when the data must change).
//   * reads raw MPEG-4 Visual streams encoded once by FFmpeg (below) and splits them into the configuration (VOS, VO
//     and VOL headers, an 'esds' DecoderSpecificInfo) and one sample per VOP;
//   * decodes every clip with FFmpeg's MPEG-4 decoder (one thread, FF_DEBUG_PICT_INFO) and records, per VOP, what its
//     decode_vop_header logged: the picture type, rounding, f_code and b_code, quarter sample, resync markers, the VOP
//     time (in vop_time_increment_resolution ticks) and the VOL's time increment bits;
//   * writes ../MPEG4Clip.h (clips A and B, and clip C's configuration) and ../MPEG4Trace.h (the per-VOP truth).
// FFmpeg's pixels are not recorded: MPEG-4 Part 2's inverse DCT is specified to an accuracy (IEEE 1180), not bit for
// bit, and FFmpeg's own IDCTs (simple, int, faani) decode these clips to different bytes.
// Clips (FFmpeg 8.1.2, testsrc2):
//   A 176x144 Advanced Simple at 30000/1001, 18 frames: 2 B-VOPs between references (time increments of 1001 ticks
//     out of 30000, 15-bit), quarter-sample motion, MPEG quantisation with both matrices loaded, resync markers
//       ffmpeg -f lavfi -i testsrc2=size=176x144:rate=30000/1001 -frames:v 18 -c:v mpeg4 -bf 2 -flags +qpel
//              -mpeg_quant 1 -intra_matrix <kIntra> -inter_matrix <kInter> -ps 300 -g 9 -qscale:v 8 -f m4v clipA.m4v
//   B 136x72 Simple at 25, 32 frames: I- and P-VOPs only, H.263 quantisation, no resync markers, a width of 8.5
//     macroblocks, modulo_time_base past each second
//       ffmpeg -f lavfi -i testsrc2=size=136x72:rate=25 -frames:v 32 -c:v mpeg4 -bf 0 -g 16 -qscale:v 10 -f m4v clipB.m4v
//   C 64x48, 2 frames, interlaced and data partitioned: only its configuration (the parser must find both)
//       ffmpeg -f lavfi -i testsrc2=size=64x48:rate=25 -frames:v 2 -c:v mpeg4 -flags +ildct+ilme -data_partitioning 1
//              -ps 200 -f m4v clipC.m4v
// Homebrew's FFmpeg 8.1.2 needs hevcgen.c's libx265.216 stand-in on DYLD_LIBRARY_PATH.
// Build and run (Homebrew FFmpeg; from this directory):
//   clang -std=c11 -O1 -I/opt/homebrew/opt/ffmpeg/include mpeg4gen.c -L/opt/homebrew/opt/ffmpeg/lib -lavcodec -lavutil
//   ./a.out <dir with clipA.m4v clipB.m4v clipC.m4v> ..
#include <libavcodec/avcodec.h>
#include <libavutil/log.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    MAX_VOPS = 64
};

typedef struct {
    char type;
    int qp, f_code, b_code, rnd, qpel, resync;
    long long time;
} Vop;

typedef struct {
    uint8_t *data;
    size_t size, config, start[MAX_VOPS + 1];
    int vops, time_bits;
    Vop trace[MAX_VOPS];
    int traced;
} Clip;

static Clip *gTracing;

static void logLine(void *avcl, int level, const char *fmt, va_list args)
{
    (void)avcl;
    (void)level;
    if (!gTracing)
        return;
    char line[512];
    vsnprintf(line, sizeof(line), fmt, args);
    int bits;
    const char *tb = strstr(line, "tincrbits:");
    if (tb && sscanf(tb, "tincrbits:%d", &bits) == 1)
        gTracing->time_bits = bits;
    if (strncmp(line, "qp:", 3) || gTracing->traced >= MAX_VOPS)
        return;
    Vop *v = &gTracing->trace[gTracing->traced];
    char pel;
    int size, pro, alt, top, part, w, a;
    if (sscanf(line, "qp:%d fc:%d,%d %c size:%d pro:%d alt:%d top:%d %cpel part:%d resync:%d w:%d a:%d rnd:%d", &v->qp,
               &v->f_code, &v->b_code, &v->type, &size, &pro, &alt, &top, &pel, &part, &v->resync, &w, &a,
               &v->rnd) != 14)
        return;
    v->qpel = pel == 'q';
    const char *t = strstr(line, "time:");
    if (!t || sscanf(t, "time:%lld", &v->time) != 1)
        return;
    gTracing->traced++;
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
    // A sample starts with the headers (VOS, VO, VOL, user data, GOV) right before its VOP, as an MP4 sync sample
    // carries them; the first VOP's headers are the configuration.
    c->vops = 0;
    size_t headers = SIZE_MAX;
    for (size_t i = 0; i + 4 <= c->size && c->vops < MAX_VOPS; i++) {
        if (c->data[i] || c->data[i + 1] || c->data[i + 2] != 1)
            continue;
        if (c->data[i + 3] != 0xb6) {
            if (headers == SIZE_MAX)
                headers = i;
            continue;
        }
        if (!c->vops)
            c->config = i;
        c->start[c->vops] = c->vops && headers != SIZE_MAX ? headers : i;
        c->vops++;
        headers = SIZE_MAX;
    }
    c->start[c->vops] = c->size;
    return c->vops ? 0 : -1;
}

static int trace(Clip *c)
{
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_MPEG4);
    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    ctx->thread_count = 1;
    ctx->debug = FF_DEBUG_PICT_INFO;
    ctx->extradata = av_mallocz(c->config + AV_INPUT_BUFFER_PADDING_SIZE);
    memcpy(ctx->extradata, c->data, c->config);
    ctx->extradata_size = (int)c->config;
    gTracing = c;  // the VOL in the extradata is logged by avcodec_open2
    if (avcodec_open2(ctx, codec, NULL) < 0)
        return -1;
    AVPacket *packet = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    for (int i = 0; i <= c->vops; i++) {
        if (i < c->vops) {
            av_new_packet(packet, (int)(c->start[i + 1] - c->start[i]));
            memcpy(packet->data, c->data + c->start[i], (size_t)packet->size);
            packet->pts = i;
            avcodec_send_packet(ctx, packet);
            av_packet_unref(packet);
        } else {
            avcodec_send_packet(ctx, NULL);
        }
        while (avcodec_receive_frame(ctx, frame) == 0)
            av_frame_unref(frame);
    }
    gTracing = NULL;
    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&ctx);
    return c->traced == c->vops && c->time_bits ? 0 : -1;
}

static void bytes(FILE *f, const char *name, const uint8_t *p, size_t n)
{
    fprintf(f, "static const uint8_t %s[%zu] = {", name, n);
    for (size_t i = 0; i < n; i++)
        fprintf(f, "%s0x%02x,", i % 20 ? "" : "\n    ", p[i]);
    fprintf(f, "\n};\n");
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: mpeg4gen <clip dir> <out dir>\n");
        return 2;
    }
    av_log_set_level(AV_LOG_DEBUG);
    av_log_set_callback(logLine);
    static Clip clips[3];
    const char *names[3] = {"A", "B", "C"};
    char path[1024];
    for (int i = 0; i < 3; i++) {
        snprintf(path, sizeof(path), "%s/clip%s.m4v", argv[1], names[i]);
        if (load(&clips[i], path) || (i < 2 && trace(&clips[i]))) {
            fprintf(stderr, "clip %s failed\n", names[i]);
            return 1;
        }
    }
    snprintf(path, sizeof(path), "%s/MPEG4Clip.h", argv[2]);
    FILE *f = fopen(path, "w");
    fprintf(f, "// Generated by test/mpeg4gen/mpeg4gen.c from FFmpeg-encoded clips (see there). Do not edit.\n"
               "#ifndef MPEG4_CLIP_H\n#define MPEG4_CLIP_H\n#include <stddef.h>\n#include <stdint.h>\n\n");
    for (int i = 0; i < 3; i++) {
        char name[32];
        snprintf(name, sizeof(name), "kMPEG4Clip%s", names[i]);
        bytes(f, name, clips[i].data, i < 2 ? clips[i].size : clips[i].config);
        if (i == 2)
            break;
        fprintf(f, "static const size_t %sConfig = %zu;\nstatic const size_t %sVOPs[%d] = {", name, clips[i].config,
                name, clips[i].vops + 1);
        for (int v = 0; v <= clips[i].vops; v++)
            fprintf(f, "%s%zu,", v % 10 ? " " : "\n    ", clips[i].start[v]);
        fprintf(f, "\n};\n");
    }
    fprintf(f, "\n#endif\n");
    fclose(f);
    snprintf(path, sizeof(path), "%s/MPEG4Trace.h", argv[2]);
    f = fopen(path, "w");
    fprintf(f, "// Generated by test/mpeg4gen/mpeg4gen.c: FFmpeg's decode_vop_header per VOP. Do not edit.\n"
               "#ifndef MPEG4_TRACE_H\n#define MPEG4_TRACE_H\n\n"
               "typedef struct {\n    char type;\n    int qp, f_code, b_code, rnd, qpel, resync;\n    long long time;\n"
               "} MPEG4TraceVOP;\n\n");
    for (int i = 0; i < 2; i++) {
        fprintf(f, "static const int kMPEG4Trace%sTimeBits = %d;\nstatic const MPEG4TraceVOP kMPEG4Trace%s[%d] = {\n",
                names[i], clips[i].time_bits, names[i], clips[i].vops);
        for (int v = 0; v < clips[i].vops; v++) {
            const Vop *t = &clips[i].trace[v];
            fprintf(f, "    {'%c', %d, %d, %d, %d, %d, %d, %lld},\n", t->type, t->qp, t->f_code, t->b_code, t->rnd,
                    t->qpel, t->resync, t->time);
        }
        fprintf(f, "};\n");
    }
    fprintf(f, "\n#endif\n");
    fclose(f);
    printf("clip A %d VOPs, clip B %d VOPs\n", clips[0].vops, clips[1].vops);
    return 0;
}
