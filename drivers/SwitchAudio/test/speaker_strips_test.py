"""The overlay's speaker strips bypass every DSP box of the IPSW kind's speaker chains but the volume.

So the built-in speaker plays the mix times the volume.

The boxes are Apple's: J120's speaker chain compiled into 21H461's VirtualAudio (its j120_speaker_*.austrip), and the
boxes of 24B119's /Library/Audio/Tunings/AID2003/VAD/speaker_*.dspg that J181's strips name, in their order.
"""

import json
import plistlib
import unittest
from pathlib import Path

from python.runfiles import runfiles

AUFX, APPL = 'aufx', 'appl'
VOLUME = ('vrq2', 'voli')
# The zero telemetry feeding speaker protection's second input (DSPGraph's own boxes, maker abox): left running
# and listed first, as J181's strips list them.
AID2003_STRUCTURE = (('ZeroConstantBox', 'cnst'), ('ZeroConstantInterleaveBox', 'srlc'))
AID2003_GENERAL = (
    ('AULoudnessNormalizer', 'ldnm'),
    ('AUVirtualBass', 'vben'),
    ('AUToneMeister_1', 'tmst'),
    ('AUVariableEQV2', 'vrq2'),
    ('AUMeisterStueck', 'mstk'),
    ('AUToneMeister_2', 'tmst'),
    ('AUControlFreak', 'cfrk'),
    ('AUSPP3', 'spp3'),
)
AID2003_MEASUREMENT = (
    ('AUNBEQ_1', 'nbeq'),
    ('AUNBEQ_2', 'nbeq'),
    ('AUVolume', 'voli'),
    ('AUSoftClip_1', 'sfcl'),
    ('AUSPP3', 'spp3'),
    ('AUSoftClip_2', 'sfcl'),
)
AID2003 = {
    'speaker_general': AID2003_GENERAL,
    'speaker_latenight_general': AID2003_GENERAL,
    'speaker_latenight_movie': AID2003_GENERAL,
    'speaker_movie': AID2003_GENERAL,
    'speaker_ringtone': AID2003_GENERAL,
    'speaker_measurement': AID2003_MEASUREMENT,
}


def fourcc(value: int) -> str:
    return value.to_bytes(4, 'big').decode()


def tree_file(path: str) -> Path:
    return Path(runfiles.Create().Rlocation('_main/' + path))


def manifest_strips(kind: str) -> dict[str, str]:
    manifest = json.loads(tree_file('overlay/manifest.json').read_text())
    return {
        e['path']: e['source'] for e in manifest['files'] if e['path'].endswith('.austrip') and e.get('kind') == kind
    }


def effects(source: str) -> list[dict]:
    with tree_file(source).open('rb') as handle:
        strip = plistlib.load(handle)
    (chain,) = strip['strips']
    return chain['effects']


class SpeakerStrips(unittest.TestCase):
    def assert_only_volume_runs(self, source: str, structure: int = 0) -> None:
        dsp = effects(source)[structure:]
        running = [fourcc(e['unit']['subtype']) for e in dsp if not e['bypass']]
        self.assertEqual(len(running), 1, source)
        self.assertIn(running[0], VOLUME, source)
        for effect in dsp:
            self.assertEqual((fourcc(effect['unit']['type']), fourcc(effect['unit']['manufacturer'])), (AUFX, APPL))

    def test_j181_strips_name_every_au_box_of_its_graphs(self):
        strips = manifest_strips('24B119')
        self.assertEqual(
            set(strips), {f'/Library/Audio/Tunings/AID2003/VAD/{name}.austrip' for name in AID2003}, 'AID2003/VAD'
        )
        for path, source in strips.items():
            name = Path(path).stem
            boxes = [(e['displayname'], fourcc(e['unit']['subtype'])) for e in effects(source)]
            self.assertEqual(tuple(boxes), AID2003_STRUCTURE + AID2003[name], name)
            for effect in effects(source)[: len(AID2003_STRUCTURE)]:
                self.assertEqual(
                    (fourcc(effect['unit']['type']), fourcc(effect['unit']['manufacturer']), effect['bypass']),
                    (AUFX, 'abox', 0),
                )
            self.assert_only_volume_runs(source, len(AID2003_STRUCTURE))

    def test_j120_strips_pass_through(self):
        strips = manifest_strips('21H461')
        self.assertEqual(len(strips), 6)
        for path, source in strips.items():
            self.assertTrue(path.startswith('/Library/Audio/Tunings/J120/VAD/j120_speaker_'), path)
            self.assert_only_volume_runs(source)


if __name__ == '__main__':
    unittest.main()
