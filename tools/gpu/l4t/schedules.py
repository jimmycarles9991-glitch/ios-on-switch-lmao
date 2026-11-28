"""Decode SM53 control words from paired NAK/R32 streams and their envydis listings."""

import argparse
import csv
import re
import statistics
import struct
import sys
from collections import Counter, defaultdict
from dataclasses import dataclass
from functools import cache
from itertools import pairwise
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[3]))
from tools.gpu.maxwell import isa

opcode = cache(isa.name)
FLOW = {
    'BRA',
    'BRX',
    'SSY',
    'SYNC',
    'BRK',
    'PBK',
    'PCNT',
    'CONT',
    'EXIT',
    'KIL',
    'CAL',
    'JCAL',
    'RET',
    'PRET',
    'BPT',
    'JMP',
    'JMX',
    'PEXIT',
    'LONGJMP',
}
LOAD = {'LD', 'LDG', 'LDS', 'LDL', 'LDC', 'SULD', 'TEX', 'TEXS', 'TLD', 'TLDS', 'TLD4', 'TLD4S', 'TXQ', 'TMML'}
ALU = {'FFMA', 'HFMA2', 'FADD', 'FMUL', 'HADD2', 'HMUL2', 'XMAD', 'IADD', 'IADD3', 'LOP', 'LOP3', 'PRMT'}
ROW = re.compile(r'^([0-9a-f]+): ([0-9a-f]{8}) ([0-9a-f]{8})\s+(?:([CB]+)\s+)?(.+)')
CTRL = ('stall', 'yield_bit', 'write', 'read', 'wait', 'reuse')


@dataclass
class Slot:
    pc: int
    word: int
    form: str
    asm: str
    target: bool
    control: tuple[int, ...]

    @property
    def op(self) -> str:
        return self.form.split('_')[0]


def stream(binary: Path, listing: Path) -> list[Slot]:
    raw = binary.read_bytes()
    if not raw or len(raw) % 32:
        raise ValueError(f'{binary}: incomplete bundle')
    slots = []
    bundle_target = False
    seen = 0
    for line in listing.read_text().splitlines():
        if not line.strip():
            continue
        m = ROW.fullmatch(line)
        if not m:
            raise ValueError(line)
        pc, lo, hi, label, asm = m.groups()
        pc = int(pc, 16)
        word = (int(hi, 16) << 32) | int(lo, 16)
        if pc != seen * 8 or word != struct.unpack_from('<Q', raw, pc)[0]:
            raise ValueError(f'{listing}: listing differs at {pc:x}')
        seen += 1
        if pc % 32 == 0:
            bundle_target = bool(label)
            control = word
            continue
        c = control >> (21 * (pc % 32 // 8 - 1))
        fields = (c & 15, c >> 4 & 1, c >> 5 & 7, c >> 8 & 7, c >> 11 & 63, c >> 17 & 15)
        slots.append(Slot(pc, word, opcode(word), asm.split(' [unknown:')[0], bool(label) or bundle_target, fields))
        bundle_target = False
    if seen * 8 != len(raw):
        raise ValueError(f'{listing}: incomplete listing')
    return slots


def signature(s: Slot) -> str:
    # Keep opcode suffixes and operand modifiers, redact registers, literals and component addresses.
    text = re.sub(r'^(?:not )?\$p\d+ ', '', s.asm)
    text = re.sub(r'\$[rp]\d+', 'REG', text)
    return re.sub(r'0x[0-9a-f]+', '#', text)


def hist(c: Counter) -> str:
    return ' '.join(f'{v}={n}' for v, n in sorted(c.items()))


def latency(op: str) -> str:
    if op in {'HSET2', 'HSETP2', 'HMNMX2', 'DADD', 'DFMA', 'DMNMX', 'DMUL', 'DSETP'}:
        return 'RAW GPR/P 13/14'
    if op in {'MUFU', 'F2F', 'F2I', 'I2F', 'FLO', 'LDC', 'IMUL'}:
        return 'scoreboard ' + str({'LDC': 24, 'IMUL': 128}.get(op, 13))
    if op in {'BAR', 'MEMBAR', 'EXIT', 'CCTL', 'OUT'}:
        return 'exec 15'
    if op in FLOW:
        return 'exec 13' if op in {'BRA', 'SSY', 'SYNC', 'BRK', 'PBK', 'CONT', 'PCNT', 'KIL'} else 'exec 1'
    return 'exec 1; RAW GPR/P 6/13 if fixed'


def study(slots: list[Slot]) -> tuple:
    groups = defaultdict(lambda: [Counter() for _ in CTRL])
    forms, banks = Counter(), Counter()
    for s in slots:
        base = s.op
        if base == 'UNKNOWN':
            raise ValueError(f'unclassified instruction at {s.pc:x}: {s.asm}')
        for c, v in zip(groups[base], s.control, strict=True):
            c[v] += 1
        forms[(s.form, signature(s))] += 1
        if base in ALU and s.form.endswith('_reg'):
            sources = [s.word >> 8 & 255, s.word >> 20 & 255]
            if base in {'FFMA', 'HFMA2', 'XMAD', 'IADD3', 'LOP3', 'PRMT'}:
                sources.append(s.word >> 39 & 255)
            pattern = '/'.join('Z' if r == 255 else str(r % 4) for r in sources)
            real = set(sources) - {255}
            conflict = len(real) - len({r % 4 for r in real})
            banks[(s.form, pattern, conflict, s.control[5])] += 1
    pairs, waits, examples = schedule_edges(slots)
    return groups, forms, banks, pairs, waits, examples


def schedule_edges(slots: list[Slot]) -> tuple:
    pairs, waits, examples, outstanding = Counter(), Counter(), {}, {}
    for a, b in pairwise(slots):
        if a.control[0] == 0 and a.op not in FLOW | {'NOP'} and b.op not in FLOW | {'NOP'} and not b.target:
            key = a.op, b.op
            pairs[key] += 1
            examples.setdefault(('pair', *key), a.pc)
    for i, s in enumerate(slots):
        if s.target:
            outstanding.clear()
        _, _, wr, rd, mask, _ = s.control
        for bar, (at, source) in list(outstanding.items()):
            if mask & (1 << bar):
                waits[(source, s.op, i - at)] += 1
                examples.setdefault(('wait', source, s.op, i - at), slots[at].pc)
                del outstanding[bar]
        if s.op in FLOW | {'DEPBAR'}:
            outstanding.clear()
        else:
            for bar in (rd, wr):
                outstanding.pop(bar, None)
            if s.op in LOAD and wr < 6:
                outstanding[wr] = (i, s.op)
    return pairs, waits, examples


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('profile', type=Path)
    ap.add_argument('output', type=Path)
    args = ap.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    tables = defaultdict(list)
    summary = defaultdict(lambda: defaultdict(lambda: [Counter() for _ in CTRL]))
    for case in sorted(args.profile.iterdir()):
        (record,) = case.glob('paired/*.json')
        for stage in ('vert', 'frag'):
            for backend, directory in [('NAK', 'paired'), ('NVIDIA', 'nvidia')]:
                stem = case / directory / stage
                slots = stream(stem.with_suffix('.sass'), stem.with_suffix('.asm'))
                groups, forms, banks, pairs, waits, examples = study(slots)
                prefix = (record.stem, stage, backend)
                for op, cs in groups.items():
                    tables['controls'].append((*prefix, op, *map(hist, cs)))
                    for total, c in zip(summary[op][backend], cs, strict=True):
                        total.update(c)
                for key, n in sorted(forms.items()):
                    tables['forms'].append((*prefix, *key, n))
                for key, n in sorted(banks.items()):
                    tables['banks'].append((*prefix, *key, n))
                for group, counter, name in [('pairs', pairs, 'pair'), ('waits', waits, 'wait')]:
                    for key, n in sorted(counter.items()):
                        tables[group].append((*prefix, *key, n, hex(examples[(name, *key)])))
    write_tables(args.output, tables)
    print_summary(summary)


def write_tables(output: Path, tables: dict) -> None:
    heads = {
        'controls': ('class', *CTRL),
        'forms': ('form', 'operands', 'count'),
        'banks': ('form', 'source_banks', 'extra_reads', 'reuse_mask', 'count'),
        'pairs': ('first', 'second', 'count', 'first_pc'),
        'waits': ('producer', 'waiter', 'slot_distance', 'count', 'producer_pc'),
    }
    for name, rows in tables.items():
        with (output / f'{name}.tsv').open('w', newline='') as f:
            w = csv.writer(f, delimiter='\t', lineterminator='\n')
            w.writerow(('state', 'stage', 'backend', *heads[name]))
            w.writerows(rows)


def print_summary(summary: dict) -> None:
    print(
        '| Class | Backend | Slots | Stall | yl=1 | Write barrier | Read barrier | '
        'Wait mask | Reuse mask | NAK model | Median stall comparison |'
    )
    print('|---|---|---:|---|---:|---|---|---|---|---|---|')
    for op, backends in sorted(summary.items()):
        med = {b: statistics.median(c[0].elements()) for b, c in backends.items()}
        if len(med) == 2:
            d = med['NVIDIA'] - med['NAK']
            mark = 'NVIDIA shorter' if d < 0 else 'NVIDIA longer' if d > 0 else 'equal median'
        else:
            mark = 'NVIDIA uses a form/flag NAK lacks in corpus' if 'NVIDIA' in med else 'NAK only in corpus'
        for backend, cs in sorted(backends.items()):
            stall, yl, wr, rd, wt, ru = cs
            print(
                f'| {op} | {backend} | {sum(stall.values())} | {hist(stall)} | {yl[1]} | {hist(wr)} | {hist(rd)} | '
                f'{hist(wt)} | {hist(ru)} | {latency(op)} | {mark} |'
            )


if __name__ == '__main__':
    main()
