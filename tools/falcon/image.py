"""NVIDIA falcon firmware images with a v1 bin header and OS header (L4T's nvhost flcn layout).

The OS (code at image offset 0) loads each application's code itself; falcon code addresses are image offsets.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass


@dataclass(frozen=True)
class Region:
    offset: int
    size: int

    @property
    def end(self) -> int:
        return self.offset + self.size


@dataclass(frozen=True)
class Image:
    ucode: bytes  # the falcon image: code address = offset into it
    os_code: Region
    os_data: Region
    app_code: tuple[Region, ...]
    app_data: tuple[Region, ...]
    overlay: Region


def parse(blob: bytes) -> Image:
    magic, version, _size, header, data, data_size = struct.unpack_from('<6I', blob, 0)
    if magic != 0x10DE or version != 1:
        raise ValueError('not a v1 falcon bin header')
    ucode = blob[data : data + data_size]
    code_off, code_size, os_data_off, os_data_size, apps = struct.unpack_from('<5I', blob, header)
    table = struct.unpack_from(f'<{4 * apps + 2}I', blob, header + 20)
    pairs = [Region(table[2 * i], table[2 * i + 1]) for i in range(2 * apps)]
    return Image(
        ucode=ucode,
        os_code=Region(code_off, code_size),
        os_data=Region(os_data_off, os_data_size),
        app_code=tuple(pairs[:apps]),
        app_data=tuple(pairs[apps:]),
        overlay=Region(table[4 * apps], table[4 * apps + 1]),
    )
