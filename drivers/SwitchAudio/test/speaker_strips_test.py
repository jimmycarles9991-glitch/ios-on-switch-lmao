"""The overlay's speaker strips bypass every DSP box of the IPSW kind's speaker chains but the volume.

So the built-in speaker plays the mix times the volume.

The boxes are Apple's: J120's speaker chain compiled into 21H461's VirtualAudio (its j120_speaker_*.austrip), and the
AU boxes of 24B119's /Library/Audio/Tunings/AID2003/VAD/speaker_*.dspg, in their order.

24B119 hands a strip to AudioDSPGraph (Graph::setAUStrip), which reads it by its 'version'. Version 1 (no 'version',
as all of Apple's own are written) requires an 'aupreset' on every effect that names a box of the graph: one without
fails the whole strip ('prs!', "failed to set parameter strip"), then the speaker's DSP chain and every route through
it. Version 2 takes 'aupreset' as optional, so J181's strips are version 2 and set only each box's bypass.
"""

import json
import plistlib
import unittest
from pathlib import Path

from python.runfiles import runfiles

AUFX, APPL = 'aufx', 'appl'
VOLUME = ('vrq2', 'voli')
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


def load(source: str) -> dict:
    with tree_file(source).open('rb') as handle:
        return plistlib.load(handle)


def effects(source: str) -> list[dict]:
    (chain,) = load(source)['strips']
    return chain['effects']


def dsp_graph_refusal(strip: dict) -> str | None:
    """Why 24B119's Graph::setAUStrip would refuse the strip, or None."""
    version = strip.get('version', 1)
    if version not in (1, 2):
        return f'version {version}'
    if not isinstance(strip.get('strips'), list):
        return 'no strips array'
    for chain in strip['strips']:
        for effect in chain.get('effects', []):
            if not isinstance(effect, dict) or not effect.get('displayname'):
                return 'an effect without a displayname'
            if version == 1 and not isinstance(effect.get('aupreset'), dict):
                return f'version 1 and {effect["displayname"]} has no aupreset'
            if not isinstance(effect.get('bypass', 0), int):
                return f"{effect['displayname']}'s bypass is not a number"
    return None


class SpeakerStrips(unittest.TestCase):
    def assert_only_volume_runs(self, source: str) -> None:
        dsp = effects(source)
        running = [fourcc(e['unit']['subtype']) for e in dsp if not e['bypass']]
        self.assertEqual(len(running), 1, source)
        self.assertIn(running[0], VOLUME, source)
        for effect in dsp:
            self.assertEqual((fourcc(effect['unit']['type']), fourcc(effect['unit']['manufacturer'])), (AUFX, APPL))

    def test_refusal_mirrors_audio_dsp_graph(self):
        effect = {'displayname': 'AUSPP3', 'bypass': 1}
        self.assertIn('no aupreset', dsp_graph_refusal({'strips': [{'effects': [effect]}]}))
        self.assertIsNone(dsp_graph_refusal({'strips': [{'effects': [effect | {'aupreset': {}}]}]}))
        self.assertIsNone(dsp_graph_refusal({'version': 2, 'strips': [{'effects': [effect]}]}))
        self.assertEqual(dsp_graph_refusal({'version': 3, 'strips': []}), 'version 3')

    def test_j181_strips_name_every_au_box_of_its_graphs(self):
        strips = manifest_strips('24B119')
        self.assertEqual(
            set(strips), {f'/Library/Audio/Tunings/AID2003/VAD/{name}.austrip' for name in AID2003}, 'AID2003/VAD'
        )
        for path, source in strips.items():
            name = Path(path).stem
            self.assertIsNone(dsp_graph_refusal(load(source)), name)
            boxes = [(e['displayname'], fourcc(e['unit']['subtype'])) for e in effects(source)]
            self.assertEqual(tuple(boxes), AID2003[name], name)
            self.assert_only_volume_runs(source)

    def test_j120_strips_pass_through(self):
        strips = manifest_strips('21H461')
        self.assertEqual(len(strips), 6)
        for path, source in strips.items():
            self.assertTrue(path.startswith('/Library/Audio/Tunings/J120/VAD/j120_speaker_'), path)
            self.assert_only_volume_runs(source)


if __name__ == '__main__':
    unittest.main()
