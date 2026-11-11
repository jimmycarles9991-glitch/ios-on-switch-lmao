// SPDX-License-Identifier: BSD-2-Clause-Patent
// J181's jack and data source controls (SwitchAudioControls.h). None is settable.
#include "SwitchAudioControls.h"

#include <string.h>

#include "SwitchAudioAbi.h"
#include "SwitchAudioObjects.h"

typedef struct {
    AudioObjectID object, device;
    AudioClassID kind;
    AudioObjectPropertyScope scope;
    uint32_t jack;          // a jack control: the SwitchAudioJack* bit it reports
    const UInt32 *sources;  // a data source control: its items, all of them selected
    UInt32 count;
} Control;

static const UInt32 kCodecItems[] = {'emic'};
static const UInt32 kMicItems[] = {'imi1', 'imi2'};

static const Control kControls[] = {
    {kCodecJackOut, kCodec, kAudioJackControlClassID, kAudioObjectPropertyScopeOutput, SwitchAudioJackHeadphones, NULL,
     0},
    {kCodecJackIn, kCodec, kAudioJackControlClassID, kAudioObjectPropertyScopeInput, SwitchAudioJackMicrophone, NULL,
     0},
    {kCodecSources, kCodec, kAudioDataSourceControlClassID, kAudioObjectPropertyScopeInput, 0, kCodecItems, 1},
    {kMicSources, kMic, kAudioDataSourceControlClassID, kAudioObjectPropertyScopeInput, 0, kMicItems, 2},
};

static const Control *find(AudioObjectID object)
{
    for (size_t i = 0; i < sizeof kControls / sizeof kControls[0]; ++i) {
        if (kControls[i].object == object)
            return &kControls[i];
    }
    return NULL;
}

Boolean switch_audio_is_control(AudioObjectID object)
{
    return find(object) != NULL;
}

UInt32 switch_audio_controls_of(AudioObjectID device, AudioObjectPropertyScope scope, AudioObjectID *list, UInt32 room)
{
    UInt32 n = 0;
    for (size_t i = 0; i < sizeof kControls / sizeof kControls[0] && n < room; ++i) {
        const Control *c = &kControls[i];
        if (c->device == device && (scope == kAudioObjectPropertyScopeGlobal || scope == c->scope))
            list[n++] = c->object;
    }
    return n;
}

static CFStringRef sourceName(UInt32 item)
{
    switch (item) {
    case 'emic':
        return CFSTR("Headset Microphone");
    case 'imi1':
        return CFSTR("Microphone 1");
    case 'imi2':
        return CFSTR("Microphone 2");
    default:
        return NULL;
    }
}

static Boolean isSource(const Control *c, UInt32 item)
{
    for (UInt32 i = 0; i < c->count; ++i) {
        if (c->sources[i] == item)
            return true;
    }
    return false;
}

#define REPLY(type, value)                             \
    do {                                               \
        if (size < sizeof(type))                       \
            return kAudioHardwareBadPropertySizeError; \
        *(type *)out = (value);                        \
        *outSize = sizeof(type);                       \
        return 0;                                      \
    } while (0)

OSStatus switch_audio_control_property(AudioObjectID control, const AudioObjectPropertyAddress *address,
                                       UInt32 qualifierSize, const void *qualifier, uint32_t jack, UInt32 size,
                                       UInt32 *outSize, void *out)
{
    const Control *c = find(control);
    if (!c)
        return kAudioHardwareBadObjectError;
    const Boolean selector = c->kind == kAudioDataSourceControlClassID;
    switch (address->mSelector) {
    case kAudioObjectPropertyBaseClass:
        REPLY(AudioClassID, selector ? kAudioSelectorControlClassID : kAudioBooleanControlClassID);
    case kAudioObjectPropertyClass:
        REPLY(AudioClassID, c->kind);
    case kAudioObjectPropertyOwner:
        REPLY(AudioObjectID, c->device);
    case kAudioObjectPropertyOwnedObjects:
    case kAudioObjectPropertyCustomPropertyInfoList:
        *outSize = 0;
        return 0;
    case kAudioControlPropertyScope:
        REPLY(AudioObjectPropertyScope, c->scope);
    case kAudioControlPropertyElement:
        REPLY(AudioObjectPropertyElement, kAudioObjectPropertyElementMain);
    case kAudioBooleanControlPropertyValue:
        if (selector)
            return kAudioHardwareUnknownPropertyError;
        REPLY(UInt32, (jack & c->jack) != 0);
    case kAudioSelectorControlPropertyCurrentItem:
    case kAudioSelectorControlPropertyAvailableItems: {
        if (!selector)
            return kAudioHardwareUnknownPropertyError;
        const UInt32 n = size / sizeof(UInt32) < c->count ? size / (UInt32)sizeof(UInt32) : c->count;
        const UInt32 bytes = n * (UInt32)sizeof(UInt32);
        memcpy(out, c->sources, bytes);
        *outSize = bytes;
        return 0;
    }
    case kAudioSelectorControlPropertyItemName:
    case kAudioSelectorControlPropertyItemKind: {
        if (!selector)
            return kAudioHardwareUnknownPropertyError;
        // Without an item this is the plug-in's HasProperty and GetPropertyDataSize, which ask with none.
        const Boolean any = qualifierSize < sizeof(UInt32);
        if (!any && !isSource(c, *(const UInt32 *)qualifier))
            return kAudioHardwareIllegalOperationError;
        if (address->mSelector == kAudioSelectorControlPropertyItemKind)
            REPLY(UInt32, 0);
        REPLY(CFStringRef, any ? CFSTR("") : sourceName(*(const UInt32 *)qualifier));
    }
    default:
        return kAudioHardwareUnknownPropertyError;
    }
}
