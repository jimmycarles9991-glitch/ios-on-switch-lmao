// SPDX-License-Identifier: BSD-2-Clause-Patent
// AVD.videodecoder: H.264, VP9 (profile 0), HEVC (Main, Main 10 in 'x420', Main Still Picture), MPEG-4 Part 2 and
// MPEG-1/2 (AVDMPEG.c) on the Tegra X1's NVDEC as VideoToolbox's hardware 'avc1', 'vp09', 'hvc1', 'mp4v', 'mp2v' and
// 'mp1v' decoders, registered by AVDRegister.c. Tile sessions (grid HEIC: wallpapers, full-size photos) are AVDTiles.c.
//
// Selection: _vtSortRegistryItemsByRating ranks a higher rating first, then hardware, so this decoder is tried before
// H264SW, and a factory that fails moves VideoToolbox to the next one.
// StartSession is not retried with another decoder, so a stream NVDEC cannot take (4:2:2 or 4:4:4, high bit depth,
// slice groups, beyond 4096x4096) or one NVDEC has no memory for runs on Apple's software decoder inside
// this instance: VTSelectAndCreateVideoDecoderInstance with hardware disabled picks it, every class call is forwarded,
// and UsingHardwareAcceleratedVideoDecoder answers false. VP9 has no software decoder in 21H461's VideoToolbox to fall
// back on (and needs kVTVideoDecoderSpecification_AllowVP9Decode, which VideoToolbox gives Fig servers and entitled
// tasks such as WebKit's), so a VP9 session this decoder cannot take (profile 1-3, beyond 4096x4096, below NVDEC's
// 144x144) fails in StartSession, which is also what clients see on iPads without VP9 hardware.
//
// Per picture (the scheduling): H264NVDEC parses and fills NVDEC's setup on the caller's decode thread and the kernel
// queues the job on host1x channel 0; with no wait, the picture's pixel buffer is backed by NVDEC's surface (native
// export) or VIC is queued to copy the block-linear picture into one from the session's pool, NV12 '420v'/'420f' (the
// format H264SW gives the same clients), through its SMMU window onto the IOSurface's own pages (TegraNVDECPresent,
// drivers/TegraVIC/TegraVICPresent.h): the kernel orders both after the job on NVDEC's syncpoint, and the decode thread
// returns. The job's completion (AVDEmitter.c) emits the picture with the status NVDEC wrote, from the session's
// completion queue, in decode order. The CPU neither de-swizzles nor touches the pixels. Where VIC does not take a
// picture (present off this boot, an odd size, a surface it cannot address: the first refusal is one line) the decode
// thread waits for the job, the CPU de-swizzles the picture and cleans the buffer's lines to DRAM (sys_dcache_flush).
// The CPU copy costs 2.47 ms a 720p60 H.264 picture, most of mediaplaybackd's CPU time.
// A synchronous DecodeFrame (kVTDecodeFrame_EnableAsynchronousDecompression clear: Apple's iOS decoders test it,
// see below) answers its frame before returning.
// Frames go out in decode order once decoded, and clients order them by presentation time. Apple's iOS
// HEVC decoder never reads kVTDecodeFrame_EnableTemporalProcessing (24B119 AVD.videodecoder's
// AppleAVDWrapperHEVCDecoderDecodeFrame, 0x22c9e67d0, tests flag bits 0 and 2 only). VideoToolbox's MuxedAlphaDecoder
// (HEVC with alpha: the AirPods cards) passes the client's flags to one session per layer and needs each layer's
// picture out with no later input: four frames in flight (dispatch_semaphore_create(4): 24B119 0x18f0a370c, 21H461
// 0x197b029d8), the next one failed after a 10 s wait (-12911), and a synchronous DecodeFrame waiting forever for both
// layers (24B119 0x18f2cbbe4, 21H461 0x197b03208). H264NVDEC's and HEVCNVDEC's display-order bumping stays (kReorder).
//
// Log (subsystem com.brunostjohn.switch.avd, category decoder): per process, a refusal by TegraNVDEC; per session, one line when
// it starts (on NVDEC, or on the software decoder and why) and one when it ends (frames, mean decode and copy time).
// Decode time is the decode thread's: parsing and submitting, not NVDEC's.
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOReturn.h>
#include <IOSurface/IOSurfaceRef.h>
#include <dlfcn.h>
#include <libkern/OSCacheControl.h>
#include <mach/mach_time.h>
#include <os/log.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include "BlockLinear.h"
#include "AVDDecoder.h"
#include "SwitchVideoColor.h"

os_log_t gAVDLog;
static const bool kReorder = false;  // see above

#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
#define kSwitchPictureRoutesKey      CFSTR("SwitchPictureRoutes")
#define kSwitchDisplayBufferProbeKey CFSTR("SwitchDisplayBufferProbe")
#endif

static Decoder *storage(CMBaseObjectRef object)
{
    return CMBaseObjectGetDerivedStorage(object);
}

static double ticksToMs(uint64_t ticks)
{
    static mach_timebase_info_data_t base;
    if (!base.denom)
        mach_timebase_info(&base);
    return (double)ticks * base.numer / base.denom / 1e6;
}

static const VTPluginDecoderVTable *softwareVTable(const Decoder *d)
{
    return CMBaseObjectGetVTable(VTVideoDecoderGetCMBaseObject(d->software));
}

static _Thread_local bool tSoftwareLookup;  // avd_start_software is choosing Apple's decoder on this thread

const char *avd_codec_name(FourCharCode codec)
{
    return codec == kCMVideoCodecType_HEVC         ? "HEVC"
           : codec == kCMVideoCodecType_VP9        ? "VP9"
           : codec == kCMVideoCodecType_MPEG4Video ? "MPEG-4 Part 2"
           : codec == kCMVideoCodecType_MPEG2Video ? "MPEG-2"
           : codec == kCMVideoCodecType_MPEG1Video ? "MPEG-1"
                                                   : "H.264";
}

OSStatus avd_start_software(Decoder *d, VTVideoDecoderSession session, CMVideoFormatDescriptionRef format,
                            const char *why)
{
    const void *keys[] = {kVTVideoDecoderSpecification_EnableHardwareAcceleratedVideoDecoder};
    const void *values[] = {kCFBooleanFalse};
    CFDictionaryRef spec =
        CFDictionaryCreate(NULL, keys, values, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    VTVideoDecoderRef software = NULL;
    tSoftwareLookup = true;  // this plug-in's own registrations decline: Apple's is wanted
    OSStatus status = VTSelectAndCreateVideoDecoderInstance(d->codec, kCFAllocatorDefault, spec, &software);
    tSoftwareLookup = false;
    CFRelease(spec);
    const VTPluginDecoderVTable *vtable =
        software ? CMBaseObjectGetVTable(VTVideoDecoderGetCMBaseObject(software)) : NULL;
    if (!status && (!vtable || !vtable->decoderClass || !vtable->decoderClass->startSession))
        status = kVTCouldNotFindVideoDecoderErr;
    if (!status)
        status = vtable->decoderClass->startSession(software, session, format);
    if (status) {
        if (software)
            CFRelease(software);
        os_log_error(gAVDLog, "AVD: %{public}s: no software %{public}s decoder either (%d)", why,
                     avd_codec_name(d->codec), (int)status);
        return status;
    }
    d->software = software;
    const CMVideoDimensions size = CMVideoFormatDescriptionGetDimensions(format);
    switch_refusal("AVD", why, "%dx%d %s session on Apple's software decoder", size.width, size.height,
                   avd_codec_name(d->codec));
    return noErr;
}

// VIC's copy of the picture into the buffer (TegraNVDECPresent). The kernel is always asked, even for a shape it will
// refuse (an odd VP9 size), because a
// refusal leaves the picture's range invalidated: this picture's Wait did not, and the CPU copy must read DRAM.
static bool presentPicture(Decoder *d, const H264Picture *picture, CVPixelBufferRef image, uint32_t x, uint32_t y,
                           uint32_t width, uint32_t height)
{
    IOSurfaceRef surface = CVPixelBufferGetIOSurface(image);
    const TegraNVDECPresent p = {.handle = picture->handle,
                                 .surface_id = surface ? IOSurfaceGetID(surface) : 0,
                                 .pitch = picture->pitch,
                                 .rows = picture->rows,
                                 .chroma_offset = picture->chroma_offset,
                                 .block_height_log2 = 1,  // GOB_2 (the decoders' gob_height 0)
                                 .crop_x = picture->crop_x,
                                 .crop_y = picture->crop_y,
                                 .width = width,
                                 .height = height,
                                 .dst_x = x,
                                 .dst_y = y,
                                 .sample_bytes = picture->wide ? 2 : 1,
                                 .plane = d->firstPlane};
    if (!x && !y && !d->triPlanar && width == CVPixelBufferGetWidth(image) && height == CVPixelBufferGetHeight(image) &&
        switch_nvdec_can_export(d->client) && !switch_nvdec_export(d->client, &p)) {
        d->exported++;
        return true;
    }
    const int err = nvdec_present(d->client, &p);
    if (!err)
        return true;
    if (err == kIOReturnUnsupported || err == kIOReturnBadArgument) {
        os_log_error(gAVDLog,
                     "AVD: VIC refused a %ux%u picture at %u,%u in %{public}s (%#x; the kernel line says why): this "
                     "picture copies on the CPU",
                     width, height, x, y, getprogname(), err);
    }
    return false;
}

// The CPU's copy reads what the picture's jobs wrote: it waits for them, the picture's range invalidated.
static bool cpuReady(Decoder *d, Output *out, const H264Picture *picture)
{
    const TegraNVDECRange range = {picture->handle, 0, picture->chroma_offset + picture->pitch * picture->rows / 2, 0};
    return !avd_emitter_wait_picture(d->emitter, out, range);
}

void avd_place_picture(Decoder *d, Output *out, const H264Picture *picture, CVPixelBufferRef image, uint32_t x,
                       uint32_t y)
{
    const uint64_t began = mach_absolute_time();
    const uint32_t w = (uint32_t)CVPixelBufferGetWidth(image), h = (uint32_t)CVPixelBufferGetHeight(image);
    const uint32_t roomW = x < w ? w - x : 0, roomH = y < h ? h - y : 0;
    const uint32_t width = roomW < picture->width ? roomW : picture->width;
    const uint32_t height = roomH < picture->height ? roomH : picture->height;
    const uint32_t bytes = picture->wide ? 2 : 1;
    const bool byVIC = d->present && presentPicture(d, picture, image, x, y, width, height);
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    avd_alpha_count(d, byVIC);
#endif
    if (byVIC) {
        d->presented++;
    } else if (width && height && !cpuReady(d, out, picture)) {
        avd_emitter_fail(d->emitter, out, kVTVideoDecoderMalfunctionErr);
    } else if (width && height && d->firstPlane == 2) {
        CVPixelBufferLockBaseAddress(image, 0);
        avd_alpha_copy_luma(picture, image, width, height);
        CVPixelBufferUnlockBaseAddress(image, 0);
        d->copied++;
    } else if (width && height) {
        CVPixelBufferLockBaseAddress(image, 0);
        const size_t lumaPitch = CVPixelBufferGetBytesPerRowOfPlane(image, 0);
        const size_t chromaPitch = CVPixelBufferGetBytesPerRowOfPlane(image, 1);
        uint8_t *luma = (uint8_t *)CVPixelBufferGetBaseAddressOfPlane(image, 0) + y * lumaPitch + x * bytes;
        uint8_t *chroma =
            (uint8_t *)CVPixelBufferGetBaseAddressOfPlane(image, 1) + y / 2 * chromaPitch + (x & ~1u) * bytes;
        block_linear_copy(luma, lumaPitch, picture->luma, picture->pitch, picture->crop_x * bytes, picture->crop_y,
                          width * bytes, height);
        block_linear_copy(chroma, chromaPitch, picture->chroma, picture->pitch, (picture->crop_x & ~1u) * bytes,
                          picture->crop_y / 2, (width & ~1u) * bytes, (height + 1) / 2);
        // Pool images without a registered device backing still need their CPU-written rows published to DRAM.
        sys_dcache_flush(luma, lumaPitch * height);
        sys_dcache_flush(chroma, chromaPitch * ((height + 1) / 2));
        CVPixelBufferUnlockBaseAddress(image, 0);
        d->copied++;
    }
    d->copyTicks += mach_absolute_time() - began;
}

static int switch_recycle_output(void *context, uint32_t handle, uint64_t bytes, void **address)
{
    return switch_nvdec_recycle(((Decoder *)context)->client, handle, bytes, address);
}

static void *decoded(void *ctx, const H264Picture *picture)
{
    Decoder *d = ctx;
    if (d->tileSession)
        return avd_tile_decoded(d, picture);
    Output *out = avd_emitter_output(d->emitter, picture->token, picture->job, picture->first_field_job);
    if (!out)
        return NULL;
    if (picture->refused)
        avd_emitter_fail(d->emitter, out, kVTVideoDecoderBadDataErr);
    CVPixelBufferPoolRef pool = d->session && !d->intoTarget ? VTDecoderSessionGetPixelBufferPool(d->session) : NULL;
    const bool made = d->intoTarget
                          ? d->session && !VTDecoderSessionCreatePixelBuffer(d->session, out->frame, &out->image)
                          : pool && !CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault, pool, &out->image);
    if (!made || !out->image) {
        avd_emitter_fail(d->emitter, out, kVTVideoDecoderMalfunctionErr);
        return out;
    }
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    d->intoTargetCount += d->intoTarget;
#endif
    if (d->firstPlane == 2) {  // the colour session tags the target both layers share
        avd_place_picture(d, out, picture, out->image, 0, 0);
        return out;
    }
    if (d->matrix)
        CVBufferSetAttachment(out->image, kCVImageBufferYCbCrMatrixKey, d->matrix, kCVAttachmentMode_ShouldPropagate);
    if (d->primaries)
        CVBufferSetAttachment(out->image, kCVImageBufferColorPrimariesKey, d->primaries,
                              kCVAttachmentMode_ShouldPropagate);
    if (d->transfer)
        CVBufferSetAttachment(out->image, kCVImageBufferTransferFunctionKey, d->transfer,
                              kCVAttachmentMode_ShouldPropagate);
    if (d->chromaTop)
        CVBufferSetAttachment(out->image, kCVImageBufferChromaLocationTopFieldKey, d->chromaTop,
                              kCVAttachmentMode_ShouldPropagate);
    if (d->chromaBottom)
        CVBufferSetAttachment(out->image, kCVImageBufferChromaLocationBottomFieldKey, d->chromaBottom,
                              kCVAttachmentMode_ShouldPropagate);
    if (picture->fields)
        avd_tag_fields(out->image, picture->fields);
    avd_place_picture(d, out, picture, out->image, 0, 0);
    return out;
}

static void emit(void *ctx, void *item)
{
    Decoder *d = ctx;
    Output *out = item;
    if (d->tileSession)
        avd_tile_emit(d, out);
    else
        avd_emitter_push(d->emitter, out);
}

// A frame answered without a picture of its own (parameter sets only, or its sample failed), in its turn.
static void answer(Decoder *d, VTVideoDecoderFrame frame, OSStatus status)
{
    Output *out = avd_emitter_output(d->emitter, frame, 0, 0);
    if (!out)
        return;
    out->status = status;
    avd_emitter_push(d->emitter, out);
}

static void discard(void *ctx, void *item)  // no_output_of_prior_pics: answered, without a picture
{
    Output *out = item;
    if (out->image)
        CVPixelBufferRelease(out->image);
    out->image = NULL;
    emit(ctx, out);
}

static int bufferAlloc(void *ctx, uint64_t size, uint32_t *handle, void **va)
{
    return nvdec_alloc(((Decoder *)ctx)->client, size, handle, va);
}

static void bufferFree(void *ctx, uint32_t handle)
{
    nvdec_free(((Decoder *)ctx)->client, handle);
}

static int jobSubmit(void *ctx, const TegraNVDECSubmit *submit, uint32_t *fence)
{
    Decoder *d = ctx;
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    avd_frame_lap(&d->timing, AVD_BEFORE_SUBMIT, mach_absolute_time());
#endif
    TegraNVDECFence reply = {0};
    const int error = nvdec_submit(d->client, submit, &reply);
    if (!error)
        avd_emitter_submitted(d->emitter, reply.fence, submit, reply.flags & SWITCH_NVDEC_FENCE_WAITED_FOR_ROOM);
    *fence = reply.fence;
    return error;
}

static int jobWait(void *ctx, const TegraNVDECWait *wait)
{
    Decoder *d = ctx;
    avd_emitter_dependency_wait(d->emitter);
    const int error = nvdec_wait(d->client, wait);
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    avd_frame_lap(&d->timing, AVD_SUBMIT_TO_FENCE, mach_absolute_time());
#endif
    return error;
}

static CFDataRef copyAvcC(CMVideoFormatDescriptionRef format)
{
    CFDictionaryRef atoms =
        CMFormatDescriptionGetExtension(format, kCMFormatDescriptionExtension_SampleDescriptionExtensionAtoms);
    CFTypeRef avcC =
        atoms && CFGetTypeID(atoms) == CFDictionaryGetTypeID() ? CFDictionaryGetValue(atoms, CFSTR("avcC")) : NULL;
    return avcC && CFGetTypeID(avcC) == CFDataGetTypeID() ? CFRetain(avcC) : NULL;
}

// ISO/IEC 14496-15 AVCDecoderConfigurationRecord: lengthSizeMinusOne, then the SPS and PPS lists.
static int loadAvcC(Decoder *d, CFDataRef avcC)
{
    const uint8_t *p = CFDataGetBytePtr(avcC);
    const size_t size = (size_t)CFDataGetLength(avcC);
    if (size < 7 || p[0] != 1)
        return -1;
    d->lengthSize = (p[4] & 3) + 1;
    size_t pos = 5;
    for (unsigned list = 0; list < 2; list++) {
        if (pos >= size)
            return -1;
        unsigned count = list ? p[pos] : p[pos] & 0x1f;
        pos++;
        while (count--) {
            if (pos + 2 > size)
                return -1;
            const size_t n = (size_t)p[pos] << 8 | p[pos + 1];
            pos += 2;
            if (pos + n > size || h264_decoder_parameter_set(d->h264, p + pos, n))
                return -1;
            pos += n;
        }
    }
    return 0;
}

void avd_set_number(CFMutableDictionaryRef dict, CFStringRef key, int32_t value)
{
    CFNumberRef n = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &value);
    CFDictionarySetValue(dict, key, n);
    CFRelease(n);
}

CFDataRef avd_copy_atom(CMVideoFormatDescriptionRef format, CFStringRef name)
{
    CFDictionaryRef atoms =
        CMFormatDescriptionGetExtension(format, kCMFormatDescriptionExtension_SampleDescriptionExtensionAtoms);
    CFTypeRef atom = atoms && CFGetTypeID(atoms) == CFDictionaryGetTypeID() ? CFDictionaryGetValue(atoms, name) : NULL;
    return atom && CFGetTypeID(atom) == CFDataGetTypeID() ? CFRetain(atom) : NULL;
}

// Rows and planes on 256-byte boundaries where VIC writes the buffers (its proven pitch, and the plane addresses it
// takes); 64 for the CPU copy.
static void setAlignment(const Decoder *d, CFMutableDictionaryRef attributes)
{
    avd_set_number(attributes, kCVPixelBufferBytesPerRowAlignmentKey, d->present ? 256 : 64);
    if (d->present)
        avd_set_number(attributes, kCVPixelBufferPlaneAlignmentKey, 256);
}

void avd_set_attributes(Decoder *d, VTVideoDecoderSession session, bool fullRange, bool tenBit, OSStatus *status)
{
    d->pixelFormat = d->triPlanar ? kCVPixelFormatType_420YpCbCr8VideoRange_8A_TriPlanar
                     : tenBit     ? (fullRange ? kCVPixelFormatType_420YpCbCr10BiPlanarFullRange
                                               : kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange)
                                  : (fullRange ? kCVPixelFormatType_420YpCbCr8BiPlanarFullRange
                                               : kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange);
    CFMutableDictionaryRef attributes = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionaryRef surface = CFDictionaryCreate(kCFAllocatorDefault, NULL, NULL, 0, &kCFTypeDictionaryKeyCallBacks,
                                                 &kCFTypeDictionaryValueCallBacks);
    avd_set_number(attributes, kCVPixelBufferPixelFormatTypeKey, (int32_t)d->pixelFormat);
    avd_set_number(attributes, kCVPixelBufferWidthKey, (int32_t)d->width);
    avd_set_number(attributes, kCVPixelBufferHeightKey, (int32_t)d->height);
    setAlignment(d, attributes);
    CFDictionarySetValue(attributes, kCVPixelBufferIOSurfacePropertiesKey, surface);
    *status = VTDecoderSessionSetPixelBufferAttributes(session, attributes);
    CFRelease(surface);
    CFRelease(attributes);
}

// VP9 (WebM's or ISO BMFF's 'vpcC', VP Codec ISO Media File Format Binding 2.3): version 1, flags, profile, level,
// bitDepth(4) chromaSubsampling(3) videoFullRangeFlag(1), colourPrimaries, transferCharacteristics,
// matrixCoefficients. Profile 0 is 8-bit 4:2:0; a format without vpcC is taken as profile 0 and checked per frame.
static OSStatus startVP9(Decoder *d, VTVideoDecoderSession session, CMVideoFormatDescriptionRef format)
{
    const CMVideoDimensions size = CMVideoFormatDescriptionGetDimensions(format);
    d->avcC = avd_copy_atom(format, CFSTR("vpcC"));
    const uint8_t *v = d->avcC && CFDataGetLength(d->avcC) >= 12 ? CFDataGetBytePtr(d->avcC) : NULL;
    const int profile = v ? v[4] : 0, depth = v ? v[6] >> 4 : 8, chroma = v ? (v[6] >> 1) & 7 : 1;
    const bool fullRange = v && (v[6] & 1);
    if (profile != 0 || depth != 8 || chroma > 1 || size.width < 144 || size.height < 144 || size.width > 4096 ||
        size.height > 4096) {
        os_log_error(gAVDLog,
                     "AVD: VP9 session %dx%d in %{public}s refused: profile %d, %d-bit, chroma %d (NVDEC takes "
                     "profile 0, 4:2:0 8-bit, 144x144 to 4096x4096; VideoToolbox has no software VP9 here)",
                     size.width, size.height, getprogname(), profile, depth, chroma);
        return kVTVideoDecoderUnsupportedDataFormatErr;
    }
    if (vp9_decoder_configure(d->vp9, (uint32_t)size.width, (uint32_t)size.height)) {
        os_log_error(gAVDLog, "AVD: VP9 session %dx%d in %{public}s: no NVDEC memory for it (see the AVD alloc line)",
                     size.width, size.height, getprogname());
        return kVTVideoDecoderMalfunctionErr;
    }
    d->width = (uint32_t)size.width;
    d->height = (uint32_t)size.height;
    d->matrix = switch_video_color_tag(format, kCMFormatDescriptionExtension_YCbCrMatrix, v ? v[9] : 2, 0, d->height);
    d->primaries = switch_video_color_tag(format, kCMFormatDescriptionExtension_ColorPrimaries, v ? v[7] : 2, 1, 0);
    d->transfer = switch_video_color_tag(format, kCMFormatDescriptionExtension_TransferFunction, v ? v[8] : 2, 2, 0);
    d->chromaTop = switch_video_chroma_tag(format, kCMFormatDescriptionExtension_ChromaLocationTopField, false, 0);
    d->chromaBottom =
        switch_video_chroma_tag(format, kCMFormatDescriptionExtension_ChromaLocationBottomField, false, 0);
    OSStatus status;
    avd_set_attributes(d, session, fullRange, false, &status);
    if (!status)
        os_log(gAVDLog,
               "AVD: session %ux%u in %{public}s on NVDEC (VP9 profile 0, level %d), pictures out by %{public}s",
               d->width, d->height, getprogname(), v ? v[5] : 0, d->present ? "VIC" : "the CPU");
    return status;
}

// HEVC ('hvcC', ISO/IEC 14496-15 8.3.3.1): 21 bytes of profile and stream facts (lengthSizeMinusOne in the low bits
// of byte 21), then arrays of VPS/SPS/PPS/SEI units. A stream NVDEC does not take (another profile or chroma format,
// Main 10 on a kernel without it, beyond 4096x4096 or below 144x144, no NVDEC memory) runs on Apple's software decoder
// inside this instance, as H.264 does.
int avd_load_hvcc(Decoder *d, CFDataRef hvcC)
{
    const uint8_t *p = CFDataGetBytePtr(hvcC);
    const size_t size = (size_t)CFDataGetLength(hvcC);
    if (size < 23 || p[0] != 1)
        return -1;
    d->lengthSize = (p[21] & 3) + 1;
    size_t pos = 23;
    for (unsigned arrays = p[22]; arrays--;) {
        if (pos + 3 > size)
            return -1;
        unsigned count = (unsigned)p[pos + 1] << 8 | p[pos + 2];
        pos += 3;
        while (count--) {
            if (pos + 2 > size)
                return -1;
            const size_t n = (size_t)p[pos] << 8 | p[pos + 1];
            pos += 2;
            if (pos + n > size || hevc_decoder_parameter_set(d->hevc, p + pos, n))
                return -1;
            pos += n;
        }
    }
    return hevc_decoder_sps(d->hevc) ? 0 : -1;
}

static bool eightBit420(CFTypeRef value)
{
    OSType format = 0;
    return value && CFGetTypeID(value) == CFNumberGetTypeID() &&
           CFNumberGetValue(value, kCFNumberSInt32Type, &format) &&
           (format == kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange ||
            format == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange);
}

// Whether the client asked VideoToolbox for 8-bit 4:2:0 only: then a Main 10 picture goes out as NVDEC's 8-bit display
// buffer instead of a 16-bit picture VideoToolbox would convert. Looked up at run time: the SPI is not in every SDK.
static bool clientWantsEightBit(VTVideoDecoderSession session)
{
    typedef CFDictionaryRef (*Attributes)(VTVideoDecoderSession);
    const Attributes attributes =
        (Attributes)dlsym(RTLD_DEFAULT, "VTDecoderSessionGetDestinationPixelBufferAttributes");
    CFDictionaryRef wanted = attributes ? attributes(session) : NULL;
    CFTypeRef formats = wanted ? CFDictionaryGetValue(wanted, kCVPixelBufferPixelFormatTypeKey) : NULL;
    if (formats && CFGetTypeID(formats) == CFArrayGetTypeID()) {
        const CFIndex count = CFArrayGetCount(formats);
        for (CFIndex i = 0; i < count; i++)
            if (!eightBit420(CFArrayGetValueAtIndex(formats, i)))
                return false;
        return count > 0;
    }
    return eightBit420(formats);
}

static OSStatus startHEVC(Decoder *d, VTVideoDecoderSession session, CMVideoFormatDescriptionRef format)
{
    d->avcC = avd_copy_atom(format, CFSTR("hvcC"));
    const HEVCSPS *sps = d->avcC && !avd_load_hvcc(d, d->avcC) ? hevc_decoder_sps(d->hevc) : NULL;
    const bool tenBit = nvdec_codecs(d->client) & TEGRA_NVDEC_HEVC_MAIN10;
    const char *why = !sps                                      ? "not Main, Main 10 or Main Still Picture 4:2:0"
                      : sps->width > 4096 || sps->height > 4096 ? "beyond NVDEC's 4096x4096"
                      : sps->width < 144 || sps->height < 144   ? "below NVDEC's HEVC floor of 144x144"
                      : sps->bit_depth == 10 && !tenBit         ? "Main 10, which this kernel's NVDEC does not decode"
                      : hevc_decoder_configure(d->hevc) ? "no NVDEC memory for its buffers (see the AVD alloc line)"
                                                        : NULL;
    if (why) {
        hevc_decoder_destroy(d->hevc);  // NVDEC's buffers go back before the software decoder starts
        nvdec_close(d->client);
        d->hevc = NULL;
        d->client = NULL;
        return avd_start_software(d, session, format, why);
    }
    hevc_decoder_allow_10bit(d->hevc, tenBit);
    d->width = (uint32_t)(sps->width - sps->crop_left - sps->crop_right);
    d->height = (uint32_t)(sps->height - sps->crop_top - sps->crop_bottom);
    const bool vui = sps->colour_description;
    d->matrix =
        switch_video_color_tag(format, kCMFormatDescriptionExtension_YCbCrMatrix, vui ? sps->matrix : 2, 0, d->height);
    d->primaries = switch_video_color_tag(format, kCMFormatDescriptionExtension_ColorPrimaries,
                                          vui ? sps->colour_primaries : 2, 1, 0);
    d->transfer =
        switch_video_color_tag(format, kCMFormatDescriptionExtension_TransferFunction, vui ? sps->transfer : 2, 2, 0);
    d->chromaTop = switch_video_chroma_tag(format, kCMFormatDescriptionExtension_ChromaLocationTopField,
                                           sps->chroma_location, sps->chroma_top);
    d->chromaBottom = switch_video_chroma_tag(format, kCMFormatDescriptionExtension_ChromaLocationBottomField,
                                              sps->chroma_location, sps->chroma_bottom);
    // Mode 2 truncates Rec.709-coded samples; PQ and HLG pictures stay 10-bit for whoever maps their range.
    const bool hdr = d->transfer && (CFEqual(d->transfer, kCVImageBufferTransferFunction_SMPTE_ST_2084_PQ) ||
                                     CFEqual(d->transfer, kCVImageBufferTransferFunction_ITU_R_2100_HLG));
    const bool display8 = sps->bit_depth == 10 && !hdr && clientWantsEightBit(session);
    hevc_decoder_display_8bit(d->hevc, display8);
    avd_alpha_start(d, sps->bit_depth == 8);
    OSStatus status;
    avd_set_attributes(d, session, sps->full_range, sps->bit_depth == 10 && !display8, &status);
    static const char *const kProfiles[4] = {"", "Main", "Main 10", "Main Still Picture"};
    if (!status)
        os_log(gAVDLog,
               "AVD: session %ux%u in %{public}s on NVDEC (HEVC %{public}s, level %u.%u), pictures out by "
               "%{public}s%{public}s",
               d->width, d->height, getprogname(), kProfiles[sps->profile_idc], sps->level_idc / 30,
               sps->level_idc % 30 / 3, d->present ? "VIC" : "the CPU", avd_alpha_describe(d));
    return status;
}

static OSStatus startSession(VTVideoDecoderRef ref, VTVideoDecoderSession session, CMVideoFormatDescriptionRef format)
{
    Decoder *d = storage(ref);
    pthread_mutex_lock(&d->lock);
    d->session = session;
    avd_emitter_set_session(d->emitter, session);
    if (d->vp9 || d->hevc || d->mpeg4 || d->mpeg2) {
        const OSStatus status = d->vp9     ? startVP9(d, session, format)
                                : d->hevc  ? startHEVC(d, session, format)
                                : d->mpeg4 ? avd_start_mpeg4(d, session, format)
                                           : avd_start_mpeg2(d, session, format);
        pthread_mutex_unlock(&d->lock);
        return status;
    }
    d->avcC = copyAvcC(format);
    const int configured = d->avcC && !loadAvcC(d, d->avcC) ? h264_decoder_configure(d->h264) : -1;
    const H264SPS *sps = configured ? NULL : h264_decoder_sps(d->h264);
    OSStatus status;
    if (!sps) {
        // NVDEC's buffers go back for other sessions before the software decoder takes over.
        h264_decoder_destroy(d->h264);
        nvdec_close(d->client);
        d->h264 = NULL;
        d->client = NULL;
        status = avd_start_software(d, session, format,
                                    configured == -3 ? "no NVDEC memory for its buffers (see the AVD alloc/map line)"
                                                     : "not decodable on NVDEC (4:2:0 8-bit up to 4096x4096 is)");
        pthread_mutex_unlock(&d->lock);
        return status;
    }
    d->width = (uint32_t)(sps->width_mbs * 16 - (int32_t)(sps->crop_left + sps->crop_right));
    d->height = (uint32_t)(sps->height_mbs * 16 - (int32_t)(sps->crop_top + sps->crop_bottom));
    const bool vui = sps->colour_description;
    d->pixelFormat = sps->full_range ? kCVPixelFormatType_420YpCbCr8BiPlanarFullRange
                                     : kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
    d->matrix =
        switch_video_color_tag(format, kCMFormatDescriptionExtension_YCbCrMatrix, vui ? sps->matrix : 2, 0, d->height);
    d->primaries = switch_video_color_tag(format, kCMFormatDescriptionExtension_ColorPrimaries,
                                          vui ? sps->colour_primaries : 2, 1, 0);
    d->transfer =
        switch_video_color_tag(format, kCMFormatDescriptionExtension_TransferFunction, vui ? sps->transfer : 2, 2, 0);
    d->chromaTop = switch_video_chroma_tag(format, kCMFormatDescriptionExtension_ChromaLocationTopField,
                                           sps->chroma_location, sps->chroma_top);
    d->chromaBottom = switch_video_chroma_tag(format, kCMFormatDescriptionExtension_ChromaLocationBottomField,
                                              sps->chroma_location, sps->chroma_bottom);
    CFMutableDictionaryRef attributes = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionaryRef surface = CFDictionaryCreate(kCFAllocatorDefault, NULL, NULL, 0, &kCFTypeDictionaryKeyCallBacks,
                                                 &kCFTypeDictionaryValueCallBacks);
    avd_set_number(attributes, kCVPixelBufferPixelFormatTypeKey, (int32_t)d->pixelFormat);
    avd_set_number(attributes, kCVPixelBufferWidthKey, (int32_t)d->width);
    avd_set_number(attributes, kCVPixelBufferHeightKey, (int32_t)d->height);
    setAlignment(d, attributes);
    CFDictionarySetValue(attributes, kCVPixelBufferIOSurfacePropertiesKey, surface);
    status = VTDecoderSessionSetPixelBufferAttributes(session, attributes);
    CFRelease(surface);
    CFRelease(attributes);
    if (!status)
        os_log(gAVDLog,
               "AVD: session %ux%u in %{public}s on NVDEC (H.264 profile %u level %u, %u reference frames, "
               "%{public}s), pictures out by %{public}s",
               d->width, d->height, getprogname(), sps->profile_idc, sps->level_idc, sps->max_num_ref_frames,
               sps->frame_mbs_only            ? "progressive"
               : sps->mb_adaptive_frame_field ? "interlaced, MBAFF"
                                              : "interlaced",
               d->present ? "VIC" : "the CPU");
    pthread_mutex_unlock(&d->lock);
    return status;
}

bool avd_sample_bytes(CMSampleBufferRef sample, const uint8_t **data, size_t *size, uint8_t **copy)
{
    *copy = NULL;
    CMBlockBufferRef block = CMSampleBufferGetDataBuffer(sample);
    *size = block ? CMBlockBufferGetDataLength(block) : 0;
    if (!*size)
        return false;
    char *at = NULL;
    if (CMBlockBufferIsRangeContiguous(block, 0, *size) &&
        CMBlockBufferGetDataPointer(block, 0, NULL, NULL, &at) == kCMBlockBufferNoErr) {
        *data = (const uint8_t *)at;
        return true;
    }
    if (!(*copy = malloc(*size)) || CMBlockBufferCopyDataBytes(block, 0, *size, *copy) != kCMBlockBufferNoErr) {
        free(*copy);
        *copy = NULL;
        return false;
    }
    *data = *copy;
    return true;
}

static OSStatus decodeFrame(VTVideoDecoderRef ref, VTVideoDecoderFrame frame, CMSampleBufferRef sample,
                            VTDecodeFrameFlags flags, VTDecodeInfoFlags *info)
{
    Decoder *d = storage(ref);
    if (d->software)
        return softwareVTable(d)->decoderClass->decodeFrame(d->software, frame, sample, flags, info);
    if (info)
        *info = 0;
    const uint8_t *data = NULL;
    uint8_t *copy = NULL;
    size_t size = 0;
    if (!avd_sample_bytes(sample, &data, &size, &copy))
        return size ? kVTVideoDecoderMalfunctionErr : kVTVideoDecoderBadDataErr;
    pthread_mutex_lock(&d->lock);
    const uint64_t began = mach_absolute_time(), copied = d->copyTicks;
    avd_emitter_frame_began(d->emitter, began);
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    avd_frame_begin(&d->timing, began);
#endif
    // VP9: a superframe's hidden frames and its shown one; 0 when the sample showed none (answered like H.264's
    // parameter-set-only units).
    const bool reorder = kReorder && (flags & kVTDecodeFrame_EnableTemporalProcessing);
    const int ret = d->vp9     ? vp9_decoder_decode(d->vp9, data, size, frame)
                    : d->mpeg4 ? mpeg4_decoder_decode(d->mpeg4, data, size, frame)
                    : d->mpeg2 ? mpeg2_decoder_decode(d->mpeg2, data, size, frame)
                    : d->hevc  ? hevc_decoder_decode(d->hevc, data, size, d->lengthSize, frame, reorder)
                               : h264_decoder_decode(d->h264, data, size, d->lengthSize, frame, reorder);
    if (ret == 1) {
        d->frames++;
        d->decodeTicks += mach_absolute_time() - began - (d->copyTicks - copied);
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
        avd_frame_end(&d->timing, mach_absolute_time());
#endif
    }
    if (!ret)  // parameter sets only: answered without a picture, after the frames before it
        answer(d, frame, noErr);
    // A VP9, HEVC or MPEG sample that failed is answered through the session with its error: VideoToolbox keeps a
    // frame whose decodeFrame returned an error outstanding, and VTDecompressionSessionWaitForAsynchronousFrames then
    // never returns (macOS 27, test/avd_host). Those decoders handle one sample's picture within the call, so the
    // frame has not been answered yet.
    const OSStatus error = ret <= -2 ? kVTVideoDecoderMalfunctionErr : ret < 0 ? kVTVideoDecoderBadDataErr : noErr;
    const bool answered = error && (d->vp9 || d->hevc || d->mpeg4 || d->mpeg2);
    if (answered)
        answer(d, frame, error);
    pthread_mutex_unlock(&d->lock);
    free(copy);
    const bool asynchronous = flags & kVTDecodeFrame_EnableAsynchronousDecompression;
    if (asynchronous && info && (!error || answered))
        *info |= kVTDecodeInfo_Asynchronous;
    avd_emitter_drain(d->emitter);
    if (!asynchronous)
        avd_emitter_settle(d->emitter, true);
    if (ret <= -2) {
        static int logged;
        if (!__atomic_exchange_n(&logged, 1, __ATOMIC_RELAXED))
            os_log_error(gAVDLog, "AVD: %{public}s",
                         ret == -2 ? "NVDEC job failed" : "no NVDEC buffers for a new geometry");
    }
    return answered ? noErr : error;
}

// VTDecompressionSessionCopyProperty hands a key it does not answer itself to the decoder only when the decoder lists
// it here (21H461, 0x1978a2438); UsingHardwareAcceleratedVideoDecoder is such a key, so it is listed, read-only, next
// to whatever the software decoder lists when this instance runs on it.
static OSStatus copySupportedPropertyDictionary(VTVideoDecoderRef ref, CFDictionaryRef *out)
{
    Decoder *d = storage(ref);
    CFDictionaryRef software = NULL;
    if (d->software && softwareVTable(d)->decoderClass->copySupportedPropertyDictionary)
        softwareVTable(d)->decoderClass->copySupportedPropertyDictionary(d->software, &software);
    CFMutableDictionaryRef supported =
        software ? CFDictionaryCreateMutableCopy(kCFAllocatorDefault, 0, software)
                 : CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
                                             &kCFTypeDictionaryValueCallBacks);
    if (software)
        CFRelease(software);
    if (!supported)
        return kVTAllocationFailedErr;
    const void *keys[] = {kVTPropertyTypeKey, kVTPropertyReadWriteStatusKey};
    const void *values[] = {kVTPropertyType_Boolean, kVTPropertyReadWriteStatus_ReadOnly};
    CFDictionaryRef description = CFDictionaryCreate(kCFAllocatorDefault, keys, values, 2,
                                                     &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(supported, kVTDecompressionPropertyKey_UsingHardwareAcceleratedVideoDecoder, description);
    CFRelease(description);
    if (!d->software)
        avd_alpha_list_properties(supported);
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    CFDictionaryRef routes = CFDictionaryCreate(kCFAllocatorDefault, &keys[1], &values[1], 1,
                                                &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(supported, kSwitchPictureRoutesKey, routes);
    CFRelease(routes);
    const void *probeValues[] = {kVTPropertyType_Enumeration, kVTPropertyReadWriteStatus_ReadWrite};
    CFDictionaryRef probe = CFDictionaryCreate(kCFAllocatorDefault, keys, probeValues, 2,
                                               &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(supported, kSwitchDisplayBufferProbeKey, probe);
    CFRelease(probe);
#endif
    *out = supported;
    return noErr;
}

// A new format keeps this instance only when its parameter sets are the same bytes; otherwise VideoToolbox makes a
// fresh decoder for it.
static Boolean canAcceptFormatDescription(VTVideoDecoderRef ref, CMVideoFormatDescriptionRef format)
{
    Decoder *d = storage(ref);
    if (d->software) {
        const VTPluginDecoderClass *software = softwareVTable(d)->decoderClass;
        return software->canAcceptFormatDescription && software->canAcceptFormatDescription(d->software, format);
    }
    if (d->hevc || d->mpeg4) {  // the same parameter sets (or video object layer) keep this instance
        CFDataRef hvcC = avd_copy_atom(format, d->mpeg4 ? CFSTR("esds") : CFSTR("hvcC"));
        const Boolean same = hvcC && d->avcC && CFEqual(hvcC, d->avcC);
        if (hvcC)
            CFRelease(hvcC);
        return same;
    }
    if (d->mpeg2)
        return avd_mpeg2_accepts(d, format);
    if (d->vp9) {  // a new size or profile gets a fresh decoder; frames within one session may change size
        const CMVideoDimensions size = CMVideoFormatDescriptionGetDimensions(format);
        CFDataRef vpcC = avd_copy_atom(format, CFSTR("vpcC"));
        const Boolean same = (uint32_t)size.width == d->width && (uint32_t)size.height == d->height &&
                             (vpcC == d->avcC || (vpcC && d->avcC && CFEqual(vpcC, d->avcC)));
        if (vpcC)
            CFRelease(vpcC);
        return same;
    }
    CFDataRef avcC = copyAvcC(format);
    const Boolean same = avcC && d->avcC && CFEqual(avcC, d->avcC);
    if (avcC)
        CFRelease(avcC);
    return same;
}

static OSStatus finishDelayedFrames(VTVideoDecoderRef ref)
{
    Decoder *d = storage(ref);
    if (d->software) {
        const VTPluginDecoderClass *software = softwareVTable(d)->decoderClass;
        return software->finishDelayedFrames ? software->finishDelayedFrames(d->software) : noErr;
    }
    pthread_mutex_lock(&d->lock);
    if (d->h264)
        h264_decoder_flush(d->h264);  // VP9 frames go out as they are decoded
    if (d->hevc)
        hevc_decoder_flush(d->hevc);
    pthread_mutex_unlock(&d->lock);
    // VideoToolbox may return before they are emitted; VTDecompressionSessionWaitForAsynchronousFrames waits for them.
    avd_emitter_drain(d->emitter);
    return noErr;
}

static OSStatus invalidate(CMBaseObjectRef ref)
{
    Decoder *d = storage(ref);
    if (d->software) {
        const VTPluginBaseClass *software = softwareVTable(d)->baseClass;
        return software->invalidate ? software->invalidate(VTVideoDecoderGetCMBaseObject(d->software)) : noErr;
    }
    pthread_mutex_lock(&d->lock);
    if (d->h264)
        h264_decoder_flush(d->h264);
    if (d->hevc)
        hevc_decoder_flush(d->hevc);
    pthread_mutex_unlock(&d->lock);
    avd_emitter_settle(d->emitter, true);  // every frame decoded so far goes out, in decode order
    pthread_mutex_lock(&d->lock);
    d->session = NULL;
    avd_emitter_close(d->emitter);
    pthread_mutex_unlock(&d->lock);
    return noErr;
}

static void finalize(CMBaseObjectRef ref)
{
    Decoder *d = storage(ref);
    d->session = NULL;
    if (d->frames)
        os_log(gAVDLog,
               "AVD: session %ux%u in %{public}s ended: %u frames on NVDEC, %u with errors, mean %.2f ms decode + "
               "%.2f ms out (%u pictures exported as the surface's native NVDEC picture, %u placed by VIC, %u by the "
               "CPU)",
               d->width, d->height, getprogname(), d->frames, avd_emitter_errors(d->emitter),
               ticksToMs(d->decodeTicks) / d->frames, ticksToMs(d->copyTicks) / d->frames, d->exported,
               d->presented - d->exported, d->copied);
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    if (d->frames) {
        const uint64_t *slowest = d->timing.slowest;
        os_log(gAVDLog,
               "AVD: session %ux%u in %{public}s slowest frame %.2f ms: %.2f ms before submit, %.2f ms submit to "
               "fence, %.2f ms fence to output",
               d->width, d->height, getprogname(), ticksToMs(avd_frame_total(slowest)),
               ticksToMs(slowest[AVD_BEFORE_SUBMIT]), ticksToMs(slowest[AVD_SUBMIT_TO_FENCE]),
               ticksToMs(slowest[AVD_FENCE_TO_OUTPUT]));
    }
#endif
    if (d->software)
        CFRelease(d->software);
    if (d->client)
        nvdec_stop_listening(d->client);
    h264_decoder_destroy(d->h264);  // frees its buffers through the client, so before closing it
    vp9_decoder_destroy(d->vp9);
    hevc_decoder_destroy(d->hevc);
    mpeg4_decoder_destroy(d->mpeg4);
    mpeg2_decoder_destroy(d->mpeg2);
    nvdec_close(d->client);
    avd_emitter_destroy(d->emitter);
    if (d->avcC)
        CFRelease(d->avcC);
    if (d->matrix)
        CFRelease(d->matrix);
    if (d->primaries)
        CFRelease(d->primaries);
    if (d->transfer)
        CFRelease(d->transfer);
    if (d->chromaTop)
        CFRelease(d->chromaTop);
    if (d->chromaBottom)
        CFRelease(d->chromaBottom);
    pthread_mutex_destroy(&d->lock);
}

static CFStringRef copyDebugDescription(CMBaseObjectRef ref)
{
    Decoder *d = storage(ref);
    return CFStringCreateWithFormat(kCFAllocatorDefault, NULL, CFSTR("<AVD %s decoder %p on %s>"),
                                    avd_codec_name(d->codec), ref, d->software ? "Apple's software decoder" : "NVDEC");
}

static OSStatus copyProperty(CMBaseObjectRef ref, CFStringRef key, CFAllocatorRef allocator, void *value)
{
    Decoder *d = storage(ref);
    if (CFEqual(key, kVTDecompressionPropertyKey_UsingHardwareAcceleratedVideoDecoder)) {
        *(CFTypeRef *)value = CFRetain(d->software ? kCFBooleanFalse : kCFBooleanTrue);
        return noErr;
    }
    if (!d->software && avd_alpha_copy_property(d, key, (CFTypeRef *)value))
        return noErr;
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    if (CFEqual(key, kSwitchPictureRoutesKey) && !d->software) {
        *(CFTypeRef *)value = avd_copy_picture_routes(d);
        return *(CFTypeRef *)value ? noErr : kVTAllocationFailedErr;
    }
    if (CFEqual(key, kSwitchDisplayBufferProbeKey) && d->hevc) {
        *(CFTypeRef *)value = avd_copy_display_buffer_probe(d);
        return *(CFTypeRef *)value ? noErr : -12783;
    }
#endif
    if (d->software && softwareVTable(d)->baseClass->copyProperty)
        return softwareVTable(d)->baseClass->copyProperty(VTVideoDecoderGetCMBaseObject(d->software), key, allocator,
                                                          value);
    return -12783;  // kCMBaseObjectError_ValueNotAvailable
}

static OSStatus setProperty(CMBaseObjectRef ref, CFStringRef key, CFTypeRef value)
{
    Decoder *d = storage(ref);
    if (!d->software && avd_alpha_set_property(d, key, value))
        return noErr;
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    if (CFEqual(key, kSwitchDisplayBufferProbeKey) && d->hevc && value == kCFBooleanTrue) {
        hevc_decoder_want_display_probe(d->hevc);
        return noErr;
    }
#endif
    if (d->software && softwareVTable(d)->baseClass->setProperty)
        return softwareVTable(d)->baseClass->setProperty(VTVideoDecoderGetCMBaseObject(d->software), key, value);
    return noErr;  // real-time, thread count, ...: nothing to tune on a fixed-function engine
}

static const VTPluginBaseClass kBaseClass = {
    .version = 1,
    .derivedStorageSize = sizeof(Decoder),
    .invalidate = invalidate,
    .finalize = finalize,
    .copyDebugDescription = copyDebugDescription,
    .copyProperty = copyProperty,
    .setProperty = setProperty,
};

// Version 3: VideoToolbox calls finishDelayedFrames (+0x38) only for version 2 and up (21H461
// VTDecompressionSessionFinishDelayedFrames, 0x1978dc40c), and display-order bumping (kReorder, HEIC tiles) holds
// pictures until then; the tile session slots (+0x40, +0x48) only for version 3 (VTTileDecompressionSessionCreate,
// 0x197bab448), where a slot that refuses moves VideoToolbox on to the next decoder. H264SW declares version 3 and
// leaves setProperties and copySerializableProperties empty, so VideoToolbox checks the optional slots before calling.
static const VTPluginDecoderClass kDecoderClass = {
    .version = 3,
    .startSession = startSession,
    .decodeFrame = decodeFrame,
    .copySupportedPropertyDictionary = copySupportedPropertyDictionary,
    .canAcceptFormatDescription = canAcceptFormatDescription,
    .finishDelayedFrames = finishDelayedFrames,
    .startTileSession = avd_start_tile_session,
    .decodeTile = avd_decode_tile,
};

static const VTPluginDecoderVTable kVTable = {NULL, &kBaseClass, &kDecoderClass};

OSStatus avd_create_instance(FourCharCode codec, CFAllocatorRef allocator, VTVideoDecoderRef *out)
{
    if (tSoftwareLookup)
        return kVTCouldNotFindVideoDecoderErr;  // VideoToolbox moves on to the next decoder
    int error = 0;
    NVDECClient *client = nvdec_open(&error);
    if (!client) {  // VideoToolbox goes on to H264SW
        switch_refusal("AVD", "TegraNVDEC refused the process after registration",
                       "error %#x: H.264 goes to Apple's software decoder", error);
        return kVTCouldNotFindVideoDecoderErr;
    }
    CMBaseObjectRef object = NULL;
    OSStatus status = CMDerivedObjectCreate(allocator, &kVTable, VTVideoDecoderGetClassID(), &object);
    if (status) {
        nvdec_close(client);
        return status;
    }
    Decoder *d = storage(object);
    memset(d, 0, sizeof(*d));
    pthread_mutex_init(&d->lock, NULL);
    d->client = client;
    d->codec = codec;
    d->planeAsked = -1;
    d->present = nvdec_can_present(client);
    d->emitter = avd_emitter_create(client);
    const H264Backend backend = {.ctx = d,
                                 .alloc = bufferAlloc,
                                 .free = bufferFree,
                                 .submit = jobSubmit,
                                 .wait = jobWait,
                                 .decoded = decoded,
                                 .emit = emit,
                                 .discard = discard,
                                 .recycle = switch_recycle_output,
                                 .inputs = nvdec_jobs_in_flight(client) + 1};
    const bool listening =
        d->emitter && nvdec_listen(client, avd_emitter_queue(d->emitter), avd_emitter_completed, d->emitter);
    if (!listening || (codec == kCMVideoCodecType_VP9          ? !(d->vp9 = vp9_decoder_create(&backend))
                       : codec == kCMVideoCodecType_HEVC       ? !(d->hevc = hevc_decoder_create(&backend))
                       : codec == kCMVideoCodecType_MPEG4Video ? !(d->mpeg4 = mpeg4_decoder_create(&backend))
                       : codec == kCMVideoCodecType_MPEG2Video || codec == kCMVideoCodecType_MPEG1Video
                           ? !(d->mpeg2 = mpeg2_decoder_create(&backend))
                           : !(d->h264 = h264_decoder_create(&backend)))) {
        CFRelease(object);  // finalize closes the client
        return kVTAllocationFailedErr;
    }
    *out = object;
    return noErr;
}
