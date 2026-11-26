"""The 21H461 prelink's code is Apple's except at the kind's declared sites, each original word checked first."""

import array
import hashlib
import struct
import symtable
import unittest
from pathlib import Path

import emit_prelink

from kits.SwitchABI.sites import Site

KEXT = 'com.example.kext'
TEXT_VA = 0x4000
APPLE = struct.pack('<8I', 0xA9BF7BFD, 0x71000A9F, 0x52A00808, 0xD503201F, 0x320A0108, 0x52800023, 0xD65F03C0, 0)


def cache(code: bytes = APPLE) -> emit_prelink.Kernelcache:
    segment = {'name': '__TEXT_EXEC', 'va': TEXT_VA, 'size': len(code), 'filesize': len(code), 'fileoff': 0}
    return emit_prelink.Kernelcache(code, [segment], {}, {KEXT: {'segments': [segment]}})


def copied(code: bytes = APPLE) -> emit_prelink.CoalescedImage:
    image = emit_prelink.CoalescedImage()
    image.append_segment('__PLK_TEXT_EXEC', TEXT_VA, code, emit_prelink.VM_PAGE, 5)
    return image


def no_native_sites() -> emit_prelink.NativeSites:
    return emit_prelink.NativeSites(emit_prelink.NativePointers([], array.array('Q'), array.array('I'), []))


SITES = [
    Site(KEXT, TEXT_VA + 4, 0x71000A9F, 0x7100029F, 'check'),
    Site(KEXT, TEXT_VA + 16, 0x320A0108, 0xD503201F, 'orr'),
]


class SitesTest(unittest.TestCase):
    def test_fileset_emitter_site_module_is_not_shadowed_by_a_local(self) -> None:
        path = Path(__file__).resolve().parents[2] / 'translate/arm64e/xnu/prelink26/emit26.py'
        module = symtable.symtable(path.read_text(), str(path), 'exec')
        main = next(scope for scope in module.get_children() if scope.get_name() == 'main')
        self.assertTrue(main.lookup('abi_sites').is_global())

    def setUp(self) -> None:
        emit_prelink.NEW_ORIGINAL_COMPONENTS.add(KEXT)
        self.addCleanup(emit_prelink.NEW_ORIGINAL_COMPONENTS.discard, KEXT)

    def hashes(self, image: emit_prelink.CoalescedImage, sites: list[Site], inherited: dict[str, str] | None = None):
        report = {'original_executable_sha256': inherited or {}}
        original = emit_prelink.Original(APPLE, [], 0, None, report, sites)
        return emit_prelink.executable_hashes(image, cache(), no_native_sites(), original)

    def test_the_prelinked_code_differs_from_apples_at_exactly_the_declared_sites(self) -> None:
        image = copied()
        emit_prelink.write_sites(image, cache(), SITES, [], 0)
        changed = {at for at in range(0, len(APPLE), 4) if image.output[at : at + 4] != APPLE[at : at + 4]}
        self.assertEqual(changed, {4, 16})
        self.assertEqual(struct.unpack_from('<I', image.output, 4)[0], 0x7100029F)
        apple, patched = self.hashes(image, SITES)
        self.assertEqual(apple, {KEXT: hashlib.sha256(APPLE).hexdigest()})
        self.assertEqual(patched, {KEXT: hashlib.sha256(image.output[: len(APPLE)]).hexdigest()})

    def test_a_word_that_differs_fails_by_its_site_and_writes_nothing(self) -> None:
        image = copied()
        wrong = [SITES[0], Site(KEXT, TEXT_VA + 8, 0x52A00809, 0x52800009, 'mov')]
        with self.assertRaises(SystemExit) as raised:
            emit_prelink.write_sites(image, cache(), wrong, [], 0)
        self.assertIn(f'{TEXT_VA + 8:#x}', str(raised.exception))
        self.assertEqual(bytes(image.output[: len(APPLE)]), APPLE)

    def test_any_other_change_to_the_code_is_refused(self) -> None:
        image = copied()
        emit_prelink.write_sites(image, cache(), SITES, [], 0)
        image.output[20] ^= 1
        with self.assertRaises(ValueError):
            self.hashes(image, SITES)

    def test_a_site_left_unwritten_is_refused(self) -> None:
        with self.assertRaises(ValueError):
            self.hashes(copied(), SITES)

    def test_an_inherited_kext_with_sites_is_checked_against_its_inherited_hash(self) -> None:
        emit_prelink.NEW_ORIGINAL_COMPONENTS.discard(KEXT)
        image = copied()
        emit_prelink.write_sites(image, cache(), SITES, [], 0)
        apple, _ = self.hashes(image, SITES, {KEXT: hashlib.sha256(APPLE).hexdigest()})
        self.assertEqual(apple[KEXT], hashlib.sha256(APPLE).hexdigest())
        with self.assertRaises(ValueError):
            self.hashes(image, SITES, {KEXT: '0' * 64})

    def test_a_site_on_a_pc_relative_encoding_is_refused(self) -> None:
        branch = [Site(KEXT, TEXT_VA, 0xA9BF7BFD, 0x14000001, 'b')]
        with self.assertRaises(ValueError):
            emit_prelink.write_sites(copied(), cache(), branch, [], 0)

    def test_a_site_in_known_inline_data_is_refused(self) -> None:
        inline = [{'module': KEXT, 'va': TEXT_VA, 'size': 8}]
        with self.assertRaises(ValueError):
            emit_prelink.write_sites(copied(), cache(), SITES[:1], inline, 0)

    def test_a_site_outside_its_kexts_code_is_refused(self) -> None:
        outside = [Site(KEXT, TEXT_VA + len(APPLE), 0, 0xD503201F, 'past the end')]
        with self.assertRaises(ValueError):
            emit_prelink.write_sites(copied(), cache(), outside, [], 0)


if __name__ == '__main__':
    unittest.main()
