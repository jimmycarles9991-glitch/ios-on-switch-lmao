"""translated_tree's plan and staging of a card's translated System file updates, on plain folders."""

import json
import os
import tempfile
import unittest
from pathlib import Path

from tools.install import card as cards, pretranslate, translated_tree
from tools.trustcache import trustcache

PINS = Path(__file__).parents[2] / 'ipsw/pretranslated/24B119'
LOCATIOND = '/usr/libexec/locationd'
# locationd's translation before 15032ab57 kept capability bit 4 (edits_24B119.json): cards installed earlier carry it.
LOCATIOND_BEFORE = 'ac43b78b6ebf882d05f51c6edf50289ec42b6b64'
LOCATIOND_AFTER = '020e95fca17c66f80bc7d1d0e62860ca8b3cf097'
OTHER = 'aa' * 20


def manifest(locationd: str, other: str = OTHER) -> dict:
    return {
        'files': [
            {'path': LOCATIOND, 'trust': True, 'mode': '0755', 'cdhash': [locationd]},
            {'path': '/usr/libexec/other', 'trust': True, 'mode': '0755', 'cdhash': [other]},
            {'path': '/usr/libexec/notrust', 'trust': False, 'cdhash': ['bb' * 20]},
        ]
    }


class TranslatedTree(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.trust = self.root / 'os.trustcache'
        self.trust.write_bytes(trustcache.trust_module([bytes.fromhex(LOCATIOND_AFTER), bytes.fromhex(OTHER)]))
        self.carried = self.root / 'carried'
        (self.carried / 'usr/libexec').mkdir(parents=True)
        (self.carried / 'usr/libexec/locationd').write_bytes(b'translated locationd')
        self.offer({LOCATIOND: LOCATIOND_AFTER})

    def tearDown(self):
        self.tmp.cleanup()

    def offer(self, updates: dict) -> None:
        (self.carried / pretranslate.UPDATES_LIST).write_text(json.dumps(updates))

    def card(self, contents: dict, folder: str = 'Switch') -> Path:
        data = self.root / 'data'
        (data / folder / 'overlay').mkdir(parents=True, exist_ok=True)
        (data / folder / translated_tree.MANIFEST).write_text(json.dumps(contents))
        return data

    def updates(self, data: Path) -> Path:
        return data / 'Switch' / translated_tree.UPDATES

    def test_a_card_whose_tree_this_build_trusts_needs_no_update(self):
        self.assertEqual(translated_tree.plan(self.card(manifest(LOCATIOND_AFTER)), self.trust, self.carried), {})

    def test_a_stale_file_the_build_carries_is_updated(self):
        data = self.card(manifest(LOCATIOND_BEFORE))
        self.assertEqual(translated_tree.plan(data, self.trust, self.carried), {LOCATIOND: LOCATIOND_AFTER})

    def test_a_stale_file_the_build_does_not_carry_refuses_naming_it_and_the_reinstall_that_replaces_data(self):
        for offered in ({}, {LOCATIOND: 'cc' * 20}):
            self.offer(offered)
            with self.subTest(offered=offered), self.assertRaises(cards.RefusedError) as refused:
                translated_tree.plan(self.card(manifest(LOCATIOND_BEFORE)), self.trust, self.carried)
            for named in (LOCATIOND, '--reinstall-container', 'replaces Data'):
                self.assertIn(named, str(refused.exception))

    def test_the_folder_under_its_former_name_is_read(self):
        data = self.card(manifest(LOCATIOND_BEFORE), folder='IOSOld')
        self.assertEqual(translated_tree.plan(data, self.trust, self.carried), {LOCATIOND: LOCATIOND_AFTER})

    def test_a_card_without_the_manifest_refuses(self):
        (self.root / 'data/Switch/overlay').mkdir(parents=True)
        with self.assertRaises(cards.RefusedError) as refused:
            translated_tree.plan(self.root / 'data', self.trust, self.carried)
        self.assertIn('--reinstall-container', str(refused.exception))

    def test_staging_places_exactly_the_wanted_updates_and_removes_them_once_the_tree_is_current(self):
        data = self.card(manifest(LOCATIOND_BEFORE))
        stray = self.updates(data) / 'usr/sbin/old'
        stray.parent.mkdir(parents=True)
        stray.write_bytes(b'an earlier build')
        log = []
        translated_tree.stage(data, self.carried, {LOCATIOND: LOCATIOND_AFTER}, log.append)
        placed = self.updates(data) / 'usr/libexec/locationd'
        self.assertEqual(placed.read_bytes(), b'translated locationd')
        self.assertEqual(placed.stat().st_mode & 0o777, 0o755)
        self.assertFalse((self.updates(data) / 'usr/sbin').exists())
        inode = placed.stat().st_ino
        translated_tree.stage(data, self.carried, {LOCATIOND: LOCATIOND_AFTER}, log.append)
        self.assertEqual(placed.stat().st_ino, inode, 'an unchanged update is left alone')
        translated_tree.stage(data, self.carried, {}, log.append)
        self.assertFalse(self.updates(data).exists())

    def test_staging_replaces_a_symlink_where_the_folder_goes(self):
        data = self.card(manifest(LOCATIOND_BEFORE))
        os.symlink(self.root, self.updates(data))
        translated_tree.stage(data, self.carried, {LOCATIOND: LOCATIOND_AFTER}, lambda *_: None)
        self.assertFalse(self.updates(data).is_symlink())
        self.assertTrue((self.updates(data) / 'usr/libexec/locationd').is_file())

    def test_the_pins_refuse_the_locationd_cards_carry_from_before_the_edit_and_trust_the_update_they_name(self):
        pinned = (PINS / 'os.trustcache').read_bytes()
        self.assertEqual(translated_tree.untrusted(manifest(LOCATIOND_BEFORE, LOCATIOND_BEFORE), pinned)[0], LOCATIOND)
        trusted = {entry[: trustcache.CDHASH_BYTES].hex() for entry in trustcache.module_entries(pinned)}
        updates = json.loads((PINS / 'os.updates.json').read_text())
        self.assertTrue(updates)
        for path, cdhash in updates.items():
            self.assertIn(cdhash, trusted, path)

    def test_the_update_list_names_exactly_the_files_the_edits_change(self):
        edits = Path(__file__).parents[2] / 'translate/arm64e/pretranslate/edits_24B119.json'
        updates = json.loads((PINS / 'os.updates.json').read_text())
        self.assertEqual(sorted(updates), sorted(json.loads(edits.read_text())['files']))


if __name__ == '__main__':
    unittest.main()
