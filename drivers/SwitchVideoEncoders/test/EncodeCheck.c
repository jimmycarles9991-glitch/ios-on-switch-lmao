// SPDX-License-Identifier: BSD-2-Clause-Patent
// See EncodeCheck.h.
#include "EncodeCheck.h"
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <VideoToolbox/VideoToolbox.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

// 21H461 is iOS 17.7: the hardware-encoder keys the SDK marks iOS 17.4 are there (weak imports at the 17.0 target).
#pragma clang diagnostic ignored "-Wunguarded-availability-new"

enum {
    W = 320,
    H = 240,
    FRAMES = 30,
    KEY_INTERVAL = 10,
    FORCED_KEY = 15
};

typedef struct {
    CMSampleBufferRef samples[FRAMES];
    int count, errors;
} Encoded;

typedef struct {
    double minPsnr[3], sumPsnr[3];
    int count, errors, badIndex;
} Decoded;

// Frame i of the clip, screen-like: a still diagonal gradient, a coloured square moving down-right over it, and a fine
// checker texture in one corner that inverts every picture. Enough edges and motion that a wrong layout or reference
// shows at once, and still areas a P picture can skip.
static bool inSquare(int i, int x, int y)
{
    const int sx = 40 + 6 * i, sy = 30 + 3 * i;
    return x >= sx && x < sx + 48 && y >= sy && y < sy + 48;
}

static uint8_t lumaAt(int i, int x, int y)
{
    int v = (x + y / 2) & 0xff;
    if (inSquare(i, x, y))
        v = 235;
    if (x >= 240 && y >= 160)
        v = ((x / 2 + y / 2 + i) & 1) ? 200 : 60;
    return (uint8_t)(v < 16 ? 16 : v > 235 ? 235 : v);
}

static void chromaAt(int i, int x, int y, uint8_t *u, uint8_t *v)  // x, y in chroma samples
{
    const bool square = inSquare(i, 2 * x, 2 * y);
    *u = (uint8_t)(square ? 90 : 64 + (x & 127));
    *v = (uint8_t)(square ? 240 : 64 + ((y * 2) & 127));
}

bool encode_check_client_buffers;

// From the session's pool, as a client should take them (buffers laid out as the encoder asked), or CoreVideo's own.
static CVPixelBufferRef makeFrame(CVPixelBufferPoolRef pool, int i)
{
    CVPixelBufferRef image = NULL;
    if (pool) {
        CVPixelBufferPoolCreatePixelBuffer(NULL, pool, &image);
    } else {
        const void *keys[] = {kCVPixelBufferIOSurfacePropertiesKey};
        CFDictionaryRef empty =
            CFDictionaryCreate(NULL, NULL, NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        const void *values[] = {empty};
        CFDictionaryRef attrs =
            CFDictionaryCreate(NULL, keys, values, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        CVPixelBufferCreate(NULL, W, H, kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, attrs, &image);
        CFRelease(attrs);
        CFRelease(empty);
    }
    if (!image)
        return NULL;
    CVPixelBufferLockBaseAddress(image, 0);
    uint8_t *y = CVPixelBufferGetBaseAddressOfPlane(image, 0), *c = CVPixelBufferGetBaseAddressOfPlane(image, 1);
    const size_t ys = CVPixelBufferGetBytesPerRowOfPlane(image, 0), cs = CVPixelBufferGetBytesPerRowOfPlane(image, 1);
    for (int row = 0; row < H; row++)
        for (int x = 0; x < W; x++)
            y[(size_t)row * ys + x] = lumaAt(i, x, row);
    for (int row = 0; row < H / 2; row++)
        for (int x = 0; x < W / 2; x++)
            chromaAt(i, x, row, &c[(size_t)row * cs + 2 * x], &c[(size_t)row * cs + 2 * x + 1]);
    CVPixelBufferUnlockBaseAddress(image, 0);
    return image;
}

CVPixelBufferRef encode_check_frame(int i)
{
    return makeFrame(NULL, i);
}

static void encoded(void *refcon, void *frameRefcon, OSStatus status, VTEncodeInfoFlags flags, CMSampleBufferRef sample)
{
    (void)flags;
    Encoded *e = refcon;
    const int index = (int)(intptr_t)frameRefcon;
    if (status || !sample || index < 0 || index >= FRAMES || e->samples[index]) {
        e->errors++;
        return;
    }
    e->samples[index] = (CMSampleBufferRef)CFRetain(sample);
    e->count++;
}

static double psnr(double sse, double n)
{
    return sse > 0 ? 10 * log10(255.0 * 255.0 * n / sse) : 99;
}

void encode_check_psnr(CVImageBufferRef image, int i, double out[3])
{
    CVPixelBufferLockBaseAddress(image, kCVPixelBufferLock_ReadOnly);
    const uint8_t *y = CVPixelBufferGetBaseAddressOfPlane(image, 0), *c = CVPixelBufferGetBaseAddressOfPlane(image, 1);
    const size_t ys = CVPixelBufferGetBytesPerRowOfPlane(image, 0), cs = CVPixelBufferGetBytesPerRowOfPlane(image, 1);
    double sse[3] = {0, 0, 0};
    for (int row = 0; row < H; row++)
        for (int x = 0; x < W; x++) {
            const double e = (double)y[(size_t)row * ys + x] - lumaAt(i, x, row);
            sse[0] += e * e;
        }
    for (int row = 0; row < H / 2; row++)
        for (int x = 0; x < W / 2; x++) {
            uint8_t u, v;
            chromaAt(i, x, row, &u, &v);
            const double eu = (double)c[(size_t)row * cs + 2 * x] - u, ev = (double)c[(size_t)row * cs + 2 * x + 1] - v;
            sse[1] += eu * eu;
            sse[2] += ev * ev;
        }
    CVPixelBufferUnlockBaseAddress(image, kCVPixelBufferLock_ReadOnly);
    for (int p = 0; p < 3; p++)
        out[p] = psnr(sse[p], p ? W * H / 4.0 : (double)W * H);
}

static void decoded(void *refcon, void *frameRefcon, OSStatus status, VTDecodeInfoFlags flags, CVImageBufferRef image,
                    CMTime pts, CMTime duration)
{
    (void)frameRefcon, (void)flags, (void)duration;
    Decoded *d = refcon;
    const int i = (int)pts.value;
    if (status || !image || i < 0 || i >= FRAMES || CVPixelBufferGetPlaneCount(image) != 2) {
        d->errors++;
        return;
    }
    double planes[3];
    encode_check_psnr(image, i, planes);
    for (int p = 0; p < 3; p++) {
        const double q = planes[p];
        if (!d->count || q < d->minPsnr[p])
            d->minPsnr[p] = q;
        d->sumPsnr[p] += q;
        if (d->badIndex < 0 && q < 20)
            d->badIndex = i;
    }
    d->count++;
}

static void setNumber(VTCompressionSessionRef s, CFStringRef key, double value)
{
    CFNumberRef n = CFNumberCreate(NULL, kCFNumberDoubleType, &value);
    VTSessionSetProperty(s, key, n);
    CFRelease(n);
}

// A SwitchEncoderRoutes count (debug builds of the AVE plug-in), -1 from any other encoder.
static int32_t routeCount(CFDictionaryRef routes, CFStringRef key)
{
    int32_t v = -1;
    CFTypeRef n = routes && CFGetTypeID(routes) == CFDictionaryGetTypeID() ? CFDictionaryGetValue(routes, key) : NULL;
    if (n && CFGetTypeID(n) == CFNumberGetTypeID())
        CFNumberGetValue(n, kCFNumberSInt32Type, &v);
    return v;
}

static bool isSync(CMSampleBufferRef sample)
{
    CFArrayRef a = CMSampleBufferGetSampleAttachmentsArray(sample, false);
    if (!a || !CFArrayGetCount(a))
        return true;
    CFBooleanRef notSync = CFDictionaryGetValue(CFArrayGetValueAtIndex(a, 0), kCMSampleAttachmentKey_NotSync);
    return !notSync || !CFBooleanGetValue(notSync);
}

bool encode_check(CFDictionaryRef spec, CFStringRef entropy, double minPsnr, char *line, size_t size)
{
    CFDictionaryRef ownSpec = NULL;
    if (!spec) {
        const void *k[] = {kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder};
        const void *v[] = {kCFBooleanTrue};
        spec = ownSpec =
            CFDictionaryCreate(NULL, k, v, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    }
    Encoded enc = {0};
    VTCompressionSessionRef session = NULL;
    OSStatus status =
        VTCompressionSessionCreate(NULL, W, H, kCMVideoCodecType_H264, spec, NULL, NULL, encoded, &enc, &session);
    if (ownSpec)
        CFRelease(ownSpec);
    if (status) {
        snprintf(line, size, "FAIL VTCompressionSessionCreate %d (no hardware H.264 encoder in this process)",
                 (int)status);
        return false;
    }
    VTSessionSetProperty(session, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);
    VTSessionSetProperty(session, kVTCompressionPropertyKey_ProfileLevel, kVTProfileLevel_H264_High_AutoLevel);
    if (entropy)
        VTSessionSetProperty(session, kVTCompressionPropertyKey_H264EntropyMode, entropy);
    setNumber(session, kVTCompressionPropertyKey_MaxKeyFrameInterval, KEY_INTERVAL);
    setNumber(session, kVTCompressionPropertyKey_ExpectedFrameRate, 30);
    setNumber(session, kVTCompressionPropertyKey_Quality, 0.8);
    CFBooleanRef hardware = NULL;
    VTSessionCopyProperty(session, kVTCompressionPropertyKey_UsingHardwareAcceleratedVideoEncoder, NULL, &hardware);
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int failed = 0;
    CVPixelBufferPoolRef pool = encode_check_client_buffers ? NULL : VTCompressionSessionGetPixelBufferPool(session);
    for (int i = 0; i < FRAMES; i++) {
        CVPixelBufferRef image = makeFrame(pool, i);
        if (!image) {
            failed++;
            continue;
        }
        CFDictionaryRef options = NULL;
        if (i == FORCED_KEY) {
            const void *k[] = {kVTEncodeFrameOptionKey_ForceKeyFrame}, *v[] = {kCFBooleanTrue};
            options =
                CFDictionaryCreate(NULL, k, v, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        }
        if (VTCompressionSessionEncodeFrame(session, image, CMTimeMake(i, 30), CMTimeMake(1, 30), options,
                                            (void *)(intptr_t)i, NULL))
            failed++;
        if (options)
            CFRelease(options);
        CVPixelBufferRelease(image);
    }
    VTCompressionSessionCompleteFrames(session, kCMTimeInvalid);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    CFDictionaryRef routes = NULL;
    VTSessionCopyProperty(session, CFSTR("SwitchEncoderRoutes"), NULL, &routes);
    const int32_t byVIC = routeCount(routes, CFSTR("input_vic")),
                  copied = routeCount(routes, CFSTR("input_cpu_copies")),
                  refused = routeCount(routes, CFSTR("input_refused")),
                  bounced = routeCount(routes, CFSTR("input_bounced"));
    if (routes)
        CFRelease(routes);
    VTCompressionSessionInvalidate(session);
    CFRelease(session);
    const double ms = ((double)(t1.tv_sec - t0.tv_sec) * 1e3 + (double)(t1.tv_nsec - t0.tv_nsec) / 1e6) / FRAMES;
    const bool onHardware = hardware == kCFBooleanTrue;
    if (hardware)
        CFRelease(hardware);

    // Sync samples: every 10th picture and the forced one, or every picture when the encoder runs intra only.
    char keys[96] = "";
    size_t bytes = 0;
    int syncs = 0, misplaced = 0;
    for (int i = 0; i < enc.count && i < FRAMES; i++) {
        if (!enc.samples[i])
            continue;
        bytes += CMSampleBufferGetTotalSampleSize(enc.samples[i]);
        const bool sync = isSync(enc.samples[i]);
        const bool expected = i == 0 || i == 10 || i == FORCED_KEY || i == 25;
        if (sync) {
            syncs++;
            if (strlen(keys) < sizeof(keys) - 4)
                snprintf(keys + strlen(keys), sizeof(keys) - strlen(keys), " %d", i);
        }
        misplaced += sync != expected;
    }
    const bool intraOnly = syncs == FRAMES;
    Decoded dec = {.badIndex = -1};
    if (enc.count == FRAMES && enc.samples[0]) {
        CMFormatDescriptionRef format = CMSampleBufferGetFormatDescription(enc.samples[0]);
        int32_t pixelFormat = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
        CFNumberRef pf = CFNumberCreate(NULL, kCFNumberSInt32Type, &pixelFormat);
        const void *k[] = {kCVPixelBufferPixelFormatTypeKey}, *v[] = {pf};
        CFDictionaryRef attrs =
            CFDictionaryCreate(NULL, k, v, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        VTDecompressionOutputCallbackRecord callback = {decoded, &dec};
        VTDecompressionSessionRef decoder = NULL;
        if (!VTDecompressionSessionCreate(NULL, format, NULL, attrs, &callback, &decoder)) {
            for (int i = 0; i < FRAMES; i++)
                VTDecompressionSessionDecodeFrame(decoder, enc.samples[i], 0, NULL, NULL);
            VTDecompressionSessionWaitForAsynchronousFrames(decoder);
            VTDecompressionSessionInvalidate(decoder);
            CFRelease(decoder);
        }
        CFRelease(attrs);
        CFRelease(pf);
    }
    for (int i = 0; i < FRAMES; i++)
        if (enc.samples[i])
            CFRelease(enc.samples[i]);
    const bool ok = enc.count == FRAMES && !enc.errors && !failed && onHardware && (!misplaced || intraOnly) &&
                    dec.count == FRAMES && !dec.errors && dec.minPsnr[0] >= minPsnr && dec.minPsnr[1] >= minPsnr &&
                    dec.minPsnr[2] >= minPsnr;
    snprintf(
        line, size,
        "%s %d/%d frames encoded (%d errors) by a %s encoder, %d decoded (%d errors), sync samples%s%s, PSNR min Y "
        "%.1f U %.1f V %.1f dB, mean Y %.1f dB, first frame under 20 dB %d, %zu bytes, %.2f ms/frame, inputs %d by "
        "VIC %d bounced %d copied %d refused",
        ok ? "ok" : "FAIL", enc.count, FRAMES, enc.errors + failed, onHardware ? "hardware" : "software", dec.count,
        dec.errors, keys, intraOnly ? " (intra only)" : "", dec.minPsnr[0], dec.minPsnr[1], dec.minPsnr[2],
        dec.count ? dec.sumPsnr[0] / dec.count : 0.0, dec.badIndex, bytes, ms, byVIC, bounced, copied, refused);
    return ok;
}
