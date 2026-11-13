// SPDX-License-Identifier: BSD-2-Clause-Patent
// See ReorderCheck.h.
#include "ReorderCheck.h"
#include <CoreMedia/CoreMedia.h>
#include <VideoToolbox/VideoToolbox.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "EncodeCheck.h"
#include "../../SwitchVideoDecoders/AVD/H264Syntax.h"

enum {
    FRAMES = 30,
    KEY_INTERVAL = 10,
    FORCED_KEY = 15
};

typedef struct {
    CMSampleBufferRef samples[FRAMES];  // in the order they came out
    int count, errors;
} Emitted;

typedef struct {
    int order[FRAMES], count, errors, inexact;
} Displayed;

static void encoded(void *refcon, void *frameRefcon, OSStatus status, VTEncodeInfoFlags flags, CMSampleBufferRef sample)
{
    (void)frameRefcon, (void)flags;
    Emitted *e = refcon;
    if (status || !sample || e->count == FRAMES) {
        e->errors++;
        return;
    }
    e->samples[e->count++] = (CMSampleBufferRef)CFRetain(sample);
}

static void displayed(void *refcon, void *frameRefcon, OSStatus status, VTDecodeInfoFlags flags, CVImageBufferRef image,
                      CMTime pts, CMTime duration)
{
    (void)frameRefcon, (void)flags, (void)duration;
    Displayed *d = refcon;
    const int i = (int)pts.value;
    if (status || !image || i < 0 || i >= FRAMES || d->count == FRAMES) {
        d->errors++;
        return;
    }
    double psnr[3];
    encode_check_psnr(image, i, psnr);
    d->inexact += psnr[0] < 99 || psnr[1] < 99 || psnr[2] < 99;
    d->order[d->count++] = i;
}

// Display order -> the order AVEReorder.h codes in, and each frame's type ('I', 'P', 'B'): key frames at 0, every
// KEY_INTERVAL after the last and at FORCED_KEY; between keys, groups of `b` B pictures then their anchor, the frames
// left before a key or the end coded with the last of them as the anchor.
static void expected(int b, int decode[FRAMES], char type[FRAMES])
{
    int n = 0, key = 0;
    for (int i = 0; i < FRAMES;) {
        decode[n++] = i;
        type[i] = 'I';
        key = i;
        int end = i + 1;
        while (end < FRAMES && end != FORCED_KEY && end - key < KEY_INTERVAL)
            end++;
        for (int first = i + 1; first < end; first += b + 1) {
            const int anchor = first + b < end ? first + b : end - 1;
            decode[n++] = anchor;
            type[anchor] = 'P';
            for (int f = first; f < anchor; f++) {
                decode[n++] = f;
                type[f] = 'B';
            }
        }
        i = end;
    }
}

static CFBooleanRef attachment(CMSampleBufferRef sample, CFStringRef key)
{
    CFArrayRef a = CMSampleBufferGetSampleAttachmentsArray(sample, false);
    return a && CFArrayGetCount(a) ? CFDictionaryGetValue(CFArrayGetValueAtIndex(a, 0), key) : NULL;
}

// The type the sample's slice and attachments agree on, or '?'.
static char sampleType(const H264ParamSets *sets, CMSampleBufferRef sample)
{
    CMBlockBufferRef block = CMSampleBufferGetDataBuffer(sample);
    const size_t size = block ? CMBlockBufferGetDataLength(block) : 0;
    uint8_t *bytes = size > 4 ? malloc(size) : NULL;
    if (!bytes || CMBlockBufferCopyDataBytes(block, 0, size, bytes)) {
        free(bytes);
        return '?';
    }
    const size_t length = (size_t)bytes[0] << 24 | (size_t)bytes[1] << 16 | (size_t)bytes[2] << 8 | bytes[3];
    H264Slice slice;
    const bool parsed = length <= size - 4 && !h264_parse_slice(sets, bytes + 4, length, &slice);
    free(bytes);
    if (!parsed)
        return '?';
    const bool notSync = attachment(sample, kCMSampleAttachmentKey_NotSync) == kCFBooleanTrue;
    const bool depended = attachment(sample, kCMSampleAttachmentKey_IsDependedOnByOthers) == kCFBooleanTrue;
    if (slice.nal_unit_type == H264_NAL_IDR && slice.slice_type == H264_SLICE_I && slice.nal_ref_idc && !notSync)
        return 'I';
    if (slice.slice_type == H264_SLICE_P && slice.nal_ref_idc && notSync && depended)
        return 'P';
    if (slice.slice_type == H264_SLICE_B && !slice.nal_ref_idc && notSync && !depended)
        return 'B';
    if (slice.nal_unit_type == H264_NAL_SLICE && slice.slice_type == H264_SLICE_I && !slice.nal_ref_idc && notSync &&
        !depended)
        return 'i';  // an I picture nothing predicts from
    return '?';
}

static H264ParamSets *parameterSets(CMFormatDescriptionRef format)
{
    H264ParamSets *sets = calloc(1, sizeof(*sets));
    for (size_t i = 0; sets && i < 2; i++) {
        const uint8_t *nal = NULL;
        size_t size = 0;
        uint8_t id = 0;
        if (CMVideoFormatDescriptionGetH264ParameterSetAtIndex(format, i, &nal, &size, NULL, NULL) ||
            (i ? h264_parse_pps(sets, nal, size, &id) : h264_parse_sps(sets, nal, size, &id))) {
            free(sets);
            return NULL;
        }
    }
    return sets;
}

static void setNumber(VTCompressionSessionRef s, CFStringRef key, double value)
{
    CFNumberRef n = CFNumberCreate(NULL, kCFNumberDoubleType, &value);
    VTSessionSetProperty(s, key, n);
    CFRelease(n);
}

static void decodeBack(const Emitted *e, Displayed *d)
{
    CMFormatDescriptionRef format = CMSampleBufferGetFormatDescription(e->samples[0]);
    VTDecompressionOutputCallbackRecord callback = {displayed, d};
    VTDecompressionSessionRef decoder = NULL;
    if (VTDecompressionSessionCreate(NULL, format, NULL, NULL, &callback, &decoder)) {
        d->errors++;
        return;
    }
    for (int i = 0; i < e->count; i++)
        VTDecompressionSessionDecodeFrame(decoder, e->samples[i], kVTDecodeFrame_EnableTemporalProcessing, NULL, NULL);
    VTDecompressionSessionFinishDelayedFrames(decoder);
    VTDecompressionSessionWaitForAsynchronousFrames(decoder);
    VTDecompressionSessionInvalidate(decoder);
    CFRelease(decoder);
}

static VTCompressionSessionRef makeSession(CFDictionaryRef spec, int allowReordering, int maxFrameDelay, Emitted *e)
{
    VTCompressionSessionRef session = NULL;
    if (VTCompressionSessionCreate(NULL, 320, 240, kCMVideoCodecType_H264, spec, NULL, NULL, encoded, e, &session))
        return NULL;
    VTSessionSetProperty(session, kVTCompressionPropertyKey_RealTime, kCFBooleanFalse);
    VTSessionSetProperty(session, kVTCompressionPropertyKey_ProfileLevel, kVTProfileLevel_H264_High_AutoLevel);
    VTSessionSetProperty(session, kVTCompressionPropertyKey_H264EntropyMode, kVTH264EntropyMode_CAVLC);
    if (allowReordering >= 0)
        VTSessionSetProperty(session, kVTCompressionPropertyKey_AllowFrameReordering,
                             allowReordering ? kCFBooleanTrue : kCFBooleanFalse);
    if (maxFrameDelay >= 0)
        setNumber(session, kVTCompressionPropertyKey_MaxFrameDelayCount, maxFrameDelay);
    setNumber(session, kVTCompressionPropertyKey_MaxKeyFrameInterval, KEY_INTERVAL);
    setNumber(session, kVTCompressionPropertyKey_ExpectedFrameRate, 30);
    setNumber(session, kVTCompressionPropertyKey_Quality, 0.8);
    return session;
}

// The clip through a session made as said: false when there was no session, else the frames that failed to go in.
static bool encodeClip(CFDictionaryRef spec, int allowReordering, int maxFrameDelay, Emitted *e, int *failed)
{
    VTCompressionSessionRef session = makeSession(spec, allowReordering, maxFrameDelay, e);
    if (!session)
        return false;
    for (int i = 0; i < FRAMES; i++) {
        CVPixelBufferRef image = encode_check_frame(i);
        CFDictionaryRef options = NULL;
        if (i == FORCED_KEY) {
            const void *k[] = {kVTEncodeFrameOptionKey_ForceKeyFrame}, *v[] = {kCFBooleanTrue};
            options =
                CFDictionaryCreate(NULL, k, v, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        }
        *failed += !image || VTCompressionSessionEncodeFrame(session, image, CMTimeMake(i, 30), CMTimeMake(1, 30),
                                                             options, NULL, NULL);
        if (options)
            CFRelease(options);
        CVPixelBufferRelease(image);
    }
    VTCompressionSessionCompleteFrames(session, kCMTimeInvalid);
    VTCompressionSessionInvalidate(session);
    CFRelease(session);
    return true;
}

bool reorder_refusal_check(CFDictionaryRef spec, char *line, size_t size)
{
    Emitted e = {0};
    int failed = 0;
    if (!encodeClip(spec, -1, -1, &e, &failed)) {
        snprintf(line, size, "FAIL VTCompressionSessionCreate");
        return false;
    }
    H264ParamSets *sets = e.count ? parameterSets(CMSampleBufferGetFormatDescription(e.samples[0])) : NULL;
    int bCount = 0, iCount = 0, unknown = 0, badTiming = 0;
    CMTime lastDts = kCMTimeNegativeInfinity, lastPts = kCMTimeNegativeInfinity;
    for (int n = 0; n < e.count; n++) {
        const char t = sets ? sampleType(sets, e.samples[n]) : '?';
        bCount += t == 'B';
        iCount += t == 'i';
        unknown += t == '?';
        const CMTime dts = CMSampleBufferGetDecodeTimeStamp(e.samples[n]);
        const CMTime pts = CMSampleBufferGetPresentationTimeStamp(e.samples[n]);
        badTiming += CMTimeCompare(dts, lastDts) <= 0 || (n > 3 && CMTimeCompare(pts, lastPts) <= 0);
        lastDts = dts;
        lastPts = pts;
    }
    free(sets);
    Displayed d = {0};
    if (e.count)
        decodeBack(&e, &d);
    for (int n = 0; n < e.count; n++)
        CFRelease(e.samples[n]);
    const bool ok = e.count == FRAMES && !e.errors && !failed && !bCount && iCount == 2 && !unknown && !badTiming &&
                    d.count == e.count && !d.errors && !d.inexact;
    snprintf(line, size,
             "%s %d out, %d dropped (%d failed to go in), %d B pictures, %d unreferenced I pictures, %d unreadable, %d "
             "with bad times; decoded %d (%d errors, %d inexact)",
             ok ? "ok" : "FAIL", e.count, e.errors, failed, bCount, iCount, unknown, badTiming, d.count, d.errors,
             d.inexact);
    return ok;
}

bool reorder_check(CFDictionaryRef spec, int allowReordering, int maxFrameDelay, int bPictures, char *line, size_t size)
{
    Emitted e = {0};
    int failed = 0;
    if (!encodeClip(spec, allowReordering, maxFrameDelay, &e, &failed)) {
        snprintf(line, size, "FAIL VTCompressionSessionCreate");
        return false;
    }

    int decode[FRAMES];
    char type[FRAMES];
    expected(bPictures, decode, type);
    int wrongOrder = 0, wrongType = 0, badTiming = 0, bCount = 0, reordered = 0;
    H264ParamSets *sets = e.count ? parameterSets(CMSampleBufferGetFormatDescription(e.samples[0])) : NULL;
    CMTime lastDts = kCMTimeNegativeInfinity;
    for (int n = 0; n < e.count; n++) {
        const CMTime pts = CMSampleBufferGetPresentationTimeStamp(e.samples[n]);
        const CMTime dts = CMSampleBufferGetDecodeTimeStamp(e.samples[n]);
        const int i = (int)pts.value;
        wrongOrder += i != decode[n];
        const char t = sets ? sampleType(sets, e.samples[n]) : '?';
        wrongType += i < 0 || i >= FRAMES || t != type[i];
        bCount += t == 'B';
        badTiming += CMTimeCompare(dts, CMTimeMake(n, 30)) != 0 || CMTimeCompare(dts, lastDts) <= 0;
        reordered += CMTimeCompare(dts, pts) != 0;
        lastDts = dts;
    }
    free(sets);
    Displayed d = {0};
    if (e.count == FRAMES)
        decodeBack(&e, &d);
    bool seen[FRAMES] = {false};
    int missing = FRAMES;
    for (int n = 0; n < d.count; n++)
        if (!seen[d.order[n]]) {
            seen[d.order[n]] = true;
            missing--;
        }
    for (int n = 0; n < e.count; n++)
        CFRelease(e.samples[n]);
    const bool ok = e.count == FRAMES && !e.errors && !failed && !wrongOrder && !wrongType && !badTiming &&
                    (reordered > 0) == (bPictures > 0) && d.count == FRAMES && !d.errors && !d.inexact && !missing;
    snprintf(line, size,
             "%s %d/%d out (%d errors), %d B pictures, %d out of decode order, %d of the wrong type, %d with bad "
             "decode times, %d decoded at another time than shown; decoded %d (%d errors, %d inexact, %d missing)",
             ok ? "ok" : "FAIL", e.count, FRAMES, e.errors + failed, bCount, wrongOrder, wrongType, badTiming,
             reordered, d.count, d.errors, d.inexact, missing);
    return ok;
}
