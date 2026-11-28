#!/usr/bin/env python3
import argparse
import lzma
import struct
from collections.abc import Iterator
from pathlib import Path


def u32(data: bytes | memoryview, offset: int) -> int:
    return struct.unpack_from('<I', data, offset)[0]


def programs(blob: bytes) -> Iterator[tuple[str, bytes, bytes, bytes]]:
    if len(blob) < 36 or struct.unpack_from('<4I', blob) != (32, 1, 0x10DE, 0x92BA03D7):
        raise ValueError('expected the Tegra X1 R32 Vulkan cache')
    if blob[16:32] != bytes.fromhex('5e07f2064ae533f3b9244773dd0e3a1c'):
        raise ValueError('unknown R32 pipeline cache UUID')
    cursor = 36
    for _ in range(u32(blob, 32)):
        header = blob[cursor : cursor + 64]
        if len(header) != 64 or header[24:28] != b'CPKV' or header[:16] != header[32:48]:
            raise ValueError('invalid R32 cache entry')
        size = u32(header, 16)
        if size != u32(header, 56):
            raise ValueError('unrecognized R32 compression header')
        cursor += 64
        payload = blob[cursor : cursor + size]
        if len(payload) != size or size < 9:
            raise ValueError('truncated compressed entry')
        length, props, dictionary = struct.unpack_from('<IBI', payload)
        if length > 64 << 20 or dictionary > 64 << 20 or props >= 225:
            raise ValueError('invalid LZMA properties')
        pb, rem = divmod(props, 45)
        lp, lc = divmod(rem, 9)
        filt = {'id': lzma.FILTER_LZMA1, 'dict_size': dictionary, 'lc': lc, 'lp': lp, 'pb': pb}
        decoder = lzma.LZMADecompressor(lzma.FORMAT_RAW, filters=[filt])
        data = decoder.decompress(payload[9:], max_length=length)
        if len(data) != length or data[:4] != b'NVVM' or data[12:16] != b'NVuc':
            raise ValueError('invalid NVuc payload')
        yield sections_of(data)
        cursor += size
    if cursor != len(blob):
        raise ValueError('unexpected bytes after the last cache entry')


def sections_of(data: bytes) -> tuple[str, bytes, bytes, bytes]:
    uc = memoryview(data)[12:]
    count = struct.unpack_from('<H', uc, 8)[0]
    end = u32(uc, 24)
    if end > len(uc) or 32 + 32 * count > end:
        raise ValueError('invalid NVuc section directory')
    sections = {}
    for index in range(count):
        tag, size_bytes, start = struct.unpack_from('<IIQ', uc, 32 + 32 * index)
        if start + size_bytes > end:
            raise ValueError('NVuc section exceeds record')
        if tag in (1, 0x10, 0x2D):
            if tag in sections:
                raise ValueError('duplicate program section')
            sections[tag] = bytes(uc[start : start + size_bytes])
    sph, code = sections[0x2D], sections[1]
    if len(sph) != 80 or not code or len(code) % 32:
        raise ValueError('invalid Maxwell graphics program')
    stage = {1: 'vert', 5: 'frag'}[(u32(sph, 0) >> 10) & 15]
    return stage, sph, code, sections.get(0x10, b'')


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument('cache', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    decoded = list(programs(args.cache.read_bytes()))
    if sorted(item[0] for item in decoded) != ['frag', 'vert']:
        raise ValueError('expected one vertex and one fragment program')
    args.output.mkdir(parents=True, exist_ok=True)
    for stage, sph, code, literals in decoded:
        (args.output / (stage + '.sph')).write_bytes(sph)
        (args.output / (stage + '.sass')).write_bytes(code)
        (args.output / (stage + '.constants')).write_bytes(literals)
        print(f'{stage}: {len(sph)} header bytes, {len(code)} code bytes')


if __name__ == '__main__':
    main()
