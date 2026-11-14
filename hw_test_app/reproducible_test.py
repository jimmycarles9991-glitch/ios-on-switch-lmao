"""The HW Test apps hold nothing of the Mac that built them, and the iOS 27 app built twice is the same bytes.

The twin is the app in a configuration of its own (apps.bzl reproducible_apps), so each of its actions ran again, in
other sandboxes and output directories: what differs between them is what an app rebuilt anywhere could differ by, and
a rebuilt app whose CDHash is not the boot package's trust cache entry does not launch.
"""

import os
import plistlib
import unittest
import zipfile
from pathlib import Path

# Info.plist keys of the build machine rather than of the inputs (rules_apple's environment plist)
HOST_KEYS = {'BuildMachineOSBuild'}
APPS = os.environ['APPS'].split()
TWINS = [path for path in APPS if path.endswith('.twin.ipa')]


def members(ipa: str) -> dict[str, bytes]:
    with zipfile.ZipFile(ipa) as archive:
        return {info.filename: archive.read(info) for info in archive.infolist()}


class Reproducible(unittest.TestCase):
    def test_the_twin_is_the_same_app(self):
        self.assertEqual(len(TWINS), 1)
        for twin_ipa in TWINS:
            [app_ipa] = [path for path in APPS if path == twin_ipa.removesuffix('.twin.ipa') + '.ipa']
            app, twin = members(app_ipa), members(twin_ipa)
            self.assertEqual(sorted(app), sorted(twin))
            self.assertEqual([name for name in app if app[name] != twin[name]], [])
            self.assertEqual(Path(app_ipa).read_bytes(), Path(twin_ipa).read_bytes())

    def test_no_app_names_its_build_machine(self):
        for ipa in APPS:
            for name, data in members(ipa).items():
                if name.endswith('.app/Info.plist'):
                    with self.subTest(app=name):
                        self.assertEqual(HOST_KEYS & set(plistlib.loads(data)), set())


if __name__ == '__main__':
    unittest.main()
