// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test for AVDRegister (AVDRegister.c) on this Mac's VideoToolbox, over a fake TegraNVDEC service. VideoToolbox's
// and CoreMedia's answers to "do this process's sessions decode in videocodecd" are defined here: AVDRegister finds
// them by dlsym, which searches the executable first. Each case counts the anonymous entries VTCopyVideoDecoderList
// gains per codec, and whether VideoToolbox lists them as hardware:
//   decodes here, may open:        'avc1', 'vp09', 'hvc1', 'mp4v', 'mp2v', 'mp1v' as hardware, after one probe
//                                  connection
//   decodes here, refused:         nothing
//   sessions run in videocodecd:   the published codecs as hardware, with no connection opened
//   is videocodecd, refused:       nothing
//   sessions in videocodecd, no codec matched:   nothing
// Prints "PASS avd-register" when all hold (avd_register_host_test).
#include <stdio.h>
#include <string.h>
#include "../../AVD/AVDDecoder.h"

void AVDRegister(void);
extern OSStatus VTCopyVideoDecoderList(CFDictionaryRef options, CFArrayRef *decoders);
extern const CFStringRef kVTVideoDecoderList_CodecType, kVTVideoDecoderList_DecoderID,
    kVTVideoDecoderList_IsHardwareAccelerated;

struct NVDECClient {
    int unused;
};

os_log_t gAVDLog;
static bool gSeparate, gIsCodecServer, gRefuse;
static uint32_t gPublished;
static int gOpens;

__attribute__((visibility("default"), used)) Boolean VTVideoCodecService_ShouldUseSeparateCodecProcessForDecode(void)
{
    return gSeparate;
}

__attribute__((visibility("default"), used)) Boolean FigServer_IsVideocodecd(void)
{
    return gIsCodecServer;
}

static const uint32_t kCodecs = 1u << TEGRA_NVDEC_CODEC_H264 | 1u << TEGRA_NVDEC_CODEC_VP9 |
                                1u << TEGRA_NVDEC_CODEC_HEVC | 1u << TEGRA_NVDEC_CODEC_MPEG4 |
                                1u << TEGRA_NVDEC_CODEC_MPEG12;

bool nvdec_service_present(void)
{
    return true;
}

uint32_t nvdec_service_codecs(void)
{
    return gPublished;
}

NVDECClient *nvdec_open(int *error)
{
    gOpens++;
    if (gRefuse) {
        *error = kIOReturnNotPermitted;
        return NULL;
    }
    return calloc(1, sizeof(NVDECClient));
}

void nvdec_close(NVDECClient *client)
{
    free(client);
}

uint32_t nvdec_codecs(const NVDECClient *client)
{
    (void)client;
    return kCodecs;
}

OSStatus avd_create_instance(FourCharCode codec, CFAllocatorRef allocator, VTVideoDecoderRef *out)
{
    (void)codec, (void)allocator, (void)out;
    return kVTCouldNotFindVideoDecoderErr;
}

typedef struct {
    int hardware, software;
} Entries;

static Entries entries(FourCharCode codec)
{
    Entries counted = {0, 0};
    CFArrayRef list = NULL;
    if (VTCopyVideoDecoderList(NULL, &list) || !list)
        return counted;
    for (CFIndex i = 0; i < CFArrayGetCount(list); i++) {
        CFDictionaryRef entry = CFArrayGetValueAtIndex(list, i);
        CFStringRef id = CFDictionaryGetValue(entry, kVTVideoDecoderList_DecoderID);
        CFNumberRef type = CFDictionaryGetValue(entry, kVTVideoDecoderList_CodecType);
        int32_t listed = 0;
        if (type)
            CFNumberGetValue(type, kCFNumberSInt32Type, &listed);
        if (!id || !CFStringHasPrefix(id, CFSTR("com.apple.videotoolbox.videodecoder.anon-")) ||
            (FourCharCode)listed != codec)
            continue;
        if (CFDictionaryGetValue(entry, kVTVideoDecoderList_IsHardwareAccelerated) == kCFBooleanTrue)
            counted.hardware++;
        else
            counted.software++;
    }
    CFRelease(list);
    return counted;
}

static const FourCharCode kRegistered[] = {kCMVideoCodecType_H264,       kCMVideoCodecType_VP9,
                                           kCMVideoCodecType_HEVC,       kCMVideoCodecType_MPEG4Video,
                                           kCMVideoCodecType_MPEG2Video, kCMVideoCodecType_MPEG1Video};

enum {
    kCount = sizeof(kRegistered) / sizeof(kRegistered[0])
};

// Runs AVDRegister in one situation and checks the entries it added (each codec once, or none) and its connections.
static bool registerCase(const char *name, bool separate, bool isCodecServer, bool refuse, uint32_t published,
                         bool wantEntries, int wantOpens)
{
    gSeparate = separate, gIsCodecServer = isCodecServer, gRefuse = refuse, gPublished = published;
    Entries before[kCount];
    for (int i = 0; i < kCount; i++)
        before[i] = entries(kRegistered[i]);
    const int opens = gOpens;
    AVDRegister();
    bool ok = gOpens - opens == wantOpens;
    for (int i = 0; i < kCount; i++) {
        const Entries after = entries(kRegistered[i]);
        ok = ok && after.hardware - before[i].hardware == (wantEntries ? 1 : 0) && after.software == before[i].software;
    }
    printf("%s %s: %d connection(s)\n", ok ? "ok" : "FAIL", name, gOpens - opens);
    return ok;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    bool ok = registerCase("decodes here, may open", false, false, false, 0, true, 1);
    ok = registerCase("decodes here, refused", false, false, true, kCodecs, false, 1) && ok;
    ok = registerCase("sessions run in videocodecd", true, false, true, kCodecs, true, 0) && ok;
    ok = registerCase("is videocodecd, refused", true, true, true, kCodecs, false, 1) && ok;
    ok = registerCase("sessions in videocodecd, no codec matched", true, false, true, 0, false, 0) && ok;
    printf("%s avd-register: AVDRegister's hardware entries, where its sessions can reach NVDEC\n",
           ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
