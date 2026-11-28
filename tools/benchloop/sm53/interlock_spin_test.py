import unittest

from tools.benchloop import protocol, ticket
from tools.benchloop.sm53 import encode, interlock, interlock_spin, latency, memops, program, timing_sim


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


class InterlockClockProbeTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.experiments = interlock_spin.generate_clock_probe()

    def test_probe_batch_validates_and_orders_safe_before_cs2r_bar(self):
        batch = protocol.batch('interlock-clock', self.experiments)
        protocol.encode_batch(batch)
        ids = [x['id'] for x in self.experiments]
        self.assertEqual(len(ids), len(set(ids)))
        first_cs2r_bar = ids.index('interlock_clock.safety.cs2r_bar')
        for eid in (
            'interlock_clock.safety.s2r_bar',
            'interlock_clock.safety.cs2r_delay',
            'interlock_clock.spin.ldcg.256x256.timed',
            'interlock_clock.spin.ldcg_s2r.256x256.timed',
        ):
            self.assertLess(ids.index(eid), first_cs2r_bar, eid)
        self.assertGreater(ids.index('interlock_clock.spin.ldcg_cs2r.64x64.check'), first_cs2r_bar)

    def test_clock_encodings_barriers_and_backoff_control_flow(self):
        self.assertEqual(memops.cs2r(8, memops.SR_CLOCKLO).word >> 48, 0x50C8)
        self.assertEqual(memops.cs2r(8, memops.SR_CLOCKLO).word >> 20 & 0xFF, 0x50)
        self.assertEqual(memops.s2r(8, memops.SR_CLOCKLO).word >> 48, 0xF0C8)
        self.assertEqual(memops.s2r(8, memops.SR_CLOCKLO).word >> 20 & 0xFF, 0x50)

        for x in self.experiments[1:]:
            code = protocol.unb64(x['program']['code'])
            instrs = encode.unbundle(code)
            self.assertEqual(encode.barrier_hazards(instrs), [], x['id'])
            if x['meta'].get('probe') == 'spin':
                header = protocol.unb64(x['program']['sph'])
                self.assertTrue(ticket.reads_ticket(code), x['id'])
                self.assertTrue(ticket.header_says_so(header), x['id'])

        for arm, op_name, wait_bar in (
            ('s2r_bar', 'S2R', True),
            ('cs2r_delay', 'CS2R', False),
            ('cs2r_bar', 'CS2R', True),
        ):
            x = next(e for e in self.experiments if e['id'] == f'interlock_clock.safety.{arm}')
            instrs = encode.unbundle(protocol.unb64(x['program']['code']))
            reads = [(k, i) for k, i in enumerate(instrs) if i.name == op_name and (i.word >> 20 & 0xFF) == 0x50]
            self.assertEqual(len(reads), 2, arm)
            for k, i in reads:
                if wait_bar:
                    self.assertEqual(i.control.wr_bar, 0, arm)
                    self.assertEqual(instrs[k + 1].name, 'NOP', arm)
                    self.assertEqual(instrs[k + 1].control.wait, 1, arm)
                else:
                    self.assertEqual(i.control.wr_bar, encode.NO_BAR, arm)

        for form, clock_name in (('ldcg_s2r', 'S2R'), ('ldcg_cs2r', 'CS2R')):
            x = next(e for e in self.experiments if e['id'] == f'interlock_clock.spin.{form}.64x64.check')
            instrs = encode.unbundle(protocol.unb64(x['program']['code']))
            ldg_idx = next(k for k, i in enumerate(instrs) if i.name == 'LDG' and (i.word >> 8 & 0xFF) == 18)
            self.assertEqual(instrs[ldg_idx].word >> 46 & 3, 1)
            bras = [(k, timing_sim.branch_target(k, i.word)) for k, i in enumerate(instrs) if i.name == 'BRA']
            self.assertEqual(len(bras), 3, form)
            fast_idx, fast_target = bras[0]
            inner_idx, inner_target = bras[1]
            outer_idx, outer_target = bras[2]
            self.assertEqual(outer_target, ldg_idx)
            self.assertEqual(instrs[fast_target].name, 'LDG')
            self.assertEqual(instrs[inner_target].name, clock_name)
            clock_reads = [
                k for k, i in enumerate(instrs) if i.name == clock_name and (i.word >> 20 & 0xFF) == memops.SR_CLOCKLO
            ]
            self.assertEqual(len(clock_reads), 2, form)
            self.assertTrue(all(fast_idx < k < outer_idx for k in clock_reads))
            self.assertEqual(clock_reads[1], inner_target)
            self.assertLess(inner_target, inner_idx)
            for k in clock_reads:
                self.assertEqual(instrs[k].control.wr_bar, 0)
                self.assertEqual(instrs[k + 1].name, 'NOP')
                self.assertEqual(instrs[k + 1].control.wait, 1)

    def test_clock_probe_analysis_and_wiring(self):
        s2r_texel = program.texel_hex(program.pack(1000, 18))
        cs2r_texel = program.texel_hex(program.pack(2000, 6))
        results: dict[str, dict] = {
            'interlock_clock.safety.s2r_bar': {
                'status': 'ok',
                'texels': {'distinct': [{'value': s2r_texel, 'count': 32}]},
            },
            'interlock_clock.safety.cs2r_delay': {
                'status': 'ok',
                'texels': {'distinct': [{'value': cs2r_texel, 'count': 32}]},
            },
            'interlock_clock.safety.cs2r_bar': {'status': 'killed', 'why': 'channel hang'},
        }
        for n in interlock_spin.CLOCK_CHAIN_BODIES:
            k = n * interlock_spin.CLOCK_CHAIN_LOOPS
            results[f'interlock_clock.latency.s2r_bar.k{k:04d}'] = {
                'status': 'ok',
                'runs': [{'draws': d, 'gpu_ns': [1000 + d * (500 + 20 * k)]} for d in latency.DRAWS],
            }
            results[f'interlock_clock.latency.cs2r_bar.k{k:04d}'] = {'status': 'killed', 'why': 'channel hang'}
        spin_x = next(e for e in self.experiments if e['id'] == 'interlock_clock.spin.ldcg_s2r.64x64.timed')
        expect = spin_x['meta']['expect']
        results[spin_x['id']] = {
            'status': 'ok',
            'storage': {'distinct': [{'value': f'{expect:08x}', 'count': 64 * 64}]},
            'runs': [{'draws': d, 'gpu_ns': [1000 + 50 * d]} for d in interlock_spin.TIMED_DRAWS],
        }

        found = interlock_spin.analyse_clock_probe(self.experiments, results, clock_hz=1e9)
        self.assertTrue(found['safety']['s2r_bar']['monotonic'])
        self.assertEqual(found['safety']['s2r_bar']['dt'], 18)
        self.assertTrue(found['safety']['cs2r_delay']['monotonic'])
        self.assertFalse(found['safety']['cs2r_bar']['monotonic'])
        self.assertEqual(found['safety']['cs2r_bar']['status'], 'killed')
        self.assertAlmostEqual(found['latency']['s2r_bar']['cycles_per_op'], 20.0)
        self.assertIsNone(found['latency']['cs2r_bar']['cycles_per_op'])
        self.assertEqual(found['spin'][spin_x['id']]['lost'], 0)
        self.assertAlmostEqual(found['spin'][spin_x['id']]['ns_per_draw'], 50.0)


if __name__ == '__main__':
    unittest.main()
