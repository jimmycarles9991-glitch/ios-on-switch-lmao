"""im4p.py IN OUT [--expect-type T] [--manifest-digest H] [--lzfse] [--thin-arm64] [--sha256 H]: an IM4P's payload.

Optionally LZFSE-decoded (macOS's compression_tool, a declared host tool) and reduced to its arm64 slice, as
vendor/unixnu's ipsw.py kernelcache() made kernelcache.decompressed; checked against a pin when one is given.
--manifest-digest is the IPSW BuildManifest's Digest of the component: the SHA-384 of the whole IM4P.
"""

import argparse
import hashlib
import struct
import subprocess
import sys
import tempfile
from pathlib import Path


def der(data: bytes, at: int) -> tuple[int, int, int]:
    tag, length, at = data[at], data[at + 1], at + 2
    if length & 0x80:
        count = length & 0x7F
        length, at = int.from_bytes(data[at : at + count], 'big'), at + count
    return tag, at, length


def im4p_fields(data: bytes) -> list[tuple[int, bytes]]:
    _, at, length = der(data, 0)
    end, fields = at + length, []
    while at < end:
        tag, start, length = der(data, at)
        fields.append((tag, data[start : start + length]))
        at = start + length
    if not fields or fields[0][1] != b'IM4P':
        sys.exit('not an IM4P')
    return fields


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument('source')
    parser.add_argument('output')
    parser.add_argument('--expect-type')
    parser.add_argument('--manifest-digest')
    parser.add_argument('--lzfse', action='store_true')
    parser.add_argument('--thin-arm64', action='store_true')
    parser.add_argument('--sha256')
    args = parser.parse_args()
    source = Path(args.source).read_bytes()
    if args.manifest_digest and hashlib.sha384(source).hexdigest() != args.manifest_digest:
        sys.exit(f"{args.source}: SHA-384 {hashlib.sha384(source).hexdigest()} is not the BuildManifest's digest")
    fields = im4p_fields(source)
    kind = fields[1][1].decode()
    if args.expect_type and kind != args.expect_type:
        sys.exit(f'IM4P type {kind} is not {args.expect_type}')
    payload = next(value for tag, value in fields[1:] if tag == 0x04)
    if args.lzfse:
        if payload[:4] not in (b'bvx2', b'bvx1', b'bvxn', b'bvx-'):
            sys.exit('payload is not LZFSE')
        with tempfile.TemporaryDirectory() as scratch:
            raw, out = Path(scratch) / 'in.lzfse', Path(scratch) / 'out'
            raw.write_bytes(payload)
            subprocess.run(
                ['/usr/bin/compression_tool', '-decode', '-a', 'lzfse', '-i', str(raw), '-o', str(out)],
                check=True,
                env={'PATH': '/usr/bin:/bin'},
            )
            payload = out.read_bytes()
    if args.thin_arm64 and payload[:4] == b'\xca\xfe\xba\xbe':
        count = struct.unpack_from('>I', payload, 4)[0]
        for i in range(count):
            cpu, _, offset, size, _ = struct.unpack_from('>iiIII', payload, 8 + 20 * i)
            if cpu == 0x0100000C:
                payload = payload[offset : offset + size]
                break
        else:
            sys.exit('no arm64 slice')
    if args.sha256 and hashlib.sha256(payload).hexdigest() != args.sha256:
        sys.exit(f'{args.output}: sha256 {hashlib.sha256(payload).hexdigest()} is not the pinned {args.sha256}')
    Path(args.output).write_bytes(payload)


if __name__ == '__main__':
    main()
