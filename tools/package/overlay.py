#!/usr/bin/env python3
"""The userland overlay: files shown over sealed system directories (SwitchRawMount.h native_switch_raw_overlay).

They are kept on Data under /private/var/Switch/overlay (the deploy copies the boot package's overlay/ there).

Three steps of //boot:package, so that each overlay edit reruns only what reads it:

    overlay.py plan --manifest M --portrait-header H --kind KIND [--given SOURCE]... [--release] --out PLAN
        overlay/manifest.json, filtered and checked: the files to stage and the directories the kernel unions
        (each with the switch-no-<feature> boot-arg that hides it), which the ADT carries. Reads no overlay file.
    overlay.py trust --plan PLAN [--file SOURCE=PATH]... --out MODULE
        the trust cache module of the trusted files' CDHashes (empty when none is trusted)
    overlay.py tree --plan PLAN [--file SOURCE=PATH]... --out DIR
        the files at overlay/<absolute path>

manifest.json: {"files": [{"path": absolute, "source": relative to the tree, "trust": bool, "mode": "0755",
"boot-arg": name, "union": absolute}]}. The union directory is the file's own unless "union" names an existing
ancestor (XNU unions are deep, so a new bundle directory can appear inside a sealed one). "portrait": true stages the
file only for a portrait-native kernel, "debug": true only in a debug boot package (not with --release), "kind" only for
that IPSW kind (a union over a directory the System volume lacks fails at boot). Under the Supplemental cryptex path
only launchd plists and launchd's jetsam properties overlay are allowed (SUPPLEMENTAL).
"bundle": true makes the entry a whole bundle directory: its source is a rules_apple .ipa, whose Payload/<the path's
name>/ becomes the directory at path, and "trust" covers every Mach-O inside it.
"""

import argparse
import hashlib
import json
import posixpath
import re
import shutil
import struct
import sys
import zipfile
from collections.abc import Iterable
from pathlib import Path

from boot_config import portrait_native

from tools.common import macho
from tools.trustcache.trustcache import trust_module

OVERLAY_ROOT = '/private/var/Switch/overlay'
# iOS 27's launchd bootstraps the Supplemental cryptex path's LaunchDaemons, through the commpage byte the 27 kernel
# sets for it (commpage.c). Other readers of that byte act on files under the same tree: Foundation dlopens
# Frameworks/ERFoundationExtension.dylib into every process, and Settings shows an Extended Research pane for a bundle
# under System/Library/PreferenceBundles. So the overlay may put there only what launchd reads: LaunchDaemons plists and
# the jetsam properties overlay its bootstrap names (SupplementalJetsamProperties), the only way 27's launchd gives a
# daemon its own tables leave out a band and memory limits.
SUPPLEMENTAL = '/private/preboot/Cryptexes/Supplemental'
SUPPLEMENTAL_ROOTS = (SUPPLEMENTAL, '/System/Cryptexes/Supplemental')
SUPPLEMENTAL_DAEMON = re.compile(re.escape(SUPPLEMENTAL) + r'/System/Library/LaunchDaemons/[A-Za-z0-9._-]+\.plist')
SUPPLEMENTAL_JETSAM = SUPPLEMENTAL + '/System/Library/JetsamProperties/com.apple.jetsamproperties.cryptex.plist'


def plan(manifest: dict, portrait: bool, kind: str, given: Iterable[str] | None = None, release: bool = False) -> dict:
    """Return the manifest's files for this kernel and IPSW kind.

    The plan: {"files": [{path, source, trust, mode, directory}], "dirs": [[d, gate]]}. `given`: the sources the build
    gives; a manifest entry it lacks is an error, not a silent omission. `release`: debug entries are left out.
    """
    entries = [
        entry for entry in manifest['files'] if not (release and entry.get('debug')) and entry.get('kind', kind) == kind
    ]
    if given is not None:
        missing = sorted({entry['source'] for entry in entries} - set(given))
        if missing:
            raise SystemExit(
                f'overlay/manifest.json lists {", ".join(missing)} but //boot:package does not give them: '
                'add them to boot/BUILD.bazel'
            )
    files, gates = [], {}
    for entry in entries:
        if entry.get('portrait') and not portrait:
            continue
        path = entry['path']
        if not path.startswith('/') or posixpath.normpath(path) != path or path.startswith('/private/var/'):
            raise SystemExit(f'overlay: bad path {path}')
        supplemental = any(path == root or path.startswith(root + '/') for root in SUPPLEMENTAL_ROOTS)
        if supplemental and not (SUPPLEMENTAL_DAEMON.fullmatch(path) or path == SUPPLEMENTAL_JETSAM):
            raise SystemExit(f'overlay: {path}: only launchd plists may go under {SUPPLEMENTAL}')
        directory, gate = entry.get('union', posixpath.dirname(path)), entry.get('boot-arg', '')
        if not path.startswith(directory.rstrip('/') + '/'):
            raise SystemExit(f'overlay: {path} is not inside its union {directory}')
        # native_switch_raw_overlay's buffers: "<directory>[:<boot-arg>]" in char[128] and the overlay source path
        # in char[176]; a longer entry would be skipped (or its source cut short) at boot without a word.
        if len(directory) + (len(gate) + 1 if gate else 0) >= 128 or len(OVERLAY_ROOT + directory) >= 176:
            raise SystemExit(f'overlay: union {directory} is too long for the kernel (128/176 bytes)')
        if gates.setdefault(directory, gate) != gate:
            raise SystemExit(f'overlay: files in {directory} need the same boot-arg')
        trust = bool(entry.get('trust'))
        files.append(
            {
                'path': path,
                'source': entry['source'],
                'trust': trust,
                'mode': int(entry.get('mode', '0755' if trust else '0644'), 8),
                'directory': directory,
                'bundle': bool(entry.get('bundle')),
            }
        )
    used = {f['directory'] for f in files}
    return {'files': files, 'dirs': sorted([d, g] for d, g in gates.items() if d in used)}


def cdhash(image: bytes) -> bytes:
    """Return the CDHash of a thin Mach-O's embedded signature.

    The first 20 bytes of the SHA-256 of its SHA-256 CodeDirectory (what codesign -dvvv prints as CDHash and what a
    trust cache entry holds).
    """
    try:
        parsed = macho.macho(image, subtypes=None)
    except ValueError as error:
        raise SystemExit(f'overlay: not a thin 64-bit ARM64 Mach-O ({error})') from None
    for at, _ in macho.commands_of(parsed, macho.LC_CODE_SIGNATURE)[:1]:
        offset = struct.unpack_from('<I', image, at + 8)[0]
        blob_magic, _, count = struct.unpack_from('>III', image, offset)
        if blob_magic != 0xFADE0CC0:  # embedded signature SuperBlob
            break
        for index in range(count):
            slot, start = struct.unpack_from('>II', image, offset + 12 + 8 * index)
            if slot != 0 and not 0x1000 <= slot < 0x1005:  # CodeDirectory and its alternates
                continue
            directory = offset + start
            cd_magic, length = struct.unpack_from('>II', image, directory)
            if cd_magic == 0xFADE0C02 and image[directory + 37] == 2:  # hashType SHA-256
                return hashlib.sha256(image[directory : directory + length]).digest()[:20]
    raise SystemExit('overlay: no SHA-256 CodeDirectory (sign it with codesign -s -)')


MACHO_64 = b'\xcf\xfa\xed\xfe'


def bundle_files(ipa: Path, path: str) -> list[tuple[str, bytes]]:
    """Return the files of the .app an .ipa carries for `path` (Payload/<its name>/), by path inside the bundle."""
    prefix = f'Payload/{posixpath.basename(path)}/'
    with zipfile.ZipFile(ipa) as archive:
        files = [
            (info.filename[len(prefix) :], archive.read(info))
            for info in sorted(archive.infolist(), key=lambda info: info.filename)
            if info.filename.startswith(prefix) and not info.is_dir()
        ]
    if not files:
        raise SystemExit(f'overlay: {ipa.name} has no {prefix}')
    return files


def code_of(entry: dict, source: Path) -> list[bytes]:
    """Return the Mach-O images a trusted entry puts in the overlay: the file itself, or a bundle's Mach-Os."""
    if not entry['bundle']:
        return [source.read_bytes()]
    return [data for _, data in bundle_files(source, entry['path']) if data[:4] == MACHO_64]


def _files(specs: Iterable[str]) -> dict[str, Path]:
    files = {}
    for spec in specs:
        source, _, path = spec.partition('=')
        files[source] = Path(path)
    return files


def _need(files: dict[str, Path], entry: dict) -> Path:
    if entry['source'] not in files:
        sys.exit(f'overlay: {entry["path"]}: //boot:package gives no {entry["source"]}')
    return files[entry['source']]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    step = parser.add_subparsers(dest='step', required=True)
    p = step.add_parser('plan')
    for name in ('manifest', 'portrait-header', 'out'):
        p.add_argument('--' + name, required=True, type=Path)
    p.add_argument('--kind', required=True)
    p.add_argument('--given', action='append', default=[])
    p.add_argument('--release', action='store_true')
    for name in ('trust', 'tree'):
        p = step.add_parser(name)
        p.add_argument('--plan', required=True, type=Path)
        p.add_argument('--file', action='append', default=[], help='SOURCE=PATH: a manifest source and its file')
        p.add_argument('--out', required=True, type=Path)
    args = parser.parse_args()
    if args.step == 'plan':
        result = plan(
            json.loads(args.manifest.read_text()),
            portrait_native(args.portrait_header),
            args.kind,
            args.given,
            args.release,
        )
        args.out.write_text(json.dumps(result, indent=1) + '\n')
        return
    files = _files(args.file)
    entries = json.loads(args.plan.read_text())['files']
    if args.step == 'trust':
        hashes = [cdhash(image) for e in entries if e['trust'] for image in code_of(e, _need(files, e))]
        # Flags and launch-constraint category 0: overlay code lives on Data, where the system categories' volume
        # constraints would refuse it.
        args.out.write_bytes(trust_module(hashes) if hashes else b'')
        print(f'overlay: {len(entries)} files ({len(hashes)} trusted) -> Data {OVERLAY_ROOT}')
        return
    args.out.mkdir(parents=True, exist_ok=True)
    for entry in entries:
        target = args.out / entry['path'].lstrip('/')
        if entry['bundle']:
            for name, data in bundle_files(_need(files, entry), entry['path']):
                member = target / name
                member.parent.mkdir(parents=True, exist_ok=True)
                member.write_bytes(data)
                member.chmod(0o755 if data[:4] == MACHO_64 else 0o644)
            continue
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(_need(files, entry), target)
        target.chmod(entry['mode'])


if __name__ == '__main__':
    main()
