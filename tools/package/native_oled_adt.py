#!/usr/bin/env python3
"""The Apple DeviceTree of the native OLED board (generate_adt).

//boot:adt's full-RAM root-boot profile, which the package then gives its iPad identity (adt_edit.py).

This is a fixed board description, not a conversion of the QEMU tree. It does
not establish memory ownership, interrupt delivery, or a successful XNU boot.
"""

from __future__ import annotations

import os
import struct
import zlib
from collections.abc import Iterable, Sequence
from typing import NamedTuple

from loader_abi import SEED_BYTES

MANAGED_BASE = 0xB2004000
MANAGED_USER_BASE = 0xB207C000
MANAGED_SIZE = 768 << 20
DRAM_BASE = 0x80000000
DRAM_SIZE = 4 << 30
TIMEBASE_HZ = 19_200_000
GICD_BASE, GICD_SIZE = 0x50041000, 0x1000
GICC_BASE, GICC_SIZE = 0x50042000, 0x100
MODEL = 'Nintendo Switch (OLED model)'
COMPATIBLE = ('nvidia,fric', 'nintendo,aula', 'nintendo,nx', 'nvidia,tegra210b01')
ROOT_VOLUME_UUID = '04769D26-2CA0-4825-B443-E2E5F88300F2'


def root_matching(volume_uuid: str) -> bytes:
    """/chosen/root-matching: the IOMedia of the APFS volume `volume_uuid` (the IPSW kind's System volume)."""
    return (
        '<dict><key>IOProviderClass</key><string>IOMedia</string>'
        '<key>IOPropertyMatch</key><dict><key>UUID</key><string>' + volume_uuid.upper() + '</string></dict></dict>'
    ).encode('ascii') + b'\0'


ROOT_MATCHING = root_matching(ROOT_VOLUME_UUID)


def property_bytes(key: str, value: bytes, flags: int = 0) -> bytes:
    """One Apple DT property: its 32-byte name, its length (`flags` in the top bit), its value padded to 4 bytes."""
    return struct.pack('<32sI', key.encode('ascii'), flags | len(value)) + value + bytes(-len(value) % 4)


def node(name: str, properties: Iterable[tuple[str, bytes]] = (), children: Sequence[bytes] = ()) -> bytes:
    """Apple DT: LE counts, then the properties (its name first), then the children."""
    properties = [('name', name.encode('ascii') + b'\0'), *properties]
    head = struct.pack('<II', len(properties), len(children))
    return head + b''.join(property_bytes(key, value) for key, value in properties) + b''.join(children)


def _empty_nvram_proxy() -> list[tuple[str, bytes]]:
    """Return an empty, volatile CHRP v1 bank for the actual IODTNVRAM consumer.

    ArmDTPlatform requires this bootstrap service. The 8 KiB bank and 2 KiB
    common partition are software allocations, not claimed OLED hardware.
    No system partition, policy variable, controller or persistence is supplied.
    """

    def header(name: bytes, size: int) -> bytes:
        data = bytearray(struct.pack('<BBH12s', 0x70, 0, size // 16, name))
        checksum = sum(data)
        while checksum > 255:
            checksum = (checksum & 255) + (checksum >> 8)
        data[1] = checksum
        return bytes(data)

    image = bytearray(8192)
    image[:16] = header(b'nvram', 32)
    image[32:48] = header(b'common', 16 + 2048)
    free_at = 32 + 16 + 2048
    image[free_at : free_at + 16] = header(b'free', len(image) - free_at)
    struct.pack_into('<I', image, 16, zlib.adler32(image[20:]))
    return [
        ('nvram-bank-size', struct.pack('<I', len(image))),
        ('nvram-bank-count', struct.pack('<I', 1)),
        ('nvram-current-bank', struct.pack('<I', 0)),
        ('nvram-proxy-data', bytes(image)),
    ]


class Profile(NamedTuple):
    userspace_fixture: bool = False
    storage_probe: bool = False
    root_boot: bool = False
    full_ram: bool = False
    native_smp: bool = False


def _check_profile(profile: Profile) -> None:
    userspace_fixture, storage_probe, root_boot, full_ram, native_smp = profile
    if type(userspace_fixture) is not bool:
        raise ValueError('userspace_fixture must be a boolean')
    if type(root_boot) is not bool or (root_boot and userspace_fixture):
        raise ValueError('root_boot must be a boolean and excludes the userspace fixture')
    if type(storage_probe) is not bool or (storage_probe and not (userspace_fixture or root_boot)):
        raise ValueError('storage_probe requires an explicit userspace or root profile')
    if root_boot and not storage_probe:
        raise ValueError('root_boot requires the same-boot native storage reference')
    if type(full_ram) is not bool:
        raise ValueError('full_ram must be boolean')
    if full_ram and not root_boot:
        raise ValueError('full_ram requires native root profile')
    if type(native_smp) is not bool or (native_smp and not (root_boot and full_ram)):
        raise ValueError('native_smp requires the explicit full-RAM native root profile')


def _bootstrap(profile: Profile) -> list[tuple[str, bytes]]:
    userspace_fixture, storage_probe, root_boot, full_ram, _ = profile
    bootstrap = (
        [
            ('osenvironment', b'main\0' if root_boot else b'ramdisk\0'),
            ('crypto-hash-method', b'sha2-384\0'),
            ('amfi-only-platform-code', struct.pack('<I', 1)),
        ]
        if userspace_fixture or root_boot
        else []
    )
    if root_boot:
        bootstrap += [('ephemeral-storage', struct.pack('<I', 1)), ('root-matching', ROOT_MATCHING)]
    if storage_probe:
        bootstrap.append(('switch-sd-reference', struct.pack('<QQ', 0xA001F000, 0x1000)))
    if full_ram:
        bootstrap.append(('switch-ram-ledger', bytes(16 + 32 + 96 * 16)))
    return bootstrap


def _memory_map(profile: Profile) -> list[tuple[str, bytes]]:
    # Sidecar slots stay empty until native_xnu_layout checks and places their
    # exact bytes inside the managed kernel window. The root profile uses a
    # disposable Data seed; the userspace fixture profile uses a RAM root.
    if profile.root_boot:
        return [('NativeDataSeed', bytes(16)), ('TrustCache', bytes(16))]
    if profile.userspace_fixture:
        return [('RAMDisk', bytes(16)), ('TrustCache', bytes(16))]
    return []


def _cpus(native_smp: bool) -> bytes:
    return node(
        'cpus',
        children=[
            node(
                f'cpu{core}',
                [
                    ('state', b'stopped\0' if core else b'running\0'),
                    ('reg', struct.pack('<I', core)),  # MPIDR affinity, not its RES1 bit.
                    ('device_type', b'cpu\0'),
                    ('compatible', b'arm,cortex-a57\0'),
                    ('timebase-frequency', struct.pack('<I', TIMEBASE_HZ)),
                    *(
                        [
                            ('cluster-type', struct.pack('<I', 0)),
                            ('die-cluster-id', struct.pack('<I', 0)),
                            ('cluster-core-id', struct.pack('<I', core)),
                            ('l2-cache-id', struct.pack('<I', 0)),
                        ]
                        if native_smp
                        else []
                    ),
                ],
            )
            for core in range(4 if native_smp else 1)
        ],
    )


def generate_adt(seed: bytes | None, profile: Profile) -> bytes:
    """Serialize only the audited native resources; deterministic seed for tests."""
    if seed is None:
        seed = os.urandom(SEED_BYTES)
    if not isinstance(seed, bytes) or len(seed) != SEED_BYTES:
        raise ValueError(f'random seed must be exactly {SEED_BYTES} bytes')
    _check_profile(profile)
    userspace_fixture, _, root_boot, full_ram, native_smp = profile
    return node(
        'device-tree',
        [
            ('model', MODEL.encode('ascii') + b'\0'),
            ('compatible', b'\0'.join(s.encode('ascii') for s in COMPATIBLE) + b'\0'),
        ],
        [
            node(
                'chosen',
                [
                    ('random-seed', seed),
                    # arm_init copies actual installed DRAM metadata to gDramBase/Size.
                    # boot_args physBase/memSize alone define the managed allocation.
                    ('dram-base', struct.pack('<Q', DRAM_BASE)),
                    ('dram-size', struct.pack('<Q', DRAM_SIZE)),
                    # Host layout request, not a claim that this window is owned.
                    ('darwinboot-memory-size', struct.pack('<Q', (4 << 30) if full_ram else MANAGED_SIZE)),
                    *_empty_nvram_proxy(),
                    *_bootstrap(profile),
                ],
                [
                    # arm_vm_init asserts that this node exists; TrustCache is optional.
                    node('memory-map', _memory_map(profile)),
                    *([node('manifest-properties')] if userspace_fixture or root_boot else []),
                ],
            ),
            _cpus(native_smp),
            *([node('psci', [('compatible', b'arm,psci-0.2\0'), ('method', b'smc\0')])] if native_smp else []),
            # Preserve XNU's RAM-based vnode sizing instead of the BCM2837 cap.
            node(
                'defaults',
                [('kern.maxvnodes', struct.pack('<I', ((4 << 30) if full_ram else MANAGED_SIZE) // 65536 + 1024))]
                if root_boot
                else [],
            ),  # No vmm-present on physical hardware.
            node('options'),
            # Original AppleImage4 requires this node in the main OS environment;
            # the strings still identify the actual Nintendo board.
            *(
                [
                    node(
                        'product',
                        [('product-name', MODEL.encode('ascii') + b'\0'), ('model', MODEL.encode('ascii') + b'\0')],
                    )
                ]
                if userspace_fixture or root_boot
                else []
            ),
            node(
                'arm-io',
                [
                    ('device_type', b'nvidia,tegra210b01\0'),
                    # Exactly the contiguous GICD/GICC span. The driver adds bus base
                    # to these relative reg offsets; no wider Tegra bus is invented.
                    ('ranges', struct.pack('<3Q', 0, GICD_BASE, GICC_BASE + GICC_SIZE - GICD_BASE)),
                ],
                [
                    node(
                        'intc',
                        [
                            ('compatible', b'arm,cortex-a15-gic\0'),
                            ('reg', struct.pack('<4Q', 0, GICD_SIZE, GICC_BASE - GICD_BASE, GICC_SIZE)),
                            # ArmCpu selects virtual timer INTID 27 internally. There is no
                            # MMIO timer or Apple interrupt-controller="master" declaration.
                        ],
                    )
                ],
            ),
        ],
    )
