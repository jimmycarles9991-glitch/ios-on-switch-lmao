"""wallpaper_1x.py's plist edit, the pinned plists it made and the overlay entries that show them.

The pinned plists are in bazel/pinned/overlay/wallpaper-1x/<kind>/.
"""

import json
import os
import plistlib
import unittest
from pathlib import Path

import wallpaper_1x

FLAT_J120 = {
    'appearanceAware': True,
    'assets': {
        'lockAndHome': {
            'default': {
                'backgroundAnimationFileName': '7570.iPadOS_17_Flat_Background-1024w-1366h@2x~ipad.ca',
                'identifier': 7570,
                'name': 'iPadOS 17',
                'type': 'LayeredAnimation',
            }
        }
    },
    'contentVersion': 2.0,
    'family': 'iPadOS 17',
    'identifier': 7570,
    'logicalScreenClass': '1024w-1366h@2x~ipad',
    'name': 'iPadOS 17',
    'productTypes': ['iPad7,1', 'iPad7,2'],
    'version': 1,
}

ANIMATED_J181 = {
    'appearanceAware': True,
    'assets': {
        'lockAndHome': {
            'default': {
                'backgroundAnimationFileName': '7570.iPadOS_17_Background-810w-1080h@2x~ipad.ca',
                'floatingAnimationFileNameKey': '7570.iPadOS_17_Floating-810w-1080h@2x~ipad.ca',
                'identifier': 7570,
                'name': 'iPadOS 17',
                'type': 'LayeredAnimation',
            }
        }
    },
    'contentVersion': 2.01,
    'family': 'iPadOS 17',
    'identifier': 7570,
    'logicalScreenClass': '810w-1080h@2x~ipad',
    'name': 'iPadOS 17',
    'preferredProminentColor': {'dark': '#F89043', 'default': '#FFC76E'},
    'version': 1,
}

PINNED = {
    '21H461': ['7560.Lake_Flat.plist', '7570.iPadOS_17_Flat.plist'],
    '24B119': ['7560.Lake.plist', '7570.iPadOS_17.plist'],
}


def checkout_file(relative: str) -> Path | None:
    """Find a source file: in the runfiles under Bazel, in the checkout otherwise."""
    for base in (os.environ.get('TEST_SRCDIR', ''), str(Path(__file__).resolve().parents[2])):
        for root in (Path(base) / '_main', Path(base)):
            if (root / relative).exists():
                return root / relative
    return None


def pinned(kind: str) -> list[Path]:
    found = checkout_file(f'bazel/pinned/overlay/wallpaper-1x/{kind}')
    return sorted(found.glob('*.plist')) if found else []


def edit(fixture: dict, kind: str) -> dict:
    return plistlib.loads(
        wallpaper_1x.one_x(plistlib.dumps(fixture, fmt=plistlib.FMT_BINARY), 'fixture', wallpaper_1x.KINDS[kind])
    )


class OneXTest(unittest.TestCase):
    def test_only_the_screen_class_changes(self):
        for fixture, kind in ((FLAT_J120, '21H461'), (ANIMATED_J181, '24B119')):
            out = edit(fixture, kind)
            self.assertEqual(out.pop('logicalScreenClass'), 'any')
            expected = dict(fixture)
            expected.pop('logicalScreenClass')
            self.assertEqual(out, expected)

    def test_refuses_what_is_not_the_kinds_own_ipads_bundle(self):
        animated = dict(FLAT_J120, unsupportedProductTypes=['iPad7,1', 'iPad7,2'])
        animated.pop('productTypes')
        for bad, kind in (
            (animated, '21H461'),
            (dict(FLAT_J120, logicalScreenClass='834w-1112h@2x~ipad'), '21H461'),
            (ANIMATED_J181, '21H461'),
            (FLAT_J120, '24B119'),
            (dict(ANIMATED_J181, productTypes=['iPad11,6']), '24B119'),
        ):
            with self.assertRaises(SystemExit):
                edit(bad, kind)

    def test_pinned_names_drop_the_screen_class(self):
        for kind, entry in wallpaper_1x.KINDS.items():
            names = sorted(wallpaper_1x.pinned_name(entry, bundle) for _, bundle in entry.bundles)
            self.assertEqual(names, PINNED[kind])


class PinnedTest(unittest.TestCase):
    def test_pinned_plists_are_any_scale_bundles_of_the_kinds_ipad(self):
        for kind, names in PINNED.items():
            found = pinned(kind)
            self.assertEqual([p.name for p in found], names, kind)
            entry = wallpaper_1x.KINDS[kind]
            for path in found:
                plist = plistlib.loads(path.read_bytes())
                self.assertEqual(plist['logicalScreenClass'], 'any', path.name)
                self.assertTrue(wallpaper_1x.installs_on(plist, entry.product), path.name)
                self.assertTrue(
                    plist['assets']['lockAndHome']['default']['backgroundAnimationFileName'].endswith(
                        f'-{entry.screen_class}.ca'
                    ),
                    path.name,
                )

    def test_ios27s_load_under_either_ipad_identity(self):
        for path in pinned('24B119'):
            plist = plistlib.loads(path.read_bytes())
            self.assertNotIn('productTypes', plist, path.name)
            self.assertNotIn('unsupportedProductTypes', plist, path.name)


class OverlayEntriesTest(unittest.TestCase):
    def test_each_kind_shows_its_pinned_plists_over_its_own_bundles(self):
        manifest = json.loads(checkout_file('overlay/manifest.json').read_text())
        for kind, entry in wallpaper_1x.KINDS.items():
            shown = {
                f['path']: f['source']
                for f in manifest['files']
                if f['path'].startswith('/' + wallpaper_1x.COLLECTIONS + '/') and f.get('kind') == kind
            }
            expected = {
                f'/{wallpaper_1x.COLLECTIONS}/{collection}/Wallpapers/{bundle}/Wallpaper.plist': (
                    f'build/wallpaper-1x/{kind}/{wallpaper_1x.pinned_name(entry, bundle)}'
                )
                for collection, bundle in entry.bundles
            }
            self.assertEqual(shown, expected, kind)
        unkinded = [f['path'] for f in manifest['files'] if 'Wallpaper' in f['path'] and 'kind' not in f]
        self.assertEqual(unkinded, [])


if __name__ == '__main__':
    unittest.main()
