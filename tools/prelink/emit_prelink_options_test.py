"""--options (the release's prelink.json) parses to the arguments kernel_prelink's flags give."""

import argparse
import json
import tempfile
import unittest
from pathlib import Path

import emit_prelink

OPTIONS = {
    'base_roots': ['com.apple.driver.AppleMobileFileIntegrity', 'com.apple.filesystems.apfs'],
    'cuts': ['com.apple.iokit.IOUSBHostFamily=com.apple.driver.AppleSMC=1'],
    'defers': ['com.apple.iokit.IOUSBHostFamily=AppleUSBHostResources'],
    'early_security_init': ['com.apple.driver.AppleMobileFileIntegrity=0xfffffff005ab4ae0'],
    'new_roots': ['com.apple.driver.usb.AppleUSBHub', 'com.apple.iokit.IOUSBHostFamily'],
    'slide': '-0x1000000',
}

# What kernel/rules/kernel.bzl's kernel_prelink passes for OPTIONS.
FLAGS = [
    *(f'--root={r}' for r in OPTIONS['base_roots'] + OPTIONS['new_roots']),
    *(f'--new-root={r}' for r in OPTIONS['new_roots']),
    *(f'--cut={c}' for c in OPTIONS['cuts']),
    *(f'--defer={d}' for d in OPTIONS['defers']),
    *(f'--early-security-init={i}' for i in OPTIONS['early_security_init']),
    f'--slide={OPTIONS["slide"]}',
]

FIELDS = ('root', 'new_root', 'cut', 'defer', 'slide', 'early_security_init')


def parse(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(exit_on_error=False)
    emit_prelink.original_arguments(parser)
    args = parser.parse_args(['--prelinked=k', '--inherit-original-report=r', '--previous=p', *argv])
    emit_prelink.take_options(parser, args)
    return args


class OptionsTest(unittest.TestCase):
    def test_options_match_flags(self) -> None:
        with tempfile.TemporaryDirectory() as scratch:
            path = Path(scratch) / 'prelink.json'
            path.write_text(json.dumps(OPTIONS))
            from_options = parse([f'--options={path}'])
        from_flags = parse(FLAGS)
        for field in FIELDS:
            self.assertEqual(getattr(from_options, field), getattr(from_flags, field), field)

    def test_options_and_flags_refused_together(self) -> None:
        with self.assertRaises(SystemExit):
            parse(['--options=prelink.json', '--slide=0'])

    def test_flags_need_roots_and_slide(self) -> None:
        with self.assertRaises(SystemExit):
            parse(['--slide=0'])


if __name__ == '__main__':
    unittest.main()
