"""icon_cache.py on a Data tree laid out as the card's, against iconservicesagent's own reset rule."""

import gzip
import os
import plistlib
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from tools.deploy import deploy, icon_cache
from tools.install import renamed

SYSTEM_BUILD = '24B119'


def agent_resets(info: Path, system_build: str = SYSTEM_BUILD) -> bool:
    """Return whether -[ISMutableIconCache initWithURL:] (21H461 and 24B119) resets the cache for this __cache_info__.

    The cache stays only when the file reads as a dictionary whose ProductBuildVersion is the system's; language is
    written, never compared.
    """
    try:
        values = plistlib.loads(info.read_bytes())
    except (OSError, plistlib.InvalidFileException, ValueError):
        return True
    return not isinstance(values, dict) or values.get('ProductBuildVersion') != system_build


def agent_launch(info: Path) -> None:
    """Launch the agent: where it resets, a new __cache_info__ for this build, in the binary form it writes."""
    if agent_resets(info):
        info.write_bytes(
            plistlib.dumps({'language': 'en', 'ProductBuildVersion': SYSTEM_BUILD}, fmt=plistlib.FMT_BINARY)
        )


class Log:
    def __init__(self) -> None:
        self.lines = []

    def __call__(self, *parts: object) -> None:
        self.lines.append(' '.join(map(str, parts)))


class IconCacheTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.data = Path(self.tmp.name)
        groups = self.data / icon_cache.SYSTEM_GROUPS
        for uuid, group in (('A-OTHER', 'systemgroup.com.apple.other'), ('B-ICONS', icon_cache.GROUP)):
            (groups / uuid).mkdir(parents=True)
            with open(groups / uuid / icon_cache.METADATA, 'wb') as handle:
                plistlib.dump({'MCMMetadataIdentifier': group}, handle, fmt=plistlib.FMT_BINARY)
        self.cache = groups / 'B-ICONS' / icon_cache.CACHE
        self.cache.mkdir(parents=True)
        self.cache.chmod(0o755)
        (self.cache / 'store.index').write_bytes(b'index')
        self.info = self.cache / icon_cache.INFO
        agent_launch(self.info)

    def tearDown(self):
        self.tmp.cleanup()

    def stage(self, digest: str) -> None:
        (self.data / 'ngpu').mkdir(exist_ok=True)
        with gzip.open(self.data / icon_cache.STAGE_LIST, 'wt') as handle:
            handle.write(f'# {digest}\nngpu/addons/TegraGM20BMetal.bundle/TegraGM20BMetal\t{digest}\t1\t755\n')

    def record(self) -> str:
        return (renamed.data_folder(self.data) / icon_cache.RECORD).read_text().strip()

    def test_the_cache_is_found_by_its_group_not_its_uuid(self):
        self.assertEqual(icon_cache.cache_directory(self.data), self.cache)
        self.assertIsNone(icon_cache.cache_directory(self.data / 'nothing'))

    def test_a_new_gpu_build_makes_the_agent_reset_and_keeps_language(self):
        self.stage('a' * 64)
        self.assertFalse(agent_resets(self.info))
        self.assertEqual(icon_cache.present(self.data, Log()), 'reset at the next launch')
        self.assertTrue(agent_resets(self.info))
        values = plistlib.loads(self.info.read_bytes())
        self.assertEqual(values, {'language': 'en'})
        self.assertTrue(self.info.read_bytes().startswith(b'bplist'))
        self.assertEqual(self.record(), 'a' * 64)

    def test_the_same_gpu_build_changes_nothing_after_the_agents_reset(self):
        self.stage('a' * 64)
        icon_cache.present(self.data, Log())
        agent_launch(self.info)
        before = self.info.read_bytes()
        self.assertEqual(icon_cache.present(self.data, Log()), 'unchanged')
        self.assertEqual(self.info.read_bytes(), before)
        self.assertFalse(agent_resets(self.info))
        self.stage('b' * 64)
        self.assertEqual(icon_cache.present(self.data, Log()), 'reset at the next launch')
        self.assertTrue(agent_resets(self.info))

    def test_the_cache_directory_keeps_its_mode_entries_and_mtime(self):
        stat, inode = self.cache.stat(), self.info.stat().st_ino
        listing = sorted(os.listdir(self.cache))
        self.stage('a' * 64)
        icon_cache.present(self.data, Log())
        self.assertEqual(sorted(os.listdir(self.cache)), listing)
        self.assertEqual((self.cache.stat().st_mode, self.cache.stat().st_mtime_ns), (stat.st_mode, stat.st_mtime_ns))
        self.assertEqual(self.info.stat().st_ino, inode)

    def test_a_removed_gpu_build_is_a_change(self):
        self.stage('a' * 64)
        icon_cache.present(self.data, Log())
        agent_launch(self.info)
        (self.data / icon_cache.STAGE_LIST).unlink()
        self.assertEqual(icon_cache.present(self.data, Log()), 'reset at the next launch')
        self.assertEqual(self.record(), '')

    def test_no_cache_yet_records_the_build_only(self):
        self.info.unlink()
        self.stage('a' * 64)
        self.assertEqual(icon_cache.present(self.data, Log()), 'no cache yet')
        self.assertFalse(self.info.exists())
        self.assertEqual(self.record(), 'a' * 64)

    def test_a_deploy_cut_short_before_the_record_marks_again(self):
        self.stage('a' * 64)
        with mock.patch.object(Path, 'write_text', side_effect=OSError('card pulled')), self.assertRaises(OSError):
            icon_cache.present(self.data, Log())
        self.assertTrue(agent_resets(self.info))
        agent_launch(self.info)
        self.assertEqual(icon_cache.present(self.data, Log()), 'reset at the next launch')
        self.assertTrue(agent_resets(self.info))

    def test_an_xml_cache_info_stays_xml(self):
        self.info.write_bytes(plistlib.dumps({'language': 'en', 'ProductBuildVersion': SYSTEM_BUILD}))
        self.stage('a' * 64)
        icon_cache.present(self.data, Log())
        self.assertTrue(self.info.read_bytes().startswith(b'<?xml'))
        self.assertTrue(agent_resets(self.info))

    def test_the_deploy_lets_the_cache_go_after_staging_the_gpu_build(self):
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
            deploy._write(args, self.data, deploy.Volumes('disk9s1', None), None, Log())
        self.assertEqual(order[order.index('_stage_gpu') :][:2], ['_stage_gpu', 'icon_cache'])


if __name__ == '__main__':
    unittest.main()
