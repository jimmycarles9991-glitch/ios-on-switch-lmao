"""Helpers shared by the kernel build tools (run by Bazel actions, never by hand)."""

import os
import shutil
import subprocess
import sys
from collections.abc import Iterable
from pathlib import Path
from typing import TextIO

# Written into recipes, link commands and records in place of the action's own execution root, which differs
# between sandboxes; every tool that reads them puts its own execution root back.
EXECROOT = '@EXECROOT@'
# Apple clang's UBSan source locations do not honour -ffile-prefix-map: keep their last three path components.
UBSAN_PATHS = '-fsanitize-undefined-strip-path-components=-3'
# The collation XNU's make sorts symbol sets with (the CMake flow inherited the user's en_IE.UTF-8).
BUILD_LOCALE = 'en_IE.UTF-8'
# The PATH every tool gives the processes it starts: the system's and Xcode's tools, nothing from Homebrew.
SYSTEM_PATH = '/usr/bin:/bin:/usr/sbin:/sbin'


def xcode_env(path: str | Path) -> dict[str, str]:
    """@xcode//:xcode.env as a dict (DEVELOPER_DIR, XCODE_TOOLCHAIN, IPHONEOS_SDK, MACOSX_SDK)."""
    values = {}
    for line in Path(path).read_text().splitlines():
        key, _, value = line.partition('=')
        if key:
            values[key] = value
    return values


def environment(xcode: dict[str, str], extra_path: Iterable[str] = (), **extra: object) -> dict[str, str]:
    """Return a clean environment: fixed PATH, Xcode by DEVELOPER_DIR, deterministic archives, no locale drift."""
    env = {
        'PATH': ':'.join([*extra_path, SYSTEM_PATH]),
        'DEVELOPER_DIR': xcode['DEVELOPER_DIR'],
        'HOME': os.environ.get('TMPDIR', '/tmp'),
        'TMPDIR': os.environ.get('TMPDIR', '/tmp'),
        'LC_ALL': 'C',
        'LANG': 'C',
        'TZ': 'UTC',
        'ZERO_AR_DATE': '1',
    }
    env.update({k: str(v) for k, v in extra.items()})
    return env


def run(
    command: Iterable[str | Path], env: dict[str, str], cwd: str | Path | None = None, log: TextIO | None = None
) -> None:
    command = [str(c) for c in command]
    result = subprocess.run(
        command, env=env, cwd=cwd, stdout=log or None, stderr=subprocess.STDOUT if log else None, check=False
    )
    if result.returncode:
        tail = ''
        if log:
            log.flush()
            # The log is an output Bazel deletes when the action fails: its end goes into the failure message.
            tail = '\n'.join(Path(log.name).read_text(errors='replace').splitlines()[-200:])
        sys.exit('command failed ({}): {}\n{}'.format(result.returncode, ' '.join(command)[:2000], tail))


def execroot() -> str:
    return os.getcwd()


# The layout table of kernel/rules/layout.bzl (keep the two in step): where the kernel tree's sources and generated
# files are, and the old spelling the artifacts keep for them.
OLD_TREE = 'out/native-a57/root-v1/tree'
TREE_ROOT = ''
KERNEL_PACKAGE = 'kernel'
MOVED_ROOTS = ('drivers', 'kits', 'diagnostics', 'overlay')


def old_spelling(path: str, bin_dir: str | None = None) -> str:
    """Return the pre-move spelling of an execroot-relative path (unchanged if it was not moved)."""
    for root in MOVED_ROOTS:
        if path == root or path.startswith(root + '/'):
            return OLD_TREE + '/' + path
    if bin_dir and KERNEL_PACKAGE != OLD_TREE and path.startswith(bin_dir + '/' + KERNEL_PACKAGE + '/'):
        return bin_dir + '/' + OLD_TREE + '/' + path[len(bin_dir + '/' + KERNEL_PACKAGE) + 1 :]
    return path


def compat_prefix_maps(path_root: str, bin_dir: str) -> list[str]:
    """Return prefix maps from moved paths (absolute, below this execution root) to their old spellings.

    They come after the generic ones (debug maps: the last match wins; macro maps: the most specific). Empty while
    nothing has moved.
    """
    here, root = execroot(), path_root.rstrip('/')
    maps = []
    if TREE_ROOT != OLD_TREE:
        maps += [f'-ffile-prefix-map={here}/{r}={root}/{OLD_TREE}/{r}' for r in MOVED_ROOTS]
    if KERNEL_PACKAGE != OLD_TREE:
        new = f'{here}/{bin_dir}/{KERNEL_PACKAGE}/build'
        maps += [
            f'-fmacro-prefix-map={new}={root}/{OLD_TREE}/build',
            f'-fdebug-prefix-map={new}={root}/{bin_dir}/{OLD_TREE}/build',
        ]
    return maps


# Where the @xnu repository (the patched tarball) put XNU's source: its spelling in debug information.
XNU_REPOSITORY = 'external/+http_archive+xnu'


def prefix_maps(path_root: str, bin_dir: str, xnu_dir: str) -> list[str]:
    """Return -ffile-prefix-map flags that make every path a compile can embed independent of the sandbox.

    The patched XNU source shows as <root>/out/native-a57/root-v1/tree/.cache/xnu-work (where the CMake flow kept its
    checkout), build outputs as <root>/<package path> (bazel-bin/out/... -> out/...), sources as <root>/<path>.
    """
    here = execroot()
    tree = path_root.rstrip('/') + '/out/native-a57/root-v1/tree/.cache/xnu-work'
    maps = [
        f'-ffile-prefix-map={here}/{xnu_dir}={tree}',
        '-ffile-prefix-map={}/{}={}'.format(here, bin_dir, path_root.rstrip('/')),
        '-ffile-prefix-map={}={}'.format(here, path_root.rstrip('/')),
        *compat_prefix_maps(path_root, bin_dir),
    ]
    if xnu_dir != XNU_REPOSITORY:
        # The vendored XNU (kernel/xnu-<version>) keeps the debug-information spelling it had as the @xnu repository
        # (debug maps go by the last match: after the execution root's).
        maps += [
            '-fdebug-prefix-map={}/{}={}/{}'.format(here, xnu_dir, path_root.rstrip('/'), XNU_REPOSITORY),
            f'-fdebug-prefix-map={xnu_dir}={XNU_REPOSITORY}',
        ]
    return maps


def copy_writable(source: str | Path, target: str | Path) -> None:
    """Copy a file with its mode, writable by its owner.

    A remote worker hands an action read-only inputs where the local sandbox's are writable: a copy made with the
    input's mode could not be written over, or written to, there.
    """
    shutil.copyfile(source, target)
    os.chmod(target, (os.stat(source).st_mode & 0o777) | 0o200)


def copy_tree(source: str | Path, target: str | Path) -> None:
    """Copy a tree's contents (files Bazel stages as symlinks become files; links inside it become copies)."""
    shutil.copytree(source, target, symlinks=False, dirs_exist_ok=True, copy_function=copy_writable)


def resolve_links(root: str | Path) -> None:
    """Replace every symbolic link below root by a copy of what it names.

    Kernel.framework's Headers -> Versions/Current/Headers and Versions/Current -> A, as XNU's installhdrs makes them.
    Tree artifacts stay self-contained: a cache that hands out a tree's links but not their targets (Bazel's build
    without the bytes) would otherwise leave them dangling, and Bazel then re-runs the action on every build.
    """
    root = Path(root)
    while True:
        links = sorted((p for p in root.rglob('*') if p.is_symlink()), key=lambda p: len(p.parts))
        if not links:
            return
        link = links[0]
        target = link.resolve()
        if not str(target).startswith(str(root.resolve()) + '/'):
            sys.exit(f'{link} links outside the tree: {target}')
        link.unlink()
        if target.is_dir():
            shutil.copytree(target, link, symlinks=True)
        else:
            shutil.copy2(target, link)
