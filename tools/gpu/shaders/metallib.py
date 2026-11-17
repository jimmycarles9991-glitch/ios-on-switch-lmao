"""Metal libraries (MTLB): their functions, each function's type and its declared function constants."""

import struct
from typing import NamedTuple

MAGIC = b'MTLB'
FAT_MAGIC = b'\xca\xfe\xba\xbe'
HEADER = 0x58

VERTEX, FRAGMENT, KERNEL = 0, 1, 2


class Constant(NamedTuple):
    """A function constant as the library declares it."""

    name: str
    type: int
    index: int
    required: bool


class Function(NamedTuple):
    """One function of a library."""

    name: str
    type: int
    constants: tuple[Constant, ...]
    air: str = ''

    def required(self) -> tuple[Constant, ...]:
        """Return the constants a specialization must give a value."""
        return tuple(c for c in self.constants if c.required)


def slices(data: bytes) -> list[tuple[int, bytes]]:
    """Return every MTLB slice of `data` with its cpusubtype: a fat file's, in file order, or a thin one as subtype 0.

    A fat file can hold several AIR64 slices that differ in their AIR version (iOS 27's CoreImage.metallib: AIR 2.4 and
    AIR 2.9), so the first is not all of it.
    """
    if data[:4] == FAT_MAGIC and len(data) >= 8:
        (count,) = struct.unpack_from('>I', data, 4)
        found = []
        for i in range(min(count, (len(data) - 8) // 20)):
            _, subtype, offset, size, _ = struct.unpack_from('>5I', data, 8 + 20 * i)
            if data[offset : offset + 4] == MAGIC:
                found.append((subtype, data[offset : offset + size]))
        return found
    return [(0, data)] if data[:4] == MAGIC and len(data) >= HEADER else []


def thin(data: bytes) -> bytes | None:
    """Return the MTLB bytes of `data`: itself, or the first MTLB slice of a fat file (None: neither)."""
    found = slices(data)
    return found[0][1] if found else None


def _tags(data: bytes, at: int, end: int) -> dict[str, bytes]:
    found = {}
    while at + 6 <= end and data[at : at + 4] != b'ENDT':
        (length,) = struct.unpack_from('<H', data, at + 4)
        found[data[at : at + 4].decode('latin-1')] = data[at + 6 : at + 6 + length]
        at += 6 + length
    return found


def _constants(cnst: bytes) -> tuple[Constant, ...]:
    """CNST: u16 count, then per constant its NUL-terminated name, u8 MTLDataType, u16 index, u8 required."""
    (count,) = struct.unpack_from('<H', cnst, 0)
    at, out = 2, []
    for _ in range(count):
        end = cnst.index(b'\0', at)
        (index,) = struct.unpack_from('<H', cnst, end + 2)
        out.append(Constant(cnst[at:end].decode('utf-8', 'replace'), cnst[end + 1], index, bool(cnst[end + 4])))
        at = end + 5
    return tuple(out)


def functions(data: bytes) -> list[Function] | None:
    """Return every function of the library `data` (fat or thin), or None when it is not a Metal library."""
    data = thin(data)
    if data is None:
        return None
    listing, _, public, _ = struct.unpack_from('<4Q', data, 0x18)
    if listing + 4 > len(data):
        return None
    (count,) = struct.unpack_from('<I', data, listing)
    at, out = listing + 4, []
    for _ in range(count):
        (size,) = struct.unpack_from('<I', data, at)
        entry = _tags(data, at + 4, at + size)
        at += size
        if 'NAME' not in entry or 'TYPE' not in entry:
            return None
        constants = ()
        if 'OFFT' in entry:
            start = public + struct.unpack_from('<Q', entry['OFFT'])[0]
            (length,) = struct.unpack_from('<I', data, start)
            cnst = _tags(data, start + 4, start + 4 + length).get('CNST')
            constants = _constants(cnst) if cnst else ()
        name = entry['NAME'].split(b'\0')[0].decode('utf-8', 'replace')
        out.append(Function(name, entry['TYPE'][0], constants, _versions(entry.get('VERS'))))
    return out


def _versions(vers: bytes | None) -> str:
    """VERS: the AIR version and the Metal language version, u16 major and minor each."""
    if not vers or len(vers) < 8:
        return ''
    air_major, air_minor, msl_major, msl_minor = struct.unpack_from('<4H', vers)
    return f'air {air_major}.{air_minor} msl {msl_major}.{msl_minor}'
