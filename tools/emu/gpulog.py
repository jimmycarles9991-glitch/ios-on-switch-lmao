"""The emulated GPU's method and fault logs, named with Mesa's class headers (third_party/mesa, MIT).

QEMU's GPU model (patch 0030) writes one line per method a channel executes, `chid subc class method data [flag]`
in hex, and a line per bring-up event (falcon starts, PMU and FECS commands, runlist submits), MMU fault or stopped
channel. This names each method from the class's header and flags the
ones the class does not have, the subchannels with no class bound and the opcodes the host rejected.
"""

import ast
import collections
import fnmatch
import re
from pathlib import Path

HEADERS = Path(__file__).parents[2] / 'third_party/mesa/src/nouveau/headers/nvidia/classes'
# Mesa's own array lengths (class_parser.py METHOD_ARRAY_SIZES): the headers give only an array's base and stride.
CLASS_PARSER = HEADERS.parent.parent / 'class_parser.py'
CLASSES = {
    0xB06F: ('MAXWELL_CHANNEL_GPFIFO_A', 'clb06f.h'),
    0xB0B5: ('MAXWELL_DMA_COPY_A', 'clb0b5.h'),
    0xB197: ('MAXWELL_B', 'clb197.h'),
    0xB1C0: ('MAXWELL_COMPUTE_B', 'clb1c0.h'),
    0x902D: ('FERMI_TWOD_A', 'cl902d.h'),
}
DEFINE = re.compile(
    r'^#define\s+NV[0-9A-F]{4}_(\w+?)(\(\w\))?\s+\(?\s*\(?(0x[0-9A-Fa-f]+)\)?\s*(?:\+\s*\(\w\)\s*\*\s*(\d+))?'
)
HOST_METHODS_END = 0x100
ARRAY_DEFAULT = 16


def array_sizes(parser: Path = CLASS_PARSER) -> dict[str, int]:
    text = parser.read_text()
    start = text.index('METHOD_ARRAY_SIZES = {') + len('METHOD_ARRAY_SIZES = ')
    return ast.literal_eval(text[start : text.index('}', start) + 1])


def array_size(name: str, sizes: dict[str, int]) -> int:
    return next((n for pattern, n in sizes.items() if fnmatch.fnmatchcase(name, pattern)), ARRAY_DEFAULT)


class Class:
    """One class's methods: fixed offsets, and arrays as (base, stride, count, name)."""

    def __init__(self, text: str, sizes: dict[str, int]):
        self.methods: dict[int, str] = {}
        self.arrays: list[tuple[int, int, int, str]] = []
        current = None
        for line in text.splitlines():
            match = DEFINE.match(line)
            if not match:
                continue
            name, index, value, stride = match.groups()
            if current and name.startswith(current + '_'):
                continue
            current = name
            if index:
                self.arrays.append((int(value, 16), int(stride or 4), array_size(name, sizes), name))
            else:
                self.methods.setdefault(int(value, 16), name)

    def name(self, method: int) -> str | None:
        if method in self.methods:
            return self.methods[method]
        for base, stride, count, name in self.arrays:
            if base <= method < base + stride * count and (method - base) % stride == 0:
                return f'{name}({(method - base) // stride})'
        return None


def load_classes(headers: Path = HEADERS) -> dict[int, Class]:
    sizes = array_sizes(headers.parent.parent / 'class_parser.py')
    return {number: Class((headers / header).read_text(), sizes) for number, (_, header) in CLASSES.items()}


def decode_line(line: str, classes: dict[int, Class]) -> tuple[str, str | None]:
    """Return the line with names, and the flag it raises (None for a method its class has)."""
    fields = line.split()
    chid, subc, cls, method, data = fields[0], fields[1], int(fields[2], 16), int(fields[3], 16), fields[4]
    flag = fields[5] if len(fields) > 5 else None
    if method < HOST_METHODS_END:
        cls = 0xB06F
    known = classes.get(cls)
    class_name = CLASSES[cls][0] if cls in CLASSES else f'class {cls:04x}'
    name = known.name(method) if known else None
    if flag is None and name is None:
        flag = 'NOT_IN_CLASS' if known else 'UNKNOWN_CLASS'
    named = f'{chid:>4} {subc} {class_name} {name or f"{method:#06x}"} {data}'
    return named + (f' !{flag}' if flag else ''), flag


def decode(raw: Path, out: Path, classes: dict[int, Class] | None = None) -> collections.Counter:
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
