"""tools/deploy/deployed.py on a throwaway git checkout and a Data volume folder."""

import contextlib
import io
import json
import os
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from tools.deploy import deployed
from tools.package import snapshot


class Record(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        (self.root / 'data').mkdir()

    def tearDown(self):
        self.tmp.cleanup()

    def git(self, checkout: Path, *args: str) -> str:
        command = ['git', '-C', str(checkout), '-c', 'user.name=t', '-c', 'user.email=t@t', *args]
        return subprocess.run(command, capture_output=True, text=True, check=True).stdout.strip()

    def test_records_the_checkouts_commit_and_whether_it_is_dirty(self):
        checkout = self.root / 'checkout'
        checkout.mkdir()
        self.git(checkout, 'init', '-q', '-b', 'deploy-next5')
        (checkout / 'a.txt').write_text('a')
        self.git(checkout, 'add', 'a.txt')
        self.git(checkout, 'commit', '-q', '-m', 'a')
        head = self.git(checkout, 'rev-parse', 'HEAD')
        lines = []
        info = deployed.record(self.root / 'data', checkout, '24B119', lines.append)
        self.assertEqual(
            (info['commit'], info['branch'], info['dirty'], info['build']), (head, 'deploy-next5', False, '24B119')
        )
        written = json.loads((self.root / 'data/Switch' / deployed.NAME).read_text())
        self.assertEqual(written, info)
        (checkout / 'a.txt').write_text('b')
        self.assertTrue(deployed.record(self.root / 'data', checkout, '', lines.append)['dirty'])

    def test_outside_a_checkout_the_commit_is_empty(self):
        info = deployed.record(self.root / 'data', self.root, '21H461', lambda *_: None)
        self.assertEqual((info['commit'], info['dirty']), ('', False))

    def test_snapshot_deploy_keeps_its_source_after_the_checkout_advances(self):
        checkout = self.root / 'checkout'
        checkout.mkdir()
        self.git(checkout, 'init', '-q', '-b', 'next')
        (checkout / 'a').write_text('candidate')
        self.git(checkout, 'add', 'a')
        self.git(checkout, 'commit', '-q', '-m', 'candidate')
        head = self.git(checkout, 'rev-parse', 'HEAD')
        boot = self.root / 'boot'
        (boot / 'switchroot/ios').mkdir(parents=True)
        (boot / 'switchroot/ios/bl33.bin').write_bytes(b'boot')
        base = self.root / 'base'
        base.mkdir()
        (base / 'TegraGM20BMetal.trustcache').write_bytes(b'trust')
        runfiles = self.root / 'runfiles'
        manifest = runfiles / '_main/gpu/bundle.stage'
        seed = runfiles / '_main/seed/MANIFEST.json'
        manifest.parent.mkdir(parents=True)
        seed.parent.mkdir(parents=True)
        seed.write_text(json.dumps({'ipsw': '24B119'}))
        manifest.write_text(
            '# gpu stage 1 _main/gpu/bundle.stage\n'
            'ngpu/addons/TegraGM20BMetal.m2v-seed/shared/MANIFEST.json\tunused\t20\t644\t_main/seed/MANIFEST.json\n'
        )
        kernel = self.root / 'kernel.macho'
        kernel.write_bytes(b'kernel')
        state = self.root / 'state'
        argv = [
            'snapshot',
            '--boot',
            str(boot),
            '--gpu',
            str(manifest),
            '--gpu-base',
            str(base),
            '--kernel',
            str(kernel),
            'candidate',
        ]
        output = io.StringIO()
        with (
            mock.patch.dict(os.environ, {'SWITCH_HOME': str(state), 'BUILD_WORKSPACE_DIRECTORY': str(checkout)}),
            mock.patch('sys.argv', argv),
            mock.patch.object(
                Path, 'read_bytes', side_effect=AssertionError('snapshot must not read back copied bytes')
            ),
            contextlib.redirect_stdout(output),
        ):
            snapshot.main()
        package = state / 'lkg/candidate'
        source = deployed.provenance(package)
        self.assertEqual(source, {'commit': head, 'branch': 'next', 'dirty': False, 'build': '24B119'})
        self.assertEqual(output.getvalue().strip(), str(package))
        self.assertEqual((package / 'switchroot/ios/bl33.bin').read_bytes(), b'boot')
        self.assertEqual((package / 'kernel.macho').read_bytes(), b'kernel')
        (checkout / 'a').write_text('later')
        self.git(checkout, 'commit', '-qam', 'later')
        self.assertNotEqual(self.git(checkout, 'rev-parse', 'HEAD'), head)
        info = deployed.record(self.root / 'data', checkout, '21H461', lambda *_: None, package)
        self.assertEqual({key: info[key] for key in source}, source)
        self.assertIn('deployed_at', info)
        self.assertNotIn('deployed_at', source)
        self.assertEqual(json.loads((self.root / 'data/Switch' / deployed.NAME).read_text()), info)
        with (
            mock.patch.dict(os.environ, {'SWITCH_HOME': str(state), 'BUILD_WORKSPACE_DIRECTORY': str(checkout)}),
            mock.patch('sys.argv', ['snapshot', '--boot', str(package), '--no-gpu', 'copy']),
            contextlib.redirect_stdout(io.StringIO()),
        ):
            snapshot.main()
        self.assertEqual(deployed.provenance(state / 'lkg/copy'), source)


if __name__ == '__main__':
    unittest.main()
