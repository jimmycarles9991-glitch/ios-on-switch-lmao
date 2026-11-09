#!/usr/bin/env python3
"""The kernel window the native loader copies into place, and the loader's generated headers.

Steps of //boot:package (boot/defs.bzl), each its own action:

    build_native_loader.py layout --kernel K --adt ADT --trustcache BUNDLE --data-seed SEED --boot-args F \
        --plan OUT --blocks DIR
    (per shard: lz4 -12 over DIR/NN/*, each block checked by decoding it again: boot/defs.bzl, no Python)
    build_native_loader.py join --plan P --compressed DIR --layout-h OUT --runs-h OUT --window OUT --constants OUT

layout lays the kernel, the ADT, the trust cache bundle and the Data seed out at their physical addresses
(native_xnu_layout.build_layout, the full-RAM root-boot profile), checks the result against every contract the loader
relies on and packs it (nonzero extents only, cut into 1 MiB LZ4 blocks). Each block goes to DIR/NN/<offset>, NN the
MiB of the window it starts in (loader_abi.WINDOW_SHARDS shards, one compress action each): a block's shard holds
whatever is around it, so an edit recompresses only the shards whose blocks it changed (a driver edit: the native
kernel's few; a boot-arg, ADT or trust cache edit: the tail's) and every other shard's compression is a cache hit.
join reads DIR/NN/<offset>.lz4 (lz4's frames) and writes:

  layout.h        the loader's LOAD_* constants (boot/loader/loader.c, copy.c, entry.S, copy.S include it)
  runs.h          copy.c's table of the staged window's blocks
  window          the staged window: the blocks back to back, padded to 4 KiB (bl33.py appends it to U-Boot)
  constants.json  the same constants for the steps after this one (bl33.py, boot_script.py, loader_link.py)

The loader itself is compiled by Bazel (boot/defs.bzl), one action per source.
"""

from __future__ import annotations

import argparse
import hashlib
import itertools
import json
import os
import shutil
import struct
import subprocess
import tempfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from typing import NamedTuple

from loader_abi import (
    BL33_BASE,
    BOOT_MANIFEST_HASH_BYTES,
    BOOT_MANIFEST_HASH_PROPERTY,
    EXPORTER_RUN,
    KERNEL_PHYS_BASE,
    LZ4_BLOCK,
    LZ4_MAGIC,
    NATIVE_FULL_RAM_EXCLUSIONS,
    PACK_MIN_GAP,
    PACK_PAGE,
    SD_REFERENCE,
    SD_REFERENCE_BYTES,
    SEED_BYTES,
    SEED_PROPERTY,
    SEED_SOURCE_BYTES,
    SEED_SOURCE_PROPERTY,
    STAGE_END,
    STAGE_HEADER,
    STAGE_SOURCE,
    STAGE_WORDS,
    STAMPS_BYTES,
    STAMPS_PROPERTY,
    VIDEO,
    WINDOW_BASE,
    WINDOW_BYTES_MAX,
    WINDOW_SHARD_BYTES,
    WINDOW_SHARDS,
    WINDOW_SIZE,
    staged_bl33_bytes,
)
from native_oled_adt import root_matching
from native_xnu_layout import LayoutRequest, build_layout, parse_adt, validate_data_seed, validate_trustcache


def pack_window(payload: bytes) -> tuple[list[tuple[int, int]], bytes]:
    """Return the payload's nonzero extents as (offset, bytes) and their bytes back to back.

    Checked by expanding them again.
    """
    zero, runs, offset = bytes(PACK_PAGE), [], 0
    while offset < len(payload):
        if payload[offset : offset + PACK_PAGE] == zero[: len(payload) - offset]:
            offset += PACK_PAGE
            continue
        start = offset
        while offset < len(payload) and payload[offset : offset + PACK_PAGE] != zero[: len(payload) - offset]:
            offset += PACK_PAGE
        end = min(offset, len(payload))
        if runs and start - (runs[-1][0] + runs[-1][1]) < PACK_MIN_GAP:
            runs[-1] = (runs[-1][0], end - runs[-1][0])
        else:
            runs.append((start, end - start))
    packed = b''.join(payload[start : start + size] for start, size in runs)
    expanded, at = bytearray(len(payload)), 0
    for start, size in runs:
        expanded[start : start + size] = packed[at : at + size]
        at += size
    if expanded != payload:
        raise ValueError('packed kernel window does not expand to the layout image')
    return runs, packed


def lz4_frame_blocks(frame: bytes, length: int) -> list[tuple[int, bool, bytes]]:
    """Split an LZ4 frame (lz4.org frame format) of `length` bytes into its blocks.

    Each block as (decoded length, stored, block bytes). Only what the loader decodes: independent blocks without
    checksums or dictionary.
    """
    if frame[:4] != LZ4_MAGIC or len(frame) < 7:
        raise ValueError('lz4: not an LZ4 frame')
    flags, descriptor = frame[4], frame[5]
    if flags >> 6 != 1 or not flags >> 5 & 1 or flags >> 4 & 1 or flags & 1 or not descriptor >> 4 & 7 >= 4:
        raise ValueError(f'lz4: unsupported frame flags {flags:#x}/{descriptor:#x}')
    block_max = 1 << (8 + 2 * (descriptor >> 4 & 7))
    at, left, blocks = 6 + (8 if flags >> 3 & 1 else 0) + 1, length, []
    while True:
        size = struct.unpack_from('<I', frame, at)[0]
        at += 4
        if not size:
            break
        stored, size = bool(size >> 31), size & 0x7FFFFFFF
        if not size or not left or at + size > len(frame):
            raise ValueError('lz4: truncated frame')
        blocks.append((min(block_max, left), stored, frame[at : at + size]))
        left -= blocks[-1][0]
        at += size
    if left or at + (4 if flags >> 2 & 1 else 0) != len(frame):
        raise ValueError('lz4: frame does not hold exactly the extent')
    return blocks


def lz4_blocks(runs: list[tuple[int, int]]) -> list[tuple[int, int]]:
    """Return the extents' LZ4 blocks as (window offset, length).

    Each extent is cut into 1 MiB blocks from its start (lz4 -B6).
    """
    return [
        (at, min(LZ4_BLOCK, start + size - at)) for start, size in runs for at in range(start, start + size, LZ4_BLOCK)
    ]


def compress_blocks(blocks: list[bytes], lz4: str | None = None) -> list[tuple[bool, bytes]]:
    """Each block LZ4-compressed on its own (lz4 -12, in parallel) as (stored, block bytes), checked by decoding it.

    The extents are independent-block frames, so a block's bytes are the same whether one lz4 compresses the whole
    extent or this block alone: each is its own compression, with nothing carried over from the block before.
    """
    lz4 = lz4 or shutil.which('lz4')
    if not lz4:
        raise ValueError('missing build tool: lz4')

    def compress(data: bytes) -> tuple[bool, bytes]:
        frame = subprocess.run(
            [lz4, '-12', '-B6', '--no-frame-crc', '-c', '-q'], input=data, capture_output=True, check=True
        ).stdout
        (decoded_length, stored, body), *more = lz4_frame_blocks(frame, len(data))
        if more or decoded_length != len(data):
            raise ValueError('lz4: a block did not compress to one block')
        if subprocess.run([lz4, '-d', '-c', '-q'], input=frame, capture_output=True, check=True).stdout != data:
            raise ValueError('lz4: a block does not decode to the window')
        return stored, body

    with ThreadPoolExecutor(max_workers=os.cpu_count()) as pool:
        return list(pool.map(compress, blocks))


def block_table(blocks: list[tuple[int, int]], compressed: list[tuple[bool, bytes]]) -> tuple[list, bytes]:
    """Return the loader's block table and the blocks back to back.

    Each row of the table: window offset, length, offset in the stream, stream bytes | 1 << 63 if stored.
    """
    table, stream = [], bytearray()
    for (at, length), (stored, body) in zip(blocks, compressed, strict=True):
        if (at | length) % PACK_PAGE:
            raise ValueError('lz4: block not page-granular')
        table.append((at, length, len(stream), len(body) | (1 << 63 if stored else 0)))
        stream += body
    return table, bytes(stream)


def compress_window(
    payload: bytes, runs: list[tuple[int, int]], lz4: str | None = None
) -> tuple[list[tuple[int, int, int, int]], bytes]:
    """Compress each of the window's nonzero extents with LZ4 (lz4 -12, independent blocks of up to 1 MiB).

    Returns the loader's block table and the blocks back to back, checked by laying the blocks back into an empty
    window.
    """
    blocks = lz4_blocks(runs)
    table, stream = block_table(blocks, compress_blocks([payload[at : at + length] for at, length in blocks], lz4))
    expanded = bytearray(len(payload))
    for at, length in blocks:
        expanded[at : at + length] = payload[at : at + length]
    if expanded != payload:
        raise ValueError('compressed kernel window does not decode to the layout image')
    return table, stream


# The EMC rate the kernel's SwitchPMIC sets (1600 MHz), which the loader sets before its copy (boot/loader/emc.h).
EMC_KHZ = 1600000


def stage_id(staged: bytes) -> int:
    """Return the staged window's id (boot/loader/stage.h): its SHA-256's first 8 bytes, little-endian."""
    return int.from_bytes(hashlib.sha256(staged).digest()[:8], 'little')


def stage_pages(staged: bytes, stream_bytes: int) -> tuple[bytes, bytes]:
    """Return the header and trailer pages around the staged window (boot/loader/stage.h)."""
    ident = stage_id(staged)
    words = STAGE_WORDS
    header = struct.pack(
        '<8I',
        words['magic'],
        words['head'],
        words['version'],
        STAGE_SOURCE - STAGE_HEADER,
        stream_bytes,
        len(staged),
        ident & 0xFFFFFFFF,
        ident >> 32,
    )
    trailer = struct.pack(
        '<6I', words['magic'], words['tail'], stream_bytes, len(staged), ident & 0xFFFFFFFF, ident >> 32
    )
    return header.ljust(PACK_PAGE, b'\0'), trailer.ljust(PACK_PAGE, b'\0')


def stage_into_bl33(bl33: bytes, staged: bytes, stream_bytes: int, slots: list | tuple = ()) -> bytes:
    """Return bl33.bin with the staged kernel window after U-Boot (boot/loader/stage.h).

    Each (address, capacity, bytes) of slots goes in U-Boot's padding (loader_abi SCRIPT_SLOT and the others); the
    result is checked by check_stage.
    """
    if len(staged) % PACK_PAGE or not stream_bytes <= len(staged) < stream_bytes + PACK_PAGE:
        raise ValueError('staged window must be its stream padded to 4 KiB')
    header, trailer = stage_pages(staged, stream_bytes)
    first = bytearray(bl33.ljust(STAGE_HEADER - BL33_BASE, b'\0'))
    for address, capacity, content in slots:
        if not content or len(content) > capacity:
            raise ValueError(f'bl33: {len(content)} bytes for the {capacity:#x}-byte slot at {address:#x}')
        first[address - BL33_BASE : address - BL33_BASE + len(content)] = content
    image = bytes(first) + header + staged + trailer
    check_stage(image, bl33, staged, stream_bytes, slots)
    if len(image) != staged_bl33_bytes(len(staged)):
        raise ValueError('bl33: not the size loader_abi.staged_bl33_bytes gives')
    return image


def check_stage(image: bytes, bl33: bytes, staged: bytes, stream_bytes: int, slots: list | tuple = ()) -> None:
    """Check the composed bl33.bin as hekate puts it at 0xAA000000.

    U-Boot untouched and wholly below the first slot, including its pre-relocation BSS (U-Boot's arm64 start.S header:
    _end_ofs at 0x10, _bss_end_ofs at 0x20); each slot holding its bytes, apart from the others and the rest zero;
    header, window and trailer where the boot script and the loader look; everything below hekate's ramoops
    (0xB0000000).
    """
    header_at = STAGE_HEADER - BL33_BASE
    end_ofs, bss_end_ofs = struct.unpack_from('<Q', bl33, 0x10)[0], struct.unpack_from('<Q', bl33, 0x20)[0]
    floor = min([address - BL33_BASE for address, _, _ in slots] + [header_at])
    if not 0 < end_ofs <= len(bl33) <= floor or not 0 < bss_end_ofs <= floor:
        raise ValueError(f'bl33: U-Boot (_end {end_ofs:#x}, BSS end {bss_end_ofs:#x}) reaches a slot or the window')
    padding = bytearray(image[:header_at])
    if padding[: len(bl33)] != bl33:
        raise ValueError('bl33: U-Boot changed')
    padding[: len(bl33)] = bytes(len(bl33))
    spans = sorted((address - BL33_BASE, capacity, content) for address, capacity, content in slots)
    for (at, capacity, content), (following, _, _) in itertools.pairwise([*spans, (header_at, 0, b'')]):
        if at + capacity > following or padding[at : at + len(content)] != content:
            raise ValueError(f'bl33: the slot at {BL33_BASE + at:#x} overlaps the next or does not hold its bytes')
        padding[at : at + len(content)] = bytes(len(content))
    if any(padding):
        raise ValueError('bl33: the padding outside the slots is not zero')
    header, trailer = stage_pages(staged, stream_bytes)
    source = STAGE_SOURCE - BL33_BASE
    if (
        image[header_at:source] != header
        or image[source : source + len(staged)] != staged
        or image[source + len(staged) :] != trailer
    ):
        raise ValueError('bl33: staged window not where the loader reads it')
    if BL33_BASE + len(image) > STAGE_END:
        raise ValueError('bl33: staged window reaches ramoops at 0xB0000000')


def _require(condition: object, message: str) -> None:
    if not condition:
        raise ValueError(message)


def _check_layout(m: dict, payload: bytes) -> None:
    _require(m['schema_version'] == 1 and m['profile'] == 'native-xnu-host-layout', 'unsupported host layout')
    _require(
        m.get('native_window_ownership_verified') is False,
        'this builder requires an explicitly unverified ownership status',
    )
    _require(m['image']['file'] == 'kernel-window.bin', 'unexpected image filename')
    _require(
        len(payload) == m['used_extent'] == m['image']['size_bytes']
        and hashlib.sha256(payload).hexdigest() == m['image']['sha256'],
        'layout image identity mismatch',
    )
    _require(0 < len(payload) <= WINDOW_BYTES_MAX and not len(payload) % 8, 'image exceeds the bounded window')
    _require(m['phys_base'] == WINDOW_BASE and m['owned_window_size'] == WINDOW_SIZE, 'not the root-boot window')
    _require(m['kernel_phys_base'] == KERNEL_PHYS_BASE, 'the kernel is not at boot_layout.h SWITCH_KERNEL_PHYS_BASE')
    for key in (
        'phys_base',
        'kernel_phys_base',
        'virt_base',
        'owned_window_size',
        'entry_pa',
        'entry_va',
        'boot_args_pa',
        'boot_args_size',
        'adt_pa',
        'adt_va',
        'adt_size',
        'top_of_kernel_data',
        'used_end',
        'window_end',
    ):
        _require(type(m[key]) is int and 0 <= m[key] < 1 << 64, 'invalid unsigned layout field: ' + key)
    _require(
        m['top_of_kernel_data'] == m['phys_base'] + len(payload)
        and m['used_end'] == m['top_of_kernel_data']
        and m['window_end'] == m['phys_base'] + m['owned_window_size']
        and m['adt_va'] == m['virt_base'] + m['adt_pa'] - m['kernel_phys_base']
        and m['entry_va'] == m['virt_base'] + m['entry_pa'] - m['kernel_phys_base']
        and m['virt_base'] + m['owned_window_size'] < 1 << 64
        and m['phys_base'] <= m['kernel_phys_base']
        and m['kernel_phys_base'] % 0x200000 == m['virt_base'] % 0x200000
        and not m['boot_args_pa'] % 0x4000
        and m['boot_args_size'] == 0x480
        and m['adt_pa'] == m['boot_args_pa'] + 0x1000
        and m['adt_size'] > 0
        and m['adt_pa'] + m['adt_size'] <= m['used_end']
        and m['boot_args_pa'] + 0x480 <= m['used_end']
        and not m['entry_pa'] % 4
        and len(payload) <= m['owned_window_size'] - 0x200000,
        'inconsistent fixed-layout relationships',
    )
    _require(STAGE_SOURCE + len(payload) <= m['phys_base'], 'staging intersects destination')
    for key in ('entry_pa', 'boot_args_pa', 'adt_pa'):
        _require(m['phys_base'] <= m[key] < m['phys_base'] + len(payload), 'layout address outside used extent: ' + key)
    _require(m.get('full_ram') is True, 'the root-boot loader takes the full-RAM layout')


def _check_sidecars(m: dict, payload: bytes, properties: dict, start: int, sidecars: tuple[str, ...]) -> None:
    spans = []
    for key in sidecars:
        pos, length = properties.get(('/chosen/memory-map', key), (-1, 0))
        _require(length == 16, 'root_boot sidecar placeholder missing: ' + key)
        address, size = struct.unpack_from('<QQ', payload, start + pos)
        expected = m['sources'][key]
        # arm_vm_init's EXTRADATA: the trust caches' pages end where the kernel starts. Everything else follows the ADT.
        if key == 'TrustCache':
            within = (
                not address % 0x4000
                and m['phys_base'] <= address
                and address + size <= m['kernel_phys_base'] < address + size + 0x4000
            )
        else:
            within = not address % 0x1000 and m['adt_pa'] + m['adt_size'] <= address <= m['used_end']
        _require(
            within and size == expected['size_bytes'] and 0 < size <= 16 << 20 and size <= m['used_end'] - address,
            'root_boot sidecar bounds changed: ' + key,
        )
        _require(
            not any(address < end and begin < address + size for begin, end in spans), 'root_boot sidecars overlap'
        )
        spans.append((address, address + size))
        content = payload[address - m['phys_base'] : address - m['phys_base'] + size]
        _require(
            hashlib.sha256(content).hexdigest() == expected['sha256'], 'root_boot sidecar contents changed: ' + key
        )
        (validate_data_seed if key == 'NativeDataSeed' else validate_trustcache)(content)


def _check_command_line(m: dict, payload: bytes) -> None:
    # The built-in line in the window's boot_args.
    words = m['command_line'].split()
    command_at = m['boot_args_pa'] - m['phys_base'] + 0x6C
    encoded = m['command_line'].encode('ascii')
    _require(
        len(encoded) < 1024 and payload[command_at : command_at + 1024] == encoded + bytes(1024 - len(encoded)),
        'the boot argument manifest does not match the staged boot arguments',
    )
    _require(
        not any(v.startswith(('rd=', 'boot-uuid=')) for v in words),
        'root_boot cannot use an alternate root selection',
    )


# The full-RAM bank (boot_phys_base, boot_memory_size, boot_memory_actual), measured on the Switch OLED.
FULL_RAM_BANK = (0x80000000, 0x100000000, 0xFFD00000)


def boot_virt_base(virt_base: int, kernel_phys_base: int, boot_phys: int) -> int:
    """Return the virtual address XNU gives physical boot_phys: the kernel's virt_base is at kernel_phys_base."""
    return virt_base - kernel_phys_base + boot_phys


def _check_exporter(phys_base: int, window_bytes: int) -> None:
    run, size = EXPORTER_RUN
    _, start, capacity = next(r for r in NATIVE_FULL_RAM_EXCLUSIONS if r[0] == 'staged-kernel-capacity')
    _require(
        start <= run
        and run + size <= start + capacity
        and (run + size <= BL33_BASE or run >= STAGE_END)
        and (run + size <= phys_base or phys_base + window_bytes <= run),
        'the firmware exporter overlaps the kernel window or leaves its reserved range',
    )


class KindPins(NamedTuple):
    """An IPSW kind and the pins (ipsw/pins.bzl) its window is checked against."""

    kind: str
    virt_base: int
    root_volume_uuid: str


def _check_native_profile(m: dict, payload: bytes, pins: KindPins) -> None:
    _require(
        m['virt_base'] == pins.virt_base,
        f"the prelinked image's lowest segment {m['virt_base']:#x} is not {pins.kind}'s kernel_virt_base "
        f'{pins.virt_base:#x} (ipsw/pins.bzl), which the kernel derives its boot_args virtBase from',
    )
    _require(
        m['phys_base'] & ~0x1FFFFF == 0xB2000000 and (m['window_end'] + 0x1FFFFF) & ~0x1FFFFF == 0xE2200000,
        'native ownership envelope changed',
    )
    expected = set(NATIVE_FULL_RAM_EXCLUSIONS)
    actual = {(r['name'], r['start'], r['size']) for r in m['reserved_ranges']}
    _require(
        actual == expected and len(m['reserved_ranges']) == len(expected),
        'native experiment requires the exact reviewed exclusion ledger',
    )
    _require(
        len(payload) <= WINDOW_BYTES_MAX and m['video'] == VIDEO,
        f'native staging or display contract mismatch (window {len(payload):#x} bytes)',
    )
    _require(not len(payload) % 0x1000, 'native cacheable copy requires a page-aligned payload')


def window_checks(m: dict, payload: bytes, adt: bytes, pins: KindPins, manifest_hash: bytes | None = None) -> dict:
    """Check what the cacheable-copy root-boot loader needs of a layout and return the loader's constants.

    `m` is the layout's manifest, `payload` its image and `adt` the ADT it was laid out with; `pins` are the IPSW kind's
    kernel_virt_base and System volume. The staged window's constants are None until it is made.
    """
    _check_layout(m, payload)

    # The same-boot SD reference, the RAM ledger, the boot timing record and the per-boot seed: zeroed ADT slots the
    # loader fills, at the offsets layout.h gives it.
    sidecars = ('NativeDataSeed', 'TrustCache')
    _require(not set(sidecars) - m['sources'].keys(), 'the root boot needs the Data seed and trust cache sidecars')
    _require(
        not any(k in m['sources'] for k in ('RAMDisk', 'PackedRAMDisk')), 'root_boot excludes a diagnostic RAM root'
    )
    source = m['sources']['adt']
    _require(
        len(adt) == source['size_bytes'] and hashlib.sha256(adt).hexdigest() == source['sha256'],
        'SD reference ADT source changed',
    )
    properties = parse_adt(adt).properties
    ram_offset, ram_bytes = properties.get(('/chosen', 'switch-ram-ledger'), (-1, 0))
    _require(
        ram_bytes == 1584 and adt[ram_offset : ram_offset + ram_bytes] == bytes(ram_bytes),
        'full RAM requires zeroed runtime ledger slot',
    )
    _require(
        (m.get('boot_phys_base'), m.get('boot_memory_size'), m.get('boot_memory_actual')) == FULL_RAM_BANK
        and m.get('boot_virt_base') == boot_virt_base(m['virt_base'], m['kernel_phys_base'], FULL_RAM_BANK[0]),
        'full RAM boot bases/span differ from measured bank profile',
    )
    start = m['adt_pa'] - m['phys_base']

    def in_window(offset: int, size: int) -> bytes:
        return payload[start + offset : start + offset + size]

    sd_reference_offset, size = properties.get(('/chosen', 'switch-sd-reference'), (-1, 0))
    expected = struct.pack('<QQ', SD_REFERENCE, SD_REFERENCE_BYTES)
    _require(
        size == 16
        and adt[sd_reference_offset : sd_reference_offset + size] == expected
        and in_window(sd_reference_offset, size) == expected,
        'missing or changed fixed SD reference property',
    )
    stamps_offset, stamps_bytes = properties.get(('/chosen', STAMPS_PROPERTY), (0, 0))
    _require(
        not stamps_offset
        or (
            stamps_bytes == STAMPS_BYTES
            and not stamps_offset % 4
            and adt[stamps_offset : stamps_offset + stamps_bytes] == bytes(stamps_bytes)
            and in_window(stamps_offset, stamps_bytes) == bytes(stamps_bytes)
        ),
        'boot timing placeholder must be a zeroed, aligned, exact-size /chosen property',
    )
    seed_offset = 0
    seed_source_offset, seed_source_bytes = properties.get(('/chosen', SEED_SOURCE_PROPERTY), (0, 0))
    if seed_source_offset:
        seed_offset, seed_bytes = properties.get(('/chosen', SEED_PROPERTY), (0, 0))
        _require(
            seed_source_bytes == SEED_SOURCE_BYTES
            and not seed_source_offset % 4
            and adt[seed_source_offset : seed_source_offset + seed_source_bytes] == bytes(seed_source_bytes)
            and in_window(seed_source_offset, seed_source_bytes) == bytes(seed_source_bytes),
            'seed report placeholder must be a zeroed, aligned, exact-size /chosen property',
        )
        _require(
            seed_offset
            and seed_bytes == SEED_BYTES
            and not seed_offset % 4
            and in_window(seed_offset, seed_bytes) == adt[seed_offset : seed_offset + seed_bytes],
            f'random-seed must be a {SEED_BYTES}-byte, aligned /chosen property carried into the window',
        )

    # The root boot: the OS volume by UUID, persistent Data, no RAM root or runtime manifest.
    required = {
        'osenvironment': b'main\0',
        'root-matching': root_matching(pins.root_volume_uuid),
        'crypto-hash-method': b'sha2-384\0',
        'amfi-only-platform-code': struct.pack('<I', 1),
        'ephemeral-storage': struct.pack('<I', 0),  # persistent SD Data
    }
    for key, value in required.items():
        pos, length = properties.get(('/chosen', key), (-1, 0))
        _require(
            length == len(value) and adt[pos : pos + length] == value and in_window(pos, length) == value,
            'root_boot chosen property changed: ' + key,
        )
    _require(
        ('/chosen/manifest-properties', 'name') in properties
        and not any(
            (path, key) != ('/chosen/manifest-properties', 'name')
            for path, key in properties
            if path.startswith('/chosen/manifest-properties')
        )
        and not any(
            key in ('RAMDisk', 'PackedRAMDisk', 'boot-uuid', 'boot-manifest-hash', 'owner-boot-manifest-domain')
            for path, key in properties
            if manifest_hash is None or (path, key) != ('/chosen', BOOT_MANIFEST_HASH_PROPERTY)
        ),
        'root_boot requires empty runtime manifest inputs and no RAM root',
    )
    if manifest_hash is not None:
        pos, length = properties[('/chosen', BOOT_MANIFEST_HASH_PROPERTY)]
        _require(
            adt[pos : pos + length] == manifest_hash and in_window(pos, length) == manifest_hash,
            'boot-manifest-hash must be the kernel and trust caches the window carries',
        )
    _check_sidecars(m, payload, properties, start, sidecars)
    _check_command_line(m, payload)
    _check_exporter(m['phys_base'], len(payload))
    _check_native_profile(m, payload, pins)

    return {
        'SOURCE': STAGE_SOURCE,
        'DEST': m['phys_base'],
        'BYTES': len(payload),
        'PACKED_BYTES': None,
        'STREAM_BYTES': None,
        'STAMPS_ADT_OFFSET': stamps_offset,
        'SEED_ADT_OFFSET': seed_offset,
        'SEED_SOURCE_ADT_OFFSET': seed_source_offset,
        'STAGE_ID': None,
        'EMC_KHZ': EMC_KHZ,
        'ARGS': m['boot_args_pa'],
        'ENTRY': m['entry_pa'],
        'CRC': 0,
        'VERIFY_KERNEL': 0,
        'VIRT': m['virt_base'],
        'ADT_PA': m['adt_pa'],
        'BOOT_PHYS': m.get('boot_phys_base', m['phys_base']),
        'BOOT_VIRT': m.get('boot_virt_base', m['virt_base']),
        'BOOT_SIZE': m.get('boot_memory_size', m['owned_window_size']),
        'BOOT_ACTUAL': m.get('boot_memory_actual', m['owned_window_size']),
        'FULL_RAM': 1,
        'RAM_ADT_OFFSET': ram_offset,
        'WINDOW': m['owned_window_size'],
        'TOP': m['top_of_kernel_data'],
        'ADT_VA': m['adt_va'],
        'ADT_BYTES': m['adt_size'],
        'NATIVE_GATES': 1,
        'SD_REFERENCE': 1,
        'SD_CARD': 1,
        'SD_REFERENCE_ADT_OFFSET': sd_reference_offset,
        'FAST_COPY': 1,
    }


def staged_window(constants: dict, stream: bytes) -> tuple[dict, bytes]:
    """Stage the window for bl33.bin: the stream padded to 4 KiB.

    Returns the constants with its sizes and stage id, and the staged window.
    """
    # Staged in bl33.bin (stage_into_bl33); the loader maps it in 4 KiB pages.
    staged = stream + bytes(-len(stream) % PACK_PAGE)
    _require(
        STAGE_SOURCE + len(staged) + PACK_PAGE <= STAGE_END, 'compressed kernel window does not fit before ramoops'
    )
    return dict(constants, PACKED_BYTES=len(staged), STREAM_BYTES=len(stream), STAGE_ID=stage_id(staged)), staged


def layout_header(constants: dict) -> str:
    return ''.join(f'#define LOAD_{k} UINT64_C(0x{v:x})\n' for k, v in constants.items())


def runs_header(blocks: list) -> str:
    return (
        "/* Generated: the kernel window's nonzero extents as LZ4 blocks (window offset, bytes, offset in\n"
        ' * kernel-window.bin, its bytes | LOADER_BLOCK_STORED if not compressed) in order, stored back to back;\n'
        ' * everything else in the window is zero. */\n'
        f'#define LOAD_BLOCK_COUNT {len(blocks)}u\n'
        'static const uint64_t loader_blocks[LOAD_BLOCK_COUNT][4] = {\n'
        + ''.join(
            f'    {{UINT64_C(0x{a:x}), UINT64_C(0x{b:x}), UINT64_C(0x{c:x}), UINT64_C(0x{d:x})}},\n'
            for a, b, c, d in blocks
        )
        + '};\n'
    )


def shard_of(at: int) -> str:
    """Return a block's shard: the MiB of the window it starts in (its directory below layout's --blocks)."""
    if not 0 <= at < WINDOW_SHARDS * WINDOW_SHARD_BYTES:
        raise ValueError(f'block at {at:#x} beyond the {WINDOW_SHARDS} window shards')
    return f'{at // WINDOW_SHARD_BYTES:02d}'


def block_name(at: int) -> str:
    return f'{at:08x}'


def with_boot_manifest_hash(adt: bytes, digest: bytes) -> bytes:
    """Return the ADT with its zeroed /chosen/boot-manifest-hash set to `digest` (kernel and trust caches SHA-384)."""
    offset, size = parse_adt(adt).properties.get(('/chosen', BOOT_MANIFEST_HASH_PROPERTY), (0, 0))
    _require(
        offset and size == BOOT_MANIFEST_HASH_BYTES == len(digest) and adt[offset : offset + size] == bytes(size),
        'boot manifest hash placeholder must be a zeroed, exact-size /chosen property',
    )
    return adt[:offset] + digest + adt[offset + size :]


def layout(args: argparse.Namespace) -> None:
    """Lay out and check the window, then write its blocks, each in its shard's directory (shard_of)."""
    command_line = args.boot_args.read_text().rstrip('\n')
    with tempfile.TemporaryDirectory() as temporary:
        scratch = Path(temporary)
        # The layout refuses symlinked inputs, and Bazel's sandbox stages every input as one: regular copies.
        inputs = {}
        for name in ('kernel', 'adt', 'trustcache', 'data_seed'):
            inputs[name] = scratch / name
            shutil.copyfile(getattr(args, name), inputs[name])
        manifest_hash = None
        if args.boot_manifest_hash:
            manifest_hash = hashlib.sha384(inputs['kernel'].read_bytes() + inputs['trustcache'].read_bytes()).digest()
            inputs['adt'].write_bytes(with_boot_manifest_hash(inputs['adt'].read_bytes(), manifest_hash))
        m = build_layout(
            inputs['kernel'],
            inputs['adt'],
            scratch / 'layout',
            LayoutRequest(
                physical_base=WINDOW_BASE,
                window_size=WINDOW_SIZE,
                reserved_ranges=[{'name': n, 'start': s, 'size': z} for n, s, z in NATIVE_FULL_RAM_EXCLUSIONS],
                command_line=command_line,
                trustcache_path=inputs['trustcache'],
                data_seed_path=inputs['data_seed'],
                video=VIDEO,
                full_ram=True,
            ),
        )
        payload = (scratch / 'layout/kernel-window.bin').read_bytes()
        constants = window_checks(
            m,
            payload,
            inputs['adt'].read_bytes(),
            KindPins(args.kind, args.virt_base, args.root_volume_uuid),
            manifest_hash,
        )
    runs, _ = pack_window(payload)
    blocks = lz4_blocks(runs)
    shards = sorted(p.name for p in args.blocks.iterdir())
    if shards != sorted(shard_of(i * WINDOW_SHARD_BYTES) for i in range(WINDOW_SHARDS)):
        raise SystemExit(f'{args.blocks}: not the {WINDOW_SHARDS} shard directories (boot/defs.bzl _WINDOW_SHARDS)')
    for at, length in blocks:
        (args.blocks / shard_of(at) / block_name(at)).write_bytes(payload[at : at + length])
    # image: what the loader's decode must give (boot:window_decode_test decodes the staged window against it).
    image = {'bytes': len(payload), 'sha256': m['image']['sha256']}  # window_checks: the payload's
    args.plan.write_text(
        json.dumps({'constants': constants, 'blocks': blocks, 'entry_pa': m['entry_pa'], 'image': image}) + '\n'
    )


def join(args: argparse.Namespace) -> None:
    """Stage the window and write what the loader compiles in.

    constants.json holds all of it, layout.h's constants and runs.h's table (BLOCKS): a loader built elsewhere (the
    release's) fits this window exactly when its constants.json is equal.
    """
    plan = json.loads(args.plan.read_text())
    compressed = []
    for at, length in plan['blocks']:
        frame = (args.compressed / shard_of(at) / (block_name(at) + '.lz4')).read_bytes()
        (decoded_length, stored, body), *more = lz4_frame_blocks(frame, length)
        if more or decoded_length != length:
            raise ValueError(f'lz4: the block at {at:#x} did not compress to one block')
        compressed.append((stored, body))
    found = sum(1 for _ in args.compressed.glob('*/*'))
    if found != len(compressed):
        raise SystemExit(f"{found} compressed blocks for the plan's {len(compressed)}")
    table, stream = block_table([(at, length) for at, length in plan['blocks']], compressed)
    constants, staged = staged_window(plan['constants'], stream)
    args.layout_h.write_text(layout_header(constants))
    args.runs_h.write_text(runs_header(table))
    args.window.write_bytes(staged)
    args.constants.write_text(json.dumps(constants | {'BLOCKS': table}, indent=1) + '\n')
    print(f'kernel window {len(staged)} bytes in {len(table)} blocks, entry {plan["entry_pa"]:#x}')


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    steps = parser.add_subparsers(dest='step', required=True)
    for step, names in (
        ('layout', ('kernel', 'adt', 'trustcache', 'data-seed', 'boot-args', 'plan', 'blocks')),
        ('join', ('plan', 'compressed', 'layout-h', 'runs-h', 'window', 'constants')),
    ):
        p = steps.add_parser(step)
        for name in names:
            p.add_argument('--' + name, required=True, type=Path)
        if step == 'layout':
            p.add_argument('--kind', required=True, help='the IPSW kind')
            p.add_argument(
                '--virt-base',
                required=True,
                type=lambda value: int(value, 0),
                help="the kind's kernel_virt_base (ipsw/pins.bzl)",
            )
            p.add_argument('--root-volume-uuid', required=True, help="the kind's System volume (ipsw/pins.bzl)")
            p.add_argument('--boot-manifest-hash', action='store_true', help="fill the ADT's zeroed slot")
    args = parser.parse_args()
    if args.step == 'layout':
        layout(args)
    else:
        join(args)


if __name__ == '__main__':
    main()
