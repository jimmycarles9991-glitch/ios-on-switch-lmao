"""check.py's `gpu` milestone: the GM20B kext's bring-up and the kernel's own channel traffic on the emulated GPU.

It is reached at the boot pool scrub's queued line once M3 and M4 have passed: by then the self-test channel, the
copy-engine fill it checks and the scrubber's channel have run on the model (patch 0030 runs a channel's work when
its GP_PUT is written). A bring-up FAIL line, a report of nvkm's or the kext's that only a GPU error produces, or a
fault or stopped channel in the model's own log fails it, as does a run in which the GPU executed no method or the
scrubber no fill, or a channel ran a method its class lacks on GM20B (gpulog.py), which the GPU kills the channel for
and the model does not.
"""

import re
from collections.abc import Sequence
from pathlib import Path

from tools.emu import gpulog

REACHED = re.compile(r'tegra-gm20b: pool scrub: \d+ MiB queued')
# The queued line comes only from a claim that finds the pool clean (gpu_scrub.c scrub_work). When the boot scrub's last
# range fills SCRUB_INFLIGHT, the claim loop ends on that bound instead, and once nothing is left dirty no completion
# schedules another claim, so the line never comes although every range was queued. The scrub queues the pool within a
# tenth of a second of M3 and panics on a range not back after SCRUB_NS (1 s), so a kernel line this long after M3,
# with no panic, also reaches the milestone.
SCRUB_SETTLED_S = 2.0
STAMP = re.compile(r'^\[\s*(\d+\.\d+)\]')
# The scrubber's fill (gpu_scrub.c scrub_submit): copy-engine lines of SCRUB_LINE bytes, 32768 four-byte elements.
SCRUB_FILL = ('b0b5', '0418', '00008000')
REQUIRED = (re.compile(r'tegra-gm20b: M3 ok'), re.compile(r'tegra-gm20b: M4 ok'))
FAILURES = re.compile(
    r'tegra-gm20b: M\d FAIL'
    r'|tegra-gm20b WARN '
    r'|pool scrub unavailable'
    r'|57000000\.gpu: \w+:[0-9a-f]+: !'
    r'|PBDMA\d+: '
    r'|fault [0-9a-f]{2} \[\w+\] at '
    r'|rc scheduled'
    r'|channel killed'
    r'|GPU error in '
    r'|GPU hang'
    r'|wait for idle timeout'
    r'|GPU scheduler error'
    r'|GPU bind error'
)


def failure(log: Sequence[str]) -> str | None:
    return next((line for line in log if FAILURES.search(line)), None)


def stamp(line: str) -> float | None:
    match = STAMP.match(line)
    return float(match.group(1)) if match else None


def reached(log: Sequence[str]) -> str | None:
    if not all(any(required.search(line) for line in log) for required in REQUIRED):
        return None
    queued = next((line for line in log if REACHED.search(line)), None)
    if queued:
        return queued
    m3 = next(stamp(line) for line in log if REQUIRED[0].search(line))
    if m3 is None:
        return None
    return next((line for line in log if (at := stamp(line)) is not None and at >= m3 + SCRUB_SETTLED_S), None)


def after_run(run: Path) -> list[str]:
    """Return what the model's logs (complete once QEMU has quit) hold against the milestone."""
    problems = []
    raw = run / 'gpu-methods.raw'
    methods = [line.split() for line in raw.read_text().splitlines() if line.strip()] if raw.exists() else []
    if not methods:
        problems.append('the GPU executed no method')
    elif not any(tuple(method[2:5]) == SCRUB_FILL for method in methods):
        problems.append('the scrubber ran no fill')
    if methods:
        classes = gpulog.load_classes()
        named = (gpulog.decode_line(' '.join(method), classes) for method in methods)
        problems += [line for line, flag in named if flag == 'NOT_IN_CLASS']
    problems += gpulog.fault_lines(run / 'gpu-events.log')
    return problems
