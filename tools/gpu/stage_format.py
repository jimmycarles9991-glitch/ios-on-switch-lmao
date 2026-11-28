r"""The GPU stage manifest, format 2: what is staged onto the iPadOS Data volume and the preferences merged there.

One tab-separated record a line, after a header that says where the files are (and optionally the GPU kext's build
identity).

    # gpu stage 2 runfiles <the manifest's own rlocation path>     files below the runfiles root (//gpu:bundle)
    # gpu stage 2 tree                                            files below the manifest's directory (gpu.stage)
    # kext <the GPU kext's build identity>                        optional, for the icon cache record
    file\t<path below the Data volume>\t<sha256>\t<size>\t<mode, octal>\t<the file, below the root>
    pref\t<plist below the Data volume>\t<owner uid>\t<key>\t<value, JSON>

A runfiles manifest finds its root by taking its own rlocation path off its location. A tree is a directory with every
staged file at its path below the Data volume and TREE_MANIFEST at its top, so its file column repeats the path. Each
pref record is merged into its plist (binary, mode 644, owned by uid:uid) when the files are staged.

drivers/TegraGM20BMetal/stage-tegra-gm20b-metal.py reads both forms; it runs as a bare script, so it has its own copy
of the reader.
"""

import json
from pathlib import PurePosixPath

HEADER = '# gpu stage 2 '
KEXT = '# kext '
TREE_MANIFEST = 'gpu.stage'

# Core Animation's framebuffer and IOSurface formats: the GPU and the display driver take plain 8-bit rows, so
# backboardd must read these before it creates its first surface. pack/ca/deepcolour.py reads PREFS from this file.
PREFS = {
    'CAEnableDeepFramebuffer': False,
    'CAEnableFramebufferCompression': False,
    'CAEnableIOSurfaceCompression': False,
}
# backboardd's defaults domain is its bundle ID, com.apple.backboardd; the global domain is what took effect first.
PREFERENCE_FILES = ('com.apple.backboardd.plist', 'backboardd.plist', '.GlobalPreferences.plist')
HOMES = (('mobile', 501), ('root', 0))


def runfiles_header(own_rlocation: str) -> str:
    return HEADER + 'runfiles ' + own_rlocation


def tree_header() -> str:
    return HEADER + 'tree'


def kext_header(digest: str) -> str:
    return KEXT + digest


def file_record(path: str, digest: str, size: int, mode: int, source: str) -> str:
    return f'file\t{path}\t{digest}\t{size}\t{mode:o}\t{source}'


def pref_records() -> list[str]:
    """Return PREFS as records: each key in each of backboardd's preference files, for mobile and root."""
    return [
        f'pref\t{home}/Library/Preferences/{name}\t{uid}\t{key}\t{json.dumps(value)}'
        for home, uid in HOMES
        for name in PREFERENCE_FILES
        for key, value in PREFS.items()
    ]


def _check_header(lines: list[str]) -> None:
    if not lines or not lines[0].startswith(HEADER):
        raise ValueError('gpu stage: expected a format 2 manifest')


def as_tree(lines: list[str]) -> list[str]:
    """Return a manifest's records with each file named by its path below the Data volume, as a tree holds it."""
    _check_header(lines)
    out = [tree_header()]
    for line in lines[1:]:
        if line.startswith(KEXT):
            out.append(line)
            continue
        kind, *fields = line.split('\t')
        if kind == 'file':
            path, digest, size, mode, _ = fields
            fields = [path, digest, size, mode, path]
        out.append('\t'.join([kind, *fields]))
    return out


def files(lines: list[str]) -> list[tuple[str, int, str]]:
    """Return each file record's path below the Data volume, its mode and its file column."""
    _check_header(lines)
    records = [line.split('\t') for line in lines[1:] if not line.startswith(KEXT)]
    return [(r[1], int(r[4], 8), r[5]) for r in records if r[0] == 'file']


def check_path(path: str) -> str:
    """Refuse a path that leaves the directory it is below."""
    parts = PurePosixPath(path).parts
    if not parts or path.startswith('/') or '..' in parts:
        raise ValueError(f'gpu stage: {path!r} is not a path below the Data volume')
    return path
