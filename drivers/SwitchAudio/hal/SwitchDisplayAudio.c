// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "SwitchDisplayAudio.h"
#include "SwitchAudioObjects.h"
#include "SwitchAudioConvert.h"
#include "../../../kits/SwitchDMA/SwitchDcache.h"
#include <mach/mach_time.h>
#include <os/log.h>
#include <pthread.h>
#include <stddef.h>
#include <string.h>

static pthread_mutex_t gDisplayLock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gDisplayCond = PTHREAD_COND_INITIALIZER;
static Boolean gDisplayTransitioning;
static AudioServerPlugInHostRef gHost;
static SwitchAudioControl *gControl;
static void *gOutput;
static UInt32 gUsers;
static uint32_t gAvailable;
static uint64_t gSeed;

Boolean switch_audio_display_object(AudioObjectID object)
{
    return object == kDisplay || object == kDisplayOut;
}

// The rate and rates[] the audio server last heard about: a re-probe for another sink can change both.
static uint32_t gPublishedClock[SWITCH_AUDIO_MAX_RATES + 1];

static Boolean rememberClock(void)
{
    uint32_t clock[SWITCH_AUDIO_MAX_RATES + 1] = {0};
    if (gControl) {
        clock[0] = gControl->rate;
        for (UInt32 i = 0; i < SWITCH_AUDIO_MAX_RATES; ++i)
            clock[i + 1] = gControl->rates[i];
    }
    const Boolean changed = memcmp(clock, gPublishedClock, sizeof(clock)) != 0;
    memcpy(gPublishedClock, clock, sizeof(clock));
    return changed;
}

void switch_audio_display_attach(AudioServerPlugInHostRef host, SwitchAudioControl *control, void *output)
{
    gHost = host;
    gOutput = output;
    gControl = control;
    __atomic_store_n(&gAvailable, control && __atomic_load_n(&control->jack, __ATOMIC_ACQUIRE), __ATOMIC_RELEASE);
    (void)rememberClock();
}

Boolean switch_audio_display_available(void)
{
    return __atomic_load_n(&gAvailable, __ATOMIC_ACQUIRE) != 0;
}

void switch_audio_display_changed(void)
{
    const uint32_t available = gControl && __atomic_load_n(&gControl->jack, __ATOMIC_ACQUIRE);
    const Boolean alive = __atomic_exchange_n(&gAvailable, available, __ATOMIC_ACQ_REL) != available;
    if (rememberClock() && gHost) {
        const AudioObjectPropertyAddress clock[] = {
            {kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain},
            {kAudioDevicePropertyAvailableNominalSampleRates, kAudioObjectPropertyScopeGlobal,
             kAudioObjectPropertyElementMain}};
        const AudioObjectPropertyAddress formats[] = {
            {kAudioStreamPropertyPhysicalFormat, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain},
            {kAudioStreamPropertyVirtualFormat, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain},
            {kAudioStreamPropertyAvailablePhysicalFormats, kAudioObjectPropertyScopeGlobal,
             kAudioObjectPropertyElementMain},
            {kAudioStreamPropertyAvailableVirtualFormats, kAudioObjectPropertyScopeGlobal,
             kAudioObjectPropertyElementMain}};
        gHost->PropertiesChanged(gHost, kDisplay, 2, clock);
        gHost->PropertiesChanged(gHost, kDisplayOut, 4, formats);
    }
    if (!alive || !gHost)
        return;
    const AudioObjectPropertyAddress device = {kAudioDevicePropertyDeviceIsAlive, kAudioObjectPropertyScopeGlobal,
                                               kAudioObjectPropertyElementMain};
    const AudioObjectPropertyAddress plugin[] = {
        {kAudioPlugInPropertyDeviceList, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain},
        {kAudioObjectPropertyOwnedObjects, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain}};
    gHost->PropertiesChanged(gHost, kDisplay, 1, &device);
    gHost->PropertiesChanged(gHost, kPlugIn, 2, plugin);
}

static Float64 currentRate(void)
{
    const uint32_t rate = gControl ? gControl->rate : 0;
    return rate ? rate : 48000;
}

static UInt32 availableRates(Float64 rates[SWITCH_AUDIO_MAX_RATES])
{
    UInt32 n = 0;
    for (UInt32 i = 0; gControl && i < SWITCH_AUDIO_MAX_RATES && gControl->rates[i]; ++i)
        rates[n++] = gControl->rates[i];
    if (!n)
        rates[n++] = currentRate();
    return n;
}

static Boolean supportedRate(Float64 rate)
{
    Float64 rates[SWITCH_AUDIO_MAX_RATES];
    const UInt32 n = availableRates(rates);
    for (UInt32 i = 0; i < n; ++i) {
        if (rates[i] == rate)
            return true;
    }
    return false;
}

static OSStatus requestRate(Float64 rate)
{
    if (rate == currentRate())
        return 0;
    if (!supportedRate(rate))
        return kAudioDeviceUnsupportedFormatError;
    return gHost ? gHost->RequestDeviceConfigurationChange(gHost, kDisplay, (UInt64)rate, NULL)
                 : kAudioHardwareNotRunningError;
}

static AudioStreamBasicDescription format(Float64 rate)
{
    return (AudioStreamBasicDescription){rate,
                                         kAudioFormatLinearPCM,
                                         kAudioFormatFlagIsFloat | kAudioFormatFlagsNativeEndian |
                                             kAudioFormatFlagIsPacked,
                                         8,
                                         1,
                                         8,
                                         2,
                                         32,
                                         0};
}

#define REPLY(type, value)                             \
    do {                                               \
        if (capacity < sizeof(type))                   \
            return kAudioHardwareBadPropertySizeError; \
        *(type *)data = (value);                       \
        *bytes = sizeof(type);                         \
        return 0;                                      \
    } while (0)

OSStatus switch_audio_display_get(AudioObjectID object, const AudioObjectPropertyAddress *address, UInt32 capacity,
                                  UInt32 *bytes, void *data)
{
    const Boolean device = object == kDisplay, input = address->mScope == kAudioObjectPropertyScopeInput;
    const AudioObjectPropertySelector selector = address->mSelector;
    *bytes = 0;
    switch (selector) {
    case kAudioObjectPropertyBaseClass:
        REPLY(AudioClassID, kAudioObjectClassID);
    case kAudioObjectPropertyClass:
        REPLY(AudioClassID, device ? kAudioDeviceClassID : kAudioStreamClassID);
    case kAudioObjectPropertyOwner:
        REPLY(AudioObjectID, device ? kPlugIn : kDisplay);
    case kAudioObjectPropertyName:
    case kAudioObjectPropertyModelName:
        REPLY(CFStringRef, CFSTR("DisplayPort"));
    case kAudioObjectPropertyManufacturer:
        REPLY(CFStringRef, CFSTR("Switch"));
    case kAudioObjectPropertyCustomPropertyInfoList:
    case 'taps':
        return 0;
    case kAudioObjectPropertyOwnedObjects:
        if (!device || input)
            return 0;
        REPLY(AudioObjectID, kDisplayOut);
    default:
        break;
    }
    if (!device) {
        switch (selector) {
        case kAudioStreamPropertyIsActive:
            REPLY(UInt32, 1);
        case kAudioStreamPropertyDirection:
            REPLY(UInt32, 0);
        case kAudioStreamPropertyTerminalType:
            REPLY(UInt32, kAudioStreamTerminalTypeDisplayPort);
        case kAudioStreamPropertyStartingChannel:
            REPLY(UInt32, 1);
        case kAudioStreamPropertyLatency:
            REPLY(UInt32, 0);
        case kAudioStreamPropertyVirtualFormat:
        case kAudioStreamPropertyPhysicalFormat:
            REPLY(AudioStreamBasicDescription, format(currentRate()));
        case kAudioStreamPropertyAvailableVirtualFormats:
        case kAudioStreamPropertyAvailablePhysicalFormats: {
            Float64 rates[SWITCH_AUDIO_MAX_RATES];
            UInt32 n = availableRates(rates);
            if (capacity / sizeof(AudioStreamRangedDescription) < n)
                n = capacity / (UInt32)sizeof(AudioStreamRangedDescription);
            for (UInt32 i = 0; i < n; ++i)
                ((AudioStreamRangedDescription *)data)[i] =
                    (AudioStreamRangedDescription){format(rates[i]), {rates[i], rates[i]}};
            *bytes = n * (UInt32)sizeof(AudioStreamRangedDescription);
            return 0;
        }
        default:
            return kAudioHardwareUnknownPropertyError;
        }
    }
    switch (selector) {
    case kAudioDevicePropertyDeviceUID:
        REPLY(CFStringRef, CFSTR(SWITCH_AUDIO_DISPLAY_UID));
    case kAudioDevicePropertyModelUID:
        REPLY(CFStringRef, CFSTR("SwitchDisplayPortAudio"));
    case kAudioObjectPropertySerialNumber:
        REPLY(CFStringRef, CFSTR("0001"));
    case kAudioObjectPropertyFirmwareVersion:
        REPLY(CFStringRef, CFSTR("1.0"));
    case kAudioDevicePropertyConfigurationApplication:
        REPLY(CFStringRef, CFSTR("com.apple.Preferences"));
    case kAudioObjectPropertyElementName:
        REPLY(CFStringRef, CFSTR(""));
    case kAudioDevicePropertyTransportType:
        REPLY(UInt32, kAudioDeviceTransportTypeDisplayPort);
    case kAudioDevicePropertyClockDomain:
        REPLY(UInt32, SWITCH_AUDIO_MAGIC + 1);
    case kAudioDevicePropertyRelatedDevices:
        REPLY(AudioObjectID, kDisplay);
    case kAudioDevicePropertyDeviceIsAlive:
        REPLY(UInt32, switch_audio_display_available());
    case kAudioDevicePropertyDeviceIsRunning:
        REPLY(UInt32, gControl && (gControl->state & SwitchAudioOutputRunning));
    case kAudioDevicePropertyDeviceCanBeDefaultDevice:
    case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
        REPLY(UInt32, !input);
    case kAudioDevicePropertyClockIsStable:
        REPLY(UInt32, 1);
    case kAudioDevicePropertyClockAlgorithm:
        REPLY(UInt32, kAudioDeviceClockAlgorithmRaw);
    case kAudioDevicePropertyIsHidden:
    case kAudioObjectPropertyIdentify:
    case 'accs':
    case 'auto':
        REPLY(UInt32, 0);
    case kAudioDevicePropertyLatency:
        REPLY(UInt32, gControl ? gControl->lead_frames : 0);
    case kAudioDevicePropertySafetyOffset:
        REPLY(UInt32, gControl ? gControl->lead_frames + 2 * SWITCH_AUDIO_PERIOD_FRAMES : 0);
    case kAudioDevicePropertyZeroTimeStampPeriod:
        REPLY(UInt32, SWITCH_AUDIO_RING_FRAMES);
    case kAudioDevicePropertyNominalSampleRate:
        REPLY(Float64, currentRate());
    case kAudioDevicePropertyAvailableNominalSampleRates: {
        Float64 rates[SWITCH_AUDIO_MAX_RATES];
        UInt32 n = availableRates(rates);
        if (capacity / sizeof(AudioValueRange) < n)
            n = capacity / (UInt32)sizeof(AudioValueRange);
        for (UInt32 i = 0; i < n; ++i)
            ((AudioValueRange *)data)[i] = (AudioValueRange){rates[i], rates[i]};
        *bytes = n * (UInt32)sizeof(AudioValueRange);
        return 0;
    }
    case kAudioDevicePropertyStreams:
        if (input)
            return 0;
        REPLY(AudioObjectID, kDisplayOut);
    case kAudioObjectPropertyControlList:
        return 0;
    case kAudioDevicePropertyPreferredChannelsForStereo:
        if (capacity < 2 * sizeof(UInt32))
            return kAudioHardwareBadPropertySizeError;
        ((UInt32 *)data)[0] = 1;
        ((UInt32 *)data)[1] = 2;
        *bytes = 2 * sizeof(UInt32);
        return 0;
    case kAudioDevicePropertyPreferredChannelLayout: {
        const UInt32 length = (UInt32)offsetof(AudioChannelLayout, mChannelDescriptions);
        if (capacity < length)
            return kAudioHardwareBadPropertySizeError;
        memset(data, 0, length);
        ((AudioChannelLayout *)data)->mChannelLayoutTag = kAudioChannelLayoutTag_Stereo;
        *bytes = length;
        return 0;
    }
    default:
        return kAudioHardwareUnknownPropertyError;
    }
}

Boolean switch_audio_display_settable(AudioObjectPropertySelector selector)
{
    return selector == kAudioStreamPropertyVirtualFormat || selector == kAudioStreamPropertyPhysicalFormat ||
           selector == kAudioStreamPropertyIsActive || selector == kAudioDevicePropertyNominalSampleRate;
}

OSStatus switch_audio_display_set(AudioObjectID object, const AudioObjectPropertyAddress *address, UInt32 bytes,
                                  const void *data)
{
    if (object == kDisplay && address->mSelector == kAudioDevicePropertyNominalSampleRate)
        return bytes < sizeof(Float64) ? kAudioHardwareBadPropertySizeError : requestRate(*(const Float64 *)data);
    if (object == kDisplayOut) {
        if (address->mSelector == kAudioStreamPropertyIsActive)
            return bytes >= sizeof(UInt32) ? 0 : kAudioHardwareBadPropertySizeError;
        if (address->mSelector == kAudioStreamPropertyVirtualFormat ||
            address->mSelector == kAudioStreamPropertyPhysicalFormat) {
            const AudioStreamBasicDescription *offered = data;
            if (bytes < sizeof(*offered))
                return kAudioDeviceUnsupportedFormatError;
            const AudioStreamBasicDescription wanted = format(offered->mSampleRate);
            return offered->mFormatID == wanted.mFormatID && offered->mFormatFlags == wanted.mFormatFlags &&
                           offered->mBytesPerPacket == wanted.mBytesPerPacket &&
                           offered->mFramesPerPacket == wanted.mFramesPerPacket &&
                           offered->mBytesPerFrame == wanted.mBytesPerFrame &&
                           offered->mChannelsPerFrame == wanted.mChannelsPerFrame &&
                           offered->mBitsPerChannel == wanted.mBitsPerChannel
                       ? requestRate(offered->mSampleRate)
                       : kAudioDeviceUnsupportedFormatError;
        }
    }
    return kAudioHardwareUnknownPropertyError;
}

OSStatus switch_audio_display_perform(io_connect_t connection, UInt64 rate)
{
    const uint64_t value = rate;
    const kern_return_t result =
        IOConnectCallScalarMethod(connection, SwitchAudioSetDisplayRate, &value, 1, NULL, NULL);
    if (result != KERN_SUCCESS) {
        os_log_error(OS_LOG_DEFAULT, "SwitchAudio: display rate %llu Hz refused (%#x)", (unsigned long long)rate,
                     result);
        return kAudioHardwareIllegalOperationError;
    }
    os_log(OS_LOG_DEFAULT, "SwitchAudio: display clock now %llu Hz", (unsigned long long)rate);
    return 0;
}

OSStatus switch_audio_display_start(io_connect_t connection)
{
    pthread_mutex_lock(&gDisplayLock);
    while (gDisplayTransitioning)
        pthread_cond_wait(&gDisplayCond, &gDisplayLock);
    if (!switch_audio_display_available() || !gControl) {
        pthread_mutex_unlock(&gDisplayLock);
        return kAudioHardwareNotRunningError;
    }
    gDisplayTransitioning = true;
    SwitchAudioControl *const control = gControl;
    const Boolean needMap = gOutput == NULL;
    const Boolean needStart = !(control->state & SwitchAudioOutputRunning);
    pthread_mutex_unlock(&gDisplayLock);

    void *mapped = NULL;
    if (needMap) {
        mach_vm_address_t address = 0;
        mach_vm_size_t size = 0;
        if (IOConnectMapMemory64(connection, SwitchAudioMemoryDisplayOutput, mach_task_self(), &address, &size,
                                 kIOMapAnywhere) != kIOReturnSuccess) {
            pthread_mutex_lock(&gDisplayLock);
            gDisplayTransitioning = false;
            pthread_cond_broadcast(&gDisplayCond);
            pthread_mutex_unlock(&gDisplayLock);
            return kAudioHardwareNotRunningError;
        }
        mapped = (void *)(uintptr_t)address;
    }
    kern_return_t result = KERN_SUCCESS;
    if (needStart)
        result = IOConnectCallScalarMethod(connection, SwitchAudioStartDisplayOutput, NULL, 0, NULL, NULL);

    pthread_mutex_lock(&gDisplayLock);
    if (mapped)
        gOutput = mapped;
    OSStatus status = 0;
    if (needStart && result != KERN_SUCCESS) {
        os_log_error(OS_LOG_DEFAULT, "SwitchAudio: display IO not started: the kernel service refused (%#x)", result);
        status = kAudioHardwareNotRunningError;
    } else {
        if (needStart) {
            ++gSeed;
            os_log(OS_LOG_DEFAULT, "SwitchAudio: display IO started (%u-bit, %u Hz)", control->sample_bits,
                   control->rate);
        }
        ++gUsers;
    }
    gDisplayTransitioning = false;
    pthread_cond_broadcast(&gDisplayCond);
    pthread_mutex_unlock(&gDisplayLock);
    return status;
}

OSStatus switch_audio_display_stop(io_connect_t connection)
{
    pthread_mutex_lock(&gDisplayLock);
    while (gDisplayTransitioning)
        pthread_cond_wait(&gDisplayCond, &gDisplayLock);
    Boolean stopNow = false;
    if (gUsers) {
        --gUsers;
        stopNow = gUsers == 0;
    }
    if (!stopNow) {
        pthread_mutex_unlock(&gDisplayLock);
        return 0;
    }
    gDisplayTransitioning = true;
    pthread_mutex_unlock(&gDisplayLock);

    (void)IOConnectCallScalarMethod(connection, SwitchAudioStopDisplayOutput, NULL, 0, NULL, NULL);
    os_log(OS_LOG_DEFAULT, "SwitchAudio: display IO stopped");

    pthread_mutex_lock(&gDisplayLock);
    gDisplayTransitioning = false;
    pthread_cond_broadcast(&gDisplayCond);
    pthread_mutex_unlock(&gDisplayLock);
    return 0;
}

OSStatus switch_audio_display_timestamp(Float64 *sample, UInt64 *host, UInt64 *seed)
{
    if (!gControl || !(gControl->state & SwitchAudioOutputRunning))
        return kAudioHardwareNotRunningError;
    const Float64 ticks = (Float64)gControl->ticks_per_frame_q32 / 4294967296.0;
    const uint64_t now = mach_absolute_time(), anchor = gControl->anchor_host;
    const uint64_t periods =
        now > anchor ? (uint64_t)((Float64)(now - anchor) / (ticks * SWITCH_AUDIO_RING_FRAMES)) : 0;
    *sample = (Float64)(periods * SWITCH_AUDIO_RING_FRAMES);
    *host = anchor + (uint64_t)((Float64)(periods * SWITCH_AUDIO_RING_FRAMES) * ticks);
    *seed = gSeed;
    return 0;
}

void switch_audio_display_mix(UInt32 frames, const AudioServerPlugInIOCycleInfo *info, const float *samples)
{
    if (!gControl || !gOutput || !switch_audio_display_available())
        return;
    const uint64_t first = (uint64_t)info->mOutputTime.mSampleTime;
    const uint32_t total = frames < SWITCH_AUDIO_RING_FRAMES ? frames : SWITCH_AUDIO_RING_FRAMES;
    const uint32_t at = (uint32_t)(first % SWITCH_AUDIO_RING_FRAMES);
    const uint32_t run = total < SWITCH_AUDIO_RING_FRAMES - at ? total : SWITCH_AUDIO_RING_FRAMES - at;
    const uint32_t width = gControl->sample_bits == 16 ? 2 : 4;
    if (width == 4) {
        int32_t *ring = gOutput;
        switch_audio_write_s32(ring + at * 2, samples, run * 2);
        if (run < total)
            switch_audio_write_s32(ring, samples + run * 2, (total - run) * 2);
    } else {
        int16_t *ring = gOutput;
        switch_audio_write_s16(ring + at * 2, samples, run * 2);
        if (run < total)
            switch_audio_write_s16(ring, samples + run * 2, (total - run) * 2);
    }
    switch_dcache_clean_lines((uint8_t *)gOutput + at * 2 * width, run * 2 * width);
    if (run < total)
        switch_dcache_clean_lines(gOutput, (total - run) * 2 * width);
    switch_dcache_sync();
    if (first + total > gControl->out_written)
        gControl->out_written = first + total;
}
