// SPDX-License-Identifier: BSD-2-Clause-Patent
// AVD.videodecoder's MPEG sessions.
//
// 'mp4v': the video object layer comes from the format description's 'esds' DecoderSpecificInfo (ISO/IEC 14496-14
// MP4VisualSampleEntry, as AVFoundation's readers and FFmpeg's VideoToolbox hwaccel describe MPEG-4 Part 2). A stream
// NVDEC does not take (mpeg4_vol_unsupported: interlaced, GMC, data partitioning, ..., or no VOL to decide by) runs on
// Apple's software decoder, VCPMP4V.videodecoder, inside this instance, as H.264 does.
//
// 'mp2v' and 'mp1v': MediaToolbox's MPEG-2 program and transport stream readers (M2VPushForType) and CoreMedia's MP4
// bridge (objectTypeIndication 0x60, 0x61, 0x65 and 0x6a) make them; iOS has no decoder of its own for them, so a
// session NVDEC cannot take (4:2:2, beyond 4080x4080) fails here as it fails without this plug-in. The sequence
// header is in the samples; an 'esds' DecoderSpecificInfo holding one is read first when there is one.
#include "AVDDecoder.h"
#include "SwitchVideoColor.h"

OSStatus avd_start_mpeg4(Decoder *d, VTVideoDecoderSession session, CMVideoFormatDescriptionRef format)
{
    d->avcC = avd_copy_atom(format, CFSTR("esds"));
    MPEG4VOL vol = {0};
    uint8_t objectType = 0;
    const uint8_t *dsi = NULL;
    size_t dsiSize = 0;
    const bool esds =
        d->avcC && mpeg4_esds(CFDataGetBytePtr(d->avcC), (size_t)CFDataGetLength(d->avcC), &objectType, &dsi, &dsiSize);
    const int parsed = esds && dsi ? mpeg4_parse_config(&vol, dsi, dsiSize) : 0;
    const char *why = !esds || objectType != 0x20 ? "no MPEG-4 Visual 'esds' to start from"
                      : parsed < 0                ? "a malformed video object layer"
                      : parsed == 0               ? "no video object layer in the format description"
                                                  : mpeg4_vol_unsupported(&vol);
    if (!why && mpeg4_decoder_configure(d->mpeg4, &vol))
        why = "no NVDEC memory for its buffers (see the AVD alloc line)";
    if (why) {
        mpeg4_decoder_destroy(d->mpeg4);
        nvdec_close(d->client);
        d->mpeg4 = NULL;
        d->client = NULL;
        return avd_start_software(d, session, format, why);
    }
    d->width = vol.width;
    d->height = vol.height;
    const bool vui = vol.colour_description;
    d->matrix =
        switch_video_color_tag(format, kCMFormatDescriptionExtension_YCbCrMatrix, vui ? vol.matrix : 2, 0, d->height);
    d->primaries = switch_video_color_tag(format, kCMFormatDescriptionExtension_ColorPrimaries,
                                          vui ? vol.colour_primaries : 2, 1, 0);
    d->transfer =
        switch_video_color_tag(format, kCMFormatDescriptionExtension_TransferFunction, vui ? vol.transfer : 2, 2, 0);
    OSStatus status;
    avd_set_attributes(d, session, vol.full_range, false, &status);
    if (!status)
        os_log(
            gAVDLog,
            "AVD: session %ux%u in %{public}s on NVDEC (MPEG-4 Part 2 object type %u, profile and level %#x%{public}s"
            "%{public}s), "
            "pictures out by %{public}s",
            d->width, d->height, getprogname(), vol.object_type, vol.profile_level,
            vol.quarter_sample ? ", quarter sample" : "", vol.quant_type ? ", MPEG quantisation" : "",
            d->present ? "VIC" : "the CPU");
    return status;
}

OSStatus avd_start_mpeg2(Decoder *d, VTVideoDecoderSession session, CMVideoFormatDescriptionRef format)
{
    const CMVideoDimensions size = CMVideoFormatDescriptionGetDimensions(format);
    d->avcC = avd_copy_atom(format, CFSTR("esds"));
    MPEG2Sequence seq = {0};
    uint8_t objectType = 0;
    const uint8_t *dsi = NULL;
    size_t dsiSize = 0;
    const int parsed =
        d->avcC &&
                mpeg4_esds(CFDataGetBytePtr(d->avcC), (size_t)CFDataGetLength(d->avcC), &objectType, &dsi, &dsiSize) &&
                dsi
            ? mpeg2_parse_config(&seq, dsi, dsiSize)
            : 0;
    if (!parsed) {
        seq.valid = true;
        seq.mpeg2 = d->codec == kCMVideoCodecType_MPEG2Video;
        seq.chroma_format = 1;
        seq.width = (uint16_t)size.width;
        seq.height = (uint16_t)size.height;
    }
    const char *why = parsed < 0 ? "a malformed sequence header" : mpeg2_sequence_unsupported(&seq);
    const int configured = why      ? 0
                           : parsed ? mpeg2_decoder_set_sequence(d->mpeg2, &seq)
                                    : mpeg2_decoder_configure(d->mpeg2, seq.width, seq.height);
    if (why || configured) {
        os_log_error(gAVDLog, "AVD: %{public}s session %dx%d in %{public}s refused: %{public}s",
                     avd_codec_name(d->codec), size.width, size.height, getprogname(),
                     why ? why : "no NVDEC memory for its buffers (see the AVD alloc line)");
        return why ? kVTVideoDecoderUnsupportedDataFormatErr : kVTVideoDecoderMalfunctionErr;
    }
    d->width = seq.width;
    d->height = seq.height;
    const bool vui = parsed && seq.colour_description;
    d->matrix =
        switch_video_color_tag(format, kCMFormatDescriptionExtension_YCbCrMatrix, vui ? seq.matrix : 2, 0, d->height);
    d->primaries = switch_video_color_tag(format, kCMFormatDescriptionExtension_ColorPrimaries,
                                          vui ? seq.colour_primaries : 2, 1, 0);
    d->transfer =
        switch_video_color_tag(format, kCMFormatDescriptionExtension_TransferFunction, vui ? seq.transfer : 2, 2, 0);
    // H.273 chroma sample location: MPEG-2's 4:2:0 chroma is co-sited with luma horizontally (left), MPEG-1's centred.
    const uint32_t chroma = seq.mpeg2 ? 0 : 1;
    d->chromaTop = switch_video_chroma_tag(format, kCMFormatDescriptionExtension_ChromaLocationTopField, true, chroma);
    d->chromaBottom =
        switch_video_chroma_tag(format, kCMFormatDescriptionExtension_ChromaLocationBottomField, true, chroma);
    OSStatus status;
    avd_set_attributes(d, session, false, false, &status);
    if (!status)
        os_log(gAVDLog, "AVD: session %ux%u in %{public}s on NVDEC (%{public}s%{public}s), pictures out by %{public}s",
               d->width, d->height, getprogname(), avd_codec_name(d->codec),
               parsed ? (seq.progressive_sequence ? ", progressive" : ", interlaced") : "",
               d->present ? "VIC" : "the CPU");
    return status;
}

// A new format keeps the session when its size and 'esds' (or the lack of one) are the same.
Boolean avd_mpeg2_accepts(const Decoder *d, CMVideoFormatDescriptionRef format)
{
    const CMVideoDimensions size = CMVideoFormatDescriptionGetDimensions(format);
    CFDataRef esds = avd_copy_atom(format, CFSTR("esds"));
    const Boolean same = (uint32_t)size.width == d->width && (uint32_t)size.height == d->height &&
                         (esds == d->avcC || (esds && d->avcC && CFEqual(esds, d->avcC)));
    if (esds)
        CFRelease(esds);
    return same;
}

// As Apple's decoders tag interlaced pictures: two fields, in the order they were captured.
void avd_tag_fields(CVPixelBufferRef image, uint8_t fields)
{
    const int32_t count = fields == AVD_FIELDS_PROGRESSIVE ? 1 : 2;
    CFNumberRef number = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &count);
    CVBufferSetAttachment(image, kCVImageBufferFieldCountKey, number, kCVAttachmentMode_ShouldPropagate);
    CFRelease(number);
    if (count == 1)
        CVBufferRemoveAttachment(image, kCVImageBufferFieldDetailKey);
    else
        CVBufferSetAttachment(image, kCVImageBufferFieldDetailKey,
                              fields == AVD_FIELDS_TOP_FIRST ? kCVImageBufferFieldDetailTemporalTopFirst
                                                             : kCVImageBufferFieldDetailTemporalBottomFirst,
                              kCVAttachmentMode_ShouldPropagate);
}
