"""The fake device's GPU, a made-up SM53.

Programs and their time come from sm53/timing_sim.py's cycle model with LATENCIES below, each pixel's colour from
the host interpreter, the iterated blend unit from fakeblend.py, and the store rounds to nearest. The loop's tests
check the analyses recover exactly this GPU.
"""

import random
import struct
from dataclasses import dataclass, field

from tools.benchloop import fakeblend, known, protocol, texels
from tools.benchloop.sm53 import encode, program, timing_sim
from tools.benchloop.sm53.program import inputs
from tools.gpu.maxwell import execute

CLOCK_HZ = 768e6
UNWRITTEN = 0xA5
OVERHEAD_NS = 14_000.0
LATENCIES = {
    'FADD': 6, 'FMUL': 6, 'FFMA': 6, 'FMNMX': 6, 'FSETP': 7, 'HADD2': 7, 'HMUL2': 7, 'HFMA2': 7, 'IADD': 6,
    'IADD32I': 6, 'LOP': 6, 'LOP32I': 6, 'SHL': 6, 'SHR': 6, 'IMNMX': 6, 'BFE': 6, 'SEL': 6, 'PRMT': 6, 'ISETP': 7,
    'MOV': 6, 'MOV32I': 6, 'MUFU': 22, 'RRO': 12, 'F2F': 14, 'F2I': 14, 'I2F': 14, 'IMUL': 13, 'FLO': 13, 'IPA': 12,
}  # fmt: skip
TABLE = timing_sim.Table(
    latency=LATENCIES,
    issue={'MUFU': 4, 'F2F': 4, 'F2I': 4, 'I2F': 4, 'IMUL': 4, 'HFMA2': 2},
    dual=frozenset({('FFMA', 'MUFU'), ('FADD', 'I2F')}),
    bank_penalty=1,
)


@dataclass
class Outcome:
    status: str = 'ok'
    why: str = ''
    runs: list[tuple[int, list[int], list[int]]] = field(default_factory=list)
    texels: bytes | None = None
    milliseconds: float = 0.0
    output: bytes | None = None


def _output(c: dict) -> bytes:
    """Give a kernel's output buffer: each thread's stores (program.store_epilogue's), the rest never written.

    The interpreter runs one lane without memory, so the stores are taken out and their registers read at EXIT; the
    stores of every thread are the same words at its own stride.
    """
    instrs = encode.unbundle(protocol.unb64(c['code']))
    stores = [(i.word & 0xFF, i.word >> 20 & 0xFFFFFF) for i in instrs if i.name == 'STG']
    code = encode.bundle([encode.nop().sched(**vars(i.control)) if i.name == 'STG' else i for i in instrs])
    regs, _ = execute.run(code, inputs(protocol.unb64(c.get('data', ''))), 256)
    out = bytearray([UNWRITTEN]) * c['output']
    threads = c['threads'][0] * c['threads'][1] * c['threads'][2] * c['groups'][0] * c['groups'][1] * c['groups'][2]
    for t in range(threads):
        for reg, offset in stores:
            at = t * program.OUTPUT_STRIDE + offset
            if at + 4 <= len(out):
                out[at : at + 4] = struct.pack('<I', regs[reg])
    return bytes(out)


def _iterated(e: dict) -> tuple[list[int], int, bool] | None:
    words, passes, enable = [], 1, 0
    for method, value in e.get('methods', []):
        if method == 0x0DF4:
            words.append(value)
        elif method == 0x0DD4:
            passes = value
        elif method == 0x0DD0:
            enable = value
    return (words, passes, bool(enable & 2)) if enable & 1 else None


class Gpu:
    def __init__(self, table: timing_sim.Table = TABLE, noise: float = 0.01, seed: int = 7) -> None:
        self.table, self.noise, self.random = table, noise, random.Random(seed)

    def run(self, e: dict) -> Outcome:
        program = e.get('program')
        if program and not protocol.reads_position_w(protocol.unb64(program.get('sph', ''))):
            return Outcome('killed', 'the kernel killed the channel')
        try:
            per_draw, texel_of = getattr(self, '_' + e['kind'])(e)
        except (ValueError, KeyError, execute.UnsupportedError) as error:
            return Outcome('refused', f'fake GPU: {error}')
        out = Outcome(runs=self._times(e, per_draw))
        if e.get('readback', 'summary') != 'none' and texel_of:
            out.texels = self._texels(e, texel_of)
        if e.get('readback', 'summary') != 'none' and e['kind'] == 'compute' and 'output' in e['compute']:
            out.output = _output(e['compute'])
        return out

    def _times(self, e: dict, per_draw: float) -> list[tuple[int, list[int], list[int]]]:
        runs = []
        for draws in e.get('draws', [1]):
            gpu = [
                round((OVERHEAD_NS + draws * per_draw) * self.random.gauss(1, self.noise))
                for _ in range(e.get('repeats', 1))
            ]
            runs.append((draws, gpu, [g + 40_000 for g in gpu]))
        return runs

    def _texels(self, e: dict, texel_of: object) -> bytes:
        t = e['target']
        fmt, size = t['format'], protocol.FORMATS[t['format']][1]
        base = (
            protocol.unb64(e['fill'])
            if 'fill' in e
            else texels.encode(tuple(e.get('clear', [0, 0, 0, 0])), fmt) * (t['width'] * t['height'])
        )
        out = bytearray(base)
        x0, y0, x1, y1 = protocol.rect_of(e, protocol.target_of(e))
        for y in range(y0, y1):
            for x in range(x0, x1):
                at = (y * t['width'] + x) * size
                out[at : at + size] = texel_of(x, y, bytes(out[at : at + size]))
        return bytes(out)

    @staticmethod
    def _words(words: list[int], fmt: str) -> bytes:
        raw = struct.pack('<4I', *words)
        return raw if fmt == 'rgba32float' else texels.encode(struct.unpack('<4f', raw), fmt)

    def _noop(self, _e: dict) -> tuple[float, object]:
        return 0.0, None

    def _known(self, e: dict) -> tuple[float, object]:
        texel = self._words(list(known.KNOWN_DATA), e['target']['format'])
        return 300.0, lambda *_: texel

    def _fragment(self, e: dict) -> tuple[float, object]:
        p = e['program']
        code, data = protocol.unb64(p['code']), protocol.unb64(p.get('data', ''))
        regs, cycles = timing_sim.run(code, data, self.table)
        x0, y0, x1, y1 = protocol.rect_of(e, protocol.target_of(e))
        per_draw = timing_sim.draw_ns(
            cycles, (x1 - x0) * (y1 - y0), CLOCK_HZ, timing_sim.issue_cycles(code, self.table, data)
        )
        texel = self._words(regs, e['target']['format'])
        return per_draw, lambda *_: texel

    def _compute(self, e: dict) -> tuple[float, object]:
        c = e['compute']
        if 'output' in c:
            return 1000.0, None
        code = protocol.unb64(c['code'])
        _, cycles = timing_sim.run(code, protocol.unb64(c.get('data', '')), self.table, outputs=0)
        lanes = c['threads'][0] * c['threads'][1] * c['threads'][2] * c['groups'][0] * c['groups'][1] * c['groups'][2]
        return timing_sim.draw_ns(cycles, lanes, CLOCK_HZ, timing_sim.issue_cycles(code, self.table)), None

    def _twod(self, e: dict) -> tuple[float, object]:
        """Give a fill its colour where the host knows the format's packing; leave the target otherwise."""
        from tools.benchloop import twod  # noqa: PLC0415  twod builds programs with the interpreter's encoder

        want = twod.wanted_fill(e) if e['twod']['op'] == 'fill' else None
        return 2000.0, lambda _x, _y, under: want if want is not None else under

    def _readback(self, e: dict) -> tuple[float, object]:
        p, fmt = e['program'], e['target']['format']
        code, data = protocol.unb64(p['code']), protocol.unb64(p.get('data', ''))
        blend = _iterated(e)

        def texel_of(x: int, y: int, under: bytes) -> bytes:
            regs, _ = execute.run(code, inputs(data, (x, y)), 4)
            src = struct.unpack('<4f', struct.pack('<4I', *regs))
            if blend:
                src = fakeblend.apply(blend[0], blend[1], blend[2], (src, texels.decode(under, fmt)))
            return texels.encode(src, fmt)

        return 0.0, texel_of
