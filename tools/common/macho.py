"""One reader of 64-bit little-endian ARM64 Mach-O images: header, load commands, segments, sections and symbols.

An image is macho()'s dict: {'kind': filetype, 'cpusubtype', 'commands': [(cmd, offset, size)], 'segments': [...]},
each segment {'name', 'va', 'size', 'fileoff', 'filesize', 'command_offset', 'maxprot', 'initprot', 'sections'} and
each section {'name', 'segment', 'va', 'size', 'fileoff', 'command_offset', 'reloff', 'nreloc', 'flags'}. Every
offset and size is checked against the image, so a reader past this one indexes nothing outside it. The native and
iOS 27 prelinkers read the kernel through it, so what it returns for a kernel must stay as it is.
"""

import re
import struct
from collections.abc import Callable, Collection, Iterable
from typing import NamedTuple

MH_MAGIC_64 = 0xFEEDFACF
CPU_TYPE_ARM64 = 0x100000C
MH_OBJECT, MH_EXECUTE, MH_DYLIB, MH_FILESET = 1, 2, 6, 12
LC_SYMTAB, LC_DYSYMTAB, LC_SEGMENT_64, LC_UUID, LC_CODE_SIGNATURE = 0x2, 0xB, 0x19, 0x1B, 0x1D
LC_FILESET_ENTRY = 0x80000035
N_STAB, N_TYPE, N_EXT, N_UNDF, N_SECT = 0xE0, 0x0E, 0x01, 0x00, 0x0E
NLIST = struct.Struct('<IBBHQ')


class Symbol(NamedTuple):
    name: str
    type: int
    sect: int
    desc: int
    value: int


def macho(data: bytes | bytearray, at: int = 0, subtypes: Collection[int] | None = (0,)) -> dict[str, object]:
    """Read the image whose header is at `at`; `subtypes` are the CPU subtypes it may have (None: any ARM64)."""
    if at < 0 or at + 32 > len(data):
        raise ValueError('Mach-O header outside image')
    magic, cpu, subtype, kind, count, size = struct.unpack_from('<6I', data, at)
    if magic != MH_MAGIC_64 or cpu != CPU_TYPE_ARM64 or (subtypes is not None and subtype not in subtypes):
        raise ValueError('Only ordinary little-endian ARM64 is supported')
    end = at + 32 + size
    if end > len(data):
        raise ValueError('Load commands outside image')
    offset = at + 32
    commands, segments = [], []
    for _ in range(count):
        if offset + 8 > end:
            raise ValueError('Truncated load command')
        command, length = struct.unpack_from('<II', data, offset)
        if length < 8 or length % 8 or offset + length > end:
            raise ValueError('Invalid load command extent')
        commands.append((command, offset, length))
        if command == LC_SEGMENT_64:
            segments.append(segment_command(data, offset, length))
        offset += length
    if offset != end:
        raise ValueError('Load command size mismatch')
    return {'kind': kind, 'cpusubtype': subtype, 'segments': segments, 'commands': commands}


def segment_command(data: bytes | bytearray, offset: int, length: int) -> dict[str, object]:
    if length < 72:
        raise ValueError('Truncated segment')
    name, va, size, fileoff, filesize, maxprot, initprot, nsects, _flags = struct.unpack_from(
        '<16s4Q4I', data, offset + 8
    )
    if length != 72 + 80 * nsects or filesize > size or va + size > 1 << 64:
        raise ValueError('Invalid segment extent')
    sections = []
    for index in range(nsects):
        section_at = offset + 72 + index * 80
        section_name, segment, address, extent, section_offset, _, reloff, nreloc, flags = struct.unpack_from(
            '<16s16sQQIIIII', data, section_at
        )
        if extent and not va <= address <= address + extent <= va + size:
            raise ValueError('Section outside segment')
        sections.append(
            {
                'name': section_name.rstrip(b'\0').decode(),
                'segment': segment.rstrip(b'\0').decode(),
                'va': address,
                'size': extent,
                'fileoff': section_offset,
                'command_offset': section_at,
                'reloff': reloff,
                'nreloc': nreloc,
                'flags': flags,
            }
        )
    return {
        'name': name.rstrip(b'\0').decode(),
        'va': va,
        'size': size,
        'fileoff': fileoff,
        'filesize': filesize,
        'command_offset': offset,
        'maxprot': maxprot,
        'initprot': initprot,
        'sections': sections,
    }


def arm64_kind(data: bytes | bytearray) -> int | None:
    """Return the filetype of an ordinary ARM64 (subtype 0) image, None for anything else."""
    try:
        return macho(data)['kind']
    except ValueError:
        return None


def backed_offset(segments: Iterable[dict[str, object]], address: int, length: int, data_length: int) -> int:
    """Return the file offset of [address, address + length), which exactly one segment's file bytes must hold."""
    matches = [
        s['fileoff'] + address - s['va']
        for s in segments
        if s['va'] <= address and address + length <= s['va'] + s['filesize']
    ]
    if len(matches) != 1 or matches[0] + length > data_length:
        raise ValueError(f'Ambiguous or unbacked address {address:#x}+{length:#x}')
    return matches[0]


def commands_of(image: dict[str, object], command: int) -> list[tuple[int, int]]:
    """Return the (offset, size) of every load command of kind `command`."""
    return [(offset, length) for kind, offset, length in image['commands'] if kind == command]


def defined_external(kind: int) -> bool:
    """Return whether an n_type is an external definition (in a section, absolute or indirect: any type but N_UNDF)."""
    return not kind & N_STAB and bool(kind & N_EXT) and bool(kind & N_TYPE)


def undefined_external(kind: int) -> bool:
    return not kind & N_STAB and bool(kind & N_EXT) and kind & N_TYPE == N_UNDF


def symbols(
    data: bytes | bytearray, image: dict[str, object], kinds: Callable[[int], bool] | None = None
) -> list[Symbol]:
    """Return the image's one symbol table in order; with `kinds`, only the symbols whose n_type it takes.

    Only the names of the symbols taken are read: a kernel holds ten times as many local symbols as exports.
    """
    tables = commands_of(image, LC_SYMTAB)
    if len(tables) != 1 or tables[0][1] != 24:
        raise ValueError('Expected one symbol table')
    symoff, count, stroff, strsize = struct.unpack_from('<4I', data, tables[0][0] + 8)
    if symoff + count * NLIST.size > len(data) or stroff + strsize > len(data):
        raise ValueError('Symbol table outside image')
    strings = bytes(data[stroff : stroff + strsize])
    if kinds is None:
        taken = range(count)
    else:
        # n_type is byte 4 of every 16-byte entry: the regex engine picks the wanted ones out of that column.
        wanted = bytes(k for k in range(256) if kinds(k))
        column = bytes(data[symoff + 4 : symoff + count * NLIST.size : NLIST.size])
        taken = (m.start() for m in re.finditer(b'[' + re.escape(wanted) + b']', column)) if wanted else ()
    out = []
    for index in taken:
        strx, kind, sect, desc, value = NLIST.unpack_from(data, symoff + index * NLIST.size)
        end = strings.find(b'\0', strx)
        if strx >= strsize or end < 0:
            raise ValueError('Unterminated symbol name')
        out.append(Symbol(strings[strx:end].decode(), kind, sect, desc, value))
    return out


def exports(data: bytes | bytearray, image: dict[str, object]) -> dict[int, list[str]]:
    """Return the names of the image's defined external symbols by address, in symbol table order."""
    result = {}
    for symbol in symbols(data, image, defined_external):
        result.setdefault(symbol.value, []).append(symbol.name)
    return result


def uuid_offset(image: dict[str, object]) -> int | None:
    """Return the file offset of the LC_UUID's 16 bytes, None for an image without one."""
    found = commands_of(image, LC_UUID)
    return found[0][0] + 8 if found else None


def fileset_entries(data: bytes | bytearray, image: dict[str, object]) -> list[tuple[str, int, int]]:
    """Return a kernel cache's (entry id, vmaddr, fileoff) in load command order."""
    entries = []
    for offset, length in commands_of(image, LC_FILESET_ENTRY):
        va, fileoff, name_at = struct.unpack_from('<QQI', data, offset + 8)
        entries.append((bytes(data[offset + name_at : offset + length]).split(b'\0')[0].decode(), va, fileoff))
    return entries
