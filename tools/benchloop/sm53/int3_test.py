import struct
import unittest

from tools.benchloop import protocol
from tools.benchloop.sm53 import int3, latency, operands, program, timing_sim
from tools.benchloop.sm53.ops import K1, K2, chain_registers
from tools.gpu.maxwell.execute import alu
from tools.gpu.maxwell.interp import Lane

SRC, DST = 20, 24


def link(r: int3.Recipe, value: int) -> int:
    lane = Lane()
    lane.w(K1, r.consts[0])
    lane.w(K2, r.consts[1])
    lane.w(SRC, value)
    i = r.link(DST, SRC)
    alu(i.name, i.word, lane, program.inputs(b''))
    return lane.r(DST)


def chain(r: int3.Recipe, stale: int | None = None) -> tuple[int, int]:
    """Give the stall chain's two results, link `stale` reading its register's poison instead of the last result."""
    v = [r.seed]
    for j in range(latency.STALL_K):
        v.append(link(r, r.poison + r.step * j if j == stale else v[j]))
    return v[-1], v[latency.STALL_K // 2]


class Int3Test(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.experiments = int3.generate()

    def test_one_valid_batch(self):
        protocol.batch('int3-xmad', self.experiments)
        self.assertEqual(self.experiments[0]['meta']['family'], 'warm')

    def test_a_single_stale_read_changes_every_chain(self):
        for r in int3.RECIPES:
            right = chain(r)
            for j in range(1, latency.STALL_K):
                self.assertNotEqual(chain(r, j), right, (r.key, j))

    def test_chains_read_wrong_one_cycle_short_of_the_latency(self):
        regs = chain_registers(latency.STALL_K + 1)
        for r in int3.RECIPES:
            key = operands.of(r.link(regs[1], regs[0])).key
            for lat in (6, 13):
                for delay in (lat - 1, lat):
                    prog = latency._stall_gprs(r, delay).fragment()
                    got, _ = timing_sim.run(prog.code, prog.data, timing_sim.Table({key: lat}))
                    self.assertEqual(got == program.expected_texel(prog.code, prog.data), delay >= lat, (r.key, delay))

    def test_interpreter_runs_the_chains_as_the_host_does(self):
        for r in int3.RECIPES:
            prog = latency._stall_gprs(r, 15).fragment()
            self.assertEqual(program.unpack(program.expected_texel(prog.code, prog.data)), chain(r), r.key)

    def test_products_through_the_interpreter(self):
        pairs = int3.mul_pairs()[:120]
        for name, (regs, want) in int3.MUL_PROGRAMS.items():
            code = int3.mul_code(regs)
            data = b''.join(struct.pack('<4I', x, y, 0, 0) for x, y in pairs)
            for col, (x, y) in enumerate(pairs):
                got = program.unpack(program.expected_texel(code, data, (col, 0)))
                self.assertEqual(got, want(x, y), (name, hex(x), hex(y)))

    def test_mul_analysis(self):
        mul = [x for x in self.experiments if x['meta']['family'] == 'xmad-mul']
        self.assertEqual(len(mul), 6)
        results = {}
        for x in mul:
            name = x['meta']['program']
            words = b''.join(
                struct.pack('<4I', *program.pack(*int3.MUL_PROGRAMS[name][1](a, b))) for a, b in int3.pairs_of(x)
            )
            results[x['id']] = {'status': 'ok', 'texels': {'data': protocol.b64(words)}}
        found = int3.analyse_mul(mul, results)
        self.assertEqual(
            {k: (v['checked'], v['wrong']) for k, v in found.items()}, dict.fromkeys(found, (6144, [0, 0]))
        )

    def test_wide_analysis(self):
        wide = [x for x in self.experiments if x['meta']['family'] == 'stall-wide']
        results = {}
        for x in wide:
            right = x['meta']['delay'] >= 6
            value = program.texel_hex(x['expect']['texel']) if right else '00' * 16
            results[x['id']] = {'status': 'ok', 'texels': {'distinct': [{'value': value, 'count': 32768}]}}
        found = int3.analyse_wide(wide, results)
        self.assertEqual(found['XMAD'], {'delays': list(int3.WIDE_DELAYS), 'pattern': '-+++++', 'right_from': 6})


if __name__ == '__main__':
    unittest.main()
