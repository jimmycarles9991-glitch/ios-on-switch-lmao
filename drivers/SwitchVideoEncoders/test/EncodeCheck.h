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
#include <stdbool.h>
#include <stddef.h>

// One result line ("ok 30/30 frames, hardware encoder, keys 0 10 15 25, PSNR min Y 41.2 U 44.0 V 43.8 dB, ...");
// true when every frame decoded at `minPsnr` dB or better in every plane, sync samples fell where they should, and
// the session said hardware. `spec` (NULL: RequireHardwareAcceleratedVideoEncoder) is the encoder specification;
// `entropy` (NULL: the encoder's choice) sets H264EntropyMode.
bool encode_check(CFDictionaryRef spec, CFStringRef entropy, double minPsnr, char *line, size_t size);

#endif
