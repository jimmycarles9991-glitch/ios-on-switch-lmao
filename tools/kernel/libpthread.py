"""libpthread_kernel.a, as tree/scripts/build-libpthread-kernel.py made it.

libpthread-519.120.4's three kernel sources (patched by kernel/libpthread), names shared with XNU renamed
pthread519_<name>, and XNU's source_pthread_bootstrap.o (our XNU patch adds the source;
//kernel/xnu-10063.141.1/osfmk:pthread_bootstrap compiles it as prng_random.c is).
--kernel-names is xnu_symbols' list of XNU's names (every one, global or local, may collide).

usage: libpthread.py --xcode-env F --source DIR --sdkroot DIR --kernel-names NAMES.txt --bootstrap O --out A.a
                     --bootstrap-out O --path-root P --bin-dir D
"""

import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import common
from kernel_names import archive_names

from tools.common import macho


def check(path: Path) -> None:
    if macho.arm64_kind(path.read_bytes()) != macho.MH_OBJECT:
        sys.exit('Unexpected pthread object architecture: ' + path.name)


def configuration_flags(config: str) -> list[str]:
    if config == 'DEVELOPMENT':
        return ['-DMACH_ASSERT=1', '-DDEBUG=1']
    if config == 'RELEASE':
        return [
            '-DMACH_ASSERT=0',
            '-DDEBUG=0',
            '-DDEVELOPMENT=0',
            '-DRELEASE=1',
            '-DIST_KDEBUG=1',
            '-DCONFIG_NO_PRINTF_STRINGS=1',
            '-DCONFIG_NO_KPRINTF_STRINGS=1',
        ]
    raise ValueError(f'unsupported kernel configuration: {config}')


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument('--config', choices=('DEVELOPMENT', 'RELEASE'), default='DEVELOPMENT')
    for name in (
        'xcode-env',
        'source',
        'sdkroot',
        'kernel-names',
        'bootstrap',
        'out',
        'bootstrap-out',
        'path-root',
        'bin-dir',
    ):
        parser.add_argument('--' + name, required=True)
    args = parser.parse_args()
    here = common.execroot()
    xcode = common.xcode_env(args.xcode_env)
    env = common.environment(xcode)
    source = Path(here, args.source)
    framework = Path(here, args.sdkroot, 'System/Library/Frameworks/Kernel.framework/Versions/A')
    root = args.path_root.rstrip('/')
    tree = root + '/out/native-a57/root-v1/tree'
    maps = [
        f'-ffile-prefix-map={source}={tree}/.cache/libpthread-work',
        f'-ffile-prefix-map={here}/{args.bin_dir}={root}',
        f'-ffile-prefix-map={here}={root}',
        # make's osfmk object directory, where these were compiled when the kernel came from make.
        (
            f'-fdebug-compilation-dir={root}/{args.bin_dir}/out/native-a57/root-v1/tree/build/xnu/ios-obj/'
            f'{args.config}_ARM64_BCM2837/osfmk/{args.config}'
        ),
    ]

    def compile_one(path: Path, destination: Path, namespace: Path | None = None) -> None:
        command = [
            'xcrun',
            '--sdk',
            'iphoneos',
            'clang',
            '-target',
            'arm64-apple-ios17.0',
            '-isysroot',
            xcode['IPHONEOS_SDK'],
            '-mkernel',
            '-ffreestanding',
            '-fno-builtin',
            '-fno-stack-check',
            '-std=gnu11',
            '-fblocks',
            '-O2',
            '-mno-implicit-float',
            '-DKERNEL=1',
            '-DKERNEL_PRIVATE=1',
            '-DXNU_KERNEL_PRIVATE=1',
            '-DMACH_KERNEL_PRIVATE=1',
            *configuration_flags(args.config),
            '-D__PTHREAD_EXPOSE_INTERNALS__=1',
            '-DABSOLUTETIME_SCALAR_TYPE=1',
            '-DNEEDS_SCHED_CALL_T=1',
            '-I',
            str(source / 'private'),
            '-I',
            str(source / 'include'),
            '-I',
            str(source),
            '-I',
            str(framework / 'PrivateHeaders'),
            '-I',
            str(framework / 'Headers'),
            '-Wno-deprecated-declarations',
            *maps,
            '-c',
            str(path),
            '-o',
            str(destination),
        ]
        if namespace:
            command += ['-include', str(namespace)]
        result = subprocess.run(
            command, cwd=here, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=env, check=False
        )
        if result.returncode:
            sys.exit(result.stdout[-4500:])
        check(destination)

    with tempfile.TemporaryDirectory() as scratch:
        out = Path(scratch)
        sources = [source / 'kern' / name for name in ('kern_init.c', 'kern_support.c', 'kern_synch.c')]
        objects = [out / (s.stem + '.o') for s in sources]
        for s, o in zip(sources, objects, strict=False):
            compile_one(s, o)
        names = {line.partition(' ')[2] for line in Path(args.kernel_names).read_text().splitlines() if line}
        definitions = set().union(*(archive_names(o, '-U', env) for o in objects))
        collisions = definitions & names
        if collisions:
            namespace = out / 'namespace.h'
            namespace.write_text(''.join(f'#define {n[1:]} pthread519_{n[1:]}\n' for n in sorted(collisions)))
            for s, o in zip(sources, objects, strict=False):
                compile_one(s, o, namespace)
        definitions = set().union(*(archive_names(o, '-U', env) for o in objects))
        imports = set().union(*(archive_names(o, '-u', env) for o in objects)) - definitions
        missing = sorted(imports - names - {'___dso_handle'})
        if not {'_pthread_kern', '_current_uthread'} <= collisions:
            sys.exit('Missing expected pthread/XNU static symbol isolation')
        if missing:
            sys.exit(f'Unavailable real kernel imports: {missing}')
        bootstrap = out / 'source_pthread_bootstrap.o'
        bootstrap.write_bytes(Path(args.bootstrap).read_bytes())
        check(bootstrap)
        archive = out / 'libpthread_kernel.a'
        subprocess.run(
            ['xcrun', 'ar', 'rcs', str(archive), *[str(o) for o in objects], str(bootstrap)], check=True, env=env
        )
        Path(args.out).write_bytes(archive.read_bytes())
        Path(args.bootstrap_out).write_bytes(bootstrap.read_bytes())


if __name__ == '__main__':
    main()
