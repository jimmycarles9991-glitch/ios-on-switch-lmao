#!/usr/bin/env python3
"""Build a .logarchive the real /usr/bin/log can read: diagnostics + uuidtext merged, Info.plist from timesync boots.

    mkarchive.py <device-log dir with diagnostics/ and uuidtext/> <out.logarchive>

(Was out/switch/device-logs/mkarchive.py; `/usr/bin/log show --archive <out> --style compact --info --debug`.)
"""

import plistlib
import shutil
import struct
import sys
import uuid
from collections.abc import Callable
from pathlib import Path


def build(src: str | Path, out: str | Path, copy: Callable = shutil.copy2) -> list[tuple[uuid.UUID, int]]:
    """Make <out> from <src>'s diagnostics/ and uuidtext/, each file put there by `copy` (os.link: no bytes copied)."""
    src, out = Path(src), Path(out)
    if out.exists():
        shutil.rmtree(out)
    shutil.copytree(src / 'diagnostics', out, copy_function=copy)
    # Both trees carry logd's .migrated marker; os.link cannot replace the one diagnostics/ already put there.
    shutil.copytree(
        src / 'uuidtext', out, dirs_exist_ok=True, copy_function=copy, ignore=shutil.ignore_patterns('.migrated')
    )
    boots = []  # (uuid, wall ns) in timesync order
    for f in sorted((out / 'timesync').glob('*.timesync')):
        d = f.read_bytes()
        pos = 0
        while pos + 4 <= len(d):
            sig, size = struct.unpack_from('<HH', d, pos)
            if sig == 0xBBB0:  # boot record, 48 bytes: sig, size, unk, uuid, numer, denom, wall ns, tz, dst
                u = uuid.UUID(bytes=d[pos + 8 : pos + 24])
                wall = struct.unpack_from('<q', d, pos + 32)[0]
                if not boots or boots[-1][0] != u:
                    boots.append((u, wall))
                pos += size
            elif d[pos : pos + 4] == b'Ts \x00':  # sync record, 32 bytes
                pos += 32
            else:
                raise SystemExit(f'{f.name}: unknown record at {pos:#x}: {d[pos : pos + 8].hex()}')
    if not boots:
        raise SystemExit(f'{src}: no boot records in timesync/')
    first, last = boots[0], boots[-1]
    oldest = {'ContinuousTime': 0, 'UUID': str(first[0]).upper(), 'WallClockNanos': first[1]}
    info = {
        'ArchiveIdentifier': str(uuid.uuid4()).upper(),
        'OSArchiveVersion': 4,
        'OldestTimeRef': oldest,
        'EndTimeRef': {
            'ContinuousTime': 3600 * 10**9,
            'UUID': str(last[0]).upper(),
            'WallClockNanos': last[1] + 3600 * 10**9,
        },
    }
    for kind in ('HighVolume', 'Live', 'Persist', 'Signpost', 'Special', 'TTL'):
        info[f'{kind}Metadata'] = {'OldestTimeRef': oldest}
    (out / 'Info.plist').write_bytes(plistlib.dumps(info))
    return boots


if __name__ == '__main__':
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    for u, wall in build(sys.argv[1], sys.argv[2]):
        print(u, wall)
