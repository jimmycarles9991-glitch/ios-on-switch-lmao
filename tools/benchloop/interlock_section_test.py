import copy
import json
import tempfile
import unittest
from pathlib import Path

from tools.benchloop import generators, interlock_section as section, protocol, report, ticket
from tools.benchloop.sm53 import encode, interlock, memops
from tools.gpu.maxwell import isa


def result_for(cell: dict, value: int | None = None, ns_per_draw: int = 5000) -> dict:
    expect = sum(cell['draws']) * (cell['repeats'] + cell['warmup'])
    value = expect if value is None else value
    return {
        'status': 'ok',
        'runs': [{'draws': d, 'gpu_ns': [1000 + d * ns_per_draw] * cell['repeats']} for d in cell['draws']],
        'storage': {'bytes': cell['storage']['bytes'], 'distinct': [{'value': f'{value:08x}', 'count': 4096}]},
    }


class InterlockSectionTest(unittest.TestCase):
    def test_matrix_round_trips_through_benchloop6(self) -> None:
        cells = section.generate()
        batch = json.loads(protocol.encode_batch(protocol.batch('section', cells)))
        self.assertEqual(batch['protocol'], 'benchloop/6')
        self.assertEqual(len(cells), 60)
        self.assertEqual(len({c['id'] for c in cells}), 60)
        for cell in batch['experiments']:
            protocol.validate_experiment(cell)
            self.assertNotIn('meta', cell)
            self.assertLessEqual(protocol.estimated_ms(cell), protocol.MAX_COMMAND_BUFFER_MS)
        self.assertEqual({c['meta']['body'] for c in cells}, {64, 256, 1024})
        self.assertEqual({(c['target']['width'], c['target']['height']) for c in cells}, set(section.SIZES))
        for size in section.SIZES:
            for body in section.BODY_SIZES:
                group = [
                    c
                    for c in cells
                    if (c['target']['width'], c['target']['height']) == size and c['meta']['body'] == body
                ]
                self.assertEqual(
                    {c['meta']['form'] for c in group}, set(section.FORMS) | ({'load_spin'} if body == 64 else set())
                )
                self.assertEqual({c['meta']['timed'] for c in group}, {False, True})
                for cell in group:
                    self.assertEqual(cell['meta']['correctness'], size == (64, 64))
                    self.assertEqual(
                        cell['draws'],
                        ([1, 2, 4] if size[0] == 1280 else [16, 32, 64])
                        if cell['meta']['timed']
                        else ([4] if size[0] == 1280 else [64]),
                    )

    def test_every_ticket_reader_sets_bit_610_and_host_rejects_its_removal(self) -> None:
        for cell in section.generate():
            code = protocol.unb64(cell['program']['code'])
            header = protocol.unb64(cell['program']['sph'])
            reads = cell['meta']['form'] != 'none'
            self.assertEqual(ticket.reads_ticket(code), reads)
            self.assertEqual(ticket.header_says_so(header), reads)
            self.assertEqual(cell['methods'], [[0x1224, 2 if reads else 0]])
            self.assertEqual(cell['restore'], [[0x1224, 0]])
            if reads:
                changed = copy.deepcopy(cell)
                changed['program']['sph'] = protocol.b64(
                    (int.from_bytes(header, 'little') & ~(1 << 610)).to_bytes(80, 'little')
                )
                with self.assertRaises(protocol.InvalidError):
                    protocol.validate_experiment(changed)

    def test_body_moves_across_the_wait_without_changing_the_counter_or_release(self) -> None:
        for body in section.BODY_SIZES:
            forms = {form: isa.instructions(interlock.code(form, 6, body)) for form in (*section.FORMS, 'load_spin')}
            for form, words in forms.items():
                names = [isa.name(w) for w in words]
                fma = [i for i, name in enumerate(names) if name == 'FFMA_reg']
                self.assertEqual(len(fma), body)
                self.assertEqual(fma, list(range(fma[0], fma[0] + body)))
                for i in fma:
                    self.assertEqual(words[i], encode.ffma(24, 24, 25, 26).word)
                stores = [i for i, name in enumerate(names) if name == 'STG']
                self.assertEqual(len(stores), 1)
                self.assertLess(fma[-1], stores[0])
                if form == 'none':
                    self.assertNotIn('ATOM', names)
                    self.assertNotIn('RED', names)
                    continue
                waits = [i for i, w in enumerate(words) if isa.name(w) in ('ATOM', 'LDG') and isa.field(w, 8, 8) == 18]
                self.assertEqual(len(waits), 2)
                self.assertLess(waits[0], stores[0])
                self.assertLess(stores[0], waits[1])
                self.assertEqual(names.count('RED'), 1)
                self.assertLess(waits[1], names.index('RED'))
                self.assertLess(names.index('MEMBAR'), names.index('RED'))
                if form == 'tail':
                    self.assertLess(fma[-1], waits[0])
                else:
                    self.assertLess(waits[0], fma[0])
                for at in waits:
                    branch = words[at + 4]
                    self.assertEqual(isa.name(branch), 'BRA')
                    delta = isa.field(branch, 20, 24)
                    delta -= 1 << 24 if delta & (1 << 23) else 0
                    self.assertEqual(encode.address(at + 4) + 8 + delta, encode.address(at))
                    self.assertEqual(isa.field(branch, 16, 4), 3 | 8)

            def counter(words: list[int]) -> list[int]:
                return [w for w in words if isa.name(w) in ('LDG', 'STG') and isa.field(w, 8, 8) == 8]

            self.assertEqual(counter(forms['whole']), counter(forms['tail']))
            self.assertEqual(counter(forms['whole']), counter(forms['none']))

    def test_wait_and_release_encodings_match_the_shipped_instructions(self) -> None:
        self.assertEqual(memops.guarded(memops.atom_add(20, 18, 21), 1).word, 0xED01000001511214)
        self.assertEqual(memops.guarded(memops.red_add(18, 16), 1).word, 0xEBF9000000011210)
        self.assertEqual(memops.guarded(memops.ldg(20, 18, 'cg'), 1).word, 0xEED4600000011214)
        for cell in section.generate():
            words = isa.instructions(protocol.unb64(cell['program']['code']))
            form = cell['meta']['form']
            self.assertEqual(sum(isa.name(w) == 'ATOM' for w in words), 2 if form in ('whole', 'tail') else 0)
            for word in words:
                if isa.name(word) == 'LDG':
                    self.assertEqual(isa.field(word, 46, 2), 1)

    def test_bad_forms_body_sizes_and_odd_address_pairs_are_refused(self) -> None:
        for form, body in [('unknown', 64), ('whole', 0), ('whole', 65)]:
            with self.assertRaises(ValueError):
                interlock.code(form, 6, body)
        for make in (
            lambda: memops.ldg(0, 1),
            lambda: memops.stg(1, 0),
            lambda: memops.atom_add(0, 1, 2),
            lambda: memops.red_add(1, 2),
        ):
            with self.assertRaises(ValueError):
                make()

    def test_only_64_square_requires_exact_counts(self) -> None:
        for size in section.SIZES:
            cell = section.experiment('whole', 64, *size, False)
            result = result_for(cell, 63)
            found = section.analyse([cell], {cell['id']: result})
            row = found['cells'][cell['id']]
            self.assertEqual(found['passed'], size != (64, 64))
            if size == (64, 64):
                self.assertEqual(row['lost_updates'], 4096)
                self.assertEqual(row['wrong_pixels'], 4096)
            else:
                self.assertEqual(row['verdict'], 'timing only')
                self.assertNotIn('lost_updates', row)

    def test_none_must_show_lost_updates_and_never_excess_updates(self) -> None:
        cell = section.experiment('none', 64, 64, 64, False)
        for value, passed in ((63, True), (64, False), (65, False)):
            self.assertEqual(section.analyse([cell], {cell['id']: result_for(cell, value)})['passed'], passed)

    def test_exact_counts_and_slope_gaps_survive_wire_metadata_removal(self) -> None:
        cells = section.generate()
        results = {
            cell['id']: result_for(
                cell, 1 if cell['meta']['form'] == 'none' else None, 5000 if cell['meta']['form'] == 'whole' else 4000
            )
            for cell in cells
        }
        wire = json.loads(protocol.encode_batch(protocol.batch('section', cells)))['experiments']
        found = section.analyse(wire, results)
        self.assertTrue(found['passed'])
        self.assertEqual(len(found['whole_minus_tail_ns']), 9)
        for gap in found['whole_minus_tail_ns'].values():
            self.assertAlmostEqual(gap, 1000)
        timed = next(c for c in cells if c['meta']['timed'] and c['meta']['form'] == 'whole')
        self.assertEqual(found['cells'][timed['id']]['lost_updates'], 0)

    def test_missing_killed_and_incomplete_results_fail(self) -> None:
        cell = section.experiment('whole', 64, 64, 64, True)
        self.assertFalse(section.analyse([], {})['passed'])
        self.assertFalse(section.analyse([cell], {})['passed'])
        variants = [{'status': 'killed'}, result_for(cell), result_for(cell), result_for(cell), result_for(cell)]
        variants[1]['runs'].pop()
        variants[2]['storage']['distinct'][0]['count'] -= 1
        variants[3]['storage']['distinct'] *= 2
        variants[4]['storage']['bytes'] -= 4
        for result in variants:
            self.assertFalse(section.analyse([cell], {cell['id']: result})['passed'])

    def test_missing_zero_nonfinite_and_nonpositive_slope_timestamps_fail(self) -> None:
        cell = section.experiment('whole', 64, 64, 64, True)
        for times in (
            [],
            [1],
            [0, 1, 1],
            [-1, 1, 1],
            [True, 1, 1],
            [float('nan')] * 3,
            [float('inf')] * 3,
            [1e308] * 3,
        ):
            result = result_for(cell)
            result['runs'][0]['gpu_ns'] = times
            self.assertFalse(section.analyse([cell], {cell['id']: result})['passed'])
        result = result_for(cell)
        for value in (100, 1e308):
            for run in result['runs']:
                run['gpu_ns'] = [value] * cell['repeats']
            self.assertFalse(section.analyse([cell], {cell['id']: result})['passed'])

    def test_generator_and_report_register_the_family(self) -> None:
        cells = generators.GENERATORS['interlock_section']()
        self.assertEqual(cells, section.generate())
        for command in ('interlock_section', 'run'):
            with tempfile.TemporaryDirectory() as directory:
                summary = report.write(command, cells, {}, Path(directory), [])
                found = json.loads((Path(directory) / 'report.json').read_text())
                self.assertFalse(found['interlock_section']['passed'])
                self.assertIn('60 interlock cells failed', summary)


if __name__ == '__main__':
    unittest.main()
