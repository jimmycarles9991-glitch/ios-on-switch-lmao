"""Apple GPU binaries: the Mach-O files (CPU_TYPE_APPLE_GPU | ABI64) Metal serializes binary archives as.

A binary archive is one such binary, or a slice of a universal file beside an AIR64 metallib (QuartzCore's
default.metallib). Its __TEXT,__descriptor holds one FlatBuffers buffer per pipeline (identifier AIRP: airp.py) or a
function list (AIRF); __TEXT,__metallib holds small metallibs, one per function the pipelines name: a library function
by its name and hash, and a specialized one under its own name with an SBUF tag whose AIRC buffer names the function it
specializes and the constant values it was specialized with.
"""

import struct
from typing import NamedTuple

from drivers.TegraGM20BMetal.corpus.tools.archives.flatbuf import Table, u32

APPLE_GPU = 0x01000013
MACHO = 0xFEEDFACF
FAT = b'\xca\xfe\xba\xbe'
LC_SEGMENT_64 = 0x19


class Specialization(NamedTuple):
    """A specialized function: the function it was made from and its constants (index, MTLDataType, value bytes)."""

    base: str
    constants: tuple[tuple[int, int, bytes], ...]


def slices(data: bytes) -> list[bytes]:
    """Give the Apple GPU binaries of a file: itself, or its universal file's Apple GPU slices."""
    if data[:4] == FAT:
        out = []
        for i in range(struct.unpack_from('>I', data, 4)[0]):
            cpu, _, offset, size, _ = struct.unpack_from('>5I', data, 8 + 20 * i)
            if cpu == APPLE_GPU:
                out.append(data[offset : offset + size])
        return out
    return [data] if is_gpu_binary(data) else []


def has_gpu_binary(head: bytes) -> bool:
    """Whether a file whose first bytes are `head` is an Apple GPU binary or a universal file with one."""
    if head[:4] == FAT:
        count = struct.unpack_from('>I', head, 4)[0]
        return 0 < count <= 64 and any(
            struct.unpack_from('>I', head, 8 + 20 * i)[0] == APPLE_GPU for i in range(count) if 12 + 20 * i <= len(head)
        )
    return is_gpu_binary(head)


def is_gpu_binary(data: bytes) -> bool:
    return len(data) >= 32 and struct.unpack_from('<2I', data) == (MACHO, APPLE_GPU)


def sections(binary: bytes) -> dict[str, bytes]:
    """Give a binary's sections by name ("__descriptor", "__metallib", ...)."""
    ncmds = struct.unpack_from('<I', binary, 16)[0]
    out, at = {}, 32
    for _ in range(ncmds):
        cmd, size = struct.unpack_from('<2I', binary, at)
        if cmd == LC_SEGMENT_64:
            for i in range(struct.unpack_from('<I', binary, at + 64)[0]):
                header = at + 72 + 80 * i
                name = binary[header : header + 16].rstrip(b'\0').decode()
                _, length, offset = struct.unpack_from('<QQI', binary, header + 32)
                out[name] = binary[offset : offset + length]
        at += size
    return out


def buffers(section: bytes, identifier: bytes) -> list[bytes]:
    """Split a __descriptor section into its FlatBuffers buffers with `identifier`, each up to the next one."""
    starts = [at - 4 for at in _find_all(section, identifier) if at >= 4 and at % 4 == 0]
    ends = [*starts[1:], len(section)]
    return [section[a:b] for a, b in zip(starts, ends[: len(starts)], strict=True)]


def _find_all(data: bytes, needle: bytes) -> list[int]:
    out, at = [], data.find(needle)
    while at >= 0:
        out.append(at)
        at = data.find(needle, at + 1)
    return out


def metallibs(section: bytes) -> list[bytes]:
    """Give the metallibs packed one after another in a __metallib section."""
    out, at = [], 0
    while (at := section.find(b'MTLB', at)) >= 0:
        size = struct.unpack_from('<Q', section, at + 16)[0] if at + 24 <= len(section) else 0
        if 0x58 <= size <= len(section) - at:
            out.append(section[at : at + size])
            at += size
        else:
            at += 4
    return out


def entries(library: bytes) -> list[dict[str, bytes]]:
    """Give a metallib's function list entries: each tag (NAME, HASH, SPCI, ...) and its bytes."""
    listing = struct.unpack_from('<Q', library, 0x18)[0]
    at, out = listing + 4, []
    for _ in range(u32(library, listing)):
        size, tags, tag_at = u32(library, at), {}, at + 4
        while tag_at + 6 <= at + size and library[tag_at : tag_at + 4] != b'ENDT':
            length = struct.unpack_from('<H', library, tag_at + 4)[0]
            tags[library[tag_at : tag_at + 4].decode('latin-1')] = library[tag_at + 6 : tag_at + 6 + length]
            tag_at += 6 + length
        out.append(tags)
        at += size
    return out


def name_of(tags: dict[str, bytes]) -> str:
    return tags.get('NAME', tags.get('ALIS', b'')).split(b'\0')[0].decode('utf-8', 'replace')


def specializations(section: bytes) -> dict[str, Specialization]:
    """Give the specialized functions of a __metallib section by name, from each metallib's AIRC buffer.

    AIRC: root field 2 is {0: the base function, 1: the specialized name, 2: its constants}; a constant is {0: its
    index (u16), 1: its MTLDataType (u8), 2: an offset to its value's bytes, as many as the type's size}.
    """
    out = {}
    for library in metallibs(section):
        at = library.find(b'AIRC')
        if at < 4:
            continue
        buffer = library[at - 4 :]
        body = Table.root(buffer).table(2)
        if body is None or not body.has(1):
            continue
        constants = []
        for c in body.tables(2):
            kind = c.scalar(1, 1)
            value = buffer[c.target(2) : c.target(2) + type_width(kind, 16)] if c.has(2) else b''
            constants.append((c.scalar(0, 2), kind, value))
        out[body.string(1)] = Specialization(body.string(0) or '', tuple(constants))
    return out


def function_hashes(section: bytes) -> dict[str, bytes]:
    """Give the hash of every function a __metallib section names (a library function's is its library's)."""
    out = {}
    for library in metallibs(section):
        for tags in entries(library):
            if 'HASH' in tags and 'SPCI' not in tags:
                out.setdefault(name_of(tags), tags['HASH'])
            if 'SPCI' in tags and len(tags['SPCI']) >= 33:
                out.setdefault('\0' + name_of(tags), tags['SPCI'][1:33])
    return out


# MTLDataType sizes, as the frontend journals constants (NativeBridgeMetalFrontend.mm constantBytes); 0 for the types it
# writes no bytes of
_WIDTHS = {
    **dict.fromkeys((45, 49, 53), 1),
    **dict.fromkeys((16, 37, 41, 46, 50, 54), 2),
    **dict.fromkeys((3, 17, 29, 33, 38, 42, 48, 52, 56), 4),
    **dict.fromkeys((4, 19, 30, 34, 40, 44, 81, 85), 8),
    **dict.fromkeys((6, 32, 36, 82, 86), 16),
    **dict.fromkeys((84, 88), 32),
}


def type_width(kind: int, otherwise: int = 0) -> int:
    return _WIDTHS.get(kind, otherwise)
