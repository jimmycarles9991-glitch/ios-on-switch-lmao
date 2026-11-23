#!/usr/bin/env python3
"""Lay out the shader seed's host tools (//gpu/seed:local_tools, gpu/seed/tools.bzl seed_tools) and archive them.

    seed.py layout --raw <raw pack tools dir> --stack <stack file> --out <dir> --manifest <a copy of its MANIFEST.json>
    seed.py archive --tools <dir> --out <tar>

layout: the tools directory the seed runs (raw/ with the pack's tools: directpack, native-seed, the mock GM20B,
own.metallib and the context init) and MANIFEST.json: the driver's stack key, which the plugin is built with. The
seed refuses tools whose manifest is not the GPU bundle's. Running the tools needs nothing but macOS.
"""

import argparse
import io
import json
import shutil
import tarfile
from pathlib import Path


def layout(args: argparse.Namespace) -> None:
    out = Path(args.out)
    shutil.rmtree(out, ignore_errors=True)
    (out / 'raw').mkdir(parents=True)
    for path in sorted(Path(args.raw).iterdir()):
        shutil.copyfile(path, out / 'raw' / path.name)
        (out / 'raw' / path.name).chmod(0o755)
    manifest = {'format': 1, 'stack': Path(args.stack).read_text().strip()}
    text = json.dumps(manifest, indent=1, sort_keys=True) + '\n'
    (out / 'MANIFEST.json').write_text(text)
    Path(args.manifest).write_text(text)


def archive(args: argparse.Namespace) -> None:
    tools = Path(args.tools)
    data = io.BytesIO()
    with tarfile.open(fileobj=data, mode='w', format=tarfile.USTAR_FORMAT) as tar:
        for path in sorted(p for p in tools.rglob('*') if p.is_file()):
            info = tarfile.TarInfo(str(path.relative_to(tools)))
            info.size, info.mtime = path.stat().st_size, 0
            info.mode = 0o644 if path.suffix in ('.json', '.metallib') else 0o755
            with path.open('rb') as f:
                tar.addfile(info, f)
    Path(args.out).write_bytes(data.getvalue())


STEPS = {
    'layout': (layout, ('raw', 'stack', 'out', 'manifest')),
    'archive': (archive, ('tools', 'out')),
}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    steps = parser.add_subparsers(dest='step', required=True)
    for step, (_, names) in STEPS.items():
        p = steps.add_parser(step)
        for name in names:
            p.add_argument('--' + name, required=True)
    args = parser.parse_args()
    STEPS[args.step][0](args)


if __name__ == '__main__':
    main()
