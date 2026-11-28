"""Every lane's owed device checks in one place: their groups for checks.py, and the summary rows devicerun prints."""

from tools.devicerun import owed, owed_gpu, owed_system
from tools.devicerun.rules import NA, REPORT, Check, Result

ORDER = (
    'frame',
    'memory',
    'state',
    'boot',
    'dma',
    'display',
    'power',
    'gputhroughput',
    'gpuformats',
    'gpurender',
    'hwtest2',
    'fetchmsaa',
    'shadergen',
    'devicegates',
    'nohash',
    'nvcompiler',
    'measure',
)
LANES = {lane: {**owed_system.LANES, **owed_gpu.LANES}[lane] for lane in ORDER}
# Lanes the next image does not carry: their checks live on their branch, and their row says why it is N/A.
NOT_IN_IMAGE = {
    'motion': "not in this image: t3/motion-fusion-driver waits for review-motion4's unblocks "
    '(c38-audit/perf1/review-motion4/NOTES.md, "Verdict")',
}
GROUPS = {f'owed-{lane}': owed.checks(entries) for lane, entries in LANES.items()}


def summary(by_check: dict[str, Result], boots: list[str]) -> list[Result]:
    """Return the archive's boots, then one row per lane: FAIL if any check failed, else PASS if any passed."""
    rows = [owed.archive_row(boots)]
    rows += [owed.lane_row(lane, entries, by_check) for lane, entries in LANES.items()]
    rows += [Result(Check(f'owed.{lane}', lane, '', REPORT), NA, why) for lane, why in NOT_IN_IMAGE.items()]
    return rows
