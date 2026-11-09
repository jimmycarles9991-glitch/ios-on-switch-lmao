"""Version-2 trust cache modules, as the kernel's trust cache loader reads them.

One copy for every tool that builds one. Entries are sorted by CDHash with no CDHash twice: the kernel
binary-searches the module and refuses one that is unsorted or repeats an entry.
"""

import hashlib
import itertools
import struct
from collections.abc import Iterable

VERSION = 2
HASH_TYPE_SHA256 = 2
CDHASH_BYTES = 20
UUID_BYTES = 16

type Entry = bytes | tuple[bytes, int, int]


def trust_module(entries: Iterable[Entry], uuid: bytes | None = None) -> bytes:
    """Build the module for `entries`.

    An entry is a CDHash (20 bytes; flags and launch-constraint category 0) or a (CDHash, flags, category) tuple.
    The same CDHash may come more than once with the same flags and category; with different ones it is an error,
    since only one entry can stand. `uuid` (16 bytes) defaults to the first 16 bytes of the entries' SHA-256.
    """
    unique = {}
    for entry in entries:
        cdhash, flags, category = (entry, 0, 0) if isinstance(entry, bytes | bytearray) else entry
        cdhash = bytes(cdhash)
        if len(cdhash) != CDHASH_BYTES:
            raise ValueError(f'trust cache: CDHash {cdhash.hex()} is {len(cdhash)} bytes, not {CDHASH_BYTES}')
        if unique.setdefault(cdhash, (flags, category)) != (flags, category):
            raise ValueError(
                f'trust cache: {cdhash.hex()} given with flags/category {unique[cdhash]} and {(flags, category)}'
            )
    body = b''.join(
        cdhash + bytes([HASH_TYPE_SHA256, flags, category, 0]) for cdhash, (flags, category) in sorted(unique.items())
    )
    if uuid is None:
        uuid = hashlib.sha256(body).digest()[:UUID_BYTES]
    if len(uuid) != UUID_BYTES:
        raise ValueError(f'trust cache: UUID of {len(uuid)} bytes')
    return struct.pack('<I16sI', VERSION, bytes(uuid), len(unique)) + body


ENTRY_BYTES = 24
HEADER_BYTES = 24


def module_entries(module: bytes) -> list[bytes]:
    """Return a module's 24-byte entries (CDHash, hash type, flags, category, reserved), checking its layout."""
    if len(module) < HEADER_BYTES:
        raise ValueError(f'trust cache: {len(module)} bytes is no module')
    version, count = struct.unpack_from('<I', module)[0], struct.unpack_from('<I', module, 20)[0]
    if version != VERSION or len(module) != HEADER_BYTES + count * ENTRY_BYTES:
        raise ValueError(f'trust cache: version {version} with {count} entries in {len(module)} bytes')
    entries = [module[at : at + ENTRY_BYTES] for at in range(HEADER_BYTES, len(module), ENTRY_BYTES)]
    if any(a[:CDHASH_BYTES] >= b[:CDHASH_BYTES] for a, b in itertools.pairwise(entries)):
        raise ValueError('trust cache: entries not in ascending CDHash order')
    return entries


def merged_module(modules: Iterable[bytes]) -> bytes:
    """Return one module holding every entry of `modules`, under the first one's UUID.

    Entries keep their hash type, flags and category. An entry in several modules stands once; the same CDHash with
    two different entries is an error.
    """
    modules = list(modules)
    if not modules:
        raise ValueError('trust cache: no modules to merge')
    unique = {}
    for module in modules:
        for entry in module_entries(module):
            if unique.setdefault(entry[:CDHASH_BYTES], entry) != entry:
                raise ValueError(f'trust cache: {entry[:CDHASH_BYTES].hex()} has two different entries')
    body = b''.join(unique[cdhash] for cdhash in sorted(unique))
    return struct.pack('<I16sI', VERSION, modules[0][4:20], len(unique)) + body


def cdhash_list(modules: Iterable[bytes]) -> bytes:
    """Return the CDHashes of `modules`' entries back to back, ascending and each once."""
    return b''.join(sorted({entry[:CDHASH_BYTES] for module in modules for entry in module_entries(module)}))
