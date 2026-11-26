/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The interface between the kernel service SwitchAudio (SwitchAudio.cpp) and the audio server plug-in iPadOS loads
 * (hal/SwitchAudioPlugIn.c). Modelled on UniXNU-sheng's package/ShengAudioAbi.h.
 *
 * The plug-in opens the service, maps three memory types and starts and stops each direction. Memory type 0 is the
 * control page, 1 the playback ring, 2 the capture ring. Rings hold 32-bit little-endian signed interleaved samples
 * (S32_LE, full scale +-2^31): the playback ring ring_frames (SWITCH_AUDIO_RING_FRAMES) frames, the capture ring
 * SWITCH_AUDIO_IN_RING_FRAMES; sample time t lives at frame t modulo the ring's size. The Tegra backend hands them
 * to the APE as they are (SwitchAudioFormat.h): the codec plays the top 24 bits of each.
 * Memory types 3 and 4 are the display's separate control page and playback ring, on its own timeline at its own
 * rate (one of its rates[]: what the sink's EDID and the HDA codec both play, 48 and 44.1 kHz).
 * Its jack is nonzero only after the sink's EDID and HDA codec agree on stereo PCM. sample_bits selects packed
 * S16_LE or 24 significant bits in left-justified S32_LE containers; both rings use the same cache ownership.
 *
 * Sample time 0 is host time anchor_host, and one frame lasts ticks_per_frame_q32 / 2^32 host ticks. Hardware period
 * interrupts also publish a lock-free (hw_seq, hw_sample_time, hw_host_time, hw_seed) anchor for drift-free zero time
 * stamps. rate is the nominal client rate both directions run at now, one of rates[] (0 ends the list); the APE's SFC
 * converts non-native rates in hardware so the I2S link stays on the clean 48 kHz or 44.1 kHz PLLA family. */
#ifndef SWITCH_AUDIO_ABI_H
#define SWITCH_AUDIO_ABI_H
#include <stdint.h>

#define SWITCH_AUDIO_CLASS          "SwitchAudio"
#define SWITCH_AUDIO_MAGIC          0x53574155u /* 'SWAU' */
#define SWITCH_AUDIO_VERSION        9u
#define SWITCH_AUDIO_RATE           48000u /* the rate a service starts at */
#define SWITCH_AUDIO_MAX_RATES      10u
#define SWITCH_AUDIO_OUT_CHANNELS   2u
#define SWITCH_AUDIO_IN_CHANNELS    1u
#define SWITCH_AUDIO_PERIOD_FRAMES  480u   /* 10 ms: the null backend's step and output lead */
#define SWITCH_AUDIO_LEAD_FRAMES    480u   /* 10 ms at 48 kHz: past ADMA prefetch and IO wakeup jitter */
#define SWITCH_AUDIO_RING_FRAMES    65536u /* playback: 1.37 s at 48 kHz, 4 ADMA periods of 16384 frames */
#define SWITCH_AUDIO_IN_RING_FRAMES 16384u /* capture: 341 ms at 48 kHz, 8 ADMA periods of 2048 frames */
typedef int32_t switch_audio_sample_t;     /* a ring sample: S32_LE */

enum {
    SwitchAudioMemoryControl = 0,
    SwitchAudioMemoryOutput = 1,
    SwitchAudioMemoryInput = 2,
    SwitchAudioMemoryDisplayControl = 3,
    SwitchAudioMemoryDisplayOutput = 4
};

enum {
    SwitchAudioStartOutput = 0,
    SwitchAudioStopOutput = 1,
    SwitchAudioStartInput = 2,
    SwitchAudioStopInput = 3,
    /* One scalar: the address, in the caller, of CoreAudio's 24 MHz host clock constant (see SwitchAudio.cpp). */
    SwitchAudioSetHostClock = 4,
    /* One scalar: the nominal rate in Hz, one of rates[]. */
    SwitchAudioSetRate = 5,
    /* One scalar: the headphone output's attenuation below its full level in millidecibels, staged across the RT5639
     * analog HP PGA (1.5 dB steps from 0 to -46.5 dB) and APE MVC1 fine trim; past SWITCH_AUDIO_HEADPHONE_RANGE_MDB
     * mutes. */
    SwitchAudioSetHeadphoneLevel = 6,
    /* One scalar: 1 mutes output via APE MVC1 smooth ramp, 0 unmutes. */
    SwitchAudioSetMute = 7,
    /* One scalar: SwitchAudioRoute* hint from HAL so AVAudioSession route overrides (e.g. .speaker) take effect. */
    SwitchAudioSetRoute = 8,
    /* One scalar: speaker attenuation in millidecibels applied in APE MVC1 (0 = full level). */
    SwitchAudioSetSpeakerLevel = 9,
    SwitchAudioStartDisplayOutput = 10,
    SwitchAudioStopDisplayOutput = 11,
    /* One scalar: the display's nominal rate in Hz, one of its control page's rates[]; refused while it plays. */
    SwitchAudioSetDisplayRate = 12
};

enum {
    SwitchAudioRouteAuto = 0,
    SwitchAudioRouteSpeaker = 1,
    SwitchAudioRouteHeadphones = 2
};

#define SWITCH_AUDIO_HEADPHONE_RANGE_MDB 63750u
#define SWITCH_AUDIO_SPEAKER_RANGE_MDB   60000u
#define SWITCH_AUDIO_NO_CAPTURE          (~(uint64_t)0)

/* jack: what is plugged into the headphone jack. The service sends kIOMessageServicePropertyChange to its interest
 * clients each time it changes. */
enum {
    SwitchAudioJackHeadphones = 1u << 0,
    SwitchAudioJackMicrophone = 1u << 1
};

enum {
    SwitchAudioOutputRunning = 1u << 0,
    SwitchAudioInputRunning = 1u << 1
};

enum {
    SwitchAudioBackendNull = 0,
    SwitchAudioBackendTegraI2S = 1,
    SwitchAudioBackendTegraHDA = 2
};

typedef struct {
    uint32_t magic, version;
    volatile uint32_t rate;
    uint32_t out_channels, in_channels, period_frames, ring_frames, lead_frames;
    uint32_t backend;
    uint32_t rates[SWITCH_AUDIO_MAX_RATES];
    volatile uint32_t state;               /* SwitchAudio*Running */
    volatile uint32_t jack;                /* SwitchAudioJack* */
    volatile uint64_t ticks_per_frame_q32; /* host ticks per frame, 32.32 fixed point */
    volatile uint64_t anchor_host;         /* host time of sample time 0 */
    volatile uint64_t out_frames;          /* the service: playback consumed (and cleared) below this sample time */
    volatile uint64_t in_frames;           /* the service: capture valid below this sample time */
    volatile uint64_t in_anchor;           /* the service: the sample time of capture ring frame 0 while capture runs,
                                            * else SWITCH_AUDIO_NO_CAPTURE (switch_audio_capture_frame) */
    volatile uint64_t out_written;         /* the plug-in: the sample time after the last frame it wrote */
    volatile uint64_t io_cycles;           /* the plug-in: playback IO cycles (WriteMix calls) since it mapped this */
    volatile uint64_t io_frames;           /* the plug-in: frames those cycles wrote */
    /* Lock-free seqlock updated on stream start and ADMA period completion for hardware-anchored ZeroTimeStamp. */
    volatile uint32_t hw_seq;         /* even = stable, odd = writer updating */
    volatile uint32_t hw_seed;        /* incremented on every hardware timeline restart */
    volatile uint64_t hw_sample_time; /* exact hardware sample count at hw_host_time */
    volatile uint64_t hw_host_time;   /* host timestamp at hw_sample_time */
    volatile uint32_t active_route;   /* SwitchAudioRoute* currently driven */
    volatile uint32_t muted;          /* 1 = MVC1 output mute active */
    uint32_t sample_bits;
} SwitchAudioControl;

#endif
