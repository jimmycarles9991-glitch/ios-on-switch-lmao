"""A QMP client on the run's own unix socket (never a TCP port: several runs share the machine)."""

import json
import socket
import tempfile
import time
from pathlib import Path


def socket_path() -> Path:
    """Return a fresh, short socket path: macOS caps sun_path at 104 bytes, and run directories can be longer."""
    return Path(tempfile.mkdtemp(prefix='switch-emu-')) / 'qmp.sock'


class Monitor:
    def __init__(self, path: Path, timeout: float = 30):
        deadline = time.monotonic() + timeout
        self.sock = socket.socket(socket.AF_UNIX)
        while True:
            try:
                self.sock.connect(str(path))
                break
            except (FileNotFoundError, ConnectionRefusedError):
                if time.monotonic() > deadline:
                    raise
                time.sleep(0.1)
        self.file = self.sock.makefile('rw')
        if not self.file.readline():
            raise RuntimeError('QMP: QEMU closed the monitor')
        self.command('qmp_capabilities')

    def command(self, name: str, **arguments: object) -> object:
        self.file.write(json.dumps({'execute': name, 'arguments': arguments}) + '\n')
        self.file.flush()
        while True:
            line = self.file.readline()
            if not line:
                raise RuntimeError(f'QMP {name}: QEMU closed the monitor')
            reply = json.loads(line)
            if 'return' in reply:
                return reply['return']
            if 'error' in reply:
                raise RuntimeError(f'QMP {name}: {reply["error"]}')

    def human(self, line: str) -> str:
        return str(self.command('human-monitor-command', **{'command-line': line}))

    def close(self) -> None:
        self.sock.close()
