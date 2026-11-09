"""fill.fill as the user on a small composed container: System, Data and Preboot volumes in a GPT image.

System's /private/var holds root's, mobile's and a daemon's files, given offline as Apple's are. Populate must copy them
to Data with those owners, put the os role's translated files on Data (under /private/var in place, except the staged
system apps, which keep Apple's bytes there; the rest, those included, in Switch/pretranslated, root's and
system-immutable, with the staged apps' .originals), stage a cryptex root's in Preboot, and give the boot package's
overlay, the GPU stager's files and the shader seed root's, except what the stager writes into mobile's home. hdiutil
and diskutil: macOS, outside the sandbox (--strategy=TestRunner=local).
"""

import hashlib
import json
import os
import shutil
import stat
import sys
import tempfile
import unittest
import uuid
from pathlib import Path

from tools.install import disks, fill, host, owners, pretranslate

MIB = 1 << 20
DAEMON = 272

STAGER = """import sys
from pathlib import Path
data = Path(sys.argv[1])
(data / 'Switch/gpu').mkdir(parents=True, exist_ok=True)
(data / 'Switch/gpu/bundle.bin').write_bytes(b'gpu')
(data / 'mobile/Library/Preferences/com.apple.backboardd.plist').write_bytes(b'prefs')
"""


class Log:
    def __call__(self, *parts: object) -> None:
        print(*parts)

    def quiet(self, *parts: object) -> None:
        pass


LOG = Log()


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


class Fill(unittest.TestCase):
    def setUp(self) -> None:
        self.dir = Path(tempfile.mkdtemp(prefix='fill-'))
        self.image = self.dir / 'composed.img'

    def tearDown(self) -> None:
        disks.sweep(self.dir, LOG)
        shutil.rmtree(self.dir)

    def compose(self) -> tuple[dict, int]:
        """Make the container; return the spec of its three volumes and the container's offset in the image."""
        with open(self.image, 'wb') as f:
            f.truncate(2048 * MIB)  # APFS allows a volume per 512 MiB of container
        with disks.attached(self.image, LOG, raw=True) as image:
            host.run('diskutil', 'partitionDisk', image.whole, 'GPT', 'APFS', 'System', '100%', log=LOG)
            store = host.disk_info(image.whole + 's1') or {}
            container = host.container_on(image.whole + 's1')['ContainerReference']
            for name, role in (('Data', 'D'), ('Preboot', 'B')):
                host.run(
                    'diskutil', 'apfs', 'addVolume', container, 'Case-sensitive APFS', name, '-role', role, '-nomount'
                )
            volumes = host.container_on(image.whole + 's1')['Volumes']
            uuids = {v['Name'].lower(): v['APFSVolumeUUID'] for v in volumes}
            host.run('diskutil', 'unmountDisk', container, check=False)
        spec = {
            'volumes': {key: {'uuid': uuids[key]} for key in ('system', 'data', 'preboot')},
            'cryptexes': [{'member': 'app_cryptex', 'path': '/private/preboot/Switch-Staging/app.dmg'}],
        }
        return spec, int(store['PartitionMapPartitionOffset'])

    def fill_system(self, spec: dict, offset: int) -> None:
        journal = owners.Journal()
        with disks.attached(self.image, LOG, raw=True) as image:
            system = fill._volumes(image, spec)['system']
            with disks.mounted(system, LOG, owners=False) as root:
                var = root / 'private/var'
                for d in ('mobile/Library', 'root/Library/Preferences', 'db/daemon', 'preboot'):
                    (var / d).mkdir(parents=True, exist_ok=True)
                (var / 'db/daemon/state').write_text('daemon')
                (var / 'db/daemon/state').chmod(0o600)
                (var / 'db/tool').write_bytes(b'arm64e var')
                (var / 'staged_system_apps/App.app').mkdir(parents=True)
                (var / 'staged_system_apps/App.app/App').write_bytes(b'arm64e app')
                (root / 'usr/libexec').mkdir(parents=True)
                (root / 'usr/libexec/tool').write_bytes(b'arm64e')
                (root / 'usr/libexec/tool').chmod(0o755)
                for path, uid, gid in (
                    ('', 0, 0),
                    ('private/var/mobile', 501, 501),
                    ('private/var/db/daemon', 272, 272),
                ):
                    journal.set(path, uid, gid)
                journal.set('private/var/db/daemon/state', DAEMON, DAEMON, 0o600)
                journal.set('private/var/mobile/Library', 501, 501)
                disks.settle(root)
                by_inode = journal.resolve(root)
        fd = os.open(self.image, os.O_RDWR)
        try:
            owners.apply(fd, offset, uuid.UUID(spec['volumes']['system']['uuid']), by_inode)
        finally:
            os.close(fd)

    def inputs(self, spec: dict) -> fill.Inputs:
        delta = self.dir / 'delta_os'
        (delta / 'files').mkdir(parents=True)
        files = []
        for i, (path, before, after) in enumerate((
            ('/private/var/db/tool', b'arm64e var', b'arm64 var'),
            ('/private/var/staged_system_apps/App.app/App', b'arm64e app', b'arm64 app'),
            ('/usr/libexec/tool', b'arm64e', b'arm64'),
        )):  # fmt: skip
            (delta / f'files/{i}').write_bytes(after)
            files.append({'path': path, 'file': f'files/{i}', 'sha256_in': sha(before), 'sha256_out': sha(after)})
        files[1]['cdhash'] = ['22' * 20]
        (delta / 'os.cdhash-pairs').write_text(f'{"11" * 20} {"22" * 20}\n')
        (delta / 'os.manifest.json').write_text(json.dumps({'files': files}))
        (delta / 'os.trustcache').write_bytes(b'trust')
        deltas = pretranslate.Deltas({'os': delta}, self.dir / 'kept', LOG)
        cryptex = self.dir / 'app.dmg'
        cryptex.write_bytes(b'sealed cryptex')
        package = self.dir / 'package'
        (package / 'overlay/usr/lib').mkdir(parents=True)
        (package / 'overlay/usr/lib/libswitch.dylib').write_bytes(b'switch')
        stager = self.dir / 'stager.py'
        stager.write_text(STAGER)
        seed = self.dir / 'seed'
        (seed / 'Switch/shaders').mkdir(parents=True)
        (seed / 'Switch/shaders/a.bin').write_bytes(b'shader')
        return fill.Inputs(
            spec=spec,
            deltas=deltas,
            images={'app_cryptex': (cryptex, sha(b'sealed cryptex'))},
            writes=0,
            boot_package=str(package),
            gpu_stager=str(stager),
            gpu_base=str(self.dir),
            shader_seed=str(seed),
        )

    def test_fill_as_the_user(self) -> None:
        spec, offset = self.compose()
        self.fill_system(spec, offset)
        inputs = self.inputs(spec)
        fill.fill(self.image, offset, inputs, LOG)
        self.assertTrue((self.dir / 'delta_os/files/0').exists(), "Bazel's delta is only read")

        def owner(root: Path, path: str) -> tuple[int, int, int]:
            info = os.lstat(root / path)
            return info.st_uid, info.st_gid, stat.S_IMODE(info.st_mode)

        with disks.attached(self.image, LOG, writable=False, raw=True) as image:
            volumes = fill._volumes(image, spec)
            with disks.mounted(volumes['data'], LOG, owners=True, read_only=True) as data:
                self.assertEqual(owner(data, '.'), (0, 0, 0o755))
                self.assertEqual(owner(data, 'db/daemon/state'), (DAEMON, DAEMON, 0o600))
                self.assertEqual(owner(data, 'mobile/Library')[:2], (501, 501))
                self.assertEqual((data / 'db/tool').read_bytes(), b'arm64 var')
                self.assertEqual((data / 'staged_system_apps/App.app/App').read_bytes(), b'arm64e app')
                staged = Path(pretranslate.OVERLAY) / 'private/var/staged_system_apps/App.app/App'
                self.assertEqual((data / staged).read_bytes(), b'arm64 app')
                self.assertEqual(owner(data, str(staged))[:2], (0, 0))
                self.assertEqual(owner(data, pretranslate.OVERLAY_ORIGINALS), (0, 0, 0o644))
                placed = Path(pretranslate.OVERLAY) / 'usr/libexec/tool'
                self.assertEqual(owner(data, str(placed)), (0, 0, 0o755))
                self.assertTrue(os.lstat(data / placed).st_flags & stat.SF_IMMUTABLE)
                self.assertTrue(os.lstat(data / pretranslate.OVERLAY_INDEX).st_flags & stat.SF_IMMUTABLE)
                self.assertEqual(owner(data, 'Switch/overlay/usr/lib/libswitch.dylib')[:2], (0, 0))
                self.assertEqual(owner(data, 'Switch/gpu/bundle.bin')[:2], (0, 0))
                self.assertEqual(owner(data, 'mobile/Library/Preferences/com.apple.backboardd.plist')[:2], (501, 501))
                self.assertEqual(owner(data, 'Switch/shaders/a.bin'), (0, 0, 0o644))
            with disks.mounted(volumes['preboot'], LOG, owners=True, read_only=True) as preboot:
                self.assertEqual(owner(preboot, 'Switch-Staging/app.dmg'), (0, 0, 0o644))
                self.assertEqual((preboot / 'Switch-Staging/app.dmg').read_bytes(), b'sealed cryptex')


if __name__ == '__main__':
    sys.exit(unittest.main())
