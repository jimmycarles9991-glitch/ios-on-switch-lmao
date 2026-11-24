"""dispatch.py's checks over boot 18's lines (11-09, iOS 27), and reports.py's reading of the crashes behind them."""

import json
import tempfile
import unittest
from pathlib import Path

from tools.devicerun import devicerun, hwtest
from tools.devicerun.dispatch import DISPATCH
from tools.devicerun.reports import stale_sync_waiter
from tools.devicerun.rules import FAIL, NA, NO_DATA, PASS

SETUP = '{"msg":"locationd main - setup"}'
FAULT = (
    'SwitchCrash: user pc dsc 0x1aba05cac lr dsc 0x1af3cfbbc, frames dsc 0x1af3d49c8 dsc 0x1af3d5470 dsc 0x1af3dfc8c '
    'dsc 0x1af3df6b0 dsc 0x18053c238 dsc 0x18053b910; far dsc 0x1e2069874'
)
UNMAPPED = 'SwitchCrash: user pc dsc 0x1af3e5f80 lr dsc 0x1af3e5f80, frames dsc 0x1af3d4b74; far 0x87bb85ff0'
# Synthetic: no device has printed a SwitchCPU line yet. This is the yield loop the KU burn is suspected to be.
HOG = 'SwitchCPU: user pc dsc 0x23c991a54 lr dsc 0x18054264c, frames dsc 0x1af3e5f80 dsc 0x1af3d4b74'


# kernel/tests/xnu_workloop_handoff_test.cpp formats these same lines.
WAIT_UNHELD = (
    'SwitchWL handoff wait-unheld #1: SwitchHWTest[412] thread 0x2f1c workloop 0x10471c0c0 ident 0x2a03 dq_state '
    '0x60000000002a03 fflags 0x4 flags 0x11 owner none servicer 0x2f20'
)
DROP_WAKE = (
    'SwitchWL handoff drop-wake #16: locationd[73] thread 0x336 workloop 0xc1d0a4000 ident 0x4c03 dq_state 0x0 '
    'fflags 0x28 flags 0x6 owner other servicer self waiter 0x341'
)
HANDOFF_RUN = 'HWTEST dispatch.sync_handoff PASS finished=1 retarget_crossed=16 overlaps=0 ms=8000.0'
SUITE_27 = 'HWTEST suite PASS pass=1 fail=0 skip=0 ios=27 ms=1.0'
SUITE_17 = 'HWTEST suite PASS pass=1 fail=0 skip=0 ios=17 ms=1.0'


def record(process: str, message: str, second: int = 0) -> dict:
    path = {
        'kernel': 'kernel.development',
        'hwtest': '/Applications/SwitchHWTest.app/SwitchHWTest',
    }.get(process, '/usr/libexec/' + process)
    return {
        'timestamp': f'2026-11-09 16:45:{second:02d}.000000+0000',
        'bootUUID': '18AB01C1',
        'processImagePath': path,
        'subsystem': {'locationd': 'com.apple.locationd.Core', 'hwtest': hwtest.SUBSYSTEM}.get(process, ''),
        'category': '',
        'messageType': 'Default',
        'eventMessage': message,
    }


def results(*records: dict) -> dict:
    suite = [check for check in hwtest.HWTEST if check.name == hwtest.SUITE]
    found, _ = devicerun.run_checks(list(records), DISPATCH + suite)
    return {r.check.name: r for r in found}


def report(directory: Path, name: str, body: dict) -> Path:
    path = directory / name
    path.write_text(json.dumps({'bug_type': '309', 'name': 'locationd'}) + '\n' + json.dumps(body) + '\n')
    return path


def faulting(*symbols: str, region: str = '') -> dict:
    return {
        'faultingThread': 0,
        'threads': [{'frames': [{'symbol': s} for s in symbols]}],
        'vmRegionInfo': region,
    }


class Dispatch(unittest.TestCase):
    def test_boot_18s_respawns_and_cache_writes_fail(self):
        got = results(
            record('locationd', SETUP, 1),
            record('kernel', FAULT, 2),
            record('locationd', SETUP, 3),
            record('kernel', FAULT, 8),
            record('locationd', SETUP, 9),
        )
        self.assertEqual(got['dispatch.locationd-starts'].verdict, FAIL)
        self.assertEqual(got['dispatch.shared-cache-fault'].verdict, FAIL)

    def test_one_start_and_a_fault_outside_the_cache_pass(self):
        got = results(record('locationd', SETUP, 1), record('kernel', UNMAPPED, 2))
        self.assertEqual(got['dispatch.locationd-starts'].verdict, PASS)
        self.assertEqual(got['dispatch.shared-cache-fault'].verdict, PASS)

    def test_a_thread_over_its_cpu_limit_fails_with_where_it_loops(self):
        got = results(record('locationd', SETUP, 1), record('kernel', HOG, 2))
        self.assertEqual(got['dispatch.cpu-hog'].verdict, FAIL)
        self.assertIn('0x18054264c', got['dispatch.cpu-hog'].observed)
        self.assertEqual(results(record('locationd', SETUP, 1))['dispatch.cpu-hog'].verdict, PASS)

    def test_a_sync_wait_on_an_earlier_waits_knote_fails(self):
        early = (
            'SwitchWL: locationd[73] thread 0x336: sync wait on workloop 0xc1d0a4000 (ident 0x4c03) returned at once '
            'on a knote already woken for this thread 0x336 (sfflags 0xc)'
        )
        got = results(record('locationd', SETUP, 1), record('kernel', early, 2))
        self.assertEqual(got['dispatch.early-sync-wait'].verdict, FAIL)
        self.assertIn('locationd[73]', got['dispatch.early-sync-wait'].observed)
        self.assertEqual(results(record('locationd', SETUP, 1))['dispatch.early-sync-wait'].verdict, PASS)


class WorkloopHandoff(unittest.TestCase):
    def test_a_daemons_handoff_lines_fail_per_kind_and_process(self):
        got = results(record('kernel', DROP_WAKE, 2), record('kernel', DROP_WAKE.replace('#16', '#32'), 3))
        self.assertEqual(got['boot.workloop-handoff'].verdict, FAIL)
        self.assertIn('drop-wake/locationd x2', got['boot.workloop-handoff'].observed)

    def test_hwtests_own_lines_in_its_run_are_expected_and_prove_the_probe_runs(self):
        got = results(
            record('kernel', WAIT_UNHELD, 3),
            record('kernel', WAIT_UNHELD.replace('wait-unheld', 'drop-wake'), 4),
            record('hwtest', HANDOFF_RUN, 9),
            record('hwtest', SUITE_27, 10),
        )
        self.assertEqual(got['boot.workloop-handoff'].verdict, PASS)
        self.assertIn('expected', got['boot.workloop-handoff'].observed)
        self.assertEqual(got['boot.workloop-handoff-live'].verdict, PASS)

    def test_hwtests_lines_outside_its_run_fail(self):
        got = results(record('hwtest', HANDOFF_RUN, 9), record('kernel', WAIT_UNHELD, 30))
        self.assertEqual(got['boot.workloop-handoff'].verdict, FAIL)

    def test_stale_wake_and_owner_dead_still_fail_inside_hwtests_run(self):
        for kind in ('stale-wake', 'owner-dead'):
            with self.subTest(kind=kind):
                got = results(
                    record('kernel', WAIT_UNHELD.replace('wait-unheld', kind), 4),
                    record('hwtest', HANDOFF_RUN, 9),
                )
                self.assertEqual(got['boot.workloop-handoff'].verdict, FAIL)
                self.assertIn(f'{kind}/SwitchHWTest x1', got['boot.workloop-handoff'].observed)

    def test_process_names_with_spaces_match_and_fail(self):
        line = DROP_WAKE.replace('locationd[73]', 'App Store[91]')
        got = results(record('kernel', line, 2))
        self.assertEqual(got['boot.workloop-handoff'].verdict, FAIL)
        self.assertIn('drop-wake/App Store x1', got['boot.workloop-handoff'].observed)

    def test_a_run_without_probe_lines_fails_on_27_and_is_not_applicable_on_17(self):
        live = 'boot.workloop-handoff-live'
        run = record('hwtest', HANDOFF_RUN, 9)
        self.assertEqual(results(run, record('hwtest', SUITE_27, 10))[live].verdict, FAIL)
        self.assertEqual(results(run, record('hwtest', SUITE_17, 10))[live].verdict, NA)
        self.assertEqual(results(record('hwtest', SUITE_27, 10))[live].verdict, NO_DATA)


class StaleSyncWaiter(unittest.TestCase):
    def test_signatures(self):
        constant = '--->  __AUTH_CONST  1e33a15e0-1e34f3098 [ 1351K] r--/rw- SM=COW  /System/.../CoreFoundation'
        stack = '--->  Stack  16fa04000-16fa8c000 [  544K] rw-/rwx SM=PRV  thread 3'
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            owned = {'asi': {'libdispatch.dylib': ['BUG IN CLIENT OF LIBDISPATCH: Invalid workloop owner, possible']}}
            cases = {
                'voucher': (
                    faulting('interfaceNameCreateWithoutPortSuffix', '_dispatch_lane_serial_drain', region=constant),
                    True,
                ),
                'enqueuer': (faulting('__DISPATCH_WAIT_FOR_ENQUEUER__', '_dispatch_lane_serial_drain'), True),
                'owner': (owned, True),
                'heap': (faulting('objc_msgSend', '_dispatch_lane_serial_drain', region=stack), False),
                'constant-elsewhere': (faulting('CFRetain', 'main', region=constant), False),
            }
            for name, (body, want) in cases.items():
                self.assertEqual(stale_sync_waiter(report(directory, f'{name}.ips', body)), want, name)


if __name__ == '__main__':
    unittest.main()
