"""Boot this workspace's kernel on the emulated Switch and write a run directory (README.md).

    bazel run //tools/emu:boot -- --kind=ios17|ios27 [--sd IMAGE|none] [--seconds N] [--bootargs ARGS]

Builds //:boot for the kind in this workspace and output base (--bazel-flag passes flags to that build), gets the
patched tegra_qemu from the shared cache, boots it on a card over the shared SD base for N seconds of wall time, stops
the machine and saves what it shows. The run directory holds the UART log, the kernel log, the panic, the screen and
summary.txt.
"""

import argparse
import contextlib
import os
import shutil
import subprocess
import sys
import time
from collections.abc import Callable
from pathlib import Path

from tools.emu import machine, qemu, report, sdcard
from tools.emu.monitor import Monitor, socket_path
from tools.install import stages

KINDS = {'ios17': '21H461', 'ios27': '24B119'}
WATCH_SECONDS = 3


def workspace() -> Path:
    return Path(os.environ.get('BUILD_WORKSPACE_DIRECTORY', os.getcwd()))


def output_base() -> Path:
    runfiles = os.environ.get('RUNFILES_DIR') or os.path.abspath(sys.argv[0])
    if '/execroot/' not in runfiles:
        sys.exit('run this through `bazel run //tools/emu:boot`, or give --package')
    return Path(runfiles.split('/execroot/')[0])


def bazel(base: Path, *args: str) -> str:
    command = ['bazel', f'--output_base={base}', *args]
    result = subprocess.run(command, cwd=workspace(), stdout=subprocess.PIPE, check=False, text=True)
    if result.returncode:
        sys.exit(f'{" ".join(command)} failed')
    return result.stdout.strip()


def build_package(kind: str, flags: list[str]) -> tuple[Path, Path]:
    """Build //:boot for the kind; return it and the prelinked kernel it carries (its symbols find the live log)."""
    base = output_base()
    options = [f'--//ipsw:kind={KINDS[kind]}', *flags]
    # The kernel is a top-level target too: Bazel takes only those out of the disk cache, so with //:boot alone a cache
    # hit leaves no kernel file at the path cquery names. A Mac on the remote host downloads nothing by default.
    bazel(base, 'build', *options, '--remote_download_toplevel', '//:boot', '//kernel:prelinked')
    root = Path(bazel(base, 'info', *options, 'execution_root'))
    package, kernel = (bazel(base, 'cquery', *options, '--output=files', t) for t in ('//:boot', '//kernel:prelinked'))
    return root / package, root / kernel


def default_image(kind: str) -> Path | None:
    """Return the kind's install image (its first part) from out/sd here or in the main checkout, or a shared one."""
    common = subprocess.run(
        ['git', 'rev-parse', '--path-format=absolute', '--git-common-dir'],
        cwd=workspace(),
        capture_output=True,
        text=True,
        check=False,
    ).stdout.strip()
    name = f'switchroot/ios/IOS{kind[3:]}.SII.001'
    roots = [workspace() / 'out/sd', Path(common).parent / 'out/sd' if common else None]
    roots.append(Path.home() / f'Library/Caches/switch-ios/sd-{KINDS[kind]}')
    for root in roots:
        if root and (root / name).exists():
            return root / name
    scratch = stages.default_scratch(KINDS[kind])
    for stage in (stages.POPULATED, stages.COMPOSED):
        if (scratch / stage).exists() and (scratch / 'state.json').exists():
            return scratch / stage
    return None


def card_files(package: Path, run: Path, bootargs: str | None) -> dict[str, Path]:
    files = {
        str(path.relative_to(package)): path
        for top in ('bootloader', 'switchroot')
        for path in (package / top).rglob('*')
        if path.is_file()
    }
    if bootargs is not None:
        (run / 'bootargs.txt').write_text(bootargs + '\n')
        files['switchroot/ios/bootargs.txt'] = run / 'bootargs.txt'
    return files


def regwrite() -> Path:
    return Path(__file__).parent / 'regwrite.bin'


def run_machine(
    run: Path, inputs: machine.Inputs, seconds: float, done: Callable[[Monitor], bool] | None = None
) -> None:
    """Run the machine for `seconds` of wall time, or until `done` (asked every WATCH_SECONDS) says so; save the run."""
    command = machine.command(run, inputs)
    with (run / 'qemu.log').open('w') as log:
        process = subprocess.Popen(command, cwd=run, stdout=log, stderr=subprocess.STDOUT)
    try:
        while not inputs.qmp.exists() and process.poll() is None:
            time.sleep(0.1)
        if process.poll() is not None:
            sys.exit(f'qemu exited at once ({run / "qemu.log"}):\n' + (run / 'qemu.log').read_text()[-2000:])
        monitor = Monitor(inputs.qmp)
        deadline = time.monotonic() + seconds
        asked = time.monotonic()
        while time.monotonic() < deadline and process.poll() is None:
            time.sleep(0.5)
            if done and time.monotonic() - asked >= WATCH_SECONDS:
                asked = time.monotonic()
                if done(monitor):
                    break
        if process.poll() is None:
            report.capture(monitor, run, inputs.kernel)
            with contextlib.suppress(OSError, RuntimeError):
                monitor.command('quit')
        monitor.close()
        process.wait(timeout=30)
    finally:
        if process.poll() is None:
            process.kill()
        shutil.rmtree(inputs.qmp.parent, ignore_errors=True)


def add_arguments(parser: argparse.ArgumentParser) -> None:
    parser.add_argument('--kind', choices=sorted(KINDS), required=True)
    parser.add_argument('--sd', help="IOS27.SII.001, a raw stage image (populated.img) or none (default: //:image's)")
    parser.add_argument('--bootargs', help='switchroot/ios/bootargs.txt for this run, e.g. -v')
    parser.add_argument('--out', type=Path, help='the run directory (default out/emu/<kind>-<time>)')
    parser.add_argument('--package', type=Path, help='a //:boot output to boot instead of building one')
    parser.add_argument('--kernel', type=Path, help="--package's prelinked kernel, for the live kernel log")
    parser.add_argument('--bazel-flag', action='append', default=[], help='a flag for the //:boot build')
    parser.add_argument('--qemu', type=Path, help='this qemu-system-aarch64 instead of the cached build')
    parser.add_argument('--mttcg', action='store_true', help='one host thread per core, guest time from the clock')


def prepare(args: argparse.Namespace) -> tuple[Path, machine.Inputs]:
    """Build or take the boot package, get the emulator, make the run directory and its card."""
    if args.package:
        package, kernel = args.package.resolve(), args.kernel
    else:
        package, kernel = build_package(args.kind, args.bazel_flag)
    binary = args.qemu.resolve() if args.qemu else qemu.ensure()
    run = (args.out or workspace() / 'out/emu' / f'{args.kind}-{time.strftime("%Y%m%d-%H%M%S")}').resolve()
    run.mkdir(parents=True, exist_ok=True)
    image = None if args.sd == 'none' else Path(args.sd) if args.sd else default_image(args.kind)
    card = sdcard.card_for_run(image, run, card_files(package, run, args.bootargs))
    inputs = machine.Inputs(
        qemu=binary,
        package=package,
        regwrite=regwrite(),
        sd=card,
        qmp=socket_path(),
        kernel=kernel,
        icount=not args.mttcg,
    )
    return run, inputs


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    add_arguments(parser)
    parser.add_argument('--seconds', type=float, default=120)
    args = parser.parse_args()
    run, inputs = prepare(args)
    run_machine(run, inputs, args.seconds)
    print(report.summarize(run), end='')


if __name__ == '__main__':
    main()
