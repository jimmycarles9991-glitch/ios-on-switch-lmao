// SPDX-License-Identifier: BSD-2-Clause-Patent
// How a picture reaches NVENC's input buffer. NVENC 5.0 reads 16x16 tiles or block linear, never the pitch rows an
// IOSurface holds (TegraNVENC/nvidia/SOURCE), so with TEGRA_NVENC_FEATURE_SURFACE_INPUT the session's input buffer is
// BL2 and every submit names the picture's IOSurface: the kernel has VIC write the region into the buffer and NVENC's
// job waits for VIC's on the hardware. BL2 is the block-linear layout both ends were seen with: NVIDIA's T210 driver
// hands NVENC its default 2-GOB surfaces (libnvtvmr 0x43a4, 0x2d4f0), and it is NVDEC's layout, the NV12 case of VIC's
// block-linear output probe (gpu.x1.vic_block_linear). The CPU touches no sample. A decoder's native picture (NVDEC's
// block-linear output) NVENC reads in place, with no VIC job at all (input_direct). Rows off the grid VIC reads
// (VideoToolbox is asked to keep to it; a client's own pool may not) go through the GPU's copy engine in the kernel
// first (input_bounced). A picture VIC cannot take at all (no IOSurface, a surface the kernel refuses) fails its frame
// with one line per reason and counts in input_refused: there is no CPU route beside VIC's. The CPU copy (16-byte runs
// into the buffer's layout) is a session's whole route only when the kernel offers no surface input at all, which its
// start line says.
#include <CoreVideo/CoreVideo.h>
#include <IOSurface/IOSurfaceRef.h>
#include <IOKit/IOReturn.h>
#include "AVEEncoder.h"
#include "../../../kits/SwitchRefusal/SwitchRefusal.h"

enum {
    kVICPlaneAlignment = 256,  // VIC addresses planes in 256-byte units
};

uint32_t ave_input_layout(const Encoder *e)
{
    return e->info.features & TEGRA_NVENC_FEATURE_SURFACE_INPUT ? TEGRA_NVENC_LAYOUT_BL2 : e->info.input_layout;
}

static void setNumber(CFMutableDictionaryRef dict, CFStringRef key, int32_t value)
{
    CFNumberRef number = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &value);
    CFDictionarySetValue(dict, key, number);
    CFRelease(number);
}

CFDictionaryRef ave_copy_input_attributes(const Encoder *e, int32_t width, int32_t height)
{
    const int32_t formats[] = {kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
                               kCVPixelFormatType_420YpCbCr8BiPlanarFullRange};
    CFNumberRef numbers[2];
    for (int i = 0; i < 2; i++)
        numbers[i] = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &formats[i]);
    CFArrayRef list = CFArrayCreate(kCFAllocatorDefault, (const void **)numbers, 2, &kCFTypeArrayCallBacks);
    CFDictionaryRef surface = CFDictionaryCreate(kCFAllocatorDefault, NULL, NULL, 0, &kCFTypeDictionaryKeyCallBacks,
                                                 &kCFTypeDictionaryValueCallBacks);
    CFMutableDictionaryRef attributes = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(attributes, kCVPixelBufferPixelFormatTypeKey, list);
    CFDictionarySetValue(attributes, kCVPixelBufferIOSurfacePropertiesKey, surface);
    if (width && height) {
        setNumber(attributes, kCVPixelBufferWidthKey, width);
        setNumber(attributes, kCVPixelBufferHeightKey, height);
    }
    if (e->info.features & TEGRA_NVENC_FEATURE_SURFACE_INPUT) {
        setNumber(attributes, kCVPixelBufferBytesPerRowAlignmentKey, (int32_t)e->info.surface_row_align);
        setNumber(attributes, kCVPixelBufferPlaneAlignmentKey, kVICPlaneAlignment);
    }
    CFRelease(surface);
    CFRelease(list);
    for (int i = 0; i < 2; i++)
        CFRelease(numbers[i]);
    return attributes;
}

static uint32_t codedWidth(const Encoder *e)
{
    return e->codec == kCMVideoCodecType_HEVC ? hevcenc_coded_width(&e->hevc) : h264enc_width_mbs(&e->config) * 16;
}

static uint32_t codedHeight(const Encoder *e)
{
    return e->codec == kCMVideoCodecType_HEVC ? hevcenc_coded_height(&e->hevc) : h264enc_height_mbs(&e->config) * 16;
}

static bool copyRegion(Encoder *e, CVPixelBufferRef image, const AVERegion *r, TegraNVENCSubmit *submit)
{
    if (CVPixelBufferLockBaseAddress(image, kCVPixelBufferLock_ReadOnly))
        return false;
    const size_t lumaStride = CVPixelBufferGetBytesPerRowOfPlane(image, 0);
    const size_t chromaStride = CVPixelBufferGetBytesPerRowOfPlane(image, 1);
    const uint8_t *luma = (const uint8_t *)CVPixelBufferGetBaseAddressOfPlane(image, 0) + r->y * lumaStride + r->x;
    const uint8_t *chroma =
        (const uint8_t *)CVPixelBufferGetBaseAddressOfPlane(image, 1) + r->y / 2 * chromaStride + r->x;
    h264enc_put_region(e->config.input_layout, &e->layout.input, e->inputVA, luma, lumaStride, chroma, chromaStride,
                       r->width, r->height, codedWidth(e), codedHeight(e));
    CVPixelBufferUnlockBaseAddress(image, kCVPixelBufferLock_ReadOnly);
    submit->source = (TegraNVENCSource){0};
    return true;
}

// The fill functions' first clean range is the whole input buffer, for the copy; VIC's input is no CPU write.
static void dropInputClean(const Encoder *e, TegraNVENCSubmit *submit)
{
    if (submit->clean_count && submit->clean[0].handle == e->handles.input) {
        submit->clean_count--;
        memmove(&submit->clean[0], &submit->clean[1], submit->clean_count * sizeof(submit->clean[0]));
    }
}

static uint32_t vicMatrix(uint8_t h273)
{
    return h273 == 9 ? 2 : h273 == 1 ? 1 : 0;
}

bool ave_input_put(Encoder *e, CVPixelBufferRef image, const AVERegion *r, TegraNVENCSubmit *submit)
{
    if (!e->surfaceInput)
        return copyRegion(e, image, r, submit);
    if (!CVPixelBufferGetIOSurface(image)) {
        e->inputRefused++;
        switch_refusal("AVE", "a picture without an IOSurface", "VIC cannot take it, its frame fails");
        return false;
    }
    dropInputClean(e, submit);
    submit->source = (TegraNVENCSource){
        .surface_id = IOSurfaceGetID(CVPixelBufferGetIOSurface(image)),
        .x = r->x,
        .y = r->y,
        .width = r->width,
        .height = r->height,
        .format = e->config.full_range ? kCVPixelFormatType_420YpCbCr8BiPlanarFullRange
                                       : kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
        .matrix = vicMatrix(e->config.matrix),
    };
    return true;
}

int ave_input_submit(Encoder *e, const TegraNVENCSubmit *submit, uint32_t *fence)
{
    uint32_t input = TEGRA_NVENC_INPUT_CPU;
    const int status = nvenc_submit(e->client, submit, fence, &input);
    if (!status) {
        e->inputCopies += input == TEGRA_NVENC_INPUT_CPU;
        e->inputVIC += input == TEGRA_NVENC_INPUT_VIC || input == TEGRA_NVENC_INPUT_BOUNCED;
        e->inputBounced += input == TEGRA_NVENC_INPUT_BOUNCED;
        e->inputDirect += input == TEGRA_NVENC_INPUT_DIRECT;
    } else if (submit->source.surface_id && (status == kIOReturnUnsupported || status == kIOReturnNotReady)) {
        e->inputRefused++;
        if (status == kIOReturnUnsupported)
            switch_refusal("AVE", "a surface VIC cannot take", "TegraNVENC's line says why; its frame fails");
        else
            switch_refusal("AVE", "a surface VIC could not take then", "TegraNVENC's line says why; its frame fails");
    }
    return status;
}
