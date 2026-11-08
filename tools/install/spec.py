#!/usr/bin/env python3
"""What the kernel expects on the SD card, read from the kernel's own sources (they are the spec, not a copy).

  spec.py --raw-mount SwitchRawMount.h --cryptex-h SwitchCryptex.h --cryptex-cpp SwitchCryptex.cpp
          --block-cpp NativeSwitchBlock.cpp --volumes-h switch_ipsw_volumes.h --out spec.json

SwitchRawMount.h        SWITCH_RAW_DATA_UUID, SWITCH_RAW_PREBOOT_UUID
SwitchCryptex.h         SWITCH_PREBOOT_UUID (must agree)
SwitchCryptex.cpp       which cryptex image macro is the System one and which the App one
NativeSwitchBlock.cpp   the APFS type GUID and GPT name it selects the partition by (any size), and that its root is
                          SWITCH_OS_VOLUME_UUID (also the ADT root-matching UUID)
switch_ipsw_volumes.h   the selected IPSW kind's System volume UUID and cryptex images and volume UUIDs
                          (//kernel:ipsw_volumes_header, from ipsw/pins.bzl)
"""

import argparse
import json
import re
import uuid
from pathlib import Path


def _define(text: str, name: str, path: str) -> str:
    found = re.search(r'^#define\s+' + name + r'\s+(.+?)\s*(?://.*|/\*.*)?$', text, re.MULTILINE)
    if not found:
        raise SystemExit(f'{path}: no #define {name}')
    return found.group(1).strip()


def _uuid(value: str, what: str) -> str:
    try:
        return str(uuid.UUID(value.strip('"'))).upper()
    except ValueError:
        raise SystemExit(f'{what} is not a UUID: {value}') from None


def parse(raw_mount: str, cryptex_h: str, cryptex_cpp: str, block_cpp: str, volumes_h: str) -> dict:
    raw_text, h_text = Path(raw_mount).read_text(), Path(cryptex_h).read_text()
    cpp_text, block_text = Path(cryptex_cpp).read_text(), Path(block_cpp).read_text()
    volumes_text = Path(volumes_h).read_text()
    data = _uuid(_define(raw_text, 'SWITCH_RAW_DATA_UUID', raw_mount), 'SWITCH_RAW_DATA_UUID')
    preboot = _uuid(_define(raw_text, 'SWITCH_RAW_PREBOOT_UUID', raw_mount), 'SWITCH_RAW_PREBOOT_UUID')
    if preboot != _uuid(_define(h_text, 'SWITCH_PREBOOT_UUID', cryptex_h), 'SWITCH_PREBOOT_UUID'):
        raise SystemExit('SwitchRawMount.h and SwitchCryptex.h disagree on the Preboot UUID')
    paths = re.search(r'paths\[SWITCH_CRYPTEX_COUNT\]\s*=\s*\{([^}]*)\}', cpp_text)
    if not paths:
        raise SystemExit(f'{cryptex_cpp}: no cryptex paths table')
    roles = [name.split('_')[1].lower() for name in re.findall(r'SWITCH_(?:SYSTEM|APP)_CRYPTEX_IMAGE', paths.group(1))]
    if roles != ['system', 'app']:
        raise SystemExit(f'{cryptex_cpp}: the paths table is not SWITCH_SYSTEM_CRYPTEX_IMAGE, SWITCH_APP_CRYPTEX_IMAGE')
    cryptexes = []
    for role in roles:
        prefix = 'SWITCH_' + role.upper() + '_CRYPTEX_'
        image = _define(volumes_text, prefix + 'IMAGE', volumes_h).strip('"')
        if not image.startswith('/private/preboot/'):
            raise SystemExit(f'{volumes_h}: {prefix}IMAGE is not on Preboot: {image}')
        volume = _uuid(_define(volumes_text, prefix + 'UUID', volumes_h), prefix + 'UUID')
        cryptexes.append({'path': image, 'volume_uuid': volume, 'member': role + '_cryptex'})
    if not re.search(r'kRootUUID\[\]\s*=\s*SWITCH_OS_VOLUME_UUID\s*;', block_text):
        raise SystemExit(f'{block_cpp}: kRootUUID is not SWITCH_OS_VOLUME_UUID')
    system = _uuid(_define(volumes_text, 'SWITCH_OS_VOLUME_UUID', volumes_h), 'SWITCH_OS_VOLUME_UUID')
    apfs_type = re.search(r'kApfsType\[16\]\s*=\s*\{([^}]*)\}', block_text)
    if not apfs_type:
        raise SystemExit(f'{block_cpp}: no kApfsType')
    type_bytes = bytes(int(b, 16) for b in re.findall(r'0x([0-9a-fA-F]{2})', apfs_type.group(1)))
    if len(type_bytes) != 16:
        raise SystemExit(f'{block_cpp}: kApfsType is not 16 bytes')
    name = re.search(r'kPartitionName\[\]\s*=\s*"([^"]+)"', block_text)
    if not name:
        raise SystemExit(f'{block_cpp}: no kPartitionName')
    return {
        'partition_type': str(uuid.UUID(bytes_le=type_bytes)).upper(),
        'partition_name': name.group(1),
        'volumes': {
            'system': {'uuid': system, 'role': 0x0001},
            'data': {'uuid': data, 'role': 0x0040, 'name': 'SwitchDataTrial'},
            'preboot': {'uuid': preboot, 'role': 0x0010, 'name': 'SwitchPrebootTrial'},
        },
        'cryptexes': cryptexes,
        'sources': {
            k: Path(v).name
            for k, v in (
                ('raw_mount', raw_mount),
                ('cryptex_h', cryptex_h),
                ('cryptex_cpp', cryptex_cpp),
                ('block_cpp', block_cpp),
                ('volumes_h', volumes_h),
            )
        },
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    for name in ('raw-mount', 'cryptex-h', 'cryptex-cpp', 'block-cpp', 'volumes-h', 'out'):
        parser.add_argument('--' + name, required=True)
    args = parser.parse_args()
    spec = parse(args.raw_mount, args.cryptex_h, args.cryptex_cpp, args.block_cpp, args.volumes_h)
    Path(args.out).write_text(json.dumps(spec, indent=2, sort_keys=True) + '\n')


if __name__ == '__main__':
    main()
