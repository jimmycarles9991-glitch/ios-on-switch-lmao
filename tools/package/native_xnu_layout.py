#!/usr/bin/env python3
"""Build a host-only fixed XNU layout from the pinned DarwinBoot contract.

DarwinBoot was UniXNU's EDK2 XNU loader (github.com/UniXNU/DarwinBoot); our copy, whose ParseKernel this mirrors, is
in git history: `git show 27f2155:drivers/DarwinBoot/DarwinBootPkg/Driver/DarwinBoot.c`.

This writes a new local directory, never a boot entry or storage device. An
explicit window and reservation list are inputs, not proof of native ownership.
Packed root ramdisks, relocations and virtual slides are deliberately unsupported.
NativeDataSeed is a separate bounded sparse input for a writable RAM provider;
it is never installed as the md0 root device by this tool.
"""

from __future__ import annotations

import argparse
import hashlib
import itertools
import json
import shutil
import stat
import struct
import sys
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import NamedTuple

U64_MAX = (1 << 64) - 1
PAGE = 0x1000
BLOCK = 0x200000
MIN_WINDOW = 0x10000000
MAX_WINDOW = 0x200000000
BOOT_ARGS_SIZE = 0x480
XNU_PAGE = 0x4000


class LayoutError(ValueError):
    """An input does not satisfy the bounded host layout contract."""


def align(value: int, boundary: int) -> int:
    return (value + boundary - 1) & -boundary


def uint64(value: int, label: str) -> int:
    if type(value) is not int or not 0 <= value <= U64_MAX:
        raise LayoutError(f'{label} must be a uint64')
    return value


def bounded_end(start: int, size: int, label: str) -> int:
    uint64(start, label + ' start')
    uint64(size, label + ' size')
    if size == 0 or start > U64_MAX - size:
        raise LayoutError(f'{label} is empty or overflows uint64')
    return start + size


def valid_window(size: int) -> None:
    uint64(size, 'window size')
    if not MIN_WINDOW <= size <= MAX_WINDOW or size % BLOCK:
        raise LayoutError('window must be 256 MiB..8 GiB in 2 MiB steps')


@dataclass(frozen=True)
class Segment:
    name: str
    vmaddr: int
    vmsize: int
    fileoff: int
    filesize: int
    protection: int


@dataclass(frozen=True)
class Kernel:
    segments: tuple[Segment, ...]
    base: int
    end: int
    entry: int


def _segment(data: bytes, cursor: int, length: int, count: int) -> Segment | None:
    if length < 72 or count == 64:
        raise LayoutError('invalid or excessive segment commands')
    sections = struct.unpack_from('<I', data, cursor + 64)[0]
    if sections > (length - 72) // 80:
        raise LayoutError('segment sections exceed command')
    address, extent, offset, backed = struct.unpack_from('<4Q', data, cursor + 24)
    protection = struct.unpack_from('<I', data, cursor + 60)[0]
    if not extent:
        return None
    bounded_end(address, extent, 'virtual segment')
    if extent > MIN_WINDOW or backed > extent or offset > len(data) or backed > len(data) - offset:
        raise LayoutError('invalid segment memory/file extent')
    raw_name = data[cursor + 8 : cursor + 24].split(b'\0', 1)[0]
    return Segment(raw_name.decode('ascii', errors='backslashreplace'), address, extent, offset, backed, protection)


def _entry(data: bytes, cursor: int, length: int, entry: int | None) -> int:
    if entry is not None or length != 288 or struct.unpack_from('<II', data, cursor + 8) != (6, 68):
        raise LayoutError('expected exactly one ARM64 LC_UNIXTHREAD state')
    return struct.unpack_from('<Q', data, cursor + 272)[0]


def _load_commands(data: bytes, count: int, end: int) -> tuple[list[Segment], int]:
    cursor, entry = 32, None
    segments = []
    for _ in range(count):
        if end - cursor < 8:
            raise LayoutError('truncated load command')
        command, length = struct.unpack_from('<II', data, cursor)
        if length < 8 or length % 8 or length > end - cursor:
            raise LayoutError('invalid load-command length')
        if command == 0x19:
            segment = _segment(data, cursor, length, len(segments))
            if segment is not None:
                segments.append(segment)
        elif command == 5:
            entry = _entry(data, cursor, length, entry)
        cursor += length
    if cursor != end or entry is None or not segments:
        raise LayoutError('incomplete load-command table, segments or entry')
    return segments, entry


def parse_kernel(data: bytes) -> Kernel:
    """Mirror DarwinBoot ParseKernel, including its 256 MiB parser bound."""
    if not 32 <= len(data) <= 128 << 20:
        raise LayoutError('kernel must be 32 bytes..128 MiB')
    magic, cpu, subtype, kind, count, size, _, _ = struct.unpack_from('<8I', data)
    if (magic, cpu, subtype, kind) != (0xFEEDFACF, 0x100000C, 0, 2):
        raise LayoutError('expected little-endian ARM64 subtype-0 MH_EXECUTE')
    if size > len(data) - 32:
        raise LayoutError('load-command table exceeds kernel file')
    segments, entry = _load_commands(data, count, 32 + size)
    ordered = sorted(segments, key=lambda item: item.vmaddr)
    if any(a.vmaddr + a.vmsize > b.vmaddr for a, b in itertools.pairwise(ordered)):
        raise LayoutError('overlapping virtual segments')
    base, last = ordered[0].vmaddr, ordered[-1].vmaddr + ordered[-1].vmsize
    executable = any(s.vmaddr <= entry < s.vmaddr + s.filesize and s.protection & 4 for s in segments)
    if not executable or entry % 4 or base % PAGE or base > U64_MAX - MIN_WINDOW or last - base > MIN_WINDOW - BLOCK:
        raise LayoutError('invalid kernel base, span or executable file-backed entry')
    return Kernel(tuple(segments), base, last, entry)


@dataclass(frozen=True)
class Adt:
    properties: dict[tuple[str, str], tuple[int, int]]
    window_size: int


def _node_name(props: dict[str, tuple[int, int, bytes]], depth: int) -> str:
    if 'name' not in props:
        raise LayoutError('ADT node has no name')
    raw_name = props['name'][2]
    if not raw_name.endswith(b'\0') or b'\0' in raw_name[:-1]:
        raise LayoutError('unterminated or embedded-NUL ADT node name')
    try:
        name = raw_name[:-1].decode('ascii')
    except UnicodeError as exc:
        raise LayoutError('non-ASCII ADT node name') from exc
    if depth and (not name or '/' in name):
        raise LayoutError('invalid ADT node name')
    return name


class _AdtReader:
    def __init__(self, data: bytes) -> None:
        self.data = data
        self.cursor = 0
        self.offsets = {}

    def read(self, size: int) -> bytes:
        if size > len(self.data) - self.cursor:
            raise LayoutError('truncated ADT')
        value = self.data[self.cursor : self.cursor + size]
        self.cursor += size
        return value

    def properties(self, count: int) -> dict[str, tuple[int, int, bytes]]:
        props = {}
        for _ in range(count):
            raw, length = struct.unpack('<32sI', self.read(36))
            if b'\0' not in raw:
                raise LayoutError('unterminated ADT property name')
            try:
                key = raw.split(b'\0', 1)[0].decode('ascii')
            except UnicodeError as exc:
                raise LayoutError('non-ASCII ADT property name') from exc
            if not key or key in props:
                raise LayoutError('empty or duplicate ADT property')
            start = self.cursor
            value = self.read(length)
            self.read(-length % 4)
            props[key] = (start, length, value)
        return props

    def node(self, depth: int, parent: str) -> str:
        if depth > 64:
            raise LayoutError('ADT nesting exceeds 64 levels')
        count, children = struct.unpack('<II', self.read(8))
        if count > (len(self.data) - self.cursor) // 36:
            raise LayoutError('ADT property count exceeds input')
        props = self.properties(count)
        name = _node_name(props, depth)
        path = '/' if depth == 0 else parent.rstrip('/') + '/' + name
        for key, (start, length, _) in props.items():
            self.offsets[path, key] = (start, length)
        if children > (len(self.data) - self.cursor) // 8:
            raise LayoutError('ADT child count exceeds input')
        names = set()
        for _ in range(children):
            child_name = self.node(depth + 1, path)
            if child_name in names:
                raise LayoutError('duplicate ADT sibling node')
            names.add(child_name)
        return name


def _adt_window(data: bytes, offsets: dict[tuple[str, str], tuple[int, int]]) -> int:
    window = MIN_WINDOW
    for (path, key), (offset, length) in offsets.items():
        if key == 'darwinboot-memory-size':
            if path != '/chosen' or length != 8:
                raise LayoutError('invalid ADT window property')
            window = struct.unpack_from('<Q', data, offset)[0]
            valid_window(window)
        if key in ('RAMDisk', 'PackedRAMDisk', 'TrustCache', 'NativeDataSeed'):
            if path != '/chosen/memory-map' or length != 16 or data[offset : offset + 16] != bytes(16):
                raise LayoutError('sidecars require zero 16-byte /chosen/memory-map placeholders')
            if key == 'PackedRAMDisk':
                raise LayoutError('PackedRAMDisk is unsupported; supply an uncompressed RAMDisk')
    return window


def parse_adt(data: bytes) -> Adt:
    """Preserve byte offsets, with stricter names than the exporter ADT helper.

    A local reader is needed for in-place sidecar fixups. It also rejects
    unterminated names and duplicate sibling nodes instead of silently picking
    one. All accepted bytes remain unchanged apart from the zero placeholders.
    """
    if not 8 <= len(data) <= 16 << 20:
        raise LayoutError('ADT must be 8 bytes..16 MiB')
    reader = _AdtReader(data)
    reader.node(0, '')
    if reader.cursor != len(data):
        raise LayoutError('trailing bytes after ADT')
    if ('/chosen', 'name') not in reader.offsets:
        raise LayoutError('ADT requires /chosen')
    return Adt(reader.offsets, _adt_window(data, reader.offsets))


def validate_trustcache(data: bytes) -> None:
    if len(data) < 8:
        raise LayoutError('truncated trustcache table')
    count = struct.unpack_from('<I', data)[0]
    if not 1 <= count <= 128 or count > (len(data) - 4) // 4:
        raise LayoutError('invalid trustcache count')
    starts = struct.unpack_from(f'<{count}I', data, 4)
    for start, end in zip(starts, (*starts[1:], len(data)), strict=True):
        if start < 4 + count * 4 or start % 4 or end > len(data) or end - start < 24:
            raise LayoutError('invalid trustcache range')
        if struct.unpack_from('<I', data, start)[0] != 2:
            raise LayoutError('trustcache must use version 2')
        entries = struct.unpack_from('<I', data, start + 20)[0]
        if end - start != 24 + entries * 24:
            raise LayoutError('invalid trustcache entries')
        previous = None
        for at in range(start + 24, end, 24):
            digest = data[at : at + 20]
            if not 1 <= data[at + 20] <= 4 or (previous is not None and previous >= digest):
                raise LayoutError('invalid hash type or unsorted trustcache')
            previous = digest


def validate_data_seed(data: bytes) -> None:
    """Check DBRDISK1 geometry without allocating its logical disk size.

    This is format validation, not APFS or seed authentication. The native
    consumer must independently pin the input hash, logical size and UUID.
    """
    if not 32 <= len(data) <= 16 << 20:
        raise LayoutError('NativeDataSeed must be 32 bytes..16 MiB')
    magic, version, block, logical, count = struct.unpack_from('<8sIIQQ', data)
    if (magic, version, block) != (b'DBRDISK1', 1, PAGE):
        raise LayoutError('NativeDataSeed requires DBRDISK1 version 1 with 4 KiB pages')
    if not 0 < logical <= 1 << 30 or logical % PAGE or count > 65536 or count > (len(data) - 32) // 16:
        raise LayoutError('NativeDataSeed logical size or extent count is invalid')
    cursor, previous_end = 32 + count * 16, 0
    for index in range(count):
        at, size = struct.unpack_from('<QQ', data, 32 + index * 16)
        if (
            not size
            or (at | size) % PAGE
            or at < previous_end
            or at > logical
            or size > logical - at
            or size > len(data) - cursor
        ):
            raise LayoutError('NativeDataSeed extent is unaligned, overlapping or out of bounds')
        previous_end = at + size
        cursor += size
    if cursor != len(data):
        raise LayoutError('NativeDataSeed has trailing bytes')


def read_input(path: Path, limit: int) -> bytes:
    try:
        info = path.lstat()
        if not stat.S_ISREG(info.st_mode) or not 0 < info.st_size <= limit:
            raise LayoutError(f'input must be a nonempty regular file <= {limit} bytes: {path}')
        with path.open('rb') as stream:
            data = stream.read(limit + 1)
        if len(data) != info.st_size:
            raise LayoutError(f'input size changed while reading: {path}')
        return data
    except OSError as exc:
        raise LayoutError(f'cannot read input {path}: {exc}') from exc


def source_record(path: Path, data: bytes) -> dict:
    return {'path': str(path.resolve()), 'size_bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()}


@dataclass(frozen=True)
class LayoutRequest:
    """physical_base is where the window starts: the trust caches, then the kernel at the next address its VA allows."""

    physical_base: int
    window_size: int
    reserved_ranges: list[dict]
    command_line: str = ''
    ramdisk_path: Path | None = None
    trustcache_path: Path | None = None
    data_seed_path: Path | None = None
    video: list[int] | None = None
    full_ram: bool = False


class _Window(NamedTuple):
    end: int
    mapped_start: int
    mapped_end: int


class _Addresses(NamedTuple):
    kernel_pa: int
    args_pa: int
    tree_pa: int
    ramdisk_pa: int
    ramdisk_size: int
    trustcache_pa: int
    data_seed_pa: int
    data_seed_size: int
    data_seed_span: int
    used_end: int


class _BootMemory(NamedTuple):
    phys: int
    virt: int
    size: int
    actual: int


def _window(request: LayoutRequest) -> _Window:
    valid_window(request.window_size)
    window_end = bounded_end(request.physical_base, request.window_size, 'physical window')
    mapped_start, mapped_end = request.physical_base & -BLOCK, align(window_end, BLOCK)
    if mapped_end > U64_MAX:
        raise LayoutError('block-rounded window overflows uint64')
    return _Window(window_end, mapped_start, mapped_end)


def _reservations(reserved_ranges: list[dict], window: _Window) -> list[dict]:
    if not isinstance(reserved_ranges, list):
        raise LayoutError('reserved_ranges must be an explicit list')
    reservations = []
    seen = set()
    for item in reserved_ranges:
        if not isinstance(item, dict) or set(item) != {'name', 'start', 'size'}:
            raise LayoutError('reservation requires exactly name, start and size')
        name = item['name']
        if not isinstance(name, str) or not name or name in seen:
            raise LayoutError('reservation names must be nonempty and unique')
        seen.add(name)
        end = bounded_end(item['start'], item['size'], 'reservation ' + name)
        if item['start'] < window.mapped_end and window.mapped_start < end:
            raise LayoutError(f'block-rounded window overlaps reservation {name}')
        reservations.append(dict(item, end=end))
    return reservations


def kernel_phys_base(window_base: int, kernel_base: int, trustcache_bytes: int) -> int:
    """Return where the kernel's lowest segment goes: above the trust caches, at its VA's offset modulo 2 MiB.

    The trust caches' 16 KiB pages end where the kernel starts: XNU's arm_vm_init makes them EXTRADATA, the start of
    the kernelcache, and a RELEASE kernel panics on a TrustCache above its lowest segment. Without KTRR or CTRR the
    device tree is not EXTRADATA and stays after the kernel, where arm_vm_physmap_init slides it.
    """
    floor = window_base + align(trustcache_bytes, XNU_PAGE)
    return floor + (kernel_base - floor) % BLOCK


def _check_bases(request: LayoutRequest, kernel: Kernel, adt: Adt) -> None:
    if kernel.base > U64_MAX - request.window_size:
        raise LayoutError('virtual window overflows uint64')
    if adt.window_size != ((4 << 30) if request.full_ram else request.window_size):
        raise LayoutError('explicit window differs from ADT darwinboot-memory-size (default 256 MiB)')


def _command(command_line: str) -> bytes:
    try:
        command = command_line.encode('ascii')
    except (AttributeError, UnicodeError) as exc:
        raise LayoutError('command line must be ASCII text') from exc
    if len(command) >= 1024 or b'\0' in command:
        raise LayoutError('command line must be at most 1023 bytes with no NUL')
    return command


def _video(video: list[int] | None, window: _Window) -> tuple[list[int], dict | None]:
    video = [0] * 6 if video is None else list(video)
    if len(video) != 6:
        raise LayoutError('video must contain six uint64 fields')
    for field in video:
        uint64(field, 'video field')
    if not any(video):
        return video, None
    base, display, row, width, height, depth = video
    native = video[:1] + video[2:] == [0xF5A00000, 2880, 720, 1280, 0x320] and display in (0, 1)
    if (
        not base
        or base % 4
        or (not native and (display != 1 or depth != 32))
        or not width
        or not height
        or row < width * 4
        or row % 4
    ):
        raise LayoutError('video requires 32bpp graphics or the native rotated profile, with valid width/height/stride')
    end = bounded_end(base, row * height, 'framebuffer')
    if base < window.mapped_end and window.mapped_start < end:
        raise LayoutError('block-rounded window overlaps framebuffer')
    return video, {'name': 'framebuffer', 'start': base, 'size': row * height, 'end': end}


def _sidecars(request: LayoutRequest, adt: Adt, sources: dict) -> dict[str, bytes]:
    sidecars = {}
    for key, given, limit in (
        ('RAMDisk', request.ramdisk_path, 256 << 20),
        ('TrustCache', request.trustcache_path, 16 << 20),
        ('NativeDataSeed', request.data_seed_path, 16 << 20),
    ):
        placeholder = ('/chosen/memory-map', key) in adt.properties
        if (given is not None) != placeholder:
            raise LayoutError(f'{key} input and ADT placeholder must both be present or absent')
        if given is not None:
            path = Path(given)
            data = read_input(path, limit)
            if key == 'RAMDisk' and data[:8] == b'DBRDISK1':
                raise LayoutError('packed ramdisk input is unsupported')
            if key == 'TrustCache':
                validate_trustcache(data)
            if key == 'NativeDataSeed':
                validate_data_seed(data)
            sidecars[key] = data
            sources[key] = source_record(path, data)
    return sidecars


def _addresses(request: LayoutRequest, kernel: Kernel, tree_size: int, sidecars: dict[str, bytes]) -> _Addresses:
    trustcache_size = len(sidecars.get('TrustCache', b''))
    kernel_pa = kernel_phys_base(request.physical_base, kernel.base, trustcache_size)
    trustcache_pa = kernel_pa - align(trustcache_size, XNU_PAGE)
    args_pa = align(kernel_pa + kernel.end - kernel.base, XNU_PAGE)
    tree_pa = args_pa + PAGE
    ramdisk_pa = align(tree_pa + tree_size, PAGE)
    ramdisk_size = align(len(sidecars.get('RAMDisk', b'')), PAGE)
    ramdisk_end = ramdisk_pa + ramdisk_size
    data_seed_pa = ramdisk_end
    data_seed_size = len(sidecars.get('NativeDataSeed', b''))
    data_seed_span = align(data_seed_size, PAGE)
    used_end = align(data_seed_pa + data_seed_span if data_seed_size else ramdisk_end, XNU_PAGE)
    if used_end - request.physical_base > request.window_size - BLOCK:
        raise LayoutError('layout exceeds window or leaves less than 2 MiB slack')
    return _Addresses(
        kernel_pa,
        args_pa,
        tree_pa,
        ramdisk_pa,
        ramdisk_size,
        trustcache_pa,
        data_seed_pa,
        data_seed_size,
        data_seed_span,
        used_end,
    )


def _patched_tree(tree_bytes: bytes, adt: Adt, at: _Addresses, sidecars: dict[str, bytes]) -> bytearray:
    tree = bytearray(tree_bytes)
    for key, address, size in (
        ('RAMDisk', at.ramdisk_pa, at.ramdisk_size),
        ('TrustCache', at.trustcache_pa, len(sidecars.get('TrustCache', b''))),
        ('NativeDataSeed', at.data_seed_pa, at.data_seed_size),
    ):
        if key in sidecars:
            struct.pack_into('<QQ', tree, adt.properties['/chosen/memory-map', key][0], address, size)
    return tree


def _boot_memory(request: LayoutRequest, kernel: Kernel, adt: Adt, kernel_pa: int) -> _BootMemory:
    full_ram, physical_base, window_size = request.full_ram, request.physical_base, request.window_size
    boot_phys = 0x80000000 if full_ram else physical_base
    boot_virt = kernel.base - kernel_pa + boot_phys
    boot_size = (4 << 30) if full_ram else window_size
    boot_actual = ((4 << 30) - (3 << 20)) if full_ram else window_size
    if full_ram and kernel_pa != 0xB207C000:
        raise LayoutError(
            f'full RAM puts the kernel at 0xB207C000 (boot_layout.h SWITCH_KERNEL_PHYS_BASE), not {kernel_pa:#x}: the '
            f'trust caches must fit the {0xB207C000 - physical_base:#x} bytes below it'
        )
    if full_ram and adt.properties.get(('/chosen', 'switch-ram-ledger'), (0, 0))[1] != 1584:
        raise LayoutError('full RAM requires the measured native profile and runtime ledger')
    return _BootMemory(boot_phys, boot_virt, boot_size, boot_actual)


class _Payload:
    def __init__(self, physical_base: int, used: int) -> None:
        self.physical_base = physical_base
        self.used = used
        self.data = bytearray(used)
        self.ranges = []

    def place(self, name: str, address: int, data: bytes, memory_size: int) -> None:
        offset = address - self.physical_base
        if offset < 0 or len(data) > memory_size or offset + memory_size > self.used:
            raise LayoutError('internal layout bounds failure')
        for other in self.ranges:
            if address < other['end'] and other['start'] < address + memory_size:
                raise LayoutError('internal layout overlap')
        self.data[offset : offset + len(data)] = data
        self.ranges.append(
            {
                'name': name,
                'start': address,
                'size': memory_size,
                'end': address + memory_size,
                'file_bytes': len(data),
                'image_offset': offset,
            }
        )

    def place_kernel(self, kernel: Kernel, kernel_pa: int, image: bytes) -> list[dict]:
        segments = []
        for index, segment in enumerate(kernel.segments):
            pa = kernel_pa + segment.vmaddr - kernel.base
            self.place(
                f'segment:{index}:{segment.name}',
                pa,
                image[segment.fileoff : segment.fileoff + segment.filesize],
                segment.vmsize,
            )
            segments.append(
                dict(
                    asdict(segment),
                    physical_address=pa,
                    image_offset=pa - self.physical_base,
                    zero_fill_bytes=segment.vmsize - segment.filesize,
                )
            )
        return segments

    def place_sidecars(self, at: _Addresses, sidecars: dict[str, bytes]) -> None:
        if 'RAMDisk' in sidecars:
            self.place('ramdisk', at.ramdisk_pa, sidecars['RAMDisk'], at.ramdisk_size)
        if 'TrustCache' in sidecars:
            self.place('trustcache', at.trustcache_pa, sidecars['TrustCache'], len(sidecars['TrustCache']))
        if 'NativeDataSeed' in sidecars:
            self.place('native_data_seed', at.data_seed_pa, sidecars['NativeDataSeed'], at.data_seed_span)


def _publish(output_dir: Path, payload: bytes, manifest: dict) -> None:
    output_dir.parent.mkdir(parents=True, exist_ok=True)
    created = False
    try:
        output_dir.mkdir()  # Atomic exclusive creation: never replace an existing result.
        created = True
        (output_dir / 'kernel-window.bin').write_bytes(payload)
        (output_dir / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    except OSError as exc:
        if created:
            shutil.rmtree(output_dir)
        raise LayoutError(f'could not publish layout: {exc}') from exc


def build_layout(kernel_path: Path, adt_path: Path, output_dir: Path, request: LayoutRequest) -> dict:
    """Validate everything, then create only kernel-window.bin and manifest.json."""
    kernel_path, adt_path, output_dir = map(Path, (kernel_path, adt_path, output_dir))
    if output_dir.exists() or output_dir.is_symlink():
        raise LayoutError('output already exists; choose a new directory')
    if type(request.full_ram) is not bool:
        raise LayoutError('full_ram must be boolean')
    physical_base, window_size = request.physical_base, request.window_size
    window = _window(request)
    reservations = _reservations(request.reserved_ranges, window)
    image = read_input(kernel_path, 128 << 20)
    tree_bytes = read_input(adt_path, 16 << 20)
    kernel, adt = parse_kernel(image), parse_adt(tree_bytes)
    _check_bases(request, kernel, adt)
    command = _command(request.command_line)
    video, video_range = _video(request.video, window)
    sources = {'kernel': source_record(kernel_path, image), 'adt': source_record(adt_path, tree_bytes)}
    sidecars = _sidecars(request, adt, sources)
    at = _addresses(request, kernel, len(tree_bytes), sidecars)
    used = at.used_end - physical_base
    tree = _patched_tree(tree_bytes, adt, at, sidecars)
    boot = _boot_memory(request, kernel, adt, at.kernel_pa)
    adt_va = kernel.base + at.tree_pa - at.kernel_pa
    boot_args = bytearray(BOOT_ARGS_SIZE)
    struct.pack_into('<HHI4Q6Q', boot_args, 0, 2, 2, 0, boot.virt, boot.phys, boot.size, at.used_end, *video)
    struct.pack_into('<QI', boot_args, 0x60, adt_va, len(tree))
    boot_args[0x6C : 0x6C + len(command)] = command
    struct.pack_into('<Q', boot_args, 0x478, boot.actual)
    payload = _Payload(physical_base, used)
    segments = payload.place_kernel(kernel, at.kernel_pa, image)
    payload.place('boot_args', at.args_pa, boot_args, PAGE)
    payload.place('adt', at.tree_pa, tree, len(tree))
    payload.place_sidecars(at, sidecars)
    manifest = {
        'schema_version': 1,
        'profile': 'native-xnu-host-layout',
        'native_window_ownership_verified': False,
        'full_ram': request.full_ram,
        'window_role': 'contiguous early image/bootstrap safety envelope' if request.full_ram else 'managed RAM',
        'boot_phys_base': boot.phys,
        'boot_virt_base': boot.virt,
        'boot_memory_size': boot.size,
        'boot_memory_actual': boot.actual,
        'proof_boundary': (
            'Host layout only; addresses and exclusions do not establish native RAM ownership, launchability or a '
            'kernel boot.'
        ),
        'sources': sources,
        'phys_base': physical_base,
        'kernel_phys_base': at.kernel_pa,
        'virt_base': kernel.base,
        'owned_window_size': window_size,
        'window_end': window.end,
        'entry_pa': at.kernel_pa + kernel.entry - kernel.base,
        'entry_va': kernel.entry,
        'boot_args_pa': at.args_pa,
        'boot_args_size': BOOT_ARGS_SIZE,
        'adt_pa': at.tree_pa,
        'adt_va': adt_va,
        'adt_size': len(tree),
        'adt_patched_sha256': hashlib.sha256(tree).hexdigest(),
        'used_extent': used,
        'used_end': at.used_end,
        'top_of_kernel_data': at.used_end,
        'unused_window_bytes': window_size - used,
        'block_rounded_extent': {
            'start': window.mapped_start,
            'end': window.mapped_end,
            'size': window.mapped_end - window.mapped_start,
        },
        'segments': segments,
        'ranges': payload.ranges,
        'reserved_ranges': reservations,
        'video': video,
        'framebuffer_range': video_range,
        'command_line': request.command_line,
        'image': {
            'file': 'kernel-window.bin',
            'size_bytes': len(payload.data),
            'sha256': hashlib.sha256(payload.data).hexdigest(),
        },
    }
    _publish(output_dir, payload.data, manifest)
    return manifest


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--kernel', required=True, type=Path)
    parser.add_argument('--adt', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument(
        '--physical-base', required=True, type=lambda value: int(value, 0), help='where the window starts'
    )
    parser.add_argument('--window-size', required=True, type=lambda value: int(value, 0))
    parser.add_argument(
        '--reserved-ranges',
        required=True,
        type=Path,
        help='JSON array of {"name": str, "start": int, "size": int}; [] is an explicit model assumption',
    )
    parser.add_argument('--ramdisk', type=Path)
    parser.add_argument('--trustcache', type=Path)
    parser.add_argument(
        '--data-seed', type=Path, help='Sparse DBRDISK1 NativeDataSeed for a separate RAM-backed Data device'
    )
    parser.add_argument('--command-line', default='')
    parser.add_argument(
        '--video',
        nargs=6,
        type=lambda value: int(value, 0),
        metavar=('BASE', 'DISPLAY', 'ROW_BYTES', 'WIDTH', 'HEIGHT', 'DEPTH'),
    )
    args = parser.parse_args()
    try:
        reservations = json.loads(args.reserved_ranges.read_text())
        manifest = build_layout(
            args.kernel,
            args.adt,
            args.output,
            LayoutRequest(
                physical_base=args.physical_base,
                window_size=args.window_size,
                reserved_ranges=reservations,
                ramdisk_path=args.ramdisk,
                trustcache_path=args.trustcache,
                data_seed_path=args.data_seed,
                command_line=args.command_line,
                video=args.video,
            ),
        )
    except (LayoutError, OSError, ValueError) as exc:
        print(f'native layout refused: {exc}', file=sys.stderr)
        return 1
    print(
        json.dumps(
            {
                'output': str(args.output.resolve()),
                'entry_pa': manifest['entry_pa'],
                'boot_args_pa': manifest['boot_args_pa'],
                'used_extent': manifest['used_extent'],
                'native_window_ownership_verified': False,
            },
            indent=2,
        )
    )
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
