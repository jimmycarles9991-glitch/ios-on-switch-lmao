"""Boot to a milestone on the emulator and fail on the way there: a gate for the batch builds.

    bazel run //tools/emu:check -- --kind=ios17|ios27 [--until launchd] [--timeout 600] [boot.py's options]

Exits 0 once the kernel log shows the milestone, 1 on a panic (its line printed), 2 when the time runs out first (the
last kernel lines printed), 3 when the kernel runs but no log line comes out of it (what the reader found printed),
4 when the `gpu` milestone's checks fail (gpucheck.py). The run directory is kept as boot.py writes it.
"""

import argparse
import re
import sys
import time
from collections.abc import Callable, Sequence
from dataclasses import dataclass, field
from pathlib import Path

from tools.emu import boot, gpucheck, msgbuf, report
from tools.emu.monitor import Monitor

MILESTONES = {
    'cores': r'ArmCpu: 3 of 3 secondary cores running',
    'display': r'SwitchDisplay: vsync from the DC A vblank interrupt',
    'sd': r'SwitchSD: ADMA2 on',
    'root': r'BSD root: ',
    # A process named after its exec: until launchd runs, pid 1 is init and its mounts are the kernel's.
    'launchd': r'\[\d+\((?!init\)|kernel_task\))[^)]+\)\]|requested by: (?!init |kernel_task )\S+ \(pid \d+\)',
    # launchd's own System Event once its early boot tasks are done: it mapped the shared cache and ran them, which
    # the `launchd` line above, printed from launchd's first dyld calls, does not show.
    'early-boot': r'\[Subsystem: launchd\] \[Event: boot\] Early boot complete',
}
PANIC_TEXT = (report.RAMOOPS[0] + 0x180000, 0x80000)
# Polls (3 s apart) with a core in the kernel and still no log line before check stops: a booting kernel logs from
# its first second, so a run this quiet has a log reader that cannot reach it, or a kernel that stopped before printing.
QUIET_POLLS = 10


Finder = Callable[[Sequence[str]], str | None]


def first_match(pattern: re.Pattern) -> Finder:
    return lambda log: next((line for line in log if pattern.search(line)), None)


@dataclass
class Watch:
    run: Path
    kernel: Path | None
    reach: Finder
    fail: Finder = lambda log: None
    panic: str | None = None
    reached: str | None = None
    failed: str | None = None
    log: list[str] = field(default_factory=list)
    ring: msgbuf.Ring | None = None
    quiet: int = 0

    def __call__(self, monitor: Monitor) -> bool:
        text = self.run / 'panic-text.raw'
        monitor.command('pmemsave', val=PANIC_TEXT[0], size=PANIC_TEXT[1], filename=str(text))
        self.panic = report.panic_text(text.read_bytes())
        text.unlink()
        registers = monitor.human('info registers -a')
        ring = msgbuf.read(monitor, self.kernel, registers)
        if ring:
            # Kept across polls: a small ring (a release kernel's 16 KiB) can wrap past a milestone between two.
            self.ring, self.log = ring, sorted(set(self.log).union(report.kernel_log(ring.data)))
        if not self.log and msgbuf.kernel_cpu(registers) is not None:
            self.quiet += 1
        self.reached = self.reach(self.log)
        self.failed = self.fail(self.log)
        return bool(self.panic or self.reached or self.failed or self.quiet >= QUIET_POLLS)

    def unread(self) -> str:
        if self.ring is None:
            return 'no msgbuf header: none at _msgbufp, none in the kernel window'
        return f'{self.ring.describe()}, no log line in them'


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    boot.add_arguments(parser)
    parser.add_argument('--until', choices=sorted([*MILESTONES, 'gpu']), default='launchd')
    parser.add_argument('--timeout', type=float, default=600, help='wall seconds before giving up')
    args = parser.parse_args()
    if args.until in ('root', 'launchd', 'early-boot'):
        image = None if args.sd == 'none' else Path(args.sd) if args.sd else boot.default_image(args.kind)
        if image is None:
            parser.error(
                f'{args.kind} {args.until} requires an iOS filesystem image; supply --sd IMAGE '
                f'or build //:image for {boot.KINDS[args.kind]}'
            )
        args.sd = str(image)
    run, inputs = boot.prepare(args)
    if args.until == 'gpu':
        watch = Watch(run, inputs.kernel, gpucheck.reached, gpucheck.failure)
    else:
        watch = Watch(run, inputs.kernel, first_match(re.compile(MILESTONES[args.until])))
    started = time.monotonic()
    boot.run_machine(run, inputs, args.timeout, watch)
    wall = time.monotonic() - started
    report.summarize(run, watch.log)
    if args.until == 'gpu' and not watch.failed:
        watch.failed = gpucheck.failure((run / 'kernel.log').read_text().splitlines())
    panic = watch.panic or report.panic_text((run / 'ramoops.raw').read_bytes())
    if panic:
        print(f'PANIC after {wall:.0f} s wall ({run}):\n{panic}')
        return 1
    if watch.failed:
        print(f'GPU FAIL after {wall:.0f} s wall ({run}): {watch.failed}')
        return 4
    problems = gpucheck.after_run(run) if watch.reached and args.until == 'gpu' else []
    if problems:
        print(f'GPU FAIL after {wall:.0f} s wall ({run}):', *problems[:8], sep='\n')
        return 4
    if watch.reached:
        print(f'reached {args.until} after {wall:.0f} s wall: {watch.reached} ({run})')
        return 0
    if watch.quiet >= QUIET_POLLS:
        print(f'NO KERNEL LOG: the kernel ran {QUIET_POLLS} polls without a log line ({watch.unread()}) ({run})')
        return 3
    print(f'TIMEOUT: no {args.until} in {wall:.0f} s wall ({run}); last kernel lines:', *watch.log[-8:], sep='\n')
    return 2


if __name__ == '__main__':
    sys.exit(main())
