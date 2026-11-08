"""A kernel Mach-O as the ABI check reads it.

Apple's kernel cache is an MH_FILESET whose pointers are chained fixups (DYLD_CHAINED_PTR_64_KERNEL_CACHE: the target
is the low 30 bits, an offset from the cache's first segment). Our prelinked kernel stores plain addresses.
"""

import re
from collections.abc import Iterator
from pathlib import Path

from tools.common import macho


class KernelImage:
    """One kernel image: its segments, its fileset entries (Apple's), and its pointers decoded."""

    def __init__(self, path: str) -> None:
        self.d = Path(path).read_bytes()
        image = macho.macho(self.d, subtypes=None)
        self.fileset = image['kind'] == macho.MH_FILESET
        self.segs = image['segments']
        self.base = self.segs[0]['va']
        self._owners = [
            (entry, seg['va'], seg['va'] + seg['size'])
            for entry, _, fileoff in macho.fileset_entries(self.d, image)
            for seg in macho.macho(self.d, fileoff, subtypes=None)['segments']
        ]

    def va2off(self, address: int) -> int | None:
        """Return the file offset of address, None outside every segment's file bytes."""
        for s in self.segs:
            if s['va'] <= address < s['va'] + s['filesize']:
                return s['fileoff'] + address - s['va']
        return None

    def read(self, address: int, size: int) -> bytes:
        """Return size bytes at address."""
        offset = self.va2off(address)
        if offset is None:
            raise KeyError(f'{address:#x} is not in the image')
        return self.d[offset : offset + size]

    def pointer(self, raw: int) -> int:
        """Return the address a stored pointer names."""
        if raw == 0 or not self.fileset or raw >> 40 == 0xFFFFFF:
            return raw
        return self.base + (raw & 0x3FFFFFFF)

    def cstring(self, address: int) -> str | None:
        """Return the identifier-like C string at address, or None."""
        offset = self.va2off(address)
        if offset is None:
            return None
        end = self.d.find(b'\0', offset, offset + 256)
        text = self.d[offset:end] if end > offset else b''
        return text.decode() if re.fullmatch(rb'[\w.\-+]+', text) else None

    def is_text(self, address: int) -> bool:
        """Return whether address is in a code segment."""
        return any('TEXT' in s['name'] and s['va'] <= address < s['va'] + s['size'] for s in self.segs)

    def data_segments(self) -> Iterator[tuple[int, bytes]]:
        """Yield (vmaddr, bytes) of every segment that holds data."""
        for s in self.segs:
            if 'DATA' in s['name']:
                yield s['va'], self.d[s['fileoff'] : s['fileoff'] + s['filesize']]

    def owner(self, address: int) -> str | None:
        """Return the fileset entry holding address ('kernel' throughout a kernel that is not a fileset)."""
        if not self.fileset:
            return 'kernel'
        return next((entry for entry, start, end in self._owners if start <= address < end), None)

    def prelinked_bundles(self) -> set[str]:
        """Return the bundle identifiers in __PRELINK_INFO."""
        for s in self.segs:
            if s['name'] == '__PRELINK_INFO':
                info = self.d[s['fileoff'] : s['fileoff'] + s['filesize']]
                pattern = rb'<key>CFBundleIdentifier</key>\s*<string>([^<]+)</string>'
                return {m.decode() for m in re.findall(pattern, info)}
        return set()
