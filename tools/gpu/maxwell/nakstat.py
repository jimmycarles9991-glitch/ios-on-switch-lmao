#!/usr/bin/env python3
"""The raw path's shader scoreboard: what each state's fragment program costs a pixel, from its machine code.

usage: nakstat.py (--pack pack.bin | --dump <directcheck DIRECTCHECK_DUMP dir>) [--corpus <R lines>]... [--json]

A pack (raw/runtime/raw_pack.h) or a directcheck dump gives each state's programs and the register count its pipeline
sets. Each state the corpora name gets a row: instructions by class, GPRs, warps per SM, the time a 1280x720 draw
takes if the program is instruction-bound (the hot-state scenes measure 8-22% above that), and the same from GM20B's
measured issue cycles (cost.py: `cyc`/`ms_c` adding them, `cyc_o`/`ms_o` overlapping them, `unm` instructions of
untimed classes, `bank` same-bank extra reads of distinct registers, `bankr` of every read, `bytes` of code, `stall`
the stall counts it encodes). A program of at most STUB_INSTRUCTIONS writes constants; the corpus totals leave those
out.
"""

import argparse
import json
import re
import struct
import sys
from collections import Counter
from pathlib import Path

if __package__ in (None, ''):
    sys.path.insert(0, str(Path(__file__).resolve().parents[3]))

from tools.gpu.maxwell import cost, isa

PIXELS_1280x720 = 1280 * 720
LANES = 256
CLOCK_HZ = 768e6
STUB_INSTRUCTIONS = 12
REGISTERS_PER_SM = 65536
WARPS_PER_SM = 64
FRAGMENT_REGISTER_COUNT = 0x214C  # SET_PIPELINE_REGISTER_COUNT(5)
NAN_GUARD_MASK = 0x7FFFFFFF

CLASSES = {
    'tex': ('TEX', 'TEXS', 'TLD', 'TLDS', 'TLD4', 'TLD4S', 'TMML', 'TXD', 'TXQ', 'TXA'),
    'conv': ('F2F', 'F2I', 'I2F', 'I2I'),
    'ldc': ('LDC',),
    'half': ('HADD2', 'HMUL2', 'HFMA2', 'HSET2', 'HSETP2'),
    'fp32': ('FADD', 'FMUL', 'FFMA', 'FMNMX', 'FSET', 'FSETP', 'FCMP', 'FSWZADD', 'RRO', 'FCHK'),
    'mufu': ('MUFU',),
    'move': ('MOV', 'SEL', 'PRMT', 'P2R', 'R2P'),
    'ctrl': ('NOP', 'SSY', 'SYNC', 'BRA', 'BRK', 'PBK', 'CONT', 'PCNT', 'EXIT', 'KIL', 'CAL', 'RET', 'JMP', 'DEPBAR'),
    'mem': (
        'IPA',
        'ALD',
        'AST',
        'AL2P',
        'LD',
        'LDG',
        'ST',
        'STG',
        'LDS',
        'STS',
        'LDL',
        'STL',
        'ATOM',
        'ATOMS',
        'RED',
        'SULD',
        'SUST',
        'SUATOM',
        'MEMBAR',
        'CCTL',
        'S2R',
        'CS2R',
        'PIXLD',
        'ISBERD',
        'OUT',
        'BAR',
        'VOTE',
        'SHFL',
    ),
}
ORDER = ['tex', 'conv', 'guard', 'ldc', 'half', 'fp32', 'mufu', 'move', 'ctrl', 'mem', 'int']
COST_COLUMNS = ('cycles', 'cycles_overlap', 'unmeasured', 'bank', 'bank_reads', 'bytes', 'stalls')


def mnemonic_class(name: str) -> str:
    base = name.split('_', 1)[0]
    for cls, names in CLASSES.items():
        if base in names or base.rstrip('32I') in names:
            return cls
    return 'int'


def classify(code: bytes) -> Counter:
    """Instruction classes of a program's code; 'guard' counts the |x| masks of NaN guards inside 'int'."""
    out = Counter()
    for word in isa.instructions(code):
        name = isa.name(word)
        out[mnemonic_class(name)] += 1
        if name == 'LOP32I' and isa.field(word, 20, 32) == NAN_GUARD_MASK:
            out['guard'] += 1
    return out


def warps_per_sm(gprs: int) -> int:
    if gprs <= 0:
        return WARPS_PER_SM
    per_warp = ((gprs + 7) // 8) * 8 * 32
    return min(WARPS_PER_SM, REGISTERS_PER_SM // per_warp // 4 * 4)


def predicted_ms(instructions: int) -> float:
    return instructions * PIXELS_1280x720 / (LANES * CLOCK_HZ) * 1e3


def row(label: str, code: bytes, gprs: int) -> dict:
    c = classify(code)
    n = sum(v for k, v in c.items() if k != 'guard')
    p = cost.program(code)
    return {
        'label': label,
        'instructions': n,
        'gprs': gprs,
        'warps': warps_per_sm(gprs),
        'ms_1280x720': round(predicted_ms(n), 3),
        'cycles': p['cycles'],
        'ms_cycles': round(cost.predicted_ms(p['cycles'], PIXELS_1280x720), 3),
        'cycles_overlap': p['cycles_overlap'],
        'ms_overlap': round(cost.predicted_ms(p['cycles_overlap'], PIXELS_1280x720), 3),
        'unmeasured': p['unmeasured'],
        'bank': p['bank'],
        'bank_reads': p['bank_reads'],
        'bytes': p['bytes'],
        'stalls': p['stalls'],
        **{k: c.get(k, 0) for k in ORDER},
    }


def fnv(b: bytes) -> int:
    h = 0xCBF29CE484222325
    for x in b:
        h = ((h ^ x) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


# A framework's default library other than QuartzCore's: its states' keys end with " | l <framework>"
FRAMEWORK_LIBRARY = re.compile(r' lib=/System/Library/(?:Private)?Frameworks/(\w+)\.framework/default\.metallib')
LIBRARY = re.compile(r' lib=\S+')
# The segments after a line's functions that name one of its state's variants, kept in its key (raw/pack/rawpack.py)
VARIANT_SEGMENTS = ('cubic ', 'xrs=', 'wtex=')


def state_key(line: str) -> str:
    """Give the pack key of a corpus R line (raw/pack/rawpack.py state_key, for lines that link no function)."""
    found = FRAMEWORK_LIBRARY.search(line)
    framework = found[1] if found and found[1] != 'QuartzCore' else ''
    line = LIBRARY.sub('', line.rstrip('\n')).rstrip()
    if line.endswith('|'):
        line = line[:-1].rstrip()
    bar1, bar2 = line.find(' | v '), line.find(' | f ')
    if bar1 < 0 or bar2 < 0:
        return ''
    bar3 = line.find(' | ', bar2 + 5)
    fields = [
        h
        for h in line[:bar1].split()
        if h.startswith(('samples=', 'topo=', 'ds=')) or (len(h) > 1 and h[0] in 'ca' and h[1].isdigit())
    ]
    key = (' '.join(fields) + ' ' + line[bar1 : bar3 if bar3 >= 0 else len(line)]).strip()
    segments = line[bar3 + 3 :].split(' | ') if bar3 >= 0 else []
    variant = ''.join(' | ' + ' '.join(t.split()) for t in segments if t.startswith(VARIANT_SEGMENTS))
    return key + (f' | l {framework}' if framework else '') + variant


# raw_pack.h by version: PackPipeline and PackProgram sizes, sections, and the pipeline's head (key hash, key string,
# vs, fs, blob, blob dwords, ..., flags). From 15 a program's register count is its own; from 16 program indices are
# 32-bit.
LAYOUTS = {
    14: (480, 32, 10, struct.Struct('<QIHHIIBBBBHHI')),
    15: (536, 36, 12, struct.Struct('<QIHHIIBBBBHHI')),
    16: (540, 36, 12, struct.Struct('<QIIIIIBBBBHHI')),
    17: (540, 36, 13, struct.Struct('<QIIIIIBBBBHHI')),
    18: (544, 36, 13, struct.Struct('<QIIIIIBBBBHHI')),
    19: (544, 44, 13, struct.Struct('<QIIIIIBBBBHHI')),
    20: (636, 44, 13, struct.Struct('<QIIIIIBBBBHHI')),
    21: (660, 44, 13, struct.Struct('<QIIIIIBBBBHHI')),
    22: (696, 44, 13, struct.Struct('<QIIIIIBBBBHHI')),
    23: (696, 44, 14, struct.Struct('<QIIIIIBBBBHHI')),
}
# PackProgram's constant buffer slots by its size: the count at byte 20, the slots' types from 21, then the registers
# and the bound textures' slot + 1
PROGRAM_CBUF_SLOTS = {32: 11, 36: 11, 44: 18}


def program_fields(p: bytes, at: int, program_bytes: int) -> tuple[int, bytes, int, int]:
    """Give a PackProgram's constant buffer count, its slots' types, its registers and its bound textures' slot + 1."""
    slots = PROGRAM_CBUF_SLOTS[program_bytes]
    return p[at + 20], p[at + 21 : at + 21 + slots], p[at + 21 + slots], p[at + 22 + slots]


VARIANT_RECORD = 16  # PackPipelineFlags PipelineVariantRecord: a variant's record under its state's key
FETCH_FREE_FORM = 32  # PipelineFetchFreeForm: the state drawn with no framebuffer fetch, under its state's key


class Pack:
    def __init__(self, path: Path):
        self.p = path.read_bytes()
        if self.p[:8] != b'GM20BPK1':
            raise SystemExit(f'{path}: not a raw pack')
        self.version, self.nprog, self.npipe, _ = struct.unpack_from('<4I', self.p, 8)
        if self.version not in LAYOUTS:
            raise SystemExit(f'{path}: a version-{self.version} pack')
        pipeline_bytes, self.program_bytes, sections, head = LAYOUTS[self.version]
        self.sec = [struct.unpack_from('<QQ', self.p, 24 + 16 * i) for i in range(sections)]
        self.pipelines, self.free = {}, {}
        for i in range(self.npipe):
            at = self.sec[1][0] + pipeline_bytes * i
            key, _, _, fs, blob, dwords, *_, flags = head.unpack_from(self.p, at)
            if flags & VARIANT_RECORD:
                continue
            (self.free if flags & FETCH_FREE_FORM else self.pipelines).setdefault(key, (fs, blob, dwords))

    def code(self, program: int) -> bytes:
        at = self.sec[0][0] + self.program_bytes * program
        heap_offset, _, size, data_offset, _ = struct.unpack_from('<5I', self.p, at)
        start = self.sec[7][0] + heap_offset + isa.HEADER_BYTES + 0x30
        return self.p[start : self.sec[7][0] + heap_offset + (data_offset or size)]

    def gprs(self, program: int) -> int:
        return program_fields(self.p, self.sec[0][0] + self.program_bytes * program, self.program_bytes)[2]

    def blob(self, at: int, dwords: int) -> list[int]:
        return list(struct.unpack_from(f'<{dwords}I', self.p, self.sec[5][0] + 4 * at))

    def state(self, key: str) -> tuple[bytes, int] | None:
        found = self.pipelines.get(fnv(key.encode()))
        if not found:
            return None
        fs, blob, dwords = found
        gprs = self.gprs(fs) if self.version >= 15 else register_count(self.blob(blob, dwords))
        return self.code(fs), gprs


def register_count(blob: list[int]) -> int:
    pairs = dict(zip(blob[0::2], blob[1::2], strict=True))
    return pairs.get(FRAGMENT_REGISTER_COUNT, 0)


def fragment_variant(d: dict, n: int) -> dict | None:
    """Give a dumped record's fragment variant `n` (its stage's order), None where it has fewer."""
    found = [v for v in d.get('variants', []) if v['stage'] == 1]
    return found[n] if n < len(found) else None


class Dump:
    def __init__(self, path: Path, variant: int | None = None):
        self.states = {}
        for f in sorted(path.glob('*.json')):
            d = json.loads(f.read_text())
            code, gprs = d['fs'], d['fs_gprs'] if 'fs_gprs' in d else register_count(d['blob'])
            if variant is not None:
                v = fragment_variant(d, variant)
                if v is None:
                    continue
                code, gprs = v['code'], v['gprs']
            self.states[d['key']] = (bytes.fromhex(code)[isa.HEADER_BYTES :], gprs)

    def state(self, key: str) -> tuple[bytes, int] | None:
        return self.states.get(key)


def labelled(corpora: list[Path]) -> list[tuple[str, str]]:
    out = []
    for path in corpora:
        for line in path.read_text().splitlines():
            if line.startswith('R ') and (' lib=' not in line or FRAMEWORK_LIBRARY.search(line)):
                label = line.split('label=')[1].split()[0].removeprefix('com.apple.coreanimation.')
                out.append((label, state_key(line)))
    return out


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument('--pack', type=Path)
    src.add_argument('--dump', type=Path)
    ap.add_argument('--corpus', type=Path, action='append', default=[])
    ap.add_argument('--variant', type=int, help="a dump's fragment variant N instead of the record's generic program")
    ap.add_argument('--json', action='store_true')
    a = ap.parse_args()
    source = Pack(a.pack) if a.pack else Dump(a.dump, a.variant)
    rows, missing = [], []
    for label, key in labelled(a.corpus):
        state = source.state(key)
        if state is None:
            missing.append(label)
            continue
        rows.append(row(label, *state))
    real = [r for r in rows if r['instructions'] > STUB_INSTRUCTIONS]
    summary = {
        'states': len(rows),
        'stubs': len(rows) - len(real),
        'missing': len(missing),
        **{k: sum(r[k] for r in real) for k in ('instructions', *COST_COLUMNS)},
        **{k: sum(r[k] for r in real) for k in ORDER},
    }
    summary['cycles'] = round(summary['cycles'], 2)
    summary['cycles_overlap'] = round(summary['cycles_overlap'], 2)
    if a.json:
        print(json.dumps({'rows': rows, 'summary': summary, 'missing': missing}, indent=1))
        return
    head = (
        f'{"state":42} {"instr":>6} {"gpr":>4} {"wrp":>4} {"ms":>7} {"cyc":>8} {"ms_c":>7} {"cyc_o":>8} {"ms_o":>7} '
        f'{"unm":>5} {"bank":>5} '
        f'{"bankr":>5} {"bytes":>7} {"stall":>7} ' + ' '.join(f'{k:>5}' for k in ORDER)
    )
    print(head)
    for r in rows:
        print(
            f'{r["label"][:42]:42} {r["instructions"]:6} {r["gprs"]:4} {r["warps"]:4} {r["ms_1280x720"]:7.3f} '
            f'{r["cycles"]:8.1f} {r["ms_cycles"]:7.3f} {r["cycles_overlap"]:8.1f} {r["ms_overlap"]:7.3f} '
            f'{r["unmeasured"]:5} {r["bank"]:5} {r["bank_reads"]:5} '
            f'{r["bytes"]:7} {r["stalls"]:7} ' + ' '.join(f'{r[k]:5}' for k in ORDER)
        )
    print(
        f'corpus: {summary["states"]} states, {summary["stubs"]} stubs left out, {summary["missing"]} not found; '
        f'{summary["instructions"]} instructions, {summary["cycles"]} cycles / {summary["cycles_overlap"]} overlapped '
        f'({summary["unmeasured"]} untimed), '
        f'{summary["bank"]} same-bank reads ({summary["bank_reads"]} counting repeats), {summary["bytes"]} bytes, '
        f'{summary["stalls"]} stall cycles: ' + ', '.join(f'{k} {summary[k]}' for k in ORDER)
    )


if __name__ == '__main__':
    main()
