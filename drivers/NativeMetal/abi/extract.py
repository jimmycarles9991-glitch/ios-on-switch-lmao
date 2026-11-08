"""kinds/<kind>.json: the Metal.framework ABI the frontend depends on, read out of a kind's SystemOS dyld cache.

    bazel run [--config=ios27] //drivers/NativeMetal/abi:extract    (rewrites the selected kind's table)
    bazel run [--config=ios27] //drivers/NativeMetal/abi:verify     (compares it)
    extract.py --cache <dyld_shared_cache_arm64[e]> --kind <kind> [--check]

The frontend's device is a dynamic subclass of _MTLDevice that adds no instance variables and writes its answers
into the base class's limits table, so the table holds _MTLDevice's instance size and every method it has, and the
limits table's layout from the _limits ivar's own type encoding. It also holds the other base classes' sizes, the
private methods the frontend calls (interface.json), and, per Metal object, the protocol methods the system's own
code sends (sends.py): the contract a device of this kind is held to. The same cache always gives the same JSON.
"""

import argparse
import contextlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from collections.abc import Iterator
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[3]))
from drivers.NativeMetal.abi.dyld_cache import CacheError, DyldCache
from drivers.NativeMetal.abi.objc_metadata import ObjcReader
from drivers.NativeMetal.abi.sends import SendScanner

HERE = Path(__file__).resolve().parent
METAL = '/System/Library/Frameworks/Metal.framework/Metal'
CACHE_DIR = 'System/Library/Caches/com.apple.dyld'
HDIUTIL = '/usr/bin/hdiutil'
SCALARS = {'c': 1, 'C': 1, 'B': 1, 's': 2, 'S': 2, 'i': 4, 'I': 4, 'f': 4, 'q': 8, 'Q': 8, 'd': 8}
COMPILER = re.compile(r'/System/Library/PrivateFrameworks/MTLCompiler\.framework/(?:Versions/(\d+)/)?MTLCompiler')
INTEGER_FIELDS = ('I', 'Q')
INTEGER_RETURNS = ('Q', 'I', 'L')
JsonObject = dict[str, object]


def struct_fields(encoding: str) -> tuple[list[tuple[str, str, int]], int]:
    """Lay out a named-field struct encoding ({?="name"T...}): its fields at their natural offsets, and its size."""
    body = re.fullmatch(r'\{[^=]*=(.*)\}', encoding)
    if not body:
        raise ValueError(f'not a struct: {encoding}')
    fields, offset, widest = [], 0, 1
    for name, kind in re.findall(r'"(\w+)"(.)', body.group(1)):
        size = SCALARS.get(kind)
        if size is None:
            raise ValueError(f'{name}: type {kind} is not a scalar')
        offset = (offset + size - 1) // size * size
        fields.append((name, kind, offset))
        offset += size
        widest = max(widest, size)
    return fields, (offset + widest - 1) // widest * widest


def metal_subclasses(reader: ObjcReader, classes: dict[str, int], name: str) -> list[str]:
    """Return Metal's own classes below `name`, in name order."""
    parents = {cls: reader.class_name(reader.superclass(at)) for cls, at in classes.items() if reader.superclass(at)}
    below, pending = [], [name]
    while pending:
        parent = pending.pop()
        children = sorted(cls for cls, of in parents.items() if of == parent)
        below += children
        pending += children
    return sorted(below)


def device_table(reader: ObjcReader, classes: dict[str, int], interface: JsonObject) -> JsonObject:
    device = reader.read_class(classes[interface['class']])
    ivar = next(i for i in device.ivars if i.name == interface['limits_ivar'])
    fields, size = struct_fields(ivar.type)
    if size != ivar.size:
        raise SystemExit(f'{ivar.name}: its encoding lays out {size} bytes, the ivar is {ivar.size}')
    methods = {m.name: m.types for m in device.methods}
    accessors = dict(methods)
    for cls in metal_subclasses(reader, classes, device.name):
        accessors |= {m.name: m.types for m in reader.read_class(classes[cls]).methods}
    return {
        'class': device.name,
        'superclass': device.superclass,
        'instance_size': device.instance_size,
        'limits_ivar': ivar.offset,
        'limits_bytes': ivar.size,
        'limits': {
            name: offset
            for name, kind, offset in fields
            if kind in INTEGER_FIELDS and accessors.get(name, '').startswith(INTEGER_RETURNS)
        },
        'limits_fields': {name: [offset, kind] for name, kind, offset in fields},
        'methods': dict(sorted(methods.items())),
    }


def class_tables(reader: ObjcReader, classes: dict[str, int], interface: dict[str, list[str]]) -> JsonObject:
    tables = {}
    for name, wanted in sorted(interface.items()):
        cls = reader.read_class(classes[name])
        methods = {m.name: m.types for m in cls.methods}
        missing = [m for m in wanted if m not in methods]
        if missing:
            raise SystemExit(f'{name} has no {", ".join(missing)}')
        tables[name] = {'superclass': cls.superclass, 'instance_size': cls.instance_size}
        if wanted:
            tables[name]['methods'] = {m: methods[m] for m in sorted(wanted)}
    return tables


def protocols(cache: DyldCache, reader: ObjcReader) -> dict[str, dict]:
    """Read every Metal protocol any image defines, each as its copy with the most methods.

    Each image holds the copy of the SDK it was built with.
    """
    found: dict[str, dict] = {}
    for image in cache.images:
        try:
            listed = reader.protocols(image)
        except CacheError:
            continue
        for name, address in listed.items():
            if not name.startswith('MTL'):
                continue
            protocol = reader.read_protocol(address)
            methods = {m.name: m.types for m in protocol.optional} | {m.name: m.types for m in protocol.required}
            if name not in found or len(methods) > len(found[name]['methods']):
                found[name] = {'protocols': protocol.protocols, 'methods': methods}
    return found


def closure(found: dict[str, dict], names: list[str]) -> dict[str, str]:
    methods: dict[str, str] = {}
    pending, seen = list(names), {'NSObject'}
    while pending:
        name = pending.pop()
        if name in seen or name not in found:
            continue
        seen.add(name)
        pending += found[name]['protocols']
        methods = found[name]['methods'] | methods
    return methods


_WORKER: dict[str, SendScanner] = {}


def _start_worker(path: str) -> None:
    _WORKER['scanner'] = SendScanner(DyldCache(Path(path)))


def _sent(image: str) -> list[str]:
    try:
        return sorted(_WORKER['scanner'].sent(image))
    except CacheError:
        return []


def system_sends(cache_path: Path, cache: DyldCache, not_senders: list[str]) -> set[str]:
    images = [i for i in cache.images if not i.startswith(tuple(not_senders))]
    with ProcessPoolExecutor(os.cpu_count(), initializer=_start_worker, initargs=(str(cache_path),)) as pool:
        return {s for sent in pool.map(_sent, images, chunksize=16) for s in sent}


def compiler_image(cache: DyldCache) -> str:
    """Return the path of the MTLCompiler image Metal itself loads, which the frontend's in-process compiler opens."""
    found = {}
    for image in cache.images:
        match = COMPILER.fullmatch(image)
        if match:
            found[int(match.group(1) or 0)] = image
    if not found:
        raise SystemExit('the cache has no MTLCompiler image')
    return found[max(found)]


def extract(cache_path: Path, kind: str) -> str:
    interface = json.loads((HERE / 'interface.json').read_text())
    cache = DyldCache(cache_path)
    reader = ObjcReader(cache)
    classes = reader.classes(METAL)
    found = protocols(cache, reader)
    sent = system_sends(cache_path, cache, interface['not_senders'])
    table = {
        'kind': kind,
        'MTLDevice': device_table(reader, classes, interface['device']),
        'classes': class_tables(reader, classes, interface['classes']),
        'compiler': compiler_image(cache),
        'sent': {
            name: {s: t for s, t in sorted(closure(found, names).items()) if s in sent}
            for name, names in sorted(interface['objects'].items())
        },
        'found': {
            'compiler': 'the MTLCompiler image Metal loads: the highest Versions/<n> the cache holds '
            '(getMaxSupportedLLVMVersion), else the unversioned one',
            'instance_size': "_MTLDevice's class_ro_t instanceSize",
            'limits': "the _limits ivar's offset, size and type encoding; the integer fields with an accessor in "
            "_MTLDevice or a Metal class below it (maxBufferLength: MTLIOAccelDevice's only)",
            'sent': "each object's protocols' methods that some image's code sends (sends.py), Metal's own and the GPU "
            "drivers' and tools' aside (interface.json not_senders)",
        },
    }
    return json.dumps(table, indent=2) + '\n'


def kind_of(cryptex: Path, pins: Path) -> str:
    scope: dict = {}
    exec(compile(pins.read_text(), str(pins), 'exec'), {'True': True, 'False': False}, scope)
    kinds = [k for k, v in scope['IPSWS'].items() if v.get('volumes', {}).get('system_cryptex_image') == cryptex.name]
    if len(kinds) != 1:
        raise SystemExit(f'{cryptex.name}: no pinned IPSW kind has this SystemOS cryptex')
    return kinds[0]


@contextlib.contextmanager
def mounted(image: Path) -> Iterator[Path]:
    """Attach `image` read-only for the block.

    macOS attaches an image once: one attached elsewhere is cloned (an APFS clone, no copy) and the clone attached.
    """
    with tempfile.TemporaryDirectory(prefix='switch-metal-abi-') as scratch:
        point = Path(scratch) / 'mnt'
        point.mkdir()
        attach = [HDIUTIL, 'attach', '-readonly', '-nobrowse', '-noverify', '-noautofsck', '-mountpoint', str(point)]
        if subprocess.run([*attach, str(image)], capture_output=True, check=False).returncode:
            clone = Path(scratch) / image.name
            subprocess.run(['/bin/cp', '-c', str(image), str(clone)], check=True)
            subprocess.run([*attach, str(clone)], capture_output=True, check=True)
        try:
            yield point
        finally:
            subprocess.run([HDIUTIL, 'detach', '-force', str(point)], capture_output=True, check=False)
            shutil.rmtree(scratch, ignore_errors=True)


def main_cache(root: Path) -> Path:
    return next(p for p in sorted((root / CACHE_DIR).glob('dyld_shared_cache_arm64*')) if '.' not in p.name)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument('--cryptex', type=Path, help="the kind's decrypted SystemOS cryptex image")
    source.add_argument('--cache', type=Path, help='the main file of a mounted dyld shared cache')
    parser.add_argument('--kind', help='the kind of --cache (with --cryptex, the pins name it)')
    parser.add_argument('--pins', type=Path, default=HERE.parents[2] / 'ipsw/pins.bzl')
    workspace = os.environ.get('BUILD_WORKSPACE_DIRECTORY')
    parser.add_argument(
        '--tables', type=Path, default=Path(workspace or HERE.parents[2]) / 'drivers/NativeMetal/abi/kinds'
    )
    parser.add_argument('--check', action='store_true', help='compare with the committed table instead of writing')
    args = parser.parse_args()
    with contextlib.ExitStack() as stack:
        if args.cryptex:
            args.kind = kind_of(args.cryptex, args.pins)
            cache = main_cache(stack.enter_context(mounted(args.cryptex)))
        elif not args.kind:
            raise SystemExit('--cache needs --kind')
        else:
            cache = args.cache
        text = extract(cache, args.kind)
    path = args.tables / f'{args.kind}.json'
    if not args.check:
        path.write_text(text)
    elif not path.exists() or path.read_text() != text:
        raise SystemExit(
            f"{path}: differs from {args.kind}'s Metal.framework (bazel run //drivers/NativeMetal/abi:extract)"
        )


if __name__ == '__main__':
    main()
