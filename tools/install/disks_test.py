"""An image's disk is detached for certain: a refused eject is forced and named, one that stays attached fails."""

import unittest
from unittest import mock

from tools.install import disks, host

BUSY = 'hdiutil detach disk9 failed (16): hdiutil: couldn\'t eject "disk9" - Resource busy'
ENTITIES = {
    'system-entities': [
        {'dev-entry': '/dev/disk9'},
        {'dev-entry': '/dev/disk9s1', 'content-hint': disks.APFS_VOLUME},
    ]
}


class FakeHdiutil:
    def __init__(self, refusals: int) -> None:
        self.refusals = refusals
        self.detaches: list[tuple[str, ...]] = []

    def run(self, _tool: str, *args: object, **options: object) -> str:
        if args[0] == 'detach':
            self.detaches.append(tuple(map(str, args)))
            if len(self.detaches) <= self.refusals and options.get('check', True):
                raise host.HostError(BUSY)
        return ''


class Log:
    def __init__(self) -> None:
        self.lines: list[str] = []

    def __call__(self, *parts: object) -> None:
        self.lines.append(' '.join(map(str, parts)))

    def quiet(self, *parts: object) -> None:
        pass


class AttachedTest(unittest.TestCase):
    def attach_once(self, refusals: int) -> tuple[FakeHdiutil, Log]:
        hdiutil, log = FakeHdiutil(refusals), Log()
        with (
            mock.patch.object(disks.host, 'plist', return_value=ENTITIES),
            mock.patch.object(disks.host, 'run', hdiutil.run),
            disks.attached('image.dmg', log, raw=True) as attachment,
        ):
            self.assertEqual((attachment.whole, attachment.volumes), ('disk9', ['disk9s1']))
        return hdiutil, log

    def test_a_disk_is_detached_once_when_macos_lets_it_go(self) -> None:
        hdiutil, log = self.attach_once(refusals=0)
        self.assertEqual(hdiutil.detaches, [('detach', 'disk9')])
        self.assertEqual(log.lines, [])

    def test_a_refused_detach_is_forced_and_named(self) -> None:
        hdiutil, log = self.attach_once(refusals=1)
        self.assertEqual(hdiutil.detaches, [('detach', 'disk9'), ('detach', '-force', 'disk9')])
        self.assertEqual(len(log.lines), 1)
        self.assertIn('disk9: detach refused, forcing it', log.lines[0])
        self.assertIn('Resource busy', log.lines[0])

    def test_a_disk_that_stays_attached_fails_the_step(self) -> None:
        with self.assertRaises(host.HostError):
            self.attach_once(refusals=2)


if __name__ == '__main__':
    unittest.main()
