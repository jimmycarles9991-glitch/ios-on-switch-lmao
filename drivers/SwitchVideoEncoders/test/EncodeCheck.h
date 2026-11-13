// SPDX-License-Identifier: BSD-2-Clause-Patent
// The encode check: a synthetic 320x240 clip (30 frames: a still gradient, a moving square, a flickering checker)
// encoded through VideoToolbox's hardware H.264 encoder with a key frame every 10 pictures and one forced at frame 15,
// then decoded through VideoToolbox and compared with the source, plane by plane (PSNR).
//
// On the Switch, AVE (H264H7.videoencoder) runs it once per boot in the first process that may open TegraNVENC
// (claimed from the kernel), requiring the hardware encoder: that proves VideoToolbox -> this plug-in -> TegraNVENC ->
// NVENC -> the slices, against a decoder (NVDEC through AVD, or Apple's H264SW). The host tests run it through the
// plug-in over a lossless fake engine on a Mac, where every frame must come back exact.
#ifndef ENCODE_CHECK_H
#define ENCODE_CHECK_H
#include <CoreFoundation/CoreFoundation.h>
#include <CoreVideo/CoreVideo.h>
#include <stdbool.h>
#include <stddef.h>

// One result line ("ok 30/30 frames, hardware encoder, keys 0 10 15 25, PSNR min Y 41.2 U 44.0 V 43.8 dB, ...");
// true when every frame decoded at `minPsnr` dB or better in every plane, sync samples fell where they should, and
// the session said hardware. `spec` (NULL: RequireHardwareAcceleratedVideoEncoder) is the encoder specification;
// `entropy` (NULL: the encoder's choice) sets H264EntropyMode. Frames come from the session's pixel buffer pool; the
// line ends with the plug-in's input routes ("inputs N by VIC B bounced M copied R refused", -1 from another
// encoder).
bool encode_check(CFDictionaryRef spec, CFStringRef entropy, double minPsnr, char *line, size_t size);
// Frame i of that clip (320x240 NV12), and a decoded picture's PSNR against it (Y, U, V; 99 when exact).
CVPixelBufferRef encode_check_frame(int i);
void encode_check_psnr(CVImageBufferRef image, int i, double psnr[3]);
// True: frames from CVPixelBufferCreate with CoreVideo's own rows instead of the session's pool.
extern bool encode_check_client_buffers;

#endif
