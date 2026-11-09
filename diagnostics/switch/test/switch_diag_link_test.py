"""The selected kind's libSwitchDiag and kernel, as this build's profile links them.

A release archive holds only SwitchDiag's units (the panic record and screens, the early exception screen, the root
boot): none defines or calls a debug unit's symbol, and the release kernel has none of the HUD's or the root record's
text. The panic record's text is in every kernel. A debug build checks the same reader finds the debug half, so a
release pass is not a reader that sees nothing.

usage: switch_diag_link_test.py --profile=release|debug --archive=libSwitchDiag.a --kernel=mach.*.unstripped.noctf
"""

import argparse
import struct
import sys
import unittest
from pathlib import Path

EVERY_CONFIG_UNITS = {'SwitchDiag.o', 'SwitchPanicEarly.o', 'SwitchPanicSave.o', 'SwitchRootBoot.o'}

KEPT = {
    'switch_diag_panic',
    'switch_diag_init',
    'switch_diag_early_fault',
    'switch_early_record',
    'switch_display_panic_hook',
    'switch_panic_begin',
    'switch_panic_early',
    'switch_panic_save_init',
    'switch_panic_save_pending',
    'switch_root_boot_prepare',
    'boot_progress_prepare_scanout',
}

# One or more names from each debug unit and each debug hook.
DEBUG_ONLY = {
    'switch_diag_user_syscall',
    'switch_diag_stage',
    'switch_diag_process_event',
    'switch_diag_console_putc',
    'switch_diag_hud_init',
    'switch_diag_hud_panic',
    'switch_diag_files_update',
    'switch_diag_graphics_takeover',
    'switch_root_event',
    'switch_root_record',
    'switch_root_record_begin',
    'switch_user_record',
    'switch_block_record',
    'switch_power_trail',
    'switch_power_trail_report',
    'switch_files_probe',
    'switch_sd_probe',
    'switch_fat_mount',
    'switch_raw_apfs_open',
    'switch_sd_read_sectors',
    'switch_split_open',
    'switch_gpt_scan',
    'switch_data_create',
    'switch_block_create',
    'switch_sd_dma_bind',
    'switch_sd_crc32',
}

PANIC_TEXT = [
    b'EARLY XNU EXCEPTION - HALTED',
    b'XNU PANIC   cpu %u',
    b'SwitchPanicSave: %s written',
    b'Switch: SD block device publish failed',
]

# The HUD's screens, the root record's screen and the stage lines XNU's hooks pass only to the debug units.
DEBUG_TEXT = [
    b'SWITCH NATIVE XNU DIAGNOSTICS',
    b'XNU PANIC - EXECUTION STOPPED',
    b'NATIVE ROOT BOOT IN PROGRESS',
    b'READ-ONLY FAT FILE CHECK',
    b'SD SECTOR 0 MATCHES U-BOOT',
    b'bsd_init entered',
    b'launchd exec attempt - EL0 not established',
    b'exec load_machfile enter',
]

MH_MAGIC_64 = 0xFEEDFACF
LC_SYMTAB = 0x2
N_STAB, N_TYPE, N_EXT, N_UNDF, N_SECT = 0xE0, 0x0E, 0x01, 0x0, 0xE


def archive_members(data: bytes) -> list[tuple[str, bytes]]:
    """Return (name, bytes) of each member of a BSD ar archive, #1/<n> long names included."""
    if not data.startswith(b'!<arch>\n'):
        raise ValueError('not an ar archive')
    at, members = 8, []
    while at + 60 <= len(data):
        header = data[at : at + 60]
        name, size = header[:16].decode().strip(), int(header[48:58].decode().strip())
        body = data[at + 60 : at + 60 + size]
        if name.startswith('#1/'):
            length = int(name[3:])
            name, body = body[:length].rstrip(b'\0').decode(), body[length:]
        members.append((name, body))
        at += 60 + size + (size & 1)
    return members


def symbols(obj: bytes) -> tuple[set[str], set[str]]:
    """Return the (defined, undefined) names of a 64-bit Mach-O object, without the leading underscore."""
    magic, _, _, _, ncmds, _, _, _ = struct.unpack_from('<IiiIIIII', obj, 0)
    if magic != MH_MAGIC_64:
        raise ValueError('not a 64-bit Mach-O object')
    at, defined, undefined = 32, set(), set()
    for _ in range(ncmds):
        cmd, size = struct.unpack_from('<II', obj, at)
        if cmd == LC_SYMTAB:
            symoff, nsyms, stroff, _ = struct.unpack_from('<IIII', obj, at + 8)
            for i in range(nsyms):
                strx, kind, _, _, _ = struct.unpack_from('<IBBhQ', obj, symoff + 16 * i)
                if kind & N_STAB:
                    continue
                name = obj[stroff + strx : obj.index(b'\0', stroff + strx)].decode().removeprefix('_')
                if kind & N_TYPE == N_SECT:
                    defined.add(name)
                elif kind & N_TYPE == N_UNDF and kind & N_EXT:
                    undefined.add(name)
        at += size
    return defined, undefined


class SwitchDiagLink(unittest.TestCase):
    profile = archive = kernel = None

    def setUp(self):
        self.members = [(n, b) for n, b in archive_members(Path(self.archive).read_bytes()) if n.endswith('.o')]
        self.units = {n: symbols(b) for n, b in self.members}
        self.defined = set().union(*(d for d, _ in self.units.values()))
        self.text = Path(self.kernel).read_bytes()

    def test_every_config_keeps_the_panic_record_and_the_root_boot(self):
        self.assertTrue(set(self.units) >= EVERY_CONFIG_UNITS, sorted(self.units))
        self.assertEqual(KEPT - self.defined, set())
        for text in PANIC_TEXT:
            self.assertIn(text, self.text, text)

    def test_release_links_no_debug_unit(self):
        if self.profile != 'release':
            self.skipTest('a debug build')
        self.assertEqual(set(self.units), EVERY_CONFIG_UNITS)
        for name, (defined, undefined) in self.units.items():
            self.assertEqual(DEBUG_ONLY & defined, set(), name)
            self.assertEqual(DEBUG_ONLY & undefined, set(), name)
        for text in DEBUG_TEXT:
            self.assertNotIn(text, self.text, text)

    def test_debug_links_the_debug_units(self):
        if self.profile != 'debug':
            self.skipTest('a release build')
        self.assertEqual(DEBUG_ONLY - self.defined, set())
        for text in DEBUG_TEXT:
            self.assertIn(text, self.text, text)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--profile', choices=['release', 'debug'], required=True)
    parser.add_argument('--archive', required=True)
    parser.add_argument('--kernel', required=True)
    args, rest = parser.parse_known_args()
    SwitchDiagLink.profile, SwitchDiagLink.archive, SwitchDiagLink.kernel = args.profile, args.archive, args.kernel
    unittest.main(argv=[sys.argv[0], *rest])
