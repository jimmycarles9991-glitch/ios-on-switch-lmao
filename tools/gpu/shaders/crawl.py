"""Every Metal library of an IPSW's volume images: the files, and the ones embedded in other files.

usage: crawl.py --image <dmg>... --out <dir> --inventory <inventory.json>

Each image is attached read-only (one already attached is read where it is, and left attached). Later images win a
path both hold: the SystemOS cryptex is grafted over the OS volume.

- <out>/root/<path>: every file that is a Metal library with a function, at its path below the volume's root (a fat
  file whose slices are none of them MTLB is scanned as any other binary).
- <out>/data/<sha256>.metallib: every library embedded in a CPU binary (the dyld shared cache's images, executables),
  which a process can only make from bytes.
- inventory.json: every library found, also those the seed leaves out (GPU binary archives and pipeline libraries,
  which the frontend refuses, so no process makes them), each with its function types and AIR versions over every
  AIR slice of a fat file (and the slices, when there are several); every MTLB that does not parse, and where; the
  Metal source strings embedded in binaries, which only the device's own compiler can turn into AIR.
"""

import argparse
import contextlib
import hashlib
import json
import mmap
import os
import plistlib
import shutil
import struct
import subprocess
import sys
from collections import Counter
from collections.abc import Iterator
from pathlib import Path

from tools.gpu.shaders.metallib import FAT_MAGIC, HEADER, MAGIC, functions, slices, thin

HDIUTIL = '/usr/bin/hdiutil'
MACHO = b'\xcf\xfa\xed\xfe'
GPU_CPU_TYPE = 0x01000013
PIPELINE_LIBRARY = b'LPPM'
MSL = b'#include <metal_stdlib>'
SKIP = ('.png', '.car', '.heic', '.jpg', '.mov', '.m4a', '.caf', '.ttf', '.ttc', '.otf', '.mp4', '.aar', '.lproj')


def _attached(image: Path) -> Path | None:
    info = plistlib.loads(subprocess.run([HDIUTIL, 'info', '-plist'], capture_output=True, check=True).stdout)
    for entry in info.get('images', []):
        if Path(entry.get('image-path', '')).resolve() == image.resolve():
            points = [e['mount-point'] for e in entry.get('system-entities', []) if e.get('mount-point')]
            if points:
                return Path(points[0])
    return None


@contextlib.contextmanager
def mounted(image: Path, point: Path) -> Iterator[Path]:
    """Attach `image` read-only at `point` for the block, or use where it is already attached."""
    point.mkdir(parents=True)
    attach = [HDIUTIL, 'attach', '-readonly', '-nobrowse', '-noverify', '-noautofsck', '-noautoopen', '-mountpoint']
    result = subprocess.run([*attach, str(point), str(image)], capture_output=True, text=True, check=False)
    if result.returncode:
        existing = _attached(image)
        if existing is None:
            sys.exit(f'hdiutil attach {image}: {result.stderr.strip()}')
        yield existing
        return
    try:
        yield point
    finally:
        subprocess.run([HDIUTIL, 'detach', '-force', str(point)], capture_output=True, check=False)


def _library_entry(data: bytes, where: str) -> dict:
    found = {subtype: functions(library) or [] for subtype, library in slices(data)}
    every = [f for fs in found.values() for f in fs]
    entry = {
        'where': where,
        'sha256': hashlib.sha256(data).hexdigest(),
        'bytes': len(data),
        'types': dict(Counter(str(f.type) for f in every)),
        'air': sorted({f.air for f in every if f.air}),
    }
    if len(found) > 1:
        entry['slices'] = [{'cpusubtype': subtype, 'functions': len(fs)} for subtype, fs in found.items()]
    return entry


def _embedded(data: mmap.mmap) -> Iterator[tuple[int, bytes]]:
    at = 0
    while (at := data.find(MAGIC, at)) >= 0:
        if at + HEADER <= len(data):
            size, listing = struct.unpack_from('<QQ', data, at + 0x10)
            if HEADER <= size <= len(data) - at and listing == HEADER:
                yield at, data[at : at + size]
                at += size
                continue
        at += 4


def _container(head: bytes) -> str:
    if head[:4] == PIPELINE_LIBRARY:
        return 'pipeline-library'
    if head[:4] == MACHO and struct.unpack_from('<I', head, 4)[0] == GPU_CPU_TYPE:
        return 'gpu-archive'
    return 'binary'


class Crawl:
    """What the images hold, gathered into the output tree and the inventory."""

    def __init__(self, out: Path) -> None:
        """Gather into `out`."""
        self.out = out
        self.files: dict[str, dict] = {}
        self.embedded: dict[str, dict] = {}
        self.unreadable: list[dict] = []
        self.sources: Counter[str] = Counter()

    def file(self, path: Path, relative: str) -> None:
        """Take a library file, or scan any other file for embedded libraries."""
        with path.open('rb') as f:
            head = f.read(8)
            data = path.read_bytes() if head[:4] == FAT_MAGIC else None
            if head[:4] != MAGIC and (data is None or thin(data) is None):
                self.scan(f, relative, head)
                return
        data = data or path.read_bytes()
        found = functions(data)
        if found is None:
            self.unreadable.append({'where': relative, 'why': 'function list does not parse'})
            return
        if not found:
            return
        target = self.out / 'root' / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
        self.files[relative] = _library_entry(data, relative)

    def scan(self, f: object, relative: str, head: bytes) -> None:
        """Take the libraries embedded in a file, and count its Metal source strings."""
        try:
            data = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
        except ValueError:
            return
        with data:
            container = _container(head)
            for at, library in _embedded(data):
                where = f'{relative}@{at:#x}'
                if functions(library) is None:
                    self.unreadable.append({'where': where, 'why': 'function list does not parse'})
                    continue
                entry = _library_entry(library, where) | {'container': container}
                self.embedded.setdefault(entry['sha256'], entry)
                if container == 'binary':
                    (self.out / 'data').mkdir(parents=True, exist_ok=True)
                    (self.out / 'data' / (entry['sha256'] + '.metallib')).write_bytes(library)
            at = data.find(MSL)
            while at >= 0:
                self.sources[relative] += 1
                at = data.find(MSL, at + len(MSL))

    def volume(self, root: Path) -> None:
        """Take every regular file of the volume mounted at `root`, not crossing into other volumes."""
        device = root.stat().st_dev
        for directory, dirs, names in os.walk(root):
            dirs[:] = [d for d in dirs if (Path(directory) / d).lstat().st_dev == device and not d.endswith(SKIP)]
            for name in names:
                path = Path(directory) / name
                if name.endswith(SKIP) or not path.is_file() or path.is_symlink() or path.stat().st_size < HEADER:
                    continue
                self.file(path, str(path.relative_to(root)))

    def inventory(self) -> dict:
        """Return what was found, for inventory.json."""
        seeded = sum(1 for e in self.embedded.values() if e['container'] == 'binary')
        return {
            'files': len(self.files),
            'embedded': len(self.embedded),
            'embedded_seeded': seeded,
            'unreadable': self.unreadable,
            'msl_sources': dict(sorted(self.sources.items())),
            'libraries': sorted(self.files.values(), key=lambda e: e['where']),
            'embedded_libraries': sorted(self.embedded.values(), key=lambda e: e['where']),
        }


def main() -> None:
    """Crawl every image into <out> and write the inventory."""
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--image', type=Path, action='append', required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--inventory', type=Path, required=True)
    args = parser.parse_args()
    shutil.rmtree(args.out, ignore_errors=True)
    (args.out / 'root').mkdir(parents=True)
    (args.out / 'data').mkdir()
    crawl = Crawl(args.out)
    work = Path.cwd() / '_crawl_mounts'
    shutil.rmtree(work, ignore_errors=True)
    for i, image in enumerate(args.image):
        with mounted(image, work / str(i)) as root:
            crawl.volume(root)
    shutil.rmtree(work, ignore_errors=True)
    inventory = crawl.inventory()
    args.inventory.write_text(json.dumps(inventory, indent=1) + '\n')
    print(
        f'crawl: {inventory["files"]} library files, {inventory["embedded"]} embedded ({inventory["embedded_seeded"]} '
        f'in CPU binaries), {len(inventory["unreadable"])} unreadable, {sum(crawl.sources.values())} Metal sources'
    )


if __name__ == '__main__':
    main()
