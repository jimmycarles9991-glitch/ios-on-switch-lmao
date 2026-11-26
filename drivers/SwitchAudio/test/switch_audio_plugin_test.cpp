// SwitchAudio's HAL plug-in through its AudioServerPlugIn driver interface, as the audio server calls it, for the iPad
// it is built for: J120 (21H461) or J181 (24B119, SWITCH_AUDIO_J181). On the Mac it finds no kernel service, which
// leaves the jacks empty and the rates at the service's default.
#include <CoreAudio/AudioServerPlugIn.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <tuple>
#include <vector>

#include "../hal/SwitchAudioObjects.h"
#include "../hal/SwitchDisplayAudio.h"

#ifndef SWITCH_AUDIO_J181
#define SWITCH_AUDIO_J181 0
#endif

extern "C" void *SwitchAudio_Create(CFAllocatorRef allocator, CFUUIDRef type);

namespace {

using testing::ElementsAre;
using testing::IsEmpty;
using testing::UnorderedElementsAre;

constexpr bool kJ181 = SWITCH_AUDIO_J181;

OSStatus HostPropertiesChanged(AudioServerPlugInHostRef, AudioObjectID, UInt32, const AudioObjectPropertyAddress *)
{
    return 0;
}

OSStatus HostRequestConfigurationChange(AudioServerPlugInHostRef, AudioObjectID, UInt64, void *)
{
    return 0;
}

AudioServerPlugInHostInterface gHostInterface = {HostPropertiesChanged, nullptr, nullptr, nullptr,
                                                 HostRequestConfigurationChange};

class PlugIn : public testing::Test {
protected:
    static void SetUpTestSuite()
    {
        driver_ = static_cast<AudioServerPlugInDriverRef>(
            SwitchAudio_Create(kCFAllocatorDefault, kAudioServerPlugInTypeUUID));
        ASSERT_NE(driver_, nullptr);
        ASSERT_EQ((*driver_)->Initialize(driver_, &gHostInterface), 0);
    }

    static OSStatus Get(AudioObjectID object, AudioObjectPropertySelector selector, std::vector<uint8_t> &out,
                        AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal, UInt32 qualifierSize = 0,
                        const void *qualifier = nullptr)
    {
        const AudioObjectPropertyAddress address = {selector, scope, kAudioObjectPropertyElementMain};
        out.assign(1024, 0);
        UInt32 size = 0;
        const OSStatus status = (*driver_)->GetPropertyData(driver_, object, 0, &address, qualifierSize, qualifier,
                                                            (UInt32)out.size(), &size, out.data());
        out.resize(status ? 0 : size);
        return status;
    }

    template <typename T>
    static T Value(AudioObjectID object, AudioObjectPropertySelector selector,
                   AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal, UInt32 qualifierSize = 0,
                   const void *qualifier = nullptr)
    {
        std::vector<uint8_t> out;
        EXPECT_EQ(Get(object, selector, out, scope, qualifierSize, qualifier), 0)
            << object << " " << std::string((const char *)&selector, 4);
        T value{};
        if (out.size() == sizeof value)
            memcpy(&value, out.data(), sizeof value);
        return value;
    }

    static std::vector<UInt32> List(AudioObjectID object, AudioObjectPropertySelector selector,
                                    AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal)
    {
        std::vector<uint8_t> out;
        EXPECT_EQ(Get(object, selector, out, scope), 0) << object;
        std::vector<UInt32> values(out.size() / sizeof(UInt32));
        memcpy(values.data(), out.data(), values.size() * sizeof(UInt32));
        return values;
    }

    static std::string String(AudioObjectID object, AudioObjectPropertySelector selector, UInt32 qualifierSize = 0,
                              const void *qualifier = nullptr)
    {
        const CFStringRef value =
            Value<CFStringRef>(object, selector, kAudioObjectPropertyScopeGlobal, qualifierSize, qualifier);
        char text[128] = {};
        if (value)
            CFStringGetCString(value, text, sizeof text, kCFStringEncodingUTF8);
        return text;
    }

    static bool Settable(AudioObjectID object, AudioObjectPropertySelector selector)
    {
        const AudioObjectPropertyAddress address = {selector, kAudioObjectPropertyScopeGlobal,
                                                    kAudioObjectPropertyElementMain};
        Boolean settable = true;
        EXPECT_EQ((*driver_)->IsPropertySettable(driver_, object, 0, &address, &settable), 0);
        return settable;
    }

    static AudioObjectID DeviceWithUID(CFStringRef uid)
    {
        return Value<AudioObjectID>(kPlugIn, kAudioPlugInPropertyTranslateUIDToDevice, kAudioObjectPropertyScopeGlobal,
                                    sizeof uid, &uid);
    }

    static AudioServerPlugInDriverRef driver_;
};

AudioServerPlugInDriverRef PlugIn::driver_;

// VirtualAudio's DeviceFactory_Aspen knows a built-in device by its UID: J120's speaker, codec and Hawking, or J181's
// speaker, codec and built-in microphone.
TEST_F(PlugIn, DevicesAreTheIPadsOwn)
{
    EXPECT_THAT(List(kPlugIn, kAudioPlugInPropertyDeviceList), ElementsAre(kSpeaker, kCodec, kMic));
    EXPECT_EQ(DeviceWithUID(CFSTR("Speaker")), (AudioObjectID)kSpeaker);
    EXPECT_EQ(DeviceWithUID(CFSTR("Codec")), (AudioObjectID)kCodec);
    EXPECT_EQ(DeviceWithUID(CFSTR("Digital Mic")), kJ181 ? (AudioObjectID)kMic : kAudioObjectUnknown);
    EXPECT_EQ(DeviceWithUID(CFSTR("Hawking")), kJ181 ? kAudioObjectUnknown : (AudioObjectID)kMic);
    EXPECT_EQ(String(kMic, kAudioDevicePropertyDeviceUID), kJ181 ? "Digital Mic" : "Hawking");
}

TEST_F(PlugIn, DisplayDeviceFollowsSinkAvailabilityThroughThePluginInterface)
{
    SwitchAudioControl control{};
    switch_audio_display_attach(&gHostInterface, &control, nullptr);
    EXPECT_THAT(List(kPlugIn, kAudioPlugInPropertyDeviceList), ElementsAre(kSpeaker, kCodec, kMic));
    control.jack = SwitchAudioJackHeadphones;
    switch_audio_display_changed();
    EXPECT_THAT(List(kPlugIn, kAudioPlugInPropertyDeviceList), ElementsAre(kSpeaker, kCodec, kMic, kDisplay));
    EXPECT_EQ(DeviceWithUID(CFSTR(SWITCH_AUDIO_DISPLAY_UID)), static_cast<AudioObjectID>(kDisplay));
    EXPECT_EQ(Value<UInt32>(kDisplay, kAudioDevicePropertyTransportType),
              static_cast<UInt32>(kAudioDeviceTransportTypeDisplayPort));
    EXPECT_THAT(List(kDisplay, kAudioDevicePropertyStreams, kAudioObjectPropertyScopeOutput), ElementsAre(kDisplayOut));
    EXPECT_THAT(List(kDisplay, kAudioDevicePropertyStreams, kAudioObjectPropertyScopeInput), IsEmpty());
    const AudioObjectPropertyAddress transport = {kAudioDevicePropertyTransportType, kAudioObjectPropertyScopeGlobal,
                                                  kAudioObjectPropertyElementMain};
    const AudioObjectPropertyAddress unknown = {'nope', kAudioObjectPropertyScopeGlobal,
                                                kAudioObjectPropertyElementMain};
    EXPECT_TRUE((*driver_)->HasProperty(driver_, kDisplay, 0, &transport));
    EXPECT_FALSE((*driver_)->HasProperty(driver_, kDisplay, 0, &unknown));
    EXPECT_TRUE(Settable(kDisplay, kAudioDevicePropertyNominalSampleRate));
    EXPECT_FALSE(Settable(kDisplay, kAudioDevicePropertyTransportType));
    EXPECT_TRUE(Settable(kDisplayOut, kAudioStreamPropertyPhysicalFormat));
    Boolean will = true, inPlace = false;
    EXPECT_EQ(
        (*driver_)->WillDoIOOperation(driver_, kDisplay, 0, kAudioServerPlugInIOOperationReadInput, &will, &inPlace),
        0);
    EXPECT_FALSE(will);
    EXPECT_TRUE(inPlace);
    control.jack = 0;
    switch_audio_display_changed();
    EXPECT_THAT(List(kPlugIn, kAudioPlugInPropertyDeviceList), ElementsAre(kSpeaker, kCodec, kMic));
    EXPECT_EQ(DeviceWithUID(CFSTR(SWITCH_AUDIO_DISPLAY_UID)), kAudioObjectUnknown);
    switch_audio_display_attach(nullptr, nullptr, nullptr);
}

// The built-in input: a silent microphone stream, one channel on J120, J181's two microphones on J181 (AID2003's mic
// graphs take two channels).
TEST_F(PlugIn, BuiltInInputIsAMicrophoneStream)
{
    EXPECT_THAT(List(kMic, kAudioDevicePropertyStreams, kAudioObjectPropertyScopeInput), ElementsAre(kMicIn));
    EXPECT_THAT(List(kMic, kAudioDevicePropertyStreams, kAudioObjectPropertyScopeOutput), IsEmpty());
    EXPECT_EQ(Value<UInt32>(kMicIn, kAudioStreamPropertyDirection), 1u);
    EXPECT_EQ(Value<UInt32>(kMicIn, kAudioStreamPropertyTerminalType), (UInt32)kAudioStreamTerminalTypeMicrophone);
    const auto format = Value<AudioStreamBasicDescription>(kMicIn, kAudioStreamPropertyVirtualFormat);
    EXPECT_EQ(format.mChannelsPerFrame, kJ181 ? 2u : 1u);
    EXPECT_EQ(format.mBytesPerFrame, format.mChannelsPerFrame * 4);
}

// J181's speaker stream has its four channels so VirtualAudio passes the speaker graph's left, left, right, right
// through instead of keeping channels 0 and 1. Its layout uses Unknown labels and {1, 2} stereo pair (matching
// ASDAudioDevice/ASDStream) so VirtualAudio forwarding 'srnd' onto its 2-ch vdef stream does not drop right.
TEST_F(PlugIn, SpeakerHasTheIPadsChannels)
{
    const auto format = Value<AudioStreamBasicDescription>(kSpeakerOut, kAudioStreamPropertyVirtualFormat);
    EXPECT_EQ(format.mChannelsPerFrame, kJ181 ? 4u : 2u);
    const auto stereo = List(kSpeaker, kAudioDevicePropertyPreferredChannelsForStereo, kAudioObjectPropertyScopeOutput);
    EXPECT_THAT(stereo, ElementsAre(1u, 2u));

    std::vector<uint8_t> out;
    ASSERT_EQ(Get(kSpeaker, kAudioDevicePropertyPreferredChannelLayout, out, kAudioObjectPropertyScopeOutput), 0);
    const auto *layout = reinterpret_cast<const AudioChannelLayout *>(out.data());
    EXPECT_EQ(layout->mChannelLayoutTag, kAudioChannelLayoutTag_UseChannelDescriptions);
    std::vector<AudioChannelLabel> labels;
    for (UInt32 i = 0; i < layout->mNumberChannelDescriptions; ++i)
        labels.push_back(layout->mChannelDescriptions[i].mChannelLabel);
    if (kJ181) {
        EXPECT_THAT(labels, ElementsAre(kAudioChannelLabel_Unknown, kAudioChannelLabel_Unknown,
                                        kAudioChannelLabel_Unknown, kAudioChannelLabel_Unknown));
    } else {
        EXPECT_THAT(labels, ElementsAre(kAudioChannelLabel_Left, kAudioChannelLabel_Right));
    }

    AudioStreamBasicDescription stereoFormat = format;
    stereoFormat.mChannelsPerFrame = 2;
    stereoFormat.mBytesPerFrame = stereoFormat.mBytesPerPacket = 8;
    const AudioObjectPropertyAddress address = {kAudioStreamPropertyVirtualFormat, kAudioObjectPropertyScopeGlobal,
                                                kAudioObjectPropertyElementMain};
    const OSStatus set =
        (*driver_)->SetPropertyData(driver_, kSpeakerOut, 0, &address, 0, nullptr, sizeof stereoFormat, &stereoFormat);
    EXPECT_EQ(set, kJ181 ? (OSStatus)kAudioDeviceUnsupportedFormatError : 0);
}

// 24B119's audio server answers the Codec's 'jack' and 'ssc#' only from controls of that scope; VirtualAudio's
// Device_Codec_Aspen reads the input data sources unasked and drops the device on an error. J120 keeps 21H461's.
TEST_F(PlugIn, CodecControls)
{
    const auto all = List(kCodec, kAudioObjectPropertyControlList);
    const auto input = List(kCodec, kAudioObjectPropertyControlList, kAudioObjectPropertyScopeInput);
    const auto output = List(kCodec, kAudioObjectPropertyControlList, kAudioObjectPropertyScopeOutput);
    if (!kJ181) {
        EXPECT_THAT(all, UnorderedElementsAre(kCodecVolume, kCodecMute));
        EXPECT_THAT(input, IsEmpty());
        EXPECT_THAT(output, UnorderedElementsAre(kCodecVolume, kCodecMute));
        std::vector<uint8_t> out;
        EXPECT_EQ(Get(kCodecJackOut, kAudioObjectPropertyClass, out), kAudioHardwareBadObjectError);
        return;
    }
    EXPECT_THAT(all, UnorderedElementsAre(kCodecVolume, kCodecMute, kCodecJackOut, kCodecJackIn, kCodecSources));
    EXPECT_THAT(input, UnorderedElementsAre(kCodecJackIn, kCodecSources));
    EXPECT_THAT(output, UnorderedElementsAre(kCodecVolume, kCodecMute, kCodecJackOut));
    EXPECT_THAT(List(kCodec, kAudioObjectPropertyOwnedObjects),
                UnorderedElementsAre(kCodecIn, kCodecOut, kCodecVolume, kCodecMute, kCodecJackOut, kCodecJackIn,
                                     kCodecSources));

    for (const auto [jack, scope] : {std::pair{kCodecJackOut, kAudioObjectPropertyScopeOutput},
                                     std::pair{kCodecJackIn, kAudioObjectPropertyScopeInput}}) {
        EXPECT_EQ(Value<AudioClassID>(jack, kAudioObjectPropertyClass), (AudioClassID)kAudioJackControlClassID);
        EXPECT_EQ(Value<AudioClassID>(jack, kAudioObjectPropertyBaseClass), (AudioClassID)kAudioBooleanControlClassID);
        EXPECT_EQ(Value<AudioObjectID>(jack, kAudioObjectPropertyOwner), (AudioObjectID)kCodec);
        EXPECT_EQ(Value<AudioObjectPropertyScope>(jack, kAudioControlPropertyScope), scope);
        EXPECT_EQ(Value<AudioObjectPropertyElement>(jack, kAudioControlPropertyElement),
                  kAudioObjectPropertyElementMain);
        EXPECT_EQ(Value<UInt32>(jack, kAudioBooleanControlPropertyValue), 0u);  // no kernel service, no plug
        EXPECT_FALSE(Settable(jack, kAudioBooleanControlPropertyValue));
    }

    EXPECT_EQ(Value<AudioClassID>(kCodecSources, kAudioObjectPropertyClass),
              (AudioClassID)kAudioDataSourceControlClassID);
    EXPECT_EQ(Value<AudioClassID>(kCodecSources, kAudioObjectPropertyBaseClass),
              (AudioClassID)kAudioSelectorControlClassID);
    EXPECT_EQ(Value<AudioObjectPropertyScope>(kCodecSources, kAudioControlPropertyScope),
              kAudioObjectPropertyScopeInput);
    EXPECT_THAT(List(kCodecSources, kAudioSelectorControlPropertyAvailableItems), ElementsAre('emic'));
    EXPECT_THAT(List(kCodecSources, kAudioSelectorControlPropertyCurrentItem), ElementsAre('emic'));
    const UInt32 emic = 'emic';
    EXPECT_EQ(String(kCodecSources, kAudioSelectorControlPropertyItemName, sizeof emic, &emic), "Headset Microphone");
    EXPECT_FALSE(Settable(kCodecSources, kAudioSelectorControlPropertyCurrentItem));
}

// J181's built-in microphone has its two input data sources, as its ADT's input-data-selectors ('1imi2imi'), both
// selected and read-only: VirtualAudio's Built-In Microphone port takes its sub-ports from them.
TEST_F(PlugIn, DigitalMicSources)
{
    if (!kJ181) {
        EXPECT_THAT(List(kMic, kAudioObjectPropertyControlList), IsEmpty());
        return;
    }
    EXPECT_THAT(List(kMic, kAudioObjectPropertyControlList, kAudioObjectPropertyScopeInput), ElementsAre(kMicSources));
    EXPECT_THAT(List(kMic, kAudioObjectPropertyControlList, kAudioObjectPropertyScopeOutput), IsEmpty());
    EXPECT_EQ(Value<AudioObjectID>(kMicSources, kAudioObjectPropertyOwner), (AudioObjectID)kMic);
    EXPECT_THAT(List(kMicSources, kAudioSelectorControlPropertyAvailableItems), ElementsAre('imi1', 'imi2'));
    EXPECT_THAT(List(kMicSources, kAudioSelectorControlPropertyCurrentItem), ElementsAre('imi1', 'imi2'));
    const UInt32 unknown = 'emic';
    std::vector<uint8_t> out;
    EXPECT_EQ(Get(kMicSources, kAudioSelectorControlPropertyItemName, out, kAudioObjectPropertyScopeGlobal,
                  sizeof unknown, &unknown),
              kAudioHardwareIllegalOperationError);
    EXPECT_FALSE(Settable(kMicSources, kAudioSelectorControlPropertyCurrentItem));
}

TEST_F(PlugIn, OutputMuteControlsAndTiming)
{
    EXPECT_THAT(List(kSpeaker, kAudioObjectPropertyControlList, kAudioObjectPropertyScopeOutput),
                UnorderedElementsAre(kVolume, kSpeakerMute));
    for (const auto [mute, owner, elem] :
         {std::tuple<AudioObjectID, AudioObjectID, AudioObjectPropertyElement>{kSpeakerMute, kSpeaker,
                                                                               kAudioObjectPropertyElementMain},
          std::tuple<AudioObjectID, AudioObjectID, AudioObjectPropertyElement>{kCodecMute, kCodec, 1u}}) {
        EXPECT_EQ(Value<AudioClassID>(mute, kAudioObjectPropertyClass), (AudioClassID)kAudioMuteControlClassID);
        EXPECT_EQ(Value<AudioClassID>(mute, kAudioObjectPropertyBaseClass), (AudioClassID)kAudioBooleanControlClassID);
        EXPECT_EQ(Value<AudioObjectID>(mute, kAudioObjectPropertyOwner), owner);
        EXPECT_EQ(Value<AudioObjectPropertyScope>(mute, kAudioControlPropertyScope), kAudioObjectPropertyScopeOutput);
        EXPECT_EQ(Value<AudioObjectPropertyElement>(mute, kAudioControlPropertyElement), elem);
        EXPECT_TRUE(Settable(mute, kAudioBooleanControlPropertyValue));
        const AudioObjectPropertyAddress addr = {kAudioBooleanControlPropertyValue, kAudioObjectPropertyScopeGlobal,
                                                 kAudioObjectPropertyElementMain};
        UInt32 on = 1u, off = 0u;
        EXPECT_EQ((*driver_)->SetPropertyData(driver_, mute, 0, &addr, 0, nullptr, sizeof on, &on), 0);
        EXPECT_EQ(Value<UInt32>(mute, kAudioBooleanControlPropertyValue), 1u);
        EXPECT_EQ((*driver_)->SetPropertyData(driver_, mute, 0, &addr, 0, nullptr, sizeof off, &off), 0);
        EXPECT_EQ(Value<UInt32>(mute, kAudioBooleanControlPropertyValue), 0u);
    }

    EXPECT_EQ(Value<UInt32>(kSpeaker, kAudioDevicePropertyLatency), 480u);
    EXPECT_EQ(Value<UInt32>(kSpeaker, kAudioDevicePropertySafetyOffset, kAudioObjectPropertyScopeOutput), 960u);
    EXPECT_EQ(Value<UInt32>(kSpeaker, kAudioDevicePropertyZeroTimeStampPeriod), 16384u);
}

// The audio server drops an object whose custom property list does not answer (empty here).
TEST_F(PlugIn, EveryObjectListsNoCustomProperties)
{
    std::vector<AudioObjectID> objects = {kPlugIn,  kSpeaker,  kSpeakerOut,  kSpeakerTap, kVolume, kSpeakerMute, kCodec,
                                          kCodecIn, kCodecOut, kCodecVolume, kCodecMute,  kMic,    kMicIn};
    if (kJ181)
        objects.insert(objects.end(), {kCodecJackOut, kCodecJackIn, kCodecSources, kMicSources});
    for (const AudioObjectID object : objects) {
        std::vector<uint8_t> out;
        EXPECT_EQ(Get(object, kAudioObjectPropertyCustomPropertyInfoList, out), 0) << object;
        EXPECT_THAT(out, IsEmpty()) << object;
    }
}

}  // namespace
