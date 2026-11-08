"""The tables user space reaches the kernel through, found by their contents (Apple's kernel cache has no symbols).

Each function returns {key: shape}, the key naming what user space calls ('sysent 148', 'trap 31', 'mig 2891',
'sysctl kern.ostype') and the shape what has to match between two kernels.
"""

import re
import struct
from collections.abc import Iterator

from kernel_image import KernelImage

SYSENT_SIZE = 24
# (sy_narg, sy_arg_bytes) of entries 0-7: nosys, exit, fork, read, write, open, close, wait4.
SYSENT_HEAD = [(0, 0), (1, 4), (0, 0), (3, 12), (3, 12), (3, 12), (1, 4), (4, 16)]
# Old calls that every kernel keeps as nosys or enosys: their handlers are the "absent" ones.
SYSENT_ABSENT_SLOTS = (0, 8)
MACH_TRAP_COUNT = 128
MIG_FIRST_SUBSYSTEMS = (4800, 3200)  # mach_vm, mach_port: mig_e's first two entries in xnu-10063 to 13432
MIG_MAX_ROUTINES = 1000
CTLTYPE_NODE = 1
CTLFLAG_OID2 = 0x00400000
SYSCTL_MAX_DEPTH = 12


def system_calls(image: KernelImage) -> dict[str, str]:
    """Return every BSD system call the sysent table answers, with its argument count and bytes."""
    for start in _matches(image, b'(?s)' + rb'.{20}\x03\x00\x0c\x00' * 3, -3 * SYSENT_SIZE):
        entries = []
        address = start
        while True:
            call, _, _, narg, arg_bytes = struct.unpack('<QQiHH', image.read(address, SYSENT_SIZE))
            if not image.is_text(image.pointer(call)):
                break
            entries.append((image.pointer(call), narg, arg_bytes))
            address += SYSENT_SIZE
        if [(n, b) for _, n, b in entries[: len(SYSENT_HEAD)]] == SYSENT_HEAD:
            absent = {entries[i][0] for i in SYSENT_ABSENT_SLOTS}
            return {
                f'sysent {i}': f'narg {n} bytes {b}' for i, (call, n, b) in enumerate(entries) if call not in absent
            }
    raise LookupError('no sysent table')


def mach_traps(image: KernelImage) -> dict[str, str]:
    """Return every Mach trap the trap table answers (a DEVELOPMENT kernel's entries carry a name: 32 bytes)."""
    for stride in (24, 32):
        pattern = (rb'\x00\x00\x01.' + b'.' * (stride - 4)) * 4
        for start in _matches(image, b'(?s)' + pattern, -26 * stride):
            traps = [image.read(start + i * stride, 16) for i in range(MACH_TRAP_COUNT)]
            if [t[0] for t in traps[30:33]] != [0, 7, 8] or traps[10][0] != 4:
                continue
            handlers = [image.pointer(struct.unpack_from('<Q', t, 8)[0]) for t in traps]
            return {
                f'trap {i}': f'args {t[0]} words {t[1]} port {t[2]}'
                for i, t in enumerate(traps)
                if handlers[i] != handlers[0]
            }
    raise LookupError('no mach_trap_table')


def mig_routines(image: KernelImage) -> dict[str, tuple[int, int, int, int]]:
    """Return every kernel MIG routine: (argc, descriptors, reply descriptors, largest reply)."""
    routines = {}
    for subsystem in _mig_table(image):
        start, end = struct.unpack('<ii', image.read(subsystem + 8, 8))
        for i in range(end - start):
            fields = struct.unpack('<QQIIII', image.read(subsystem + 32 + 32 * i, 32))
            if fields[1]:
                routines[f'mig {start + i}'] = fields[2:]
    return routines


def _mig_table(image: KernelImage) -> list[int]:
    structs = set(_aligned(image, struct.pack('<i', MIG_FIRST_SUBSYSTEMS[0]), -8))
    for _, data in image.data_segments():
        words = struct.unpack(f'<{len(data) // 8}Q', data[: len(data) // 8 * 8])
        for i, word in enumerate(words):
            if word and image.pointer(word) in structs:
                table = _mig_subsystems(image, words[i:])
                if [struct.unpack('<i', image.read(s + 8, 4))[0] for s in table[:2]] == list(MIG_FIRST_SUBSYSTEMS):
                    return table
    raise LookupError('no mig_e table')


def _mig_subsystems(image: KernelImage, words: tuple[int, ...]) -> list[int]:
    table = []
    for raw in words:
        subsystem = image.pointer(raw)
        try:
            start, end = struct.unpack('<ii', image.read(subsystem + 8, 8))
        except KeyError:
            break
        if start <= 0 or not 0 < end - start < MIG_MAX_ROUTINES:
            break
        table.append(subsystem)
    return table


def sysctls(image: KernelImage, owners: set[str]) -> dict[str, tuple[str, int]]:
    """Return {name: (owner, CTLTYPE)} of every static sysctl oid whose owner (fileset entry or 'kernel') is listed."""
    oids = dict(_oids(image))
    lists = {arg1: address for address, (_, kind, arg1, _) in oids.items() if kind & 0xF == CTLTYPE_NODE}
    roots = {parent for parent, kind, _, leaf in oids.values() if leaf == 'kern' and kind & 0xF == CTLTYPE_NODE}
    names = {}
    for address, (first_parent, kind, _, leaf) in oids.items():
        owner = image.owner(address)
        if owner not in owners:
            continue
        path, parent = [leaf], first_parent
        while parent not in roots and parent in lists and len(path) < SYSCTL_MAX_DEPTH:
            parent, _, _, node = oids[lists[parent]]
            path.insert(0, node)
        if parent in roots:
            names['sysctl ' + '.'.join(path)] = (owner, kind & 0xF)
    return names


def _oids(image: KernelImage) -> Iterator[tuple[int, tuple[int, int, int, str]]]:
    """Yield (address, (parent list, kind, arg1, name)) of every struct sysctl_oid of version 1 in the data."""
    for vmaddr, data in image.data_segments():
        for i in range(0, len(data) - 80, 8):
            if data[i + 72 : i + 76] != b'\1\0\0\0':
                continue
            parent, _, _, kind, arg1, _, name = struct.unpack_from('<QQiIQQQ', data, i)
            if not parent or not name or not kind & CTLFLAG_OID2:
                continue
            leaf = image.cstring(image.pointer(name))
            if leaf and not leaf.startswith('__anchor__'):
                yield vmaddr + i, (image.pointer(parent), kind, image.pointer(arg1), leaf)


def _matches(image: KernelImage, pattern: bytes, adjust: int) -> Iterator[int]:
    for vmaddr, data in image.data_segments():
        for match in re.finditer(pattern, data):
            if (match.start() + adjust) % 8 == 0:
                yield vmaddr + match.start() + adjust


def _aligned(image: KernelImage, needle: bytes, adjust: int) -> Iterator[int]:
    for vmaddr, data in image.data_segments():
        at = data.find(needle)
        while at != -1:
            if (at + adjust) % 8 == 0:
                yield vmaddr + at + adjust
            at = data.find(needle, at + 1)
