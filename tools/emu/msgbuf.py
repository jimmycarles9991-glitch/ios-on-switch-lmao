"""The kernel's live log: XNU's msgbuf, read through a core's MMU, for runs that end without a panic record."""

import functools
import re
import struct
import subprocess
import tempfile
from dataclasses import dataclass
from pathlib import Path

from tools.emu.monitor import Monitor

MSG_MAGIC = 0x063061
KERNEL_VA = 0xFFFFFFF000000000
# Where the loader puts the kernel window (both kinds), which holds XNU's `struct msgbuf msgbuf` in its __DATA.
KERNEL_PHYS = (0xB0000000, 0x8000000)


@dataclass(frozen=True)
class Ring:
    """A msgbuf ring, oldest byte first, and where its header was found."""

    where: str
    data: bytes

    def describe(self) -> str:
        return f'msgbuf at {self.where}: {len(self.data) - self.data.count(0)} of {len(self.data)} bytes written'


@functools.cache
def symbol(kernel: Path, name: str) -> int | None:
    """Look a symbol up once per run: a later build in the same output base may replace the file mid-run."""
    out = subprocess.run(['/usr/bin/nm', '-gU', str(kernel)], capture_output=True, text=True, check=False).stdout
    found = re.search(rf'^([0-9a-f]+) \w {re.escape(name)}$', out, re.MULTILINE)
    return int(found.group(1), 16) if found else None


def kernel_cpu(registers: str) -> int | None:
    """Return a core that was running the kernel at EL1, whose page tables reach the kernel's heap."""
    for index, block in enumerate(registers.split('CPU#')[1:]):
        pc = re.search(r'PC=([0-9a-f]+)', block)
        if pc and int(pc.group(1), 16) >= KERNEL_VA and 'EL1' in block:
            return index
    return None


def _read(monitor: Monitor, cpu: int, address: int, size: int) -> bytes:
    with tempfile.NamedTemporaryFile() as f:
        monitor.command('memsave', val=address, size=size, filename=f.name, **{'cpu-index': cpu})
        return Path(f.name).read_bytes()


def header(fields: bytes) -> tuple[int, int, int] | None:
    """Return a msgbuf header's (size, write offset, ring address), or None when these 24 bytes are not one."""
    magic, size, write, _, ring = struct.unpack('<iiiiQ', fields)
    if magic != MSG_MAGIC or not 0 < size <= 16 << 20 or not 0 <= write < size or ring < KERNEL_VA:
        return None
    return size, write, ring


def find_physical(window: bytes, base: int) -> int | None:
    """Return the physical address of the first msgbuf header in `window` (memory from `base`), 8-byte aligned."""
    magic = struct.pack('<i', MSG_MAGIC)
    at = window.find(magic)
    while at >= 0:
        if at % 8 == 0 and at + 24 <= len(window) and header(window[at : at + 24]):
            return base + at
        at = window.find(magic, at + 1)
    return None


def _via_symbol(monitor: Monitor, kernel: Path | None, cpu: int) -> tuple[tuple[int, int, int], str] | None:
    pointer = symbol(kernel, '_msgbufp') if kernel else None
    if pointer is None:
        return None
    try:
        (address,) = struct.unpack('<Q', _read(monitor, cpu, pointer, 8))
        found = header(_read(monitor, cpu, address, 24))
    except RuntimeError:
        return None
    return (found, f'_msgbufp -> {address:#x}') if found else None


def _via_scan(monitor: Monitor) -> tuple[tuple[int, int, int], str] | None:
    """Find the header in the kernel window, which needs no symbols: a release kernel exports no _msgbufp."""
    if getattr(monitor, 'msgbuf_physical', None) is None:
        with tempfile.NamedTemporaryFile() as f:
            monitor.command('pmemsave', val=KERNEL_PHYS[0], size=KERNEL_PHYS[1], filename=f.name)
            monitor.msgbuf_physical = find_physical(Path(f.name).read_bytes(), KERNEL_PHYS[0])
    if monitor.msgbuf_physical is None:
        return None
    with tempfile.NamedTemporaryFile() as f:
        monitor.command('pmemsave', val=monitor.msgbuf_physical, size=24, filename=f.name)
        found = header(Path(f.name).read_bytes())
    return (found, f'physical {monitor.msgbuf_physical:#x}') if found else None


def read(monitor: Monitor, kernel: Path | None, registers: str) -> Ring | None:
    """Return the msgbuf ring, or None when no core runs the kernel or no header is found.

    A kernel file that is missing, is not the one booted or has no _msgbufp (release) falls back to the scan.
    """
    cpu = kernel_cpu(registers)
    if cpu is None:
        return None
    try:
        located = _via_symbol(monitor, kernel, cpu) or _via_scan(monitor)
        if not located:
            return None
        (size, write, ring), where = located
        data = _read(monitor, cpu, ring, size)
    except RuntimeError:
        return None
    return Ring(where, data[write:] + data[:write])
