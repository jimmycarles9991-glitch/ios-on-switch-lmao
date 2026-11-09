"""A FlatBuffers table read without its schema: the fields its vtable lists, by number, typed as the caller says."""

import struct


def u32(data: bytes, at: int) -> int:
    return struct.unpack_from('<I', data, at)[0]


class Table:
    """One table of a FlatBuffers buffer: field `i` is absent, a little-endian scalar, or an offset to what it names."""

    def __init__(self, data: bytes, at: int) -> None:
        """Read the table at `at` (its signed offset back to its vtable first)."""
        self.data, self.at = data, at
        vtable = at - struct.unpack_from('<i', data, at)[0]
        size, self.inline = struct.unpack_from('<2H', data, vtable)
        self.offsets = struct.unpack_from(f'<{(size - 4) // 2}H', data, vtable + 4)

    @classmethod
    def root(cls, data: bytes) -> 'Table':
        return cls(data, u32(data, 0))

    def has(self, i: int) -> bool:
        return i < len(self.offsets) and self.offsets[i] != 0

    def fields(self) -> list[int]:
        return [i for i, o in enumerate(self.offsets) if o]

    def scalar(self, i: int, width: int, default: int = 0) -> int:
        if not self.has(i):
            return default
        at = self.at + self.offsets[i]
        return int.from_bytes(self.data[at : at + width], 'little')

    def target(self, i: int) -> int:
        """Where field `i`'s offset points."""
        at = self.at + self.offsets[i]
        return at + u32(self.data, at)

    def string(self, i: int) -> str | None:
        if not self.has(i):
            return None
        at = self.target(i)
        return self.data[at + 4 : at + 4 + u32(self.data, at)].decode('utf-8', 'replace')

    def table(self, i: int) -> 'Table | None':
        return Table(self.data, self.target(i)) if self.has(i) else None

    def tables(self, i: int) -> list['Table']:
        """Give field `i` as a vector of tables, in order."""
        return [Table(self.data, at) for at in self._elements(i)]

    def strings(self, i: int) -> list[str]:
        """Give field `i` as a vector of strings, in order."""
        return [self.data[at + 4 : at + 4 + u32(self.data, at)].decode('utf-8', 'replace') for at in self._elements(i)]

    def _elements(self, i: int) -> list[int]:
        if not self.has(i):
            return []
        at = self.target(i)
        slots = (at + 4 + 4 * k for k in range(u32(self.data, at)))
        return [slot + u32(self.data, slot) for slot in slots]
