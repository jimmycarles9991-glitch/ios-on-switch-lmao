// SPDX-License-Identifier: BSD-2-Clause-Patent
// VideoToolbox's tri-planar contract for HEVC with alpha (24B119): MuxedAlphaDecoder decodes the colour and the alpha
// layer in one session each, both on this decoder, and when both write straight into its 'v0a8' target (Y, CbCr, alpha)
// it skips its CPU merge. The contract as VideoToolbox 3385.14.1.3 and Apple's AVD.videodecoder 995 implement it:
// - alphadecoder_createSubLayerVTDecompressionSession (0x18f2cc028) puts SelectPixelFormatWithAlpha = true and
//   WriteDirectlyToPlanesOfTargetCVPixelBuffer = the layer's first plane (0 colour, 2 alpha) in the session options;
//   VTDecompressionSessionCreateWithOptions hands them to the decoder's setProperty in that order, before StartSession
//   (0x18f0044d0, 0x18f004548). Apple's HEVC decoder takes the plane as a CFNumber within 0..(alpha ? 2 : 1)
//   (AppleAVDWrapperHEVCDecoderSetProperty 0x22c9cfa08) and drops both if its stream cannot do it (StartSession
//   0x22c9f3618, "Disabling decodeToBufferWithAlpha").
// - After the start it reads the plane back (VTSessionCopyProperty, 0x18f2cc154; a key the decoder does not list is
//   never asked, VTDecompressionSessionCopyProperty 0x18edc0194); a session that answers its own plane writes directly.
//   The colour session's pool format picks the target's: 'v0a8' stays 'v0a8' (MuxedAlphaDecoder_StartSession
//   0x18f2cafd8), and the alpha session is then asked for 'v0a8' too.
// - With both answering, each frame's target comes from the wrapper's pool, remembered for the frame, and a layer
//   session's VTDecoderSessionCreatePixelBuffer for its own frame returns that same buffer (0x18f008628), which is
//   where Apple's BufferPool::getBuffer takes its output (0x22c9f28ec). The merge is then skipped (set at 0x18f2cb110,
//   tested at 0x18f0a41d0) and the target goes out as written.
// Here VIC writes the planes (TegraNVDECPresent.plane): the colour picture into planes 0 and 1, the alpha picture's
// luma alone into plane 2. 21H461's VideoToolbox (3115.7.1.101.6) has neither key and always merges on the CPU.
#include <CoreVideo/CoreVideo.h>
#include <libkern/OSCacheControl.h>
#include "AVDDecoder.h"
#include "BlockLinear.h"

#define kSelectPixelFormatWithAlpha CFSTR("SelectPixelFormatWithAlpha")
#define kWriteDirectlyToPlanes      CFSTR("WriteDirectlyToPlanesOfTargetCVPixelBuffer")

#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
static uint32_t gAlphaPlaneVIC, gAlphaPlaneCPU;  // every session of this process: alpha-plane pictures by route
#endif

bool avd_alpha_set_property(Decoder *d, CFStringRef key, CFTypeRef value)
{
    if (CFEqual(key, kSelectPixelFormatWithAlpha)) {
        d->alphaAsked = value && CFGetTypeID(value) == CFBooleanGetTypeID() && CFBooleanGetValue(value);
        return true;
    }
    if (!CFEqual(key, kWriteDirectlyToPlanes))
        return false;
    int32_t plane = -1;
    if (value && CFGetTypeID(value) == CFNumberGetTypeID())
        CFNumberGetValue(value, kCFNumberSInt32Type, &plane);
    d->planeAsked = plane;
    return true;
}

void avd_alpha_start(Decoder *d, bool eightBit420)
{
    d->triPlanar = d->alphaAsked && eightBit420;
    d->intoTarget = d->triPlanar && (d->planeAsked == 0 || d->planeAsked == 2);
    d->firstPlane = d->intoTarget ? (uint32_t)d->planeAsked : 0;
}

const char *avd_alpha_describe(const Decoder *d)
{
    return !d->triPlanar    ? ""
           : !d->intoTarget ? " into 'v0a8' buffers (VideoToolbox merges the layers)"
           : d->firstPlane  ? " into the alpha plane of VideoToolbox's 'v0a8' target"
                            : " into the colour planes of VideoToolbox's 'v0a8' target";
}

bool avd_alpha_copy_property(const Decoder *d, CFStringRef key, CFTypeRef *value)
{
    if (CFEqual(key, kSelectPixelFormatWithAlpha)) {
        *value = d->triPlanar ? CFRetain(kCFBooleanTrue) : NULL;
        return true;
    }
    if (!CFEqual(key, kWriteDirectlyToPlanes))
        return false;
    const int32_t plane = (int32_t)d->firstPlane;
    *value = d->intoTarget ? CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &plane) : NULL;
    return true;
}

static void listProperty(CFMutableDictionaryRef supported, CFStringRef key, CFStringRef type)
{
    const void *keys[] = {kVTPropertyTypeKey, kVTPropertyReadWriteStatusKey};
    const void *values[] = {type, kVTPropertyReadWriteStatus_ReadWrite};
    CFDictionaryRef description = CFDictionaryCreate(kCFAllocatorDefault, keys, values, 2,
                                                     &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(supported, key, description);
    CFRelease(description);
}

void avd_alpha_list_properties(CFMutableDictionaryRef supported)
{
    listProperty(supported, kSelectPixelFormatWithAlpha, kVTPropertyType_Boolean);
    listProperty(supported, kWriteDirectlyToPlanes, kVTPropertyType_Number);
}

void avd_alpha_copy_luma(const H264Picture *picture, CVPixelBufferRef image, uint32_t width, uint32_t height)
{
    const size_t pitch = CVPixelBufferGetBytesPerRowOfPlane(image, 2);
    uint8_t *alpha = CVPixelBufferGetBaseAddressOfPlane(image, 2);
    block_linear_copy(alpha, pitch, picture->luma, picture->pitch, picture->crop_x, picture->crop_y, width, height);
    sys_dcache_flush(alpha, pitch * height);
}

#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
void avd_alpha_count(const Decoder *d, bool byVIC)
{
    if (d->intoTarget && d->firstPlane == 2)
        __atomic_add_fetch(byVIC ? &gAlphaPlaneVIC : &gAlphaPlaneCPU, 1, __ATOMIC_RELAXED);
}

void avd_alpha_routes(const Decoder *d, int32_t counts[4])
{
    counts[0] = d->intoTarget ? (int32_t)d->firstPlane : -1;
    counts[1] = (int32_t)d->intoTargetCount;
    counts[2] = (int32_t)__atomic_load_n(&gAlphaPlaneVIC, __ATOMIC_RELAXED);
    counts[3] = (int32_t)__atomic_load_n(&gAlphaPlaneCPU, __ATOMIC_RELAXED);
}
#endif
