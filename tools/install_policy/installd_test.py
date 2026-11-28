import plistlib
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import installd

from tools.common import macho


def executable(subtype: int = 0, payload: int = 512) -> bytes:
    segment = struct.pack('<II16sQQQQIIII', 0x19, 152, b'__TEXT', 0x100000000, 4096, 0, 4096, 5, 5, 1, 0)
    section = struct.pack(
        '<16s16sQQIIIIIIII', b'__text', b'__TEXT', 0x100000000 + payload, 32, payload, 2, 0, 0, 0x80000400, 0, 0, 0
    )
    header = struct.pack('<8I', 0xFEEDFACF, macho.CPU_TYPE_ARM64, subtype, 2, 1, 152, 0, 0)
    image = bytearray((header + segment + section).ljust(4096, b'\0'))
    image[payload : payload + 32] = bytes(range(32))
    return bytes(image)


def universal(slices: list[bytes], wide: bool = False, endian: str = '>') -> bytes:
    magic = 0xCAFEBABF if wide else 0xCAFEBABE
    entries = []
    for index, image in enumerate(slices):
        subtype = struct.unpack_from('<I', image, 8)[0]
        fields = [macho.CPU_TYPE_ARM64, subtype, 4096 * (index + 1), len(image), 12]
        if wide:
            fields.append(0)
        entries.append(struct.pack(endian + ('IIQQII' if wide else 'IIIII'), *fields))
    return (struct.pack(endian + 'II', magic, len(slices)) + b''.join(entries)).ljust(4096, b'\0') + b''.join(slices)


class InjectionTest(unittest.TestCase):
    def test_required_dependency_preserves_executable_payload_for_both_architectures(self):
        for subtype in (0, 0x80000002):
            with self.subTest(subtype=subtype):
                original = executable(subtype)
                patched = installd.inject(original)
                self.assertEqual(patched[512:], original[512:])
                self.assertEqual(len(patched), len(original))
                image = macho.macho(patched, subtypes=None)
                self.assertEqual(image['cpusubtype'], subtype)
                offset, size = macho.commands_of(image, installd.LC_LOAD_DYLIB)[0]
                self.assertEqual(patched[offset + 24 : offset + size].rstrip(b'\0').decode(), installd.LIBRARY)
                self.assertEqual(installd.inject(patched), patched)

    def test_universal_images_are_thinned_without_changing_selected_architecture(self):
        image = executable(0x80000002)
        for wide in (False, True):
            for endian in ('<', '>'):
                with self.subTest(wide=wide, endian=endian):
                    self.assertEqual(installd.inject(universal([image], wide, endian)), installd.inject(image))

    def test_multiple_arm64_slices_are_ambiguous(self):
        with self.assertRaisesRegex(ValueError, 'exactly one'):
            installd.inject(universal([executable(), executable(0x80000002)]))

    def test_universal_extents_cannot_escape_or_overlap(self):
        for offset, size in ((16, 4096), (4096, 8192)):
            image = bytearray(universal([executable()]))
            struct.pack_into('>II', image, 16, offset, size)
            with self.assertRaisesRegex(ValueError, 'extent'):
                installd.inject(bytes(image))
        image = bytearray(universal([executable(), executable()]))
        struct.pack_into('>I', image, 36, 4096)
        with self.assertRaisesRegex(ValueError, 'overlapping'):
            installd.inject(bytes(image))

    def test_universal_table_and_subtype_are_checked(self):
        image = bytearray(universal([executable()]))
        struct.pack_into('>I', image, 12, 2)
        with self.assertRaisesRegex(ValueError, 'subtypes'):
            installd.inject(bytes(image))
        with self.assertRaisesRegex(ValueError, 'architecture table'):
            installd.inject(struct.pack('>II', 0xCAFEBABE, 8))

    def test_rejects_non_executable_and_unsupported_subtypes(self):
        for location, value in ((12, 6), (8, 1)):
            image = bytearray(executable())
            struct.pack_into('<I', image, location, value)
            with self.assertRaisesRegex(ValueError, 'executable'):
                installd.inject(bytes(image))

    def test_insufficient_or_nonempty_padding_is_not_overwritten(self):
        with self.assertRaisesRegex(ValueError, 'insufficient'):
            installd.inject(executable(payload=224))
        image = bytearray(executable())
        image[184] = 1
        with self.assertRaisesRegex(ValueError, 'not empty'):
            installd.inject(bytes(image))

    def test_weak_dependency_does_not_count_as_required_policy(self):
        image = bytearray(installd.inject(executable()))
        struct.pack_into('<I', image, 184, 0x80000018)
        with self.assertRaisesRegex(ValueError, 'required dependency'):
            installd.inject(bytes(image))

    def test_dylib_names_and_command_extents_are_checked(self):
        original = installd.inject(executable())
        for location, value in ((188, 0x1000), (192, 0x1000)):
            image = bytearray(original)
            struct.pack_into('<I', image, location, value)
            with self.assertRaises(ValueError):
                installd.inject(bytes(image))
        image = bytearray(original)
        _, length = struct.unpack_from('<II', image, 184)
        image[184 + 24 : 184 + length] = b'x' * (length - 24)
        with self.assertRaisesRegex(ValueError, 'unterminated'):
            installd.inject(bytes(image))

    def test_segment_and_section_file_bounds_are_checked(self):
        for location, value in ((80, 8192), (152, 4080)):
            image = bytearray(executable())
            struct.pack_into('<Q' if location == 80 else '<I', image, location, value)
            with self.assertRaisesRegex(ValueError, 'segment'):
                installd.inject(bytes(image))


class PreparationTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name) / 'os'
        version = self.root / 'System/Library/CoreServices/SystemVersion.plist'
        version.parent.mkdir(parents=True)
        version.write_bytes(plistlib.dumps({'ProductBuildVersion': '24B119'}))
        self.source = self.root / installd.SOURCE
        self.source.parent.mkdir(parents=True)
        self.source.write_bytes(executable(0x80000002))
        self.out = Path(self.tmp.name) / 'out/installd'
        self.rights = {'seatbelt-profiles': ['installd'], 'com.apple.private.MobileContainerManager.allowed': True}

    def test_signing_preserves_original_entitlements_and_does_not_modify_source(self):
        original = self.source.read_bytes()
        commands = []

        def run(command: list[str], **_kwargs: object) -> subprocess.CompletedProcess[bytes]:
            commands.append(command)
            if '--sign' in command:
                rights = Path(command[command.index('--entitlements') + 1])
                self.assertEqual(plistlib.loads(rights.read_bytes()), self.rights)
            return subprocess.CompletedProcess(command, 0)

        with (
            mock.patch.object(installd, 'entitlements', side_effect=[self.rights, self.rights]),
            mock.patch.object(installd.subprocess, 'run', side_effect=run),
        ):
            installd.prepare(self.root, {'build': '24B119'}, self.out)
        self.assertEqual(self.source.read_bytes(), original)
        self.assertEqual(self.out.read_bytes(), installd.inject(original))
        self.assertEqual(self.out.stat().st_mode & 0o777, 0o755)
        self.assertIn('--generate-entitlement-der', commands[0])
        self.assertIn('--timestamp=none', commands[0])
        self.assertIn('--verify', commands[1])

    def test_wrong_build_is_rejected_before_signing(self):
        with mock.patch.object(installd, 'entitlements') as read:
            with self.assertRaisesRegex(ValueError, 'selected IPSW'):
                installd.prepare(self.root, {'build': '21H461'}, self.out)
            read.assert_not_called()
        self.assertFalse(self.out.exists())

    def test_entitlement_change_does_not_replace_previous_output(self):
        self.out.parent.mkdir()
        self.out.write_bytes(b'previous')
        with (
            mock.patch.object(installd, 'entitlements', side_effect=[self.rights, {'other': True}]),
            mock.patch.object(installd.subprocess, 'run'),
            self.assertRaisesRegex(ValueError, 'changed installd entitlements'),
        ):
            installd.prepare(self.root, {'build': '24B119'}, self.out)
        self.assertEqual(self.out.read_bytes(), b'previous')

    def test_failed_signing_does_not_publish_output(self):
        with (
            mock.patch.object(installd, 'entitlements', return_value=self.rights),
            mock.patch.object(installd.subprocess, 'run', side_effect=subprocess.CalledProcessError(1, 'codesign')),
            self.assertRaises(subprocess.CalledProcessError),
        ):
            installd.prepare(self.root, {'build': '24B119'}, self.out)
        self.assertFalse(self.out.exists())

    def test_arm64e_pretranslates_after_signing_and_rejects_fallback(self):
        original = self.source.read_bytes()
        tool = Path(self.tmp.name) / 'arm64e'
        commands = []

        def run(command: list[str], **_kwargs: object) -> subprocess.CompletedProcess[bytes]:
            commands.append(command)
            if command[0] == str(tool):
                report = Path(command[command.index('-r') + 1])
                src, dst = Path(command[-2]), Path(command[-1])
                patched = bytearray(src.read_bytes())
                struct.pack_into('<I', patched, 28, 0x41363458)
                dst.write_bytes(patched)
                report.write_text('{"slices": [{"status": "translated"}]}')
            return subprocess.CompletedProcess(command, 0)

        with (
            mock.patch.object(installd, 'entitlements', side_effect=[self.rights, self.rights]),
            mock.patch.object(installd.subprocess, 'run', side_effect=run),
        ):
            installd.prepare(self.root, {'build': '24B119'}, self.out, arm64e=tool)
        self.assertEqual([cmd[0] for cmd in commands], ['/usr/bin/codesign', str(tool), '/usr/bin/codesign'])
        self.assertEqual(struct.unpack_from('<I', self.out.read_bytes(), 28)[0], 0x41363458)
        self.assertEqual(self.out.stat().st_mode & 0o777, 0o755)
        self.assertEqual(self.source.read_bytes(), original)

        def run_fallback(command: list[str], **_kwargs: object) -> subprocess.CompletedProcess[bytes]:
            if command[0] == str(tool):
                report = Path(command[command.index('-r') + 1])
                Path(command[-1]).write_bytes(Path(command[-2]).read_bytes())
                report.write_text('{"slices": [{"status": "fallback", "why": "no room"}]}')
            return subprocess.CompletedProcess(command, 0)

        fallback_out = Path(self.tmp.name) / 'out/fallback'
        with (
            mock.patch.object(installd, 'entitlements', return_value=self.rights),
            mock.patch.object(installd.subprocess, 'run', side_effect=run_fallback),
            self.assertRaisesRegex(ValueError, 'no room'),
        ):
            installd.prepare(self.root, {'build': '24B119'}, fallback_out, arm64e=tool)
        self.assertFalse(fallback_out.exists())


if __name__ == '__main__':
    unittest.main()
