"""Join the buckets into the packed seed, check the tools against the GPU bundle, and lay the seed out for an image.

usage: merge.py join --index <bucket index>... --report <bucket report>... [--records <driver record store>]
                     [--replies <compile reply store>] --ipsw <build> --plan <plan.json> --inventory <inventory.json>
                     --out <dir>
       merge.py check --tools-manifest <MANIFEST.json> --twin <tools' libvulkan_nouveau.dylib> --base <path>... --out F
       merge.py tree --seed <join's dir> --pack <bucket pack>... [--raw <raw pack dir>] [--native <packed native dir>]
                     --out <tree dir> --size F

join: the seed is every bucket's cache directory laid over one another (an entry's name is what it holds, so two
buckets' entries of one name are the same bytes), the device's driver records of the tools' NVK identity beside them,
SEED (the line the plugin logs) and MANIFEST.json. It is packed (m2v's shader_cache/seed.rs): bucket N's pack is
packs/<NNN> as the bucket wrote it, the merged records are the last pack, and `index` names every entry's pack, offset
and length. The join reads only the buckets' indexes and reports, never their packs or the tools (the records name
NVK's identity; the stack key, which every raw/ edit changes, is no part of the seed), and writes SEED, MANIFEST.json,
index and packs/<the records' pack> into <out>. The device's Metal compiler replies (the frontend's msl/<name>.reply:
Core Image's kernel sources compiled to AIR by the OS's MTLCompiler, named by its LC_UUID and the request) whose
envelope holds go to <out>/msl/ as they are, where the frontend reads one its own cache lacks.
check: refuses tools that are not the GPU bundle's (--base: its tree or files): their NVK twin must have the bundle
NVK's LC_UUID (nvk_twin.py gives the twin the iOS dylib's), and the bundle's Info.plist must name the tools' stack key
(NVK, m2v, the bridge and the frontend); a seed of other tools would never be hit. A validation action: the seed does
not wait for it.
tree: ngpu/addons/TegraGM20BMetal.m2v-seed/shared (the join's files and packs/<NNN>, and the packed native seed's
native-analysis/) and ngpu/addons/TegraGM20BMetal.raw (with the packed native seed's raw-<stack>/ in native/; of the
packed native seed, only each index and its observed packs go: tools/gpu/seed_pack.py), directories 0755 and files
0644 (APFS clones of the sources), and size.json (bytes, files, blocks4k: what the tree takes on APFS) beside it.
"""

import argparse
import ctypes
import hashlib
import json
import os
import plistlib
import re
import shutil
import struct
import sys
from collections import Counter
from pathlib import Path

from tools.gpu import seed_pack
from tools.gpu.nvk_twin import lc_uuid
from tools.gpu.shaders import replies as compiler_replies

CONTENT = '529a20fd270ababa3e5ec2fe4d1674d93739ba5bcd61861674b4fb6db349af9f'
RECORD_MAGIC = b'M2VPPLC\0'
REPLY_MAGIC = b'NSC1'
REPLY_HEADER = 48
REPLY_NAME = re.compile(r'^[0-9a-f]{64}\.reply$')
SEED = 'ngpu/addons/TegraGM20BMetal.m2v-seed/shared'
RAW = 'ngpu/addons/TegraGM20BMetal.raw'
INDEX_MAGIC = b'M2VSEED2'
INDEX_VERSION = 2
RUN_ENTRIES = 4

# path below the cache directory -> (sha256, size, pack, offset)
Entries = dict[str, tuple[str, int, int, int]]


def read_index(path: Path) -> list[tuple[str, str, int, int]]:
    """Return a bucket index's (path, sha256, size, offset) entries."""
    entries = []
    for line in path.read_text().splitlines():
        relative, digest, size, offset = line.split('\t')
        entries.append((relative, digest, int(size), int(offset)))
    return entries


def record_ok(data: bytes) -> bool:
    """Check a driver record's envelope: magic, identity, a length that ends at the checksum, the checksum."""
    return (
        len(data) >= 76
        and data[:8] == RECORD_MAGIC
        and 44 + int.from_bytes(data[36:44], 'little') + 32 == len(data)
        and hashlib.sha256(data[:-32]).digest() == data[-32:]
    )


reply_ok = compiler_replies.reply_ok


def copy_replies(store: Path | None, out: Path, report: dict | None = None) -> tuple[dict[str, int], list[str]]:
    report = report if report is not None else compiler_replies.inventory(store)
    if store:
        compiler_replies.copy_inventory(store, out, report)
        compiler_replies.verify(out, report)
    counts = {'kept': report['replies'], 'refused': len(report['refused'])}
    kept = [f'msl/{relative}\0{digest}\n' for relative, digest in sorted(report['files'].items())]
    return counts, kept


def union(indexes: list[Path]) -> tuple[Entries, int]:
    """Lay the buckets' indexes over one another (the first to name a file wins); count names two disagree on."""
    entries: Entries = {}
    disagree = 0
    for number, index in enumerate(indexes):
        for relative, digest, size, offset in read_index(index):
            if relative in entries:
                disagree += entries[relative][0] != digest
                continue
            entries[relative] = (digest, size, number, offset)
    return entries, disagree


def merge_records(store: Path | None, pack: Path, number: int, identity: bytes, entries: Entries) -> dict[str, int]:
    """Pack the device's records of `identity` whose envelope holds where the seed has none of that name."""
    counts = dict.fromkeys(('merged', 'here', 'other', 'refused'), 0)
    pack.parent.mkdir(parents=True, exist_ok=True)
    out = pack.open('wb')
    for path in sorted(store.glob('*/*')) if store and store.is_dir() else []:
        data = path.read_bytes()
        relative = f'v1/{CONTENT}/{path.parent.name}/{path.name}'
        kind = (
            'refused'
            if not record_ok(data)
            else 'other'
            if data[8:36] != identity
            else 'here'
            if relative in entries
            else 'merged'
        )
        counts[kind] += 1
        if kind == 'merged':
            entries[relative] = (hashlib.sha256(data).hexdigest(), len(data), number, out.tell())
            out.write(data)
    out.close()
    return counts


def prefix_bits(entries: int) -> int:
    """Return k for 2^k runs of about RUN_ENTRIES entries each (the names' first k bits pick the run)."""
    return min(24, max(0, (entries // RUN_ENTRIES).bit_length()))


def write_index(path: Path, build: str, packs: int, entries: Entries) -> None:
    """Write the packed seed's index: the header, the run table, then each entry sorted by (name, space)."""
    records = []
    for relative, (_, size, pack, offset) in entries.items():
        directory, _, name = relative.split('/')[1:]
        records.append((bytes.fromhex(name), 1 if directory == CONTENT else 0, pack, size, offset))
    records.sort()
    bits = prefix_bits(len(records))
    starts, at = [], 0
    for run in range((1 << bits) + 1):
        while at < len(records) and bits and int.from_bytes(records[at][0][:4], 'big') >> (32 - bits) < run:
            at += 1
        starts.append(at if bits else 0)
    starts[-1] = len(records)
    with path.open('wb') as f:
        f.write(INDEX_MAGIC + struct.pack('<IIQ', INDEX_VERSION, packs, len(records)) + bytes.fromhex(build))
        f.write(struct.pack('<II', bits, 0) + struct.pack(f'<{len(starts)}I', *starts))
        for name, space, pack, size, offset in records:
            f.write(name + struct.pack('<BBHIQ', space, 0, pack, size, offset))


def crawled(inventory: dict) -> dict:
    """Summarize the crawl: what it found, what does not parse, and the Metal sources only the device compiles."""
    kinds = Counter(e['container'] for e in inventory['embedded_libraries'])
    return {
        'files': inventory['files'],
        'embedded': dict(kinds),
        'unreadable': inventory['unreadable'],
        'air': dict(Counter(a for e in inventory['libraries'] for a in e['air']).most_common()),
        'msl_sources': sum(inventory['msl_sources'].values()),
    }


STORE_PATH = re.compile(r'^.*/(store|crawl)/root/')
# The library as the bucket saw it, below its own directory (an older report's absolute sandbox path included).
STORE_IN_LINE = re.compile(r'\((?:[^()\s]*/)?(?:store|crawl)/root/')


def refusals(lines: list[str]) -> dict[str, dict]:
    """Group the tool's refusals (`<kind> <what> (<library>): <error>`) by library: a count and the first few."""
    grouped: dict[str, dict] = {}
    for line in (STORE_IN_LINE.sub('(/', raw) for raw in lines):
        library = line[line.find(' (') + 2 : line.find('): ')] if ' (' in line and '): ' in line else '?'
        library = STORE_PATH.sub('/', library)
        entry = grouped.setdefault(library, {'count': 0, 'first': []})
        entry['count'] += 1
        if len(entry['first']) < 3:
            entry['first'].append(line[:300])
    return dict(sorted(grouped.items(), key=lambda kv: -kv[1]['count']))


def join(args: argparse.Namespace) -> None:
    """Write the seed's own files: SEED, MANIFEST.json, the index and the merged records' pack."""
    out = args.out
    shutil.rmtree(out, ignore_errors=True)
    out.mkdir(parents=True)
    entries, disagree = union(args.index)
    records = len(args.index)
    builds = {relative.split('/')[1] for relative in entries} - {CONTENT}
    if len(builds) != 1:
        sys.exit(f'the buckets made {len(builds)} m2v build directories ({", ".join(sorted(builds))}), not one')
    build = builds.pop()
    reports = [json.loads(path.read_text()) for path in args.report]
    identities = {r['identity'] for r in reports if r.get('identity')}
    if len(identities) != 1:
        sys.exit(f'the buckets made driver records of {len(identities)} identities, not one')
    identity = bytes.fromhex(identities.pop())
    device = merge_records(args.records, out / 'packs' / f'{records:03d}', records, identity, entries)
    digest = hashlib.sha256()
    for relative in sorted(entries):
        digest.update(f'{relative}\0{entries[relative][0]}\n'.encode())
    reply_inventory = compiler_replies.inventory(
        args.replies, getattr(args, 'reply_input', None), getattr(args, 'strict_replies', False)
    )
    replies, kept = copy_replies(args.replies, out / 'msl', reply_inventory)
    for reply in kept:
        digest.update(reply.encode())
    line = f'generated m2v-build {build[:8]} nvk {identity[12:28].hex()[:8]} content {digest.hexdigest()[:8]}'
    meta = {
        'SEED': line + '\n',
        'MANIFEST.json': json.dumps(
            {
                'seed': line,
                'ipsw': args.ipsw,
                'plan': json.loads(args.plan.read_text()),
                'crawl': crawled(json.loads(args.inventory.read_text())),
                'device_records': device,
                'compile_replies': replies,
                'compiler_reply_inventory': reply_inventory,
                'disagreeing_names': disagree,
                'crashed': [line for r in reports for line in r.get('crashed', [])],
                'refused': refusals([line for r in reports for line in r.get('refused', [])]),
            },
            indent=1,
        )
        + '\n',
    }
    for name, text in meta.items():
        (out / name).write_text(text)
    write_index(out / 'index', build, records + 1, entries)
    print(
        f'seed: {line} ({len(entries)} entries from {len(args.index)} buckets; device records {device}; '
        f'compile replies {replies})'
    )


def check(args: argparse.Namespace) -> None:
    """Check the tools are the GPU bundle's; write the stamp."""
    manifest = json.loads(args.tools_manifest.read_text())
    files = [p for b in args.base for p in ([b] if b.is_file() else b.rglob('*')) if p.is_file()]
    info = [p for p in files if p.name == 'Info.plist' and p.parent.name == 'TegraGM20BMetal.bundle']
    nvk = [p for p in files if p.name == 'libvulkan_nouveau.dylib' and p.parent.name == 'TegraGM20BMetal.bundle']
    if len(info) != 1 or len(nvk) != 1:
        sys.exit('--base has no single TegraGM20BMetal.bundle (Info.plist and libvulkan_nouveau.dylib)')
    if lc_uuid(args.twin) != lc_uuid(nvk[0]):
        sys.exit("the shader tools' NVK twin is not the GPU bundle's NVK: tools and bundle come from different builds")
    stack = plistlib.loads(info[0].read_bytes()).get('SwitchShaderStack')
    if stack != manifest['stack']:
        sys.exit(f"the GPU bundle's stack is {stack}, the shader tools' {manifest['stack']}: different builds")
    args.out.write_text(f'tools {manifest["stack"]} match the bundle\n')


_LIBC = ctypes.CDLL(None, use_errno=True)


def clone(source: Path, target: Path) -> None:
    """Copy a file as an APFS clone (no data written), or by bytes where the file system has no clones."""
    if _LIBC.clonefile(os.fsencode(source), os.fsencode(target), 0):
        shutil.copyfile(source, target)
    target.chmod(0o644)


def native_pairs(native: Path, out: Path) -> list[tuple[Path, Path]]:
    """Return the packed native seed's staged files and where they go.

    The host's manifest and coverage beside them and the derived entries' packs stay.
    """
    pairs = []
    for path in sorted(native.rglob('*')):
        relative = path.relative_to(native)
        if not path.is_file() or len(relative.parts) < 2:
            continue
        top = relative.parts[0]
        if top == 'native-analysis':
            under = out / SEED
        elif top.startswith('raw-'):
            under = out / RAW / 'native'
        else:
            continue
        if seed_pack.staged(native / top, relative.relative_to(top).as_posix()):
            pairs.append((path, under / relative))
    return pairs


def tree(args: argparse.Namespace) -> None:
    """Lay the seed and the raw pack out under ngpu/addons, and write the size."""
    out = args.out
    shutil.rmtree(out, ignore_errors=True)
    pairs = [(p, out / SEED / p.relative_to(args.seed)) for p in sorted(args.seed.rglob('*')) if p.is_file()]
    pairs += [(pack, out / SEED / 'packs' / f'{number:03d}') for number, pack in enumerate(args.pack)]
    if args.raw:
        pairs += [(p, out / RAW / p.relative_to(args.raw)) for p in sorted(args.raw.rglob('*')) if p.is_file()]
    if args.native:
        pairs += native_pairs(args.native, out)
    for source, target in pairs:
        target.parent.mkdir(parents=True, exist_ok=True)
        clone(source, target)
    for path in [out, *(p for p in out.rglob('*') if p.is_dir())]:
        path.chmod(0o755)
    manifest_path = out / SEED / 'MANIFEST.json'
    if manifest_path.is_file():
        reply_inventory = json.loads(manifest_path.read_text()).get('compiler_reply_inventory')
        if reply_inventory is not None:
            compiler_replies.verify(out / SEED / 'msl', reply_inventory)
    sizes = [target.stat().st_size for _, target in pairs]
    args.size.write_text(
        json.dumps({'bytes': sum(sizes), 'files': len(sizes), 'blocks4k': sum((s + 4095) // 4096 for s in sizes)})
        + '\n'
    )


def main() -> None:
    """Run one of the three steps."""
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    steps = parser.add_subparsers(dest='step', required=True)
    j = steps.add_parser('join')
    j.add_argument('--index', type=Path, action='append', required=True)
    j.add_argument('--report', type=Path, action='append', required=True)
    j.add_argument('--records', type=Path)
    j.add_argument('--replies', type=Path)
    j.add_argument('--reply-input', type=Path, action='append')
    j.add_argument('--strict-replies', action='store_true')
    for name in ('plan', 'inventory', 'out'):
        j.add_argument('--' + name, type=Path, required=True)
    j.add_argument('--ipsw', required=True)
    c = steps.add_parser('check')
    for name in ('tools-manifest', 'twin', 'out'):
        c.add_argument('--' + name, type=Path, required=True)
    c.add_argument('--base', type=Path, action='append', required=True)
    t = steps.add_parser('tree')
    t.add_argument('--seed', type=Path, required=True)
    t.add_argument('--pack', type=Path, action='append', required=True)
    t.add_argument('--raw', type=Path)
    t.add_argument('--native', type=Path)
    t.add_argument('--out', type=Path, required=True)
    t.add_argument('--size', type=Path, required=True)
    args = parser.parse_args()
    {'join': join, 'check': check, 'tree': tree}[args.step](args)


if __name__ == '__main__':
    main()
