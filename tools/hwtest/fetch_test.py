"""tools/hwtest/fetch.py and hwtest.after_pull over a synthetic Data volume in a temporary directory."""

import json
import os
import plistlib
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from tools.hwtest import fetch, hwtest
from tools.hwtest.reports import Report, history, sidecar_of

BOOT = '6A25F6C4-A931-4851-9437-EDE961F57EC1'
OTHER_BOOT = '11111111-2222-3333-4444-555555555555'


def report(minute: int, status: str = 'done', current: str | None = None) -> dict:
    return {
        'schema': 2,
        'run': {
            'id': f'{BOOT}-{minute}',
            'boot_uuid': BOOT,
            'started': f'2026-11-08T22:{minute:02d}:00.000Z',
            'status': status,
            'current': current,
            'order': ['a'],
        },
        'identity': {
            'bundle_id': 'com.brunostjohn.switch.hwtest',
            'build': '24B119',
            'boot_time': '2026-11-08T21:58:46.000Z',
        },
        'summary': {'pass': 1, 'planned': 1},
        'results': [{'id': 'a', 'verdict': 'PASS', 'subsystem': 'Storage', 'metrics': {'mb_s': 80}, 'log': ''}],
    }


class Card:
    """A Data volume with HW Test's container, another app's, crash reports, a saved panic and a deploy record."""

    def __init__(self, root: Path) -> None:
        self.data = root / 'data'
        self.switch = self.data / 'Switch'
        self.reports = self.container('AAAA', 'com.brunostjohn.switch.hwtest') / 'Documents/hwtest'
        self.container('BBBB', 'com.apple.Preferences')
        (self.container('CCCC', 'com.apple.Other') / 'Documents/hwtest').mkdir(parents=True)
        self.reports.mkdir(parents=True)
        self.switch.mkdir(parents=True)
        crash = self.data / fetch.CRASH_REPORTS
        crash.mkdir(parents=True)
        self.ips(crash / 'backboardd-1.ips', '309', 'backboardd', BOOT)
        self.ips(crash / 'SwitchHWTest-1.ips', '309', 'SwitchHWTest', BOOT, exception={'type': 'EXC_BAD_ACCESS'})
        self.ips(crash / 'JetsamEvent-1.ips', '298', 'JetsamEvent', BOOT)
        self.ips(crash / 'dasd-1.ips', '309', 'dasd', OTHER_BOOT)
        frames = {'threads': [{'frames': [{'symbol': '+[CADisplay displays]'}]}]}
        self.ips(crash / 'chronod-1.ips', '309', 'chronod', BOOT, **frames)
        (self.switch / 'panic-2026-11-08-221030.log').write_text('panic')
        (self.switch / 'panic-2026-11-07-101010.log').write_text('older panic')
        self.app('A1', 'Real', executable=True)
        self.app('A2', 'Placeholder', executable=False)
        states = self.data / 'installd/Library/MobileInstallation/BackedUpState'
        states.mkdir(parents=True)
        (states / 'SystemAppInstallState.plist').write_bytes(plistlib.dumps({'com.apple.a': 1, 'com.apple.b': 0}))
        (self.data / 'root/Library/Lockdown/pair_records').mkdir(parents=True)
        (self.data / 'root/Library/Lockdown/pair_records/host.plist').write_bytes(b'')

    def app(self, uuid: str, name: str, executable: bool) -> None:
        bundle = self.data / 'containers/Bundle/Application' / uuid / f'{name}.app'
        bundle.mkdir(parents=True)
        (bundle / 'Info.plist').write_bytes(plistlib.dumps({'CFBundleExecutable': name}))
        if executable:
            (bundle / name).write_bytes(b'')

    def container(self, uuid: str, identifier: str) -> Path:
        path = self.data / fetch.CONTAINERS / uuid
        path.mkdir(parents=True)
        (path / fetch.METADATA).write_bytes(plistlib.dumps({'MCMMetadataIdentifier': identifier}))
        return path

    @staticmethod
    def ips(path: Path, bug_type: str, process: str, boot: str, **body: object) -> None:
        header = json.dumps({'bug_type': bug_type, 'name': process})
        path.write_text(header + '\n' + json.dumps({'procName': process, 'bootSessionUUID': boot, **body}))

    def deployed(self, commit: str, at: str) -> None:
        (self.switch / fetch.DEPLOYED).write_text(json.dumps({'commit': commit, 'dirty': False, 'deployed_at': at}))

    def run(self, name: str, data: dict) -> None:
        (self.reports / name).write_text(json.dumps(data))


class Fetch(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.card = Card(self.root)
        self.history = self.root / 'history'

    def tearDown(self):
        self.tmp.cleanup()

    def test_only_hwtest_containers_are_read(self):
        found = fetch.containers(self.card.data)
        self.assertEqual({k: p.name for k, p in found.items()}, {'com.brunostjohn.switch.hwtest': 'AAAA'})

    def test_the_gpu_scenes_files_come_along(self):
        scene = self.card.reports.parent / 'gpu/gpu.xr_blend'
        scene.mkdir(parents=True)
        (scene / 'raw-0-got.png').write_bytes(b'png')
        fetch.fetch(self.card.data, self.card.switch, self.history, self.root / 'out')
        copied = self.root / 'out/hwtest-gpu/com.brunostjohn.switch.hwtest/gpu.xr_blend/raw-0-got.png'
        self.assertEqual(copied.read_bytes(), b'png')

    def test_the_ui_tests_pictures_come_along(self):
        test = self.card.reports.parent / 'ui/display.pocket_strip'
        test.mkdir(parents=True)
        (test / 'shown.png').write_bytes(b'png')
        fetch.fetch(self.card.data, self.card.switch, self.history, self.root / 'out')
        copied = self.root / 'out/hwtest-ui/com.brunostjohn.switch.hwtest/display.pocket_strip/shown.png'
        self.assertEqual(copied.read_bytes(), b'png')

    def test_a_report_is_copied_once_with_its_boots_health(self):
        self.card.deployed('0123456789abcdef0123', '2026-11-08T21:50:00+00:00')
        self.card.run('r1.json', report(10, status='running', current='gpu.ca.sweep'))
        (self.card.reports / '.r1.json.new').write_text('torn')
        changed = fetch.fetch(self.card.data, self.card.switch, self.history, self.root / 'out')
        self.assertEqual([p.name for p in changed], ['r1.json'])
        self.assertEqual(fetch.fetch(self.card.data, self.card.switch, self.history), [])
        mac = json.loads(sidecar_of(self.history / 'r1.json').read_text())
        self.assertEqual(mac['deployed']['commit'], '0123456789abcdef0123')
        self.assertEqual(mac['health']['crashes'], {'backboardd': 1, 'SwitchHWTest': 1, 'chronod': 1})
        self.assertEqual(mac['health']['cadisplay_aborts'], {'chronod': 1})
        self.assertIsNone(mac['health']['boot_s'], 'no logarchive was pulled')
        self.assertEqual(mac['health']['jetsams'], 1)
        self.assertEqual(mac['health']['hwtest_crashes'], ['SwitchHWTest-1.ips: EXC_BAD_ACCESS '])
        self.assertEqual(mac['health']['panics'], ['panic-2026-11-08-221030.log'])
        self.assertEqual(mac['health']['pair_records'], 1)
        self.assertEqual(mac['health']['apps'], {'installed': 1, 'placeholders': 1, 'system_apps_wanted': 1})
        self.assertTrue((self.root / 'out/r1.json').is_file())
        self.assertEqual(Report.load(self.history / 'r1.json').commit, '0123456789ab')

    def test_a_deploy_after_the_boot_leaves_the_commit_unknown(self):
        self.card.deployed('fedcba', '2026-11-09T08:00:00+00:00')
        self.card.run('r1.json', report(10))
        fetch.fetch(self.card.data, self.card.switch, self.history)
        loaded = Report.load(self.history / 'r1.json')
        self.assertEqual(loaded.commit, 'commit unknown')
        self.assertEqual(loaded.mac['health']['panics'], [], 'a finished run claims no panic')

    def test_after_pull_summarises_the_new_run_against_the_last(self):
        self.card.run('r1.json', report(10))
        later = report(20)
        later['results'][0]['metrics']['mb_s'] = 40
        out = self.root / 'out'
        lines = []
        with mock.patch.dict(os.environ, {'SWITCH_HOME': str(self.root / 'home')}):
            hwtest.after_pull(self.card.data, self.card.switch, out, lambda *a: lines.append(' '.join(map(str, a))))
            self.card.run('r2.json', later)
            hwtest.after_pull(self.card.data, self.card.switch, out, lambda *a: lines.append(' '.join(map(str, a))))
            hwtest.after_pull(self.card.data, self.card.switch, out, lambda *a: lines.append(' '.join(map(str, a))))
            self.assertEqual(len(history(hwtest.history_dir())), 2)
        summary = (out / hwtest.SUMMARY).read_text()
        self.assertIn('a mb_s: 80 -> 40 (-50%)', summary)
        self.assertIn('slower (1)', summary)
        self.assertEqual(lines[-1], 'hwtest: no new HW Test run on the card')


if __name__ == '__main__':
    unittest.main()
