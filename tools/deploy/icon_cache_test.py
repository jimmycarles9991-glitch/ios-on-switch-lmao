"""icon_cache.py on a Data tree laid out as the card's: it names the staged GPU build, never reading the icons cache."""

import contextlib
import errno
import gzip
import os
import plistlib
import tempfile
import unittest
from collections.abc import Iterator
from pathlib import Path
from unittest import mock

from tools.deploy import deploy, icon_cache
from tools.install import renamed

GROUP = 'containers/Shared/SystemGroup/systemgroup.com.apple.lsd.iconscache'


class Log:
    def __init__(self) -> None:
        self.lines = []

    def __call__(self, *parts: object) -> None:
        self.lines.append(' '.join(map(str, parts)))


@contextlib.contextmanager
def unsearchable(directory: Path) -> Iterator[None]:
    """Refuse search below `directory` with EACCES, as the card's icons cache refused the deploying user on 14 Nov.

    Mode bits do that for any user but root, which some build hosts run tests as; stat() refuses for root too.
    """
    real_stat = os.stat

    def stat(path: object, *args: object, **kwargs: object) -> os.stat_result:
        if Path(os.fsdecode(path)).is_relative_to(directory) and Path(os.fsdecode(path)) != directory:
            raise PermissionError(errno.EACCES, os.strerror(errno.EACCES), os.fsdecode(path))
        return real_stat(path, *args, **kwargs)

    directory.chmod(0o000)
    try:
        with mock.patch('os.stat', stat):
            yield
    finally:
        directory.chmod(0o755)


class IconCacheTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.data = Path(self.tmp.name)
        container = self.data / GROUP
        container.mkdir(parents=True)
        with open(container / '.com.apple.mobile_container_manager.metadata.plist', 'wb') as handle:
            plistlib.dump({'MCMMetadataIdentifier': 'systemgroup.com.apple.lsd.iconscache'}, handle)
        self.library = container / 'Library'
        self.cache = self.library / 'Caches/com.apple.IconsCache'
        self.cache.mkdir(parents=True)
        self.info = self.cache / '__cache_info__'
        self.info.write_bytes(
            plistlib.dumps({'language': 'en', 'ProductBuildVersion': '24B119'}, fmt=plistlib.FMT_BINARY)
        )

    def tearDown(self):
        self.tmp.cleanup()

    def stage(self, digest: str) -> None:
        (self.data / 'ngpu').mkdir(exist_ok=True)
        with gzip.open(self.data / icon_cache.STAGE_LIST, 'wt') as handle:
            handle.write(f'# {digest}\nngpu/addons/TegraGM20BMetal.bundle/TegraGM20BMetal\t{digest}\t1\t755\n')

    def record(self) -> str:
        return (renamed.data_folder(self.data) / icon_cache.RECORD).read_text()

    def test_a_new_gpu_build_is_named_for_the_kernel(self):
        self.stage('a' * 64)
        self.assertEqual(icon_cache.present(self.data, Log()), 'named')
        self.assertEqual(self.record(), 'a' * 64 + '\n')
        self.stage('b' * 64)
        self.assertEqual(icon_cache.present(self.data, Log()), 'named')
        self.assertEqual(self.record(), 'b' * 64 + '\n')

    def test_the_same_gpu_build_leaves_the_record_as_it_is(self):
        self.stage('a' * 64)
        icon_cache.present(self.data, Log())
        record = renamed.data_folder(self.data) / icon_cache.RECORD
        before = record.stat()
        self.assertEqual(icon_cache.present(self.data, Log()), 'unchanged')
        self.assertEqual((record.stat().st_ino, record.stat().st_mtime_ns), (before.st_ino, before.st_mtime_ns))

    def test_a_removed_gpu_build_is_named_as_none(self):
        self.stage('a' * 64)
        icon_cache.present(self.data, Log())
        (self.data / icon_cache.STAGE_LIST).unlink()
        self.assertEqual(icon_cache.present(self.data, Log()), 'named')
        self.assertEqual(self.record(), '\n')

    def test_an_unsearchable_icons_cache_is_never_read_or_changed(self):
        before = self.info.read_bytes()
        self.stage('a' * 64)
        with unsearchable(self.library):
            with self.assertRaises(PermissionError):
                self.info.stat()
            self.assertEqual(icon_cache.present(self.data, Log()), 'named')
        self.assertEqual(self.record(), 'a' * 64 + '\n')
        self.assertEqual(self.info.read_bytes(), before)

    def test_the_deploy_names_the_build_after_staging_the_gpu_build(self):
        order = []

        def step(name: str) -> mock.MagicMock:
            return mock.MagicMock(side_effect=lambda *_, **__: order.append(name))

        args = deploy._parser().parse_args(['disk9', '--boot-package', 'p', '--spec', 's', '--no-joycon-pairing'])
        with (
            mock.patch.object(deploy, '_stage_gpu', step('_stage_gpu')),
            mock.patch.object(deploy, '_restage_system_apps', step('_restage_system_apps')),
            mock.patch.object(deploy, 'remove_stale_logging_profiles', step('remove_stale_logging_profiles')),
            mock.patch.object(deploy, '_boot_args', step('_boot_args')),
            mock.patch.object(deploy.icon_cache, 'present', step('icon_cache')),
            mock.patch.object(deploy.renamed, 'migrate_data'),
            mock.patch.object(deploy.cards, 'mounted', mock.MagicMock()),
            mock.patch.object(deploy.cards, 'sync_boot_entry'),
            mock.patch.object(deploy.cards, 'sync_overlay'),
            mock.patch.object(deploy.deployed, 'record'),
            mock.patch.object(deploy.system_apps, 'apps'),
            mock.patch.object(deploy.system_apps, 'register'),
        ):
            self.assertEqual(deploy._write(args, self.data, deploy.Volumes('disk9s1', None), None, Log()), [])
        self.assertEqual(order[order.index('_stage_gpu') :][:2], ['_stage_gpu', 'icon_cache'])


if __name__ == '__main__':
    unittest.main()
