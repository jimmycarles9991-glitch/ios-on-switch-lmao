import struct
import unittest

from tools.benchloop import protocol, qmdchain
from tools.benchloop.sm53 import encode
from tools.benchloop.sm53.program import inputs
from tools.gpu.maxwell import execute


def stores(code: bytes) -> list[int]:
    """Give the byte offsets of the kernel's STGs, in order."""
    return [i.word >> 20 & 0xFFFFFF for i in encode.unbundle(code) if i.name == 'STG']


class QmdChain(unittest.TestCase):
    def test_every_experiment_is_one_command_buffer_the_app_takes(self):
        batch = protocol.batch('qmdchain', qmdchain.generate())
        ids = [e['id'] for e in batch['experiments']]
        self.assertEqual(ids, ['qc-ab', 'qc-ab-off', 'qc-abc', 'qc-steer'])
        for e in batch['experiments']:
            c = e['compute']
            self.assertEqual(len(c['schedule']), len(c['chain']))
            self.assertEqual(c['output'], qmdchain.OUTPUT_BYTES)

    def test_each_kernel_stores_its_marker_in_its_own_word(self):
        for i in range(4):
            self.assertEqual(stores(qmdchain.kernel(i).code), [4 * i])

    def test_the_steering_kernel_rewrites_qmd_1s_pointer_before_its_marker(self):
        self.assertEqual(stores(qmdchain.kernel(0, steer=(1, 3)).code), [qmdchain.POINTER_BYTE, 0])

    def test_the_steered_pointer_is_the_target_qmds_address_over_256(self):
        qmds = 0x0012_3456_7800
        body = [*qmdchain._addresses(), *qmdchain.steered_pointer(3), encode.mov(0, qmdchain.POINTER)]
        code = encode.bundle(encode.finish(body))
        data = struct.pack('<QQ', 0, qmds)
        regs, _ = execute.run(code, inputs(data), 1)
        self.assertEqual(regs[0], ((qmds + 2 * qmdchain.QMD_BYTES) >> 8) & 0xFFFFFFFF)

    def test_expected_words_are_the_markers_of_what_ran_and_the_release_count(self):
        words = qmdchain.expected([0, 1, 3])
        self.assertEqual(words[:4], [0xA0, 0xB0, qmdchain.UNWRITTEN, 0xD0])
        self.assertEqual(words[-1], 3)

    def test_analyse_tells_the_steered_outcome_from_the_cpus(self):
        steer = qmdchain.generate()[-1]

        def result(ran):
            return {'status': 'ok', 'output': {'data': protocol.b64(struct.pack('<16I', *qmdchain.expected(ran)))}}

        rows = qmdchain.analyse([steer], {steer['id']: result([0, 1, 3])})['rows']
        self.assertTrue(rows[0]['expected'])
        rows = qmdchain.analyse([steer], {steer['id']: result([0, 1, 2])})['rows']
        self.assertFalse(rows[0]['expected'])
        self.assertTrue(rows[0]['otherwise'])


if __name__ == '__main__':
    unittest.main()
