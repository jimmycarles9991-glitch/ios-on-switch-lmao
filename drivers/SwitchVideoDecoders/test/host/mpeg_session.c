// SPDX-License-Identifier: BSD-2-Clause-Patent
// avd_host's MPEG sessions through VideoToolbox: a clip of test/MPEG4Clip.h described as an MP4 file describes it (an
// 'esds' atom whose DecoderSpecificInfo is the clip's VOS, VO and VOL headers), every VOP a sample; a clip of
// test/MPEG2Clip.h as MediaToolbox's MPEG-2 stream readers describe it (no extensions: the sequence header is in the
// first sample), every picture a sample.
#include "mpeg_session.h"
#include <CoreMedia/CoreMedia.h>
#include <VideoToolbox/VideoToolbox.h>
#include <stdio.h>
#include <string.h>
#include "../MPEG2Clip.h"
#include "../MPEG4Clip.h"

typedef struct {
    int out, errors, misordered;
    intptr_t last;
} Count;

static void output(void *refcon, void *frameRefcon, OSStatus status, VTDecodeInfoFlags flags, CVImageBufferRef image,
                   CMTime pts, CMTime duration)
{
    (void)flags, (void)pts, (void)duration;
    Count *c = refcon;
    if (status || !image)
        c->errors++;
    else
        c->out++;
    c->misordered += (intptr_t)frameRefcon <= c->last;
    c->last = (intptr_t)frameRefcon;
}

typedef struct {
    VTDecodeFrameFlags flags;
    const char *const *keys;
    int32_t *values;
    unsigned count;
} Options;

static void readRoutes(VTDecompressionSessionRef session, const Options *options)
{
    CFTypeRef routes = NULL;
    VTSessionCopyProperty(session, CFSTR("SwitchPictureRoutes"), NULL, &routes);
    for (unsigned i = 0; i < options->count; i++) {
        CFStringRef key = CFStringCreateWithCString(NULL, options->keys[i], kCFStringEncodingUTF8);
        CFTypeRef value =
            routes && CFGetTypeID(routes) == CFDictionaryGetTypeID() ? CFDictionaryGetValue(routes, key) : NULL;
        options->values[i] = -1;
        if (value && CFGetTypeID(value) == CFNumberGetTypeID())
            CFNumberGetValue(value, kCFNumberSInt32Type, &options->values[i]);
        CFRelease(key);
    }
    if (routes)
        CFRelease(routes);
}

static size_t descriptor(uint8_t *at, uint8_t tag, size_t length)
{
    const uint8_t header[5] = {tag, 0x80 | (uint8_t)(length >> 21 & 0x7f), 0x80 | (uint8_t)(length >> 14 & 0x7f),
                               0x80 | (uint8_t)(length >> 7 & 0x7f), (uint8_t)(length & 0x7f)};
    memcpy(at, header, 5);
    return 5;
}

// ISO/IEC 14496-1 ES_Descriptor for MPEG-4 Visual (objectTypeIndication 0x20), every length in the four-byte form.
static CFDataRef esds(const uint8_t *dsi, size_t size)
{
    uint8_t e[512] = {0};
    const size_t config = 13 + 5 + size, es = 3 + 5 + config + 5 + 1;
    size_t n = 4;  // version, flags
    n += descriptor(e + n, 0x03, es);
    n += 3;  // ES_ID, flags
    n += descriptor(e + n, 0x04, config);
    e[n++] = 0x20;
    e[n++] = 0x11;  // visual stream
    n += 11;        // bufferSizeDB, maxBitrate, avgBitrate
    n += descriptor(e + n, 0x05, size);
    memcpy(e + n, dsi, size);
    n += size;
    n += descriptor(e + n, 0x06, 1);
    e[n++] = 0x02;  // SLConfigDescriptor: predefined MP4
    return CFDataCreate(NULL, e, (CFIndex)n);
}

// Every sample of a clip through a session on `format`; one result line, true when every frame came out without an
// error from a session on (or off) hardware.
static bool run(CFDictionaryRef spec, CMVideoFormatDescriptionRef format, const char *name, const uint8_t *clip,
                const size_t *samples, int count, bool wantHardware, const Options *options, char *line, size_t size)
{
    Count out = {.last = -1};
    VTDecompressionOutputCallbackRecord callback = {output, &out};
    VTDecompressionSessionRef session = NULL;
    const OSStatus status = VTDecompressionSessionCreate(NULL, format, spec, NULL, &callback, &session);
    if (status) {
        snprintf(line, size, "%s: session failed %d", name, (int)status);
        return false;
    }
    CFBooleanRef hardware = NULL;
    VTSessionCopyProperty(session, kVTDecompressionPropertyKey_UsingHardwareAcceleratedVideoDecoder, NULL, &hardware);
    int refused = 0;
    for (int i = 0; i < count; i++) {
        const size_t offset = samples[i], bytes = samples[i + 1] - offset;
        CMBlockBufferRef block = NULL;
        CMSampleBufferRef sample = NULL;
        CMBlockBufferCreateWithMemoryBlock(NULL, (void *)(clip + offset), bytes, kCFAllocatorNull, NULL, 0, bytes, 0,
                                           &block);
        CMSampleBufferCreateReady(NULL, block, format, 1, 0, NULL, 1, &bytes, &sample);
        VTDecodeInfoFlags info = 0;
        if (VTDecompressionSessionDecodeFrame(session, sample, options ? options->flags : 0, (void *)(intptr_t)i,
                                              &info))
            refused++;
        CFRelease(sample);
        CFRelease(block);
    }
    VTDecompressionSessionWaitForAsynchronousFrames(session);
    if (options)
        readRoutes(session, options);
    const bool onHardware = hardware == kCFBooleanTrue;
    if (hardware)
        CFRelease(hardware);
    VTDecompressionSessionInvalidate(session);
    CFRelease(session);
    snprintf(line, size, "%s: %s decoder, %d of %d out, %d errors, %d refused, %d out of decode order", name,
             onHardware ? "hardware" : "software", out.out, count, out.errors, refused, out.misordered);
    return onHardware == wantHardware && out.out == count && !out.errors && !refused && !out.misordered;
}

bool mpeg4_session_check(CFDictionaryRef spec, bool simple, bool wantHardware, char *line, size_t size)
{
    const uint8_t *clip = simple ? kMPEG4ClipB : kMPEG4ClipA;
    CFDataRef atom = esds(clip, simple ? kMPEG4ClipBConfig : kMPEG4ClipAConfig);
    const void *atomKeys[] = {CFSTR("esds")}, *atomValues[] = {atom};
    CFDictionaryRef atoms = CFDictionaryCreate(NULL, atomKeys, atomValues, 1, &kCFTypeDictionaryKeyCallBacks,
                                               &kCFTypeDictionaryValueCallBacks);
    const void *extKeys[] = {kCMFormatDescriptionExtension_SampleDescriptionExtensionAtoms}, *extValues[] = {atoms};
    CFDictionaryRef extensions = CFDictionaryCreate(NULL, extKeys, extValues, 1, &kCFTypeDictionaryKeyCallBacks,
                                                    &kCFTypeDictionaryValueCallBacks);
    CMVideoFormatDescriptionRef format = NULL;
    const OSStatus status = CMVideoFormatDescriptionCreate(NULL, kCMVideoCodecType_MPEG4Video, simple ? 136 : 176,
                                                           simple ? 72 : 144, extensions, &format);
    CFRelease(extensions);
    CFRelease(atoms);
    CFRelease(atom);
    if (status) {
        snprintf(line, size, "mp4v: format failed %d", (int)status);
        return false;
    }
    const bool ok = run(spec, format, simple ? "mp4v B" : "mp4v A", clip, simple ? kMPEG4ClipBVOPs : kMPEG4ClipAVOPs,
                        simple ? 32 : 18, wantHardware, NULL, line, size);
    CFRelease(format);
    return ok;
}

static bool mpeg2Run(CFDictionaryRef spec, bool mpeg1, const Options *options, char *line, size_t size)
{
    CMVideoFormatDescriptionRef format = NULL;
    const OSStatus status = CMVideoFormatDescriptionCreate(
        NULL, mpeg1 ? kCMVideoCodecType_MPEG1Video : kCMVideoCodecType_MPEG2Video, 176, 144, NULL, &format);
    if (status) {
        snprintf(line, size, "mp2v: format failed %d", (int)status);
        return false;
    }
    // The first sample carries the sequence header, as a stream reader's first access unit does.
    size_t pictures[19];
    memcpy(pictures, mpeg1 ? kMPEG2ClipCPictures : kMPEG2ClipAPictures, sizeof(size_t) * (mpeg1 ? 16 : 19));
    pictures[0] = 0;
    const bool ok = run(spec, format, mpeg1 ? "mp1v C" : "mp2v A", mpeg1 ? kMPEG2ClipC : kMPEG2ClipA, pictures,
                        mpeg1 ? 15 : 18, true, options, line, size);
    CFRelease(format);
    return ok;
}

bool mpeg2_session_check(CFDictionaryRef spec, bool mpeg1, char *line, size_t size)
{
    return mpeg2Run(spec, mpeg1, NULL, line, size);
}

bool mpeg2_session_order(CFDictionaryRef spec, VTDecodeFrameFlags flags, const char *const *keys, int32_t *values,
                         unsigned count, char *line, size_t size)
{
    const Options options = {flags, keys, values, count};
    return mpeg2Run(spec, false, &options, line, size);
}
