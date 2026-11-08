"""The kernel's corecrypto provider archive.

As tree/scripts/build-corecrypto-14.py made it, run plain then --namespace, in one action: the corecrypto_kext target's
C and assembly sources, compiled for the iOS 17 kernel as Apple's kext is, so SHA-1/SHA-2, AES, GHASH and the bignum
routines run on the A57's crypto and NEON instructions (the kernel's exception entry saves the NEON registers); the
names the plain archive shares with XNU are renamed cc14_<name> (namespace.h) in the namespaced archive the kernel
links.

usage: corecrypto.py --xcode-env F --source DIR --sdkroot DIR --kernel-names NAMES.txt --out A.a --namespace-header H
                     --path-root P [--jobs N]
"""

import argparse
import concurrent.futures
import dataclasses
import hashlib
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import common
from kernel_names import archive_names

from tools.common import macho


def kext_sources(project: dict[str, dict[str, dict[str, object]]], source: Path) -> list[Path]:
    objects = project['objects']
    parents = {child: key for key, value in objects.items() for child in value.get('children', [])}

    def resolve(key: str) -> Path:
        entry = objects[key]
        path = entry.get('path', '')
        if entry.get('sourceTree') == 'SOURCE_ROOT' or key not in parents:
            return source / path
        if entry.get('sourceTree') != '<group>':
            raise ValueError(f'Unsupported source tree: {entry}')
        return resolve(parents[key]) / path

    target = next(
        v for v in objects.values() if v.get('isa') == 'PBXNativeTarget' and v.get('name') == 'corecrypto_kext'
    )
    phase = next(objects[k] for k in target['buildPhases'] if objects[k]['isa'] == 'PBXSourcesBuildPhase')
    return [
        resolve(objects[k]['fileRef'])
        for k in phase['files']
        if objects[objects[k]['fileRef']].get('path', '').endswith(('.c', '.s'))
    ]


def link_headers(source: Path, include: Path) -> None:
    include.mkdir(parents=True)
    for header in sorted(source.glob('*/corecrypto/*.h')):
        link = include / header.name
        if link.exists() and link.resolve() != header.resolve():
            sys.exit('Conflicting header: ' + header.name)
        if not link.exists():
            link.symlink_to(header)


def clang_command(iphoneos_sdk: str, include: Path, framework: Path, source: Path) -> list[str]:
    command = [
        'xcrun',
        '--sdk',
        'iphoneos',
        'clang',
        '-target',
        'arm64-apple-ios17.0',
        '-isysroot',
        iphoneos_sdk,
        '-mkernel',
        '-ffreestanding',
        '-fno-builtin',
        '-fno-stack-check',
        '-std=gnu99',
        '-fblocks',
        '-O2',
        '-fno-vectorize',
        '-fno-slp-vectorize',
        '-mno-implicit-float',
        '-DKERNEL=1',
        '-DKERNEL_PRIVATE=1',
        '-DBUILDKERNEL=1',
        '-DNDEBUG=1',
        '-DCC_DISABLE_RSAKEYGEN=1',
        '-Wno-deprecated-declarations',
        '-Wno-incompatible-library-redeclaration',
        '-I' + str(include.parent),
        '-I' + str(framework / 'PrivateHeaders'),
        '-I' + str(framework / 'Headers'),
    ]
    for folder in sorted({h.parent for h in source.rglob('*.h') if 'test' not in str(h.relative_to(source)).lower()}):
        command.extend(['-I', str(folder)])
    return command


def language(path: Path) -> list[str]:
    """Xcode's language for corecrypto's .s files: they use #if and #include."""
    return ['-x', 'assembler-with-cpp'] if path.suffix == '.s' else []


@dataclasses.dataclass
class Archiver:
    command: list[str]
    sources: list[Path]
    source: Path
    out: Path
    env: dict[str, str]
    jobs: int
    objects: dict[Path, Path] = dataclasses.field(default_factory=dict)

    def build(self, variant: str, extra: list[str]) -> Path:
        object_dir = self.out / ('objects' + variant)
        object_dir.mkdir()

        def compile_one(path: Path) -> tuple[Path, subprocess.CompletedProcess[str]]:
            relative = str(path.relative_to(self.source))
            name = hashlib.sha256(relative.encode()).hexdigest()[:12] + '-' + path.stem
            destination = object_dir / (name + '.o')
            result = subprocess.run(
                [*self.command, *extra, *language(path), '-c', str(path), '-o', str(destination)],
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                env=self.env,
                check=False,
            )
            return destination, result

        with concurrent.futures.ThreadPoolExecutor(max_workers=self.jobs) as pool:
            results = list(pool.map(compile_one, self.sources))
        self.objects = {path: destination for path, (destination, _) in zip(self.sources, results, strict=True)}
        failed = [(d, r.stdout[-2500:]) for d, r in results if r.returncode]
        if failed:
            sys.exit(f'{len(failed)} corecrypto sources failed:\n{failed[0][1]}')
        for destination, _ in results:
            if macho.arm64_kind(destination.read_bytes()) != macho.MH_OBJECT:
                sys.exit('Wrong object architecture: ' + destination.name)
        archive = self.out / ('libcorecrypto_kernel' + variant + '.a')
        subprocess.run(['xcrun', 'ar', 'rcs', str(archive), *[str(d) for d, _ in results]], check=True, env=self.env)
        return archive


def main() -> None:
    parser = argparse.ArgumentParser()
    for name in ('xcode-env', 'source', 'sdkroot', 'kernel-names', 'out', 'namespace-header', 'path-root'):
        parser.add_argument('--' + name, required=True)
    parser.add_argument('--jobs', type=int, default=os.cpu_count() or 8)
    args = parser.parse_args()
    here = common.execroot()
    xcode = common.xcode_env(args.xcode_env)
    env = common.environment(xcode)
    source = Path(here, args.source)
    # Where the CMake flow kept these (tree/refer/corecrypto-2023, tree/build/corecrypto-2023).
    tree = args.path_root.rstrip('/') + '/out/native-a57/root-v1/tree'
    with tempfile.TemporaryDirectory() as scratch:
        out = Path(scratch)
        subprocess.run(
            [
                '/usr/bin/plutil',
                '-convert',
                'json',
                '-o',
                str(out / 'project.json'),
                str(source / 'corecrypto.xcodeproj/project.pbxproj'),
            ],
            check=True,
            env=env,
        )
        sources = kext_sources(json.loads((out / 'project.json').read_text()), source)
        include = out / 'include/corecrypto'
        link_headers(source, include)
        framework = Path(here, args.sdkroot, 'System/Library/Frameworks/Kernel.framework/Versions/A')
        command = clang_command(xcode['IPHONEOS_SDK'], include, framework, source)
        command += [
            f'-ffile-prefix-map={source}={tree}/refer/corecrypto-2023',
            f'-ffile-prefix-map={out}={tree}/build/corecrypto-2023',
            '-ffile-prefix-map={}={}'.format(here, args.path_root.rstrip('/')),
        ]
        archiver = Archiver(command, sources, source, out, env, args.jobs)
        plain = archiver.build('', [])
        # XNU's global names (xnu_symbols.py's list): the only part of XNU this archive depends on.
        kernel = {line[2:] for line in Path(args.kernel_names).read_text().splitlines() if line.startswith('g ')}
        # XNU links Apple's own copies of some AccelerateCrypto routines (osfmk/arm64/corecrypto); namespace.h cannot
        # rename assembly, so those sources are left out and corecrypto's C calls XNU's copy.
        duplicates = set()
        for path, obj in archiver.objects.items():
            names = archive_names(obj, '-U', env) if path.suffix == '.s' else set()
            if names & kernel:
                if not names <= kernel:
                    sys.exit(f'{path.name} defines names XNU also defines and names it does not')
                duplicates |= names
                archiver.sources.remove(path)
        collisions = sorted((archive_names(plain, '-U', env) & kernel) - duplicates)
        if any(not name[1:].isidentifier() for name in collisions):
            sys.exit('A colliding symbol cannot be namespaced with a C identifier')
        header = out / 'namespace.h'
        header.write_text(''.join(f'#define {n[1:]} cc14_{n[1:]}\n' for n in collisions))
        namespaced = archiver.build('-namespaced', ['-include', str(header)])
        shared = sorted(archive_names(namespaced, '-U', env) & kernel)
        if shared:
            sys.exit('Defined by both XNU and the namespaced archive (assembly is not renamed): ' + ' '.join(shared))
        Path(args.out).write_bytes(namespaced.read_bytes())
        Path(args.namespace_header).write_text(header.read_text())


if __name__ == '__main__':
    main()
