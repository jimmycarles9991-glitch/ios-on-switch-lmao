#!/usr/bin/env python3
"""Make the shader seed's host tools (//gpu/seed:local_tools, gpu/seed/defs.bzl seed_tools), one action per step.

    seed.py mock --xcode-env F --abi-header TegraGM20BABI.h --mock-iokit mock_iokit.c --out libmock_iokit.dylib
    seed.py tool --xcode-env F --metal-header NativeBridgeMetalFrontend.h --precompile precompile.mm
                 --frontend NativeBridgeMetalFrontend.mm --host-bridge <darwin libnative_metal_bridge.a>
                 --mock libmock_iokit.dylib --out precompile
    seed.py twin --nvk <iOS dylib> --twin <host dylib> --out libvulkan_nouveau.dylib
    seed.py layout --precompile F --mock F --twin-dylib F --raw <raw pack tools dir> --stack <stack file>
                   --nvk-source <the iOS dylib's .source> --out <dir> --manifest <a copy of its MANIFEST.json>
    seed.py archive --tools <dir> --out <tar>

layout: the tools directory the seed runs (precompile, libmock_iokit.dylib beside it, the twin with its .source, raw/
with the raw pack's probe, mock and ycbcr.metallib) and MANIFEST.json: the raw path's stack key (the plugin is built
with it), the twin's LC_UUID (the iOS dylib's), NVK's compiler identity (which NVK hashes into its pipeline cache UUID),
the Mesa tree and the architecture. The seed refuses tools whose manifest is not the GPU bundle's. Only mock, tool and
the raw pack's build need Xcode; running the tools needs nothing but macOS.
"""

import argparse
import io
import json
import os
import shutil
import struct
import subprocess
import sys
import tarfile
from pathlib import Path

from tools.gpu.nvk_twin import lc_uuid, make_twin

ARCHES = {0x0100000C: 'arm64', 0x01000007: 'x86_64'}


def _xcode_env(path: str, work: Path) -> dict[str, str]:
    xcode = dict(line.split('=', 1) for line in Path(path).read_text().splitlines() if '=' in line)
    (work / 'home').mkdir(parents=True, exist_ok=True)
    return {
        'PATH': '/usr/bin:/bin',
        'DEVELOPER_DIR': xcode['DEVELOPER_DIR'],
        'ZERO_AR_DATE': '1',
        'HOME': str(work / 'home'),
        'TMPDIR': os.environ.get('TMPDIR', str(work)),
    }


def _run(command: list[str | Path], env: dict[str, str]) -> str:
    result = subprocess.run([str(c) for c in command], env=env, capture_output=True, text=True, check=False)
    if result.returncode:
        sys.exit(f'FAILED: {" ".join(map(str, command))[:600]}\n{result.stdout[-4000:]}{result.stderr[-6000:]}')
    return result.stdout + result.stderr


def mock(args: argparse.Namespace, root: Path, work: Path) -> None:
    env = _xcode_env(args.xcode_env, work)
    _run(
        [
            '/usr/bin/xcrun',
            'clang',
            '-O1',
            '-Wall',
            '-dynamiclib',
            f'-ffile-prefix-map={root}=/switch-ios',
            '-I',
            Path(args.abi_header).parent,
            args.mock_iokit,
            '-framework',
            'IOKit',
            '-framework',
            'CoreFoundation',
            '-install_name',
            '@rpath/libmock_iokit.dylib',
            '-o',
            args.out,
        ],
        env,
    )


def tool(args: argparse.Namespace, root: Path, work: Path) -> None:
    """Build the precompile tool, linked against the mock IOKit beside it.

    The tools directory puts the two side by side.
    """
    env = _xcode_env(args.xcode_env, work)
    _run(
        [
            '/usr/bin/xcrun',
            'clang++',
            '-O2',
            '-std=c++17',
            '-fobjc-arc',
            '-Wno-deprecated-declarations',
            f'-ffile-prefix-map={root}=/switch-ios',
            '-I',
            Path(args.metal_header).parent,
            '-x',
            'objective-c++',
            args.precompile,
            args.frontend,
            '-x',
            'none',
            args.host_bridge,
            '-L',
            Path(args.mock).parent,
            '-lmock_iokit',
            '-Wl,-rpath,@executable_path',
            '-framework',
            'Foundation',
            '-framework',
            'Metal',
            '-framework',
            'IOSurface',
            '-framework',
            'IOKit',
            '-o',
            args.out,
        ],
        env,
    )


def twin(args: argparse.Namespace, _root: Path, work: Path) -> None:
    # The twin with the device's pipeline cache identity (tools/gpu/nvk_twin.py).
    (work / 'home').mkdir()
    make_twin(
        args.nvk,
        args.twin,
        args.out,
        {'PATH': '/usr/bin:/bin', 'HOME': str(work / 'home'), 'TMPDIR': os.environ.get('TMPDIR', str(work))},
    )


def layout(args: argparse.Namespace, _root: Path, _work: Path) -> None:
    out = Path(args.out)
    shutil.rmtree(out, ignore_errors=True)
    (out / 'raw').mkdir(parents=True)
    for source, name in (
        (args.precompile, 'precompile'),
        (args.mock, 'libmock_iokit.dylib'),
        (args.twin_dylib, 'libvulkan_nouveau.dylib'),
    ):
        shutil.copyfile(source, out / name)
        (out / name).chmod(0o755)
    for path in sorted(Path(args.raw).iterdir()):
        shutil.copyfile(path, out / 'raw' / path.name)
        (out / 'raw' / path.name).chmod(0o755)
    shutil.copyfile(args.nvk_source, out / 'libvulkan_nouveau.dylib.source')
    (cpu,) = struct.unpack_from('<I', (out / 'precompile').read_bytes(), 4)
    source = dict(line.split(' ', 1) for line in Path(args.nvk_source).read_text().splitlines() if ' ' in line)
    manifest = {
        'format': 1,
        'arch': ARCHES.get(cpu, hex(cpu)),
        'stack': Path(args.stack).read_text().strip(),
        'nvk_uuid': lc_uuid(out / 'libvulkan_nouveau.dylib').hex(),
        'nvk_build_id': source['nvk-build-id'],
        'mesa_tree': source['mesa-tree'],
    }
    text = json.dumps(manifest, indent=1, sort_keys=True) + '\n'
    (out / 'MANIFEST.json').write_text(text)
    Path(args.manifest).write_text(text)


def archive(args: argparse.Namespace, _root: Path, _work: Path) -> None:
    tools = Path(args.tools)
    data = io.BytesIO()
    with tarfile.open(fileobj=data, mode='w', format=tarfile.USTAR_FORMAT) as tar:
        for path in sorted(p for p in tools.rglob('*') if p.is_file()):
            info = tarfile.TarInfo(str(path.relative_to(tools)))
            info.size, info.mtime = path.stat().st_size, 0
            info.mode = 0o644 if path.suffix in ('.json', '.metallib', '.source') else 0o755
            with path.open('rb') as f:
                tar.addfile(info, f)
    Path(args.out).write_bytes(data.getvalue())


STEPS = {
    'mock': (mock, ('xcode-env', 'abi-header', 'mock-iokit', 'out')),
    'tool': (tool, ('xcode-env', 'metal-header', 'precompile', 'frontend', 'host-bridge', 'mock', 'out')),
    'twin': (twin, ('nvk', 'twin', 'out')),
    'layout': (layout, ('precompile', 'mock', 'twin-dylib', 'raw', 'stack', 'nvk-source', 'out', 'manifest')),
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
    root = Path.cwd()
    work = root / f'_seed_{args.step}_work'
    if work.exists():
        shutil.rmtree(work)
    work.mkdir(parents=True)
    STEPS[args.step][0](args, root, work)
    shutil.rmtree(work)


if __name__ == '__main__':
    main()
