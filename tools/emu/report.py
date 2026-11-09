"""What a stopped run leaves in its directory: registers, the kernel's log and panic, the screen, a summary."""

import re
import struct
import zlib
from collections.abc import Sequence
from pathlib import Path

from tools.emu import gpulog, msgbuf
from tools.emu.monitor import Monitor

RAMOOPS = (0xB0000000, 0x200000)
FRAMEBUFFER = (0xF5A00000, 720 * 1280 * 4)
LOG_LINE = re.compile(rb'\[ *(\d+)\.(\d{6})\]: ?([\x20-\x7e]*)')
PANIC = re.compile(rb'panic\(cpu \d+ caller [^\n\0]*')


def capture(monitor: Monitor, run: Path, kernel: Path | None) -> None:
    monitor.command('stop')
    registers = monitor.human('info registers -a')
    (run / 'registers.txt').write_text(registers)
    ring = msgbuf.read(monitor, kernel, registers)
    if ring:
        (run / 'msgbuf.raw').write_bytes(ring.data)
    monitor.command('pmemsave', val=RAMOOPS[0], size=RAMOOPS[1], filename=str(run / 'ramoops.raw'))
    monitor.command('pmemsave', val=FRAMEBUFFER[0], size=FRAMEBUFFER[1], filename=str(run / 'framebuffer.raw'))
    try:
        monitor.command('screendump', filename=str(run / 'screen.png'), format='png')
    except RuntimeError as error:
        (run / 'screen.txt').write_text(f'no screen: {error}\n')


def kernel_log(ramoops: bytes) -> list[str]:
    """Return the log lines in the kernel's RAM log, oldest first (the ring may have wrapped)."""
    lines = {}
    for match in LOG_LINE.finditer(ramoops):
        stamp = int(match.group(1)) * 1_000_000 + int(match.group(2))
        lines.setdefault((stamp, match.group(3)), None)
    return [f'[{s // 1_000_000:6d}.{s % 1_000_000:06d}] {text.decode()}' for s, text in sorted(lines)]


def panic_text(ramoops: bytes) -> str | None:
    found = PANIC.search(ramoops)
    return found.group(0).decode(errors='replace') if found else None


def png(raw: bytes, width: int, height: int, out: Path) -> None:
    """Write a B8G8R8A8 frame as an RGB PNG."""
    rgb = bytearray(width * height * 3)
    rgb[0::3], rgb[1::3], rgb[2::3] = raw[2::4], raw[1::4], raw[0::4]
    stride = width * 3
    rows = b''.join(b'\0' + rgb[y * stride : (y + 1) * stride] for y in range(height))

    def chunk(kind: bytes, data: bytes) -> bytes:
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))

    header = struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0)
    out.write_bytes(
        b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', header) + chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b'')
    )


def program_counters(registers: str) -> list[str]:
    return re.findall(r'PC=([0-9a-f]+)', registers)[:4]


def display_notes(run: Path) -> str:
    """Count the display controller's QEMU log lines: reads that faulted or left DRAM, and what it does not model."""
    log = run / 'qemu-guest.log'
    if not log.exists():
        return 'no QEMU log'
    notes = [line for line in log.read_text(errors='replace').splitlines() if line.startswith('tegra.dc:')]
    return f'{len(notes)} lines' + (f', first: {notes[0]}' if notes else '')


def summarize(run: Path, polled: Sequence[str] = ()) -> str:
    """Write and return the run's summary.

    `polled` are log lines read during the run, which the capture at its end misses when no core is in the kernel then
    (all of them idle in bl31).
    """
    ramoops = (run / 'ramoops.raw').read_bytes()
    live = run / 'msgbuf.raw'
    log = sorted(set(kernel_log(live.read_bytes() if live.exists() else ramoops)).union(polled))
    (run / 'kernel.log').write_text('\n'.join(log) + '\n')
    panic = panic_text(ramoops)
    if panic:
        (run / 'panic.txt').write_text(panic + '\n')
    png((run / 'framebuffer.raw').read_bytes(), 720, 1280, run / 'framebuffer.png')
    uart = (run / 'uart-a.log').read_text(errors='replace') if (run / 'uart-a.log').exists() else ''
    online = sorted(set(re.findall(r'arm_cpu_init\(\): cpu (\d) online', '\n'.join(log))))
    lines = [
        f'run: {run}',
        f'firmware: {"U-Boot handed over to the loader" if "Starting kernel" in uart else "never reached the loader"}',
        f'kernel log: {len(log)} lines; secondary cores online: {", ".join(online) or "none"}',
        f'core PCs: {" ".join(program_counters((run / "registers.txt").read_text()))}',
        f'panic: {panic or "none"}',
        f'gpu: {gpulog.summarize(run)}',
        f'display: {display_notes(run)}',
        'last kernel lines:',
        *log[-12:],
    ]
    text = '\n'.join(lines) + '\n'
    (run / 'summary.txt').write_text(text)
    return text
