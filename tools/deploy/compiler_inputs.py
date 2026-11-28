"""Stage the five externally built shader comparison payloads for their matching HW Test binary."""

import hashlib
import mmap
import shutil
from pathlib import Path

from tools.common import files

STATES = ('3ef3a99e4f382ce1', 'ae6d01e7cac0d964', '17d6ade58fef2152', '357f8355b68acebd', '3f9f7f04b2b91507')


def load(source: Path, package: Path, build: str) -> dict[str, bytes]:
    if build != '24B119':
        raise ValueError('these compiler inputs require the 24B119 development build')
    executable = package / 'overlay/Applications/SwitchHWTest.app/SwitchHWTest'
    if not executable.is_file():
        raise ValueError(f'{package} does not include SwitchHWTest.app')
    result = {}
    with executable.open('rb') as f, mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ) as app:
        for state in STATES:
            name = state + '.swab'
            path = source / name
            if path.stat().st_size > 2 << 20:
                raise ValueError(f'{path} exceeds the fixture size limit')
            data = path.read_bytes()
            identity = b'SWAB0001' + (16).to_bytes(4, 'little') + state.encode()
            if not data.startswith(identity) or app.find(hashlib.sha256(data).hexdigest().encode()) < 0:
                raise ValueError(f'{path} does not match the development HW Test in {package}')
            result[name] = data
    return result


DIRECTORY = Path('mobile/Media/Switch/benchloop/compiler')


def staged_states(data_root: Path) -> list[str]:
    return [key for key in STATES if (data_root / DIRECTORY / (key + '.swab')).is_file()]


def _discard(path: Path) -> None:
    if path.is_symlink() or path.is_file():
        path.unlink()
    elif path.exists():
        shutil.rmtree(path)


def stage(data_root: Path, inputs: dict[str, bytes] | None) -> None:
    destination = data_root.resolve() / DIRECTORY
    parent = destination.parent
    if parent.resolve() != parent:
        raise ValueError(f'compiler fixture directory has a symlinked parent: {parent}')
    pending = files.partial(destination)
    _discard(pending)
    if destination.exists() or destination.is_symlink():
        destination.rename(pending)
        _discard(pending)
    if inputs is None:
        return
    pending.mkdir(parents=True)
    for name, data in inputs.items():
        files.write_bytes(pending / name, data, mode=0o644, sync=True)
    pending.rename(destination)
