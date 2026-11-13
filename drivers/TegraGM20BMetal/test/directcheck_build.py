#!/usr/bin/env python3
"""Build directcheck (raw/test/directcheck.mm) into one directory with what it runs with.

usage: directcheck_build.py --xcode-env <xcode.env> --host-bridge <darwin bridge .a> --metal-abi <MetalABI.h>
                            --raw <shader tools' raw/> --twin <NVK twin dylib> --out <dir>

directcheck links the raw runtime's direct compile as the pack generator's directpack does, over the frontend and the
host bridge, beside the shader tools' mock GM20B (rpath @executable_path). The directory also holds the NVK twin the
pack's records were compiled with and the raw path's own library (own.metallib, PROBE_OWN_LIB).
"""

import argparse
import shutil
import subprocess
import sys
from pathlib import Path

RAW = Path('drivers/TegraGM20BMetal/raw')
METAL = Path('drivers/NativeMetal')
DIRECT_RUNTIME = ('raw_direct', 'raw_direct_compute', 'raw_record_parts', 'raw_emit', 'raw_pack', 'raw_metallib')


def main() -> None:
    parser = argparse.ArgumentParser()
    for name in ('xcode-env', 'host-bridge', 'metal-abi', 'raw', 'twin', 'out'):
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
    shutil.copyfile(args.twin, out / 'libvulkan_nouveau.dylib')
    frameworks = [
        f for name in ('Foundation', 'Metal', 'IOSurface', 'IOKit', 'CoreVideo') for f in ('-framework', name)
    ]
    includes = ['-I', METAL, '-I', Path(args.metal_abi).parent, '-I', 'drivers/TegraGM20B']
    includes += ['-I', 'third_party/mesa/src/nouveau/headers']
    objcxx = [
        RAW / 'test/directcheck.mm',
        RAW / 'runtime/RawDirect.mm',
        RAW / 'runtime/RawRenderKey.mm',
        METAL / 'NativeBridgeMetalFrontend.mm',
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
        *(RAW / 'runtime' / f'{name}.cpp' for name in DIRECT_RUNTIME),
        '-x',
        'none',
        args.host_bridge,
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
