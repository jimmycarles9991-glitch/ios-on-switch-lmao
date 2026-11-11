"""audio.py's checks over boot 16's and 17's audiomxd lines (11-09, iOS 27)."""

import unittest

from tools.devicerun import devicerun
from tools.devicerun.audio import AUDIO
from tools.devicerun.rules import FAIL, INFO, NA, NO_DATA, PASS

SETTING = "[0xd49838040|speaker_general] setting DSP graph '*.austrip' at index 0"
REFUSED = (
    "[0xd49838040|speaker_general] failed to set DSP graph '*.austrip' at index 0 - Error Domain="
    'com.apple.audio.AudioDSPGraph Code=1702388592 "failed to set parameter strip" UserInfo={NSLocalizedDescription='
    'failed to set parameter strip, CADSPErrorSourceLine=261, CADSPErrorSourceFile=/Library/Caches/com.apple.xbs/'
    'Sources/AudioDSPGraph/Targets/Framework/AudioDSPGraph/CADSPGraph.mm, CADSPErrorStatus=1886548769}'
)
NOT_SUPPORTED = 'RoutingManager.cpp:3687  Routing is not supported: attempt to activate the routes failed.'
NO_CODEC = 'SwitchAudio: no RT5639 on I2C1 0x1c (VENDOR_ID2 0000, error -4): audio is the null backend'
CODEC_READY = (
    'SwitchAudio: Tegra I2S1 + RT5639 ready (playback and capture): S32_LE, I2S 32-bit slots at 64 fs (3072 kHz BCLK), '
    'RT5639 24-bit, 47999.878 Hz'
)
CODEC_ADDED = 'HALDeviceManager.cpp:283   Handling device state [Added] for Device ID: 41 for UID: Codec'
MIC_ADDED = 'HALDeviceManager.cpp:283   Handling device state [Added] for Device ID: 44 for UID: Digital Mic'
DROPPED = "StandardUtilities.h:673   CAException caught by ExceptionBarrier: 'who?' (:268)."
NO_INPUT_DEVICE = (
    '-CMSessionMgr- cmsShouldSkipBeginInterruption: Skipping begin interruption for sid:0x75008, SwitchHWTest(638), '
    "'prim' because input can't be made routable, session hasInput=YES, hasInputDevice=NO, session hwControlFlags=18."
)
CONVERTER = (
    'AudioConverter.cpp:1110  Created a new in process converter -> 0xcf94536f0, from  4 ch,  48000 Hz, Float32, '
    'deinterleaved to  {} ch,  48000 Hz, Float32, interleaved'
)


def port(kind: str, connected: int = 1) -> str:
    return (
        'VirtualAudio_PortManager.cpp:273   Created and published port: [ id: 79; addr: 0xcf8f3f410; core port: '
        f'[ type: {kind}; uid: Speaker; addr: 0xcf9271b18; conn: {connected}; rout: 1; quies: 0 ] ].'
    )


def record(message: str, second: int = 0, process: str = '/usr/sbin/audiomxd') -> dict:
    return {
        'timestamp': f'2026-11-09 03:39:{second:02d}.000000-0800',
        'bootUUID': '5D1A0C3E',
        'processImagePath': process,
        'subsystem': 'com.apple.coreaudio',
        'category': '',
        'messageType': 'Error',
        'eventMessage': message,
    }


KERNEL = '/System/Library/Caches/com.apple.kernelcaches/kernel.development'


def results(*records: dict) -> dict:
    found, _ = devicerun.run_checks(list(records), AUDIO)
    return {r.check.name: r for r in found}


class Audio(unittest.TestCase):
    def test_boot_16_fails_the_speaker_route(self):
        got = results(record(SETTING), record(REFUSED), record(NOT_SUPPORTED, second=1))
        self.assertEqual(got['audio.speaker-strip'].verdict, INFO)
        self.assertEqual(got['audio.strip-refused'].verdict, FAIL)
        self.assertIn('1886548769', got['audio.strip-refused'].observed)
        self.assertEqual(got['audio.routing-not-supported'].verdict, FAIL)

    def test_an_applied_strip_passes(self):
        got = results(record(SETTING))
        self.assertEqual(got['audio.strip-refused'].verdict, PASS)
        self.assertEqual(got['audio.routing-not-supported'].verdict, PASS)

    def test_boot_17_found_no_codec(self):
        got = results(record(NO_CODEC, process=KERNEL))
        self.assertEqual(got['audio.codec-ready'].verdict, FAIL)
        self.assertEqual(got['audio.codec-absent'].verdict, FAIL)
        self.assertIn('-4', got['audio.codec-absent'].observed)

    def test_a_found_codec_passes(self):
        got = results(record(CODEC_READY, process=KERNEL))
        self.assertEqual(got['audio.codec-ready'].verdict, PASS)
        self.assertEqual(got['audio.codec-absent'].verdict, PASS)

    def test_boot_17_dropped_the_codec_and_had_no_input(self):
        got = results(
            record(CODEC_ADDED),
            record(DROPPED),
            record(port('pspk')),
            record(CONVERTER.format(2), second=3),
            record(NO_INPUT_DEVICE, second=4),
        )
        self.assertEqual(got['audio.device-dropped'].verdict, FAIL)
        self.assertEqual(got['audio.input-device'].verdict, FAIL)
        self.assertIn('SwitchHWTest', got['audio.input-device'].observed)
        self.assertEqual(got['audio.builtin-mic'].verdict, NA)
        self.assertEqual(got['audio.speaker-fold'].verdict, NA)

    def test_j181_devices_pass(self):
        got = results(
            record(CODEC_ADDED),
            record(MIC_ADDED),
            record(port('pspk')),
            record(port('phpw', 0)),
            record(port('pmwi', 0)),
            record(port('pmbi')),
            record(CONVERTER.format(4), second=3),
        )
        self.assertEqual(got['audio.devices'].verdict, INFO)
        self.assertIn('Digital Mic', got['audio.devices'].observed)
        self.assertEqual(got['audio.device-dropped'].verdict, PASS)
        self.assertEqual(got['audio.builtin-mic'].verdict, PASS)
        self.assertEqual(got['audio.wired-ports'].verdict, INFO)
        self.assertIn('pmwi', got['audio.wired-ports'].observed)
        self.assertEqual(got['audio.input-device'].verdict, PASS)
        self.assertEqual(got['audio.speaker-fold'].verdict, PASS)

    def test_j181_left_only_speaker_fails(self):
        got = results(record(MIC_ADDED), record(CONVERTER.format(2), second=3))
        self.assertEqual(got['audio.speaker-fold'].verdict, FAIL)
        self.assertEqual(got['audio.builtin-mic'].verdict, FAIL)

    def test_no_speaker_chain_is_no_data(self):
        got = results()
        self.assertEqual(got['audio.strip-refused'].verdict, NO_DATA)
        self.assertEqual(got['audio.routing-not-supported'].verdict, NO_DATA)


if __name__ == '__main__':
    unittest.main()
