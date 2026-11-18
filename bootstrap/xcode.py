import os
import shutil
import subprocess
from pathlib import Path


def replace_once(path: Path, before: str, after: str) -> None:
    text = path.read_text()
    if text.count(before) != 1:
        raise ValueError(f'{path.name}: pinned source no longer matches')
    path.write_text(text.replace(before, after))


def run(command: list[str], cwd: Path, log: Path, env: dict) -> None:
    with log.open('wb') as output:
        result = subprocess.run(command, cwd=cwd, stdout=output, stderr=subprocess.STDOUT, env=env, check=False)
    if result.returncode:
        lines = log.read_text(errors='replace').splitlines()
        errors = [line for line in lines if 'error:' in line and len(line) < 1500]
        tail = '\n'.join(errors[:40] + [line for line in lines[-8:] if len(line) < 1500])
        raise RuntimeError(f'{command[0]} failed ({result.returncode}):\n{tail}')


def copy_source(source: Path, destination: Path) -> None:
    shutil.copytree(source, destination)
    for path in [destination, *destination.rglob('*')]:
        path.chmod(path.stat().st_mode | 0o200)


def xcode(root: Path, project: str, scheme: str, env: dict, settings: tuple[str, ...] = ()) -> Path:
    work = root.parent
    derived = work / (project + '-derived')
    command = [
        '/usr/bin/xcodebuild',
        '-quiet',
        '-project',
        project + '.xcodeproj',
        '-scheme',
        scheme,
        '-configuration',
        'Release',
        '-sdk',
        'iphoneos',
        '-destination',
        'generic/platform=iOS',
        '-derivedDataPath',
        str(derived),
        '-disableAutomaticPackageResolution',
        '-packageCachePath',
        str(work / 'package-cache'),
        'CODE_SIGNING_ALLOWED=NO',
        'CODE_SIGNING_REQUIRED=NO',
        'ARCHS=arm64',
        'IPHONEOS_DEPLOYMENT_TARGET=17.0',
        'DEBUG_INFORMATION_FORMAT=dwarf',
        'OTHER_LDFLAGS=$(inherited) -Wl,-segalign,0x1000 -Wl,-headerpad,0x1000',
        'OTHER_SWIFT_FLAGS=$(inherited) -debug-prefix-map ' + str(work) + '=. -file-prefix-map ' + str(work) + '=.',
        'OTHER_CFLAGS=$(inherited) -ffile-prefix-map='
        + str(work)
        + '=. -fdebug-prefix-map='
        + str(work)
        + '=. -fmacro-prefix-map='
        + str(work)
        + '=.',
        'OTHER_CPLUSPLUSFLAGS=$(inherited) -ffile-prefix-map='
        + str(work)
        + '=. -fdebug-prefix-map='
        + str(work)
        + '=. -fmacro-prefix-map='
        + str(work)
        + '=.',
        'build',
    ]
    command.extend(settings)
    run(command, root, work / (scheme + '.log'), env)
    return derived / 'Build/Products/Release-iphoneos'


def build_environment(xcode_env: Path, work: Path) -> dict:
    env = dict(os.environ, LC_ALL='C', TZ='UTC', SOURCE_DATE_EPOCH='0')
    for line in xcode_env.read_text().splitlines():
        if line.startswith('DEVELOPER_DIR='):
            env['DEVELOPER_DIR'] = line.split('=', 1)[1]
    env['CLANG_MODULE_CACHE_PATH'] = str(work / 'clang-cache')
    env['SWIFTPM_MODULECACHE_OVERRIDE'] = str(work / 'manifest-cache')
    env['CFFIXED_USER_HOME'] = str(work / 'user-cache')
    return env


def strip_products(out: Path, work: Path, env: dict) -> None:
    for path in out.rglob('*'):
        if not path.is_file():
            continue
        data = path.read_bytes()
        if data[:4] != b'\xcf\xfa\xed\xfe':
            continue
        subprocess.run(
            ['/usr/bin/xcrun', '--sdk', 'iphoneos', 'strip', '-S', str(path)], env=env, check=True, capture_output=True
        )
        if str(work).encode() in path.read_bytes():
            raise ValueError(f'{path.name}: Mach-O embeds the temporary build path')
