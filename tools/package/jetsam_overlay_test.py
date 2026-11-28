"""The Supplemental cryptex's jetsam overlay as this build's profile makes it (overlay/BUILD.bazel jetsam_properties).

It holds the shader compiler's, the bootstrap helper's and the app registration daemon's Overrides in both profiles (the
manifest stages their jobs in release too) and the texture peer's (HW Test's second process) when HW Test is enabled;
its merge refuses a label named twice.
"""

import json
import os
import plistlib
import unittest
from pathlib import Path

import jetsam_overlay

HWTEST = os.environ['SWITCH_HWTEST'] == '1'
OVERLAY = Path('overlay/supplemental/com.apple.jetsamproperties.cryptex.plist')
COMPILER = Path('overlay/supplemental/com.brunostjohn.switch.shadercompiler.jetsam.plist')
PEER = Path('apps/hwtest/peer/com.brunostjohn.switch.texpeer.jetsam.plist')
MANIFEST = Path('overlay/manifest.json')
COMPILER_LABEL = 'com.brunostjohn.switch.shadercompiler'
BOOTSTRAP_LABEL = 'com.brunostjohn.switch.bootstrap'
APPREG_LABELS = {'com.brunostjohn.switch.appregd', 'com.brunostjohn.switch.bootstrap-apps'}
PEER_LABEL = 'com.brunostjohn.switch.texpeer'


def override(path: Path) -> dict:
    return plistlib.loads(path.read_bytes())['Version4']['Daemon']['Override']


class ProfileTest(unittest.TestCase):
    def setUp(self):
        self.override = override(OVERLAY)

    def test_the_shader_compiler_has_its_override_in_both_profiles(self):
        self.assertEqual(self.override[COMPILER_LABEL], override(COMPILER)[COMPILER_LABEL])

    def test_the_app_registration_daemon_is_above_the_global_6_mb(self):
        for label in APPREG_LABELS:
            self.assertEqual(self.override[label], {'ActiveSoftMemoryLimit': 32, 'InactiveHardMemoryLimit': 32})

    def test_the_texture_peer_has_its_override_only_with_hwtest(self):
        labels = {COMPILER_LABEL, BOOTSTRAP_LABEL} | APPREG_LABELS | ({PEER_LABEL} if HWTEST else set())
        self.assertEqual(set(self.override), labels)
        if HWTEST:
            self.assertEqual(self.override[PEER_LABEL], {'ActiveSoftMemoryLimit': 40, 'InactiveHardMemoryLimit': 40})

    def test_the_peer_is_staged_only_with_hwtest(self):
        peer = [e for e in json.loads(MANIFEST.read_text())['files'] if 'texpeer' in e['path']]
        self.assertTrue(peer)
        self.assertTrue(all(e.get('hwtest') and not e.get('debug') for e in peer))


class MergeTest(unittest.TestCase):
    def fragment(self, label: str) -> dict:
        return {'Version4': {'Daemon': {'Override': {label: {'ActiveSoftMemoryLimit': 1}}}}}

    def test_a_label_named_twice_is_refused(self):
        with self.assertRaisesRegex(ValueError, 'two fragments name a'):
            jetsam_overlay.merged([self.fragment('a'), self.fragment('a')])

    def test_a_fragment_of_anything_else_is_refused(self):
        with self.assertRaisesRegex(ValueError, 'besides'):
            jetsam_overlay.merged([{'Version4': {'Daemon': {'Override': {}, 'Default': {}}}}])


if __name__ == '__main__':
    unittest.main()
