/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* J181's jacks and input data sources as control objects of the plug-in's devices (24B119's plug-in only).
 *
 * The audio server answers a device's kAudioDevicePropertyJackIsConnected and kAudioDevicePropertyDataSources from
 * the device's jack control and data source control of that scope and element (24B119's CoreAudio,
 * HALB_Info::TranslateDeviceAddressToControlAddress); with no such control it asks a plug-in device only for the
 * custom properties it lists, so a device that answers those selectors itself is never asked. 24B119's VirtualAudio
 * needs both on the Codec: Device_Codec_Aspen reads its input data sources without asking whether it has any and drops
 * the whole device on the error ('who?', every iOS 27 boot since 11-08: no headphone or headset microphone port), and
 * builds the Wired Headphones and Wired Microphone ports only on a device whose jacks it finds.
 *  - the Codec's two jacks: output (a plug) and input (a plug with a microphone), the kernel's jack state;
 *  - the Codec's input data source, the headset microphone ('emic', VirtualAudio's source of the Wired Microphone),
 *    so VirtualAudio finds no built-in microphone on the codec, as on J181;
 *  - the Digital Mic's two, J181's ADT input-data-selectors ('imi1', 'imi2'), both always selected and read-only as
 *    J181's (input-data-source-read-only): VirtualAudio's built-in microphone port takes its sub-ports from them. */
#ifndef SWITCH_AUDIO_CONTROLS_H
#define SWITCH_AUDIO_CONTROLS_H
#include <CoreAudio/AudioServerPlugIn.h>
#include <stdint.h>

Boolean switch_audio_is_control(AudioObjectID object);

/* The controls of a device in a scope (global: all of them), at most room of them; returns how many it wrote. */
UInt32 switch_audio_controls_of(AudioObjectID device, AudioObjectPropertyScope scope, AudioObjectID *list, UInt32 room);

/* A control's property. jack is the kernel's jack state (SwitchAudioJack*), which the jack controls report. */
OSStatus switch_audio_control_property(AudioObjectID control, const AudioObjectPropertyAddress *address,
                                       UInt32 qualifierSize, const void *qualifier, uint32_t jack, UInt32 size,
                                       UInt32 *outSize, void *out);

#endif
