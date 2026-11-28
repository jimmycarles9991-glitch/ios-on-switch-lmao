"""The tier-0 pack image's place in a boot, in uptime (mach time): launchd's exec, the image's build, open and stream,
backboardd's device open and its wait there, and the boot time, uptime 0 to backboardd's "Dismissing boot logo".

The kernel logs one line when the stream ends, holding how long after the build began the image opened and how long
after that it streamed, so the open and the build's start are counted back from that line. backboardd names its wait
only when it is 1 ms or more; its pack window line marks an open that waited less.
"""

from tools.devicerun.rules import INFO, NO_DATA, REPORT, Check, Result
from tools.devicerun.springboard import LOGO, MACH_HZ, in_boot

OWNER = 'pack-image'
LAUNCHD = 'boot.pack-launchd-exec'
IMAGE = 'boot.pack-image-stream'
OPENED = 'boot.pack-backboardd-open'
WAITED = 'boot.pack-backboardd-wait'
DEBUG_KERNELS = 'debug kernels'

PACK_IMAGE = [
    Check(
        LAUNCHD,
        OWNER,
        'SwitchBootSteps: before launchd',
        REPORT,
        process='kernel',
        source=f'kernel/xnu-*/bsd/kern/kern_exec.c ({DEBUG_KERNELS})',
    ),
    Check(
        IMAGE,
        OWNER,
        'tegra-gm20b: pack image: tier 0 ',
        REPORT,
        process='kernel',
        regex=(
            r'tier 0 (?P<mib>\d+) MiB for (?P<first>\S+), open (?P<open>\d+) ms after the build began \(tables \w+\), '
            r'streamed (?P<stream>\d+) ms after that .*?; (?P<jobs>\d+) jobs waited on it on the GPU, the first '
            r'submitted (?P<ahead>\d+) ms before its end'
        ),
        show=('first', 'open', 'stream', 'jobs'),
        source=f'drivers/TegraGM20B/SwitchGpuPackImage.cpp ({DEBUG_KERNELS})',
    ),
    Check(
        OPENED,
        OWNER,
        'SwitchMetal: backboardd: pack window mapped in ',
        REPORT,
        process='backboardd',
        source='drivers/TegraGM20BMetal/gpu.cpp (debug builds)',
    ),
    Check(
        WAITED,
        OWNER,
        ' at device open for the pack image',
        REPORT,
        process='backboardd',
        regex=r'waited (?P<ms>\d+) ms at device open',
        source='drivers/TegraGM20BMetal/gpu.cpp (debug builds)',
    ),
]

ROW = Check(
    'boot.pack-image-time',
    OWNER,
    'tegra-gm20b: pack image: tier 0 ',
    REPORT,
    source='the four boot.pack-* lines and backboardd\'s "Dismissing boot logo"',
    note='boot 2693043C (25 Nov, before the lazy image): built 127 MiB in 7442 ms for cameracaptured, backboardd '
    'waited 7138 ms at device open at 29.8 s; compare the boot time across builds, not only the wait',
)


def uptime(line: dict) -> float:
    return line['mach'] / MACH_HZ


def first(lines: list[dict]) -> dict | None:
    return lines[0] if lines else None


def image_text(line: dict | None) -> str:
    if line is None:
        return 'no pack image line'
    end = uptime(line)
    opened = end - int(line['stream']) / 1000
    began = opened - int(line['open']) / 1000
    jobs = int(line['jobs'])
    gpu = f'{jobs} jobs waited on the GPU' + (f', the first {line["ahead"]} ms before the end' if jobs else '')
    return (
        f'pack image began {began:.1f} s, open {opened:.1f} s, streamed {end:.1f} s '
        f'({line["mib"]} MiB, for {line["first"]}; {gpu})'
    )


def open_text(opened: dict | None, waited: list[dict]) -> str:
    if opened is None:
        return 'no backboardd pack window line'
    wait = next((w for w in waited if w['mach'] <= opened['mach']), None)
    return f'backboardd opened {uptime(opened):.1f} s, waited {wait["ms"] if wait else "< 1"} ms'


def boot_text(boot: str, lines: dict[str, list[dict]]) -> str:
    launchd, logo = first(in_boot(lines[LAUNCHD], boot)), first(in_boot(lines[LOGO], boot))
    parts = [
        f'launchd {uptime(launchd):.1f} s' if launchd else 'no launchd line',
        image_text(first(in_boot(lines[IMAGE], boot))),
        open_text(first(in_boot(lines[OPENED], boot)), in_boot(lines[WAITED], boot)),
        f'boot time {uptime(logo):.1f} s' if logo else 'no boot logo dismissal',
    ]
    return f'boot {boot}: ' + '; '.join(parts)


def pack_image_rows(by_check: dict[str, Result]) -> list[Result]:
    """Return the boot-time row of every boot with a pack image or backboardd open line."""
    names = (LAUNCHD, IMAGE, OPENED, WAITED, LOGO)
    lines = {name: by_check[name].matches if name in by_check else [] for name in names}
    if any(line['mach'] <= 0 for name in names for line in lines[name]):
        return [Result(ROW, NO_DATA, 'a line without machTimestamp')]
    boots = sorted({line['boot'] for name in (IMAGE, OPENED) for line in lines[name]})
    if not boots:
        return [Result(ROW, NO_DATA, 'no pack image or backboardd pack window line (debug builds log them)')]
    return [Result(ROW, INFO, ' | '.join(boot_text(boot, lines) for boot in boots))]
