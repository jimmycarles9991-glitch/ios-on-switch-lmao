/* libvpx's VP9 encoder as Swift can call it: vpx_codec_enc_init and vpx_codec_control are macros, and a packet's
 * frame lives in an anonymous union. */
#ifndef SWITCH_VPX_H
#define SWITCH_VPX_H

#include <vpx/vp8cx.h>
#include <vpx/vpx_encoder.h>

static inline vpx_codec_err_t switch_vp9_init(vpx_codec_ctx_t *codec, const vpx_codec_enc_cfg_t *config)
{
    return vpx_codec_enc_init(codec, vpx_codec_vp9_cx(), config, 0);
}

static inline vpx_codec_err_t switch_vp9_set_cpu_used(vpx_codec_ctx_t *codec, int cpu_used)
{
    return vpx_codec_control(codec, VP8E_SET_CPUUSED, cpu_used);
}

static inline const void *switch_vpx_frame(const vpx_codec_cx_pkt_t *packet, size_t *size, vpx_codec_pts_t *pts)
{
    if (packet->kind != VPX_CODEC_CX_FRAME_PKT)
        return NULL;
    *size = packet->data.frame.sz;
    *pts = packet->data.frame.pts;
    return packet->data.frame.buf;
}

#endif
