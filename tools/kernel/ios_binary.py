"""An iOS 17 userland binary for the overlay, as cmake/Drivers.cmake built them.

Xcode's clang for arm64-apple-ios17.0 with the iPhoneOS SDK, then an ad hoc signature (no timestamp: the CDHash is a
function of the bytes).

usage: ios_binary.py --xcode-env F --out O --path-root P --bin-dir D [--coreaudio-headers] [--strip]
                     [--sign-identifier ID] [--entitlements E] [--pagesize N] -- <clang args and sources>
--strip (the release profile) strips local symbols (strip -x) before the signature.
--coreaudio-headers puts the macOS SDK's AudioServerPlugIn.h, AudioHardwareBase.h and AudioHardware.h first on the
include path as CoreAudio/ (the iOS SDK lacks them; the HAL plug-in interface is the same).
C sources among C++ ones built with -std=c++NN (which clang refuses for C) are compiled first, as C11 with the include
paths and defines, NDEBUG, and linked as objects.
"""

import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import common


def c_objects(clang: list[str], base: list[str], scratch: str, env: dict) -> list[str]:
    """Return `clang` with each C source replaced by its object, when C++ sources set a C++ standard."""
    if not any(a.startswith('-std=c++') for a in clang) or not any(a.endswith('.c') for a in clang):
        return clang
    flags, at = [], 0
    while at < len(clang):
        a = clang[at]
        if a in ('-I', '-iquote', '-D') and at + 1 < len(clang):
            flags += clang[at : at + 2]
            at += 2
            continue
        if a.startswith(('-I', '-D', '-O')):
            flags.append(a)
        at += 1
    out = []
    for a in clang:
        if not (a.endswith('.c') and os.path.exists(a)):
            out.append(a)
            continue
        obj = os.path.join(scratch, f'{len(out)}-{Path(a).stem}.o')
        subprocess.run([*base, '-std=c11', '-DNDEBUG', *flags, '-c', a, '-o', obj], check=True, env=env)
        out.append(obj)
    return out


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument('--xcode-env', required=True)
    parser.add_argument('--out', required=True)
    parser.add_argument('--path-root', required=True)
    parser.add_argument('--bin-dir', required=True)
    parser.add_argument('--coreaudio-headers', action='store_true')
    parser.add_argument('--sign-identifier')
    parser.add_argument('--entitlements')
    parser.add_argument('--pagesize')
    parser.add_argument('--strip', action='store_true')
    parser.add_argument('clang', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    here = common.execroot()
    xcode = common.xcode_env(args.xcode_env)
    env = common.environment(xcode)
    clang = [a for a in args.clang if a != '--']
    scratch = tempfile.mkdtemp()
    if args.coreaudio_headers:
        headers = Path(scratch, 'H/CoreAudio')
        headers.mkdir(parents=True, exist_ok=True)
        for name in ('AudioServerPlugIn.h', 'AudioHardwareBase.h', 'AudioHardware.h'):
            os.symlink(
                xcode['MACOSX_SDK'] + '/System/Library/Frameworks/CoreAudio.framework/Headers/' + name, headers / name
            )
        clang = ['-I', str(Path(scratch, 'H')), *clang]
    # Absolute paths, as the CMake flow gave them, mapped back to the path root.
    clang = [os.path.join(here, a) if (not a.startswith('-') and os.path.exists(a)) else a for a in clang]
    base = ['xcrun', 'clang', '-target', 'arm64-apple-ios17.0', '-isysroot', xcode['IPHONEOS_SDK']]
    maps = common.prefix_maps(args.path_root, args.bin_dir, 'external/+http_archive+xnu')
    clang = c_objects(clang, base + maps, scratch, env)
    command = [*base, *clang, *maps]
    command += ['-o', os.path.join(here, args.out)]
    subprocess.run(command, check=True, env=env)
    if args.strip:
        subprocess.run(['xcrun', 'strip', '-x', args.out], check=True, env=env)
    if True:
        # Ad hoc, as the CMake flow signed them (identifier defaults to the file name).
        sign = ['codesign', '-f', '-s', '-']
        if args.pagesize:
            sign += ['--pagesize', args.pagesize]
        if args.sign_identifier:
            sign += ['--identifier', args.sign_identifier]
        if args.entitlements:
            sign += ['--entitlements', args.entitlements]
        subprocess.run([*sign, args.out], check=True, env=env)
    data = Path(args.out).read_bytes()
    if here.encode() in data:
        sys.exit(f'{args.out} names the execution root')


if __name__ == '__main__':
    main()
