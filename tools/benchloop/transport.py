"""Files to and from the bench loop's folders on the Switch, over USB: libimobiledevice's afcclient.

Two roots, as hw_test_app/kit/BenchLoop.swift watches them:
  documents  HW Test's Documents through house_arrest (afcclient --documents <bundle id>): /Documents/benchloop
  media      the AFC root, /private/var/mobile/Media on the device: /Switch/benchloop
`doctor` tries the first, then the second. A local folder stands in for either in the tests and for the fake device.
"""

import shutil
import subprocess
import tempfile
from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path

BUNDLES = ('com.brunostjohn.switch.hwtest', 'com.brunostjohn.switch.hwtest27e')
DOCUMENTS_ROOT = '/Documents/benchloop'
MEDIA_ROOT = '/Switch/benchloop'
TIMEOUT_S = 120

Runner = Callable[..., subprocess.CompletedProcess]


class TransportError(RuntimeError):
    pass


@dataclass
class Afc:
    """One afcclient command a call, `--documents <bundle>` for the app's Documents or nothing for the AFC root."""

    udid: str | None = None
    documents: str | None = None
    tool: str = 'afcclient'
    run: Runner = subprocess.run

    @property
    def root(self) -> str:
        return DOCUMENTS_ROOT if self.documents else MEDIA_ROOT

    @property
    def name(self) -> str:
        return f'house_arrest Documents of {self.documents}' if self.documents else 'AFC /Switch/benchloop'

    def _call(self, *args: str) -> str:
        command = [self.tool]
        if self.udid:
            command += ['-u', self.udid]
        if self.documents:
            command += ['--documents', self.documents]
        # afcclient's getopt permutes argv: without `--`, `get -r` and `rm -rf` reach it as its own (invalid) options
        command.append('--')
        try:
            done = self.run([*command, *args], capture_output=True, text=True, timeout=TIMEOUT_S, check=False)
        except (OSError, subprocess.TimeoutExpired) as e:
            raise TransportError(f'{" ".join(command + list(args))}: {e}') from e
        text = (done.stdout or '') + (done.stderr or '')
        if done.returncode or 'Error' in text or 'No device found' in text:
            raise TransportError(f'{" ".join(command + list(args))}: {text.strip() or f"exit {done.returncode}"}')
        return done.stdout or ''

    def mkdir(self, remote: str) -> None:
        parts = remote.strip('/').split('/')
        for depth in range(1, len(parts) + 1):
            path = '/' + '/'.join(parts[:depth])
            if self.ls(path) is None:
                self._call('mkdir', path)

    def ls(self, remote: str) -> list[str] | None:
        try:
            out = self._call('ls', remote)
        except TransportError:
            return None
        names = [line.strip().rstrip('/') for line in out.splitlines()]
        return [n for n in names if n and n not in ('.', '..')]

    def put(self, local: Path, remote: str) -> None:
        self._call('put', str(local), remote)

    def mv(self, old: str, new: str) -> None:
        self._call('mv', old, new)

    def rm(self, remote: str) -> None:
        self._call('rm', '-rf', remote)

    def read(self, remote: str) -> bytes | None:
        """Give one file's bytes, None where it cannot be read."""
        with tempfile.TemporaryDirectory() as scratch:
            local = Path(scratch) / 'file'
            try:
                self._call('get', remote, str(local))
            except TransportError:
                return None
            return local.read_bytes() if local.is_file() else None

    def get(self, remote: str, local: Path) -> None:
        """`remote`'s folder into `local` itself, whichever way afcclient nests it."""
        staging = local.with_name(local.name + '.get')
        shutil.rmtree(staging, ignore_errors=True)
        staging.mkdir(parents=True)
        self._call('get', '-r', remote, str(staging))
        nested = staging / Path(remote).name
        source = nested if nested.is_dir() else staging
        local.mkdir(parents=True, exist_ok=True)
        for item in source.iterdir():
            shutil.move(str(item), local / item.name)
        shutil.rmtree(staging, ignore_errors=True)


@dataclass
class Local:
    """A folder as the device's root; `changed` runs after every rename, as the app's vnode source would fire."""

    folder: Path
    changed: Callable[[], None] | None = None

    root = ''
    name = 'local folder'

    def _at(self, remote: str) -> Path:
        return self.folder / remote.lstrip('/')

    def mkdir(self, remote: str) -> None:
        self._at(remote).mkdir(parents=True, exist_ok=True)

    def ls(self, remote: str) -> list[str] | None:
        at = self._at(remote)
        return sorted(p.name for p in at.iterdir()) if at.is_dir() else None

    def put(self, local: Path, remote: str) -> None:
        shutil.copyfile(local, self._at(remote))

    def mv(self, old: str, new: str) -> None:
        self._at(old).rename(self._at(new))
        if self.changed:
            self.changed()

    def rm(self, remote: str) -> None:
        at = self._at(remote)
        if at.is_dir():
            shutil.rmtree(at)
        elif at.exists():
            at.unlink()

    def read(self, remote: str) -> bytes | None:
        at = self._at(remote)
        return at.read_bytes() if at.is_file() else None

    def get(self, remote: str, local: Path) -> None:
        shutil.copytree(self._at(remote), local, dirs_exist_ok=True)


def devices(run: Runner = subprocess.run) -> list[str]:
    try:
        done = run(['idevice_id', '-l'], capture_output=True, text=True, timeout=30, check=False)
    except OSError:
        return []
    return [line.strip() for line in (done.stdout or '').splitlines() if line.strip()]
