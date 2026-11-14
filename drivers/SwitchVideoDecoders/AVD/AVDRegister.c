// SPDX-License-Identifier: BSD-2-Clause-Patent
// AVDRegister: AVD.videodecoder's entries in a process's VideoToolbox registry.
//
// VideoToolbox fills its registry once per process (VTLoadVideoDecoders): it dlopens a fixed list of paths and calls
// each one's register function. This file sits in the AVDRegister slot that the IPSW's dyld cache leaves empty
// (overlay/manifest.json). On 21H461 that is /System/Library/VideoDecoders/AVD.videodecoder, because iPad7,1 ships no
// such file. 24B119 ships its own AVD.videodecoder in the dyld cache, and dyld maps that one for the path whatever is
// on disk. Apple's AVD looks for an "AppleAVD" service ("AVCSupported" support bits), finds none and registers nothing.
// The same list then dlopens /System/Library/VideoDecoders/AVD.videodecoder_ps, which is not in the cache.
//
// Each entry carries kVTVideoDecoder_IsHardwareAccelerated = true, as Apple's own plug-ins state theirs (H264SW's
// VCPAVCRegisterDecoder registers with it false). 24B119's registry counts an entry without the key as software
// (_vtFilterRegistryItemByCodecTypeAndVideoDecoderSpecification), so VTIsHardwareDecodeSupported and
// RequireHardwareAcceleratedVideoDecoder would pass it over. 21H461 defaulted the key to true.
//
// Where the entries go: VTIsHardwareDecodeSupported asks this process's registry, but a session may decode elsewhere.
// With CoreMedia's SeparateCodecProcess on (24B119), VTDecompressionSessionCreateWithOptions decodes in videocodecd,
// unless the process is videocodecd or is entitled com.apple.videotoolbox.decode-in-process. A process whose sessions
// go to videocodecd (granted the service) registers the codecs TegraNVDEC's service names, found by matching because
// an app's sandbox reads only the property names its profile lists. A process that decodes itself registers only if
// TegraNVDEC lets it open a client (a probe connection, closed at once): registering where the sandbox refuses would
// make VTIsHardwareDecodeSupported answer yes for a decoder that cannot run there.
//
// The registry is filled once per process, so a process that fills it before TegraNVDEC publishes (a daemon started
// early in boot) waits for the service, or for the kernel's word that it will not come, before deciding.
#include <dlfcn.h>
#include <stdlib.h>
#include <time.h>
#include "AVDDecoder.h"

typedef struct __SecTask *SecTaskRef;
extern SecTaskRef SecTaskCreateFromSelf(CFAllocatorRef allocator);
extern CFTypeRef SecTaskCopyValueForEntitlement(SecTaskRef task, CFStringRef entitlement, CFErrorRef *error);

static bool entitledToDecodeInProcess(void)
{
    SecTaskRef task = SecTaskCreateFromSelf(kCFAllocatorDefault);
    CFTypeRef value =
        task ? SecTaskCopyValueForEntitlement(task, CFSTR("com.apple.videotoolbox.decode-in-process"), NULL) : NULL;
    const bool entitled = value == kCFBooleanTrue;
    if (value)
        CFRelease(value);
    if (task)
        CFRelease(task);
    return entitled;
}

// VideoToolbox's and CoreMedia's own answers, looked up because 21H461 and macOS do not export both.
static bool decodesInCodecServer(void)
{
    Boolean (*separate)(void) =
        (Boolean (*)(void))dlsym(RTLD_DEFAULT, "VTVideoCodecService_ShouldUseSeparateCodecProcessForDecode");
    Boolean (*isCodecServer)(void) = (Boolean (*)(void))dlsym(RTLD_DEFAULT, "FigServer_IsVideocodecd");
    return separate && isCodecServer && separate() && !isCodecServer() && !entitledToDecodeInProcess();
}

static void registerCodec(FourCharCode codec, int32_t rating)
{
    CFNumberRef number = CFNumberCreate(NULL, kCFNumberSInt32Type, &rating);
    const void *keys[] = {kVTVideoDecoder_IsHardwareAccelerated, kVTVideoDecoderInfo_Rating};
    const void *values[] = {kCFBooleanTrue, number};
    CFDictionaryRef info =
        CFDictionaryCreate(NULL, keys, values, 2, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    const OSStatus status = VTRegisterVideoDecoderWithInfo(codec, info, avd_create_instance);
    CFRelease(info);
    CFRelease(number);
    if (status)
        os_log_error(gAVDLog, "AVD: VTRegisterVideoDecoderWithInfo('%c%c%c%c') failed: %d", (char)(codec >> 24),
                     (char)(codec >> 16), (char)(codec >> 8), (char)codec, (int)status);
}

__attribute__((visibility("default"))) void AVDRegister(void);

void AVDRegister(void)
{
    gAVDLog = os_log_create("com.brunostjohn.switch.avd", "decoder");
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    const uint64_t began = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
#endif
    const bool published = nvdec_service_wait();
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    const uint64_t waitedMs = (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - began) / 1000000;
    if (waitedMs)
        os_log(gAVDLog, "AVD: %{public}s waited %llu ms for TegraNVDEC: %{public}s", getprogname(), waitedMs,
               published ? "published" : "absent");
#endif
    if (!published)
        return;  // switch-no-nvdec, or NVDEC did not come up at boot: nothing changes
    uint32_t codecs = 0;
    if (decodesInCodecServer()) {
        codecs = nvdec_service_codecs();
        if (!codecs) {
            switch_refusal("AVD", "TegraNVDEC matches no codec here",
                           "this process's sessions decode in videocodecd but it is told no hardware decoder exists");
            return;
        }
    } else {
        int error = 0;
        NVDECClient *probe = nvdec_open(&error);
        if (!probe) {
            switch_refusal("AVD", "TegraNVDEC refused the process",
                           "error %#x: H.264 stays on Apple's software decoder", error);
            return;
        }
        codecs = nvdec_codecs(probe);
        nvdec_close(probe);
    }
    if (codecs & (1u << TEGRA_NVDEC_CODEC_H264))
        registerCodec(kCMVideoCodecType_H264, 0);
    if (codecs & (1u << TEGRA_NVDEC_CODEC_VP9))
        registerCodec(kCMVideoCodecType_VP9, 0);
    if (codecs & (1u << TEGRA_NVDEC_CODEC_MPEG4))
        registerCodec(kCMVideoCodecType_MPEG4Video, 0);
    if (codecs & (1u << TEGRA_NVDEC_CODEC_MPEG12)) {
        registerCodec(kCMVideoCodecType_MPEG2Video, 0);
        registerCodec(kCMVideoCodecType_MPEG1Video, 0);
    }
    // Rated above the default 0: the registry sorts by rating before the hardware flag, so NVDEC's 'hvc1' is chosen
    // before HEVCH8's stand-in (Apple's software decoder under the hardware flag). The stand-in stays registered behind
    // it for the streams avd_load_hvcc hands to software and the tile sessions avd_start_tile_session refuses.
    if (codecs & (1u << TEGRA_NVDEC_CODEC_HEVC))
        registerCodec(kCMVideoCodecType_HEVC, 1);
}
