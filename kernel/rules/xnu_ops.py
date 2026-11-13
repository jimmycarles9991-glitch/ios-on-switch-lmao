"""Runs one action of kernel/rules/xnu.bzl: a batch of steps of one kind from a table xnu_record.py wrote.

usage: xnu_ops.py SPEC.json

The spec names the roots ({s} source, {o} object root, {d} DSTROOT, {k} KERNEL_SDKROOT, {x} Xcode), the steps, the
tools (host tools built from XNU's SETUP/), and for mig/iig the declared inputs and where to list the unused ones
(Bazel's unused_inputs_list: an edit to a header the step never read does not rerun it).

Kinds: copy (installfile), unifdef (unifdef + decomment + install: installhdrs), unifdef_to, mig, iig, script (doconf,
makesyscalls.sh, make_symbol_aliasing.sh, make_posix_availability.sh, newvers.pl), write, base (the SDK root's other
parts: availability.pl and its headers, libplatform's headers, libdispatch's os/ headers, as tools/kernel/sdkroot.py).
"""

import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from collections.abc import Iterable
from pathlib import Path

HERE = os.getcwd()


class Spec:
    def __init__(self, data: dict[str, object]) -> None:
        self.data = data
        self.roots = data['roots']

    def path(self, key: str) -> str:
        """'o/a/b' -> the exec path of that file."""
        root, _, rest = key.partition('/')
        base = self.roots[root]
        return os.path.join(base, rest) if rest and rest != '.' else base

    def abs(self, key: str) -> str:
        p = self.path(key)
        return p if os.path.isabs(p) else os.path.join(HERE, p)

    def text(self, value: str) -> str:
        """'{o}/x' placeholders in an argument -> absolute paths."""

        def repl(m: re.Match[str]) -> str:
            base = self.roots[m.group(1)]
            return base if os.path.isabs(base) else os.path.join(HERE, base)

        return re.sub(r'\{(\w)\}', repl, value)


def copy_writable(source: str, target: str) -> None:
    """Copy a file with its mode, writable by its owner: a remote worker's inputs are read-only, the sandbox's not."""
    shutil.copyfile(source, target)
    os.chmod(target, (os.stat(source).st_mode & 0o777) | 0o200)


def fail(message: str) -> None:
    sys.exit('xnu_ops: ' + message)


def run(
    command: Iterable[str | Path],
    cwd: str | None = None,
    env: dict[str, str] | None = None,
    stdout: int | None = None,
) -> subprocess.CompletedProcess[bytes]:
    return subprocess.run(
        [str(c) for c in command],
        cwd=cwd,
        env=env,
        stdout=stdout or subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )


def environment(spec: Spec, step: dict[str, object]) -> dict[str, str]:
    env = {
        'PATH': '/usr/bin:/bin:/usr/sbin:/sbin',
        'DEVELOPER_DIR': spec.roots['x'],
        'LC_ALL': 'en_IE.UTF-8',
        'LANG': 'en_IE.UTF-8',
        'TZ': 'UTC',
        'HOME': os.environ.get('TMPDIR', '/tmp'),
        'TMPDIR': os.environ.get('TMPDIR', '/tmp'),
    }
    for key, value in step.get('env', {}).items():
        env[key] = spec.text(value)
    return env


def writable(path: str | Path) -> None:
    Path(path).parent.mkdir(parents=True, exist_ok=True)
    if os.path.lexists(path):
        os.chmod(path, 0o644)
        os.unlink(path)


def do_copy(spec: Spec, step: dict[str, object]) -> None:
    out = spec.path(step['out'])
    writable(out)
    shutil.copyfile(spec.path(step['src']), out)


def unifdef(spec: Spec, step: dict[str, object]) -> bytes:
    tool = spec.roots['x'] + '/Toolchains/XcodeDefault.xctoolchain/usr/bin/unifdef'
    flags = [spec.text(f) for f in step['flags']]
    result = run([tool, *flags, spec.path(step['src'])])
    if result.returncode not in (0, 1):
        fail('unifdef: parse failure for {}: {}'.format(step['src'], result.stderr.decode(errors='replace')))
    return result.stdout


def do_unifdef(spec: Spec, step: dict[str, object]) -> None:
    data = unifdef(spec, step)
    with tempfile.NamedTemporaryFile(delete=False) as tmp:
        tmp.write(data)
    stripped = run([spec.data['tools']['decomment'], tmp.name, 'r'])
    os.unlink(tmp.name)
    if stripped.returncode:
        fail('decomment failed on ' + step['src'])
    if not stripped.stdout:
        fail(
            '{} is only comments after unifdef, so make would not install {}: rerun '
            'bazel run //kernel/xnu-<version>:update_tables'.format(step['src'], step['out'])
        )
    out = spec.path(step['out'])
    writable(out)
    Path(out).write_bytes(data)


def do_unifdef_to(spec: Spec, step: dict[str, object]) -> None:
    out = spec.path(step['out'])
    writable(out)
    Path(out).write_bytes(unifdef(spec, step))


def parse_deps(text: str) -> list[str]:
    """Return the files a make-style dependency file lists (its first target's prerequisites)."""
    text = text.replace('\\\n', ' ')
    _, _, prereqs = text.partition(':')
    out = []
    for raw in re.split(r'(?<!\\) ', prereqs.strip()):
        token = raw.replace('\\ ', ' ').strip()
        if token:
            out.append(token)
    return out


def write_unused(spec: Spec, used: Iterable[str]) -> None:
    """Bazel's unused_inputs_list: the declared inputs (exec paths) a step did not read."""
    target = spec.data.get('unused')
    if not target:
        return
    used = {os.path.normpath(os.path.relpath(u, HERE)) if os.path.isabs(u) else os.path.normpath(u) for u in used}
    unused = [p for p in spec.data['inputs'] if os.path.normpath(p) not in used]
    Path(target).write_text(''.join(p + '\n' for p in unused))


def do_mig(spec: Spec, step: dict[str, object], used: set[str]) -> bool:
    cwd = spec.path(step['cwd'])
    os.makedirs(cwd, exist_ok=True)
    for o in step['outs']:
        writable(spec.path(o))
    before = set(os.listdir(cwd))
    argv = ['-MD'] + [spec.text(a) for a in step['argv']]
    toolchain = spec.roots['x'] + '/Toolchains/XcodeDefault.xctoolchain/usr'
    env = environment(spec, step)
    # What mig would otherwise look up itself: `xcrun -find cc`, and its own ../libexec/migcom.
    env['MIGCC'] = toolchain + '/bin/cc'
    env['MIGCOM'] = toolchain + '/libexec/migcom'
    # Run by /bin/bash, not by mig's #!: exec'ing a script inside an app bundle puts the process in macOS's provenance
    # sandbox for that bundle. On the Mac that runs the remote worker, every process mig then started sat at
    # _dyld_start for seconds while the kernel waited on an App Management approval: 100 s per action, not 0.1 s.
    result = run(['/bin/bash', toolchain + '/bin/mig', *argv], cwd=cwd, env=env)
    if result.returncode:
        fail(
            'mig {} failed:\n{}{}'.format(
                step['src'], result.stdout.decode(errors='replace'), result.stderr.decode(errors='replace')
            )
        )
    # mig -MD leaves <defs>.<pid>.d in the working directory (the preprocessor's, beside its input's name).
    base = os.path.basename(step['src'])[: -len('.defs')]
    deps = [f for f in set(os.listdir(cwd)) - before if f.startswith(base + '.') and f.endswith('.d')]
    if len(deps) != 1:
        return False
    path = os.path.join(cwd, deps[0])
    used.update(d for d in parse_deps(Path(path).read_text()) if not d.endswith('.c'))
    os.unlink(path)
    used.add(spec.path(step['src']))
    return True


def do_iig(spec: Spec, step: dict[str, object], used: set[str]) -> bool:
    cwd = spec.path(step['cwd'])
    os.makedirs(cwd, exist_ok=True)
    for o in step['outs']:
        writable(spec.path(o))
    argv = [spec.text(a) for a in step['argv']]
    tool = spec.roots['x'] + '/Toolchains/XcodeDefault.xctoolchain/usr/bin/iig'
    env = environment(spec, step)
    result = run([tool, *argv], cwd=cwd, env=env)
    if result.returncode:
        fail(
            'iig {} failed:\n{}{}'.format(
                step['src'], result.stdout.decode(errors='replace'), result.stderr.decode(errors='replace')
            )
        )
    # What iig's parse read: the same clang arguments, preprocessed for dependencies only.
    clang = spec.roots['x'] + '/Toolchains/XcodeDefault.xctoolchain/usr/bin/clang'
    source = argv[argv.index('--def') + 1]
    flags = argv[argv.index('--') + 1 :]
    deps = run([clang, '-M', '-MG', *flags, source], cwd=cwd, env=env)
    if deps.returncode:
        return False
    used.update(parse_deps(deps.stdout.decode()))
    used.add(spec.path(step['src']))
    return True


def wrap_iig_impl(spec: Spec, step: dict[str, object]) -> None:
    """iokit/Tests/TestServices' rule: the implementation iig wrote to <out>.tmp, inside #if DEBUG || DEVELOPMENT."""
    out = spec.path(step['wrapped'])
    body = Path(out + '.tmp').read_text()
    Path(out).write_text('#if DEBUG || DEVELOPMENT\n' + body + '#endif\n')


def do_script(spec: Spec, step: dict[str, object]) -> None:
    cwd = spec.path(step['cwd'])
    os.makedirs(cwd, exist_ok=True)
    for o in step['outs']:
        writable(spec.path(o))
    if step.get('pre_copy'):
        out = spec.path(step['outs'][0])
        shutil.copyfile(spec.path(step['pre_copy']), out)
        os.chmod(out, 0o644)
    env = environment(spec, step)
    with tempfile.TemporaryDirectory() as scratch:
        if step['tool'].endswith('/doconf'):
            # doconf runs $OBJROOT/SETUP/config/config: the config tool built from XNU's SETUP/config.
            tooldir = Path(scratch) / 'SETUP/config'
            tooldir.mkdir(parents=True)
            os.symlink(os.path.join(HERE, spec.data['tools']['config']), tooldir / 'config')
            env['OBJROOT'] = scratch
        command = [spec.abs(step['tool'])] + [spec.text(a) for a in step['argv']]
        result = run(command, cwd=cwd, env=env)
    if result.returncode:
        fail(
            '{} failed ({}):\n{}{}'.format(
                step['tool'],
                result.returncode,
                result.stdout.decode(errors='replace'),
                result.stderr.decode(errors='replace'),
            )
        )
    # doconf writes the directories it was given into its Makefile and config file: execution-root relative here, so
    # no sandbox path reaches an output (and the action's result is the same in every checkout).
    for o in step['outs']:
        path = Path(spec.path(o))
        data = path.read_bytes()
        if HERE.encode() in data:
            path.write_bytes(data.replace(HERE.encode() + b'/', b'').replace(HERE.encode(), b'.'))


def do_write(spec: Spec, step: dict[str, object]) -> None:
    out = spec.path(step['out'])
    writable(out)
    Path(out).write_text(step['content'])


# --- The SDK root's other parts (tools/kernel/sdkroot.py, without XNU's installhdrs) ---------------------------------
AVAILABILITY_TEMPLATES = [
    'Availability.h',
    'AvailabilityInternal.h',
    'AvailabilityInternalLegacy.h',
    'AvailabilityInternalPrivate.h',
    'AvailabilityMacros.h',
    'AvailabilityProhibitedInternal.h',
    'AvailabilityVersions.h',
    'VersionMap.h',
    'for_dyld_priv.inc',
    'os_availability.h',
    'dyld_version_defines.h',
]
# AvailabilityVersions-157 dropped the module maps; each is installed only where the checkout still has its template.
AVAILABILITY_MODULEMAPS = {
    'os_availability.modulemap': 'usr/include',
    'os_availability_private.modulemap': 'usr/local/include',
    'AvailabilityPrivate.modulemap': 'usr/local/include',
}


def run_or_fail(command: list[str | Path], env: dict[str, str], cwd: Path | None = None) -> None:
    r = subprocess.run([str(c) for c in command], cwd=cwd, env=env, stdout=None, stderr=subprocess.PIPE, check=False)
    if r.returncode:
        fail('{} failed: {}'.format(command[0], r.stderr.decode(errors='replace')))


def install_base(
    availability: Path, libplatform: Path, libdispatch: Path, out: str | Path, env: dict[str, str]
) -> None:
    """Install the SDK root's other parts into out.

    availability.pl and AvailabilityVersions' headers (its CMake install, INSTALL_KERNEL_HEADERS=1), libplatform's
    public and private headers, libdispatch's os/ headers reduced to their kernel view.
    """
    python3 = sys.executable
    env = dict(env, PATH=os.path.dirname(python3) + ':' + env['PATH'])
    out = Path(out)
    libexec = out / 'usr/local/libexec'
    libexec.mkdir(parents=True, exist_ok=True)
    run_or_fail(
        [
            python3,
            availability / 'availability',
            '--preprocess',
            availability / 'availability',
            libexec / 'availability.pl',
        ],
        env,
        cwd=availability,
    )
    (libexec / 'availability.pl').chmod(0o755)
    with tempfile.TemporaryDirectory() as scratch:
        generated = Path(scratch)
        templates = availability / 'templates'
        for name in [n for n in AVAILABILITY_TEMPLATES if (templates / n).exists()]:
            run_or_fail(
                [
                    python3,
                    libexec / 'availability.pl',
                    '--preprocess',
                    templates / name,
                    generated / name,
                ],
                env,
            )
        modulemaps = {n: d for n, d in AVAILABILITY_MODULEMAPS.items() if (templates / n).exists()}
        for name in modulemaps:
            run_or_fail(
                [
                    'sh',
                    availability / 'unifdef_command.sh',
                    '-UBUILD_FOR_DRIVERKIT',
                    '-o',
                    generated / name,
                    templates / name,
                ],
                env,
            )
        public = [
            'Availability.h',
            'AvailabilityInternal.h',
            'AvailabilityInternalLegacy.h',
            'AvailabilityMacros.h',
            'AvailabilityVersions.h',
        ]
        local = ['AvailabilityInternalPrivate.h', 'AvailabilityProhibitedInternal.h']
        kernel = out / 'System/Library/Frameworks/Kernel.framework/Versions/A'
        places = [(name, out / 'usr/local/include/dyld' / name) for name in ('VersionMap.h', 'for_dyld_priv.inc')]
        places += [(name, out / 'usr/local/include' / name) for name in local]
        places += [(name, out / directory / name) for name, directory in modulemaps.items()]
        places += [('os_availability.h', out / 'usr/include/os/availability.h')]
        places += [(name, out / 'usr/include' / name) for name in public]
        if (generated / 'dyld_version_defines.h').exists():
            places += [('dyld_version_defines.h', out / 'usr/local/include/mach-o/dyld_version_defines.h')]
        places += [(name, kernel / 'PrivateHeaders' / name) for name in local]
        places += [('os_availability.h', kernel / 'Headers/os/availability.h')]
        places += [(name, kernel / 'Headers' / name) for name in public]
        for name, target in places:
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(generated / name, target)
            target.chmod(0o644)
    (out / 'usr/local/include/kernel').mkdir(parents=True, exist_ok=True)
    for source, target in (
        (libplatform / 'include', out / 'usr/local/include'),
        (libplatform / 'private', out / 'usr/local/include'),
        (
            libplatform / 'private/platform',
            out / 'System/Library/Frameworks/Kernel.framework/Versions/A/PrivateHeaders/platform',
        ),
    ):
        shutil.copytree(source, target, symlinks=False, dirs_exist_ok=True, copy_function=copy_writable)
    os_dir = out / 'usr/local/include/os'
    os_dir.mkdir(parents=True, exist_ok=True)
    unifdef = env['DEVELOPER_DIR'] + '/Toolchains/XcodeDefault.xctoolchain/usr/bin/unifdef'
    if not os.path.exists(unifdef):
        unifdef = '/usr/bin/unifdef'
    for header in sorted((libdispatch / 'os').glob('*.h')):
        r = subprocess.run(
            [
                unifdef,
                '-DKERNEL=1',
                '-DOS_FIREHOSE_SPI=1',
                '-DOS_VOUCHER_ACTIVITY_SPI_TYPES=1',
                '-UOS_VOUCHER_ACTIVITY_SPI',
                str(header),
            ],
            capture_output=True,
            env=env,
            check=False,
        )
        if r.returncode not in (0, 1):
            fail(r.stderr.decode())
        (os_dir / header.name).write_bytes(r.stdout)
    (out / '.xnu-10063.141.1-headers').write_text('')


def do_base(spec: Spec, step: dict[str, object]) -> None:
    """Run the base step in a scratch root, then move only its declared outputs into place.

    installhdrs owns the rest of the SDK root's files.
    """
    with tempfile.TemporaryDirectory() as scratch:
        sources = spec.data['base_sources']
        env = environment(spec, step)
        install_base(
            Path(HERE, sources['availability']),
            Path(HERE, sources['libplatform']),
            Path(HERE, sources['libdispatch']),
            Path(scratch),
            env,
        )
        root = Path(scratch)
        for o in step['outs']:
            rel = o.partition('/')[2]
            out = spec.path(o)
            writable(out)
            if not (root / rel).exists():
                fail("the SDK root's base step did not write " + rel)
            shutil.copyfile(root / rel, out)


def do_ignorelist(spec: Spec, step: dict[str, object]) -> None:
    """Write a sanitizer ignore list as san/Makefile's sed does: its parts, src:./ entries rooted at the XNU tree."""
    text = ''.join(Path(spec.path(s)).read_text() for s in step['srcs'])
    out = spec.path(step['out'])
    writable(out)
    Path(out).write_text(re.sub(r'(?m)^src:\./', 'src:' + spec.roots['s'] + '/', text))


KINDS = {
    'ignorelist': do_ignorelist,
    'copy': do_copy,
    'unifdef': do_unifdef,
    'unifdef_to': do_unifdef_to,
    'script': do_script,
    'write': do_write,
    'base': do_base,
}


def main() -> None:
    spec = Spec(json.loads(Path(sys.argv[1]).read_text()))
    # The include directories' files (mig/iig): the parameter file Bazel writes, one exec path per line.
    if len(sys.argv) > 2 and sys.argv[2].startswith('@'):
        spec.data['inputs'] = [line for line in Path(sys.argv[2][1:]).read_text().splitlines() if line]
    used, complete = set(), True
    for step in spec.data['steps']:
        kind = step['kind']
        if kind == 'mig':
            complete = do_mig(spec, step, used) and complete
        elif kind == 'iig':
            complete = do_iig(spec, step, used) and complete
            if step.get('wrapped'):
                wrap_iig_impl(spec, step)
        else:
            KINDS[kind](spec, step)
    if spec.data.get('unused'):
        if complete:
            write_unused(spec, used)
        else:
            Path(spec.data['unused']).write_text('')


if __name__ == '__main__':
    main()
