"""overlay.py's plan of the real overlay/manifest.json per IPSW kind.

An entry with a kind is staged and unioned only for that kind, and the other kind's sources are not asked of the build.
"""

import importlib.util
import json
import unittest
from types import ModuleType

from python.runfiles import runfiles

KINDS = ('21H461', '24B119')


def load_overlay() -> ModuleType:
    path = runfiles.Create().Rlocation('_main/tools/package/overlay.py')
    spec = importlib.util.spec_from_file_location('overlay', path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def manifest() -> dict:
    with open(runfiles.Create().Rlocation('_main/overlay/manifest.json')) as handle:
        return json.load(handle)


class KindTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.overlay = load_overlay()
        cls.manifest = manifest()

    def plan(self, kind: str, given: list[str] | None = None) -> dict:
        return self.overlay.plan(self.manifest, False, kind, given, release=True)

    def kinded(self) -> list[str]:
        return sorted({entry['kind'] for entry in self.manifest['files'] if 'kind' in entry})

    def test_manifest_kinds_are_ipsw_kinds(self):
        self.assertTrue(self.kinded())
        self.assertLessEqual(set(self.kinded()), set(KINDS))

    def test_each_kind_unions_only_its_own_bundles(self):
        for kind in KINDS:
            mine = {e['path'].rpartition('/')[0] for e in self.manifest['files'] if e.get('kind') == kind}
            others = {e['path'].rpartition('/')[0] for e in self.manifest['files'] if e.get('kind', kind) != kind}
            dirs = {d for d, _ in self.plan(kind)['dirs']}
            self.assertLessEqual(mine, dirs, kind)
            self.assertFalse(others & dirs, kind)

    def test_entries_without_a_kind_are_planned_for_both(self):
        unkinded = {
            e['path'] for e in self.manifest['files'] if not ('kind' in e or e.get('debug') or e.get('portrait'))
        }
        for kind in KINDS:
            files = self.plan(kind)['files']
            kinded = {e['path'] for e in self.manifest['files'] if e.get('kind') == kind}
            self.assertEqual({f['path'] for f in files} - kinded, unkinded, kind)

    def test_only_the_kinds_own_sources_are_required(self):
        for kind in self.kinded():
            given = [e['source'] for e in self.manifest['files'] if e.get('kind', kind) == kind]
            self.plan(kind, given)
            mine = next(e['source'] for e in self.manifest['files'] if e.get('kind') == kind)
            with self.assertRaises(SystemExit):
                self.plan(kind, [s for s in given if s != mine])


if __name__ == '__main__':
    unittest.main()
