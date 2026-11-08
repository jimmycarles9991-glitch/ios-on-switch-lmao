"""The macOS tools the installer and deploy use, by absolute path, with plist output parsed.

These are the declared host inputs of //install:sd and //deploy:* (macOS base system, not downloadable): diskutil,
hdiutil, ditto, fsck_apfs, sw_vers, sync. Each run checks they exist and logs `sw_vers`.
"""

import concurrent.futures
import datetime
import os
import plistlib
import pty
import re
import selectors
import subprocess
import sys
from pathlib import Path

from tools.common import sudo
from tools.install import progress

TOOLS = {
    'diskutil': '/usr/sbin/diskutil',
    'hdiutil': '/usr/bin/hdiutil',
    'ditto': '/usr/bin/ditto',
    'cp': '/bin/cp',
    'fsck_apfs': '/sbin/fsck_apfs',
    'newfs_apfs': '/sbin/newfs_apfs',
    'mkbom': '/usr/bin/mkbom',
    'sw_vers': '/usr/bin/sw_vers',
    'sync': '/bin/sync',
}


class HostError(Exception):
    pass


class Log:
    """Everything printed also goes to the run's log file."""

    def __init__(self, path: str | Path):
        self.path = Path(path)
        sudo.user_directory(self.path.parent)
        self.file = self.path.open('a')
        sudo.give_back(self.path)

    def __call__(self, *parts: object) -> None:
        line = ' '.join(str(p) for p in parts)
        progress.println(line)
        self.file.write('{} {}\n'.format(datetime.datetime.now().isoformat(timespec='seconds'), line))
        self.file.flush()

    def quiet(self, *parts: object) -> None:
        self.file.write(
            '{} {}\n'.format(datetime.datetime.now().isoformat(timespec='seconds'), ' '.join(str(p) for p in parts))
        )
        self.file.flush()


def check_tools(log: Log) -> str:
    missing = [name for name, path in TOOLS.items() if not os.access(path, os.X_OK)]
    if missing:
        raise HostError('missing macOS tools: {}'.format(', '.join(missing)))
    versions = run('sw_vers').strip().replace('\n', '; ')
    log.quiet('host:', versions, '| euid', os.geteuid())
    return versions


def run(tool: str, *args: object, check: bool = True, binary: bool = False, log: Log | None = None) -> str | bytes:
    command = [TOOLS.get(tool, tool), *map(str, args)]
    if log:
        log.quiet('$', ' '.join(command))
    progress.running(brief(command))
    process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=not binary)
    stdout, stderr = _communicate(process)
    if check and process.returncode:
        raise HostError(_failure(command, process.returncode, stdout, stderr))
    return stdout


def _communicate(process: subprocess.Popen) -> tuple[str | bytes, str | bytes]:
    with process:
        try:
            while True:
                try:
                    return process.communicate(timeout=progress.WAIT)
                except subprocess.TimeoutExpired:
                    progress.tick()
        except BaseException:
            process.kill()
            raise


def _failure(command: list[str], returncode: int, stdout: str | bytes, stderr: str | bytes) -> str:
    detail = stderr.decode(errors='replace') if isinstance(stderr, bytes) else (stderr or stdout or '').strip()
    return '{} failed ({}): {}'.format(' '.join(command), returncode, detail)


def brief(command: list[str]) -> str:
    """Return a command as the running step's note: its words without options, each path by its name."""
    words = [Path(w).name if '/' in w else w for w in command if not w.startswith('-')]
    return ' '.join(words)[:80]


class _Lines:
    """One stream of a tool's output, cut into lines at each newline or carriage return as it arrives."""

    def __init__(self, parse: progress.Parser) -> None:
        self.parse, self.partial, self.kept = parse, b'', []

    def feed(self, data: bytes) -> None:
        buffer = self.partial + data
        cut = len(buffer) - 1 if buffer.endswith(b'\r') else len(buffer)
        *lines, rest = re.split(rb'\r\n|\r|\n', buffer[:cut])
        self.partial = rest + buffer[cut:]
        for line in lines:
            self.take(line)

    def take(self, raw: bytes) -> None:
        line = raw.decode(errors='replace')
        update = self.parse(line)
        if update:
            progress.apply(update)
        if update is None or update.keep:
            self.kept.append(line)

    def text(self) -> str:
        if self.partial.rstrip(b'\r'):
            self.take(self.partial.rstrip(b'\r'))
        self.partial = b''
        return ''.join(line + '\n' for line in self.kept)


def _read(fd: int) -> bytes:
    try:
        return os.read(fd, 1 << 16)
    except OSError:  # EIO: a pseudo-terminal whose last writer is gone
        return b''


def stream(command: list[str], parse: progress.Parser) -> subprocess.CompletedProcess:
    """Run `command` and read its output as it comes; parse() turns its progress lines into the running step's.

    Its stdout is a pseudo-terminal, so a tool that buffers a pipe (apfs_prepare_cryptex) still prints each line as it
    goes. Returns its output without the lines parse() took as progress alone.
    """
    progress.running(brief(command))
    terminal, child = pty.openpty()
    try:
        process = subprocess.Popen(command, stdout=child, stderr=subprocess.PIPE)
    except BaseException:
        os.close(terminal)
        raise
    finally:
        os.close(child)
    errors = process.stderr.fileno()
    lines = {terminal: _Lines(parse), errors: _Lines(parse)}
    try:
        with selectors.DefaultSelector() as selector:
            for fd in lines:
                selector.register(fd, selectors.EVENT_READ)
            while selector.get_map():
                for key, _ in selector.select(progress.WAIT):
                    data = _read(key.fd)
                    if data:
                        lines[key.fd].feed(data)
                    else:
                        selector.unregister(key.fd)
                progress.tick()
        process.wait()
    except BaseException:
        process.kill()
        process.wait()
        raise
    finally:
        os.close(terminal)
        process.stderr.close()
    return subprocess.CompletedProcess(command, process.returncode, lines[terminal].text(), lines[errors].text())


def watch(tool: str, *args: object, parse: progress.Parser, log: Log | None = None, check: bool = True) -> str:
    """Run a tool that reports its progress as run() does, reading that progress with parse() (stream)."""
    command = [TOOLS.get(tool, tool), *map(str, args)]
    if log:
        log.quiet('$', ' '.join(command))
    result = stream(command, parse)
    if check and result.returncode:
        raise HostError(_failure(command, result.returncode, result.stdout, result.stderr))
    return result.stdout


def plist(tool: str, *args: object, log: Log | None = None) -> dict:
    return plistlib.loads(run(tool, *args, binary=True, log=log))


def disk_info(identifier: str) -> dict | None:
    try:
        return plist('diskutil', 'info', '-plist', identifier)
    except HostError:
        return None


def partitions(disk: str) -> list[dict]:
    """Diskutil's view of the whole disk's partitions: [{DeviceIdentifier, Size, Content, VolumeName, ...}]."""
    listing = plist('diskutil', 'list', '-plist', disk)
    for item in listing.get('AllDisksAndPartitions', []):
        if item.get('DeviceIdentifier') == disk:
            return item.get('Partitions', [])
    return []


def boot_disks() -> set[str]:
    """Whole disks that hold the running system (the boot volume's physical stores)."""
    disks = set()
    info = disk_info('/')
    if not info:
        raise HostError('cannot find the boot volume')
    for key in ('ParentWholeDisk',):
        if info.get(key):
            disks.add(info[key])
    container = disk_info(info.get('APFSContainerReference', '')) if info.get('APFSContainerReference') else None
    for store in (container or {}).get('APFSPhysicalStores', []):
        ident = store.get('APFSPhysicalStore', '')
        whole = disk_info(ident)
        if whole and whole.get('ParentWholeDisk'):
            disks.add(whole['ParentWholeDisk'])
    return disks


PARTITION_SCHEMES = ('GUID_partition_scheme', 'FDisk_partition_scheme')


def whole_disk_of(entities: list[dict]) -> str | None:
    """Find the image's own whole disk in hdiutil's system-entities (not an APFS container disk synthesized from it)."""
    for entity in entities:
        dev = entity.get('dev-entry', '')
        if (
            dev.startswith('/dev/disk')
            and 's' not in dev[len('/dev/disk') :]
            and entity.get('content-hint') in PARTITION_SCHEMES
        ):
            return dev[len('/dev/') :]
    return None


def images() -> dict[str, str]:
    """{whole disk: image path} of attached disk images."""
    out = {}
    for item in plist('hdiutil', 'info', '-plist').get('images', []):
        disk = whole_disk_of(item.get('system-entities', []))
        if disk:
            out[disk] = item.get('image-path', '')
    return out


def apfs_containers() -> list[dict]:
    return plist('diskutil', 'apfs', 'list', '-plist').get('Containers', [])


def container_on(store: str) -> dict | None:
    """Return the APFS container whose physical store is `store` (e.g. disk4s9), or None."""
    for container in apfs_containers():
        if any(s.get('DeviceIdentifier') == store for s in container.get('PhysicalStores', [])):
            return container
    return None


def mount_point(identifier: str) -> str | None:
    info = disk_info(identifier)
    return (info or {}).get('MountPoint') or None


def require_root_or(reason: str) -> None:
    if os.geteuid() != 0:
        raise HostError(f'{reason} needs root: rerun with sudo')


def python() -> str:
    return sys.executable


CHUNK = 8 << 20


def copy_parallel(src: int, src_offset: int, dst: int, dst_offset: int, size: int) -> None:
    """`size` bytes from `src` to `dst` in CHUNK pieces on every core; all-zero pieces stay holes.

    Each piece counts toward the running step (progress) as it lands.
    """

    def piece(at: int) -> int:
        chunk = os.pread(src, min(CHUNK, size - at), src_offset + at)
        if len(chunk) != min(CHUNK, size - at):
            raise HostError(f'short read at {src_offset + at}')
        if chunk.strip(b'\0'):
            os.pwrite(dst, chunk, dst_offset + at)
        return len(chunk)

    with concurrent.futures.ThreadPoolExecutor(os.cpu_count() or 4) as pool:
        pieces = [pool.submit(piece, at) for at in range(0, size, CHUNK)]
        try:
            for copied in concurrent.futures.as_completed(pieces):
                progress.advance(copied.result())
        finally:
            for pending in pieces:
                pending.cancel()


def copy_file_parallel(source: str | os.PathLike, target: str | os.PathLike) -> None:
    """Copy a whole file on every core (copy_parallel)."""
    size = os.path.getsize(source)
    src = os.open(source, os.O_RDONLY)
    try:
        dst = os.open(target, os.O_RDWR | os.O_CREAT | os.O_TRUNC, 0o644)
        try:
            os.ftruncate(dst, size)
            copy_parallel(src, 0, dst, 0, size)
            os.fsync(dst)
        finally:
            os.close(dst)
    finally:
        os.close(src)
