"""tegra_qemu at a pinned commit with tools/emu/patches applied, built once per Mac in the shared cache (cache.py).

The key is the commit, every patch's bytes, the configure flags and BUILD_REVISION; nothing else rebuilds it. One
checkout and one ninja tree under ~/Library/Caches/switch-ios/tegra-qemu serve every key, so a new patch set recompiles
only the files it touches.
"""

import os
import shutil
import subprocess
import sys
from pathlib import Path

from tools.emu import cache

URL = 'https://github.com/yellows8/tegra_qemu'
# Branch `tegra` (QEMU 9.2.0 plus the tegrax1/tegrax1plus machines), 2024-12-24.
COMMIT = '1e835d58bbf9ad818047ceba2257f1dcbda3911b'
# Bump when this file changes how a build is made without changing the flags or patches.
BUILD_REVISION = 1
PATCHES = Path(__file__).parent / 'patches'
BREW = Path('/opt/homebrew')
BREW_FORMULAE = ['glib', 'pixman', 'libgcrypt', 'dtc', 'pkgconf', 'ninja']
# QEMU 9.2's mkvenv needs distlib.version, which pip 25's vendored distlib no longer has.
PYTHON = '3.13'
DISTLIB = 'distlib==0.3.9'
CONFIGURE = [
    '--target-list=aarch64-softmmu',
    '--disable-docs',
    '--disable-werror',
    '--disable-tools',
    '--disable-guest-agent',
    '--disable-sdl',
    '--disable-gtk',
    '--disable-vnc',
    '--disable-curl',
    '--disable-libssh',
    '--disable-capstone',
    '--disable-slirp',
    '--disable-rust',
    # The tegra SE model calls libgcrypt's MPI functions itself.
    '--disable-gnutls',
    '--enable-gcrypt',
    '--enable-fdt=system',
    f'--extra-cflags=-I{BREW}/include',
    f'--extra-ldflags=-L{BREW}/lib',
]
BINARY = 'qemu-system-aarch64'


def patches() -> list[Path]:
    return sorted(PATCHES.glob('*.patch'))


def key() -> str:
    return cache.digest(
        {
            'commit': COMMIT,
            'patches': [[p.name, cache.file_digest(p)] for p in patches()],
            'configure': CONFIGURE,
            'revision': BUILD_REVISION,
        }
    )


def _host_env() -> dict:
    """Return the caller's environment without `bazel run`'s Python settings (PYTHONSAFEPATH breaks QEMU's tools)."""
    return {k: v for k, v in os.environ.items() if not k.startswith(('PYTHON', 'RUNFILES', 'VIRTUAL_ENV'))}


def _run(args: list, log: Path, cwd: Path | None = None) -> None:
    with log.open('a') as out:
        out.write(f'$ {" ".join(map(str, args))}\n')
        out.flush()
        result = subprocess.run(args, cwd=cwd, env=_host_env(), stdout=out, stderr=subprocess.STDOUT, check=False)
    if result.returncode:
        tail = log.read_text().splitlines()[-25:]
        sys.exit(f'tegra_qemu build failed ({log}):\n' + '\n'.join(tail))


def _check_host() -> None:
    missing = [f for f in BREW_FORMULAE if not (BREW / 'opt' / f).exists()]
    if missing:
        sys.exit('tegra_qemu needs Homebrew formulae: brew install ' + ' '.join(missing))
    if not shutil.which('uv'):
        sys.exit('tegra_qemu needs uv (https://docs.astral.sh/uv/) for its build-time Python')


def _source(base: Path, log: Path) -> Path:
    src = base / 'src'
    if not (src / '.git').is_dir():
        _run(['git', 'init', '-q', src], log)
        _run(['git', '-C', src, 'remote', 'add', 'origin', URL], log)
    have = subprocess.run(['git', '-C', src, 'cat-file', '-e', COMMIT + '^{commit}'], capture_output=True, check=False)
    if have.returncode:
        _run(['git', '-C', src, 'fetch', '-q', '--depth', '1', 'origin', COMMIT], log)
    # Downloaded meson subprojects are ignored files: clean -fd keeps them, and removes files an old patch added.
    _run(['git', '-C', src, 'checkout', '-q', '-f', COMMIT], log)
    _run(['git', '-C', src, 'clean', '-q', '-fd'], log)
    for patch in patches():
        _run(['git', '-C', src, 'apply', '--whitespace=nowarn', patch], log)
    return src


def _python(base: Path, log: Path) -> Path:
    python = base / 'py/bin/python'
    if not python.exists():
        _run(['uv', 'venv', '-q', '--python', PYTHON, base / 'py'], log)
        _run(['uv', 'pip', 'install', '-q', '--python', python, DISTLIB], log)
    return python


def _make(tmp: Path, base: Path) -> None:
    _check_host()
    log = base / 'build.log'
    log.write_text('')
    print(f'tegra_qemu: building {key()} (log: {log})', file=sys.stderr)
    src = _source(base, log)
    build = base / 'build'
    flags = '\n'.join(CONFIGURE)
    stamp = build / 'switch-configure-flags'
    if not (build / 'build.ninja').exists() or not stamp.exists() or stamp.read_text() != flags:
        shutil.rmtree(build, ignore_errors=True)
        build.mkdir()
        python = _python(base, log)
        _run([src / 'configure', f'--python={python}', *CONFIGURE], log, cwd=build)
        stamp.write_text(flags)
    env_jobs = str(os.cpu_count() or 4)
    _run(['ninja', '-C', build, '-j', env_jobs, BINARY], log)
    shutil.copy2(build / BINARY, tmp / BINARY)
    for notice in ('COPYING', 'COPYING.LIB', 'LICENSE'):
        shutil.copy2(src / notice, tmp / notice)
    (tmp / 'SOURCE').write_text(f'{URL} {COMMIT}\n' + ''.join(f'{p.name} {cache.file_digest(p)}\n' for p in patches()))


def ever_built() -> bool:
    """Return whether this Mac has built tegra_qemu at all, for any key."""
    return (cache.ROOT / 'tegra-qemu' / 'entries').is_dir()


def built() -> Path | None:
    """Return the patched tegra_qemu binary if this Mac has a build of this key; never build it."""
    done = cache.existing('tegra-qemu', key())
    return done / BINARY if done else None


def ensure() -> Path:
    """Return the patched tegra_qemu binary, built first if this Mac has no build of this key."""
    return cache.entry('tegra-qemu', key(), _make) / BINARY
