#!/usr/bin/env python3
"""Build directrun (directrun.mm) and exec.metallib (exec.metal and the counter resolve kernel) into one directory.

usage: build.py --xcode-env <xcode.env> --host-bridge <darwin bridge .a> --metal-abi <MetalABI.h>
                --raw <shader tools' raw/> --out <dir>

directrun links the raw runtime's direct compile as the pack generator's directpack does (raw/pack/rawpack.py tool),
over the frontend and the host bridge, beside the shader tools' mock GM20B (rpath @executable_path). The counter resolve
kernel is the source the raw device compiles for itself (raw/runtime/raw_counter_kernel.h kResolveSource).
"""

import argparse
import re
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path('tools/gpu/maxwell/direct')
RUNTIME = Path('drivers/TegraGM20BMetal/raw/runtime')
METAL = Path('drivers/NativeMetal')
DIRECT_RUNTIME = ('raw_direct', 'raw_direct_compute', 'raw_record_parts', 'raw_emit', 'raw_pack', 'raw_metallib')
RESOLVE = re.compile(r'kResolveSource = R"\((.*?)\)";', re.DOTALL)


def run(command: list, env: dict[str, str]) -> None:
    result = subprocess.run([str(c) for c in command], env=env, capture_output=True, text=True, check=False)
    if result.returncode:
        sys.exit(f'{" ".join(map(str, command[:3]))}: {result.stdout[-4000:]}{result.stderr[-4000:]}')


def main() -> None:
    parser = argparse.ArgumentParser()
    for name in ('xcode-env', 'host-bridge', 'metal-abi', 'raw', 'out'):
        parser.add_argument('--' + name, required=True)
    args = parser.parse_args()
    xcode = dict(line.split('=', 1) for line in Path(args.xcode_env).read_text().splitlines() if '=' in line)
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    work = out / 'work'
    work.mkdir()
    env = {'PATH': '/usr/bin:/bin', 'DEVELOPER_DIR': xcode['DEVELOPER_DIR'], 'ZERO_AR_DATE': '1', 'HOME': str(work)}
    env['TMPDIR'] = str(work)
    counter = RESOLVE.search((RUNTIME / 'raw_counter_kernel.h').read_text())
    if not counter:
        sys.exit('no kResolveSource in raw_counter_kernel.h')
    (work / 'counter.metal').write_text(counter.group(1))
    airs = []
    for source in (HERE / 'exec.metal', work / 'counter.metal'):
        air = work / (source.stem + '.air')
        run(['xcrun', '-sdk', 'iphoneos', 'metal', '-c', source, '-o', air], env)
        airs.append(air)
    run(['xcrun', '-sdk', 'iphoneos', 'metallib', *airs, '-o', out / 'exec.metallib'], env)
    shutil.copyfile(Path(args.raw) / 'libmock_iokit.dylib', out / 'libmock_iokit.dylib')
    frameworks = [
        f for name in ('Foundation', 'Metal', 'IOSurface', 'IOKit', 'CoreVideo') for f in ('-framework', name)
    ]
    includes = [
        '-I',
        METAL,
        '-I',
        Path(args.metal_abi).parent,
        '-I',
        'drivers/TegraGM20B',
        '-I',
        'drivers/TegraGM20BMetal',
    ]
    includes += ['-I', 'third_party/mesa/src/nouveau/headers']
    objcxx = [
        HERE / 'directrun.mm',
        RUNTIME / 'RawDirect.mm',
        RUNTIME / 'RawRenderKey.mm',
        METAL / 'NativeBridgeMetalFrontend.mm',
    ]
    run(
        [
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
            *(RUNTIME / f'{name}.cpp' for name in DIRECT_RUNTIME),
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
            out / 'directrun',
        ],
        env,
    )
    shutil.rmtree(work)


if __name__ == '__main__':
    main()
