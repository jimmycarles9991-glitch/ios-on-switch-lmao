"""Translate one mounted volume (the System volume, the SystemOS cryptex or the App cryptex) offline.

    translate_tree --role ROLE --tool arm64e --volume MNT --out DIR [--apple-trustcache IM4P]... [--progress]

The tree is read-only (a decrypted image mounted on the Mac). For every Mach-O whose translation changes bytes, the
replacement file goes to DIR under its volume path and an entry to DIR/<role>.manifest.json. The new cdhashes become
DIR/<role>.trustcache, a version-2 module carrying each binary's launch-constraint category from Apple's own trust
caches. For the SystemOS cryptex the dyld shared cache is translated with its plan (made here, DIR/<role>.a64xplan),
the prebuilt-loader cdhashes of on-disk binaries are updated to their translations, and the translated cache files'
cdhashes go to DIR/a64x-cache-pretranslated.bin for the device tree.

Each file is translated by one `arm64e` subprocess, skipped when DIR already holds its output for this input and
translator version (content addressed): reruns resume. It does not mount or write outside DIR. --progress prints
'progress DONE/TOTAL files' as it goes and 'progress WHAT' before a long wait, for the image builder to show.
"""

import argparse
import concurrent.futures
import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys
from collections.abc import Callable, Iterable
from pathlib import Path
from typing import NamedTuple

from tools.common import files, hashing
from tools.trustcache.trustcache import trust_module

CACHE_DIR = 'System/Library/Caches/com.apple.dyld'
MACHO_MAGIC = (b'\xcf\xfa\xed\xfe', b'\xca\xfe\xba\xbe', b'\xca\xfe\xba\xbf')  # thin 64, fat BE, fat64 BE


def key_sha256(path: Path) -> str | None:
    try:
        return hashing.sha256_file(path)
    except OSError:
        return None


def is_macho(path: Path) -> bool:
    try:
        with open(path, 'rb') as f:
            return f.read(4) in MACHO_MAGIC
    except OSError:
        return False


def apple_trust_entries(paths: Iterable[str]) -> dict[str, tuple[int, int]]:
    """Cdhash (hex) -> (flags, category) from Apple's trust cache IM4Ps (version-2 modules)."""
    out = {}
    for p in paths:
        data = Path(p).read_bytes()
        payload = _im4p_payload(data)
        version = struct.unpack_from('<I', payload)[0]
        count = struct.unpack_from('<I', payload, 20)[0]
        stride = 24 if version == 2 else 22
        for i in range(count):
            e = payload[24 + stride * i : 24 + stride * (i + 1)]
            out[e[:20].hex()] = (e[21], e[22] if version == 2 else 0)
    return out


def _im4p_payload(data: bytes) -> bytes:
    def tlv(at: int) -> tuple[int, int, int]:
        tag, length = data[at], data[at + 1]
        at += 2
        if length & 0x80:
            n = length & 0x7F
            length = int.from_bytes(data[at : at + n], 'big')
            at += n
        return tag, at, length

    _, at, length = tlv(0)
    end = at + length
    fields = []
    while at < end:
        tag, start, length = tlv(at)
        fields.append((tag, data[start : start + length]))
        at = start + length
    return next(v for t, v in fields[1:] if t == 0x04)


def run_tool(tool: str, *args: str) -> str:
    return subprocess.run([tool, *args], capture_output=True, text=True, check=True).stdout


def pretranslate_file(tool: str, src: Path, out_path: Path, report_path: Path) -> dict:
    out_path.parent.mkdir(parents=True, exist_ok=True)
    run_tool(tool, 'pretranslate', '-r', str(report_path), str(src), str(out_path))
    return json.loads(report_path.read_text())


class Translator(NamedTuple):
    tool: str
    version: str


def _memoized(translator: Translator, src: Path, dst: Path, memo: Path, in_sha: str) -> dict:
    """Translate src, or reuse an earlier run whose report records the same input and translator (resume)."""
    if memo.exists():
        prev = json.loads(memo.read_text())
        if prev.get('_in') == in_sha and prev.get('_tool') == translator.version:
            translated = any(s['status'] == 'translated' for s in prev['slices'])
            if translated == dst.exists():
                return prev
    r = pretranslate_file(translator.tool, src, dst, memo)
    r['_in'] = in_sha
    r['_tool'] = translator.version
    memo.write_text(json.dumps(r))
    return r


class Memo(NamedTuple):
    """Translations kept across runs, each under the hash of every input that decides it.

    A key part that is None (an input that could not be hashed) is a miss: produce runs, and nothing is kept.
    """

    root: Path

    def run(self, parts: list[str | None], dst: Path, produce: Callable[[], dict]) -> dict:
        """Return produce()'s record with its dst written, restored from an earlier run of the same key if kept."""
        if any(p is None for p in parts):
            return produce()
        key = hashlib.sha256('\0'.join(parts).encode()).hexdigest()
        record, data = self.root / f'{key}.json', self.root / f'{key}.out'
        kept = json.loads(record.read_text()) if record.is_file() else {}
        if '_sha_out' in kept:
            sha_out = kept.pop('_sha_out')
            if sha_out is None:
                dst.unlink(missing_ok=True)
                return kept
            if data.is_file() and hashing.sha256_file(data) == sha_out:
                dst.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(data, dst)
                return kept
        result = produce()
        self.root.mkdir(parents=True, exist_ok=True)
        sha_out = None
        if dst.is_file():
            shutil.copyfile(dst, data)
            sha_out = hashing.sha256_file(data)
        files.write_text(record, json.dumps(dict(result, _sha_out=sha_out)))
        return result


def _external_pairs(paths: Iterable[str]) -> list[tuple[str, str]]:
    pairs = []
    for p in paths:
        for line in Path(p).read_text().splitlines():
            old, new = line.split()
            pairs.append((old, new))
    return pairs


def _report(args: argparse.Namespace, what: str) -> None:
    if args.progress:
        print('progress', what, flush=True)


def _machos(volume: Path, cache: Path) -> list[Path]:
    machos = []
    for root, _, names in os.walk(volume):
        if os.path.abspath(root).startswith(os.path.abspath(cache)):
            continue
        for name in names:
            p = Path(root) / name
            if not p.is_symlink() and is_macho(p):
                machos.append(p)
    return machos


def _arguments() -> argparse.Namespace:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--role', required=True, choices=('os', 'system_cryptex', 'app_cryptex'))
    ap.add_argument('--tool', required=True, help='the arm64e CLI')
    ap.add_argument('--volume', required=True, type=Path, help='the mounted, read-only volume')
    ap.add_argument('--out', required=True, type=Path)
    ap.add_argument('--apple-trustcache', action='append', default=[], help='Apple trust cache IM4P (flags/category)')
    ap.add_argument(
        '--cdhash-pairs',
        action='append',
        default=[],
        help="another role's <role>.cdhash-pairs, for the cache's prebuilt loaders",
    )
    ap.add_argument(
        '--memo', type=Path, help='keep translations here across runs, keyed by their inputs (default: none)'
    )
    ap.add_argument('--progress', action='store_true', help='print how far it is (progress DONE/TOTAL files)')
    return ap.parse_args()


def main() -> None:
    args = _arguments()
    memo = Memo(args.memo) if args.memo else None

    out = args.out
    out.mkdir(parents=True, exist_ok=True)
    work = out / '.work'
    work.mkdir(exist_ok=True)
    translator = Translator(args.tool, _tool_version(args.tool))
    apple = apple_trust_entries(args.apple_trustcache)

    manifest = {'role': args.role, 'translator': translator.version, 'files': []}
    trust = []  # (cdhash_bytes, flags, category)
    # (old_hex, new_hex): prebuilt loaders in the cache validate on-disk binaries of every volume, so the cache apply
    # takes this role's pairs and those of the roles run before it (os and app_cryptex, for system_cryptex).
    pairs = []
    external_pairs = _external_pairs(args.cdhash_pairs)
    no_category = 0

    cache = args.volume / CACHE_DIR
    _report(args, 'finding the Mach-O files')
    machos = sorted(_machos(args.volume, cache))

    def translate(src: Path) -> tuple[Path, Path, str, dict]:
        rel = src.relative_to(args.volume)
        dst = out / 'files' / rel
        in_sha = hashing.sha256_file(src)
        record = work / (str(rel).replace('/', '_') + '.json')
        if memo:
            r = memo.run(
                ['file', in_sha, translator.version],
                dst,
                lambda: pretranslate_file(translator.tool, src, dst, record.with_suffix('.report.json')),
            )
        else:
            r = _memoized(translator, src, dst, record, in_sha)
        return rel, dst, in_sha, r

    for rel, dst, in_sha, r in _in_order(args, translate, machos, 'files'):
        translated = [s for s in r['slices'] if s['status'] == 'translated']
        if not translated:
            dst.unlink(missing_ok=True)
            continue
        manifest['files'].append(
            {
                'path': '/' + str(rel),
                'file': os.path.relpath(dst, out),
                'sha256_in': in_sha,
                'sha256_out': hashing.sha256_file(dst),
                'cdhash': [s['cdhash_out'] for s in translated],
            }
        )
        for s in translated:
            pairs.append((s['cdhash_in'], s['cdhash_out']))
            if s['cdhash_in'] not in apple:
                no_category += 1
            flags, cat = apple.get(s['cdhash_in'], (0, 0))
            trust.append((bytes.fromhex(s['cdhash_out']), flags, cat))

    (out / f'{args.role}.cdhash-pairs').write_text(''.join(f'{a} {b}\n' for a, b in pairs))
    if args.role == 'system_cryptex' and cache.is_dir():
        _translate_cache(args, pairs + external_pairs, trust, apple, manifest)

    (out / f'{args.role}.manifest.json').write_text(json.dumps(manifest, indent=1))
    (out / f'{args.role}.trustcache').write_bytes(trust_module(trust))
    if no_category:
        print(
            f"{args.role}: WARNING {no_category} binaries not in Apple's trust caches (trust category 0)",
            file=sys.stderr,
        )
    print(f'{args.role}: {len(manifest["files"])} files translated, {len(trust)} trust entries')


def _translate_cache(
    args: argparse.Namespace,
    pairs: list[tuple[str, str]],
    trust: list[tuple[bytes, int, int]],
    apple: dict[str, tuple[int, int]],
    manifest: dict,
) -> None:
    cache, out = args.volume / CACHE_DIR, args.out
    memo, version = (Memo(args.memo) if args.memo else None), _tool_version(args.tool)
    main_cache = cache / 'dyld_shared_cache_arm64e'
    plan = out / f'{args.role}.a64xplan'

    def make_plan() -> dict:
        run_tool(args.tool, 'plan', '-o', str(plan), str(main_cache))
        return {}

    _report(args, 'planning the dyld shared cache')
    if memo:
        memo.run(['plan', key_sha256(main_cache), version], plan, make_plan)
    else:
        make_plan()
    pairs_file = out / '.work' / 'cache_pairs.txt'
    pairs_file.write_text(''.join(f'{a} {b}\n' for a, b in pairs))

    suffixes = _subcache_suffixes(main_cache)
    names = ['dyld_shared_cache_arm64e'] + [f'dyld_shared_cache_arm64e{s}' for s in suffixes]
    sources = [cache / n for n in names if not n.endswith(('.symbols', '.atlas')) and (cache / n).exists()]

    def apply(src: Path) -> tuple[Path, Path, str, str, str]:
        dst = out / 'files' / CACHE_DIR / src.name
        dst.parent.mkdir(parents=True, exist_ok=True)
        sha_in = hashing.sha256_file(src)

        def run() -> dict:
            line = run_tool(args.tool, 'dsc-apply', '-c', str(pairs_file), str(plan), str(src), str(dst))
            return {'cdhash': line.split()[0]}

        if memo:
            cdhash = memo.run(['dsc', sha_in, key_sha256(plan), key_sha256(pairs_file), version], dst, run)['cdhash']
        else:
            cdhash = run()['cdhash']
        return src, dst, cdhash, sha_in, hashing.sha256_file(dst)

    cdhashes = []
    for src, dst, cdhash, sha_in, sha_out in _in_order(args, apply, sources, 'dyld cache files'):
        cdhashes.append(bytes.fromhex(cdhash))
        if sha_out == sha_in:
            dst.unlink()
            continue
        manifest['files'].append(
            {
                'path': '/' + str(dst.relative_to(out / 'files')),
                'file': os.path.relpath(dst, out),
                'sha256_in': sha_in,
                'sha256_out': sha_out,
                'cdhash': [cdhash],
            }
        )
        flags, cat = apple.get(_cache_cdhash_in(src), (0, 0))
        trust.append((bytes.fromhex(cdhash), flags, cat))
    (out / 'a64x-cache-pretranslated.bin').write_bytes(b''.join(cdhashes))


def _in_order[T, R](args: argparse.Namespace, work: Callable[[T], R], items: list[T], unit: str) -> list[R]:
    """Run work(item) for every item on every core, each its own arm64e process; return the results in items' order.

    The arm64e tool is single-threaded, and each item writes only its own outputs, so they run side by side; folding
    the results in the given order keeps the manifest, trust module and cdhash lists byte for byte the serial run's.
    """
    results: dict[int, R] = {}
    with concurrent.futures.ThreadPoolExecutor(os.cpu_count() or 1) as pool:
        pending = {pool.submit(work, item): i for i, item in enumerate(items)}
        _report(args, f'0/{len(items)} {unit}')
        for future in concurrent.futures.as_completed(pending):
            results[pending[future]] = future.result()
            _report(args, f'{len(results)}/{len(items)} {unit}')
    return [results[i] for i in range(len(items))]


def _subcache_suffixes(main_cache: Path) -> list[str]:
    data = main_cache.read_bytes()
    off, count = struct.unpack_from('<II', data, 0x188)
    return [data[off + 56 * i + 24 : off + 56 * i + 56].split(b'\0')[0].decode() for i in range(count)]


def _cache_cdhash_in(src: Path) -> str:
    # dsc-apply reports the new cdhash; the original is read directly for the trust-category lookup.
    data = src.read_bytes()
    sig_off, _sig_size = struct.unpack_from('<QQ', data, 0x28)
    cd = data.index(b'\xfa\xde\x0c\x02', sig_off)
    length = struct.unpack_from('>I', data, cd + 4)[0]
    return hashlib.sha256(data[cd : cd + length]).digest()[:20].hex()


def _tool_version(tool: str) -> str:
    return hashlib.sha256(Path(tool).read_bytes()).hexdigest()[:16]


if __name__ == '__main__':
    sys.exit(main())
