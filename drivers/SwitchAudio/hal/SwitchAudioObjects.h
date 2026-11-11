/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The plug-in's audio objects, one numbering for every iPad it plays as (SwitchAudioPlugIn.c's top comment). */
#ifndef SWITCH_AUDIO_OBJECTS_H
#define SWITCH_AUDIO_OBJECTS_H
#include <CoreAudio/AudioServerPlugIn.h>

enum {
    kPlugIn = kAudioObjectPlugInObject,
    kSpeaker = 2,
    kSpeakerOut = 3,
    kSpeakerTap = 4,
    kVolume = 5,
    kCodec = 6,
    kCodecIn = 7,
    kMic = 8,
    kMicIn = 9,
    kCodecOut = 10,
    kCodecVolume = 11,
    kCodecJackOut = 12,
    kCodecJackIn = 13,
    kCodecSources = 14,
    kMicSources = 15,
};

#endif
