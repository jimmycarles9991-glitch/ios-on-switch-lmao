"""timing_sim's stale reads, barriers, dual issue and bank conflicts on hand-built programs."""

import unittest

from tools.benchloop.sm53 import encode as e, program, timing_sim
from tools.benchloop.sm53.encode import f32

TABLE = timing_sim.Table(
    latency={'FADD': 5, 'MUFU.RCP': 20},
    issue={'MUFU': 4},
    dual=frozenset({('FFMA', 'IADD')}),
    bank_penalty=2,
)


def _run(body: list[e.Instr], init: dict[int, int]) -> tuple[int, int]:
    prog = program.fragment(body, init, (8, 8))
    regs, cycles = timing_sim.run(prog.code, prog.data, TABLE)
    return program.unpack(regs)[0], cycles


class TimingSimTest(unittest.TestCase):
    def test_a_read_before_the_latency_sees_the_old_value(self):
        init = {8: f32(0.0), 5: f32(1.0)}
        early, _ = _run([e.fadd(8, 8, 5).sched(delay=4) for _ in range(4)], init)
        on_time, _ = _run([e.fadd(8, 8, 5).sched(delay=5) for _ in range(4)], init)
        self.assertEqual(on_time, f32(4.0))
        self.assertEqual(early, f32(2.0))

    def test_cycles_follow_the_encoded_delays(self):
        init = {8: f32(0.0), 5: f32(1.0)}
        cycles = [_run([e.fadd(8, 8, 5).sched(delay=d) for _ in range(10)], init)[1] for d in (6, 7)]
        self.assertEqual(cycles[1] - cycles[0], 10)

    def test_a_looped_body_runs_once_per_iteration(self):
        init = {8: f32(0.0), 5: f32(1.0)}
        body = [e.fadd(8, 8, 5).sched(delay=6) for _ in range(4)]
        once = program.fragment(body, init, (8, 8))
        looped = program.fragment(body, init, (8, 8), loop=5)
        regs, _ = timing_sim.run(looped.code, looped.data, TABLE)
        self.assertEqual(program.unpack(program.expected_texel(looped.code, looped.data))[0], f32(20.0))
        self.assertEqual(program.unpack(regs)[0], f32(20.0))
        self.assertEqual(
            len(timing_sim.path(looped.code, looped.data)) - len(timing_sim.path(once.code)), 1 + 5 * (3 + 4) - 4
        )

    def test_a_barrier_wait_holds_the_consumer_until_the_result_lands(self):
        def chain(k: int) -> list[e.Instr]:
            return [
                e.mufu(8, 8, 'rcp').sched(wr_bar=j % 2, wait=1 << (1 - j % 2) if j else 0, delay=e.BARRIER_SET_CYCLES)
                for j in range(k)
            ]

        (x4, c4), (_, c8) = _run(chain(4), {8: f32(4.0)}), _run(chain(8), {8: f32(4.0)})
        self.assertEqual(c8 - c4, 4 * 20)
        self.assertEqual(x4, f32(4.0))
        unprotected, _ = _run([e.mufu(8, 8, 'rcp'), e.fadd(8, 8, 8)], {8: f32(4.0)})
        self.assertEqual(unprotected, f32(8.0))

    def test_delay_zero_pairs_only_allowed_pairs(self):
        init = {5: f32(1.0), 6: f32(2.0), 7: f32(3.0)}

        def cycles(second: e.Instr, delay: int) -> int:
            return _run([e.ffma(8, 5, 6, 7).sched(delay=delay), second] * 16, init)[1]

        self.assertEqual(cycles(e.iadd(12, 5, 6), 1) - cycles(e.iadd(12, 5, 6), 0), 16)
        self.assertEqual(cycles(e.fadd(12, 5, 6), 1), cycles(e.fadd(12, 5, 6), 0))

    def test_bank_conflicts_and_the_reuse_cache(self):
        init = {4: f32(1.0), 5: f32(1.0), 6: f32(1.0), 8: f32(0.0), 12: f32(0.0)}

        def issue(a: int, b: int, reuse: int) -> int:
            body = [e.ffma(12, a, b, 12).sched(reuse=reuse) for _ in range(8)]
            return timing_sim.issue_cycles(program.fragment(body, init, (12, 12)).code, TABLE)

        spread, conflict, cached = issue(5, 6, 0), issue(4, 8, 0), issue(4, 8, 0b011)
        self.assertEqual(conflict - spread, 8 * 2 * TABLE.bank_penalty)
        self.assertEqual(cached - spread, 2 * TABLE.bank_penalty)

    def test_draw_time_is_latency_or_issue_bound(self):
        self.assertEqual(timing_sim.draw_ns(1000, 32, 1e9, 100), 1000.0)
        self.assertEqual(timing_sim.draw_ns(1000, 32 * 1024, 1e9, 100), 128 * 100.0)


if __name__ == '__main__':
    unittest.main()
