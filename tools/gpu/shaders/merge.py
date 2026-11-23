"""Lay out the device's Metal compiler replies, check the tools against the GPU bundle, lay the seed out for an image.

usage: merge.py replies [--replies <compile reply store> --reply-input <file>...] [--strict-replies] --ipsw <build>
                        --out <dir>
       merge.py check --tools-manifest <MANIFEST.json> --base <path>... --out F
       merge.py tree --seed <replies' dir> [--raw <raw pack dir>] [--native <packed native dir>] --out <tree dir>
                     --size F

replies: the device's Metal compiler replies (msl/<name>.reply: Core Image's kernel sources compiled to AIR by the OS's
MTLCompiler, named by its LC_UUID and the request) whose envelope holds go to <out>/msl/ as they are, where the shader
compiler reads one its own store lacks (compiler/compiler_codegen.mm), and MANIFEST.json (the IPSW build and the
replies' inventory) beside them.
check: refuses tools that are not the GPU bundle's (--base: its tree or files): the bundle's Info.plist must name the
tools' stack key; a seed of other tools would never be hit. A validation action: the seed does not wait for it.
tree: ngpu/addons/TegraGM20BMetal.seed/shared (the replies' directory) and ngpu/addons/TegraGM20BMetal.pack (with the
packed native seed's raw-<stack>/ in native/; of the packed native seed, only each index and its observed packs go:
tools/gpu/seed_pack.py), directories 0755 and files 0644 (APFS clones of the sources), and size.json (bytes, files,
blocks4k: what the tree takes on APFS) beside it.
"""

import argparse
import ctypes
import json
import os
import plistlib
import shutil
import sys
from pathlib import Path

from tools.gpu import seed_pack
from tools.gpu.shaders import replies as compiler_replies

SEED = 'ngpu/addons/TegraGM20BMetal.seed/shared'
RAW = 'ngpu/addons/TegraGM20BMetal.pack'


def replies(args: argparse.Namespace) -> None:
    """Write msl/ and MANIFEST.json: the replies whose envelope holds, and their inventory."""
    out = args.out
    shutil.rmtree(out, ignore_errors=True)
    out.mkdir(parents=True)
    inventory = compiler_replies.inventory(args.replies, args.reply_input, args.strict_replies)
    if args.replies:
        compiler_replies.copy_inventory(args.replies, out / 'msl', inventory)
        compiler_replies.verify(out / 'msl', inventory)
    counts = {'kept': inventory['replies'], 'refused': len(inventory['refused'])}
    manifest = {'ipsw': args.ipsw, 'compile_replies': counts, 'compiler_reply_inventory': inventory}
    (out / 'MANIFEST.json').write_text(json.dumps(manifest, indent=1) + '\n')
    print(f'compile replies {counts}')


def check(args: argparse.Namespace) -> None:
    """Check the tools are the GPU bundle's; write the stamp."""
    manifest = json.loads(args.tools_manifest.read_text())
    files = [p for b in args.base for p in ([b] if b.is_file() else b.rglob('*')) if p.is_file()]
    info = [p for p in files if p.name == 'Info.plist' and p.parent.name == 'TegraGM20BMetal.bundle']
    if len(info) != 1:
        sys.exit('--base has no single TegraGM20BMetal.bundle (Info.plist)')
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
        if not path.is_file() or len(relative.parts) < 2 or not relative.parts[0].startswith('rec-'):
            continue
        top = relative.parts[0]
        if seed_pack.staged(native / top, relative.relative_to(top).as_posix()):
            pairs.append((path, out / RAW / 'native' / relative))
    return pairs


def tree(args: argparse.Namespace) -> None:
    """Lay the seed and the pack out under ngpu/addons, and write the size."""
    out = args.out
    shutil.rmtree(out, ignore_errors=True)
    pairs = [(p, out / SEED / p.relative_to(args.seed)) for p in sorted(args.seed.rglob('*')) if p.is_file()]
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
    r = steps.add_parser('replies')
    r.add_argument('--replies', type=Path)
    r.add_argument('--reply-input', type=Path, action='append')
    r.add_argument('--strict-replies', action='store_true')
    r.add_argument('--ipsw', required=True)
    r.add_argument('--out', type=Path, required=True)
    c = steps.add_parser('check')
    for name in ('tools-manifest', 'out'):
        c.add_argument('--' + name, type=Path, required=True)
    c.add_argument('--base', type=Path, action='append', required=True)
    t = steps.add_parser('tree')
    t.add_argument('--seed', type=Path, required=True)
    t.add_argument('--raw', type=Path)
    t.add_argument('--native', type=Path)
    t.add_argument('--out', type=Path, required=True)
    t.add_argument('--size', type=Path, required=True)
    args = parser.parse_args()
    {'replies': replies, 'check': check, 'tree': tree}[args.step](args)


if __name__ == '__main__':
    main()
