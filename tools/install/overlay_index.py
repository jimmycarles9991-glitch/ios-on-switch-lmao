"""Data:/private/var/Switch/pretranslated/.index: the kernel's index of iOS 27's translated System volume files.

The kernel shows each at its System path from Switch/pretranslated/<path> (bsd/vfs/vfs_switch_pretranslated.c reads the
index once, before launchd, through the tree it captured).

Little-endian: "SWOV", u32 version (2), u32 file count, u32 directory count, u32 string bytes, u32 0; then per file, in
path order, u64 the System file's id, u64 the Data file's id, u32 the offset of its path in the string table, u32 0,
20 bytes the translated file's CDHash; the file ids of the tree's directories (its root among them), ascending; the
string table, the files' NUL-terminated absolute System paths, sorted by bytes and unique, every byte belonging to one
path. The kernel finds a System file by its id and the Data file by its, so no path is resolved on the device; the
directory ids are what it refuses every change under, and a file keeps the System volume's launch status only if its
own CDHash equals the one its entry carries. A System file with several hard links has one entry per path, all with
its id, one Data file's id and one CDHash: the kernel redirects whichever name a lookup arrives by.
"""

import struct

MAGIC = b'SWOV'
VERSION = 2
NAME = '.index'
HEADER = struct.Struct('<4sIIIII')
FILE = struct.Struct('<QQII20s')
CDHASH = 20
MAX_PATH = 1024
MAX_FILES = 1 << 20
MAX_DIRECTORIES = 1 << 20


class OverlayIndexError(ValueError):
    """An index, or what would go into one, that the kernel refuses."""


def check_path(path: str) -> None:
    """Refuse a path the kernel would not take."""
    if (
        not path.startswith('/')
        or path.endswith('/')
        or '//' in path
        or any(part in ('.', '..') for part in path.split('/'))
        or '\0' in path
        or len(path.encode()) >= MAX_PATH
        or path == '/' + NAME
    ):
        raise OverlayIndexError(f'{path!r} is not a path the overlay index takes')


def _check(files: list[tuple[str, int, int, bytes]], directories: list[int]) -> None:
    for _, _, _, cdhash in files:
        if not isinstance(cdhash, (bytes, bytearray)) or len(cdhash) != CDHASH:
            raise OverlayIndexError('a CDHash that is not 20 bytes')
    if any(0 in (s, d) for _, s, d, _ in files) or 0 in directories:
        raise OverlayIndexError('a file id of 0')
    by_system, by_data = {}, {}
    for _, system, data, cdhash in files:
        if by_system.setdefault(system, (data, bytes(cdhash))) != (data, bytes(cdhash)):
            raise OverlayIndexError('hard links of one System file with different Data files or CDHashes')
        if by_data.setdefault(data, system) != system:
            raise OverlayIndexError('the overlay index names a Data file id twice')
    if len(set(directories)) != len(directories) or set(directories) & by_data.keys():
        raise OverlayIndexError('the overlay index names a directory id twice')


def encode(files: list[tuple[str, int, int, bytes]], directories: list[int]) -> bytes:
    """Return the index of files (System path, System file id, Data file id, CDHash) and the tree's directory ids."""
    files = sorted(files, key=lambda f: f[0].encode())
    paths = [f[0] for f in files]
    if len({p.encode() for p in paths}) != len(paths):
        raise OverlayIndexError('the overlay index lists a path twice')
    for p in paths:
        check_path(p)
    directories = sorted(directories)
    _check(files, directories)
    if len(files) > MAX_FILES or len(directories) > MAX_DIRECTORIES:
        raise OverlayIndexError('the overlay index is too large')
    records, strings = b'', b''
    for path, system, data, cdhash in files:
        records += FILE.pack(system, data, len(strings), 0, bytes(cdhash))
        strings += path.encode() + b'\0'
    return (
        HEADER.pack(MAGIC, VERSION, len(files), len(directories), len(strings), 0)
        + records
        + struct.pack(f'<{len(directories)}Q', *directories)
        + strings
    )


def decode(raw: bytes) -> tuple[list[tuple[str, int, int, bytes]], list[int]]:
    """Return (files, directories) as encode takes them, refusing anything the kernel would."""
    if len(raw) < HEADER.size:
        raise OverlayIndexError('short overlay index')
    magic, version, count, dirs, size, reserved = HEADER.unpack_from(raw)
    if magic != MAGIC or version != VERSION or reserved or count > MAX_FILES or dirs > MAX_DIRECTORIES:
        raise OverlayIndexError(f'not a version {VERSION} overlay index')
    if len(raw) != HEADER.size + FILE.size * count + 8 * dirs + size:
        raise OverlayIndexError('the overlay index is not the size its header says')
    records = [FILE.unpack_from(raw, HEADER.size + FILE.size * i) for i in range(count)]
    at = HEADER.size + FILE.size * count
    directories = list(struct.unpack_from(f'<{dirs}Q', raw, at))
    strings = raw[at + 8 * dirs :]
    files, end = [], 0
    for system, data, offset, pad, cdhash in records:
        stop = strings.find(b'\0', offset)
        if pad or offset != end or stop < 0:
            raise OverlayIndexError('the overlay index strings are not packed in order')
        files.append((strings[offset:stop].decode(), system, data, cdhash))
        end = stop + 1
    paths = [f[0] for f in files]
    if end != size or paths != sorted(set(paths), key=str.encode) or directories != sorted(set(directories)):
        raise OverlayIndexError('the overlay index is not sorted, unique and packed')
    for p in paths:
        check_path(p)
    _check(files, directories)
    return files, directories
