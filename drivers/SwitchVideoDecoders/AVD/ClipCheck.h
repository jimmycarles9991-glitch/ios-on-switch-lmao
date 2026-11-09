// SPDX-License-Identifier: BSD-2-Clause-Patent
// The clip checks: test/VDECClip.h's H.264 clip (320x240, 30 frames, High profile with B-pyramid and 3 references) and
// test/VP9Clip.h's VP9 clip, decoded through VideoToolbox, every output frame's luma CRC compared with the reference
// encoder's reconstruction (x264) or decoder's output (libvpx).
//
// On the Switch, test/vdectest runs it requiring the hardware decoder: that proves the whole path, VideoToolbox -> this
// plug-in -> TegraNVDEC -> NVDEC -> the de-swizzle, against an independent reference. On a Mac the same command checks
// the clips against Apple's decoders, and the host tests drive it through this plug-in's VideoToolbox plumbing.
#ifndef CLIP_CHECK_H
#define CLIP_CHECK_H
#include <CoreFoundation/CoreFoundation.h>
#include <stdbool.h>
#include <stddef.h>

typedef enum {
    ClipCheckHardware,      // kVTVideoDecoderSpecification_RequireHardwareAcceleratedVideoDecoder
    ClipCheckSoftware,      // kVTVideoDecoderSpecification_EnableHardwareAcceleratedVideoDecoder = false
    ClipCheckVP9,           // test/VP9Clip.h (512x288, superframes, segmentation, 2 tile columns) on a hardware 'vp09'
                            // decoder, against libvpx's output
    ClipCheckHEVC,          // test/HEVCClip.h's Main clips A (x265: B-pyramid, RASL, explicit RPS, 2 slices, scaling
                            // lists) and V (Apple's encoder, conformance window) on a hardware 'hvc1' decoder, against
                            // FFmpeg's output
    ClipCheckHEVC10,        // clip T (Main 10) the same way as 'x420', its high bytes against FFmpeg's samples >> 2
    ClipCheckHEVCSoftware,  // clips A and V with hardware disabled: on a Mac, Apple's decoder proves clips and CRCs
} ClipCheckMode;

// One result line ("hardware: hardware decoder, 30/30 frames match x264, ... OK"); true when every frame matched and,
// for ClipCheckHardware, the session reported a hardware decoder. extraSpec (NULL on the device) adds decoder
// specification entries: the host test names the plug-in under test among the Mac's own decoders.
bool clip_check(ClipCheckMode mode, CFDictionaryRef extraSpec, char *line, size_t size);

#endif
