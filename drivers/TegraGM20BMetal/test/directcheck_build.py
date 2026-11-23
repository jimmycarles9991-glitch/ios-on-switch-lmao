#!/usr/bin/env python3
"""Build directcheck beside its mock GPU and own.metallib, using the native translator and compiler."""

import argparse
import shutil
import subprocess
import sys
from pathlib import Path

METAL = Path('drivers/TegraGM20BMetal')
PLUGIN = Path('drivers/TegraGM20BMetal/plugin')
DIRECT_RUNTIME = ('direct', 'direct_compute', 'record_parts', 'emit', 'pack', 'metallib')


def main() -> None:
    parser = argparse.ArgumentParser()
    for name in ('xcode-env', 'translate', 'compile', 'metal-abi', 'raw', 'out'):
        parser.add_argument('--' + name, required=True)
    args = parser.parse_args()
    xcode = dict(line.split('=', 1) for line in Path(args.xcode_env).read_text().splitlines() if '=' in line)
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    work = out / 'work'
    work.mkdir()
    env = {'PATH': '/usr/bin:/bin', 'DEVELOPER_DIR': xcode['DEVELOPER_DIR'], 'ZERO_AR_DATE': '1', 'HOME': str(work)}
    env['TMPDIR'] = str(work)
    for name in ('libmock_iokit.dylib', 'own.metallib'):
        shutil.copyfile(Path(args.raw) / name, out / name)
    frameworks = [
        f for name in ('Foundation', 'Metal', 'IOSurface', 'IOKit', 'CoreVideo') for f in ('-framework', name)
    ]
    includes = ['-I', PLUGIN, '-I', Path(args.metal_abi).parent, '-I', 'drivers/TegraGM20B']
    includes += ['-I', 'third_party/mesa/src/nouveau/headers']
    objcxx = [
        METAL / 'harness/directcheck.mm',
        METAL / 'TegraGM20BMetalDirect.mm',
        METAL / 'TegraGM20BMetalRenderKey.mm',
        METAL / 'TegraGM20BMetalOffline.mm',
        METAL / 'TegraGM20BMetalOfflineReflection.mm',
    ]
    command = [
        'xcrun',
        'clang++',
        '-O1',
        '-std=c++17',
        '-fobjc-arc',
        '-Wno-deprecated-declarations',
        *includes,
        '-x',
        'objective-c++',
        *objcxx,
        '-x',
        'c++',
        *(METAL / f'{name}.cpp' for name in DIRECT_RUNTIME),
        '-x',
        'none',
        args.translate,
        args.compile,
        '-L',
        out,
        '-lmock_iokit',
        '-Wl,-rpath,@executable_path',
        '-lobjc',
        *frameworks,
        '-o',
        out / 'directcheck',
    ]
    result = subprocess.run([str(c) for c in command], env=env, capture_output=True, text=True, check=False)
    if result.returncode:
        sys.exit(f'directcheck: {result.stdout[-4000:]}{result.stderr[-4000:]}')
    shutil.rmtree(work)


if __name__ == '__main__':
    main()
