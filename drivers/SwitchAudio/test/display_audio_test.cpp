// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gtest/gtest.h>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>
#include "../hal/SwitchDisplayAudio.h"
#include "../hal/SwitchAudioObjects.h"

namespace {
SwitchAudioControl *gKernelControl;
uint32_t gMaps, gStarts, gStops;
int32_t gMappedRing[2];
kern_return_t gMapResult;
std::mutex gHookMutex;
std::condition_variable gHookCv;
bool gBlockStartIoctl = false;
bool gInStartIoctl = false;
bool gReleaseStartIoctl = false;
}

extern "C" kern_return_t IOConnectMapMemory64(io_connect_t, uint32_t type, task_t, mach_vm_address_t *address,
                                              mach_vm_size_t *size, IOOptionBits)
{
    EXPECT_EQ(type, static_cast<uint32_t>(SwitchAudioMemoryDisplayOutput));
    ++gMaps;
    *address = reinterpret_cast<mach_vm_address_t>(gMappedRing);
    *size = sizeof(gMappedRing);
    return gMapResult;
}

extern "C" kern_return_t IOConnectCallScalarMethod(mach_port_t, uint32_t selector, const uint64_t *input,
                                                   uint32_t count, uint64_t *, uint32_t *)
{
    if (selector == SwitchAudioStartDisplayOutput) {
        {
            std::unique_lock<std::mutex> lock(gHookMutex);
            if (gBlockStartIoctl) {
                gInStartIoctl = true;
                gHookCv.notify_all();
                gHookCv.wait(lock, [] {
                    return gReleaseStartIoctl;
                });
            }
        }
        ++gStarts;
        gKernelControl->state = SwitchAudioOutputRunning;
    } else if (selector == SwitchAudioStopDisplayOutput) {
        ++gStops;
        gKernelControl->state = 0;
    } else if (selector == SwitchAudioSetDisplayRate && count == 1) {
        gKernelControl->rate = static_cast<uint32_t>(input[0]);
    } else {
        ADD_FAILURE() << "unexpected selector " << selector;
        return kIOReturnBadArgument;
    }
    return KERN_SUCCESS;
}

namespace {
struct Notification {
    AudioObjectID object;
    AudioObjectPropertySelector selector;
};

std::vector<Notification> gNotifications;
std::vector<std::pair<AudioObjectID, UInt64>> gConfigurationChanges;

OSStatus changed(AudioServerPlugInHostRef, AudioObjectID object, UInt32 count,
                 const AudioObjectPropertyAddress *addresses)
{
    for (UInt32 i = 0; i < count; ++i)
        gNotifications.push_back({object, addresses[i].mSelector});
    return 0;
}

OSStatus requestChange(AudioServerPlugInHostRef, AudioObjectID object, UInt64 action, void *)
{
    gConfigurationChanges.emplace_back(object, action);
    return 0;
}

AudioServerPlugInHostInterface gCallbacks = {changed, nullptr, nullptr, nullptr, requestChange};

class DisplayAudio : public testing::Test {
protected:
    SwitchAudioControl control{};

    void SetUp() override
    {
        gNotifications.clear();
        gConfigurationChanges.clear();
        gMaps = gStarts = gStops = 0;
        gMapResult = KERN_SUCCESS;
        gBlockStartIoctl = false;
        gInStartIoctl = false;
        gReleaseStartIoctl = false;
        gKernelControl = &control;
        switch_audio_display_attach(&gCallbacks, &control, nullptr);
    }

    void TearDown() override
    {
        switch_audio_display_attach(nullptr, nullptr, nullptr);
    }

    template <class T>
    T property(AudioObjectID object, AudioObjectPropertySelector selector,
               AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal)
    {
        T value{};
        UInt32 bytes = 0;
        const AudioObjectPropertyAddress address = {selector, scope, kAudioObjectPropertyElementMain};
        EXPECT_EQ(switch_audio_display_get(object, &address, sizeof(value), &bytes, &value), 0);
        EXPECT_EQ(bytes, sizeof(value));
        return value;
    }
};

TEST_F(DisplayAudio, PublishesRealSinkAvailabilityAndDeviceListChanges)
{
    EXPECT_FALSE(switch_audio_display_available());
    EXPECT_EQ(property<UInt32>(kDisplay, kAudioDevicePropertyDeviceIsAlive), 0u);
    control.jack = SwitchAudioJackHeadphones;
    switch_audio_display_changed();
    EXPECT_TRUE(switch_audio_display_available());
    ASSERT_EQ(gNotifications.size(), 3u);
    EXPECT_EQ(gNotifications[0].object, kDisplay);
    EXPECT_EQ(gNotifications[0].selector, kAudioDevicePropertyDeviceIsAlive);
    EXPECT_EQ(gNotifications[1].object, kPlugIn);
    EXPECT_EQ(gNotifications[1].selector, kAudioPlugInPropertyDeviceList);
    EXPECT_EQ(gNotifications[2].selector, kAudioObjectPropertyOwnedObjects);
    switch_audio_display_changed();
    EXPECT_EQ(gNotifications.size(), 3u);
    control.jack = 0;
    switch_audio_display_changed();
    EXPECT_FALSE(switch_audio_display_available());
    EXPECT_EQ(gNotifications.size(), 6u);
    EXPECT_EQ(switch_audio_display_start(0), kAudioHardwareNotRunningError);
}

TEST_F(DisplayAudio, HasDisplayPortTransportAndAnIndependent48kStereoClock)
{
    EXPECT_EQ(property<UInt32>(kDisplay, kAudioDevicePropertyTransportType), kAudioDeviceTransportTypeDisplayPort);
    EXPECT_EQ(property<UInt32>(kDisplayOut, kAudioStreamPropertyTerminalType), kAudioStreamTerminalTypeDisplayPort);
    EXPECT_NE(property<UInt32>(kDisplay, kAudioDevicePropertyClockDomain), SWITCH_AUDIO_MAGIC);
    const auto format = property<AudioStreamBasicDescription>(kDisplayOut, kAudioStreamPropertyPhysicalFormat);
    EXPECT_EQ(format.mSampleRate, 48000);
    EXPECT_EQ(format.mChannelsPerFrame, 2u);
    EXPECT_EQ(format.mFormatID, kAudioFormatLinearPCM);
    EXPECT_EQ(format.mBitsPerChannel, 32u);
    EXPECT_NE(format.mFormatFlags & kAudioFormatFlagIsFloat, 0u);
}

TEST_F(DisplayAudio, RejectsUnsupportedRatesAndHasNoInputStream)
{
    const Float64 unsupported = 96000;
    const AudioObjectPropertyAddress rate = {kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal,
                                             kAudioObjectPropertyElementMain};
    EXPECT_EQ(switch_audio_display_set(kDisplay, &rate, sizeof(unsupported), &unsupported),
              kAudioDeviceUnsupportedFormatError);
    UInt32 bytes = 100, value = 0;
    const AudioObjectPropertyAddress input = {kAudioDevicePropertyStreams, kAudioObjectPropertyScopeInput,
                                              kAudioObjectPropertyElementMain};
    EXPECT_EQ(switch_audio_display_get(kDisplay, &input, sizeof(value), &bytes, &value), 0);
    EXPECT_EQ(bytes, 0u);
    EXPECT_EQ(property<AudioObjectID>(kDisplay, kAudioDevicePropertyStreams, kAudioObjectPropertyScopeOutput),
              static_cast<AudioObjectID>(kDisplayOut));
}

TEST_F(DisplayAudio, OffersTheSinksRatesAndSwitchesThroughAConfigurationChange)
{
    control.rate = 48000;
    control.rates[0] = 48000;
    control.rates[1] = 44100;
    AudioValueRange ranges[4] = {};
    UInt32 bytes = 0;
    const AudioObjectPropertyAddress available = {kAudioDevicePropertyAvailableNominalSampleRates,
                                                  kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    EXPECT_EQ(switch_audio_display_get(kDisplay, &available, sizeof(ranges), &bytes, ranges), 0);
    ASSERT_EQ(bytes, 2 * sizeof(AudioValueRange));
    EXPECT_EQ(ranges[0].mMinimum, 48000);
    EXPECT_EQ(ranges[1].mMinimum, 44100);

    const AudioObjectPropertyAddress rate = {kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal,
                                             kAudioObjectPropertyElementMain};
    const Float64 current = 48000, other = 44100;
    EXPECT_EQ(switch_audio_display_set(kDisplay, &rate, sizeof(current), &current), 0);
    EXPECT_TRUE(gConfigurationChanges.empty());
    EXPECT_EQ(switch_audio_display_set(kDisplay, &rate, sizeof(other), &other), 0);
    ASSERT_EQ(gConfigurationChanges.size(), 1u);
    EXPECT_EQ(gConfigurationChanges[0].first, static_cast<AudioObjectID>(kDisplay));
    EXPECT_EQ(gConfigurationChanges[0].second, 44100u);

    EXPECT_EQ(switch_audio_display_perform(1, 44100), 0);
    EXPECT_EQ(control.rate, 44100u);
    EXPECT_EQ(property<Float64>(kDisplay, kAudioDevicePropertyNominalSampleRate), 44100);
    EXPECT_EQ(property<AudioStreamBasicDescription>(kDisplayOut, kAudioStreamPropertyPhysicalFormat).mSampleRate,
              44100);
}

TEST_F(DisplayAudio, TellsTheAudioServerWhenAReprobeChangesTheClock)
{
    control.rate = 44100;
    control.rates[0] = 48000;
    control.rates[1] = 44100;
    switch_audio_display_changed();
    ASSERT_EQ(gNotifications.size(), 6u);
    EXPECT_EQ(gNotifications[0].object, static_cast<AudioObjectID>(kDisplay));
    EXPECT_EQ(gNotifications[0].selector, kAudioDevicePropertyNominalSampleRate);
    EXPECT_EQ(gNotifications[1].selector, kAudioDevicePropertyAvailableNominalSampleRates);
    EXPECT_EQ(gNotifications[2].object, static_cast<AudioObjectID>(kDisplayOut));
    EXPECT_EQ(gNotifications[2].selector, kAudioStreamPropertyPhysicalFormat);
    switch_audio_display_changed();
    EXPECT_EQ(gNotifications.size(), 6u);
}

TEST_F(DisplayAudio, PlaybackClearingPeriodDoesNotIncreaseTheAudioServerWriteLead)
{
    control.period_frames = 16384;
    control.lead_frames = 960;
    EXPECT_EQ(property<UInt32>(kDisplay, kAudioDevicePropertyLatency), 960u);
    EXPECT_EQ(property<UInt32>(kDisplay, kAudioDevicePropertySafetyOffset), 1920u);
}

TEST_F(DisplayAudio, MapsPlaybackOnlyWhenIOStartsAndReusesItAcrossStarts)
{
    control.jack = SwitchAudioJackHeadphones;
    switch_audio_display_changed();
    EXPECT_EQ(gMaps, 0u);
    EXPECT_EQ(switch_audio_display_start(1), 0);
    EXPECT_EQ(switch_audio_display_start(1), 0);
    EXPECT_EQ(gMaps, 1u);
    EXPECT_EQ(gStarts, 1u);
    EXPECT_EQ(switch_audio_display_stop(1), 0);
    EXPECT_EQ(gStops, 0u);
    EXPECT_EQ(switch_audio_display_stop(1), 0);
    EXPECT_EQ(gStops, 1u);
    EXPECT_EQ(switch_audio_display_start(1), 0);
    EXPECT_EQ(gMaps, 1u);
    EXPECT_EQ(gStarts, 2u);
    EXPECT_EQ(switch_audio_display_stop(1), 0);
}

TEST_F(DisplayAudio, MappingFailureDoesNotStartDMA)
{
    control.jack = SwitchAudioJackHeadphones;
    switch_audio_display_changed();
    gMapResult = kIOReturnNoMemory;
    EXPECT_EQ(switch_audio_display_start(1), kAudioHardwareNotRunningError);
    EXPECT_EQ(gMaps, 1u);
    EXPECT_EQ(gStarts, 0u);
    EXPECT_EQ(control.state, 0u);
}

TEST_F(DisplayAudio, DropsDisplayLockAcrossBlockingStartIoctlWhileSerializingStop)
{
    control.jack = SwitchAudioJackHeadphones;
    switch_audio_display_changed();
    gBlockStartIoctl = true;

    std::thread starter([] {
        EXPECT_EQ(switch_audio_display_start(1), 0);
    });
    {
        std::unique_lock<std::mutex> lock(gHookMutex);
        gHookCv.wait(lock, [] {
            return gInStartIoctl;
        });
    }

    EXPECT_EQ(switch_audio_display_perform(1, 44100), 0);
    EXPECT_EQ(control.rate, 44100u);

    std::thread stopper([] {
        EXPECT_EQ(switch_audio_display_stop(1), 0);
    });
    {
        const std::lock_guard<std::mutex> lock(gHookMutex);
        gReleaseStartIoctl = true;
    }
    gHookCv.notify_all();
    starter.join();
    stopper.join();
    EXPECT_EQ(gStarts, 1u);
    EXPECT_EQ(gStops, 1u);
    EXPECT_EQ(control.state, 0u);
}
}
