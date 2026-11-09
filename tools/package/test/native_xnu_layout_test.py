"""Host-only Darwin layout checks with independently encoded binary fixtures."""

import hashlib
import json
import shutil
import struct
import tempfile
import unittest
from collections.abc import Iterable, Sequence
from pathlib import Path

import native_xnu_layout as layout  # //tools/package:packaging

V = 0xFFFFFFF000001000
P = 0xA0001000
N = 0x10000000
# A kernel one 16 KiB page above its 2 MiB congruence with P: the room a small trust cache takes below it.
ROOM = 0x4000


def macho(
    *, base: int = V, entry: int | None = None, second: int = 0x5000, first_size: int = 0x3000, protection: int = 5
) -> bytes:
    def segment(name: bytes, address: int, size: int, file: tuple[int, int], prot: int) -> bytes:
        return struct.pack('<II16s4Q4I', 0x19, 72, name, address, size, *file, 7, prot, 0, 0)

    commands = segment(b'__TEXT', base, first_size, (0x200, 0x100), protection)
    commands += segment(b'__DATA', base + second, 0x2000, (0x400, 0x40), 3)
    thread = bytearray(288)
    struct.pack_into('<4I', thread, 0, 5, 288, 6, 68)
    struct.pack_into('<Q', thread, 272, base + 0x10 if entry is None else entry)
    commands += thread
    result = bytearray(0x440)
    result[:32] = struct.pack('<8I', 0xFEEDFACF, 0x100000C, 0, 2, 3, len(commands), 0, 0)
    result[32 : 32 + len(commands)] = commands
    result[0x200:0x300] = bytes(range(256))
    result[0x400:0x440] = bytes(range(64))
    return bytes(result)


def node(name: str, properties: Iterable[tuple[str, bytes]] = (), children: Sequence[bytes] = ()) -> bytes:
    properties = [('name', name.encode() + b'\0'), *properties]
    result = struct.pack('<II', len(properties), len(children))
    for key, value in properties:
        result += struct.pack('<32sI', key.encode(), len(value)) + value + bytes(-len(value) % 4)
    return result + b''.join(children)


def adt(
    *,
    window: int | None = N,
    ramdisk: bool = False,
    trustcache: bool = False,
    data_seed: bool = False,
    extra: Iterable[bytes] = (),
) -> bytes:
    placeholders = [
        (name, bytes(16))
        for name, present in (('RAMDisk', ramdisk), ('TrustCache', trustcache), ('NativeDataSeed', data_seed))
        if present
    ]
    chosen = [] if window is None else [('darwinboot-memory-size', struct.pack('<Q', window))]
    return node('device-tree', children=[node('chosen', chosen, [node('memory-map', placeholders)]), *extra])


def trustcache(hashes: Iterable[bytes] = (b'a' * 20,)) -> bytes:
    return struct.pack('<III16sI', 1, 8, 2, b'u' * 16, len(hashes)) + b''.join(h + b'\x02\0\0\0' for h in hashes)


def patched(data: bytes, offset: int, form: str, value: int) -> bytes:
    result = bytearray(data)
    struct.pack_into(form, result, offset, value)
    return bytes(result)


class NativeLayoutTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.kernel = self.root / 'kernel.macho'
        self.tree = self.root / 'kernel.adt'
        self.kernel.write_bytes(macho())
        self.tree.write_bytes(adt())
        self.output = self.root / 'layout'

    def build(self, **kwargs: object):
        args = {'physical_base': P, 'window_size': N, 'reserved_ranges': []}
        args.update(kwargs)
        return layout.build_layout(self.kernel, self.tree, self.output, layout.LayoutRequest(**args))

    def test_exact_abi_absolute_alignment_zero_fill_and_source_preservation(self):
        original = self.kernel.read_bytes(), self.tree.read_bytes()
        video = [0x80000000, 1, 2880, 720, 1280, 32]
        result = self.build(command_line='-v cpus=1', video=video)
        data = (self.output / 'kernel-window.bin').read_bytes()
        # P is 4K, not 16K aligned: the argument offset is 0x7000, not 0x8000.
        self.assertEqual(result['boot_args_pa'], P + 0x7000)
        self.assertEqual(result['adt_pa'], P + 0x8000)
        self.assertEqual(result['used_extent'], 0xB000)
        self.assertEqual(len(data), 0xB000)
        self.assertLess(len(data), N)
        self.assertEqual(data[:0x100], original[0][0x200:0x300])
        self.assertEqual(data[0x100:0x5000], bytes(0x4F00))  # BSS and segment gap.
        self.assertEqual(data[0x5000:0x5040], original[0][0x400:0x440])
        self.assertEqual(data[0x5040:0x7000], bytes(0x1FC0))
        args = data[0x7000:0x7480]
        self.assertEqual(len(args), 0x480)
        self.assertEqual(struct.unpack_from('<HHI4Q', args), (2, 2, 0, V, P, N, P + 0xB000))
        self.assertEqual(struct.unpack_from('<6Q', args, 0x28), tuple(video))
        self.assertEqual(args[0x58:0x60], bytes(8))  # machine type + padding.
        self.assertEqual(struct.unpack_from('<QI', args, 0x60), (V + 0x8000, len(original[1])))
        self.assertEqual(args[0x6C:0x46C], b'-v cpus=1' + bytes(1024 - 9))
        self.assertEqual(args[0x46C:0x478], bytes(12))  # padding and boot flags.
        self.assertEqual(struct.unpack_from('<Q', args, 0x478)[0], N)
        self.assertEqual(data[0x7480:0x8000], bytes(0xB80))
        self.assertEqual(data[0x8000 : 0x8000 + len(original[1])], original[1])
        self.assertEqual(data[0x8000 + len(original[1]) :], bytes(0x3000 - len(original[1])))
        self.assertEqual(result['entry_pa'], P + 0x10)
        self.assertEqual(result['adt_va'], V + 0x8000)
        self.assertFalse(result['native_window_ownership_verified'])
        self.assertEqual(result['block_rounded_extent'], {'start': 0xA0000000, 'end': 0xB0200000, 'size': N + 0x200000})
        self.assertEqual(result['image']['sha256'], hashlib.sha256(data).hexdigest())
        self.assertEqual(json.loads((self.output / 'manifest.json').read_text()), result)
        self.assertEqual((self.kernel.read_bytes(), self.tree.read_bytes()), original)

    def test_raw_sidecars_are_placed_padded_and_fix_only_placeholders(self):
        self.kernel.write_bytes(macho(base=V + ROOM))
        self.tree.write_bytes(adt(ramdisk=True, trustcache=True))
        ramdisk, cache = self.root / 'ramdisk', self.root / 'trustcache'
        ramdisk.write_bytes(b'ramdisk' * 1000)
        cache.write_bytes(trustcache())
        original = self.tree.read_bytes()
        result = self.build(ramdisk_path=ramdisk, trustcache_path=cache)
        data = (self.output / 'kernel-window.bin').read_bytes()
        ranges = {item['name']: item for item in result['ranges']}
        parsed = layout.parse_adt(original)
        expected = bytearray(original)
        for key, name in (('RAMDisk', 'ramdisk'), ('TrustCache', 'trustcache')):
            region = ranges[name]
            struct.pack_into(
                '<QQ', expected, parsed.properties['/chosen/memory-map', key][0], region['start'], region['size']
            )
            content = ramdisk.read_bytes() if name == 'ramdisk' else cache.read_bytes()
            self.assertEqual(data[region['image_offset'] : region['image_offset'] + len(content)], content)
            self.assertEqual(
                data[region['image_offset'] + len(content) : region['image_offset'] + region['size']],
                bytes(region['size'] - len(content)),
            )
        self.assertEqual(ranges['ramdisk']['size'], 8192)
        self.assertEqual(result['kernel_phys_base'], P + ROOM)
        self.assertEqual((ranges['trustcache']['start'], ranges['trustcache']['image_offset']), (P, 0))
        self.assertGreaterEqual(ranges['ramdisk']['start'], result['adt_pa'] + result['adt_size'])
        tree_offset = result['adt_pa'] - P
        self.assertEqual(data[tree_offset : tree_offset + len(original)], expected)
        self.assertEqual(self.tree.read_bytes(), original)

    def test_trust_cache_is_extradata_directly_below_the_kernel(self):
        # The native window: its 2 MiB block, then the kernel at its VA's offset in it (0x7c000).
        window, base = 0xB2000000, 0xFFFFFFF00427C000
        self.kernel.write_bytes(macho(base=base))
        self.tree.write_bytes(adt(trustcache=True))
        cache = self.root / 'trustcache'
        cache.write_bytes(trustcache([bytes([i]) * 20 for i in range(1, 200)]))
        bundle = cache.read_bytes()
        result = self.build(physical_base=window, trustcache_path=cache)
        data = (self.output / 'kernel-window.bin').read_bytes()
        kernel = 0xB207C000
        start = kernel - 0x4000 * -(-len(bundle) // 0x4000)
        self.assertEqual(result['kernel_phys_base'], kernel)
        self.assertEqual(result['entry_pa'], kernel + 0x10)
        self.assertEqual(data[: start - window], bytes(start - window))
        self.assertEqual(data[start - window : start - window + len(bundle)], bundle)
        self.assertEqual(data[start - window + len(bundle) : kernel - window], bytes(kernel - start - len(bundle)))
        self.assertEqual(data[kernel - window : kernel - window + 0x100], macho()[0x200:0x300])
        tree = data[result['adt_pa'] - window : result['adt_pa'] - window + result['adt_size']]
        offset = layout.parse_adt(self.tree.read_bytes()).properties['/chosen/memory-map', 'TrustCache'][0]
        self.assertEqual(struct.unpack_from('<QQ', tree, offset), (start, len(bundle)))
        # XNU's phystokv of the trust caches must land below the kernel's lowest segment: boot_args map the kernel's VA
        # at its physical base, and physBase (the window's, here) lies below the trust caches.
        args = data[result['boot_args_pa'] - window :]
        virt, phys = struct.unpack_from('<QQ', args, 8)
        self.assertEqual((virt, phys), (base - kernel + window, window))
        self.assertLess(start - phys + virt, base)
        self.assertEqual(struct.unpack_from('<Q', args, 0x60)[0], base + result['adt_pa'] - kernel)

    def test_a_trust_cache_larger_than_the_room_moves_the_kernel_a_block_up(self):
        self.kernel.write_bytes(macho(base=V + ROOM))
        self.tree.write_bytes(adt(trustcache=True))
        cache = self.root / 'trustcache'
        cache.write_bytes(trustcache([i.to_bytes(20, 'big') for i in range(1, 700)]))
        self.assertGreater(len(cache.read_bytes()), ROOM)
        result = self.build(trustcache_path=cache)
        ranges = {r['name']: r for r in result['ranges']}
        self.assertEqual(result['kernel_phys_base'], P + ROOM + 0x200000)
        self.assertEqual(ranges['trustcache']['start'], result['kernel_phys_base'] - 0x8000)
        self.assertEqual(result['entry_pa'], P + ROOM + 0x200000 + 0x10)

    def test_full_ram_refuses_trust_caches_that_would_move_the_kernel(self):
        self.kernel.write_bytes(macho(base=0xFFFFFFF00427C000))
        chosen = [('darwinboot-memory-size', struct.pack('<Q', 4 << 30)), ('switch-ram-ledger', bytes(1584))]
        self.tree.write_bytes(
            node('device-tree', children=[node('chosen', chosen, [node('memory-map', [('TrustCache', bytes(16))])])])
        )
        cache = self.root / 'trustcache'
        request = {'physical_base': 0xB2000000, 'window_size': 770 << 20, 'full_ram': True, 'trustcache_path': cache}
        cache.write_bytes(trustcache([i.to_bytes(20, 'big') for i in range(1, 0x7C000 // 24 + 1)]))
        self.assertGreater(len(cache.read_bytes()), 0x7C000)
        with self.assertRaisesRegex(layout.LayoutError, 'trust caches must fit the 0x7c000 bytes below it'):
            self.build(**request)
        self.assertFalse(self.output.exists())
        cache.write_bytes(trustcache([i.to_bytes(20, 'big') for i in range(1, 0x7C000 // 24)]))
        self.assertLessEqual(len(cache.read_bytes()), 0x7C000)
        result = self.build(**request)
        self.assertEqual((result['kernel_phys_base'], result['boot_phys_base']), (0xB207C000, 0x80000000))
        self.assertEqual(result['boot_virt_base'], 0xFFFFFFF00427C000 - 0xB207C000 + 0x80000000)

    def test_a_window_off_the_kernels_2_mib_offset_puts_the_kernel_at_its_next_one(self):
        result = self.build(physical_base=P + 0x1000)
        data = (self.output / 'kernel-window.bin').read_bytes()
        self.assertEqual(result['kernel_phys_base'], P + 0x200000)
        self.assertEqual(result['phys_base'], P + 0x1000)
        self.assertEqual(data[: 0x200000 - 0x1000], bytes(0x200000 - 0x1000))
        self.assertEqual(data[0x200000 - 0x1000 : 0x200000 - 0x1000 + 0x100], macho()[0x200:0x300])

    def test_sparse_data_seed_reserves_only_encoded_bytes_and_preserves_exact_adt_length(self):
        # A 1 GiB logical disk with two distant nonzero pages must not expand.
        encoded = (
            struct.pack('<8sIIQQ', b'DBRDISK1', 1, 4096, 1 << 30, 2)
            + struct.pack('<4Q', 0, 4096, (1 << 30) - 4096, 4096)
            + b'a' * 4096
            + b'z' * 4096
        )
        seed, cache = self.root / 'seed.dbr', self.root / 'trustcache'
        seed.write_bytes(encoded)
        cache.write_bytes(trustcache())
        self.kernel.write_bytes(macho(base=V + ROOM))
        self.tree.write_bytes(adt(data_seed=True, trustcache=True))
        original = self.tree.read_bytes()
        result = self.build(data_seed_path=seed, trustcache_path=cache)
        payload = (self.output / 'kernel-window.bin').read_bytes()
        ranges = {r['name']: r for r in result['ranges']}
        region = ranges['native_data_seed']
        self.assertNotIn('ramdisk', ranges)
        self.assertEqual(region['start'] % 4096, 0)
        self.assertGreaterEqual(region['start'], result['adt_pa'] + result['adt_size'])
        self.assertLessEqual(ranges['trustcache']['end'], result['kernel_phys_base'])
        self.assertEqual(region['size'], 12288)
        self.assertEqual(payload[region['image_offset'] : region['image_offset'] + len(encoded)], encoded)
        self.assertEqual(
            payload[region['image_offset'] + len(encoded) : region['image_offset'] + region['size']], bytes(4032)
        )
        offset = layout.parse_adt(original).properties['/chosen/memory-map', 'NativeDataSeed'][0]
        self.assertEqual(
            struct.unpack_from('<QQ', payload, result['adt_pa'] - P + offset), (region['start'], len(encoded))
        )
        self.assertLessEqual(region['end'], result['top_of_kernel_data'])
        self.assertLess(len(payload), 0x20000)
        self.assertEqual(seed.read_bytes(), encoded)
        self.assertEqual(result['sources']['NativeDataSeed']['sha256'], hashlib.sha256(encoded).hexdigest())

    def test_sparse_data_seed_rejects_malformed_geometry_before_output(self):
        valid = (
            struct.pack('<8sIIQQ', b'DBRDISK1', 1, 4096, 8192, 2)
            + struct.pack('<4Q', 0, 4096, 4096, 4096)
            + bytes(8192)
        )
        for data in (
            valid[:31],
            patched(valid, 8, '<I', 2),
            patched(valid, 12, '<I', 512),
            patched(valid, 16, '<Q', (1 << 30) + 4096),
            patched(valid, 16, '<Q', 8193),
            patched(valid, 24, '<Q', 65537),
            patched(valid, 32, '<Q', 1),
            patched(valid, 40, '<Q', 0),
            patched(valid, 48, '<Q', 0),
            patched(valid, 48, '<Q', 8192),
            valid[:-1],
            valid + b'x',
        ):
            with self.subTest(length=len(data)), self.assertRaises(layout.LayoutError):
                layout.validate_data_seed(data)
        seed = self.root / 'seed.dbr'
        seed.write_bytes(valid)
        with self.assertRaisesRegex(layout.LayoutError, 'placeholder'):
            self.build(data_seed_path=seed)
        self.tree.write_bytes(adt(data_seed=True))
        with self.assertRaisesRegex(layout.LayoutError, 'placeholder'):
            self.build()
        seed.write_bytes(valid[:-1])
        with self.assertRaises(layout.LayoutError):
            self.build(data_seed_path=seed)
        self.assertFalse(self.output.exists())

    def test_rejects_malformed_macho_contracts(self):
        data = macho()
        cases = {
            'header': patched(data, 0, '<I', 0xCAFEBABE),
            'cpu': patched(data, 4, '<I', 12),
            'subtype': patched(data, 8, '<I', 2),
            'kind': patched(data, 12, '<I', 6),
            'command table': patched(data, 20, '<I', len(data)),
            'command count': patched(data, 16, '<I', 4),
            'command length': patched(data, 36, '<I', 73),
            'sections': patched(data, 32 + 64, '<I', 1),
            'file overrun': patched(data, 32 + 40, '<Q', len(data)),
            'backed beyond memory': patched(data, 32 + 48, '<Q', 0x3001),
            'overlap': macho(second=0x2000),
            'entry bss': macho(entry=V + 0x100),
            'entry alignment': macho(entry=V + 1),
            'entry not executable': macho(protection=3),
            'VA alignment': macho(base=V + 1),
            'VA span bound': macho(second=0xFE00000),
            'thread flavor': patched(data, 32 + 144 + 8, '<I', 1),
            'thread count': patched(data, 32 + 144 + 12, '<I', 67),
            'truncation': data[:20],
        }
        for label, value in cases.items():
            with self.subTest(label=label), self.assertRaises(layout.LayoutError):
                layout.parse_kernel(value)

    def test_rejects_adt_bounds_names_duplicates_and_wrong_placeholder_path(self):
        original = adt()
        wrong_names = node('chosen', [('name', b'chosen\0')])
        malformed = [
            original[:-1],
            original + b'\0',
            bytes(8),
            node('root', children=[wrong_names]),
            node('root', children=[node('chosen'), node('chosen')]),
            node('root', [('darwinboot-memory-size', struct.pack('<Q', N))], [node('chosen')]),
            node('root', children=[node('chosen', [('RAMDisk', bytes(16))])]),
            node('root', children=[node('chosen', children=[node('memory-map', [('RAMDisk', bytes(15))])])]),
            node('root', children=[node('chosen', children=[node('memory-map', [('RAMDisk', b'x' + bytes(15))])])]),
            node('root', children=[node('chosen', children=[node('memory-map', [('PackedRAMDisk', bytes(16))])])]),
            node('root', children=[node('chosen', [('darwinboot-memory-size', struct.pack('<Q', 123))])]),
        ]
        unterminated = bytearray(original)
        unterminated[8:40] = b'n' * 32
        malformed.append(unterminated)
        for value in malformed:
            with self.subTest(length=len(value)), self.assertRaises(layout.LayoutError):
                layout.parse_adt(value)

    def test_missing_or_extra_sidecars_and_packed_input_are_rejected(self):
        raw = self.root / 'ramdisk'
        raw.write_bytes(b'raw ramdisk')
        cache = self.root / 'trustcache'
        cache.write_bytes(trustcache())
        for tree, kwargs in (
            (adt(ramdisk=True), {}),
            (adt(trustcache=True), {}),
            (adt(), {'ramdisk_path': raw}),
            (adt(), {'trustcache_path': cache}),
        ):
            self.tree.write_bytes(tree)
            with self.subTest(kwargs=kwargs), self.assertRaises(layout.LayoutError):
                self.build(**kwargs)
            self.assertFalse(self.output.exists())
        self.tree.write_bytes(adt(ramdisk=True))
        raw.write_bytes(b'DBRDISK1' + bytes(32))
        with self.assertRaisesRegex(layout.LayoutError, 'packed ramdisk'):
            self.build(ramdisk_path=raw)

    def test_invalid_trustcaches_are_rejected(self):
        valid = trustcache()
        layout.validate_trustcache(valid)
        for value in (
            valid[:7],
            patched(valid, 0, '<I', 0),
            patched(valid, 4, '<I', 9),
            patched(valid, 8, '<I', 1),
            valid[:-1],
            patched(valid, 52, '<B', 0),
            trustcache((b'b' * 20, b'a' * 20)),
            trustcache((b'a' * 20, b'a' * 20)),
        ):
            with self.subTest(length=len(value)), self.assertRaises(layout.LayoutError):
                layout.validate_trustcache(value)

    def test_adt_depth_and_truncated_property_padding_are_rejected(self):
        nested = node('leaf')
        for _ in range(65):
            nested = node('branch', children=[nested])
        with self.assertRaisesRegex(layout.LayoutError, 'nesting'):
            layout.parse_adt(node('root', children=[node('chosen'), nested]))
        # An empty final child name has one value byte and three pad bytes.
        with self.assertRaisesRegex(layout.LayoutError, 'truncated'):
            layout.parse_adt(node('root', children=[node('chosen'), node('')])[:-1])

    def test_minimum_kernel_slack_is_checked_before_output_allocation(self):
        self.kernel.write_bytes(macho(second=N - 0x200000 - 0x2000))
        parsed = layout.parse_kernel(self.kernel.read_bytes())
        self.assertEqual(parsed.end - parsed.base, N - 0x200000)
        with self.assertRaisesRegex(layout.LayoutError, 'slack'):
            self.build()
        self.assertFalse(self.output.exists())

    def test_full_rounded_ownership_envelope_rejects_prefix_suffix_and_free_heap_conflicts(self):
        for name, start, size in (('prefix', P - 1, 1), ('suffix', P + N, 1), ('future heap', P + 0x8000000, 1)):
            with self.subTest(name=name), self.assertRaisesRegex(layout.LayoutError, 'overlaps'):
                self.build(reserved_ranges=[{'name': name, 'start': start, 'size': size}])
        result = self.build(
            reserved_ranges=[
                {'name': 'before', 'start': 0x9FFFF000, 'size': 0x1000},
                {'name': 'after', 'start': 0xB0200000, 'size': 0x1000},
            ]
        )
        self.assertEqual(len(result['reserved_ranges']), 2)

    def test_invalid_windows_and_ranges(self):
        for kwargs in (
            {'window_size': N - 0x200000},
            {'window_size': N + 1},
            {'window_size': 0x200200000},
            {'window_size': N + 0x200000},
            {'physical_base': (1 << 64) - N},
            {'reserved_ranges': None},
            {'reserved_ranges': [{'name': 'bad', 'start': 0, 'size': 0}]},
            {'reserved_ranges': [{'name': 'bad', 'start': -1, 'size': 1}]},
            {'reserved_ranges': [{'name': 'bad', 'start': (1 << 64) - 1, 'size': 1}]},
        ):
            with self.subTest(kwargs=kwargs), self.assertRaises(layout.LayoutError):
                self.build(**kwargs)
        self.assertFalse(self.output.exists())

    def test_virtual_window_overflow_rejected_even_when_kernel_parser_accepts_base(self):
        base = (1 << 64) - 0x30000000 + 0x1000
        self.kernel.write_bytes(macho(base=base))
        self.tree.write_bytes(adt(window=0x30000000))
        layout.parse_kernel(self.kernel.read_bytes())
        with self.assertRaisesRegex(layout.LayoutError, 'virtual window'):
            self.build(window_size=0x30000000)

    def test_block_round_overflow_rejected_before_payload_reads(self):
        # Raw range fits, but rounding its last nonaligned block does not.
        with self.assertRaisesRegex(layout.LayoutError, 'block-rounded'):
            self.build(physical_base=(1 << 64) - N - 0x1000)

    def test_default_adt_window_is_256_mib(self):
        self.tree.write_bytes(adt(window=None))
        self.assertEqual(self.build()['owned_window_size'], N)

    def test_native_rotated_profile_in_text_or_graphics_mode(self):
        for display in (0, 1):
            video = [0xF5A00000, display, 2880, 720, 1280, 0x320]
            with self.subTest(display=display):
                self.assertEqual(self.build(video=video)['video'], video)
            shutil.rmtree(self.output)
        with self.assertRaises(layout.LayoutError):
            self.build(video=[0xF5A00000, 2, 2880, 720, 1280, 0x320])

    def test_bad_command_video_and_source_type_leave_no_output(self):
        for kwargs in (
            {'command_line': 'x' * 1024},
            {'command_line': 'nonasciié'},
            {'command_line': 'a\0b'},
            {'video': [0]},
            {'video': [0x80000000, 1, 1, 720, 1280, 32]},
            {'video': [0x90000001, 1, 4, 1, 1, 32]},
            {'video': [P, 1, 4, 1, 1, 32]},
            {'video': [0] * 5 + [-1]},
        ):
            with self.subTest(kwargs=kwargs), self.assertRaises(layout.LayoutError):
                self.build(**kwargs)
            self.assertFalse(self.output.exists())
        self.tree.unlink()
        self.tree.symlink_to(self.kernel)
        with self.assertRaisesRegex(layout.LayoutError, 'regular file'):
            self.build()

    def test_existing_output_is_never_modified(self):
        self.output.mkdir()
        keep = self.output / 'keep'
        keep.write_text('preserved')
        with self.assertRaisesRegex(layout.LayoutError, 'already exists'):
            self.build()
        self.assertEqual(list(self.output.iterdir()), [keep])
        self.assertEqual(keep.read_text(), 'preserved')


if __name__ == '__main__':
    unittest.main()
