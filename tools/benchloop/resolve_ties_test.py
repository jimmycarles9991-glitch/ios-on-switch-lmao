"""Batch resolvetie2 is valid and covers what it claims, and its verdict names each kind of engine from its texels."""

import unittest

from tools.benchloop import protocol, resolve_ties


def engine(rounding: str):
    """Give a fake device's result maker for an engine whose box rounds by `rounding` (a resolve_ties model)."""

    def result(e: dict, broken_rop: bool = False) -> dict:
        t, reading = e['twod'], e['meta']['reading']
        form = {'rop': t.get('rop', 0), 'invert_grid': int(bool(t.get('invertGrid')))}
        if reading == 'not1to1':
            src = protocol.unb64(e['source']['data'])
            data = src if broken_rop else bytes(255 - v for v in src)
            return {'status': 'ok', 'texels': {'data': protocol.b64(data)}, 'form': form}
        tuples, pixels = resolve_ties._tuples(e), e['target']['width'] * e['target']['height']
        point = t.get('byScale') and not t.get('bilinear')
        out, metal = bytearray(pixels * 4), bytearray(pixels * 4)
        for c in range(4):
            for p, s in enumerate(tuples[c]):
                inverted = tuple(255 - v for v in s)
                if point:
                    v = inverted[0] if t.get('invertGrid') else s[0]
                else:
                    v = resolve_ties.model(rounding, inverted if t.get('invertGrid') else s)
                out[p * 4 + c] = 255 - v if t.get('rop') else v
                metal[p * 4 + c] = resolve_ties.model('up', s)
        r = {'status': 'ok', 'texels': {'data': protocol.b64(bytes(out))}, 'form': form}
        if 'compare' in e:
            histogram: dict[str, int] = {}
            for p in range(pixels):
                most = max(abs(out[p * 4 + c] - metal[p * 4 + c]) for c in range(4))
                histogram[str(most)] = histogram.get(str(most), 0) + 1
            r['compare'] = {'status': 'ok', 'histogram': histogram}
        return r

    return result


class ResolveTiesTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.cells = resolve_ties.generate()

    def test_the_batch_is_one_valid_device_run(self) -> None:
        b = resolve_ties.batch()
        self.assertLessEqual(len(b['experiments']), protocol.MAX_EXPERIMENTS)
        self.assertLess(len(protocol.encode_batch(b)), 8 << 20)
        ids = [e['id'] for e in self.cells]
        self.assertEqual(len(set(ids)), len(ids))
        sandwich = [e for e in self.cells if e['meta']['reading'] == 'sandwich']
        self.assertTrue(all(e['twod']['rop'] == 0x33 and e['twod']['invertGrid'] for e in sandwich))
        self.assertTrue(all(e['compare'] == 'resolve3d' for e in sandwich))

    def test_the_sources_hold_every_pair_and_every_sum(self) -> None:
        pairs = {s for channel in resolve_ties.exhaustive2() for s in channel}
        self.assertEqual(len(pairs), 256 * 256)
        for channel in resolve_ties.exhaustive4():
            counts = [0] * 1021
            for s in channel:
                counts[sum(s)] += 1
                self.assertTrue(all(0 <= v <= 255 for v in s))
            self.assertGreaterEqual(min(counts), 4)
        for axis, (a, b, c, d) in (('x', (0, 1, 2, 3)), ('y', (0, 2, 1, 3))):
            for channel in resolve_ties.odd_pairs(axis):
                self.assertTrue(all((s[a] + s[b]) % 2 and (s[c] + s[d]) % 2 for s in channel))

    def test_a_cells_samples_come_back_from_its_grid(self) -> None:
        cell = next(e for e in self.cells if e['id'] == 'rt-bgra8unorm-4x-prod-ypairs')
        self.assertEqual(resolve_ties._tuples(cell), resolve_ties.odd_pairs('y'))
        cell = next(e for e in self.cells if e['id'] == 'rt-rgba8unorm-2x-prod')
        self.assertEqual(resolve_ties._tuples(cell), resolve_ties.exhaustive2())

    def test_the_models(self) -> None:
        m = resolve_ties.model
        self.assertEqual((m('up', (0, 1)), m('down', (0, 1))), (1, 0))
        s = (1, 1, 1, 0)
        self.assertEqual([m(k, s) for k in resolve_ties.MODELS[4]], [1, 1, 0, 0, 0, 1, 1])
        for n in (2, 4):
            for total in range(255 * n + 1):
                q, r = divmod(total, n)
                s = tuple(q + (k < r) for k in range(n))
                self.assertEqual(255 - m('down', tuple(255 - v for v in s)), m('up', s))
        self.assertNotEqual(255 - m('pair_x_down', (254, 255, 255, 255)), m('up', (1, 0, 0, 0)))

    def verdict(self, rounding: str, **broken: bool) -> list[str]:
        make = engine(rounding)
        return resolve_ties.analyse(self.cells, {e['id']: make(e, **broken) for e in self.cells})['verdict']

    def test_a_one_rounding_engine_reads_as_one_rounding_and_its_complement_as_exact(self) -> None:
        lines = self.verdict('down')
        for fmt in resolve_ties.FORMATS:
            for n in (2, 4):
                self.assertTrue(any(line.startswith(f'{fmt} {n}x: one ties-down rounding') for line in lines), lines)
                self.assertTrue(any(line.startswith(f'{fmt} {n}x: the complement route is exact') for line in lines))
        self.assertTrue(any('twodResolveSingleRounding4x() true' in line for line in lines))
        self.assertTrue(any(line.startswith('4x: no sample mode') for line in lines), lines)

    def test_a_two_step_engine_reads_as_two_steps_and_its_complement_as_within_one(self) -> None:
        lines = self.verdict('pair_x_down')
        self.assertTrue(any(line == 'bgra8unorm 4x: two rounded 2:1 steps, x pairs first' for line in lines), lines)
        self.assertTrue(any(line.startswith('bgra8unorm 4x: two steps, so the complement') for line in lines), lines)
        self.assertFalse(any('twodResolveSingleRounding4x() true' in line for line in lines))

    def test_a_rop_the_engine_ignored_decides_nothing(self) -> None:
        lines = self.verdict('down', broken_rop=True)
        self.assertEqual(len(lines), 1)
        self.assertTrue(lines[0].startswith('controls failed'), lines)


if __name__ == '__main__':
    unittest.main()
