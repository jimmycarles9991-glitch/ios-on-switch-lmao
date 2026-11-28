// SPDX-License-Identifier: BSD-2-Clause-Patent
// AVE stand-in: H.264 and HEVC on the Tegra X1's NVENC as VideoToolbox's hardware 'avc1' and 'hvc1' encoders, the
// latter under ave.videoencoder's HEVC encoder ID; both code IDR and P pictures, H.264 B pictures too where the
// session allows reordering (AVEReorder.c), HEIC tiles are IDRs (AVETiles.c).
//
// How VideoToolbox finds it (21H461 VTLoadVideoEncoders, disassembled): VTInitializeVideoEncoderRegistry registers
// VideoToolbox's own wrappers (muxed alpha, depth), then dlopens a fixed list of paths and calls each one's register
// function: HEVCH9, H9, VCPHEVC, H264H9, then /System/Library/VideoEncoders/H264H7.videoencoder (H264H7EncoderRegister),
// H264H4, H264H2, JPEGH1, VCH263, ... On iPad7,1 the H.264 and HEVC encoders are H264H9 and H9 (AVE), which register
// only when IOServiceMatching("AppleAVE2Driver") finds the AVE kext (AppleAVEVA_CheckPlatform), and VCPHEVC's register
// function is an empty stub: without AVE this system has no H.264 encoder at all, hardware or software. H264H7 is not
// in the dyld cache and iPad7,1 ships no such file; the userland overlay puts this one there.
//
// Registration: 'avc1' with CMClassImplementationID com.apple.videotoolbox.videoencoder.h264 (AVE's own encoder ID)
// and IsHardwareAccelerated, in every process once the kernel published TegraNVENC (it proved itself at boot, E4).
// Most processes never run an encoder themselves: VTCompressionSession remotes their sessions to the media server
// (mediaserverd; videocodecd with CoreMedia's SeparateCodecProcess_Encode), but they look encoders up (AVAssetWriter's
// output settings, VTCopySupportedPropertyDictionaryForEncoder), so the entry has to be there. An instance opens the
// kernel client only when a session starts; a process the kernel refuses gets kVTCouldNotFindVideoEncoderErr there.
//
// Sessions: what NVENC takes (TegraNVENCCheck.h: progressive 4:2:0, 32..4096 a side, at most 4096x2304 macroblocks'
// worth) runs on NVENC; anything else is refused at StartSession, and VideoToolbox has no other H.264 encoder to try.
// No software encoder inside: there is no Apple one on 21H461 to fall back to.
// Pixel buffers are asked for as NV12 ('420v'/'420f') with rows and planes VIC reads (AVEInput.c); VideoToolbox's
// pixel transfer converts anything else (BGRA screen captures) before EncodeFrame.
//
// Per picture: the submit names the picture's IOSurface and VIC writes it into the session's input buffer in the
// layout NVENC reads (AVEInput.c), the setup built (H264NVENC.c) with the QP the rate control picked (RateControl.h),
// the job queued on host1x channel 0 off e->lock, and completion arrives via the kernel's Mach notification port
// (which also invalidates the produced bitstream slice in the kernel, avoiding Wait ioctls altogether). Then NVENC's
// Annex B slice becomes a length-prefixed sample and goes out off e->lock. Tile sessions pipeline multiple IDRs
// asynchronously across tile slots (AVETiles.c / AVEEmitter.c).
//
// Log (subsystem com.brunostjohn.switch.ave, category encoder): per session, one line when it starts and one when it ends
// (frames, bytes, inputs by VIC and copied, mean input and encode time). Refusals (a size, TegraNVENC refusing the
// process, a property) are kits/SwitchRefusal's, category AVE: one error line per process and reason.
#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
#include <mach/mach_time.h>
#endif
#include <os/log.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "AVEEncoder.h"
#include "../../../kits/SwitchRefusal/SwitchRefusal.h"
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-const-variable"
#pragma clang diagnostic ignored "-Wunused-variable"
#include "../../TegraNVENC/nvidia/nvenc_drv.h"
#pragma clang diagnostic pop

// 21H461 is iOS 17.7: the hardware-encoder keys the SDK marks iOS 17.4 are there (weak imports at the 17.0 target).
#pragma clang diagnostic ignored "-Wunguarded-availability-new"

#define AVE_ENCODER_ID      "com.apple.videotoolbox.videoencoder.h264"
#define AVE_HEVC_ENCODER_ID "com.apple.videotoolbox.videoencoder.hevc"

#define kSwitchEncoderRoutesKey      CFSTR("SwitchEncoderRoutes")
#define kSwitchEncoderLayoutProbeKey CFSTR("SwitchEncoderLayoutProbe")

os_log_t gLog;

static Encoder *storage(CMBaseObjectRef object)
{
    return CMBaseObjectGetDerivedStorage(object);
}

#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
static double ticksToMs(uint64_t ticks)
{
    static mach_timebase_info_data_t base;
    if (!base.denom)
        mach_timebase_info(&base);
    return (double)ticks * base.numer / base.denom / 1e6;
}
#endif

static void replace(CFTypeRef *slot, CFTypeRef value)
{
    if (value)
        CFRetain(value);
    if (*slot)
        CFRelease(*slot);
    *slot = value;
}

static bool number(CFTypeRef value, double *out)
{
    if (!value || CFGetTypeID(value) != CFNumberGetTypeID())
        return false;
    return CFNumberGetValue(value, kCFNumberDoubleType, out);
}

// Colour: session properties, else the first picture's attachments, as H.273 code points.
static uint8_t primariesCode(CFTypeRef v)
{
    if (!v)
        return 2;
    if (CFEqual(v, kCVImageBufferColorPrimaries_ITU_R_709_2))
        return 1;
    if (CFEqual(v, kCVImageBufferColorPrimaries_EBU_3213))
        return 5;
    if (CFEqual(v, kCVImageBufferColorPrimaries_SMPTE_C))
        return 6;
    if (CFEqual(v, kCVImageBufferColorPrimaries_ITU_R_2020))
        return 9;
    if (CFEqual(v, kCVImageBufferColorPrimaries_P3_D65))
        return 12;
    return 2;
}

static uint8_t transferCode(CFTypeRef v)
{
    if (!v)
        return 2;
    if (CFEqual(v, kCVImageBufferTransferFunction_ITU_R_709_2))
        return 1;
    if (CFEqual(v, kCVImageBufferTransferFunction_SMPTE_ST_2084_PQ))
        return 16;
    if (CFEqual(v, kCVImageBufferTransferFunction_ITU_R_2100_HLG))
        return 18;
    if (CFEqual(v, kCVImageBufferTransferFunction_sRGB))
        return 13;
    return 2;
}

static uint8_t matrixCode(CFTypeRef v)
{
    if (!v)
        return 2;
    if (CFEqual(v, kCVImageBufferYCbCrMatrix_ITU_R_709_2))
        return 1;
    if (CFEqual(v, kCVImageBufferYCbCrMatrix_ITU_R_601_4))
        return 6;
    if (CFEqual(v, kCVImageBufferYCbCrMatrix_ITU_R_2020))
        return 9;
    return 2;
}

void ave_begin_encode(Encoder *e)
{
    while (e->encoding) {
        e->busyWaiters++;
        pthread_cond_wait(&e->busyCond, &e->lock);
        e->busyWaiters--;
    }
    e->encoding = true;
}

void ave_end_encode(Encoder *e)
{
    e->encoding = false;
    if (e->busyWaiters)
        pthread_cond_broadcast(&e->busyCond);
}

static void releaseBuffers(Encoder *e)
{
    for (unsigned i = 0; i < e->bufferCount; i++)
        nvenc_free(e->client, e->buffers[i]);
    e->bufferCount = 0;
}

static int allocBuffer(Encoder *e, uint64_t size, uint32_t *handle, uint8_t **va)
{
    void *p = NULL;
    if (e->bufferCount == sizeof(e->buffers) / sizeof(e->buffers[0]) || nvenc_alloc(e->client, size, handle, &p))
        return -1;
    e->buffers[e->bufferCount++] = *handle;
    if (va)
        *va = p;
    return 0;
}

int ave_alloc_buffer(Encoder *e, uint64_t size, uint32_t *handle)
{
    return allocBuffer(e, size, handle, NULL);
}

void ave_tile_record_stats(void *context, OSStatus status, size_t bytes, uint64_t copyTicks, uint64_t encodeTicks)
{
    Encoder *e = context;
    pthread_mutex_lock(&e->lock);
    if (status) {
        e->errors++;
    } else {
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
        e->copyTicks += copyTicks;
        e->encodeTicks += encodeTicks;
#else
        (void)copyTicks;
        (void)encodeTicks;
#endif
        e->frames++;
        e->keyFrames++;
        e->tiles++;
        e->bytes += bytes;
    }
    pthread_mutex_unlock(&e->lock);
}

bool ave_wait_job(Encoder *e, uint32_t fence, uint32_t auxHandle, const uint8_t *auxVA, uint32_t bitsHandle,
                  uint32_t bitsLimit, AVEJobStatus *out)
{
    memset(out, 0, sizeof(*out));
    if (e->emitter) {
        if (!ave_emitter_await(e->emitter, fence, out)) {
            out->result = -5;
            return false;
        }
        return out->result == 0;
    }
    TegraNVENCWait wait = {.fence = fence, .timeout_us = 1000000, .invalidate_count = 1};
    wait.invalidate[0] = (TegraNVENCRange){auxHandle, 0, sizeof(nvenc_pic_stat_s), 0};
    if (nvenc_wait(e->client, &wait)) {
        out->result = -5;
        return false;
    }
    const nvenc_pic_stat_s *stat = (const nvenc_pic_stat_s *)auxVA;
    out->error_status = stat->error_status;
    out->ucode_error_status = stat->ucode_error_status;
    out->total_bit_count = stat->total_bit_count;
    if (!(e->info.features & TEGRA_NVENC_FEATURE_COMPLETION)) {
        const uint32_t produced = (stat->total_bit_count + 7) / 8;
        if (!stat->ucode_error_status && produced && produced <= bitsLimit) {
            TegraNVENCWait bits = {.fence = fence, .timeout_us = 1000000, .invalidate_count = 1};
            bits.invalidate[0] = (TegraNVENCRange){bitsHandle, 0, produced, 0};
            if (nvenc_wait(e->client, &bits)) {
                out->result = -5;
                return false;
            }
        }
    }
    return true;
}

static OSStatus startStream(Encoder *e, CFTypeRef primaries, CFTypeRef transfer, CFTypeRef matrix);

// The first picture's pixel format (full range or not) and colour tags go into the SPS.
OSStatus ave_configure(Encoder *e, CVPixelBufferRef image)
{
    H264EncConfig *c = &e->config;
    memset(c, 0, sizeof(*c));
    c->width = (uint32_t)e->size.width;
    c->height = (uint32_t)e->size.height;
    c->profile_idc = H264_PROFILE_HIGH;
    c->cabac = true;
    if (e->profileLevel) {
        char name[64] = "";
        CFStringGetCString(e->profileLevel, name, sizeof(name), kCFStringEncodingUTF8);
        if (!strncmp(name, "H264_Baseline", 13) || !strncmp(name, "H264_ConstrainedBaseline", 24))
            c->profile_idc = H264_PROFILE_BASELINE;
        else if (!strncmp(name, "H264_Main", 9))
            c->profile_idc = H264_PROFILE_MAIN;
        const char *level = strrchr(name, '_');
        if (level && level[1] >= '1' && level[1] <= '5' && level[2] == '_' && level[3] >= '0' && level[3] <= '9')
            c->level_idc = (uint8_t)((level[1] - '0') * 10 + (level[3] - '0'));
    }
    if (e->entropy && CFEqual(e->entropy, kVTH264EntropyMode_CAVLC))
        c->cabac = false;
    c->reorder = ave_reorder_wanted(e);
    c->transform8x8 = c->profile_idc == H264_PROFILE_HIGH;
    const OSType format = CVPixelBufferGetPixelFormatType(image);
    c->full_range = format == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange;
    // Retained: the session's own tags, else the picture's.
    CFTypeRef primaries =
        e->primaries ? CFRetain(e->primaries) : CVBufferCopyAttachment(image, kCVImageBufferColorPrimariesKey, NULL);
    CFTypeRef transfer =
        e->transfer ? CFRetain(e->transfer) : CVBufferCopyAttachment(image, kCVImageBufferTransferFunctionKey, NULL);
    CFTypeRef matrix =
        e->matrix ? CFRetain(e->matrix) : CVBufferCopyAttachment(image, kCVImageBufferYCbCrMatrixKey, NULL);
    c->colour_primaries = primariesCode(primaries);
    c->transfer = transferCode(transfer);
    c->matrix = matrixCode(matrix);
    c->colour_description = c->colour_primaries != 2 || c->transfer != 2 || c->matrix != 2;
    OSStatus status = startStream(e, primaries, transfer, matrix);
    if (primaries)
        CFRelease(primaries);
    if (transfer)
        CFRelease(transfer);
    if (matrix)
        CFRelease(matrix);
    return status;
}

static void setupTileSlots(Encoder *e)
{
    e->tileSlotCount = 0;
    if (!e->tileSession || !e->emitter)
        return;
    uint32_t want = e->info.jobs_in_flight ? e->info.jobs_in_flight : AVE_TILE_SLOTS;
    if (want > AVE_TILE_SLOTS)
        want = AVE_TILE_SLOTS;
    e->tileSlots[0] =
        (AVETileSlot){e->handles.input, e->handles.bitstream, e->handles.aux, e->inputVA, e->bitstreamVA, e->auxVA};
    uint32_t count = 1;
    const H264EncLayout *l = &e->layout;
    for (uint32_t i = 1; i < want; i++) {
        AVETileSlot slot = {0};
        if (allocBuffer(e, l->input.bytes, &slot.inputHandle, &slot.inputVA) ||
            allocBuffer(e, l->bitstream_bytes, &slot.bitstreamHandle, &slot.bitstreamVA) ||
            allocBuffer(e, l->aux_bytes, &slot.auxHandle, &slot.auxVA))
            break;
        e->tileSlots[count++] = slot;
    }
    if (ave_emitter_configure_tiles(e->emitter, e->tileSession, e->format, e->tileSlots, count, l->bitstream_bytes,
                                    ave_tile_record_stats, e))
        e->tileSlotCount = count;
}

// The kernel buffers, the format description of this stream's parameter sets, and the rate control.
static OSStatus startStream(Encoder *e, CFTypeRef primaries, CFTypeRef transfer, CFTypeRef matrix)
{
    H264EncConfig *c = &e->config;
    if (e->expectedFrameRate > 0 && e->expectedFrameRate <= 240) {
        c->fps_num = (uint32_t)lround(e->expectedFrameRate * 1000);
        c->fps_den = 1000;
    }
    c->input_layout = ave_input_layout(e);
    c->chroma_mode = e->info.chroma_mode;
    e->surfaceInput = (e->info.features & TEGRA_NVENC_FEATURE_SURFACE_INPUT) != 0;
    if (h264enc_configure(c))
        return kVTPropertyNotSupportedErr;
    const bool hevc = e->codec == kCMVideoCodecType_HEVC;
    if (hevc) {
        // The H.264 config above stays the input copy's (h264enc_put_input); the stream is the HEVC config's.
        e->hevc = (HEVCEncConfig){.width = c->width,
                                  .height = c->height,
                                  .fast = e->realTime || e->prioritizeSpeed,
                                  .full_range = c->full_range,
                                  .colour_description = c->colour_description,
                                  .colour_primaries = c->colour_primaries,
                                  .transfer = c->transfer,
                                  .matrix = c->matrix,
                                  .fps_num = c->fps_num,
                                  .fps_den = c->fps_den};
        if (hevcnvenc_configure(&e->hevc))
            return kVTPropertyNotSupportedErr;
        hevcnvenc_layout(&e->hevc, c->input_layout, &e->layout);
    } else {
        h264enc_layout(c, &e->layout);
    }
    const H264EncLayout *l = &e->layout;
    uint8_t *ignored;
    if (allocBuffer(e, l->input.bytes, &e->handles.input, &e->inputVA) ||
        allocBuffer(e, l->recon.bytes, &e->handles.recon, &ignored) ||
        allocBuffer(e, l->recon.bytes, &e->handles.ref, &ignored) ||
        allocBuffer(e, l->bitstream_bytes, &e->handles.bitstream, &e->bitstreamVA) ||
        allocBuffer(e, l->aux_bytes, &e->handles.aux, &e->auxVA) ||
        allocBuffer(e, l->mepred_bytes, &e->handles.mepred[0], &ignored) ||
        allocBuffer(e, l->mepred_bytes, &e->handles.mepred[1], &ignored) || (c->reorder && ave_reorder_start(e))) {
        releaseBuffers(e);
        return kVTAllocationFailedErr;
    }
    e->avccCap = l->bitstream_bytes + 4096;
    if (!(e->avcc = malloc(e->avccCap)))
        return kVTAllocationFailedErr;

    uint8_t record[512];
    const size_t recordSize =
        hevc ? hevcenc_write_hvcc(&e->hevc, record, sizeof(record)) : h264enc_write_avcc(c, record, sizeof(record));
    if (!recordSize)
        return kVTPropertyNotSupportedErr;
    CFDataRef atom = CFDataCreate(kCFAllocatorDefault, record, (CFIndex)recordSize);
    const void *atomKeys[] = {hevc ? CFSTR("hvcC") : CFSTR("avcC")}, *atomValues[] = {atom};
    CFDictionaryRef atoms = CFDictionaryCreate(kCFAllocatorDefault, atomKeys, atomValues, 1,
                                               &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFMutableDictionaryRef extensions = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(extensions, kCMFormatDescriptionExtension_SampleDescriptionExtensionAtoms, atoms);
    CFDictionarySetValue(extensions, kCMFormatDescriptionExtension_FormatName, hevc ? CFSTR("HEVC") : CFSTR("H.264"));
    if (c->full_range)
        CFDictionarySetValue(extensions, kCMFormatDescriptionExtension_FullRangeVideo, kCFBooleanTrue);
    if (primaries)
        CFDictionarySetValue(extensions, kCMFormatDescriptionExtension_ColorPrimaries, primaries);
    if (transfer)
        CFDictionarySetValue(extensions, kCMFormatDescriptionExtension_TransferFunction, transfer);
    if (matrix)
        CFDictionarySetValue(extensions, kCMFormatDescriptionExtension_YCbCrMatrix, matrix);
    OSStatus status =
        e->tileSession ? VTTileEncoderSessionCreateVideoFormatDescription(e->tileSession, e->codec, (int32_t)c->width,
                                                                          (int32_t)c->height, extensions, &e->format)
                       : VTEncoderSessionCreateVideoFormatDescription(e->session, e->codec, (int32_t)c->width,
                                                                      (int32_t)c->height, extensions, &e->format);
    CFRelease(extensions);
    CFRelease(atoms);
    CFRelease(atom);
    if (status)
        return status;

    setupTileSlots(e);
    const double fps = e->expectedFrameRate > 0 ? e->expectedFrameRate : 30;
    const bool cbr = e->constantBitRate > 0;
    rc_init(&e->rc, cbr ? e->constantBitRate : e->averageBitRate, cbr, e->quality, fps, c->width, c->height);
    rc_set_limit(&e->rc, e->limitBytes, e->limitSeconds);
    e->configured = true;
    os_log(gLog,
           "AVE: %{public}s %ux%u in %{public}s on NVENC (%{public}s profile %u level %u, %{public}s, %{public}s, "
           "input layout %u %{public}s, %{public}s)",
           e->tileSession ? "tile session" : "session", c->width, c->height, getprogname(), hevc ? "HEVC" : "H.264",
           hevc ? 1 : c->profile_idc, hevc ? e->hevc.level_idc : c->level_idc, hevc || c->cabac ? "CABAC" : "CAVLC",
           e->rc.constQP >= 0 ? "constant quality"
           : cbr              ? "CBR"
                              : "VBR",
           c->input_layout, e->surfaceInput ? "written by VIC from IOSurfaces" : "copied by the CPU",
           c->reorder         ? "B pictures"
           : e->info.p_frames ? "P pictures"
                              : "intra only");
    return noErr;
}

bool ave_wants_key(const Encoder *e, CMTime pts, CFDictionaryRef options, uint32_t since)
{
    if (!e->haveRef || e->keyPending || !e->allowTemporal || !e->info.p_frames)
        return true;
    if (options) {
        CFTypeRef force = CFDictionaryGetValue(options, kVTEncodeFrameOptionKey_ForceKeyFrame);
        if (force && CFGetTypeID(force) == CFBooleanGetTypeID() && CFBooleanGetValue(force))
            return true;
    }
    if (e->maxKeyInterval > 0 && since + 1 >= (uint32_t)e->maxKeyInterval)
        return true;
    if (e->maxKeyDuration > 0 && CMTIME_IS_NUMERIC(pts) && CMTIME_IS_NUMERIC(e->lastKeyTime) &&
        CMTimeGetSeconds(CMTimeSubtract(pts, e->lastKeyTime)) >= e->maxKeyDuration)
        return true;
    return false;
}

static OSStatus buildSample(const Encoder *e, size_t size, const H264EncPicture *picture, bool allowTemporal,
                            CMTime pts, CMTime duration, CMTime dts, CMSampleBufferRef *out)
{
    const bool key = picture->type == H264ENC_IDR;
    const bool reference = picture->type != H264ENC_B && !picture->unreferenced;
    CMBlockBufferRef block = NULL;
    OSStatus status = CMBlockBufferCreateWithMemoryBlock(kCFAllocatorDefault, NULL, size, kCFAllocatorDefault, NULL, 0,
                                                         size, kCMBlockBufferAssureMemoryNowFlag, &block);
    if (!status)
        status = CMBlockBufferReplaceDataBytes(e->avcc, block, 0, size);
    CMSampleBufferRef sample = NULL;
    const CMSampleTimingInfo timing = {duration, pts, dts};
    if (!status)
        status = CMSampleBufferCreateReady(kCFAllocatorDefault, block, e->format, 1, 1, &timing, 1, &size, &sample);
    if (block)
        CFRelease(block);
    if (status)
        return status;
    CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sample, true);
    if (attachments && CFArrayGetCount(attachments)) {
        CFMutableDictionaryRef a = (CFMutableDictionaryRef)CFArrayGetValueAtIndex(attachments, 0);
        CFDictionarySetValue(a, kCMSampleAttachmentKey_NotSync, key ? kCFBooleanFalse : kCFBooleanTrue);
        CFDictionarySetValue(a, kCMSampleAttachmentKey_DependsOnOthers, key ? kCFBooleanFalse : kCFBooleanTrue);
        CFDictionarySetValue(a, kCMSampleAttachmentKey_IsDependedOnByOthers,
                             reference && e->info.p_frames && allowTemporal ? kCFBooleanTrue : kCFBooleanFalse);
    }
    *out = sample;
    return noErr;
}

// Called with e->lock held and e->encoding true; drops e->lock across NVENC submit/wait and VideoToolbox emit.
static OSStatus encodePicture(Encoder *e, VTVideoEncoderFrame frame, CVPixelBufferRef image, CMTime pts,
                              CMTime duration, CFDictionaryRef options, AVEPlan *plan)
{
    if (CVPixelBufferGetWidth(image) != (size_t)e->size.width ||
        CVPixelBufferGetHeight(image) != (size_t)e->size.height || CVPixelBufferGetPlaneCount(image) != 2)
        return kVTPixelTransferNotSupportedErr;
    const bool hevc = e->codec == kCMVideoCodecType_HEVC;
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    CVPixelBufferRetain(image);
    CVPixelBufferRelease(e->probeSource);
    e->probeSource = image;
#endif
    for (int attempt = 0; attempt < 2; attempt++) {
        const bool key = plan ? plan->picture.type == H264ENC_IDR : ave_wants_key(e, pts, options, e->sinceKey);
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
        const uint64_t began = mach_absolute_time();
#endif
        if (key && !plan) {
            e->frameNum = 0;
            e->pocCount = 0;
        }
        H264EncPicture picture = {
            .type = key ? H264ENC_IDR : H264ENC_P,
            .frame_num = e->frameNum,
            .poc_lsb = (2 * e->pocCount) & 0xffff,
        };
        if (plan)
            picture = plan->picture;
        picture.qp = (uint8_t)(picture.type == H264ENC_B ? rc_qp_b(&e->rc) : rc_qp(&e->rc, key));
        picture.idr_pic_id = e->idrPicId;
        picture.picture_index = e->pictureIndex++;
        const H264EncHandles handles = plan ? plan->handles : e->handles;
        const bool allowTemporal = e->allowTemporal;
        const uint32_t pocCount = e->pocCount;

        TegraNVENCSubmit *submit = nvenc_submit_buffer(e->client);
        if (!submit)
            return kVTAllocationFailedErr;
        if (hevc) {
            const HEVCEncPicture coded = {
                .idr = key, .poc = pocCount, .qp = picture.qp, .picture_index = picture.picture_index};
            hevcnvenc_fill_submit(&e->hevc, e->config.input_layout, e->info.chroma_mode, &e->layout, &coded, &handles,
                                  submit);
        } else {
            h264enc_fill_submit(&e->config, &e->layout, &picture, &handles, submit);
        }
        const AVERegion whole = {0, 0, e->config.width, e->config.height};

        pthread_mutex_unlock(&e->lock);
        bool inputRefused = false;
        const bool putOk = ave_input_put_buffer(e, image, &whole, handles.input, e->inputVA, submit, &inputRefused);
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
        uint64_t copied = began;
#endif
        int submitted = -1;
        uint32_t fence = 0, route = TEGRA_NVENC_INPUT_CPU;
        AVEJobStatus job = {0};
        bool waitedOk = false;
        if (putOk) {
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
            copied = mach_absolute_time();
#endif
            // A job that ends without writing its record must not leave the previous picture's record behind.
            memset(e->auxVA, 0, sizeof(nvenc_pic_stat_s));
            submit->clean[submit->clean_count++] = (TegraNVENCRange){handles.aux, 0, sizeof(nvenc_pic_stat_s), 0};
            submitted = ave_input_submit_raw(e->client, submit, &fence, &route);
            if (!submitted)
                waitedOk =
                    ave_wait_job(e, fence, handles.aux, e->auxVA, handles.bitstream, e->layout.bitstream_bytes, &job);
        }
        const uint32_t produced = (job.total_bit_count + 7) / 8;
        const bool nvencRefused =
            waitedOk && (job.ucode_error_status || !produced || produced > e->layout.bitstream_bytes);
        size_t size = 0;
        CMSampleBufferRef sample = NULL;
        OSStatus sampleStatus = noErr;
        if (waitedOk && !nvencRefused) {
            size = h264enc_annexb_to_avcc(e->bitstreamVA, produced, e->avcc, e->avccCap);
            if (size)
                sampleStatus =
                    buildSample(e, size, &picture, allowTemporal, pts, duration, plan ? plan->dts : pts, &sample);
        }
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
        const uint64_t finished = mach_absolute_time();
#endif

        pthread_mutex_lock(&e->lock);
        if (inputRefused)
            e->inputRefused++;
        if (putOk)
            ave_input_record_route(e, submit, submitted, route);

        if (!putOk) {
            e->errors++;
            return kVTVideoEncoderMalfunctionErr;
        }
        if (submitted || !waitedOk) {
            e->errors++;
            e->keyPending = true;
            return kVTVideoEncoderMalfunctionErr;
        }
        // NVENC 5.0 puts 2 in error_status of every status record; real errors are in ucode_error_status.
        if (nvencRefused) {
            const bool bPicture = picture.type == H264ENC_B;
            os_log_error(gLog, "AVE: NVENC %{public}s picture failed: error_status %u ucode %#x, %u bits%{public}s",
                         key        ? "IDR"
                         : bPicture ? "B"
                                    : "P",
                         job.error_status, job.ucode_error_status, job.total_bit_count,
                         key        ? ""
                         : bPicture ? "; coded again as an I picture"
                                    : "; coded again as an IDR");
            e->errors++;
            if (!bPicture)
                e->keyPending = true;
            if (key)
                return kVTVideoEncoderMalfunctionErr;
            if (plan) {
                plan->refused = true;
                return kVTVideoEncoderMalfunctionErr;
            }
            continue;  // retry the same picture as an IDR
        }
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
        e->encodeTicks += finished - copied;
        e->copyTicks += copied - began;
#endif
        if (!size) {
            e->errors++;
            e->keyPending = true;
            return kVTVideoEncoderMalfunctionErr;
        }
        if (sampleStatus)
            return sampleStatus;
        rc_update(&e->rc, (double)size, key);
        if (!plan) {
            const uint32_t recon = e->handles.recon;
            e->handles.recon = e->handles.ref;
            e->handles.ref = recon;
            e->frameNum = (e->frameNum + 1) & 0xffff;
            e->pocCount++;
        }
        e->haveRef = true;
        e->keyPending = false;
        if (key) {
            e->idrPicId = (e->idrPicId + 1) & 0xffff;
            e->sinceKey = 0;
            e->lastKeyTime = pts;
            e->keyFrames++;
        } else {
            e->sinceKey++;
        }
        e->frames++;
        e->bytes += size;
        VTVideoEncoderSession session = e->session;
        pthread_mutex_unlock(&e->lock);
        OSStatus emitStatus = noErr;
        if (session)
            emitStatus = VTEncoderSessionEmitEncodedFrame(session, frame, noErr, 0, sample);
        CFRelease(sample);
        pthread_mutex_lock(&e->lock);
        return emitStatus;
    }
    return kVTVideoEncoderMalfunctionErr;
}

OSStatus ave_encode_planned(Encoder *e, AVEPlan *plan)
{
    plan->refused = false;
    return encodePicture(e, plan->frame, plan->image, plan->pts, plan->duration, NULL, plan);
}

OSStatus ave_open_engine(Encoder *e)
{
    const CMVideoDimensions size = e->size;
    H264EncConfig probe = {
        .width = (uint32_t)size.width, .height = (uint32_t)size.height, .profile_idc = H264_PROFILE_HIGH};
    int error = 0;
    if (h264enc_configure(&probe)) {
        switch_refusal_k(switch_refusal_key_with(switch_refusal_key("AVE", "size outside NVENC's range"),
                                                 (uint64_t)(uint32_t)size.width << 32 | (uint32_t)size.height),
                         "AVE", "size outside NVENC's range",
                         "%dx%d: NVENC encodes 32..4096 a side, even sizes, up to 4096x2304", size.width, size.height);
        return kVTCouldNotFindVideoEncoderErr;
    }
    if (!(e->client = nvenc_open(&error)) || !nvenc_info(e->client, &e->info)) {
        switch_refusal("AVE", "TegraNVENC refused the process", "error %#x: no H.264 encoder here", error);
        return kVTCouldNotFindVideoEncoderErr;
    }
    if (e->codec == kCMVideoCodecType_HEVC && !(e->info.codecs & 1u << TEGRA_NVENC_CODEC_H265)) {
        switch_refusal("AVE", "this kernel's NVENC has no H.265", "no HEVC encoder here");
        return kVTCouldNotFindVideoEncoderErr;
    }
    if (e->info.features & TEGRA_NVENC_FEATURE_COMPLETION) {
        AVEEmitter *emitter = ave_emitter_create();
        if (emitter && nvenc_listen(e->client, ave_emitter_queue(emitter), ave_emitter_completed, emitter))
            e->emitter = emitter;
        else
            ave_emitter_destroy(emitter);
    }
    return noErr;
}

static OSStatus startSession(VTVideoEncoderRef ref, VTVideoEncoderSession session, CMVideoDimensions size,
                             CFTypeRef unused)
{
    (void)unused;
    Encoder *e = storage(ref);
    pthread_mutex_lock(&e->lock);
    e->session = session;
    e->size = size;
    OSStatus status = ave_open_engine(e);
    if (!status) {
        CFDictionaryRef attributes = ave_copy_input_attributes(e, size.width, size.height);
        status = VTEncoderSessionSetPixelBufferAttributes(session, attributes);
        CFRelease(attributes);
    }
    pthread_mutex_unlock(&e->lock);
    return status;
}

static OSStatus encodeFrame(VTVideoEncoderRef ref, VTVideoEncoderFrame frame, CVImageBufferRef image, CMTime pts,
                            CMTime duration, CFDictionaryRef options, VTEncodeInfoFlags *info)
{
    Encoder *e = storage(ref);
    if (info)
        *info = 0;
    pthread_mutex_lock(&e->lock);
    ave_begin_encode(e);
    OSStatus status = e->failed ? kVTVideoEncoderMalfunctionErr : noErr;
    if (!status && !e->configured && (status = ave_configure(e, image))) {
        e->failed = true;
        os_log_error(gLog, "AVE: session %dx%d in %{public}s could not start on NVENC (%d)", e->size.width,
                     e->size.height, getprogname(), (int)status);
    }
    if (!status && e->reorder.on && info)
        *info = kVTEncodeInfo_Asynchronous;
    if (!status)
        status = e->reorder.on ? ave_reorder_frame(e, frame, image, pts, duration, options)
                               : encodePicture(e, frame, image, pts, duration, options, NULL);
    VTVideoEncoderSession session = e->session;
    ave_end_encode(e);
    pthread_mutex_unlock(&e->lock);
    if (status && session)
        VTEncoderSessionEmitEncodedFrame(session, frame, status, kVTEncodeInfo_FrameDropped, NULL);
    return noErr;  // the frame was answered (with its status) above
}

// Every held frame, whatever `until` says: the last of them has to be coded as an anchor for the others anyway.
static OSStatus completeFrames(VTVideoEncoderRef ref, CMTime until)
{
    (void)until;
    Encoder *e = storage(ref);
    pthread_mutex_lock(&e->lock);
    ave_begin_encode(e);
    if (e->reorder.on)
        ave_reorder_flush(e);
    ave_end_encode(e);
    pthread_mutex_unlock(&e->lock);
    return noErr;
}

// type NULL: no type stated (dictionary and data values: clean aperture, ICC profile, HDR metadata).
static void describe(CFMutableDictionaryRef supported, CFStringRef key, CFStringRef type, bool readOnly)
{
    const void *keys[] = {kVTPropertyReadWriteStatusKey, kVTPropertyTypeKey};
    const void *values[] = {readOnly ? kVTPropertyReadWriteStatus_ReadOnly : kVTPropertyReadWriteStatus_ReadWrite,
                            type};
    CFDictionaryRef d = CFDictionaryCreate(kCFAllocatorDefault, keys, values, type ? 2 : 1,
                                           &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(supported, key, d);
    CFRelease(d);
}

static OSStatus copySupportedPropertyDictionary(VTVideoEncoderRef ref, CFDictionaryRef *out)
{
    (void)ref;
    CFMutableDictionaryRef s = CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
                                                         &kCFTypeDictionaryValueCallBacks);
    if (!s)
        return kVTAllocationFailedErr;
    const CFStringRef numbers[] = {
        kVTCompressionPropertyKey_AverageBitRate,
        kVTCompressionPropertyKey_ConstantBitRate,
        kVTCompressionPropertyKey_Quality,
        kVTCompressionPropertyKey_MaxKeyFrameInterval,
        kVTCompressionPropertyKey_MaxKeyFrameIntervalDuration,
        kVTCompressionPropertyKey_ExpectedFrameRate,
        kVTCompressionPropertyKey_MaxFrameDelayCount,
        kVTCompressionPropertyKey_SourceFrameCount,
        kVTCompressionPropertyKey_ExpectedDuration,
    };
    const CFStringRef booleans[] = {
        kVTCompressionPropertyKey_AllowTemporalCompression,
        kVTCompressionPropertyKey_AllowFrameReordering,
        kVTCompressionPropertyKey_RealTime,
        kVTCompressionPropertyKey_AllowOpenGOP,
        kVTCompressionPropertyKey_MaximizePowerEfficiency,
        kVTCompressionPropertyKey_PrioritizeEncodingSpeedOverQuality,
    };
    const CFStringRef enumerations[] = {
        kVTCompressionPropertyKey_ProfileLevel,   kVTCompressionPropertyKey_H264EntropyMode,
        kVTCompressionPropertyKey_ColorPrimaries, kVTCompressionPropertyKey_TransferFunction,
        kVTCompressionPropertyKey_YCbCrMatrix,
    };
    for (unsigned i = 0; i < sizeof(numbers) / sizeof(numbers[0]); i++)
        describe(s, numbers[i], kVTPropertyType_Number, false);
    for (unsigned i = 0; i < sizeof(booleans) / sizeof(booleans[0]); i++)
        describe(s, booleans[i], kVTPropertyType_Boolean, false);
    for (unsigned i = 0; i < sizeof(enumerations) / sizeof(enumerations[0]); i++)
        describe(s, enumerations[i], kVTPropertyType_Enumeration, false);
    const CFStringRef metadata[] = {
        kVTCompressionPropertyKey_PixelAspectRatio,
        kVTCompressionPropertyKey_CleanAperture,
        kVTCompressionPropertyKey_FieldCount,
        kVTCompressionPropertyKey_FieldDetail,
        kVTCompressionPropertyKey_AspectRatio16x9,
        kVTCompressionPropertyKey_ProgressiveScan,
        kVTCompressionPropertyKey_ICCProfile,
        kVTCompressionPropertyKey_GammaLevel,
        kVTCompressionPropertyKey_Depth,
        kVTCompressionPropertyKey_MoreFramesBeforeStart,
        kVTCompressionPropertyKey_MoreFramesAfterEnd,
        kVTCompressionPropertyKey_MasteringDisplayColorVolume,
        kVTCompressionPropertyKey_ContentLightLevelInfo,
        kVTCompressionPropertyKey_HDRMetadataInsertionMode,
    };
    for (unsigned i = 0; i < sizeof(metadata) / sizeof(metadata[0]); i++)
        describe(s, metadata[i], NULL, false);
    describe(s, kVTCompressionPropertyKey_DataRateLimits, kVTPropertyType_Number, false);
    describe(s, kVTCompressionPropertyKey_UsingHardwareAcceleratedVideoEncoder, kVTPropertyType_Boolean, true);
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    describe(s, kSwitchEncoderRoutesKey, NULL, true);
    describe(s, kSwitchEncoderLayoutProbeKey, NULL, true);
#endif
    *out = s;
    return noErr;
}

static OSStatus setProperty(CMBaseObjectRef ref, CFStringRef key, CFTypeRef value)
{
    Encoder *e = storage(ref);
    double v = 0;
    OSStatus status = noErr;
    pthread_mutex_lock(&e->lock);
    if (CFEqual(key, kVTCompressionPropertyKey_AverageBitRate)) {
        e->averageBitRate = number(value, &v) ? (int32_t)v : 0;
        if (e->configured && e->constantBitRate <= 0) {
            e->rc.bitrate = e->averageBitRate;
            e->rc.buffer = e->averageBitRate;
            e->rc.constQP = e->averageBitRate > 0 ? -1 : e->rc.constQP;
        }
    } else if (CFEqual(key, kVTCompressionPropertyKey_ConstantBitRate)) {
        e->constantBitRate = number(value, &v) ? (int32_t)v : 0;
    } else if (CFEqual(key, kVTCompressionPropertyKey_Quality)) {
        e->quality = number(value, &v) ? v : -1;
    } else if (CFEqual(key, kVTCompressionPropertyKey_MaxKeyFrameInterval)) {
        e->maxKeyInterval = number(value, &v) ? (int32_t)v : 0;
    } else if (CFEqual(key, kVTCompressionPropertyKey_MaxKeyFrameIntervalDuration)) {
        e->maxKeyDuration = number(value, &v) ? v : 0;
    } else if (CFEqual(key, kVTCompressionPropertyKey_ExpectedFrameRate)) {
        e->expectedFrameRate = number(value, &v) ? v : 0;
    } else if (CFEqual(key, kVTCompressionPropertyKey_AllowTemporalCompression)) {
        e->allowTemporal = !value || CFGetTypeID(value) != CFBooleanGetTypeID() || CFBooleanGetValue(value);
    } else if (CFEqual(key, kVTCompressionPropertyKey_RealTime)) {
        e->realTime = value && CFGetTypeID(value) == CFBooleanGetTypeID() && CFBooleanGetValue(value);
    } else if (CFEqual(key, kVTCompressionPropertyKey_PrioritizeEncodingSpeedOverQuality)) {
        e->prioritizeSpeed = value && CFGetTypeID(value) == CFBooleanGetTypeID() && CFBooleanGetValue(value);
    } else if (CFEqual(key, kVTCompressionPropertyKey_DataRateLimits)) {
        // [bytes, seconds, ...]: the first pair is enforced.
        double bytes = 0, seconds = 0;
        if (value && CFGetTypeID(value) == CFArrayGetTypeID() && CFArrayGetCount(value) >= 2 &&
            number(CFArrayGetValueAtIndex(value, 0), &bytes) && number(CFArrayGetValueAtIndex(value, 1), &seconds)) {
            e->limitBytes = bytes;
            e->limitSeconds = seconds;
            if (e->configured)
                rc_set_limit(&e->rc, bytes, seconds);
        }
    } else if (CFEqual(key, kVTCompressionPropertyKey_ProfileLevel)) {
        if (value && CFGetTypeID(value) == CFStringGetTypeID() &&
            (e->codec == kCMVideoCodecType_HEVC
                 ? CFStringHasPrefix(value, CFSTR("HEVC_Main_")) || CFStringHasPrefix(value, CFSTR("HEVC_MainStill_"))
                 : CFStringHasPrefix(value, CFSTR("H264_"))))
            replace((CFTypeRef *)&e->profileLevel, value);
        else
            status = kVTPropertyNotSupportedErr;
    } else if (CFEqual(key, kVTCompressionPropertyKey_H264EntropyMode)) {
        replace((CFTypeRef *)&e->entropy, value);
    } else if (CFEqual(key, kVTCompressionPropertyKey_ColorPrimaries)) {
        replace((CFTypeRef *)&e->primaries, value);
    } else if (CFEqual(key, kVTCompressionPropertyKey_TransferFunction)) {
        replace((CFTypeRef *)&e->transfer, value);
    } else if (CFEqual(key, kVTCompressionPropertyKey_YCbCrMatrix)) {
        replace((CFTypeRef *)&e->matrix, value);
    } else if (CFEqual(key, kVTCompressionPropertyKey_AllowFrameReordering)) {
        e->allowReordering = !value || CFGetTypeID(value) != CFBooleanGetTypeID() || CFBooleanGetValue(value);
    } else if (CFEqual(key, kVTCompressionPropertyKey_MaxFrameDelayCount)) {
        e->maxFrameDelay = number(value, &v) ? (int32_t)v : -1;
    } else if (CFEqual(key, kVTCompressionPropertyKey_AllowOpenGOP) ||
               CFEqual(key, kVTCompressionPropertyKey_MaximizePowerEfficiency) ||
               CFEqual(key, kVTCompressionPropertyKey_SourceFrameCount) ||
               CFEqual(key, kVTCompressionPropertyKey_ExpectedDuration)) {
        // Accepted: closed GOPs whatever is asked; nothing to tune on a fixed-function engine.
    } else if (CFEqual(key, kVTCompressionPropertyKey_PixelAspectRatio) ||
               CFEqual(key, kVTCompressionPropertyKey_CleanAperture) ||
               CFEqual(key, kVTCompressionPropertyKey_FieldCount) ||
               CFEqual(key, kVTCompressionPropertyKey_FieldDetail) ||
               CFEqual(key, kVTCompressionPropertyKey_AspectRatio16x9) ||
               CFEqual(key, kVTCompressionPropertyKey_ProgressiveScan) ||
               CFEqual(key, kVTCompressionPropertyKey_ICCProfile) ||
               CFEqual(key, kVTCompressionPropertyKey_GammaLevel) || CFEqual(key, kVTCompressionPropertyKey_Depth) ||
               CFEqual(key, kVTCompressionPropertyKey_MoreFramesBeforeStart) ||
               CFEqual(key, kVTCompressionPropertyKey_MoreFramesAfterEnd) ||
               CFEqual(key, kVTCompressionPropertyKey_MasteringDisplayColorVolume) ||
               CFEqual(key, kVTCompressionPropertyKey_ContentLightLevelInfo) ||
               CFEqual(key, kVTCompressionPropertyKey_HDRMetadataInsertionMode)) {
        // Accepted and left to the format description VideoToolbox builds around ours: geometry and colour metadata
        // that change no slice (progressive, square-sample 8-bit streams only).
    } else {
        status = kVTPropertyNotSupportedErr;
        char name[96] = "";
        CFStringGetCString(key, name, sizeof(name), kCFStringEncodingUTF8);
        switch_refusal_k(switch_refusal_key_with(switch_refusal_key("AVE", "property not supported"), CFHash(key)),
                         "AVE", "property not supported", "%s", name);
    }
    pthread_mutex_unlock(&e->lock);
    return status;
}

static OSStatus copyProperty(CMBaseObjectRef ref, CFStringRef key, CFAllocatorRef allocator, void *value)
{
    (void)allocator;
    Encoder *e = storage(ref);
    if (CFEqual(key, kVTCompressionPropertyKey_UsingHardwareAcceleratedVideoEncoder)) {
        *(CFTypeRef *)value = CFRetain(kCFBooleanTrue);
        return noErr;
    }
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    if (CFEqual(key, kSwitchEncoderLayoutProbeKey)) {
        *(CFTypeRef *)value = ave_copy_layout_probe(e);
        return *(CFTypeRef *)value ? noErr : -12783;
    }
#endif
    OSStatus status = -12783;  // kCMBaseObjectError_ValueNotAvailable
    pthread_mutex_lock(&e->lock);
    if (CFEqual(key, kVTCompressionPropertyKey_AverageBitRate)) {
        *(CFTypeRef *)value = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &e->averageBitRate);
        status = noErr;
    } else if (CFEqual(key, kVTCompressionPropertyKey_MaxKeyFrameInterval)) {
        *(CFTypeRef *)value = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &e->maxKeyInterval);
        status = noErr;
    } else if (CFEqual(key, kVTCompressionPropertyKey_AllowTemporalCompression)) {
        *(CFTypeRef *)value = CFRetain(e->allowTemporal ? kCFBooleanTrue : kCFBooleanFalse);
        status = noErr;
    } else if (CFEqual(key, kVTCompressionPropertyKey_RealTime)) {
        *(CFTypeRef *)value = CFRetain(e->realTime ? kCFBooleanTrue : kCFBooleanFalse);
        status = noErr;
    } else if (CFEqual(key, kVTCompressionPropertyKey_PrioritizeEncodingSpeedOverQuality)) {
        *(CFTypeRef *)value = CFRetain(e->prioritizeSpeed ? kCFBooleanTrue : kCFBooleanFalse);
        status = noErr;
    } else if (CFEqual(key, kVTCompressionPropertyKey_AllowFrameReordering)) {
        *(CFTypeRef *)value = CFRetain(e->allowReordering ? kCFBooleanTrue : kCFBooleanFalse);
        status = noErr;
    } else if (CFEqual(key, kVTCompressionPropertyKey_MaxFrameDelayCount)) {
        *(CFTypeRef *)value = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &e->maxFrameDelay);
        status = noErr;
    } else if (CFEqual(key, kVTCompressionPropertyKey_ProfileLevel) && e->profileLevel) {
        *(CFTypeRef *)value = CFRetain(e->profileLevel);
        status = noErr;
    }
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    else if (CFEqual(key, kSwitchEncoderRoutesKey)) {
        *(CFTypeRef *)value = ave_copy_routes(e);
        status = *(CFTypeRef *)value ? noErr : kVTAllocationFailedErr;
    }
#endif
    pthread_mutex_unlock(&e->lock);
    return status;
}

static OSStatus invalidate(CMBaseObjectRef ref)
{
    Encoder *e = storage(ref);
    pthread_mutex_lock(&e->lock);
    e->session = NULL;
    e->tileSession = NULL;
    AVEEmitter *emitter = e->emitter;
    pthread_mutex_unlock(&e->lock);
    if (emitter)
        ave_emitter_close(emitter);
    return noErr;
}

static void finalize(CMBaseObjectRef ref)
{
    Encoder *e = storage(ref);
    if (e->client)
        nvenc_stop_listening(e->client);
    ave_emitter_destroy(e->emitter);
    e->emitter = NULL;
    if (e->frames)
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
        os_log(
            gLog,
            "AVE: session %ux%u in %{public}s ended: %u pictures on NVENC (%u key), %u failed, %llu bytes, inputs "
            "%u read in place, %u by VIC (%u through the copy engine), %u copied, %u VIC could not take, mean %.2f ms "
            "input + %.2f ms encode",
            e->config.width, e->config.height, getprogname(), e->frames, e->keyFrames, e->errors,
            (unsigned long long)e->bytes, e->inputDirect, e->inputVIC, e->inputBounced, e->inputCopies, e->inputRefused,
            ticksToMs(e->copyTicks) / e->frames, ticksToMs(e->encodeTicks) / e->frames);
#else
        os_log(gLog,
               "AVE: session %ux%u in %{public}s ended: %u pictures on NVENC (%u key), %u failed, %llu bytes, inputs "
               "%u read in place, %u by VIC (%u through the copy engine), %u copied, %u VIC could not take",
               e->config.width, e->config.height, getprogname(), e->frames, e->keyFrames, e->errors,
               (unsigned long long)e->bytes, e->inputDirect, e->inputVIC, e->inputBounced, e->inputCopies,
               e->inputRefused);
#endif
    ave_reorder_release(e);
    if (e->client) {
        releaseBuffers(e);
        nvenc_close(e->client);  // the kernel waits for this connection's jobs
    }
    free(e->avcc);
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    CVPixelBufferRelease(e->probeSource);
#endif
    if (e->format)
        CFRelease(e->format);
    CFTypeRef *strings[] = {(CFTypeRef *)&e->profileLevel, (CFTypeRef *)&e->entropy, (CFTypeRef *)&e->primaries,
                            (CFTypeRef *)&e->transfer, (CFTypeRef *)&e->matrix};
    for (unsigned i = 0; i < 5; i++)
        replace(strings[i], NULL);
    pthread_cond_destroy(&e->busyCond);
    pthread_mutex_destroy(&e->lock);
}

static CFStringRef copyDebugDescription(CMBaseObjectRef ref)
{
    return CFStringCreateWithFormat(kCFAllocatorDefault, NULL, CFSTR("<AVE H.264 encoder %p on NVENC>"), ref);
}

static const VTPluginBaseClass kBaseClass = {
    .version = 1,
    .derivedStorageSize = sizeof(Encoder),
    .invalidate = invalidate,
    .finalize = finalize,
    .copyDebugDescription = copyDebugDescription,
    .copyProperty = copyProperty,
    .setProperty = setProperty,
};

// Version 3 for the tile slots (VTEncoderSPI.h). VideoToolbox checks every version 2 and 3 slot left empty here for
// NULL before calling it, and reads +0xa8 from version 4 only (vtCompressionSessionCompressionWork, 21H461 and 24B119).
static const VTPluginEncoderClass kEncoderClass = {
    .version = 3,
    .startSession = startSession,
    .encodeFrame = encodeFrame,
    .completeFrames = completeFrames,
    .copySupportedPropertyDictionary = copySupportedPropertyDictionary,
    .startTileSession = ave_start_tile_session,
    .prepareToEncodeTiles = ave_prepare_to_encode_tiles,
    .encodeTile = ave_encode_tile,
    .completeTiles = ave_complete_tiles,
};

static const VTPluginEncoderVTable kVTable = {NULL, &kBaseClass, &kEncoderClass};

static OSStatus createInstance(FourCharCode codec, CFAllocatorRef allocator, VTVideoEncoderRef *out)
{
    if (codec != kCMVideoCodecType_H264 && codec != kCMVideoCodecType_HEVC)
        return kVTCouldNotFindVideoEncoderErr;
    CMBaseObjectRef object = NULL;
    const OSStatus status = CMDerivedObjectCreate(allocator, &kVTable, VTVideoEncoderGetClassID(), &object);
    if (status)
        return status;
    Encoder *e = storage(object);
    memset(e, 0, sizeof(*e));
    pthread_mutex_init(&e->lock, NULL);
    pthread_cond_init(&e->busyCond, NULL);
    e->codec = codec;
    e->allowTemporal = true;
    e->allowReordering = true;
    e->maxFrameDelay = -1;
    e->quality = -1;
    e->lastKeyTime = kCMTimeInvalid;
    *out = object;
    return noErr;
}

static OSStatus registerEncoder(CMVideoCodecType codec, CFStringRef encoderID, CFStringRef name)
{
    const void *keys[] = {CFSTR("CMClassImplementationID"), kVTVideoEncoder_IsHardwareAccelerated,
                          kVTVideoEncoder_EncoderNameKey};
    const void *values[] = {encoderID, kCFBooleanTrue, name};
    CFDictionaryRef info =
        CFDictionaryCreate(NULL, keys, values, 3, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    const OSStatus status = VTRegisterVideoEncoderWithInfo(codec, info, createInstance);
    CFRelease(info);
    return status;
}

__attribute__((visibility("default"))) void H264H7EncoderRegister(void);

void H264H7EncoderRegister(void)
{
    gLog = os_log_create("com.brunostjohn.switch.ave", "encoder");
    // The registry is filled once per process: one filled before TegraNVENC publishes waits for it.
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    const uint64_t began = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
#endif
    const bool published = nvenc_service_wait();
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    const uint64_t waitedMs = (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - began) / 1000000;
    if (waitedMs)
        os_log(gLog, "AVE: %{public}s waited %llu ms for TegraNVENC: %{public}s", getprogname(), waitedMs,
               published ? "published" : "absent");
#endif
    if (!published)
        return;  // switch-no-nvdec, or NVENC did not prove itself at boot: nothing changes
    OSStatus status = registerEncoder(kCMVideoCodecType_H264, CFSTR(AVE_ENCODER_ID), CFSTR("Apple H.264 (HW)"));
    if (status)
        os_log_error(gLog, "AVE: VTRegisterVideoEncoderWithInfo('avc1') failed: %d", (int)status);
    // Apple's own HEVC registration (ave.videoencoder AVE_Plugin_HEVC_Register, 24B119 0x2c46d3e3c): its encoder ID
    // and the hardware flag; VideoToolbox's 'muxa', depth and disparity wrappers take whatever 'hvc1' is registered.
    // A kernel without H.265 refuses the session at StartSession (TegraNVENCInfo.codecs).
    status = registerEncoder(kCMVideoCodecType_HEVC, CFSTR(AVE_HEVC_ENCODER_ID), CFSTR("Apple HEVC (HW)"));
    if (status)
        os_log_error(gLog, "AVE: VTRegisterVideoEncoderWithInfo('hvc1') failed: %d", (int)status);
}
