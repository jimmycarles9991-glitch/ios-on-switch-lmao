import unittest

from tools.benchloop import protocol
from tools.benchloop.sm53 import encode, interlock, interlock_spin, memops


def names(x: dict) -> list[str]:
    return [i.name for i in encode.unbundle(protocol.unb64(x['program']['code']))]


class InterlockSpinTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.experiments = interlock_spin.generate()

    def test_one_batch_with_both_channel_settings(self):
        protocol.batch('interlock-spin', self.experiments)
        competing = [x for x in self.experiments[1:] if x.get('compete')]
        self.assertEqual(len(competing), (len(self.experiments) - 1) // 2)
        self.assertTrue(all(x['id'].endswith('-compete') for x in competing))

    def test_forms_differ_only_in_the_spin_read(self):
        by_form = {x['meta']['form']: names(x) for x in self.experiments[1:5]}
        self.assertEqual(by_form['atom'].count('ATOM'), 2)
        self.assertEqual(by_form['ldcg'].count('LDG'), 3)
        swapped = [n if n != 'ATOM' else 'LDG' for n in by_form['atom']]
        self.assertEqual(swapped, by_form['ldcg'])
        self.assertEqual(swapped, by_form['ldcv'])
        self.assertNotIn('RED', by_form['none'])
        self.assertEqual((by_form['none'].count('S2R'), by_form['atom'].count('S2R')), (1, 4))

    def test_load_spins_differ_in_the_cache_op(self):
        ops = {}
        for x in self.experiments[1:5]:
            spins = [i.word for i in encode.unbundle(protocol.unb64(x['program']['code'])) if i.name == 'LDG']
            ops[x['meta']['form']] = sorted({w >> 46 & 3 for w in spins[:1] + spins[2:]})
        self.assertEqual((ops['ldcg'], ops['ldcv']), ([1], [3]))

    def test_spins_branch_back_to_their_read(self):
        x = self.experiments[1]
        instrs = encode.unbundle(protocol.unb64(x['program']['code']))
        for k, i in enumerate(instrs):
            if i.name != 'BRA':
                continue
            offset = i.word >> 20 & 0xFFFFFF
            offset -= (offset >> 23) << 24
            target = encode.address(k) + 8 + offset
            self.assertEqual(instrs[[encode.address(j) for j in range(len(instrs))].index(target)].name, 'ATOM')
            self.assertEqual((i.word >> 16 & 0xF), interlock.ANY | 8)

    def test_no_barrier_hazards_and_leader_guards(self):
        for x in self.experiments[1:]:
            instrs = encode.unbundle(protocol.unb64(x['program']['code']))
            self.assertEqual(encode.barrier_hazards(instrs), [])
            for i in instrs:
                if i.name in ('ATOM', 'RED'):
                    self.assertEqual(i.word >> 16 & 0xF, interlock.LEADER)
                if i.name == 'STG':
                    self.assertEqual(i.word >> 16 & 0xF, interlock.ALIVE)

    def test_ticket_address_reads_the_root_tables_buffer(self):
        instrs = encode.unbundle(protocol.unb64(self.experiments[1]['program']['code']))
        cbufs = [(i.word >> 34 & 0x1F, (i.word >> 20 & 0x3FFF) * 4) for i in instrs if i.name == 'IADD_cbuf']
        self.assertEqual(cbufs, [(1, 0), (1, 4), (0, 0x260), (0, 0x264)])
        words = {i.word & ~0xFF for i in instrs if i.name == 'S2R'}
        self.assertIn(memops.s2r(0, memops.SR_ORDERING_TICKET).word, words)

    def test_counts_expected(self):
        check = next(x for x in self.experiments if x['id'] == 'interlock.atom.1280x720.check')
        timed = next(x for x in self.experiments if x['id'] == 'interlock.atom.1280x720.timed')
        self.assertEqual((check['meta']['expect'], timed['meta']['expect']), (64, 448))
        self.assertEqual(check['storage']['bytes'], 720 * 2048 * 4)

    def test_analysis(self):
        x = next(x for x in self.experiments if x['id'] == 'interlock.ldcg.1280x720.timed')
        gaps = (2048 - 1280) * 720
        storage = {'distinct': [{'value': f'{448:08x}', 'count': 1280 * 720 - 3}, {'value': '000001bf', 'count': 3}]}
        storage['distinct'].append({'value': '00000000', 'count': gaps})
        runs = [{'draws': d, 'gpu_ns': [1000 + 10 * d]} for d in interlock_spin.TIMED_DRAWS]
        found = interlock_spin.analyse([x], {x['id']: {'status': 'ok', 'storage': storage, 'runs': runs}})[x['id']]
        self.assertEqual((found['lost'], found['other_values'], found['zeros_past_gaps']), (3, {0x1BF: 3}, 0))
        self.assertAlmostEqual(found['ns_per_draw'], 10.0)


if __name__ == '__main__':
    unittest.main()
