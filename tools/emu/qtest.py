"""The patched tegra_qemu under QEMU's qtest protocol.

No CPU runs: a test reads and writes the machine's registers and memory and steps its clock itself.
"""

import base64
import subprocess
from pathlib import Path


class Qtest:
    def __init__(self, binary: Path, log: Path | None = None):
        args = [
            str(binary),
            '-M', 'tegrax1plus',
            '-m', '4G',
            '-display', 'none',
            '-accel', 'qtest',
            '-qtest', 'stdio',
            '-serial', 'null',
            '-monitor', 'none',
        ]  # fmt: skip
        if log:
            args += ['-d', 'guest_errors,unimp', '-D', str(log)]
        self.proc = subprocess.Popen(
            args, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True
        )

    def command(self, line: str) -> list[str]:
        assert self.proc.stdin
        assert self.proc.stdout
        self.proc.stdin.write(line + '\n')
        self.proc.stdin.flush()
        while True:
            reply = self.proc.stdout.readline()
            if not reply:
                raise RuntimeError(f'qtest {line!r}: the emulator exited')
            words = reply.split()
            if words and words[0] == 'OK':
                return words[1:]
            if words and words[0] in ('FAIL', 'ERR'):
                raise RuntimeError(f'qtest {line!r}: {reply.strip()}')

    def readl(self, address: int) -> int:
        return int(self.command(f'readl {address:#x}')[0], 16)

    def writel(self, address: int, value: int) -> None:
        self.command(f'writel {address:#x} {value:#x}')

    def write(self, address: int, data: bytes) -> None:
        self.command(f'b64write {address:#x} {len(data):#x} {base64.b64encode(data).decode()}')

    def read(self, address: int, size: int) -> bytes:
        return base64.b64decode(self.command(f'b64read {address:#x} {size:#x}')[0])

    def step(self, nanoseconds: int) -> None:
        self.command(f'clock_step {nanoseconds}')

    def close(self) -> None:
        self.proc.kill()
        self.proc.communicate()
