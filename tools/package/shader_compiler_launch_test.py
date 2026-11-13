"""How each iOS version launches the shader compiler, and the jetsam band and limits it gets there.

24B119's launchd ignores a daemon plist's JetsamProperties (its importer's handler for the key is empty) and merges a
daemon's jetsam properties from its tables only: Override[label], then Override Global, then the plist's own top-level
keys, then Default. J181's Daemon Override Global holds band 40 and 6 MB limits, so only an Override entry for the
label, in the Supplemental cryptex's overlay that launchd loads, gives the compiler its own. EnablePressuredExit and
EnableTransactions are top-level plist keys that Global leaves alone: false, the compiler is never clean, aged or
idle-exited. 21H461's BlueTool stand-in spawns it with the same numbers (SwitchShaderCompilerJetsam.h).
"""

import json
import plistlib
import re
import unittest
from pathlib import Path

import overlay

LABEL = 'com.brunostjohn.switch.shadercompiler'
LAUNCHD_PLIST = Path('drivers/TegraGM20BMetal/compiler/com.brunostjohn.switch.shadercompiler.plist')
JETSAM_OVERLAY = Path('overlay/supplemental/com.apple.jetsamproperties.cryptex.plist')
HEADER = Path('drivers/TegraGM20BMetal/compiler/SwitchShaderCompilerJetsam.h')
BLUETOOL = Path('drivers/SwitchBluetooth/BlueTool/BlueTool.c')
MANIFEST = Path('overlay/manifest.json')
SUPPLEMENTAL_DAEMONS = overlay.SUPPLEMENTAL + '/System/Library/LaunchDaemons/'
# launchd's keys for a memory limit or band: at a plist's top level 24B119 ignores each, as it does JetsamProperties
JETSAM_KEYS = {
    'JetsamProperties',
    'JetsamPriority',
    'JetsamMemoryLimit',
    'ActiveSoftMemoryLimit',
    'ActiveHardMemoryLimit',
    'InactiveSoftMemoryLimit',
    'InactiveHardMemoryLimit',
}


def header_value(name: str) -> int:
    match = re.search(rf'^#define {name}\s+(\d+)$', HEADER.read_text(), re.MULTILINE)
    assert match, f'{HEADER} has no {name}'
    return int(match.group(1))


def manifest_entry(path: str) -> dict:
    entries = [e for e in json.loads(MANIFEST.read_text())['files'] if e['path'] == path]
    assert len(entries) == 1, f'{MANIFEST} names {path} {len(entries)} times'
    return entries[0]


class Ios27(unittest.TestCase):
    def setUp(self):
        self.plist = plistlib.loads(LAUNCHD_PLIST.read_bytes())
        self.jetsam = plistlib.loads(JETSAM_OVERLAY.read_bytes())

    def test_the_launchd_plist_holds_no_key_launchd_ignores(self):
        self.assertEqual(self.plist['Label'], LABEL)
        self.assertEqual(sorted(JETSAM_KEYS & set(self.plist)), [])

    def test_the_compiler_is_never_clean_aged_or_idle_exited(self):
        self.assertIs(self.plist['EnablePressuredExit'], False)
        self.assertIs(self.plist['EnableTransactions'], False)
        self.assertIs(self.plist['RunAtLoad'], True)
        self.assertEqual(self.plist['KeepAlive'], {'SuccessfulExit': False})

    def test_the_overlay_names_the_compiler_in_the_daemon_override_alone(self):
        self.assertEqual(list(self.jetsam), ['Version4'])
        self.assertEqual(list(self.jetsam['Version4']), ['Daemon'])
        self.assertEqual(list(self.jetsam['Version4']['Daemon']), ['Override'])
        self.assertNotIn('Global', self.jetsam['Version4']['Daemon']['Override'])

    def test_the_overlay_gives_the_headers_band_and_limits(self):
        entry = self.jetsam['Version4']['Daemon']['Override'][LABEL]
        self.assertEqual(
            entry,
            {
                'JetsamPriority': header_value('SWITCH_SHADER_COMPILER_JETSAM_PRIORITY'),
                'ActiveSoftMemoryLimit': header_value('SWITCH_SHADER_COMPILER_ACTIVE_LIMIT_MB'),
                'InactiveHardMemoryLimit': header_value('SWITCH_SHADER_COMPILER_INACTIVE_LIMIT_MB'),
                'EnergyEfficiencyMode': 'UserInterface',
            },
        )

    def test_every_overlay_entry_is_a_daemon_the_supplemental_cryptex_carries(self):
        labels = set()
        for entry in json.loads(MANIFEST.read_text())['files']:
            if entry['path'].startswith(SUPPLEMENTAL_DAEMONS):
                labels.add(plistlib.loads(Path(entry['source']).read_bytes())['Label'])
        self.assertLessEqual(set(self.jetsam['Version4']['Daemon']['Override']), labels)

    def test_the_overlay_is_staged_where_launchd_reads_it_on_24b119_only(self):
        entry = manifest_entry(overlay.SUPPLEMENTAL_JETSAM)
        self.assertEqual(entry['source'], str(JETSAM_OVERLAY))
        self.assertEqual(entry['union'], overlay.SUPPLEMENTAL)
        self.assertEqual(entry['kind'], '24B119')
        self.assertNotIn('boot-arg', entry)
        self.assertEqual(manifest_entry(SUPPLEMENTAL_DAEMONS + LAUNCHD_PLIST.name)['kind'], '24B119')
        manifest = json.loads(MANIFEST.read_text())
        for kind, staged in (('24B119', True), ('21H461', False)):
            paths = [f['path'] for f in overlay.plan(manifest, portrait=False, kind=kind)['files']]
            self.assertEqual(overlay.SUPPLEMENTAL_JETSAM in paths, staged, kind)


class Ios17(unittest.TestCase):
    def test_the_bluetool_stand_in_spawns_it_with_the_headers_band_and_limits(self):
        source = BLUETOOL.read_text()
        call = re.search(r'posix_spawnattr_setjetsam_ext\(\s*&attributes,([^;]*)\);', source)
        self.assertIsNotNone(call)
        arguments = [a.strip() for a in call.group(1).split(',')]
        self.assertEqual(
            arguments,
            [
                'kInactiveLimitFatal',
                'SWITCH_SHADER_COMPILER_JETSAM_PRIORITY',
                'SWITCH_SHADER_COMPILER_ACTIVE_LIMIT_MB',
                'SWITCH_SHADER_COMPILER_INACTIVE_LIMIT_MB',
            ],
        )
        self.assertIn('static const short kInactiveLimitFatal = 0x08;', source)
        self.assertLess(call.start(), source.index('posix_spawn(&pid, kShaderCompiler'))


if __name__ == '__main__':
    unittest.main()
