"""A dyld shared cache read the way dyld maps it: every subcache's mappings, its images, and its pointers resolved.

Pointers in a cache's data are stored in the form its slide info names, not as addresses: version 2 (arm64) keeps
the address under a delta mask, version 3 (arm64e, iOS 15-17) a plain address or an offset from the cache's base
for an authenticated one, version 5 (arm64e, iOS 18 and later) an offset from the base either way. `pointer()`
gives the address whichever form the cache uses.

Layouts: dyld's dyld_cache_format.h (dyld_cache_header, dyld_cache_mapping_and_slide_info, dyld_cache_image_info,
dyld_cache_slide_info2/3/5) and <mach-o/loader.h>.
"""

import mmap
import struct
from dataclasses import dataclass
from pathlib import Path

MAGIC = b'dyld_v1'
SKIPPED_SUFFIXES = ('.symbols', '.map', '.atlas')

HEADER_MAPPINGS = 0x10
HEADER_MAPPINGS_WITH_SLIDE = 0x138
HEADER_IMAGES_OLD = 0x18
HEADER_IMAGES = 0x1C0
HEADER_OBJC_OPTS = 0x1D0
MAPPING_BYTES = 32
MAPPING_WITH_SLIDE_BYTES = 56
IMAGE_INFO_BYTES = 32

MH_MAGIC_64 = 0xFEEDFACF
LC_SEGMENT_64 = 0x19
SEGMENT_HEADER_BYTES = 72
SECTION_BYTES = 80

SLIDE_V3_PLAIN_MASK = (1 << 51) - 1
SLIDE_V3_HIGH8 = 0x0007F80000000000
SLIDE_V3_LOW43 = 0x000007FFFFFFFFFF
SLIDE_V5_OFFSET_MASK = (1 << 34) - 1
AUTHENTICATED = 1 << 63


class CacheError(Exception):
    pass


def decode_pointer(raw: int, version: int, info: tuple[int, int]) -> int:
    """Return the address a cache pointer in slide info `version`'s form holds.

    `info` is version 2's delta mask and value add, or version 3's and 5's base.
    """
    if not raw:
        return 0
    if version == 2:
        delta_mask, value_add = info
        value = raw & ~delta_mask
        return value + value_add if value else 0
    if version == 3:
        if raw & AUTHENTICATED:
            return info[0] + (raw & 0xFFFFFFFF)
        value = raw & SLIDE_V3_PLAIN_MASK
        return ((value & SLIDE_V3_HIGH8) << 13) | (value & SLIDE_V3_LOW43)
    if version == 5:
        return info[0] + (raw & SLIDE_V5_OFFSET_MASK)
    return raw


@dataclass(frozen=True)
class Section:
    segment: str
    name: str
    address: int
    size: int


@dataclass(frozen=True)
class _Mapping:
    address: int
    end: int
    file_offset: int
    data: mmap.mmap


class DyldCache:
    """The cache whose main file is `main`, with every subcache beside it that maps memory."""

    def __init__(self, main: Path):
        main = Path(main)
        self._files = []
        self._mappings: list[_Mapping] = []
        self._slide_version = 0
        self._slide_info: tuple[int, int] = (0, 0)
        for path in sorted(main.parent.glob(main.name + '*')):
            if path != main and (not path.name.startswith(main.name + '.') or path.name.endswith(SKIPPED_SUFFIXES)):
                continue
            self._add_file(path)
        self._main = self._files[0] if self._files and self._files[0][0] == main else None
        if self._main is None:
            raise CacheError(f'{main} is not a dyld shared cache')
        self._mappings.sort(key=lambda m: m.address)
        self.images = self._read_images(self._main[1])
        objc_offset, objc_size = struct.unpack_from('<2Q', self._main[1], HEADER_OBJC_OPTS)
        self.objc_opts = self.base + objc_offset if objc_size else 0

    def _add_file(self, path: Path) -> None:
        with path.open('rb') as f:
            data = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
        if not data[:16].startswith(MAGIC):
            return
        self._files.append((path, data))
        offset, count = struct.unpack_from('<2I', data, HEADER_MAPPINGS_WITH_SLIDE)
        for i in range(count):
            address, size, file_offset, slide_offset, slide_size = struct.unpack_from(
                '<5Q', data, offset + MAPPING_WITH_SLIDE_BYTES * i
            )
            self._mappings.append(_Mapping(address, address + size, file_offset, data))
            if slide_size:
                self._note_slide(data, slide_offset)
        if not count:
            offset, count = struct.unpack_from('<2I', data, HEADER_MAPPINGS)
            for i in range(count):
                address, size, file_offset = struct.unpack_from('<3Q', data, offset + MAPPING_BYTES * i)
                self._mappings.append(_Mapping(address, address + size, file_offset, data))

    def _note_slide(self, data: mmap.mmap, offset: int) -> None:
        version = struct.unpack_from('<I', data, offset)[0]
        if version == 2:
            info = struct.unpack_from('<2Q', data, offset + 24)
        elif version in (3, 5):
            info = (struct.unpack_from('<Q', data, offset + 16)[0], 0)
        else:
            raise CacheError(f'slide info version {version}')
        if self._slide_version and (self._slide_version, self._slide_info) != (version, info):
            raise CacheError('subcaches disagree on their slide info')
        self._slide_version, self._slide_info = version, info

    def _read_images(self, header: mmap.mmap) -> dict[str, int]:
        mappings_at = struct.unpack_from('<I', header, HEADER_MAPPINGS)[0]
        offset, count = struct.unpack_from('<2I', header, HEADER_IMAGES)
        if mappings_at <= HEADER_IMAGES or not count:
            offset, count = struct.unpack_from('<2I', header, HEADER_IMAGES_OLD)
        images = {}
        for i in range(count):
            address, _, _, path_offset = struct.unpack_from('<3QI', header, offset + IMAGE_INFO_BYTES * i)
            images[header[path_offset : header.find(b'\0', path_offset)].decode()] = address
        return images

    @property
    def base(self) -> int:
        return self._mappings[0].address

    def read(self, address: int, size: int) -> bytes:
        for m in self._mappings:
            if m.address <= address and address + size <= m.end:
                at = m.file_offset + address - m.address
                return m.data[at : at + size]
        raise CacheError(f'0x{address:x}+{size} is not mapped')

    def u32(self, address: int) -> int:
        return struct.unpack('<I', self.read(address, 4))[0]

    def i32(self, address: int) -> int:
        return struct.unpack('<i', self.read(address, 4))[0]

    def u64(self, address: int) -> int:
        return struct.unpack('<Q', self.read(address, 8))[0]

    def pointer(self, address: int) -> int:
        """Return the address the pointer stored at `address` points at (0 for a null pointer)."""
        return decode_pointer(self.u64(address), self._slide_version, self._slide_info)

    def cstring(self, address: int, limit: int = 4096) -> str:
        for m in self._mappings:
            if m.address <= address < m.end:
                at = m.file_offset + address - m.address
                end = m.data.find(b'\0', at, at + limit)
                if end < 0:
                    raise CacheError(f'no string at 0x{address:x}')
                return m.data[at:end].decode('utf-8', 'replace')
        raise CacheError(f'0x{address:x} is not mapped')

    def sections(self, image: str) -> list[Section]:
        """Return the sections of the image installed at `image` (its path in the cache)."""
        header = self.images[image]
        magic, _, _, _, ncmds, sizeofcmds = struct.unpack('<6I', self.read(header, 24))
        if magic != MH_MAGIC_64:
            raise CacheError(f'{image} is not a 64-bit Mach-O')
        commands = self.read(header + 32, sizeofcmds)
        found, at = [], 0
        for _ in range(ncmds):
            command, size = struct.unpack_from('<2I', commands, at)
            if command == LC_SEGMENT_64:
                count = struct.unpack_from('<I', commands, at + 64)[0]
                for i in range(count):
                    s = at + SEGMENT_HEADER_BYTES + SECTION_BYTES * i
                    name, segment = (commands[s + k : s + k + 16].rstrip(b'\0').decode() for k in (0, 16))
                    address, length = struct.unpack_from('<2Q', commands, s + 32)
                    found.append(Section(segment, name, address, length))
            at += size
        return found

    def section(self, image: str, name: str) -> Section | None:
        """Return the image's section `name` in whichever segment holds it (__DATA, __DATA_CONST, __AUTH_CONST...)."""
        return next((s for s in self.sections(image) if s.name == name), None)

    def pointers(self, section: Section | None) -> list[int]:
        if section is None:
            return []
        return [self.pointer(section.address + 8 * i) for i in range(section.size // 8)]
