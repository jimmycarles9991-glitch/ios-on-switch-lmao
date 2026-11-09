"""Which of a test's metrics are performance, which way is better, and when two runs' numbers cannot be compared."""

import re
import statistics

# Tokens of a metric key (split at '_'): times and losses go down, rates go up. raw_t_ds4, raw_us_<state>,
# wall_us_<state> and raw_gpu_ms0 are times; fill_gpix_s, copy_gbps and fps are rates.
LOWER = re.compile(r'^(?:ms|us|ns|t|latency|missed|dropped|stall|jank|throttle)\d*$')
HIGHER = re.compile(r'^(?:fps|gflops|gbps|mbps|iops|hz|mhz|gpix|mpix|mops|speedup)\d*$')
RATE_SUFFIXES = ('_per_s', '_mb_s', '_gb_s')
# The share of command buffers that fell off raw (gpu.raw_share).
LOWER_SUFFIXES = ('_m2v_pct',)
# A change smaller than either is noise: 10 %, or twice the spread of the earlier runs.
RELATIVE_BAND = 0.10
SIGMAS = 2.0
ABSOLUTE_FLOOR = 0.02
# Subsystems whose numbers NAK_DEBUG changes (it changes how NAK compiles every shader of the process).
GPU_SUBSYSTEMS = ('Metal / GPU', 'Compositor', 'Images')


def direction(key: str) -> int:
    """+1 when a larger value is better, -1 when a smaller one is, 0 for anything that is not performance."""
    tokens = key.split('_')
    if any(HIGHER.match(token) for token in tokens) or ('_' + key).endswith(RATE_SUFFIXES):
        return 1
    if any(LOWER.match(token) for token in tokens) or ('_' + key).endswith(LOWER_SUFFIXES):
        return -1
    return 0


def number(value: object) -> float | None:
    if isinstance(value, bool) or not isinstance(value, int | float):
        return None
    return float(value)


def band(previous: float, earlier: list[float]) -> float:
    """How far from `previous` a value must move to count as a change."""
    spread = SIGMAS * statistics.pstdev(earlier) if len(earlier) >= 3 else 0.0
    return max(RELATIVE_BAND * abs(previous), spread, ABSOLUTE_FLOOR)


def clock_change(entry: dict, other: dict) -> str | None:
    """Return the *_gpu_clock metric (r104: raw_gpu_clock=ns, m2v_gpu_clock=ptimer) two runs of a test disagree on."""
    for key in sorted(set(entry.get('metrics', {})) | set(other.get('metrics', {}))):
        if key.endswith('gpu_clock') and entry.get('metrics', {}).get(key) != other.get('metrics', {}).get(key):
            return key
    return None


def nak_change(entry: dict, identities: tuple[dict, dict]) -> str | None:
    """Say why a GPU test's verdict and numbers mean something else: NAK_DEBUG differs (latest, previous)."""
    nak = [identity.get('nak_debug', '') for identity in identities]
    if nak[0] != nak[1] and entry.get('subsystem') in GPU_SUBSYSTEMS:
        return 'nak_debug changed ({} -> {})'.format(nak[1] or '-', nak[0] or '-')
    return None


def not_comparable(entry: dict, other: dict, identities: tuple[dict, dict]) -> str | None:
    """Why the two runs' numbers for this test must not be compared, or None."""
    clock = clock_change(entry, other)
    return nak_change(entry, identities) or (f'{clock} changed' if clock else None)
