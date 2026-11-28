// SPDX-License-Identifier: BSD-2-Clause-Patent
// The Switch's audio devices for iPadOS's audio server (audiomxd). Samples move through rings shared with the kernel
// service SwitchAudio (../SwitchAudioAbi.h). Three devices, as VirtualAudio (VirtualAudio.plugin, which turns HAL
// devices into the ports routes are made of) knows the IPSW's iPad's by their UIDs (its DeviceFactory_Aspen); J120's
// for 21H461, J181's for 24B119 (SWITCH_AUDIO_J181):
//  - "Speaker" (built in): the output to the RT5639 (its speakers or the jack) and a silent stereo tap input. Stereo on
//    J120. On J181 four channels, left, left, right, right, as its audio-speaker (num-channels 4) and as every one of
//    24B119's speaker graphs (AID2003/VAD/speaker_*.dspg) ends; channels 0 and 2 are mixed into the stereo ring. With a
//    stereo stream VirtualAudio converts the graph's four channels to two with no channel map, which keeps channels 0
//    and 1: the left channel on both speakers.
//    VirtualAudio makes it Device_Speaker_Aspen, whose port is the built-in speaker (pspk, "Speaker"): the route
//    AVAudioSession calls BuiltInSpeaker and Control Center and the route picker show as the iPad itself. On 21H461 a
//    speaker device must have a tap stream (an input whose 'taps' is 1): every product's speaker policy sets
//    "tap stream required" when VirtualAudio's expanseEnabled feature is on, as it is, and without one the speaker
//    device throws ("TapStream is enabled by policy but not found") and plays nothing. Real hardware carries what
//    the speaker plays there, for echo cancellation; this one is silence.
//  - "Codec" (built in): the headphone jack, a stereo output and the headset microphone's mono input. VirtualAudio
//    makes it Device_Codec_Aspen: its 'jack' (kAudioDevicePropertyJackIsConnected) in the output scope makes the
//    Wired Headphones port (phpw) and in the input scope the Wired Microphone (pmwi), each available while its value
//    is 1. Both scopes must answer from the first query (VirtualAudio looks once per process); they change with the
//    kernel's jack state (control page, then PropertiesChanged on this device). With no input data sources it has no
//    built-in microphone, which is the Switch's case. The headphone volume is the codec's DAC volume (the level
//    control on output element 1, which VirtualAudio reads as the port's hardware volume range); the kernel routes the
//    RT5639 to the jack whenever a plug is in, so this device's output and the speaker's share one ring. On J181 the
//    jacks and the input data source are control objects, as 24B119's audio server needs them (SwitchAudioControls.h).
//  - the built-in input, silent (the Switch has no microphone): what a playAndRecord session's input is with no
//    headset, without which no such session activates ('!res') and none of its playback starts. On J120 "Hawking", one
//    mono channel, the Hawking port (phki) of J120's routing tables, which otherwise put the null input in its place.
//    On J181 "Digital Mic", its built-in microphone (its ADT's /arm-io/alc0/audio-leap-mic device-uid), which
//    VirtualAudio makes Device_DigitalMic_Aspen and its Built-In Microphone port (pmbi): two channels, as its two
//    microphones and AID2003's mic graphs (two in, one out) have.
// Speaker and Codec run on the service's one timeline (one clock domain, so their aggregate needs no drift
// correction); the built-in input keeps its own on the host clock and never starts the hardware.
// Objects: SwitchAudioObjects.h; the controls from 12 on exist only on J181.
// VirtualAudio runs a port's DSP chain after the mix. For pspk on J120 the chain is compiled into VirtualAudio: nine AUs
// (two AUNBandEQs, a compressor, AUVariableEQV2 'vrq2', a 400 Hz crossover, rotation shading to four channels, a
// four-channel EQ, AUControlFreak, speaker protection V3), tuned for J120's four MAX98723 drivers.
// /Library/Audio/Tunings/J120/VAD/j120_speaker_*.austrip only gives each of those AUs its preset and bypass flag,
// matched by subtype and occurrence. The system volume is software on this port: J120's routing database gives its
// hardware part a 0..0 dB range, so VirtualAudio leaves the control below at 0 dB and sets vrq2's gain instead. With
// their defaults the eight other AUs add about 20 dB and compress after vrq2 (PostDSP 7 to 13 dB over PreDSP at -14 to
// -4 dB of volume, peaks clipping), so the speaker is loud at any volume and volume steps barely change it.
// ../tunings lists every AU of each J120 speaker chain, bypassed except the volume (vrq2; AUVolume in measurement),
// and the Data overlay shows those strips over J120's (tree/overlay/manifest.json): the speaker plays the mix times the
// volume.
//
// Derived from UniXNU-sheng's hal/ShengAudioPlugIn.c (BSD-2-Clause-Patent), which plays on iPadOS 17.7.11 hardware;
// its diagnostics, property trace and VirtualAudio rebinding are left out. What it established and this keeps:
//  - 'accs', 'auto' and empty custom-property, box, clock-device and 'taps' lists must be answered or the device is
//    dropped (a tap stream answers 'taps' with 1 instead),
//  - CoreAudio's 24 MHz host clock constant must be set to the board's timer (19.2 MHz here, as on sheng),
//  - zero time stamps start at the frame IO starts on, with a period longer than safety offset plus one IO cycle.
//
// Samples: float32 from the audio server, S32_LE in the rings (../SwitchAudioAbi.h version 3; SwitchAudioConvert.h);
// the kernel service is built from the same ABI header and the two ship together (a version mismatch refuses the map).
// Build (the iOS SDK has no AudioServerPlugIn headers; the macOS ones declare the same interface):
//   mkdir -p H/CoreAudio; for h in AudioServerPlugIn.h AudioHardwareBase.h AudioHardware.h; do
//     ln -sf "$(xcrun --sdk macosx --show-sdk-path)/System/Library/Frameworks/CoreAudio.framework/Headers/$h" H/CoreAudio/; done
//   xcrun clang -target arm64-apple-ios17.0 -isysroot "$(xcrun --sdk iphoneos --show-sdk-path)" -IH -I.. -O2 -Wall \
//     -Wextra -Werror -bundle -fvisibility=hidden -framework CoreFoundation -framework IOKit SwitchAudioPlugIn.c \
//     -o SwitchAudio.driver/SwitchAudio   (24B119: also -DSWITCH_AUDIO_J181=1 SwitchAudioControls.c)
//   codesign -f -s - --pagesize 4096 --identifier com.brunostjohn.switch.audio SwitchAudio.driver/SwitchAudio
// The executable alone (the Data overlay ships it and Info.plist, no resource seal), with 4 KiB code pages as the
// system's own binaries here; scripts/package_raw_boot.py puts its CDHash in the boot trust cache.
#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOMessage.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <math.h>
#include <os/log.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <string.h>
#include "SwitchAudioAbi.h"
#include "SwitchAudioConvert.h"
#include "SwitchAudioFormat.h"
#include "SwitchAudioObjects.h"
#include "SwitchDisplayAudio.h"
#include "../../../kits/SwitchDMA/SwitchDcache.h"
#include "../../../kits/SwitchRefusal/SwitchRefusal.h"

// AudioHardware.h's kAudioDevicePropertyJackIsConnected (that header needs others the build does not stage).
enum {
    kJackIsConnected = 'jack'
};

#ifndef SWITCH_AUDIO_J181
#define SWITCH_AUDIO_J181 0
#endif

#if SWITCH_AUDIO_J181
#include "SwitchAudioControls.h"
#define kMicUID    "Digital Mic"
#define kMicName   "Digital Mic"
#define kMicInName "Digital Mic"

enum {
    kDeviceObjects = 7,  // Codec: two streams, volume, mute, two jacks and data source
};
#else
#define kMicUID    "Hawking"
#define kMicName   "Hawking"
#define kMicInName "Hawking Input"

enum {
    kDeviceObjects = 4,  // Speaker/Codec: two streams, volume and mute
};
#endif

#define kSpeakerUID "Speaker"  // VirtualAudio's names for an iPad's built-in devices (DeviceFactory_Aspen)
#define kCodecUID   "Codec"
#define kModelUID   "SwitchAudioModel"
static const UInt32 kClockDomain = SWITCH_AUDIO_MAGIC;  // speaker and microphone: the service's one clock
static const Float64 kRate = SWITCH_AUDIO_RATE;  // the built-in input's, and the service's until another is picked
static const UInt32 kZeroPeriod = SWITCH_AUDIO_RING_FRAMES / 4;  // matches ADMA ch0 kOutPeriodFrames (16384)

static AudioServerPlugInHostRef gHost;
static pthread_mutex_t gLock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gIoCond = PTHREAD_COND_INITIALIZER;
static Boolean gIoTransitioning;
static atomic_uint gRefs = 1;
static io_connect_t gConnect;
static SwitchAudioControl *gControl;
static switch_audio_sample_t *gOut, *gIn;
static UInt32 gSpeakerUsers, gCodecUsers, gMicUsers;  // IO clients per device (gLock; read atomically)
// The audio server marks the Codec's input stream inactive while no client reads it (kAudioStreamPropertyIsActive):
// headphone playback alone then leaves the microphone's path (DMA, I2S TX, the codec's ADC and bias) off. gLock.
static Boolean gCodecInActive = true, gCodecOutActive = true, gCodecInRunning;
static _Atomic Float32 gVolume = 1.0f, gCodecVolume = 1.0f, gSpeakerGain = 1.0f;
static _Atomic UInt32 gSpeakerMuted = 0, gCodecMuted = 0;
static uint32_t gJack;  // SwitchAudioJack*, as last told to the audio server
static IONotificationPortRef gJackPort;
static io_object_t gJackNotification;
static _Atomic Float64 gRate = SWITCH_AUDIO_RATE;  // the service's clock: speaker and codec
static _Atomic Float64 gTicksPerFrame = 0.0;       // the service's clock (its control page), nominal until mapped
static Float64 gHostTicksPerFrame;                 // the host clock's nominal rate: the built-in input's
static _Atomic uint64_t gZeroOrigin = 0, gZeroSeed = 0, gMicOrigin = 0, gMicSeed = 0;

static int mapType(uint32_t type, void **out)
{
    mach_vm_address_t address = 0;
    mach_vm_size_t size = 0;
    if (IOConnectMapMemory64(gConnect, type, mach_task_self(), &address, &size, kIOMapAnywhere) != kIOReturnSuccess)
        return -1;
    *out = (void *)(uintptr_t)address;
    return 0;
}

// CoreAudio's host clock frequency is a 24 MHz constant that AudioGetHostClockFrequency loads with adrp + ldr d0; its
// address is decoded from those two instructions and the kernel service rewrites it (the page is read-only here).
static void setHostClock(void)
{
    // audiomxd is arm64e: dlsym's pointer is signed, and this arm64 code only reads through the address bits.
    const uintptr_t symbol = (uintptr_t)dlsym(RTLD_DEFAULT, "AudioGetHostClockFrequency") & (((uintptr_t)1 << 47) - 1);
    if (!symbol)
        return;
    const uint32_t *code = (const uint32_t *)symbol, adrp = code[0], ldr = code[1];
    if ((adrp & 0x9f000000u) != 0x90000000u || (ldr & 0xffc00000u) != 0xfd400000u || ((ldr >> 5) & 31) != (adrp & 31)) {
        os_log_error(OS_LOG_DEFAULT, "SwitchAudio: AudioGetHostClockFrequency has an unexpected shape");
        return;
    }
    int64_t page = (int64_t)(((adrp >> 5) & 0x7ffffu) << 2 | ((adrp >> 29) & 3u));
    if (page & (1 << 20))
        page -= 1 << 21;
    const uint64_t address =
        ((uintptr_t)code & ~(uintptr_t)0xfff) + (uint64_t)(page << 12) + ((ldr >> 10) & 0xfffu) * 8;
    if (IOConnectCallScalarMethod(gConnect, SwitchAudioSetHostClock, &address, 1, NULL, NULL) != KERN_SUCCESS)
        os_log_error(OS_LOG_DEFAULT, "SwitchAudio: host clock constant not set");
}

static void jackChanged(void *refcon, io_service_t service, uint32_t type, void *argument);
static Float32 levelToDb(Float32 min, Float32 scalar);
static void sendHeadphoneLevel(void);
static void sendSpeakerLevel(void);
static void syncMute(AudioObjectID obj);

static int connectService(Boolean *connectedNow)
{
    *connectedNow = false;
    if (gControl)
        return 0;
    io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching(SWITCH_AUDIO_CLASS));
    if (!service)
        return -1;
    kern_return_t kr = IOServiceOpen(service, mach_task_self(), 0, &gConnect);
    // The jack's changes come as interest messages, registered before its state is first read so none is missed.
    if (kr == KERN_SUCCESS && !gJackPort) {
        gJackPort = IONotificationPortCreate(kIOMainPortDefault);
        if (gJackPort) {
            IONotificationPortSetDispatchQueue(gJackPort, dispatch_queue_create("SwitchAudio.jack", NULL));
            if (IOServiceAddInterestNotification(gJackPort, service, kIOGeneralInterest, jackChanged, NULL,
                                                 &gJackNotification) != KERN_SUCCESS)
                os_log_error(OS_LOG_DEFAULT, "SwitchAudio: no jack notifications: headphones stay as first read");
        }
    }
    IOObjectRelease(service);
    if (kr != KERN_SUCCESS)
        return -1;
    void *control = NULL, *out = NULL, *in = NULL;
    if (mapType(SwitchAudioMemoryControl, &control) || mapType(SwitchAudioMemoryOutput, &out) ||
        mapType(SwitchAudioMemoryInput, &in) || ((SwitchAudioControl *)control)->magic != SWITCH_AUDIO_MAGIC ||
        ((SwitchAudioControl *)control)->version != SWITCH_AUDIO_VERSION) {
        IOServiceClose(gConnect);
        gConnect = 0;
        return -1;
    }
    SwitchAudioControl *c = control;
    gOut = out;
    gIn = in;
    void *displayControl = NULL;
    if (!mapType(SwitchAudioMemoryDisplayControl, &displayControl) &&
        ((SwitchAudioControl *)displayControl)->magic == SWITCH_AUDIO_MAGIC &&
        ((SwitchAudioControl *)displayControl)->version == SWITCH_AUDIO_VERSION)
        switch_audio_display_attach(gHost, displayControl, NULL);
    atomic_store_explicit(&gTicksPerFrame, (Float64)c->ticks_per_frame_q32 / 4294967296.0, memory_order_release);
    if (c->rate)
        atomic_store_explicit(&gRate, (Float64)c->rate, memory_order_release);
    __atomic_store_n(&gJack, c->jack, __ATOMIC_RELAXED);
    __atomic_store_n(&gControl, c, __ATOMIC_RELEASE);
    *connectedNow = true;
    return 0;
}

static int connectServiceLocked(void)
{
    if (__atomic_load_n(&gControl, __ATOMIC_ACQUIRE))
        return 0;
    Boolean connectedNow = false;
    pthread_mutex_lock(&gLock);
    const int r = connectService(&connectedNow);
    pthread_mutex_unlock(&gLock);
    if (r == 0 && connectedNow) {
        setHostClock();
        sendHeadphoneLevel();
        sendSpeakerLevel();
        syncMute(kSpeakerMute);
    }
    return r;
}

static uint32_t jackState(void)
{
    return connectServiceLocked() ? 0 : __atomic_load_n(&gJack, __ATOMIC_RELAXED);
}

// The jack's state changed (the kernel service's interest message): the Codec device's 'jack' in each scope that moved.
static void jackChanged(void *refcon, io_service_t service, uint32_t type, void *argument)
{
    (void)refcon;
    (void)service;
    (void)argument;
    const SwitchAudioControl *c = __atomic_load_n(&gControl, __ATOMIC_ACQUIRE);
    if (type != kIOMessageServicePropertyChange || !c)
        return;
    switch_audio_display_changed();
    const uint32_t now = c->jack, was = __atomic_exchange_n(&gJack, now, __ATOMIC_RELAXED);
    AudioObjectPropertyAddress changed[2];
    UInt32 n = 0;
    if ((now ^ was) & SwitchAudioJackHeadphones) {
        changed[n++] = (AudioObjectPropertyAddress){kJackIsConnected, kAudioObjectPropertyScopeOutput,
                                                    kAudioObjectPropertyElementMain};
    }
    if ((now ^ was) & SwitchAudioJackMicrophone) {
        changed[n++] = (AudioObjectPropertyAddress){kJackIsConnected, kAudioObjectPropertyScopeInput,
                                                    kAudioObjectPropertyElementMain};
    }
    if (!n)
        return;
    os_log(OS_LOG_DEFAULT, "SwitchAudio: jack: headphones %{public}s, microphone %{public}s",
           now & SwitchAudioJackHeadphones ? "in" : "out", now & SwitchAudioJackMicrophone ? "in" : "out");
#if SWITCH_AUDIO_J181
    // The audio server reads the device's 'jack' from these controls, but its listeners (VirtualAudio's
    // HALJackStateHandler) are on the device's 'jack', which it does not derive from a control's change: both.
    static const AudioObjectPropertyAddress value = {kAudioBooleanControlPropertyValue, kAudioObjectPropertyScopeGlobal,
                                                     kAudioObjectPropertyElementMain};
    if ((now ^ was) & SwitchAudioJackHeadphones)
        gHost->PropertiesChanged(gHost, kCodecJackOut, 1, &value);
    if ((now ^ was) & SwitchAudioJackMicrophone)
        gHost->PropertiesChanged(gHost, kCodecJackIn, 1, &value);
#endif
    gHost->PropertiesChanged(gHost, kCodec, n, changed);
}

// gConnect is open (connectService): the headphone level to the codec, as attenuation in millidecibels.
static void sendHeadphoneLevel(void)
{
    if (!gConnect)
        return;
    for (int tries = 0; tries < 4; ++tries) {
        const Float32 level = atomic_load_explicit(&gCodecVolume, memory_order_acquire);
        const uint64_t attenuation =
            level <= 0.0f
                ? UINT32_MAX
                : (uint64_t)(-levelToDb(-(Float32)SWITCH_AUDIO_HEADPHONE_RANGE_MDB / 1000.0f, level) * 1000.0f + 0.5f);
        if (IOConnectCallScalarMethod(gConnect, SwitchAudioSetHeadphoneLevel, &attenuation, 1, NULL, NULL) !=
            KERN_SUCCESS)
            os_log_error(OS_LOG_DEFAULT, "SwitchAudio: headphone level not set");
        if (atomic_load_explicit(&gCodecVolume, memory_order_acquire) == level)
            break;
    }
}

static void applyHeadphoneLevel(void)
{
    if (connectServiceLocked() == 0)
        sendHeadphoneLevel();
}

static void sendSpeakerLevel(void)
{
    if (!gConnect)
        return;
    for (int tries = 0; tries < 4; ++tries) {
        const Float32 level = atomic_load_explicit(&gVolume, memory_order_acquire);
        const uint64_t attenuation =
            level <= 0.0f
                ? UINT32_MAX
                : (uint64_t)(-levelToDb(-(Float32)SWITCH_AUDIO_SPEAKER_RANGE_MDB / 1000.0f, level) * 1000.0f + 0.5f);
        if (IOConnectCallScalarMethod(gConnect, SwitchAudioSetSpeakerLevel, &attenuation, 1, NULL, NULL) !=
            KERN_SUCCESS)
            os_log_error(OS_LOG_DEFAULT, "SwitchAudio: speaker level not set");
        if (atomic_load_explicit(&gVolume, memory_order_acquire) == level)
            break;
    }
}

static void applySpeakerLevel(void)
{
    if (connectServiceLocked() == 0)
        sendSpeakerLevel();
}

static uint64_t effectiveMuteLocked(AudioObjectID fallbackObj)
{
    const UInt32 spk = atomic_load_explicit(&gSpeakerMuted, memory_order_relaxed);
    const UInt32 hp = atomic_load_explicit(&gCodecMuted, memory_order_relaxed);
    if (gSpeakerUsers && !gCodecUsers)
        return spk;
    if (gCodecUsers && !gSpeakerUsers)
        return hp;
    if (gSpeakerUsers && gCodecUsers)
        return gCodecOutActive ? hp : spk;
    return fallbackObj == kCodecMute ? hp : spk;
}

static void sendMuteValue(uint64_t muted)
{
    if (!gConnect)
        return;
    if (IOConnectCallScalarMethod(gConnect, SwitchAudioSetMute, &muted, 1, NULL, NULL) != KERN_SUCCESS)
        os_log_error(OS_LOG_DEFAULT, "SwitchAudio: output mute not set");
}

static void syncMute(AudioObjectID obj)
{
    for (int tries = 0; tries < 4; ++tries) {
        pthread_mutex_lock(&gLock);
        const uint64_t muted = effectiveMuteLocked(obj);
        pthread_mutex_unlock(&gLock);
        sendMuteValue(muted);
        pthread_mutex_lock(&gLock);
        const Boolean done = effectiveMuteLocked(obj) == muted;
        pthread_mutex_unlock(&gLock);
        if (done)
            break;
    }
}

static void applyMute(AudioObjectID obj)
{
    if (connectServiceLocked() == 0)
        syncMute(obj);
}

static void sendRoute(uint32_t route)
{
    const uint64_t arg = route;
    if (gConnect && IOConnectCallScalarMethod(gConnect, SwitchAudioSetRoute, &arg, 1, NULL, NULL) != KERN_SUCCESS)
        os_log_error(OS_LOG_DEFAULT, "SwitchAudio: route %u not set", (unsigned)route);
}

static kern_return_t call(uint32_t selector)
{
    return gConnect ? IOConnectCallScalarMethod(gConnect, selector, NULL, 0, NULL, NULL) : KERN_FAILURE;
}

static UInt32 latencyFrames(void)
{
    const SwitchAudioControl *c = __atomic_load_n(&gControl, __ATOMIC_ACQUIRE);
    return c ? c->lead_frames : SWITCH_AUDIO_LEAD_FRAMES;
}

// Capture becomes valid a period at a time (in_frames), so input is read at least that much plus the lead behind now.
static UInt32 inputSafetyFrames(void)
{
    const SwitchAudioControl *c = __atomic_load_n(&gControl, __ATOMIC_ACQUIRE);
    return latencyFrames() + (c ? c->period_frames : SWITCH_AUDIO_PERIOD_FRAMES);
}

static AudioStreamBasicDescription format(UInt32 channels, Float64 rate)
{
    AudioStreamBasicDescription f = {0};
    f.mSampleRate = rate;
    f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagsNativeEndian | kAudioFormatFlagIsPacked;
    f.mBytesPerPacket = 4 * channels;
    f.mFramesPerPacket = 1;
    f.mBytesPerFrame = 4 * channels;
    f.mChannelsPerFrame = channels;
    f.mBitsPerChannel = 32;
    return f;
}

#define REPLY(type, value)                             \
    do {                                               \
        if (inDataSize < sizeof(type))                 \
            return kAudioHardwareBadPropertySizeError; \
        *(type *)outData = (value);                    \
        *outDataSize = sizeof(type);                   \
        return 0;                                      \
    } while (0)
#define REPLY_STR(text) REPLY(CFStringRef, CFSTR(text))

// A volume control spans its minimum..0 dB on a squared curve over the scalar; scalar 0 mutes.
// On Tegra hardware, speaker attenuation runs in APE MVC1 and headphone attenuation splits across RT5639 HP_VOL + MVC1.
static const Float32 kMinDb = -(Float32)SWITCH_AUDIO_SPEAKER_RANGE_MDB / 1000.0f;
static const Float32 kCodecMinDb = -(Float32)SWITCH_AUDIO_HEADPHONE_RANGE_MDB / 1000.0f;

static Float32 minDbOf(AudioObjectID control)
{
    return control == kCodecVolume ? kCodecMinDb : kMinDb;
}

static _Atomic Float32 *levelOf(AudioObjectID control)
{
    return control == kCodecVolume ? &gCodecVolume : &gVolume;
}

static Float32 levelToDb(Float32 min, Float32 scalar)
{
    const Float32 r = 1.0f - (scalar < 0 ? 0 : scalar > 1 ? 1 : scalar);
    return min * r * r;
}

static Float32 levelToScalar(Float32 min, Float32 db)
{
    return db <= min ? 0.0f : db >= 0 ? 1.0f : 1.0f - sqrtf(db / min);
}

static Float32 toDb(Float32 scalar)
{
    return levelToDb(kMinDb, scalar);
}

static Boolean isDevice(AudioObjectID o)
{
    return o == kSpeaker || o == kCodec || o == kMic;
}

static Boolean isStream(AudioObjectID o)
{
    return o == kSpeakerOut || o == kSpeakerTap || o == kCodecIn || o == kCodecOut || o == kMicIn;
}

static Boolean isVolume(AudioObjectID o)
{
    return o == kVolume || o == kCodecVolume;
}

static Boolean isMute(AudioObjectID o)
{
    return o == kSpeakerMute || o == kCodecMute;
}

static AudioObjectID deviceOf(AudioObjectID o)  // the device a stream or control belongs to
{
    return o == kCodecIn || o == kCodecOut || o == kCodecVolume || o == kCodecMute ? kCodec
           : o == kMicIn                                                           ? kMic
                                                                                   : kSpeaker;
}

static AudioObjectID outputOf(AudioObjectID device)
{
    return device == kSpeaker ? kSpeakerOut : device == kCodec ? kCodecOut : 0;
}

static AudioObjectID inputOf(AudioObjectID device)
{
    return device == kSpeaker ? kSpeakerTap : device == kCodec ? kCodecIn : kMicIn;
}

static UInt32 channelsOf(AudioObjectID stream)
{
#if SWITCH_AUDIO_J181
    return stream == kSpeakerOut ? 4 : stream == kCodecIn ? 1 : 2;
#else
    return stream == kSpeakerOut || stream == kSpeakerTap || stream == kCodecOut ? 2 : 1;
#endif
}

static UInt32 usersOf(AudioObjectID device)
{
    return __atomic_load_n(device == kSpeaker ? &gSpeakerUsers
                           : device == kCodec ? &gCodecUsers
                                              : &gMicUsers,
                           __ATOMIC_ACQUIRE);
}

static Float64 rateOf(AudioObjectID device)
{
    return device == kMic ? kRate : atomic_load_explicit(&gRate, memory_order_acquire);
}

// The rates a device runs at: the service's (its control page) for the speaker and codec, which share its clock.
static UInt32 ratesOf(AudioObjectID device, Float64 rates[SWITCH_AUDIO_MAX_RATES])
{
    rates[0] = kRate;
    if (device == kMic || connectServiceLocked())
        return 1;
    const SwitchAudioControl *c = __atomic_load_n(&gControl, __ATOMIC_ACQUIRE);
    UInt32 n = 0;
    for (UInt32 i = 0; i < SWITCH_AUDIO_MAX_RATES && c->rates[i]; ++i)
        rates[n++] = c->rates[i];
    return n ? n : 1;
}

static OSStatus requestRate(AudioObjectID device, Float64 rate)
{
    if (rate == rateOf(device))
        return 0;
    Float64 rates[SWITCH_AUDIO_MAX_RATES];
    const UInt32 n = ratesOf(device, rates);
    for (UInt32 i = 0; i < n; ++i) {
        if (rates[i] == rate)
            return gHost->RequestDeviceConfigurationChange(gHost, device, (UInt64)rate, NULL);
    }
    switch_refusal_k(switch_refusal_key_with(switch_refusal_key("audio", "rate"), (uint64_t)rate), "audio", "rate",
                     "%.0f Hz asked of device %u; it runs %.0f Hz", rate, (unsigned)device, rateOf(device));
    return kAudioDeviceUnsupportedFormatError;
}

static HRESULT QueryInterface(void *self, REFIID uuid, LPVOID *out);

static ULONG AddRef(void *self)
{
    (void)self;
    return atomic_fetch_add_explicit(&gRefs, 1, memory_order_relaxed) + 1;
}

static ULONG Release(void *self)
{
    (void)self;
    UInt32 r = atomic_load_explicit(&gRefs, memory_order_relaxed);
    if (r)
        r = atomic_fetch_sub_explicit(&gRefs, 1, memory_order_acq_rel) - 1;
    return r;
}

static OSStatus Initialize(AudioServerPlugInDriverRef d, AudioServerPlugInHostRef host)
{
    (void)d;
    gHost = host;
    if (connectServiceLocked())
        os_log_error(OS_LOG_DEFAULT, "SwitchAudio: kernel service not available yet");
    return 0;
}

static OSStatus CreateDevice(AudioServerPlugInDriverRef d, CFDictionaryRef desc, const AudioServerPlugInClientInfo *c,
                             AudioObjectID *o)
{
    (void)d;
    (void)desc;
    (void)c;
    (void)o;
    return kAudioHardwareUnsupportedOperationError;
}

static OSStatus DestroyDevice(AudioServerPlugInDriverRef d, AudioObjectID o)
{
    (void)d;
    (void)o;
    return kAudioHardwareUnsupportedOperationError;
}

static OSStatus AddDeviceClient(AudioServerPlugInDriverRef d, AudioObjectID o, const AudioServerPlugInClientInfo *c)
{
    (void)d;
    (void)o;
    (void)c;
    return 0;
}

static OSStatus RemoveDeviceClient(AudioServerPlugInDriverRef d, AudioObjectID o, const AudioServerPlugInClientInfo *c)
{
    (void)d;
    (void)o;
    (void)c;
    return 0;
}

// A rate change (requestRate: the action is the rate). The audio server has stopped this device's IO; the kernel
// service changes the clock only while the other device's is stopped too, and that device then follows.
static OSStatus PerformDeviceConfigurationChange(AudioServerPlugInDriverRef d, AudioObjectID o, UInt64 a, void *i)
{
    (void)d;
    (void)i;
    if (o == kDisplay) {
        return connectServiceLocked() ? kAudioHardwareNotRunningError : switch_audio_display_perform(gConnect, a);
    }
    if (o == kMic || (Float64)a == atomic_load_explicit(&gRate, memory_order_acquire))
        return 0;
    if (connectServiceLocked())
        return kAudioHardwareIllegalOperationError;
    const uint64_t rate = a;
    const kern_return_t kr = IOConnectCallScalarMethod(gConnect, SwitchAudioSetRate, &rate, 1, NULL, NULL);
    if (kr != KERN_SUCCESS)
        return kAudioHardwareIllegalOperationError;
    pthread_mutex_lock(&gLock);
    const Float64 newRate = (Float64)gControl->rate;
    atomic_store_explicit(&gTicksPerFrame, (Float64)gControl->ticks_per_frame_q32 / 4294967296.0, memory_order_release);
    atomic_store_explicit(&gRate, newRate, memory_order_release);
    pthread_mutex_unlock(&gLock);
    os_log(OS_LOG_DEFAULT, "SwitchAudio: clock now %.0f Hz", newRate);
    gHost->RequestDeviceConfigurationChange(gHost, o == kSpeaker ? kCodec : kSpeaker, a, NULL);
    return 0;
}

static OSStatus AbortDeviceConfigurationChange(AudioServerPlugInDriverRef d, AudioObjectID o, UInt64 a, void *i)
{
    (void)d;
    (void)o;
    (void)a;
    (void)i;
    return 0;
}

static OSStatus GetPropertyData(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t pid,
                                const AudioObjectPropertyAddress *a, UInt32 qs, const void *q, UInt32 inDataSize,
                                UInt32 *outDataSize, void *outData);

static OSStatus GetPropertyDataSize(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t pid,
                                    const AudioObjectPropertyAddress *a, UInt32 qs, const void *q, UInt32 *size)
{
    if (o == kCodec && a->mSelector == kJackIsConnected) {
        if (a->mScope != kAudioObjectPropertyScopeOutput && a->mScope != kAudioObjectPropertyScopeInput)
            return kAudioHardwareUnknownPropertyError;
        *size = sizeof(UInt32);
        return 0;
    }
#if SWITCH_AUDIO_J181
    if (switch_audio_is_control(o)) {
        UInt8 scratch[64];
        UInt32 written = 0;
        const OSStatus r = switch_audio_control_property(o, a, qs, q, 0, sizeof scratch, &written, scratch);
        if (r == 0)
            *size = written;
        return r;
    }
#endif
    UInt8 scratch[1024];
    UInt32 written = 0;
    const OSStatus r = GetPropertyData(d, o, pid, a, qs, q, sizeof scratch, &written, scratch);
    if (r == 0)
        *size = written;
    return r;
}

static Boolean HasProperty(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t pid,
                           const AudioObjectPropertyAddress *a)
{
    (void)d;
    (void)pid;
    if (switch_audio_display_object(o)) {
        UInt8 scratch[256];
        UInt32 written = 0;
        return switch_audio_display_get(o, a, sizeof scratch, &written, scratch) == 0;
    }
    const AudioObjectPropertySelector s = a->mSelector;
    if (o == kPlugIn) {
        switch (s) {
        case kAudioObjectPropertyCustomPropertyInfoList:
        case kAudioPlugInPropertyBoxList:
        case kAudioPlugInPropertyClockDeviceList:
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:
        case kAudioObjectPropertyOwner:
        case kAudioObjectPropertyManufacturer:
        case kAudioObjectPropertyOwnedObjects:
        case kAudioPlugInPropertyDeviceList:
        case kAudioPlugInPropertyTranslateUIDToDevice:
        case kAudioPlugInPropertyResourceBundle:
            return true;
        default:
            return false;
        }
    }
    if (isDevice(o)) {
        switch (s) {
        case kAudioObjectPropertyCustomPropertyInfoList:
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:
        case kAudioObjectPropertyOwner:
        case kAudioObjectPropertyName:
        case kAudioObjectPropertyModelName:
        case kAudioObjectPropertyManufacturer:
        case kAudioDevicePropertyDeviceUID:
        case kAudioDevicePropertyModelUID:
        case kAudioObjectPropertySerialNumber:
        case kAudioObjectPropertyFirmwareVersion:
        case kAudioObjectPropertyIdentify:
        case 'accs':
        case 'auto':
        case kAudioObjectPropertyElementName:
        case kAudioDevicePropertyConfigurationApplication:
        case kAudioDevicePropertyTransportType:
        case kAudioDevicePropertyRelatedDevices:
        case kAudioDevicePropertyClockDomain:
        case kAudioDevicePropertyDeviceIsAlive:
        case kAudioDevicePropertyDeviceIsRunning:
        case kAudioDevicePropertyDeviceCanBeDefaultDevice:
        case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
        case kAudioDevicePropertyLatency:
        case kAudioDevicePropertySafetyOffset:
        case kAudioDevicePropertyNominalSampleRate:
        case kAudioDevicePropertyAvailableNominalSampleRates:
        case kAudioDevicePropertyIsHidden:
        case kAudioDevicePropertyZeroTimeStampPeriod:
        case kAudioDevicePropertyClockIsStable:
        case kAudioDevicePropertyClockAlgorithm:
        case kAudioDevicePropertyPreferredChannelsForStereo:
        case kAudioDevicePropertyPreferredChannelLayout:
        case kAudioObjectPropertyOwnedObjects:
        case kAudioDevicePropertyStreams:
        case kAudioObjectPropertyControlList:
            return true;
        case kJackIsConnected:
            return o == kCodec &&
                   (a->mScope == kAudioObjectPropertyScopeOutput || a->mScope == kAudioObjectPropertyScopeInput);
        default:
            return false;
        }
    }
    if (isStream(o)) {
        switch (s) {
        case kAudioObjectPropertyCustomPropertyInfoList:
        case 'taps':
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:
        case kAudioObjectPropertyOwner:
        case kAudioObjectPropertyName:
        case kAudioObjectPropertyOwnedObjects:
        case kAudioStreamPropertyIsActive:
        case kAudioStreamPropertyDirection:
        case kAudioStreamPropertyTerminalType:
        case kAudioStreamPropertyStartingChannel:
        case kAudioStreamPropertyLatency:
        case kAudioStreamPropertyVirtualFormat:
        case kAudioStreamPropertyPhysicalFormat:
        case kAudioStreamPropertyAvailableVirtualFormats:
        case kAudioStreamPropertyAvailablePhysicalFormats:
            return true;
        default:
            return false;
        }
    }
    if (isVolume(o)) {
        switch (s) {
        case kAudioObjectPropertyCustomPropertyInfoList:
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:
        case kAudioObjectPropertyOwner:
        case kAudioObjectPropertyOwnedObjects:
        case kAudioControlPropertyScope:
        case kAudioControlPropertyElement:
        case kAudioLevelControlPropertyScalarValue:
        case kAudioLevelControlPropertyDecibelValue:
        case kAudioLevelControlPropertyDecibelRange:
        case kAudioLevelControlPropertyConvertScalarToDecibels:
        case kAudioLevelControlPropertyConvertDecibelsToScalar:
            return true;
        default:
            return false;
        }
    }
    if (isMute(o)) {
        switch (s) {
        case kAudioObjectPropertyCustomPropertyInfoList:
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:
        case kAudioObjectPropertyOwner:
        case kAudioObjectPropertyOwnedObjects:
        case kAudioControlPropertyScope:
        case kAudioControlPropertyElement:
        case kAudioBooleanControlPropertyValue:
            return true;
        default:
            return false;
        }
    }
#if SWITCH_AUDIO_J181
    if (switch_audio_is_control(o)) {
        UInt8 scratch[64];
        UInt32 written = 0;
        return switch_audio_control_property(o, a, 0, NULL, 0, sizeof scratch, &written, scratch) == 0;
    }
#endif
    return false;
}

static OSStatus IsPropertySettable(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t pid,
                                   const AudioObjectPropertyAddress *a, Boolean *out)
{
    const AudioObjectPropertySelector s = a->mSelector;
    if (switch_audio_display_object(o)) {
        *out = switch_audio_display_settable(s);
        return HasProperty(d, o, pid, a) ? 0 : kAudioHardwareUnknownPropertyError;
    }
    *out =
        (isVolume(o) && (s == kAudioLevelControlPropertyScalarValue || s == kAudioLevelControlPropertyDecibelValue)) ||
        (isMute(o) && s == kAudioBooleanControlPropertyValue) ||
        (isStream(o) && (s == kAudioStreamPropertyVirtualFormat || s == kAudioStreamPropertyPhysicalFormat ||
                         s == kAudioStreamPropertyIsActive)) ||
        (isDevice(o) && s == kAudioDevicePropertyNominalSampleRate);
    return HasProperty(d, o, pid, a) ? 0 : kAudioHardwareUnknownPropertyError;
}

// gLock held and gIoTransitioning set: drops gLock across the blocking ioctl while preserving input serialization.
static void syncCodecInput(void)
{
    for (;;) {
        const Boolean want = gCodecUsers > 0 && gCodecInActive && gControl;
        if (want == gCodecInRunning)
            return;
        pthread_mutex_unlock(&gLock);
        const kern_return_t kr = call(want ? SwitchAudioStartInput : SwitchAudioStopInput);
        pthread_mutex_lock(&gLock);
        if (kr == KERN_SUCCESS) {
            gCodecInRunning = want;
            os_log(OS_LOG_DEFAULT, "SwitchAudio: headset microphone %{public}s", want ? "on" : "off");
        } else {
            if (want)
                os_log_error(OS_LOG_DEFAULT, "SwitchAudio: headset microphone not started: the kernel service refused");
            return;
        }
    }
}

static OSStatus SetPropertyData(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t pid,
                                const AudioObjectPropertyAddress *a, UInt32 qs, const void *q, UInt32 size,
                                const void *data)
{
    (void)d;
    (void)pid;
    (void)qs;
    (void)q;
    if (switch_audio_display_object(o))
        return switch_audio_display_set(o, a, size, data);
    if (isVolume(o) && size >= sizeof(Float32)) {
        const Float32 min = minDbOf(o);
        Float32 v = *(const Float32 *)data;
        if (a->mSelector == kAudioLevelControlPropertyDecibelValue)
            v = levelToScalar(min, v);
        else if (a->mSelector != kAudioLevelControlPropertyScalarValue)
            return kAudioHardwareUnknownPropertyError;
        const Float32 clamped = v < 0 ? 0 : v > 1 ? 1 : v;
        atomic_store_explicit(levelOf(o), clamped, memory_order_relaxed);
        if (o == kCodecVolume) {
            applyHeadphoneLevel();
        } else {
            const Float32 linear = clamped <= 0.0f ? 0.0f : powf(10.0f, levelToDb(min, clamped) / 20.0f);
            atomic_store_explicit(&gSpeakerGain, linear, memory_order_relaxed);
            applySpeakerLevel();
        }
        os_log(OS_LOG_DEFAULT, "SwitchAudio: %{public}s volume control %.3f (%.1f dB)",
               o == kCodecVolume ? "headphone" : "output", (double)clamped, (double)levelToDb(min, clamped));
        const AudioObjectPropertyAddress changed[2] = {
            {kAudioLevelControlPropertyScalarValue, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain},
            {kAudioLevelControlPropertyDecibelValue, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain}};
        gHost->PropertiesChanged(gHost, o, 2, changed);
        return 0;
    }
    if (isMute(o) && a->mSelector == kAudioBooleanControlPropertyValue) {
        if (size < sizeof(UInt32))
            return kAudioHardwareBadPropertySizeError;
        const UInt32 muted = *(const UInt32 *)data != 0 ? 1u : 0u;
        atomic_store_explicit(o == kCodecMute ? &gCodecMuted : &gSpeakerMuted, muted, memory_order_relaxed);
        applyMute(o);
        os_log(OS_LOG_DEFAULT, "SwitchAudio: %{public}s mute %{public}s", o == kCodecMute ? "headphone" : "speaker",
               muted ? "on" : "off");
        const AudioObjectPropertyAddress changed = {kAudioBooleanControlPropertyValue, kAudioObjectPropertyScopeGlobal,
                                                    kAudioObjectPropertyElementMain};
        gHost->PropertiesChanged(gHost, o, 1, &changed);
        return 0;
    }
    if (isStream(o) &&
        (a->mSelector == kAudioStreamPropertyVirtualFormat || a->mSelector == kAudioStreamPropertyPhysicalFormat)) {
        const AudioStreamBasicDescription *f = data;
        if (size < sizeof *f || f->mChannelsPerFrame != channelsOf(o))
            return kAudioDeviceUnsupportedFormatError;
        return requestRate(deviceOf(o), f->mSampleRate);
    }
    if (isDevice(o) && a->mSelector == kAudioDevicePropertyNominalSampleRate)
        return size >= sizeof(Float64) ? requestRate(o, *(const Float64 *)data) : kAudioHardwareBadPropertySizeError;
    if (isStream(o) && a->mSelector == kAudioStreamPropertyIsActive) {
        if (size < sizeof(UInt32))
            return kAudioHardwareBadPropertySizeError;
        const Boolean active = *(const UInt32 *)data != 0;
        if (o == kCodecIn) {
            pthread_mutex_lock(&gLock);
            __atomic_store_n(&gCodecInActive, active, __ATOMIC_RELEASE);
            while (gIoTransitioning)
                pthread_cond_wait(&gIoCond, &gLock);
            gIoTransitioning = true;
            syncCodecInput();
            gIoTransitioning = false;
            pthread_cond_broadcast(&gIoCond);
            pthread_mutex_unlock(&gLock);
        } else if (o == kCodecOut) {
            pthread_mutex_lock(&gLock);
            __atomic_store_n(&gCodecOutActive, active, __ATOMIC_RELEASE);
            while (gIoTransitioning)
                pthread_cond_wait(&gIoCond, &gLock);
            if (gCodecUsers > 0 && gControl && (active || gSpeakerUsers > 0)) {
                gIoTransitioning = true;
                const uint32_t route = active ? SwitchAudioRouteHeadphones : SwitchAudioRouteSpeaker;
                const uint64_t muted = effectiveMuteLocked(active ? kCodecMute : kSpeakerMute);
                pthread_mutex_unlock(&gLock);
                sendRoute(route);
                sendMuteValue(muted);
                pthread_mutex_lock(&gLock);
                gIoTransitioning = false;
                pthread_cond_broadcast(&gIoCond);
            }
            pthread_mutex_unlock(&gLock);
        }
        return 0;
    }
    return kAudioHardwareUnknownPropertyError;
}

static OSStatus GetPropertyData(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t pid,
                                const AudioObjectPropertyAddress *a, UInt32 qs, const void *q, UInt32 inDataSize,
                                UInt32 *outDataSize, void *outData)
{
    (void)d;
    (void)pid;
    const AudioObjectPropertySelector s = a->mSelector;
    if (switch_audio_display_object(o))
        return switch_audio_display_get(o, a, inDataSize, outDataSize, outData);
    // Lists the audio server reads for each new device and that must answer, empty: custom properties on every
    // object, boxes and clock devices on the plug-in, taps on the streams (an unknown-property error drops the device).
    // The speaker's tap stream answers 'taps' with 1 (top of file).
    if (o == kSpeakerTap && s == 'taps')
        REPLY(UInt32, 1);
    if ((o <= kCodecVolume || isMute(o)) &&
        (s == kAudioObjectPropertyCustomPropertyInfoList ||
         ((s == kAudioPlugInPropertyBoxList || s == kAudioPlugInPropertyClockDeviceList) && o == kPlugIn) ||
         (s == 'taps' && isStream(o)))) {
        *outDataSize = 0;
        return 0;
    }
    if (o == kPlugIn) {
        switch (s) {
        case kAudioObjectPropertyBaseClass:
            REPLY(AudioClassID, kAudioObjectClassID);
        case kAudioObjectPropertyClass:
            REPLY(AudioClassID, kAudioPlugInClassID);
        case kAudioObjectPropertyOwner:
            REPLY(AudioObjectID, kAudioObjectUnknown);
        case kAudioObjectPropertyManufacturer:
            REPLY_STR("Switch");
        case kAudioObjectPropertyOwnedObjects:
        case kAudioPlugInPropertyDeviceList: {
            const AudioObjectID devices[] = {kSpeaker, kCodec, kMic, kDisplay};
            UInt32 n = switch_audio_display_available() ? 4 : 3;
            if (inDataSize / sizeof(AudioObjectID) < n)
                n = inDataSize / (UInt32)sizeof(AudioObjectID);
            memcpy(outData, devices, n * sizeof(AudioObjectID));
            *outDataSize = n * (UInt32)sizeof(AudioObjectID);
            return 0;
        }
        case kAudioPlugInPropertyTranslateUIDToDevice: {
            CFStringRef uid = qs >= sizeof(CFStringRef) ? *(const CFStringRef *)q : NULL;
            AudioObjectID device = kAudioObjectUnknown;
            if (uid && CFStringCompare(uid, CFSTR(kSpeakerUID), 0) == kCFCompareEqualTo)
                device = kSpeaker;
            if (uid && CFStringCompare(uid, CFSTR(kCodecUID), 0) == kCFCompareEqualTo)
                device = kCodec;
            if (uid && CFStringCompare(uid, CFSTR(kMicUID), 0) == kCFCompareEqualTo)
                device = kMic;
            if (uid && switch_audio_display_available() &&
                CFStringCompare(uid, CFSTR(SWITCH_AUDIO_DISPLAY_UID), 0) == kCFCompareEqualTo)
                device = kDisplay;
            REPLY(AudioObjectID, device);
        }
        case kAudioPlugInPropertyResourceBundle:
            REPLY_STR("");
        default:
            return kAudioHardwareUnknownPropertyError;
        }
    }
    if (isDevice(o)) {
        const AudioObjectID output = outputOf(o), input = inputOf(o);
        const Boolean scopeIn = a->mScope == kAudioObjectPropertyScopeInput;
        switch (s) {
        case kAudioObjectPropertyBaseClass:
            REPLY(AudioClassID, kAudioObjectClassID);
        case kAudioObjectPropertyClass:
            REPLY(AudioClassID, kAudioDeviceClassID);
        case kAudioObjectPropertyOwner:
            REPLY(AudioObjectID, kPlugIn);
        case kAudioObjectPropertyName:
        case kAudioObjectPropertyModelName:
            if (o == kSpeaker)
                REPLY_STR("Speaker");
            if (o == kCodec)
                REPLY_STR("Headphones");
            REPLY_STR(kMicName);
        case kAudioObjectPropertyManufacturer:
            REPLY_STR("Switch");
        case kAudioDevicePropertyDeviceUID:
            if (o == kSpeaker)
                REPLY_STR(kSpeakerUID);
            if (o == kCodec)
                REPLY_STR(kCodecUID);
            REPLY_STR(kMicUID);
        case kAudioDevicePropertyModelUID:
            REPLY_STR(kModelUID);
        case kAudioObjectPropertySerialNumber:
            REPLY_STR("0001");
        case kAudioObjectPropertyFirmwareVersion:
            REPLY_STR("1.0");
        case kAudioObjectPropertyIdentify:
            REPLY(UInt32, 0);
        case 'accs':
            REPLY(UInt32, 0);  // private device properties the audio server reads for each new device
        case 'auto':
            REPLY(UInt32, 0);
        case kAudioObjectPropertyElementName:
            REPLY_STR("");
        case kAudioDevicePropertyConfigurationApplication:
            REPLY_STR("com.apple.Preferences");
        case kAudioDevicePropertyTransportType:
            REPLY(UInt32, kAudioDeviceTransportTypeBuiltIn);
        case kJackIsConnected:
            if (o != kCodec || (a->mScope != kAudioObjectPropertyScopeOutput && !scopeIn))
                return kAudioHardwareUnknownPropertyError;
            REPLY(UInt32, (jackState() & (scopeIn ? SwitchAudioJackMicrophone : SwitchAudioJackHeadphones)) != 0);
        case kAudioDevicePropertyRelatedDevices:
            REPLY(AudioObjectID, o);
        case kAudioDevicePropertyClockDomain:
            REPLY(UInt32, o == kMic ? 0 : kClockDomain);
        case kAudioDevicePropertyDeviceIsAlive:
            REPLY(UInt32, 1);
        case kAudioDevicePropertyDeviceIsRunning:
            REPLY(UInt32, usersOf(o) > 0);
        case kAudioDevicePropertyDeviceCanBeDefaultDevice:
        case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
            REPLY(UInt32, o != kMic);
        case kAudioDevicePropertyLatency:
            REPLY(UInt32, o == kMic ? 0 : latencyFrames());
        case kAudioDevicePropertySafetyOffset:
            if (o == kMic)
                REPLY(UInt32, SWITCH_AUDIO_PERIOD_FRAMES);
            REPLY(UInt32, scopeIn ? inputSafetyFrames() : latencyFrames() + SWITCH_AUDIO_PERIOD_FRAMES);
        case kAudioDevicePropertyNominalSampleRate:
            REPLY(Float64, rateOf(o));
        case kAudioDevicePropertyAvailableNominalSampleRates: {
            Float64 rates[SWITCH_AUDIO_MAX_RATES];
            UInt32 n = ratesOf(o, rates);
            if (inDataSize / sizeof(AudioValueRange) < n)
                n = inDataSize / (UInt32)sizeof(AudioValueRange);
            for (UInt32 k = 0; k < n; ++k)
                ((AudioValueRange *)outData)[k] = (AudioValueRange){rates[k], rates[k]};
            *outDataSize = n * (UInt32)sizeof(AudioValueRange);
            return 0;
        }
        case kAudioDevicePropertyIsHidden:
            REPLY(UInt32, 0);
        case kAudioDevicePropertyZeroTimeStampPeriod:
            REPLY(UInt32, kZeroPeriod);
        case kAudioDevicePropertyClockIsStable:
            REPLY(UInt32, 1);
        case kAudioDevicePropertyClockAlgorithm: {
            const SwitchAudioControl *ctrl = __atomic_load_n(&gControl, __ATOMIC_ACQUIRE);
            const UInt32 algo = (o != kMic && ctrl && ctrl->backend == SwitchAudioBackendTegraI2S)
                                    ? kAudioDeviceClockAlgorithmSimpleIIR
                                    : kAudioDeviceClockAlgorithmRaw;
            REPLY(UInt32, algo);
        }
        case kAudioDevicePropertyPreferredChannelsForStereo: {
            if (inDataSize < 2 * sizeof(UInt32))
                return kAudioHardwareBadPropertySizeError;
            const UInt32 channels = channelsOf(scopeIn || !output ? input : output);
            ((UInt32 *)outData)[0] = 1;
            ((UInt32 *)outData)[1] = channels >= 2 ? 2 : 1;
            *outDataSize = 2 * sizeof(UInt32);
            return 0;
        }
        case kAudioDevicePropertyPreferredChannelLayout: {
            const UInt32 channels = channelsOf(scopeIn || !output ? input : output);
            const UInt32 bytes = (UInt32)(offsetof(AudioChannelLayout, mChannelDescriptions) +
                                          channels * sizeof(AudioChannelDescription));
            if (inDataSize < bytes)
                return kAudioHardwareBadPropertySizeError;
            AudioChannelLayout *l = outData;
            memset(l, 0, bytes);
            l->mChannelLayoutTag = kAudioChannelLayoutTag_UseChannelDescriptions;
            l->mNumberChannelDescriptions = channels;
            if (SWITCH_AUDIO_J181 && channels == 4) {
                // VirtualAudio forwards this layout verbatim onto its 2-ch vdef stream. Matching ASDStream's
                // kAudioChannelLabel_Unknown causes AQMEIO_HAL to fall back to PreferredChannelsForStereo {1, 2};
                // labeling ch 0/2 Left/Right leaves vdef's 2-ch prefix {Left, Unused} and zeroes the right channel.
                for (UInt32 i = 0; i < channels; ++i)
                    l->mChannelDescriptions[i].mChannelLabel = kAudioChannelLabel_Unknown;
            } else {
                l->mChannelDescriptions[0].mChannelLabel =
                    channels == 1 ? kAudioChannelLabel_Mono : kAudioChannelLabel_Left;
                if (channels == 2)
                    l->mChannelDescriptions[1].mChannelLabel = kAudioChannelLabel_Right;
            }
            *outDataSize = bytes;
            return 0;
        }
        case kAudioObjectPropertyOwnedObjects:
        case kAudioDevicePropertyStreams:
        case kAudioObjectPropertyControlList: {
            AudioObjectID list[kDeviceObjects];
            UInt32 n = 0;
            const Boolean streams = s != kAudioObjectPropertyControlList, controls = s != kAudioDevicePropertyStreams;
            if (streams && output && !scopeIn)
                list[n++] = output;
            if (streams && a->mScope != kAudioObjectPropertyScopeOutput)
                list[n++] = input;
            if (controls && o == kSpeaker && !scopeIn) {
                list[n++] = kVolume;
                list[n++] = kSpeakerMute;
            }
            if (controls && o == kCodec && !scopeIn) {
                list[n++] = kCodecVolume;
                list[n++] = kCodecMute;
            }
#if SWITCH_AUDIO_J181
            if (controls)
                n += switch_audio_controls_of(o, a->mScope, list + n, kDeviceObjects - n);
#endif
            if (inDataSize / sizeof(AudioObjectID) < n)
                n = inDataSize / (UInt32)sizeof(AudioObjectID);
            memcpy(outData, list, n * sizeof(AudioObjectID));
            *outDataSize = n * (UInt32)sizeof(AudioObjectID);
            return 0;
        }
        default:
            return kAudioHardwareUnknownPropertyError;
        }
    }
    if (isStream(o)) {
        const UInt32 channels = channelsOf(o);
        switch (s) {
        case kAudioObjectPropertyBaseClass:
            REPLY(AudioClassID, kAudioObjectClassID);
        case kAudioObjectPropertyClass:
            REPLY(AudioClassID, kAudioStreamClassID);
        case kAudioObjectPropertyOwner:
            REPLY(AudioObjectID, deviceOf(o));
        case kAudioObjectPropertyName:
            if (o == kSpeakerOut)
                REPLY_STR("Speaker");
            if (o == kSpeakerTap)
                REPLY_STR("Speaker Tap");
            if (o == kCodecIn)
                REPLY_STR("Headset Microphone");
            if (o == kCodecOut)
                REPLY_STR("Headphones");
            REPLY_STR(kMicInName);
        case kAudioObjectPropertyOwnedObjects:
            *outDataSize = 0;
            return 0;
        case kAudioStreamPropertyIsActive:
            REPLY(UInt32, o == kCodecIn    ? __atomic_load_n(&gCodecInActive, __ATOMIC_ACQUIRE)
                          : o == kCodecOut ? __atomic_load_n(&gCodecOutActive, __ATOMIC_ACQUIRE)
                                           : 1);
        case kAudioStreamPropertyDirection:
            REPLY(UInt32, o != kSpeakerOut && o != kCodecOut);
        case kAudioStreamPropertyTerminalType:
            REPLY(UInt32, o == kSpeakerOut   ? kAudioStreamTerminalTypeSpeaker
                          : o == kSpeakerTap ? kAudioStreamTerminalTypeUnknown
                          : o == kCodecOut   ? kAudioStreamTerminalTypeHeadphones
                          : o == kCodecIn    ? kAudioStreamTerminalTypeHeadsetMicrophone
                                             : kAudioStreamTerminalTypeMicrophone);
        case kAudioStreamPropertyStartingChannel:
            REPLY(UInt32, 1);
        case kAudioStreamPropertyLatency:
            REPLY(UInt32, 0);
        case kAudioStreamPropertyVirtualFormat:
        case kAudioStreamPropertyPhysicalFormat:
            REPLY(AudioStreamBasicDescription, format(channels, rateOf(deviceOf(o))));
        case kAudioStreamPropertyAvailableVirtualFormats:
        case kAudioStreamPropertyAvailablePhysicalFormats: {
            Float64 rates[SWITCH_AUDIO_MAX_RATES];
            UInt32 n = ratesOf(deviceOf(o), rates);
            if (inDataSize / sizeof(AudioStreamRangedDescription) < n)
                n = inDataSize / (UInt32)sizeof(AudioStreamRangedDescription);
            for (UInt32 k = 0; k < n; ++k) {
                ((AudioStreamRangedDescription *)outData)[k] =
                    (AudioStreamRangedDescription){format(channels, rates[k]), {rates[k], rates[k]}};
            }
            *outDataSize = n * (UInt32)sizeof(AudioStreamRangedDescription);
            return 0;
        }
        default:
            return kAudioHardwareUnknownPropertyError;
        }
    }
    if (isVolume(o)) {
        const Float32 min = minDbOf(o), level = atomic_load_explicit(levelOf(o), memory_order_relaxed);
        switch (s) {
        case kAudioObjectPropertyBaseClass:
            REPLY(AudioClassID, kAudioLevelControlClassID);
        case kAudioObjectPropertyClass:
            REPLY(AudioClassID, kAudioVolumeControlClassID);
        case kAudioObjectPropertyOwner:
            REPLY(AudioObjectID, deviceOf(o));
        case kAudioObjectPropertyOwnedObjects:
            *outDataSize = 0;
            return 0;
        case kAudioControlPropertyScope:
            REPLY(AudioObjectPropertyScope, kAudioObjectPropertyScopeOutput);
        case kAudioControlPropertyElement:
            // VirtualAudio's codec ports by output element: 1 is the headphones.
            REPLY(AudioObjectPropertyElement, o == kCodecVolume ? 1 : kAudioObjectPropertyElementMain);
        case kAudioLevelControlPropertyScalarValue:
            REPLY(Float32, level);
        case kAudioLevelControlPropertyDecibelValue:
            REPLY(Float32, levelToDb(min, level));
        case kAudioLevelControlPropertyDecibelRange:
            REPLY(AudioValueRange, ((AudioValueRange){min, 0.0}));
        case kAudioLevelControlPropertyConvertScalarToDecibels:
            if (inDataSize < sizeof(Float32))
                return kAudioHardwareBadPropertySizeError;
            *(Float32 *)outData = levelToDb(min, *(Float32 *)outData);
            *outDataSize = sizeof(Float32);
            return 0;
        case kAudioLevelControlPropertyConvertDecibelsToScalar:
            if (inDataSize < sizeof(Float32))
                return kAudioHardwareBadPropertySizeError;
            *(Float32 *)outData = levelToScalar(min, *(Float32 *)outData);
            *outDataSize = sizeof(Float32);
            return 0;
        default:
            return kAudioHardwareUnknownPropertyError;
        }
    }
    if (isMute(o)) {
        switch (s) {
        case kAudioObjectPropertyBaseClass:
            REPLY(AudioClassID, kAudioBooleanControlClassID);
        case kAudioObjectPropertyClass:
            REPLY(AudioClassID, kAudioMuteControlClassID);
        case kAudioObjectPropertyOwner:
            REPLY(AudioObjectID, deviceOf(o));
        case kAudioObjectPropertyOwnedObjects:
        case kAudioObjectPropertyCustomPropertyInfoList:
            *outDataSize = 0;
            return 0;
        case kAudioControlPropertyScope:
            REPLY(AudioObjectPropertyScope, kAudioObjectPropertyScopeOutput);
        case kAudioControlPropertyElement:
            REPLY(AudioObjectPropertyElement, o == kCodecMute ? 1 : kAudioObjectPropertyElementMain);
        case kAudioBooleanControlPropertyValue:
            REPLY(UInt32, o == kCodecMute ? atomic_load_explicit(&gCodecMuted, memory_order_relaxed)
                                          : atomic_load_explicit(&gSpeakerMuted, memory_order_relaxed));
        default:
            return kAudioHardwareUnknownPropertyError;
        }
    }
#if SWITCH_AUDIO_J181
    if (switch_audio_is_control(o)) {
        const uint32_t jack = o == kCodecJackOut || o == kCodecJackIn ? jackState() : 0;
        return switch_audio_control_property(o, a, qs, q, jack, inDataSize, outDataSize, outData);
    }
#endif
    return kAudioHardwareBadObjectError;
}

// The play position now, in sample time on the service's timeline.
static uint64_t nowFrames(void)
{
    const SwitchAudioControl *c = __atomic_load_n(&gControl, __ATOMIC_ACQUIRE);
    const Float64 tpf = atomic_load_explicit(&gTicksPerFrame, memory_order_acquire);
    if (!c || tpf <= 0.0)
        return 0;
    const uint64_t anchor = __atomic_load_n(&c->anchor_host, __ATOMIC_ACQUIRE);
    const uint64_t host = mach_absolute_time();
    return host > anchor ? (uint64_t)((Float64)(host - anchor) / tpf) : 0;
}

typedef struct {
    uint64_t sample;
    uint64_t host;
    uint32_t seed;
} HwAnchor;

static Boolean readHwAnchor(const SwitchAudioControl *c, HwAnchor *out)
{
    for (int tries = 0; tries < 8; ++tries) {
        const uint32_t s0 = __atomic_load_n(&c->hw_seq, __ATOMIC_ACQUIRE);
        if (s0 & 1u)
            continue;
        const uint32_t seed = __atomic_load_n(&c->hw_seed, __ATOMIC_RELAXED);
        const uint64_t sample = __atomic_load_n(&c->hw_sample_time, __ATOMIC_RELAXED);
        const uint64_t host = __atomic_load_n(&c->hw_host_time, __ATOMIC_RELAXED);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        const uint32_t s1 = __atomic_load_n(&c->hw_seq, __ATOMIC_ACQUIRE);
        if (s0 == s1) {
            out->sample = sample;
            out->host = host;
            out->seed = seed;
            return seed != 0 && host != 0;
        }
    }
    return false;
}

// The speaker and the codec device start the service's output (syncCodecInput the microphone); the hardware runs while
// either does, on one timeline that starts (new origin, new seed) when the first of them starts. The built-in input
// only starts its own clock.
static OSStatus StartIO(AudioServerPlugInDriverRef d, AudioObjectID o, UInt32 client)
{
    (void)d;
    (void)client;
    if (o == kDisplay) {
        return connectServiceLocked() ? kAudioHardwareNotRunningError : switch_audio_display_start(gConnect);
    }
    if (!isDevice(o))
        return kAudioHardwareBadObjectError;
    if (o == kMic) {
        pthread_mutex_lock(&gLock);
        if (!gMicUsers) {
            atomic_store_explicit(&gMicOrigin, mach_absolute_time(), memory_order_release);
            atomic_fetch_add_explicit(&gMicSeed, 1, memory_order_acq_rel);
        }
        __atomic_store_n(&gMicUsers, gMicUsers + 1, __ATOMIC_RELEASE);
        pthread_mutex_unlock(&gLock);
        return 0;
    }
    const char *const what = o == kSpeaker ? "speaker" : "headphone";
    if (connectServiceLocked()) {
        os_log_error(OS_LOG_DEFAULT, "SwitchAudio: %{public}s IO not started: the kernel service refused", what);
        return kAudioHardwareNotRunningError;
    }
    OSStatus r = 0;
    pthread_mutex_lock(&gLock);
    while (gIoTransitioning)
        pthread_cond_wait(&gIoCond, &gLock);
    gIoTransitioning = true;
    UInt32 *users = o == kSpeaker ? &gSpeakerUsers : &gCodecUsers;
    AudioObjectID muteObj = kSpeakerMute;
    uint64_t sentMute = 0;
    Boolean startedNew = false;
    if (*users == 0) {
        const Boolean first = !gSpeakerUsers && !gCodecUsers;
        uint32_t route = SwitchAudioRouteSpeaker;
        if (o == kCodec && (!gSpeakerUsers || gCodecOutActive)) {
            route = SwitchAudioRouteHeadphones;
        }
        muteObj = route == SwitchAudioRouteHeadphones ? kCodecMute : kSpeakerMute;
        sentMute = effectiveMuteLocked(muteObj);
        pthread_mutex_unlock(&gLock);

        sendRoute(route);
        sendMuteValue(sentMute);
        kern_return_t startKr = KERN_SUCCESS;
        if (first) {
            startKr = call(SwitchAudioStartOutput);
            if (startKr != KERN_SUCCESS) {
                sendRoute(SwitchAudioRouteAuto);
            } else {
                setHostClock();  // again: the shared page may have been read back from disk since
            }
        }

        pthread_mutex_lock(&gLock);
        if (first && startKr != KERN_SUCCESS) {
            r = kAudioHardwareNotRunningError;
            os_log_error(OS_LOG_DEFAULT, "SwitchAudio: %{public}s IO not started: the kernel service refused", what);
        } else {
            startedNew = true;
            if (first) {
                atomic_store_explicit(&gZeroOrigin, nowFrames(), memory_order_release);
                atomic_store_explicit(&gZeroSeed, atomic_load_explicit(&gZeroSeed, memory_order_relaxed) + 1,
                                      memory_order_release);
            }
            os_log(OS_LOG_DEFAULT, "SwitchAudio: %{public}s IO started (volume control %.1f dB)", what,
                   (double)toDb(atomic_load_explicit(&gVolume, memory_order_relaxed)));
        }
    }
    if (!r) {
        __atomic_store_n(users, *users + 1, __ATOMIC_RELEASE);
        syncCodecInput();
        if (startedNew) {
            const uint64_t nowMute = effectiveMuteLocked(muteObj);
            if (nowMute != sentMute) {
                pthread_mutex_unlock(&gLock);
                sendMuteValue(nowMute);
                pthread_mutex_lock(&gLock);
            }
        }
    }
    gIoTransitioning = false;
    pthread_cond_broadcast(&gIoCond);
    pthread_mutex_unlock(&gLock);
    return r;
}

static OSStatus StopIO(AudioServerPlugInDriverRef d, AudioObjectID o, UInt32 client)
{
    (void)d;
    (void)client;
    if (o == kDisplay) {
        return __atomic_load_n(&gControl, __ATOMIC_ACQUIRE) ? switch_audio_display_stop(gConnect) : 0;
    }
    if (!isDevice(o))
        return kAudioHardwareBadObjectError;
    if (o == kMic) {
        pthread_mutex_lock(&gLock);
        if (gMicUsers)
            __atomic_store_n(&gMicUsers, gMicUsers - 1, __ATOMIC_RELEASE);
        pthread_mutex_unlock(&gLock);
        return 0;
    }
    pthread_mutex_lock(&gLock);
    while (gIoTransitioning)
        pthread_cond_wait(&gIoCond, &gLock);
    gIoTransitioning = true;
    UInt32 *users = o == kSpeaker ? &gSpeakerUsers : &gCodecUsers;
    if (*users) {
        const UInt32 next = *users - 1;
        __atomic_store_n(users, next, __ATOMIC_RELEASE);
        if (next == 0) {
            syncCodecInput();
            const Boolean stopAll = !gSpeakerUsers && !gCodecUsers;
            uint32_t route = SwitchAudioRouteAuto;
            if (!stopAll) {
                route = gSpeakerUsers ? SwitchAudioRouteSpeaker : SwitchAudioRouteHeadphones;
            }
            const uint64_t muted = stopAll ? 0 : effectiveMuteLocked(gSpeakerUsers ? kSpeakerMute : kCodecMute);
            pthread_mutex_unlock(&gLock);

            if (stopAll) {
                call(SwitchAudioStopOutput);
                sendRoute(SwitchAudioRouteAuto);
            } else {
                sendRoute(route);
                sendMuteValue(muted);
            }
            os_log(OS_LOG_DEFAULT, "SwitchAudio: %{public}s IO stopped", o == kSpeaker ? "speaker" : "headphone");

            pthread_mutex_lock(&gLock);
        }
    }
    gIoTransitioning = false;
    pthread_cond_broadcast(&gIoCond);
    pthread_mutex_unlock(&gLock);
    return 0;
}

static OSStatus GetZeroTimeStamp(AudioServerPlugInDriverRef d, AudioObjectID o, UInt32 client, Float64 *sampleTime,
                                 UInt64 *hostTime, UInt64 *seed)
{
    (void)d;
    (void)client;
    if (o == kDisplay)
        return switch_audio_display_timestamp(sampleTime, hostTime, seed);
    if (o == kMic) {
        const uint64_t origin = atomic_load_explicit(&gMicOrigin, memory_order_acquire);
        const uint64_t now = mach_absolute_time();
        const Float64 period = gHostTicksPerFrame * kZeroPeriod;
        const uint64_t wraps = now > origin ? (uint64_t)((Float64)(now - origin) / period) : 0;
        *sampleTime = (Float64)(wraps * kZeroPeriod);
        *hostTime = origin + (uint64_t)((Float64)wraps * period);
        *seed = atomic_load_explicit(&gMicSeed, memory_order_acquire);
        return 0;
    }
    // gControl is write-once before StartIO; RT callbacks need no acquire barrier.
    const SwitchAudioControl *ctrl = gControl;
    const Float64 tpf = atomic_load_explicit(&gTicksPerFrame, memory_order_acquire);
    if (!ctrl || tpf <= 0.0)
        return kAudioHardwareNotRunningError;
    HwAnchor hw;
    if (readHwAnchor(ctrl, &hw)) {
        *sampleTime = (Float64)hw.sample;
        *hostTime = hw.host;
        *seed = (atomic_load_explicit(&gZeroSeed, memory_order_acquire) << 32) | hw.seed;
        return 0;
    }
    const uint64_t anchor = __atomic_load_n(&ctrl->anchor_host, __ATOMIC_ACQUIRE);
    const uint64_t originFrames = atomic_load_explicit(&gZeroOrigin, memory_order_acquire);
    const uint64_t origin = anchor + (uint64_t)((Float64)originFrames * tpf);
    const uint64_t now = mach_absolute_time();
    const Float64 period = tpf * kZeroPeriod;
    const uint64_t wraps = now > origin ? (uint64_t)((Float64)(now - origin) / period) : 0;
    *sampleTime = (Float64)(originFrames + wraps * kZeroPeriod);
    *hostTime = origin + (uint64_t)((Float64)wraps * period);
    *seed = atomic_load_explicit(&gZeroSeed, memory_order_acquire);
    return 0;
}

static OSStatus WillDoIOOperation(AudioServerPlugInDriverRef d, AudioObjectID o, UInt32 client, UInt32 op,
                                  Boolean *will, Boolean *inPlace)
{
    (void)d;
    (void)client;
    *will = (op == kAudioServerPlugInIOOperationWriteMix && (o == kSpeaker || o == kCodec || o == kDisplay)) ||
            (op == kAudioServerPlugInIOOperationReadInput && o != kDisplay);
    *inPlace = true;
    return 0;
}

static OSStatus BeginIOOperation(AudioServerPlugInDriverRef d, AudioObjectID o, UInt32 client, UInt32 op, UInt32 frames,
                                 const AudioServerPlugInIOCycleInfo *info)
{
    (void)d;
    (void)o;
    (void)client;
    (void)op;
    (void)frames;
    (void)info;
    return 0;
}

static OSStatus DoIOOperation(AudioServerPlugInDriverRef d, AudioObjectID o, AudioObjectID stream, UInt32 client,
                              UInt32 op, UInt32 frames, const AudioServerPlugInIOCycleInfo *info, void *main,
                              void *secondary)
{
    (void)d;
    (void)o;
    (void)client;
    (void)secondary;
    if (o == kDisplay && stream == kDisplayOut && op == kAudioServerPlugInIOOperationWriteMix) {
        switch_audio_display_mix(frames, info, main);
        return 0;
    }
    if (op == kAudioServerPlugInIOOperationReadInput && (stream == kSpeakerTap || stream == kMicIn)) {
        memset(main, 0, (size_t)frames * channelsOf(stream) * sizeof(Float32));  // silent inputs (top of file)
        return 0;
    }
    SwitchAudioControl *ctrl = gControl;
    if (!ctrl)
        return 0;
    if (op == kAudioServerPlugInIOOperationWriteMix && (stream == kSpeakerOut || stream == kCodecOut)) {
        if (!frames || info->mOutputTime.mSampleTime < 0.0)
            return 0;
        uint64_t first = (uint64_t)info->mOutputTime.mSampleTime;
        const uint64_t played = __atomic_load_n(&ctrl->out_frames, __ATOMIC_ACQUIRE);
        const Float32 *src = main;
        UInt32 count = frames;
        if (first + count <= played)
            return 0;
        if (first < played) {
            const uint64_t skip = played - first;
            src += skip * channelsOf(stream);
            first = played;
            count -= (UInt32)skip;
        }
        const uint64_t limit = played + (SWITCH_AUDIO_RING_FRAMES - SWITCH_AUDIO_RING_FRAMES / 4);
        if (first >= limit)
            return 0;
        if (first + count > limit)
            count = (UInt32)(limit - first);
        Float32 mixGain = 1.0f;
        if (ctrl->backend != SwitchAudioBackendTegraI2S) {
            if (stream == kSpeakerOut) {
                mixGain = atomic_load_explicit(&gSpeakerMuted, memory_order_relaxed)
                              ? 0.0f
                              : atomic_load_explicit(&gSpeakerGain, memory_order_relaxed);
            } else if (atomic_load_explicit(&gCodecMuted, memory_order_relaxed)) {
                mixGain = 0.0f;
            }
        }
        const uint32_t at = (uint32_t)(first % SWITCH_AUDIO_RING_FRAMES);
#if SWITCH_AUDIO_J181
        const uint32_t run = stream == kSpeakerOut
                                 ? switch_audio_ring_fold(gOut, SWITCH_AUDIO_RING_FRAMES, first, src, count, mixGain)
                                 : switch_audio_ring_mix(gOut, SWITCH_AUDIO_RING_FRAMES, 2, first, src, count, mixGain);
#else
        const uint32_t run = switch_audio_ring_mix(gOut, SWITCH_AUDIO_RING_FRAMES, 2, first, src, count, mixGain);
#endif
        // The Tegra backend's DMA reads the ring from memory and does not snoop the caches: write back what was mixed.
        if (ctrl->backend == SwitchAudioBackendTegraI2S && count) {
            switch_dcache_clean_lines(gOut + 2 * at, run * 2 * sizeof(switch_audio_sample_t));
            if (run < count)
                switch_dcache_clean_lines(gOut, (count - run) * 2 * sizeof(switch_audio_sample_t));
            switch_dcache_sync();
        }
        const uint64_t t = first + count;
        if (t > __atomic_load_n(&ctrl->out_written, __ATOMIC_RELAXED))
            __atomic_store_n(&ctrl->out_written, t, __ATOMIC_RELEASE);
#if SWITCH_DEBUG
        ctrl->io_cycles = ctrl->io_cycles + 1;
        ctrl->io_frames = ctrl->io_frames + count;
#endif
    } else if (op == kAudioServerPlugInIOOperationReadInput && stream == kCodecIn) {
        if (info->mInputTime.mSampleTime < 0.0) {
            memset(main, 0, (size_t)frames * sizeof(Float32));
            return 0;
        }
        const uint64_t t = (uint64_t)info->mInputTime.mSampleTime;
        const uint64_t anchor = __atomic_load_n(&ctrl->in_anchor, __ATOMIC_ACQUIRE);
        const uint64_t captured = __atomic_load_n(&ctrl->in_frames, __ATOMIC_ACQUIRE);
        switch_audio_ring_read_s32(main, gIn, SWITCH_AUDIO_IN_RING_FRAMES, t, frames, anchor, captured);
    }
    return 0;
}

static OSStatus EndIOOperation(AudioServerPlugInDriverRef d, AudioObjectID o, UInt32 client, UInt32 op, UInt32 frames,
                               const AudioServerPlugInIOCycleInfo *info)
{
    (void)d;
    (void)o;
    (void)client;
    (void)op;
    (void)frames;
    (void)info;
    return 0;
}

static AudioServerPlugInDriverInterface gInterface = {NULL,
                                                      QueryInterface,
                                                      AddRef,
                                                      Release,
                                                      Initialize,
                                                      CreateDevice,
                                                      DestroyDevice,
                                                      AddDeviceClient,
                                                      RemoveDeviceClient,
                                                      PerformDeviceConfigurationChange,
                                                      AbortDeviceConfigurationChange,
                                                      HasProperty,
                                                      IsPropertySettable,
                                                      GetPropertyDataSize,
                                                      GetPropertyData,
                                                      SetPropertyData,
                                                      StartIO,
                                                      StopIO,
                                                      GetZeroTimeStamp,
                                                      WillDoIOOperation,
                                                      BeginIOOperation,
                                                      DoIOOperation,
                                                      EndIOOperation};
static AudioServerPlugInDriverInterface *gInterfacePtr = &gInterface;

static HRESULT QueryInterface(void *self, REFIID uuid, LPVOID *out)
{
    CFUUIDRef requested = CFUUIDCreateFromUUIDBytes(NULL, uuid);
    HRESULT r = E_NOINTERFACE;
    if (CFEqual(requested, IUnknownUUID) || CFEqual(requested, kAudioServerPlugInDriverInterfaceUUID)) {
        AddRef(self);
        *out = &gInterfacePtr;
        r = S_OK;
    }
    CFRelease(requested);
    return r;
}

__attribute__((visibility("default"))) void *SwitchAudio_Create(CFAllocatorRef allocator, CFUUIDRef type)
{
    (void)allocator;
    if (!CFEqual(type, kAudioServerPlugInTypeUUID))
        return NULL;
    if (!gHostTicksPerFrame) {
        mach_timebase_info_data_t tb;
        mach_timebase_info(&tb);
        gHostTicksPerFrame = 1e9 / kRate * tb.denom / tb.numer;
        if (atomic_load_explicit(&gTicksPerFrame, memory_order_relaxed) == 0.0)
            atomic_store_explicit(&gTicksPerFrame, gHostTicksPerFrame, memory_order_release);
    }
    return &gInterfacePtr;
}
