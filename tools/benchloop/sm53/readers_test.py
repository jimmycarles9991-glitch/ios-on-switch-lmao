import unittest

from tools.benchloop.sm53 import encode as e, program, readers


def texel(body: list[e.Instr], init: dict[int, int]) -> list[int]:
    p = program.fragment(body, init, (readers.SUM, readers.LAST))
    return program.expected_texel(p.code, p.data)


class ReadersTest(unittest.TestCase):
    def test_a_stale_read_in_any_link_changes_the_texel(self):
        for pair in readers.PAIRS_READ + readers.PAIRS_WRITERS:
            body, init = readers.reads(pair, 15)
            right = texel(body, init)
            for j in range(readers.LINKS):
                stale = list(body)
                stale[2 * j] = e.nop().sched(delay=15)
                self.assertNotEqual(texel(stale, init), right, (pair.key, j))

    def test_only_the_writer_s_delay_varies(self):
        for pair in readers.PAIRS_READ:
            short, _ = readers.reads(pair, 1)
            long, _ = readers.reads(pair, 15)
            differ = [i for i, (a, b) in enumerate(zip(short, long, strict=True)) if a != b]
            self.assertEqual(differ, [2 * j for j in range(readers.LINKS)], pair.key)

    def test_rro_alone_has_no_barrier(self):
        for pair in readers.PAIRS_READ:
            body, _ = readers.reads(pair, 2)
            writes = [i.control.wr_bar for i in body[0 : 2 * readers.LINKS : 2]]
            reads = [i.control.wr_bar for i in body[1 : 2 * readers.LINKS : 2]]
            self.assertEqual(set(writes), {e.NO_BAR}, pair.key)
            self.assertEqual(set(reads), {0} if pair.scoreboarded else {e.NO_BAR}, pair.key)

    def test_bit_4_set_only_where_asked(self):
        found = {x['id']: x for x in readers.generate_yield()}
        for eid, x in found.items():
            if x['meta']['family'] != 'reuse':
                continue
            code = program.protocol.unb64(x['program']['code'])
            yielding = sum(i.control.yld for i in e.unbundle(code))
            if '.y.' in eid:
                self.assertGreater(yielding, 0, eid)
            else:
                self.assertEqual(yielding, 0, eid)

    def test_no_result_is_no_answer_and_a_mixed_one_is_wrong(self):
        planned = readers.reader()
        found = readers.analyse(planned, {})
        for pair in readers.PAIRS_READ:
            self.assertEqual(found['reader'][pair.key]['pattern'], '?' * 15, pair.key)
        group = [x for x in planned if x['meta']['op'] == 'FFMA>F2F']
        right = program.texel_hex(group[0]['expect']['texel'])
        results = {
            x['id']: {'status': 'ok', 'texels': {'distinct': [{'value': right}] + ([{'value': '0'}] if d < 3 else [])}}
            for x in group
            if (d := x['meta']['delay']) != 9
        }
        found = readers.pattern(group, results, tuple(readers.DELAYS))
        self.assertEqual(found['pattern'], '--++++++?++++++')
        self.assertEqual(found['right_from'], 10)


if __name__ == '__main__':
    unittest.main()
