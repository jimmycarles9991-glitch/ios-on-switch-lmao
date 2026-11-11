"""The built-in speaker: the kernel finds the RT5639, and audiomxd routes through the overlay's speaker strips.

Boots 15 and 16 (11-09, iOS 27) are the baseline: AudioDSPGraph refused every AID2003 speaker strip ('prs!', the
strips had no aupreset), the speaker's DSP chain failed, and VirtualAudio fell back to the null output, so nothing
played and the volume controls were greyed out. Boot 17 routed the speaker, but every boot since 11-08 found no codec
(NACK at I2C1 0x1c: SwitchAudio read it before PZ4's pad drove LDO1_EN), so the kernel ran the null backend.

Every iOS 27 audiomxd start since 11-08 also dropped the Codec device ('who?': VirtualAudio read its input data sources,
which 24B119's audio server answers only from a data source control) and had no built-in input, so a playAndRecord
session never activated ('!res') and its playback never started; and VirtualAudio converted the speaker graph's four
channels (left, left, right, right) to the stereo stream by keeping channels 0 and 1.
"""

from tools.devicerun.rules import REPORT, Check

OWNER = 'r124-audio27'
INPUT_OWNER = 'r130-audioin'
J181 = 'audio.devices uid=Digital Mic'
PLUGIN = 'drivers/SwitchAudio/hal/SwitchAudioPlugIn.c'
PORT = 'Created and published port'
AUDIOMXD = 'audiomxd'
KERNEL = 'kernel'
STRIP = "DSP graph '*.austrip'"
RT5639 = 'drivers/SwitchAudio/SwitchRT5639.cpp'

AUDIO = [
    Check(
        'audio.codec-ready',
        OWNER,
        'Tegra I2S1 + RT5639 ready',
        'count >= 1',
        process=KERNEL,
        source='drivers/SwitchAudio/SwitchAudio.cpp',
        note='the Tegra backend: boots since 11-08 never logged it',
    ),
    Check(
        'audio.codec-absent',
        OWNER,
        'no RT5639 on I2C1',
        process=KERNEL,
        regex=r'VENDOR_ID2 (?P<id>\w+), error (?P<error>-?\d+)',
        show=('error',),
        source=RT5639,
        note='boots 15 to 17: error -4 (NACK) every boot, the null backend',
    ),
    Check(
        'audio.speaker-strip',
        OWNER,
        f'setting {STRIP}',
        REPORT,
        process=AUDIOMXD,
        regex=r'\|(?P<chain>speaker_\w+)\] setting',
        show=('chain',),
        source='AudioDSPGraph (dyld cache)',
        note='the speaker chains audiomxd built',
    ),
    Check(
        'audio.strip-refused',
        OWNER,
        f'failed to set {STRIP}',
        process=AUDIOMXD,
        regex=r'\|(?P<chain>\w+)\] failed to set .*CADSPErrorStatus=(?P<status>-?\d+)',
        show=('chain', 'status'),
        needs='audio.speaker-strip',
        source='AudioDSPGraph (dyld cache)',
        note="boots 15 and 16: speaker_general, status 1886548769 ('prs!')",
    ),
    Check(
        'audio.routing-not-supported',
        OWNER,
        'Routing is not supported',
        process=AUDIOMXD,
        needs='audio.speaker-strip',
        source='VirtualAudio RoutingManager.cpp',
        note='every route activation after the speaker chain failed; boots 15 and 16',
    ),
    Check(
        'audio.devices',
        INPUT_OWNER,
        'Handling device state [Added] for Device ID',
        REPORT,
        process=AUDIOMXD,
        regex=r'for UID: (?P<uid>.+?)\s*$',
        show=('uid',),
        source='VirtualAudio HALDeviceManager.cpp',
        note="the plug-in's devices VirtualAudio took: Digital Mic only on iOS 27 (J181)",
    ),
    Check(
        'audio.device-dropped',
        INPUT_OWNER,
        "CAException caught by ExceptionBarrier: 'who?'",
        process=AUDIOMXD,
        source='drivers/SwitchAudio/hal/SwitchAudioControls.c',
        note='every iOS 27 audiomxd start since 11-08, once per Codec add: the Codec device dropped',
    ),
    Check(
        'audio.builtin-mic',
        INPUT_OWNER,
        PORT,
        'count >= 1',
        process=AUDIOMXD,
        regex=r'type: pmbi;',
        applies=J181,
        source=PLUGIN,
        note="J181's Built-In Microphone port, the Digital Mic's",
    ),
    Check(
        'audio.wired-ports',
        INPUT_OWNER,
        PORT,
        REPORT,
        process=AUDIOMXD,
        regex=r'type: (?P<port>phpw|pmwi);.*conn: (?P<connected>\d)',
        show=('port', 'connected'),
        applies=J181,
        source='drivers/SwitchAudio/hal/SwitchAudioControls.c',
        note="the Codec's Wired Headphones and Wired Microphone ports; none on iOS 27 before the Codec's controls",
    ),
    Check(
        'audio.input-device',
        INPUT_OWNER,
        'hasInputDevice=NO',
        process=AUDIOMXD,
        regex=r'for sid:\w+, (?P<client>[^(]+)\(',
        show=('client',),
        source=PLUGIN,
        note="boot 17: SwitchHWTest's playAndRecord session had no input device and its playback never started",
    ),
    Check(
        'audio.speaker-fold',
        INPUT_OWNER,
        'Created a new in process converter',
        process=AUDIOMXD,
        regex=r'from\s+4 ch,.* to\s+2 ch,',
        applies=J181,
        source=PLUGIN,
        note="boot 17: the speaker graph's four channels to a stereo stream kept channels 0 and 1, the left one twice",
    ),
]
