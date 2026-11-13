"""floatadd.py: the batch tells the ROP's float add models apart, and its check names the one the texels hold."""

import itertools
import struct
import unittest
from collections.abc import Callable

from tools.benchloop import floatadd, protocol, texels


def result(experiment: dict, model: Callable[[float, float], float]) -> dict:
    """Give the texels a ROP adding by `model` writes."""
    fmt = experiment['target']['format']
    src, dst = experiment['meta']['src'], experiment['meta']['dst']
    data = b''.join(texels.encode((model(s, d),) * 4, fmt) for d in dst for s in src)
    return {'status': 'ok', 'texels': {'data': protocol.b64(data)}}


class FloatAdd(unittest.TestCase):
    def test_the_batch_is_valid(self):
        protocol.batch('floatadd', floatadd.generate())

    def test_each_inputs_separate_every_pair_of_models(self):
        for experiment in floatadd.generate():
            fmt = experiment['target']['format']
            src, dst = experiment['meta']['src'], experiment['meta']['dst']
            for a, b in itertools.combinations(floatadd.MODELS[fmt].values(), 2):
                differ = [
                    (s, d) for s in src for d in dst if floatadd._bits(a(s, d), fmt) != floatadd._bits(b(s, d), fmt)
                ]
                self.assertTrue(differ, (fmt, a.__name__, b.__name__))

    def test_the_check_names_the_model_the_texels_hold(self):
        for experiment in floatadd.generate():
            fmt = experiment['target']['format']
            for name, model in floatadd.MODELS[fmt].items():
                row = floatadd.check(experiment, result(experiment, model))
                self.assertEqual(row['verdict'], [name])
                self.assertEqual(row['shader_exact'], name == floatadd.SHADER[fmt])

    def test_fp16_double_rounding_misses_a_tie_the_shader_rounds_once(self):
        source = struct.unpack('<f', struct.pack('<f', 2.0**-11 + 2.0**-25))[0]
        self.assertEqual(floatadd.fp16_single(source, 1.0), 1.0 + 2.0**-10)
        self.assertEqual(floatadd.fp16_double(source, 1.0), 1.0)
        self.assertIn(source, floatadd.values('rgba16float')[0])


if __name__ == '__main__':
    unittest.main()
