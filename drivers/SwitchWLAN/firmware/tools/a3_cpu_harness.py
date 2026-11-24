import contextlib
import json
import socket
import struct
import subprocess
from collections.abc import Callable, Iterator
from pathlib import Path
from typing import BinaryIO


def switch_qmp_command(control: BinaryIO, name: str, arguments: dict[str, str] | None = None) -> str:
    message = {'execute': name}
    if arguments is not None:
        message['arguments'] = arguments
    control.write(json.dumps(message).encode() + b'\n')
    while True:
        reply = json.loads(control.readline())
        if 'error' in reply:
            raise RuntimeError(reply)
        if 'return' in reply:
            return str(reply['return'])


def switch_read_words(memory: BinaryIO, address: int, count: int) -> tuple[int, ...]:
    memory.write(f'read {address:#x} {count * 4}\n'.encode())
    reply = memory.readline().decode().split()
    if len(reply) != 2 or reply[0] != 'OK':
        raise RuntimeError(reply)
    return struct.unpack(f'<{count}I', bytes.fromhex(reply[1][2:]))


@contextlib.contextmanager
def switch_qemu(arguments: list[str], path: Path) -> Iterator[tuple[socket.socket, socket.socket]]:
    """QEMU stopped before its first instruction, with its QMP monitor and qtest channel connected."""
    # QEMU connects to sockets that already listen, as QEMU's own libqtest starts it. A socket QEMU serves itself is on
    # disk from bind() but refuses connections until listen(), which a loaded machine makes long enough to hit.
    with socket.socket(socket.AF_UNIX) as qmp_server, socket.socket(socket.AF_UNIX) as qtest_server:
        for server, name in ((qmp_server, 'qmp'), (qtest_server, 'qtest')):
            (path / name).unlink(missing_ok=True)
            server.bind(str(path / name))
            server.listen(1)
            server.settimeout(3)
        proc = subprocess.Popen(
            [*arguments, '-qtest', f'unix:{path}/qtest', '-qmp', f'unix:{path}/qmp', '-S'],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
        )
        try:
            try:
                qmp, _ = qmp_server.accept()
                qtest, _ = qtest_server.accept()
            except TimeoutError:
                proc.kill()
                detail = proc.communicate()[1].decode()
                raise RuntimeError('QEMU did not connect its control sockets: ' + detail) from None
            with qmp, qtest:
                qmp.settimeout(3)
                qtest.settimeout(3)
                yield qmp, qtest
        finally:
            proc.terminate()
            try:
                proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
            proc.stderr.close()


def switch_run_arm(
    qemu: str,
    elf: Path,
    path: Path,
    symbols: dict[str, int],
    reader: Callable[[socket.socket, socket.socket, dict[str, int]], tuple[int, ...]],
) -> tuple[int, ...]:
    arguments = [
        qemu,
        '-M',
        'versatilepb',
        '-cpu',
        'cortex-a15',
        '-m',
        '16M',
        '-display',
        'none',
        '-serial',
        'none',
        '-monitor',
        'none',
        '-audio',
        'none',
        '-device',
        f'loader,file={elf},cpu-num=0',
    ]
    with switch_qemu(arguments, path) as (qmp, qtest):
        return reader(qmp, qtest, symbols)
