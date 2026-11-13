"""check.py's milestones against kernel log lines from emulator runs."""

import contextlib
import io
import re
import unittest
from pathlib import Path
from unittest import mock

from tools.emu import check

LAUNCHD = re.compile(check.MILESTONES['launchd'])
EARLY_BOOT = re.compile(check.MILESTONES['early-boot'])


class FilesystemImageTest(unittest.TestCase):
    def test_filesystem_milestones_reject_a_missing_image_before_building(self):
        for milestone in ('root', 'launchd', 'early-boot'):
            for sd in ([], ['--sd=none']):
                with self.subTest(milestone=milestone, sd=sd):
                    argv = ['check', '--kind=ios27', f'--until={milestone}', *sd]
                    error = io.StringIO()
                    with (
                        mock.patch.object(check.sys, 'argv', argv),
                        mock.patch.object(check.boot, 'default_image', return_value=None),
                        mock.patch.object(check.boot, 'prepare') as prepare,
                        contextlib.redirect_stderr(error),
                        self.assertRaises(SystemExit) as raised,
                    ):
                        check.main()
                    self.assertEqual(raised.exception.code, 2)
                    self.assertIn('requires an iOS filesystem image', error.getvalue())
                    prepare.assert_not_called()

    def test_default_image_is_passed_to_the_boot(self):
        image = Path('/fixture/IOS27.SII.001')
        with (
            mock.patch.object(check.sys, 'argv', ['check', '--kind=ios27']),
            mock.patch.object(check.boot, 'default_image', return_value=image),
            mock.patch.object(check.boot, 'prepare', side_effect=RuntimeError('prepared')) as prepare,
            self.assertRaisesRegex(RuntimeError, 'prepared'),
        ):
            check.main()
        self.assertEqual(prepare.call_args.args[0].sd, str(image))

    def test_early_hardware_milestones_allow_an_empty_card(self):
        for milestone in ('cores', 'display', 'gpu', 'sd'):
            with (
                self.subTest(milestone=milestone),
                mock.patch.object(check.sys, 'argv', ['check', '--kind=ios27', f'--until={milestone}', '--sd=none']),
                mock.patch.object(check.boot, 'prepare', side_effect=RuntimeError('prepared')) as prepare,
                self.assertRaisesRegex(RuntimeError, 'prepared'),
            ):
                check.main()
            self.assertEqual(prepare.call_args.args[0].sd, 'none')


class LaunchdMilestoneTest(unittest.TestCase):
    def test_the_kernels_mounts_before_launchd_do_not_count(self):
        for line in (
            (
                '[    17.834832] apfs_log_op_with_proc:3290: disk1s3 mounting volume SwitchPrebootTrial, '
                'requested by: init (pid 1); parent: kernel_task (pid 0)'
            ),
            (
                '[    17.277098] apfs_log_op_with_proc:3290: disk1s1 mounting volume RaveB24B119.J181J182OS, '
                'requested by: kernel_task (pid 0); parent: kernel_task (pid 0)'
            ),
            '[    28.386237] load_init_program: attempting to load /sbin/launchd',
        ):
            self.assertIsNone(LAUNCHD.search(line), line)

    def test_launchd_running_counts(self):
        line = (
            '[    24.113775] shared_region: 0x29a3324b708ef361 [1(launchd)] check_np(0x16ef6af18) '
            'vm_shared_region_start_address() returned 0x1'
        )
        self.assertIsNotNone(LAUNCHD.search(line))

    def test_a_job_launchd_spawned_counts(self):
        line = (
            '[    31.002114] apfs_log_op_with_proc:3290: disk1s2 mounting volume Data, requested by: mount_apfs '
            '(pid 34); parent: launchd (pid 1)'
        )
        self.assertIsNotNone(LAUNCHD.search(line))


class EarlyBootMilestoneTest(unittest.TestCase):
    def test_launchd_dying_after_its_first_dyld_calls_does_not_count(self):
        for line in (
            (
                '[     2.544877] shared_region: 0xe003d428dd7aa83b [1(launchd)] check_np(0x16f131e88) '
                'vm_shared_region_start_address() returned 0x1'
            ),
            (
                '[     2.547348] pid 1 exited -- exit reason namespace 6 subcode 0x1, description Library not '
                'loaded: /usr/lib/libSystem.B.dylib'
            ),
            (
                '[     4.132320] [System Event] [79340552] [INFO] [Subsystem: launchd] '
                '[Event: workload-properties-init] Doing boot task'
            ),
        ):
            self.assertIsNone(EARLY_BOOT.search(line), line)

    def test_early_boot_complete_counts_after_other_output_on_the_line(self):
        line = (
            '[     4.129727] virtual bool CoreAnalyticsMessenger::attach(IOService *)::81:attaching[System Event] '
            '[85089740] [INFO] [Subsystem: launchd] [Event: boot] Early boot complete. Continuing system boot.'
        )
        self.assertIsNotNone(EARLY_BOOT.search(line))


if __name__ == '__main__':
    unittest.main()
