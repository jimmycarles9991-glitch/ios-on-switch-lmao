"""sites.py: a declared site is written only when every original word matches, and only for its own kind."""

import json
import tempfile
import unittest
from pathlib import Path

from kits.SwitchABI import sites

TABLES = Path(__file__).resolve().parents[1] / 'kinds'


def table(kind: str, rows: list[tuple[str, str, str]]) -> Path:
    path = Path(tempfile.mkdtemp()) / 'com.example.kext.json'
    path.write_text(
        json.dumps(
            {
                'kind': kind,
                'bundle': 'com.example.kext',
                'sites': [{'at': a, 'from': f, 'to': t, 'why': 'test'} for a, f, t in rows],
            }
        )
    )
    return path


class SitesTest(unittest.TestCase):
    def setUp(self):
        self.words = {0x1000: 0x71000A9F, 0x1004: 0x52A00808, 0x1008: 0xD503201F}

    def test_patched_at_exactly_the_declared_sites(self):
        before = dict(self.words)
        rows = [('0x1000', '71000a9f', '7100029f'), ('0x1004', '52a00808', '52800008')]
        found = sites.load(table('24B119', rows), '24B119')
        sites.apply(found, self.words.__getitem__, self.words.__setitem__)
        changed = {a for a in before if before[a] != self.words[a]}
        self.assertEqual(changed, {0x1000, 0x1004})
        self.assertEqual(self.words[0x1000], 0x7100029F)

    def test_a_word_that_differs_fails_before_anything_is_written(self):
        before = dict(self.words)
        rows = [('0x1000', '71000a9f', '7100029f'), ('0x1004', '52a00809', '52800009')]
        found = sites.load(table('24B119', rows), '24B119')
        with self.assertRaises(SystemExit) as raised:
            sites.apply(found, self.words.__getitem__, self.words.__setitem__)
        self.assertIn('0x1004', str(raised.exception))
        self.assertEqual(self.words, before)

    def test_another_kinds_table_is_refused(self):
        with self.assertRaises(SystemExit):
            sites.load(table('21H461', [('0x1000', '71000a9f', '7100029f')]), '24B119')

    def test_a_site_declared_twice_is_refused(self):
        rows = [('0x1000', '71000a9f', '7100029f')]
        with self.assertRaises(SystemExit):
            sites.load_all([table('24B119', rows), table('24B119', rows)], '24B119')

    def test_the_kinds_tables_load(self):
        for kind in ('24B119', '21H461'):
            for path in sorted((TABLES / kind / 'sites').glob('*.json')):
                found = sites.load(path, kind)
                self.assertTrue(found, path)
                self.assertTrue(all(s.original != s.replacement for s in found), path)


class FamilyCommandTest(unittest.TestCase):
    def run_requests(self, word: int, overlapping: bool) -> tuple[int, int, int]:
        remembered = None
        active = {}
        maps = unmaps = 0

        def prepare() -> tuple[int, bool]:
            nonlocal remembered, maps
            command = remembered
            if command is None:
                command = len(active)
                active[command] = 1
                maps += 1
                if word == 0xEB09011F:  # cmp x8, x9: the mapper equals the buffer's owner mapper
                    remembered = command
                else:
                    self.assertEqual(word, 0xF100011F)  # cmp x8, #0: our mapper is non-null
            active[command] += 1
            return command, remembered != command

        def complete(request: tuple[int, bool]) -> None:
            nonlocal remembered, unmaps
            command, pool_owned = request
            self.assertGreater(active[command], 0)
            active[command] -= 1
            if pool_owned:
                unmaps += int(active[command] != 0)
                active[command] = 0
                if remembered == command:
                    remembered = None

        for _ in range(64):
            first = prepare()
            second = prepare() if overlapping else None
            complete(first)
            if second is not None:
                self.assertGreater(active[second[0]], 0, 'one completion must not unmap another request')
                complete(second)
        return maps, unmaps, sum(active.values())

    def test_cached_family_buffer_returns_its_mapping_after_every_transfer(self) -> None:
        patch = sites.load(TABLES / '24B119/sites/com.apple.iokit.IOUSBHostFamily.json', '24B119')[0]
        self.assertEqual(self.run_requests(patch.original, False), (1, 0, 1))
        self.assertEqual(self.run_requests(patch.replacement, False), (64, 64, 0))

    def test_overlapping_requests_keep_independent_commands(self) -> None:
        patch = sites.load(TABLES / '24B119/sites/com.apple.iokit.IOUSBHostFamily.json', '24B119')[0]
        self.assertEqual(self.run_requests(patch.replacement, True), (128, 128, 0))


if __name__ == '__main__':
    unittest.main()
