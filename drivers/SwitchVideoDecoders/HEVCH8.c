// SPDX-License-Identifier: BSD-2-Clause-Patent
// HEVCH8.videodecoder: Apple's own software HEVC decoder, registered in VideoToolbox's hardware slot for still images.
//
// How 21H461 decodes HEIC/HEVC on iPad7,1 (disassembly of the dyld cache):
//   ImageIO HEIFReadPlugin -> CMPhoto (dlsym'd) -> VTDecompressionSession / VTTileDecompressionSession -> a VideoToolbox
//   decoder for 'hvc1'. VideoToolbox fills its decoder registry once per process (vtInitializeVideoDecoderRegistry ->
//   VTLoadVideoDecoders), which dlopens a fixed table of paths and calls each one's register function:
//     /System/Library/VideoDecoders/H264H8.videodecoder  H264H8Register: registers the A10X hardware H.264 and HEVC
//         decoders only when IOServiceMatching("AppleD5500") finds the VXD393 kext's service. There is none here.
//     /System/Library/VideoDecoders/HEVCH8.videodecoder  HEVCH8Register: a slot this SoC does not ship (the path is
//         not in the dyld cache), so dlopen looks on disk. This file sits there through the userland overlay.
//     /System/Library/VideoCodecs/VCPHEVC.videocodec   VCPHEVCRegisterDecoder: always registers the software decoder
//         HEVCVideoDecoder_CreateInstance for 'hvc1' with kVTVideoDecoder_IsHardwareAccelerated = false.
//   With no options, ImageIO asks CMPhoto for AccelerationMode 0 (auto) and VideoToolbox falls back to the software
//   decoder by itself. But kCGImageSourceUseHardwareAcceleration = true (QuickLook previews, PhotoLibraryServices
//   thumbnail rebuilds) becomes AccelerationMode 2, CMPhoto's RequireHardwareCodec, and
//   kVTVideoDecoderSpecification_RequireHardwareAcceleratedVideoDecoder: those decodes fail without a hardware entry.
//
// So this registers the same software decoder again with kVTVideoDecoder_IsHardwareAccelerated true (which 21H461
// assumed for an entry without the key and 24B119 does not), and those still-image decodes find a decoder (Apple's
// NEON decoder on the CPU, tile decoding included). No Apple binary is patched. HEVCVideoDecoder_CreateInstance is
// exported but not in the SDK, so it and VideoToolbox's registration come from dlsym.
//
// The entry is a stand-in, and VTIsHardwareDecodeSupported('hvc1') answers from the registry alone, so it would tell
// every process that HEVC decodes in hardware. Processes that choose streaming codecs from that answer (Safari's WebKit
// processes for MSE, MediaCapabilities and HLS; mediaplaybackd and mediaserverd for AVPlayer's HLS variants;
// videocodecd) get no entry here.
//
// Where NVDEC decodes HEVC (TegraNVDEC is open to the process),
// AVD.videodecoder registers its own 'hvc1' with kVTVideoDecoderInfo_Rating 1, which VideoToolbox's registry ranks
// before this entry (rating 0): sessions run on NVDEC, in streaming processes too, and this entry steps aside without
// being withdrawn. It stays behind AVD for what AVD does not take: tile sessions (grid HEIC, iPhone photos: AVD has no
// tile entry points, so VTTileDecompressionSessionCreate moves on to the next decoder) and the streams AVD hands to
// software itself. Single-picture HEIC goes through an ordinary session, so on NVDEC.
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <os/log.h>
#include <stdlib.h>
#include <string.h>

typedef OSStatus (*CreateInstance)(FourCharCode codecType, CFAllocatorRef allocator, void **decoderOut);
typedef OSStatus (*RegisterDecoder)(FourCharCode codecType, CFDictionaryRef info, CreateInstance create);

__attribute__((visibility("default"))) void HEVCH8Register(void);

static bool choosesStreamingCodecs(const char *process)
{
    static const char *const kExact[] = {"mediaplaybackd", "mediaserverd", "videocodecd", "MobileSafari",
                                         "SafariViewService"};
    if (!process)
        return false;
    if (!strncmp(process, "com.apple.WebKit.", strlen("com.apple.WebKit.")))
        return true;
    for (size_t i = 0; i < sizeof(kExact) / sizeof(kExact[0]); i++)
        if (!strcmp(process, kExact[i]))
            return true;
    return false;
}

void HEVCH8Register(void)
{
    if (choosesStreamingCodecs(getprogname()))
        return;  // HEVC is software here, and this process is told so
    os_log_t log = os_log_create("com.brunostjohn.switch.hevc", "register");
    void *codec = dlopen("/System/Library/VideoCodecs/VCPHEVC.videocodec", RTLD_NOW | RTLD_LOCAL);
    CreateInstance create = codec ? (CreateInstance)dlsym(codec, "HEVCVideoDecoder_CreateInstance") : NULL;
    RegisterDecoder registerDecoder = (RegisterDecoder)dlsym(RTLD_DEFAULT, "VTRegisterVideoDecoderWithInfo");
    const CFStringRef *hardware = dlsym(RTLD_DEFAULT, "kVTVideoDecoder_IsHardwareAccelerated");
    if (!create || !registerDecoder || !hardware) {
        os_log_error(log, "HEVCH8: no %{public}s, HEVC stays software-only",
                     !create            ? "HEVCVideoDecoder_CreateInstance in VCPHEVC.videocodec"
                     : !registerDecoder ? "VTRegisterVideoDecoderWithInfo"
                                        : "kVTVideoDecoder_IsHardwareAccelerated");
        return;
    }
    const void *keys[] = {*hardware}, *values[] = {kCFBooleanTrue};
    CFDictionaryRef info =
        CFDictionaryCreate(NULL, keys, values, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    OSStatus status = registerDecoder('hvc1', info, create);
    CFRelease(info);
    if (status)
        os_log_error(log, "HEVCH8: VTRegisterVideoDecoderWithInfo('hvc1') failed: %d", (int)status);
    else
        os_log(log, "HEVCH8: software HEVC (VCPHEVC) registered as the hardware 'hvc1' decoder for still images");
}
