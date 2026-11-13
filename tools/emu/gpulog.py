"""The emulated GPU's method and fault logs, named with Mesa's class headers (third_party/mesa, MIT).

The method arrays are as long as GM20B's (kits/SwitchGM20B/caps/gen_methods.py).

QEMU's GPU model (patch 0030) writes one line per method a channel executes, `chid subc class method data [flag]`
in hex, and a line per bring-up event (falcon starts, PMU and FECS commands, runlist submits), MMU fault or stopped
channel. This names each method from the class's header and flags the
ones the class does not have, the subchannels with no class bound and the opcodes the host rejected.
"""

import collections
from pathlib import Path

import gen_methods

ROOT = Path(__file__).parents[2]
HEADERS = ROOT / 'third_party/mesa/src/nouveau/headers/nvidia/classes'
CLASSES = {
    0xB06F: 'MAXWELL_CHANNEL_GPFIFO_A',
    0xB0B5: 'MAXWELL_DMA_COPY_A',
    0xB197: 'MAXWELL_B',
    0xB1C0: 'MAXWELL_COMPUTE_B',
    0x902D: 'FERMI_TWOD_A',
    0xA140: 'KEPLER_INLINE_TO_MEMORY_B',
}
HOST_METHODS_END = 0x100


def load_classes(headers: Path = HEADERS) -> dict[int, gen_methods.ClassMethods]:
    """Each class's methods as GM20B takes them (kits/SwitchGM20B/caps/gen_methods.py)."""
    return gen_methods.load(
        headers,
        headers.parent.parent / 'class_parser.py',
        ROOT / 'kits/SwitchGM20B/SwitchGM20BTegra.h',
        ROOT / 'kits/SwitchGM20B/caps/limits.tsv',
    )


def decode_line(line: str, classes: dict[int, gen_methods.ClassMethods]) -> tuple[str, str | None]:
    """Return the line with names, and the flag it raises (None for a method its class has)."""
    fields = line.split()
    chid, subc, cls, method, data = fields[0], fields[1], int(fields[2], 16), int(fields[3], 16), fields[4]
    flag = fields[5] if len(fields) > 5 else None
    if method < HOST_METHODS_END:
        cls = 0xB06F
    known = classes.get(cls)
    class_name = CLASSES.get(cls, f'class {cls:04x}')
    name = known.name(method) if known else None
    if flag is None and name is None:
        flag = 'NOT_IN_CLASS' if known else 'UNKNOWN_CLASS'
    named = f'{chid:>4} {subc} {class_name} {name or f"{method:#06x}"} {data}'
    return named + (f' !{flag}' if flag else ''), flag


def decode(raw: Path, out: Path, classes: dict[int, gen_methods.ClassMethods] | None = None) -> collections.Counter:
    """Write the named log; return the count of each flag and of methods per class."""
    classes = classes or load_classes()
    counts: collections.Counter = collections.Counter()
    with raw.open() as source, out.open('w') as sink:
        for line in source:
            if not line.strip():
                continue
            named, flag = decode_line(line, classes)
            sink.write(named + '\n')
            counts['methods'] += 1
            if flag:
                counts[flag] += 1
    return counts


def fault_lines(events: Path) -> list[str]:
    lines = events.read_text().splitlines() if events.exists() else []
    return [line for line in lines if ' fault ' in line or ' stopped: ' in line]


def summarize(run: Path) -> str:
    """Name the run's GPU method log (gpu-methods.log) and describe what both logs hold."""
    raw = run / 'gpu-methods.raw'
    counts = decode(raw, run / 'gpu-methods.log') if raw.exists() else collections.Counter()
    faults = fault_lines(run / 'gpu-events.log')
    flags = ', '.join(f'{flag} {n}' for flag, n in sorted(counts.items()) if flag != 'methods') or 'none flagged'
    first = f'; first: {faults[0].split(" ", 1)[1]}' if faults else ''
    return f'{counts["methods"]} methods ({flags}); {len(faults)} faults or stopped channels{first}'
