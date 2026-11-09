// SPDX-License-Identifier: BSD-2-Clause-Patent
// See ClipCheck.h.
#include "ClipCheck.h"
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <VideoToolbox/VideoToolbox.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "../test/VDECClip.h"
#include "../test/VP9Clip.h"
#include "../test/HEVCClip.h"
#include "../../../kits/SwitchCRC/SwitchCRC.h"

extern const CFStringRef kVTVideoDecoderSpecification_AllowVP9Decode, kVTVideoDecoderSpecification_DecoderID;

typedef struct {
    int width, height, frames;
    const uint32_t *crcs;  // reference luma CRC of each frame, by presentation index
    int count, match, errors, outOfOrder, last, first;
    uint32_t firstCrc;
} Result;

// Frames carry their display position as the presentation time and each output is checked against that frame's CRC.
// Apple's own H.264 decoders put frames out in decode order even with temporal processing (macOS 27, both hardware
// and software), leaving display order to the client, so the order is reported, not required.
static void output(void *refcon, void *frameRefcon, OSStatus status, VTDecodeInfoFlags flags, CVImageBufferRef image,
                   CMTime pts, CMTime duration)
{
    (void)frameRefcon, (void)flags, (void)duration;
    Result *r = refcon;
    if (status || !image) {
        if (status)
            r->errors++;
        return;
    }
    CVPixelBufferLockBaseAddress(image, kCVPixelBufferLock_ReadOnly);
    const uint8_t *y = CVPixelBufferGetBaseAddressOfPlane(image, 0);
    const size_t pitch = CVPixelBufferGetBytesPerRowOfPlane(image, 0);
    const OSType format = CVPixelBufferGetPixelFormatType(image);
    const bool wide = format == kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange ||
                      format == kCVPixelFormatType_420YpCbCr10BiPlanarFullRange;
    uint32_t crc = 0;
    for (int row = 0; row < r->height; row++) {
        const uint8_t *line = y + (size_t)row * pitch;
        if (!wide) {
            crc = switch_crc32(crc, line, (size_t)r->width);
            continue;
        }
        // 'x420' holds 10 bits in the high bits of each little-endian 16-bit sample: its high byte is sample >> 2.
        uint8_t high[4096];
        for (int x = 0; x < r->width && x < 4096; x++)
            high[x] = line[2 * x + 1];
        crc = switch_crc32(crc, high, (size_t)r->width);
    }
    CVPixelBufferUnlockBaseAddress(image, kCVPixelBufferLock_ReadOnly);
    const int index = (int)pts.value;
    if (index >= 0 && index < r->frames && crc == r->crcs[index]) {
        r->match++;
    } else if (r->first < 0) {
        r->first = index;
        r->firstCrc = crc;
    }
    if (index < r->last)
        r->outOfOrder++;
    r->last = index;
    r->count++;
}

// H.264: Annex-B NAL units -> the parameter sets and 4-byte length-prefixed access units (a slice with
// first_mb_in_slice 0 starts a picture).
static int splitH264(const uint8_t *sets[2], size_t setSizes[2], uint8_t *buffer, uint8_t *samples[], size_t sizes[],
                     int maxSamples)
{
    const uint8_t *nal[256];
    size_t length[256];
    int n = 0;
    for (size_t i = 0; i + 3 <= sizeof(kClip) && n < 256;) {
        if (kClip[i] == 0 && kClip[i + 1] == 0 && kClip[i + 2] == 1) {
            if (n) {
                size_t e = i;
                while (e && !kClip[e - 1])
                    e--;
                length[n - 1] = (size_t)(kClip + e - nal[n - 1]);
            }
            nal[n++] = kClip + i + 3;
            i += 3;
        } else {
            i++;
        }
    }
    if (!n)
        return 0;
    length[n - 1] = (size_t)(kClip + sizeof(kClip) - nal[n - 1]);
    int count = 0;
    size_t used = 0;
    for (int i = 0; i < n; i++) {
        const int type = nal[i][0] & 0x1f;
        if (type == 7 || type == 8) {
            sets[type - 7] = nal[i], setSizes[type - 7] = length[i];
            continue;
        }
        if (type != 1 && type != 5)
            continue;
        if (nal[i][1] & 0x80) {  // first_mb_in_slice == 0
            if (count == maxSamples)
                break;
            samples[count] = buffer + used;
            sizes[count++] = 0;
        }
        if (!count)
            continue;
        uint8_t *p = buffer + used;
        p[0] = (uint8_t)(length[i] >> 24), p[1] = (uint8_t)(length[i] >> 16), p[2] = (uint8_t)(length[i] >> 8);
        p[3] = (uint8_t)length[i];
        memcpy(p + 4, nal[i], length[i]);
        used += length[i] + 4;
        sizes[count - 1] += length[i] + 4;
    }
    return count;
}

// VP9: VP9Clip.h's packets (frames and superframes), each shown at its own index; vpcC for profile 0, level 3.0,
// 8-bit 4:2:0 (colocated), studio range, BT.709.
static CMVideoFormatDescriptionRef vp9Format(void)
{
    static const uint8_t kVpcC[12] = {1, 0, 0, 0, 0, 30, (8 << 4) | (1 << 1), 1, 1, 1, 0, 0};
    CFDataRef vpcC = CFDataCreate(NULL, kVpcC, sizeof(kVpcC));
    const void *atomKeys[] = {CFSTR("vpcC")}, *atomValues[] = {vpcC};
    CFDictionaryRef atoms = CFDictionaryCreate(NULL, atomKeys, atomValues, 1, &kCFTypeDictionaryKeyCallBacks,
                                               &kCFTypeDictionaryValueCallBacks);
    const void *keys[] = {kCMFormatDescriptionExtension_SampleDescriptionExtensionAtoms}, *values[] = {atoms};
    CFDictionaryRef extensions =
        CFDictionaryCreate(NULL, keys, values, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CMVideoFormatDescriptionRef format = NULL;
    CMVideoFormatDescriptionCreate(NULL, kCMVideoCodecType_VP9, kVP9ClipWidth, kVP9ClipHeight, extensions, &format);
    CFRelease(extensions), CFRelease(atoms), CFRelease(vpcC);
    return format;
}

// HEVC: an Annex-B clip's access units as 4-byte length-prefixed samples without their parameter sets, which go to
// the format description (hvcC) instead.
static int splitHEVC(const HEVCClip *clip, const uint8_t *sets[3], size_t setSizes[3], uint8_t *buffer,
                     uint8_t *samples[], size_t sizes[], int maxSamples)
{
    int count = 0;
    size_t used = 0;
    const uint8_t *nal = NULL;
    for (size_t i = 0; i + 3 <= clip->size || nal; i++) {
        const bool start = i + 3 <= clip->size && !clip->data[i] && !clip->data[i + 1] && clip->data[i + 2] == 1;
        if (!start && i < clip->size)
            continue;
        if (nal) {
            size_t e = start ? i : clip->size;
            while (start && e && !clip->data[e - 1])
                e--;
            const size_t length = (size_t)(clip->data + e - nal);
            const int type = (nal[0] >> 1) & 0x3f;
            if (type >= 32 && type <= 34) {
                if (!sets[type - 32])
                    sets[type - 32] = nal, setSizes[type - 32] = length;
            } else if (type < 32 || type == 39) {
                if (type < 32 && (nal[2] & 0x80)) {  // first_slice_segment_in_pic_flag
                    if (count == maxSamples)
                        return count;
                    samples[count] = buffer + used;
                    sizes[count++] = 0;
                }
                if (count) {
                    uint8_t *p = buffer + used;
                    p[0] = (uint8_t)(length >> 24), p[1] = (uint8_t)(length >> 16), p[2] = (uint8_t)(length >> 8);
                    p[3] = (uint8_t)length;
                    memcpy(p + 4, nal, length);
                    used += length + 4;
                    sizes[count - 1] += length + 4;
                }
            }
        }
        if (i >= clip->size)
            break;
        nal = clip->data + i + 3;
        i += 2;
    }
    return count;
}

// One HEVC clip through a session; appends "A 20/20" or the first mismatch to `parts`. True when every frame matched
// and (hardware) the session said hardware.
static bool hevcSession(const HEVCClip *clip, CFDictionaryRef spec, bool hardware, char *parts, size_t partsSize,
                        bool *onHardware, double *ms)
{
    static uint8_t buffer[sizeof(kHEVCClipAData) + 4096];
    uint8_t *samples[64];
    size_t sizes[64];
    const uint8_t *sets[3] = {NULL, NULL, NULL};
    size_t setSizes[3] = {0, 0, 0};
    const int count = splitHEVC(clip, sets, setSizes, buffer, samples, sizes, 64);
    CMVideoFormatDescriptionRef format = NULL;
    if (count && sets[0] && sets[1] && sets[2])
        CMVideoFormatDescriptionCreateFromHEVCParameterSets(NULL, 3, sets, setSizes, 4, NULL, &format);
    char part[160];
    if (!format) {
        snprintf(part, sizeof(part), "%s%c: no format description", *parts ? ", " : "", clip->name);
        strlcat(parts, part, partsSize);
        return false;
    }
    Result result = {.last = -1,
                     .first = -1,
                     .width = clip->width,
                     .height = clip->height,
                     .frames = clip->shown,
                     .crcs = clip->crc};
    int32_t pixelFormat = clip->depth == 10 ? kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange
                                            : kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
    CFNumberRef pf = CFNumberCreate(NULL, kCFNumberSInt32Type, &pixelFormat);
    const void *attrKeys[] = {kCVPixelBufferPixelFormatTypeKey}, *attrValues[] = {pf};
    CFDictionaryRef attrs = CFDictionaryCreate(NULL, attrKeys, attrValues, 1, &kCFTypeDictionaryKeyCallBacks,
                                               &kCFTypeDictionaryValueCallBacks);
    VTDecompressionOutputCallbackRecord callback = {output, &result};
    VTDecompressionSessionRef session = NULL;
    OSStatus status = VTDecompressionSessionCreate(NULL, format, spec, attrs, &callback, &session);
    CFRelease(attrs), CFRelease(pf);
    if (status) {
        CFRelease(format);
        snprintf(part, sizeof(part), "%s%c: VTDecompressionSessionCreate failed %d", *parts ? ", " : "", clip->name,
                 (int)status);
        strlcat(parts, part, partsSize);
        return false;
    }
    CFBooleanRef usingHardware = NULL;
    VTSessionCopyProperty(session, kVTDecompressionPropertyKey_UsingHardwareAcceleratedVideoDecoder, NULL,
                          &usingHardware);
    *onHardware = usingHardware == kCFBooleanTrue;
    if (usingHardware)
        CFRelease(usingHardware);
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int failed = 0;
    for (int i = 0; i < count; i++) {
        CMBlockBufferRef block = NULL;
        CMSampleBufferRef sample = NULL;
        CMBlockBufferCreateWithMemoryBlock(NULL, samples[i], sizes[i], kCFAllocatorNull, NULL, 0, sizes[i], 0, &block);
        const int64_t shown = i < clip->aus && clip->display[i] >= 0 ? clip->display[i] : i;
        CMSampleTimingInfo timing = {CMTimeMake(1, 30), CMTimeMake(shown, 30), CMTimeMake(i, 30)};
        CMSampleBufferCreateReady(NULL, block, format, 1, 1, &timing, 1, &sizes[i], &sample);
        VTDecodeInfoFlags info = 0;
        if (VTDecompressionSessionDecodeFrame(session, sample, kVTDecodeFrame_EnableTemporalProcessing, NULL, &info))
            failed++;
        CFRelease(sample);
        CFRelease(block);
    }
    VTDecompressionSessionFinishDelayedFrames(session);
    VTDecompressionSessionWaitForAsynchronousFrames(session);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    VTDecompressionSessionInvalidate(session);
    CFRelease(session);
    CFRelease(format);
    *ms += (double)(t1.tv_sec - t0.tv_sec) * 1e3 + (double)(t1.tv_nsec - t0.tv_nsec) / 1e6;
    const bool ok = result.match == result.frames && result.count == result.frames && !failed && !result.errors &&
                    (!hardware || *onHardware);
    snprintf(part, sizeof(part), "%s%c %d/%d", *parts ? ", " : "", clip->name, result.match, result.frames);
    strlcat(parts, part, partsSize);
    if (result.first >= 0) {
        snprintf(part, sizeof(part), " (first mismatch frame %d luma CRC %#010x, expected %#010x)", result.first,
                 result.firstCrc, result.first < result.frames ? result.crcs[result.first] : 0);
        strlcat(parts, part, partsSize);
    }
    if (failed || result.errors || result.count != result.frames) {
        snprintf(part, sizeof(part), " (%d out, %d decode errors, %d callback errors)", result.count, failed,
                 result.errors);
        strlcat(parts, part, partsSize);
    }
    return ok;
}

static bool clipCheckHEVC(ClipCheckMode mode, CFDictionaryRef extraSpec, char *line, size_t size)
{
    const bool hardware = mode != ClipCheckHEVCSoftware, tenBit = mode == ClipCheckHEVC10;
    CFMutableDictionaryRef spec = extraSpec ? CFDictionaryCreateMutableCopy(NULL, 0, extraSpec)
                                            : CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
                                                                        &kCFTypeDictionaryValueCallBacks);
    if (hardware && !CFDictionaryContainsKey(spec, kVTVideoDecoderSpecification_DecoderID))
        CFDictionarySetValue(spec, kVTVideoDecoderSpecification_RequireHardwareAcceleratedVideoDecoder, kCFBooleanTrue);
    else if (!hardware)
        CFDictionarySetValue(spec, kVTVideoDecoderSpecification_EnableHardwareAcceleratedVideoDecoder, kCFBooleanFalse);
    char parts[384] = "";
    bool ok = true, onHardware = true;
    double ms = 0;
    for (int i = tenBit ? 2 : 0; i < (tenBit ? 3 : 2); i++) {
        bool hw = false;
        ok &= hevcSession(&kHEVCClips[i], spec, hardware, parts, sizeof(parts), &hw, &ms);
        onHardware &= hw;
    }
    CFRelease(spec);
    snprintf(line, size, "%s: %s decoder, frames matching FFmpeg%s: %s, %.1f ms %s",
             tenBit     ? "hevc10"
             : hardware ? "hevc"
                        : "hevc-software",
             onHardware ? "hardware" : "software", tenBit ? " (>> 2)" : "", parts, ms, ok ? "OK" : "FAIL");
    return ok;
}

bool clip_check(ClipCheckMode mode, CFDictionaryRef extraSpec, char *line, size_t size)
{
    if (mode == ClipCheckHEVC || mode == ClipCheckHEVC10 || mode == ClipCheckHEVCSoftware)
        return clipCheckHEVC(mode, extraSpec, line, size);
    const bool hardware = mode != ClipCheckSoftware, vp9 = mode == ClipCheckVP9;
    const char *name = vp9 ? "vp9" : hardware ? "hardware" : "software";
    static uint8_t buffer[sizeof(kClip) + 4096];
    uint8_t *samples[64];
    size_t sizes[64];
    int count = 0;
    CMVideoFormatDescriptionRef format = NULL;
    Result result = {.last = -1, .first = -1};
    if (vp9) {
        size_t at = 0;
        for (int i = 0; i < kVP9ClipPackets && i < 64; i++) {
            samples[count] = (uint8_t *)kVP9ClipData + at;
            sizes[count++] = kVP9ClipSizes[i];
            at += kVP9ClipSizes[i];
        }
        format = vp9Format();
        result.width = kVP9ClipWidth, result.height = kVP9ClipHeight, result.frames = kVP9ClipShown;
        result.crcs = kVP9ClipLumaCrc;
    } else {
        const uint8_t *sets[2] = {NULL, NULL};
        size_t setSizes[2] = {0, 0};
        count = splitH264(sets, setSizes, buffer, samples, sizes, 64);
        if (count && sets[0] && sets[1])
            CMVideoFormatDescriptionCreateFromH264ParameterSets(NULL, 2, sets, setSizes, 4, &format);
        result.width = kClipWidth, result.height = kClipHeight, result.frames = kClipFrames;
        result.crcs = kClipLumaCrc;
    }
    if (!count || !format) {
        snprintf(line, size, "%s: could not build the clip's format description FAIL", name);
        if (format)
            CFRelease(format);
        return false;
    }

    CFMutableDictionaryRef spec = extraSpec ? CFDictionaryCreateMutableCopy(NULL, 0, extraSpec)
                                            : CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
                                                                        &kCFTypeDictionaryValueCallBacks);
    // A named decoder (AVD's private check registration is not flagged hardware) is taken as named; the session must
    // still report hardware below.
    const bool named = CFDictionaryContainsKey(spec, kVTVideoDecoderSpecification_DecoderID);
    if (hardware && !named)
        CFDictionarySetValue(spec, kVTVideoDecoderSpecification_RequireHardwareAcceleratedVideoDecoder, kCFBooleanTrue);
    else if (!hardware)
        CFDictionarySetValue(spec, kVTVideoDecoderSpecification_EnableHardwareAcceleratedVideoDecoder, kCFBooleanFalse);
    if (vp9)
        CFDictionarySetValue(spec, kVTVideoDecoderSpecification_AllowVP9Decode, kCFBooleanTrue);
    int32_t pixelFormat = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
    CFNumberRef pf = CFNumberCreate(NULL, kCFNumberSInt32Type, &pixelFormat);
    const void *attrKeys[] = {kCVPixelBufferPixelFormatTypeKey};
    const void *attrValues[] = {pf};
    CFDictionaryRef attrs = CFDictionaryCreate(NULL, attrKeys, attrValues, 1, &kCFTypeDictionaryKeyCallBacks,
                                               &kCFTypeDictionaryValueCallBacks);
    VTDecompressionOutputCallbackRecord callback = {output, &result};
    VTDecompressionSessionRef session = NULL;
    OSStatus status = VTDecompressionSessionCreate(NULL, format, spec, attrs, &callback, &session);
    CFRelease(spec), CFRelease(attrs), CFRelease(pf);
    if (status) {
        CFRelease(format);
        snprintf(line, size, "%s: VTDecompressionSessionCreate failed %d%s FAIL", name, (int)status,
                 hardware ? " (VideoToolbox found no hardware decoder for it, here or in the codec process it remotes "
                            "the session to)"
                          : "");
        return false;
    }
    CFBooleanRef usingHardware = NULL;
    VTSessionCopyProperty(session, kVTDecompressionPropertyKey_UsingHardwareAcceleratedVideoDecoder, NULL,
                          &usingHardware);
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int failed = 0;
    for (int i = 0; i < count; i++) {
        CMBlockBufferRef block = NULL;
        CMSampleBufferRef sample = NULL;
        CMBlockBufferCreateWithMemoryBlock(NULL, samples[i], sizes[i], kCFAllocatorNull, NULL, 0, sizes[i], 0, &block);
        const int64_t shown = vp9 ? i : i < kClipFrames ? kClipDisplay[i] : i;
        CMSampleTimingInfo timing = {CMTimeMake(1, 30), CMTimeMake(shown, 30), CMTimeMake(i, 30)};
        CMSampleBufferCreateReady(NULL, block, format, 1, 1, &timing, 1, &sizes[i], &sample);
        VTDecodeInfoFlags info = 0;
        if (VTDecompressionSessionDecodeFrame(session, sample, kVTDecodeFrame_EnableTemporalProcessing, NULL, &info))
            failed++;
        CFRelease(sample);
        CFRelease(block);
    }
    VTDecompressionSessionFinishDelayedFrames(session);
    VTDecompressionSessionWaitForAsynchronousFrames(session);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    VTDecompressionSessionInvalidate(session);
    CFRelease(session);
    CFRelease(format);
    const double ms = (double)(t1.tv_sec - t0.tv_sec) * 1e3 + (double)(t1.tv_nsec - t0.tv_nsec) / 1e6;
    const bool onHardware = usingHardware == kCFBooleanTrue;
    if (usingHardware)
        CFRelease(usingHardware);
    const bool ok = result.match == result.frames && result.count == result.frames && (!hardware || onHardware);
    char mismatch[96] = "";
    if (result.first >= 0)
        snprintf(mismatch, sizeof(mismatch), "; first mismatch frame %d luma CRC %#010x, expected %#010x", result.first,
                 result.firstCrc, result.first < result.frames ? result.crcs[result.first] : 0);
    snprintf(line, size,
             "%s: %s decoder, %d/%d frames match %s, %d out (%d before an earlier frame), %d decode errors, "
             "%d callback errors, %.1f ms%s %s",
             name, onHardware ? "hardware" : "software", result.match, result.frames, vp9 ? "libvpx" : "x264",
             result.count, result.outOfOrder, failed, result.errors, ms, mismatch, ok ? "OK" : "FAIL");
    return ok;
}
