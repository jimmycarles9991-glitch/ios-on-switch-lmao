// SPDX-License-Identifier: BSD-2-Clause-Patent
// See ProbeCheck.h.
#include "ProbeCheck.h"
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <VideoToolbox/VideoToolbox.h>
#include <stdio.h>
#include <string.h>

enum {
    W = 320,
    H = 240,
};

static const char *const kLayouts[] = {"tiled16", "pitch", "bl2", "bl4", "bl8", "bl16", "vic_bl2", "vic_bl16"};

typedef struct {
    CMSampleBufferRef sample;
    int errors;
} Encoded;

typedef struct {
    CVPixelBufferRef source;
    bool exact;
    int frames;
} Decoded;

static void encoded(void *refcon, void *frame, OSStatus status, VTEncodeInfoFlags flags, CMSampleBufferRef sample)
{
    (void)frame, (void)flags;
    Encoded *e = refcon;
    if (status || !sample || e->sample) {
        e->errors++;
        return;
    }
    e->sample = (CMSampleBufferRef)CFRetain(sample);
}

static bool samePlane(CVPixelBufferRef a, CVPixelBufferRef b, size_t plane, size_t rows)
{
    const uint8_t *pa = CVPixelBufferGetBaseAddressOfPlane(a, plane),
                  *pb = CVPixelBufferGetBaseAddressOfPlane(b, plane);
    const size_t sa = CVPixelBufferGetBytesPerRowOfPlane(a, plane), sb = CVPixelBufferGetBytesPerRowOfPlane(b, plane);
    for (size_t y = 0; y < rows; y++)
        if (memcmp(pa + y * sa, pb + y * sb, W))
            return false;
    return true;
}

static void decoded(void *refcon, void *frame, OSStatus status, VTDecodeInfoFlags flags, CVImageBufferRef image,
                    CMTime pts, CMTime duration)
{
    (void)frame, (void)flags, (void)pts, (void)duration;
    Decoded *d = refcon;
    d->frames++;
    if (status || !image || CVPixelBufferGetWidth(image) != W || CVPixelBufferGetHeight(image) != H)
        return;
    CVPixelBufferLockBaseAddress(image, kCVPixelBufferLock_ReadOnly);
    CVPixelBufferLockBaseAddress(d->source, kCVPixelBufferLock_ReadOnly);
    d->exact = samePlane(image, d->source, 0, H) && samePlane(image, d->source, 1, H / 2);
    CVPixelBufferUnlockBaseAddress(d->source, kCVPixelBufferLock_ReadOnly);
    CVPixelBufferUnlockBaseAddress(image, kCVPixelBufferLock_ReadOnly);
}

// The probe's AVCC sample for one layout, decoded on its own with the stream's format description.
static bool decodesExactly(CFDataRef data, CMFormatDescriptionRef format, CVPixelBufferRef source)
{
    CMBlockBufferRef block = NULL;
    CMSampleBufferRef sample = NULL;
    const size_t size = (size_t)CFDataGetLength(data);
    if (CMBlockBufferCreateWithMemoryBlock(NULL, NULL, size, NULL, NULL, 0, size, kCMBlockBufferAssureMemoryNowFlag,
                                           &block) ||
        CMBlockBufferReplaceDataBytes(CFDataGetBytePtr(data), block, 0, size) ||
        CMSampleBufferCreateReady(NULL, block, format, 1, 0, NULL, 1, &size, &sample)) {
        if (block)
            CFRelease(block);
        return false;
    }
    CFRelease(block);
    Decoded d = {.source = source};
    VTDecompressionOutputCallbackRecord callback = {decoded, &d};
    VTDecompressionSessionRef decoder = NULL;
    if (!VTDecompressionSessionCreate(NULL, format, NULL, NULL, &callback, &decoder)) {
        VTDecompressionSessionDecodeFrame(decoder, sample, 0, NULL, NULL);
        VTDecompressionSessionWaitForAsynchronousFrames(decoder);
        VTDecompressionSessionInvalidate(decoder);
        CFRelease(decoder);
    }
    CFRelease(sample);
    return d.frames == 1 && d.exact;
}

static void fill(CVPixelBufferRef image)
{
    CVPixelBufferLockBaseAddress(image, 0);
    uint8_t *y = CVPixelBufferGetBaseAddressOfPlane(image, 0), *c = CVPixelBufferGetBaseAddressOfPlane(image, 1);
    const size_t ys = CVPixelBufferGetBytesPerRowOfPlane(image, 0), cs = CVPixelBufferGetBytesPerRowOfPlane(image, 1);
    for (int row = 0; row < H; row++)
        for (int x = 0; x < W; x++)
            y[row * ys + x] = (uint8_t)(16 + ((x * 3 + row * 5) % 220));
    for (int row = 0; row < H / 2; row++)
        for (int x = 0; x < W; x++)
            c[row * cs + x] = (uint8_t)(x & 1 ? 64 + row : 200 - x / 4);
    CVPixelBufferUnlockBaseAddress(image, 0);
}

bool probe_check(CFDictionaryRef spec, char *line, size_t size)
{
    Encoded enc = {0};
    VTCompressionSessionRef session = NULL;
    if (VTCompressionSessionCreate(NULL, W, H, kCMVideoCodecType_H264, spec, NULL, NULL, encoded, &enc, &session)) {
        snprintf(line, size, "FAIL VTCompressionSessionCreate");
        return false;
    }
    VTSessionSetProperty(session, kVTCompressionPropertyKey_H264EntropyMode, kVTH264EntropyMode_CAVLC);
    VTSessionSetProperty(session, kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse);
    CVPixelBufferRef image = NULL;
    CVPixelBufferPoolRef pool = VTCompressionSessionGetPixelBufferPool(session);
    if (pool)
        CVPixelBufferPoolCreatePixelBuffer(NULL, pool, &image);
    if (image) {
        fill(image);
        VTCompressionSessionEncodeFrame(session, image, CMTimeMake(0, 30), CMTimeMake(1, 30), NULL, NULL, NULL);
        VTCompressionSessionCompleteFrames(session, kCMTimeInvalid);
    }
    CFDictionaryRef probe = NULL;
    VTSessionCopyProperty(session, CFSTR("SwitchEncoderLayoutProbe"), NULL, &probe);
    VTCompressionSessionInvalidate(session);
    CFRelease(session);
    int exact = 0, entries = 0;
    char failed[160] = "";
    const CMFormatDescriptionRef format = enc.sample ? CMSampleBufferGetFormatDescription(enc.sample) : NULL;
    const int layouts = (int)(sizeof(kLayouts) / sizeof(kLayouts[0]));
    for (int i = 0; probe && format && i < layouts; i++) {
        CFStringRef name = CFStringCreateWithCString(NULL, kLayouts[i], kCFStringEncodingUTF8);
        CFTypeRef value = CFDictionaryGetValue(probe, name);
        CFRelease(name);
        entries += value != NULL;
        if (value && CFGetTypeID(value) == CFDataGetTypeID() && decodesExactly(value, format, image))
            exact++;
        else
            snprintf(failed + strlen(failed), sizeof(failed) - strlen(failed), " %s", kLayouts[i]);
    }
    const bool ok = probe && entries == layouts && exact == layouts;
    snprintf(line, size, "%s layout probe: %d of %d layouts, %d decode back exactly%s%s", ok ? "ok" : "FAIL", entries,
             layouts, exact, failed[0] ? "; not:" : "", failed);
    if (probe)
        CFRelease(probe);
    if (enc.sample)
        CFRelease(enc.sample);
    CVPixelBufferRelease(image);
    return ok;
}
