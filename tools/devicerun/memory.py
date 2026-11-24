"""Jetsam's kills beside what the Metal driver held in the process killed, and a ceiling on it in small daemons.

The driver's memory line (debug builds, drivers/TegraGM20BMetal/TegraGM20BMetalFootprint.mm) comes on the stall line's
five-second tick whenever what the device holds or the process's footprint moved, so the last one before a kill is at
most a few seconds old. A process killed for passing its own limit fails, named with that line.

The ceiling is boot 2's (23 Nov): iconservicesagent, limit about 15 MB, was killed three times (22, 16 and 15.7 MB) with
4.7 to 12.5 MB in the driver's pool alone, while its lowest give-back line held 3.6 MB of the driver's (pool 1.9, tables
and rings 1.2, write-combined 0.25, kept imports 0.24) in a 9.5 MB footprint. 5 MiB passes that floor and fails every
kill. A daemon's limit is its active one while it works and its inactive one (iconservicesagent's 15 MB) once idle, so
it is small when the lowest limit any of its lines in a boot names is 20 MiB or less, and judged by the most it held
in that boot under either. Private heaps are kernel memory no task maps: their line names them apart, and the ceiling
leaves them out, as jetsam does.
"""

from tools.devicerun.rules import FAIL, NO_DATA, PASS, REPORT, Check, Result

OWNER = 'pools'
HELD = 'SwitchMetal: memory held '
KILLED = 'memorystatus: killing_'
SMALL_LIMIT_KB = 20 << 10
SMALL_HELD_KB = 5 << 10
OWN_LIMIT = 'per-process-limit'
DRIVER_SOURCE = 'drivers/TegraGM20BMetal/TegraGM20BMetalFootprint.mm'
SPLIT = ('chunks', 'pinned', 'reuse', 'wc', 'heaps', 'kept', 'tables', 'gpu_heaps')

MEMORY = [
    Check(
        'memory.driver-held',
        OWNER,
        HELD,
        REPORT,
        subsystem='com.brunostjohn.switch.TegraGM20BMetal',
        regex=r'memory held (?P<held>\d+) KB: chunks (?P<chunks>\d+) KB in (?P<chunk_count>\d+) \(pinned '
        r'(?P<pinned>\d+) KB in (?P<pinned_count>\d+), kept for reuse (?P<reuse>\d+) KB\), write-combined '
        r'(?P<wc>\d+) KB, heaps (?P<heaps>\d+) KB, kept imports (?P<kept>\d+) KB, tables (?P<tables>\d+) KB; '
        r'GPU-only heaps (?P<gpu_heaps>\d+) KB, volatile heaps (?P<volatile>\d+) KB; footprint (?P<footprint>\d+) KB '
        r'(?:of limit (?P<limit>\d+) KB|of no limit), IOKit (?P<iokit>\d+) KB',
        show=('process',),
        source=DRIVER_SOURCE,
    ),
    Check(
        'memory.jetsam',
        OWNER,
        KILLED,
        REPORT,
        process='kernel',
        regex=r'memorystatus: killing_(?:specific_process|top_process_elevated\d*) pid (?P<pid>\d+) '
        r'\[(?P<name>[^\]]+)\] \((?P<cause>[\w-]+) [^)]*\) (?P<kb>\d+)KB',
        show=('name', 'cause'),
        source='kernel/xnu-*/bsd/kern/kern_memorystatus.c',
    ),
]

LIMIT_KILLS = Check(
    'memory.limit-kills',
    OWNER,
    KILLED,
    'count == 0',
    source=DRIVER_SOURCE,
    note='a process jetsam killed for passing its own limit, with the driver memory line it last wrote',
)
SMALL_DAEMONS = Check(
    'memory.small-daemon-held',
    OWNER,
    HELD,
    f'max(held) <= {SMALL_HELD_KB}',
    source=DRIVER_SOURCE,
    note=f"driver memory where a process's lowest limit is {SMALL_LIMIT_KB} KB or less (boot 2: kills from 6 MB)",
)


def split(line: dict) -> str:
    """Return a driver line's held KB and its split."""
    parts = ' '.join(f'{k} {line[k]}' for k in SPLIT if k in line)
    return f'held {line["held"]} KB ({parts}), footprint {line["footprint"]} KB'


def last_before(lines: list[dict], kill: dict) -> dict | None:
    """Return the killed process's last driver line before its kill, in the same boot."""
    before = [
        f for f in lines if f['boot'] == kill['boot'] and f['process'] == kill['name'] and f['time'] <= kill['time']
    ]
    return before[-1] if before else None


def limit_kills_row(kills: list[dict], held: list[dict]) -> Result:
    own = [k for k in kills if k.get('cause') == OWN_LIMIT]
    others = len(kills) - len(own)
    named = []
    for kill in own:
        line = last_before(held, kill)
        driver = f'{split(line)} at {line["time"]:%H:%M:%S}' if line else 'no driver line'
        named.append(f'{kill["time"]:%H:%M:%S} {kill["name"]}[{kill["pid"]}] {kill["kb"]} KB: {driver}')
    observed = f'{len(own)} {OWN_LIMIT} kills' + (': ' + '; '.join(named) if named else '')
    if others:
        observed += f'; {others} other kills'
    return Result(LIMIT_KILLS, FAIL if own else PASS, observed, own)


def small_daemons_row(held: list[dict]) -> Result:
    by_process: dict[tuple[str, str], list[dict]] = {}
    for f in held:
        by_process.setdefault((f['boot'], f['process']), []).append(f)
    worst = []
    for lines in by_process.values():
        limits = [int(f['limit']) for f in lines if 'limit' in f]
        if limits and min(limits) <= SMALL_LIMIT_KB:
            worst.append({**max(lines, key=lambda f: int(f['held'])), 'lowest': str(min(limits))})
    if not worst:
        return Result(SMALL_DAEMONS, NO_DATA, 'no driver line from a process with a small limit', worst)
    worst.sort(key=lambda f: -int(f['held']))
    over = [f for f in worst if int(f['held']) > SMALL_HELD_KB]
    shown = (over or worst)[:6]
    rows = '; '.join(
        f'{f["process"]} {split(f)} at {f["time"]:%H:%M:%S} (lowest limit {f["lowest"]} KB)' for f in shown
    )
    observed = f'{len(worst)} small processes, {len(over)} over {SMALL_HELD_KB} KB: {rows}'
    return Result(SMALL_DAEMONS, FAIL if over else PASS, observed, over)


def memory_rows(by_check: dict[str, Result]) -> list[Result]:
    """Return the kill and small-daemon rows over the lines the memory checks matched."""

    def lines(name: str) -> list[dict]:
        return by_check[name].matches if name in by_check else []

    held = lines('memory.driver-held')
    return [limit_kills_row(lines('memory.jetsam'), held), small_daemons_row(held)]
