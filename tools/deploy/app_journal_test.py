"""The registration journal a deploy leaves for switch-appregd, and what its list of applied entries drops."""

import plistlib
import tempfile
import unittest
from pathlib import Path

from tools.deploy import app_journal
from tools.deploy.registration import App

HWTEST = App('com.brunostjohn.switch.hwtest', 'a')
HWTEST27E = App('com.brunostjohn.switch.hwtest27e', 'b')


class Log:
    def __init__(self) -> None:
        self.lines = []

    def __call__(self, *parts: object) -> None:
        self.lines.append(' '.join(map(str, parts)))


def identifiers(entries: dict) -> dict:
    return {path: entry[app_journal.IDENTIFIER] for path, entry in entries.items()}


class ChangesTest(unittest.TestCase):
    def test_added_changed_and_removed_apps(self):
        before = {'SwitchHWTest.app': HWTEST, 'Old.app': App('com.brunostjohn.switch.old', 'c')}
        after = {'SwitchHWTest.app': HWTEST._replace(digest='z'), 'SwitchHWTest27e.app': HWTEST27E}
        register, unregister = app_journal.changes(before, after)
        self.assertEqual(
            register,
            {
                '/Applications/SwitchHWTest.app': 'com.brunostjohn.switch.hwtest',
                '/Applications/SwitchHWTest27e.app': 'com.brunostjohn.switch.hwtest27e',
            },
        )
        self.assertEqual(unregister, {'/Applications/Old.app': 'com.brunostjohn.switch.old'})

    def test_an_app_that_changes_identifier_in_place_is_unregistered_then_registered(self):
        register, unregister = app_journal.changes({'A.app': HWTEST}, {'A.app': HWTEST27E})
        self.assertEqual(register, {'/Applications/A.app': HWTEST27E.identifier})
        self.assertEqual(unregister, {'/Applications/A.app': HWTEST.identifier})

    def test_an_app_that_moves_keeps_its_record(self):
        register, unregister = app_journal.changes({'A.app': HWTEST}, {'B.app': HWTEST})
        self.assertEqual(register, {'/Applications/B.app': HWTEST.identifier})
        self.assertEqual(unregister, {})


class JournalTest(unittest.TestCase):
    def setUp(self):
        self.data = Path(tempfile.mkdtemp())
        self.journal = self.data / app_journal.JOURNAL

    def applied(self) -> None:
        tokens = [e[app_journal.TOKEN] for stage in app_journal.pending(self.data).values() for e in stage.values()]
        path = self.data / app_journal.APPLIED
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(plistlib.dumps(tokens))

    def test_each_entry_has_a_token_of_its_own(self):
        app_journal.record(self.data, {}, {'SwitchHWTest.app': HWTEST, 'SwitchHWTest27e.app': HWTEST27E}, Log())
        written = plistlib.loads(self.journal.read_bytes())
        tokens = {entry[app_journal.TOKEN] for entry in written[app_journal.REGISTER].values()}
        self.assertEqual(len(tokens), 2)
        self.assertEqual(written[app_journal.UNREGISTER], {})

    def test_applied_entries_are_dropped_and_the_journal_goes_when_empty(self):
        app_journal.record(self.data, {}, {'SwitchHWTest.app': HWTEST}, Log())
        self.applied()
        self.assertEqual(app_journal.pending(self.data)[app_journal.REGISTER], {})
        self.assertEqual(
            app_journal.record(self.data, {'SwitchHWTest.app': HWTEST}, {'SwitchHWTest.app': HWTEST}, Log()), 0
        )
        self.assertFalse(self.journal.exists())

    def test_entries_never_applied_stay_and_are_reported(self):
        app_journal.record(self.data, {}, {'SwitchHWTest.app': HWTEST}, Log())
        log = Log()
        self.assertEqual(
            app_journal.record(self.data, {'SwitchHWTest.app': HWTEST}, {'SwitchHWTest.app': HWTEST}, log), 1
        )
        self.assertIn('still pending', log.lines[0])

    def test_changed_again_before_a_boot_takes_a_new_token(self):
        app_journal.record(self.data, {}, {'SwitchHWTest.app': HWTEST}, Log())
        first = app_journal.pending(self.data)[app_journal.REGISTER]['/Applications/SwitchHWTest.app']
        changed = HWTEST._replace(digest='z')
        app_journal.record(self.data, {'SwitchHWTest.app': HWTEST}, {'SwitchHWTest.app': changed}, Log())
        second = app_journal.pending(self.data)[app_journal.REGISTER]['/Applications/SwitchHWTest.app']
        self.assertNotEqual(first[app_journal.TOKEN], second[app_journal.TOKEN])

    def test_an_app_removed_before_its_registration_ran_is_only_unregistered(self):
        app_journal.record(self.data, {}, {'SwitchHWTest.app': HWTEST}, Log())
        app_journal.record(self.data, {'SwitchHWTest.app': HWTEST}, {}, Log())
        pending = app_journal.pending(self.data)
        self.assertEqual(pending[app_journal.REGISTER], {})
        self.assertEqual(
            identifiers(pending[app_journal.UNREGISTER]), {'/Applications/SwitchHWTest.app': HWTEST.identifier}
        )

    def test_an_app_back_before_its_unregistration_ran_is_only_registered(self):
        app_journal.record(self.data, {'SwitchHWTest.app': HWTEST}, {}, Log())
        app_journal.record(self.data, {}, {'SwitchHWTest.app': HWTEST}, Log())
        pending = app_journal.pending(self.data)
        self.assertEqual(pending[app_journal.UNREGISTER], {})
        self.assertEqual(
            identifiers(pending[app_journal.REGISTER]), {'/Applications/SwitchHWTest.app': HWTEST.identifier}
        )

    def test_an_unreadable_journal_or_list_counts_as_empty(self):
        self.journal.parent.mkdir(parents=True)
        self.journal.write_bytes(b'not a plist')
        (self.data / app_journal.APPLIED).parent.mkdir(parents=True)
        (self.data / app_journal.APPLIED).write_bytes(plistlib.dumps({'not': 'a list'}))
        self.assertEqual(app_journal.pending(self.data), {app_journal.UNREGISTER: {}, app_journal.REGISTER: {}})

    def test_clear(self):
        app_journal.record(self.data, {}, {'SwitchHWTest.app': HWTEST}, Log())
        app_journal.clear(self.data)
        self.assertFalse(self.journal.exists())


class BootTest(unittest.TestCase):
    def test_boot_args_read_as_the_kernel_reads_them(self):
        self.assertTrue(app_journal.boot_arg('-v switch-no-bt', 'switch-no-bt'))
        self.assertTrue(app_journal.boot_arg('switch-no-bt=1', 'switch-no-bt'))
        self.assertTrue(app_journal.boot_arg('switch-no-bt=0x1', 'switch-no-bt'))
        self.assertFalse(app_journal.boot_arg('switch-no-bt=0', 'switch-no-bt'))
        self.assertFalse(app_journal.boot_arg('switch-no-btx switch-no-audio', 'switch-no-bt'))
        self.assertFalse(app_journal.boot_arg('', 'switch-no-bt'))

    def test_only_ios_17_without_bluetooth_has_nothing_to_register_at_boot(self):
        self.assertTrue(app_journal.NextBoot('27.2', 'switch-no-bt').registers_apps())
        self.assertTrue(app_journal.NextBoot('17.7.11', '-v').registers_apps())
        self.assertFalse(app_journal.NextBoot('17.7.11', 'switch-no-bt').registers_apps())


if __name__ == '__main__':
    unittest.main()
