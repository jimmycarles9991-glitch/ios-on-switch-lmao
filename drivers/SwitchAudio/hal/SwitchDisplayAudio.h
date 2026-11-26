// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <CoreAudio/AudioServerPlugIn.h>
#include <IOKit/IOKitLib.h>
#include "../SwitchAudioAbi.h"

#define SWITCH_AUDIO_DISPLAY_UID "Switch DisplayPort"

#ifdef __cplusplus
extern "C" {
#endif
Boolean switch_audio_display_object(AudioObjectID object);
void switch_audio_display_attach(AudioServerPlugInHostRef host, SwitchAudioControl *control, void *output);
Boolean switch_audio_display_available(void);
void switch_audio_display_changed(void);
Boolean switch_audio_display_settable(AudioObjectPropertySelector selector);
OSStatus switch_audio_display_get(AudioObjectID object, const AudioObjectPropertyAddress *address, UInt32 capacity,
                                  UInt32 *bytes, void *data);
OSStatus switch_audio_display_set(AudioObjectID object, const AudioObjectPropertyAddress *address, UInt32 bytes,
                                  const void *data);
OSStatus switch_audio_display_perform(io_connect_t connection, UInt64 rate);
OSStatus switch_audio_display_start(io_connect_t connection);
OSStatus switch_audio_display_stop(io_connect_t connection);
OSStatus switch_audio_display_timestamp(Float64 *sample, UInt64 *host, UInt64 *seed);
void switch_audio_display_mix(UInt32 frames, const AudioServerPlugInIOCycleInfo *info, const float *samples);
#ifdef __cplusplus
}
#endif
