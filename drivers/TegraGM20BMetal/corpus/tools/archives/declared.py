"""Every pipeline state Apple's OS declares ahead, as corpus lines: binary archives, pipeline libraries and scripts.

usage: declared.py --root <mounted volume>... --inventory <out.tsv>

The roots are an IPSW's OS volume and its cryptexes, mounted read-only (later roots win a path both hold, as the
SystemOS cryptex is grafted over the OS volume). Each state's functions are found in the library the device loads them
from: an archive's function by the hash it names (gpubin.py), a pipeline library's or script's in the metallib its
file names. --inventory: per owner and distinct state (model.py's corpus line) its kind, owner, archive, whether the
raw device's GPU families are among those it is declared for, and its line (inventory()).
"""

import argparse
import os
import struct
import sys
from collections import Counter
from collections.abc import Callable
from pathlib import Path

from drivers.TegraGM20BMetal.corpus.tools.archives import airp, desc, gpubin, script
from drivers.TegraGM20BMetal.corpus.tools.archives.model import Function, Pipeline
from tools.gpu.shaders import metallib

OWNER_SUFFIXES = ('.framework', '.bundle', '.app', '.appex', '.cifilter', '.xpc')
# The GPU driver's own shaders (ds.g<gpu>: no states a process makes)
DRIVER_BUNDLE = 'AGXMetal'
SCRIPT_NAMES = ('.mtlpipeline', '.mtlp-json', '-pipeline.json', '_pipeline.json', 'pipeline_watch.json')
# The libraries whose callers set function constants by name (QuartzCore: create_function_constants);
# every other caller sets them by index, as the device's journals show. An archive records constants by index alone.
NAMED_CONSTANTS = ('/System/Library/Frameworks/QuartzCore.framework/default.metallib',)
SKIP = ('.png', '.car', '.heic', '.jpg', '.mov', '.m4a', '.caf', '.ttf', '.ttc', '.otf', '.mp4', '.aar', '.lproj')


def owner_of(path: str) -> str:
    parts = path.split('/')
    for part in reversed(parts[:-1]):
        if part.endswith(OWNER_SUFFIXES):
            return part
    return parts[-2] if len(parts) > 1 else path


class Volume:
    """The files of the roots by their path on the device, later roots winning."""

    def __init__(self, roots: list[Path]) -> None:
        self.files: dict[str, Path] = {}
        for root in roots:
            device = root.stat().st_dev
            for directory, dirs, names in os.walk(root):
                dirs[:] = [d for d in dirs if (Path(directory) / d).lstat().st_dev == device and not d.endswith(SKIP)]
                for name in names:
                    path = Path(directory) / name
                    if not name.endswith(SKIP) and _readable(path):
                        self.files['/' + str(path.relative_to(root))] = path


def _readable(path: Path) -> bool:
    return path.is_file() and not path.is_symlink() and os.access(path, os.R_OK)


class Libraries:
    """Every metallib of the volume: which holds a function, by the function's hash or by its name."""

    def __init__(self, volume: Volume) -> None:
        self.by_hash: dict[bytes, str] = {}
        self.by_name: dict[str, list[str]] = {}
        self.constant_names: dict[str, dict[str, dict[int, str]]] = {}
        for path, file in volume.files.items():
            if not path.endswith('.metallib'):
                continue
            data = file.read_bytes()
            library = _air(data)
            if library is None:
                continue
            if path in NAMED_CONSTANTS:
                self.constant_names[path] = {
                    f.name: {c.index: c.name for c in f.constants} for f in metallib.functions(library) or ()
                }
            for tags in gpubin.entries(library):
                name = gpubin.name_of(tags)
                if 'HASH' in tags:
                    self.by_hash.setdefault(tags['HASH'], path)
                self.by_name.setdefault(name, []).append(path)

    def place(self, function: Function | None, near: str, hashed: bytes | None = None) -> bool:
        """Set the function's library: the one of its hash, else the one beside `near` that has it; say if found."""
        if function is None:
            return True
        if hashed and hashed in self.by_hash:
            function.library = self.by_hash[hashed]
            return self._name_constants(function)
        holders = self.by_name.get(function.name, [])
        directory = near.rsplit('/', 1)[0]
        while directory:
            beside = [h for h in holders if h.rsplit('/', 1)[0] == directory]
            if beside:
                function.library = beside[0]
                return self._name_constants(function)
            directory = directory.rsplit('/', 1)[0]
        if len(holders) == 1:
            function.library = holders[0]
            return self._name_constants(function)
        return False

    def _name_constants(self, function: Function) -> bool:
        names = self.constant_names.get(function.library or '', {}).get(function.name)
        for c in function.constants if names else ():
            if c.name is None and c.index in names:
                c.name, c.index = names[c.index], None
        return True


def _air(data: bytes) -> bytes | None:
    """Give the AIR metallib of a file: itself, or the AIR64 slice of a universal one."""
    if data[:4] == gpubin.FAT:
        for i in range(struct.unpack_from('>I', data, 4)[0]):
            _, _, offset, size, _ = struct.unpack_from('>5I', data, 8 + 20 * i)
            if data[offset : offset + 4] == b'MTLB':
                return data[offset : offset + size]
        return None
    return data if data[:4] == b'MTLB' else None


def gather(volume: Volume, libraries: Libraries) -> tuple[list[Pipeline], Counter[str]]:
    """Give every declared state with its functions placed in their libraries, and what was left unresolved."""
    states: list[Pipeline] = []
    unresolved: Counter[str] = Counter()
    for path, file in sorted(volume.files.items()):
        if f'/{DRIVER_BUNDLE}' in path:
            continue
        for p, place in _declared(volume, libraries, path, file):
            for f in (p.vertex, p.fragment, p.kernel, *p.vertex_linked):
                if f is not None and not place(f):
                    unresolved[f'{p.owner} {f.name}'] += 1
            states.append(p)
    return states, unresolved


Placer = Callable[[Function], bool]


def _declared(volume: Volume, libraries: Libraries, path: str, file: Path) -> list[tuple[Pipeline, Placer]]:
    """Give the states the file at `path` declares, each with what places a function of it in its library."""
    owner = owner_of(path)
    if path.endswith('/pipelines.desc'):
        d = desc.Desc(file.read_bytes(), path, owner)
        near = _beside(volume, path, d.libraries[0] if d.libraries else 'default.metallib') or path
        return [(p, lambda f, near=near: libraries.place(f, near)) for p in d.pipelines()]
    if path.endswith(SCRIPT_NAMES):
        states = script.Script(file.read_text(errors='replace'), path, owner).pipelines()
        return [(p, lambda f: libraries.place(f, path)) for p in states]
    with file.open('rb') as fh:
        if not gpubin.has_gpu_binary(fh.read(4096)):
            return []
    out = []
    for binary in gpubin.slices(file.read_bytes()):
        archive = airp.Archive(binary, path, owner)
        out += [(p, lambda f, a=archive: libraries.place(f, path, a.base_hash(f))) for p in archive.pipelines()]
    return out


def _beside(volume: Volume, path: str, name: str) -> str | None:
    """Give the path of `name` in the desc's directory or the nearest one above it (a pipeline library's metallib)."""
    directory = path.rsplit('/', 1)[0]
    while directory:
        if f'{directory}/{name}' in volume.files:
            return f'{directory}/{name}'
        directory = directory.rsplit('/', 1)[0]
    return None


def inventory(states: list[Pipeline]) -> list[tuple[str, str, str, str, str]]:
    """Give one row per owner's distinct state, sorted.

    A row: its kind, owner, first archive, "device" when any of its archives declares it for the raw device's GPU
    families ("other" otherwise), and its line.
    """
    rows: dict[tuple[str, str, str], list[str]] = {}
    for p in sorted(states, key=lambda p: p.source):
        row = rows.setdefault((p.kind, p.owner, p.line()), [p.source, 'other'])
        if p.for_device():
            row[1] = 'device'
    return sorted((kind, owner, source, family, line) for (kind, owner, line), (source, family) in rows.items())


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--root', type=Path, action='append', required=True)
    parser.add_argument('--inventory', type=Path, required=True)
    args = parser.parse_args()
    volume = Volume(args.root)
    states, unresolved = gather(volume, Libraries(volume))
    rows = inventory(states)
    args.inventory.write_text(''.join('\t'.join(row) + '\n' for row in rows))
    for (owner, kind), n in sorted(Counter((row[1], row[0]) for row in rows).items()):
        print(f'{owner} {kind}: {n}')
    print(f'{len(states)} states declared, {len(rows)} distinct', file=sys.stderr)
    for name, n in unresolved.most_common():
        print(f'unresolved: {name} ({n})', file=sys.stderr)


if __name__ == '__main__':
    main()
