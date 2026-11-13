"""Add the selected build's application and embedded libraries to its immutable shader inputs."""

import argparse
import hashlib
import json
import plistlib
import re
import shutil
import zipfile
from pathlib import Path, PurePosixPath

from tools.gpu.shaders.metallib import functions


def put_library(root: Path, relative: str, data: bytes) -> dict:
    path = PurePosixPath(relative)
    if path.is_absolute() or '..' in path.parts:
        raise ValueError(f'invalid library path: {relative}')
    found = functions(data)
    if not found:
        raise ValueError(f'no Metal functions in {relative}')
    target = root / path
    if target.exists() and target.read_bytes() != data:
        raise ValueError(f'conflicting library inputs at {relative}')
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(data)
    return {'path': relative, 'sha256': hashlib.sha256(data).hexdigest(), 'functions': len(found)}


def application(archive: Path, out: Path, major: str) -> dict:
    with zipfile.ZipFile(archive) as bundle:
        roots = [
            name.removesuffix('/Info.plist')
            for name in bundle.namelist()
            if re.fullmatch(r'Payload/[^/]+\.app/Info\.plist', name)
        ]
        if len(roots) != 1:
            raise ValueError(f'{archive.name}: expected one application')
        prefix = roots[0]
        info = plistlib.loads(bundle.read(prefix + '/Info.plist'))
        minimum = str(info.get('MinimumOSVersion', ''))
        if minimum.split('.', maxsplit=1)[0] != major or info.get('CFBundleSupportedPlatforms') != ['iPhoneOS']:
            raise ValueError(f'{archive.name}: expected iPhoneOS {major}, got {minimum}')
        entries = []
        for name in sorted(bundle.namelist()):
            if name.startswith(prefix + '/') and name.endswith('.metallib'):
                device = 'Applications/' + prefix.split('/', 1)[1] + name[len(prefix) :]
                entries.append(put_library(out, 'root/' + device, bundle.read(name)))
        if not entries:
            raise ValueError(f'{archive.name}: no application Metal library')
        return {
            'bundle': info['CFBundleIdentifier'],
            'minimumOs': minimum,
            'libraries': entries,
        }


def embedded(header: Path, out: Path) -> dict:
    text = header.read_text()
    array = text[text.index('{') : text.rindex('}')]
    data = bytes(int(value, 16) for value in re.findall(r'0x([0-9a-fA-F]{2})\b', array))
    digest = hashlib.sha256(data).hexdigest()
    return put_library(out, 'data/' + digest + '.metallib', data)


def prepare(crawl: Path, applications: list[Path], headers: list[Path], major: str, out: Path) -> dict:
    if not (crawl / 'root').is_dir() or not (crawl / 'data').is_dir():
        raise ValueError('shader crawl must contain root/ and data/')
    shutil.copytree(crawl, out, copy_function=shutil.copyfile, dirs_exist_ok=True)
    return {
        'applications': [application(archive, out, major) for archive in applications],
        'embedded': [embedded(header, out) for header in headers],
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--crawl', type=Path, required=True)
    parser.add_argument('--application', type=Path, action='append', default=[])
    parser.add_argument('--embedded', type=Path, action='append', default=[])
    parser.add_argument('--ios-major', required=True)
    parser.add_argument('--ipsw', required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--report', type=Path)
    args = parser.parse_args()
    report = prepare(args.crawl, args.application, args.embedded, args.ios_major, args.out)
    report['ipsw'] = args.ipsw
    if args.report:
        args.report.write_text(json.dumps(report, indent=2, sort_keys=True) + '\n')


if __name__ == '__main__':
    main()
