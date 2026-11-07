"""The NVK host twin with the device's pipeline cache identity, as the seed and the raw pack both run it."""

import shutil
import struct
import subprocess
import sys
from pathlib import Path


def lc_uuid(path: str | Path, value: bytes | None = None) -> bytes:
    """Return the LC_UUID of the thin 64-bit Mach-O at `path`, after setting it to `value` (16 bytes) when given."""
    data = bytearray(Path(path).read_bytes())
    magic, _, _, _, ncmds = struct.unpack_from('<5I', data)
    at = 32
    for _ in range(ncmds if magic == 0xFEEDFACF else 0):
        cmd, size = struct.unpack_from('<2I', data, at)
        if cmd == 0x1B:  # LC_UUID
            if value is not None:
                data[at + 8 : at + 24] = value
                Path(path).write_bytes(data)
            return bytes(data[at + 8 : at + 24])
        at += size
    sys.exit(f'{path}: no LC_UUID')


def make_twin(nvk: str, twin: str, out: str, env: dict[str, str]) -> None:
    """Copy the host twin to `out` with the iOS dylib's LC_UUID, ad hoc signed.

    The two dylibs' .source files must be equal: the same Mesa tree and compiler identity, which NVK hashes into its
    pipeline cache UUID. The LC_UUID is how the seed's check (merge.py) tells the twin is the bundle's NVK's.
    """
    sources = [Path(str(p) + '.source') for p in (nvk, twin)]
    if not all(p.is_file() for p in sources) or sources[0].read_text() != sources[1].read_text():
        sys.exit(f"{sources[0]} and {sources[1]} missing or different: the twin is not of the iOS dylib's sources")
    shutil.copyfile(twin, out)
    Path(out).chmod(0o755)
    lc_uuid(out, lc_uuid(nvk))
    result = subprocess.run(
        ['/usr/bin/codesign', '--force', '--sign', '-', str(out)], env=env, capture_output=True, text=True, check=False
    )
    if result.returncode:
        sys.exit(f'codesign {out}: {result.stderr}')
