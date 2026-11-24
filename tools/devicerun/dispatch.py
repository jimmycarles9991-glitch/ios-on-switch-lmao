"""libdispatch's serial queues on iOS 27: a dispatch_sync waiter's context reached after its thread has left it.

Boot 18 (11-09) is the baseline: locationd started three times (pids 73, 196, 212). The first two died seconds in
when their cohort queue's drainer retained a voucher read from a reused stack, CFSTR("com.apple.locationd"), a write
into CoreFoundation's __AUTH_CONST. reports.py's crash.stale-sync-waiter row reads the same crashes from the .ips files.
"""

from tools.devicerun.hwtest import SUBSYSTEM as HWTEST_SUBSYSTEM, SUITE
from tools.devicerun.rules import REPORT, Check

OWNER = 'r132-locationd'
KERNEL = 'kernel'
# Each run of HW Test's dispatch.sync_handoff, a window back from its line by its ms=: its retargeted queues wake their
# waiters from the other workloop, which the kernel's SwitchWL handoff probe logs as wait-unheld and drop-wake (and
# end-enoent when a cancel beats the wait) for HW Test itself.
HANDOFF_RUNS = 'dispatch.sync-handoff-runs'
HANDOFF = 'SwitchWL handoff '
HWTEST_PROCESS = r': SwitchHWTest\w*\['

DISPATCH = [
    Check(
        'dispatch.locationd-starts',
        OWNER,
        '"msg":"locationd main - setup"',
        'count <= 1',
        process='locationd',
        source='locationd',
        note='any restart fails it; boot 18: 3 (two crashed on their cohort queue); crash.stale-sync-waiter names why',
    ),
    Check(
        'dispatch.shared-cache-fault',
        OWNER,
        '; far dsc ',
        process=KERNEL,
        regex=r'user pc dsc (?P<pc>0x[0-9a-f]+) lr dsc (?P<lr>0x[0-9a-f]+)',
        show=('pc', 'lr'),
        source='kernel/switch/diag/crash.c',
        note='a crash whose fault address is in the dyld cache; boot 18: locationd x2, lr dsc 0x1af3cfbbc (24B119)',
    ),
    Check(
        'dispatch.early-sync-wait',
        OWNER,
        'returned at once on a knote already woken for',
        process=KERNEL,
        regex=r'SwitchWL: (?P<who>\S+\[\d+\]) thread .* woken for (?P<waiter>this|another) thread',
        show=('who', 'waiter'),
        source='kernel/switch/diag/workloop.c',
        note="a sync waiter given ownership by an earlier wait's wake; before a locationd SwitchCrash, the kernel side",
    ),
    Check(
        'dispatch.cpu-hog',
        OWNER,
        'SwitchCPU: user pc ',
        process=KERNEL,
        regex=r'user pc (?P<pc>(?:dsc |main\+)?0x[0-9a-f]+) lr (?P<lr>(?:dsc |main\+)?0x[0-9a-f]+)',
        show=('pc', 'lr'),
        source='kernel/switch/diag/crash.c',
        note=(
            'a thread over its CPU limit, where its loop runs; slice KU (11-10): locationd thread 0x624, 50% for 3 '
            'min, before this line existed'
        ),
    ),
    Check(
        HANDOFF_RUNS,
        OWNER,
        'HWTEST dispatch.sync_handoff ',
        REPORT,
        subsystem=HWTEST_SUBSYSTEM,
        regex=(
            r'^HWTEST dispatch\.sync_handoff (?:PASS|FAIL) .*\bretarget_crossed=(?P<crossed>\d+)\b'
            r'.* ms=(?P<ms>[\d.]+)(?: why=|$)'
        ),
        show=('crossed',),
        source='apps/hwtest/tests/DispatchSyncHandoffTest.swift',
        note='each run of dispatch.sync_handoff and its rounds whose waiter was woken from the other workloop',
    ),
    Check(
        'boot.workloop-handoff',
        OWNER,
        HANDOFF,
        process=KERNEL,
        regex=r'SwitchWL handoff (?P<kind>[a-z-]+) #(?P<n>\d+): (?P<who>[^\[]+)\[\d+\]',
        show=('kind', 'who'),
        during=HANDOFF_RUNS,
        excusable=r'SwitchWL handoff (?:wait-unheld|drop-wake|end-enoent) #\d+' + HWTEST_PROCESS,
        source='kernel/switch/diag/workloop.c',
        note=(
            "a dispatch_sync hand-off the workloop contract allows only for a queue retargeted mid-sync (HW Test's "
            "own, in its window, are expected) or never: per kind and process; a line's #n is the running count"
        ),
    ),
    Check(
        'boot.workloop-handoff-live',
        OWNER,
        HANDOFF,
        'count > 0',
        process=KERNEL,
        regex=r'SwitchWL handoff (?P<kind>wait-unheld|drop-wake) #\d+' + HWTEST_PROCESS,
        show=('kind',),
        needs=HANDOFF_RUNS,
        applies=f'{SUITE} ios=27',
        source='kernel/switch/diag/workloop.c',
        note="dispatch.sync_handoff's cross-workloop wakes reach the probe; without these lines it is not running",
    ),
]
