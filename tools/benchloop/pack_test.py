"""A pack experiment is checked as the app checks it.

Its state, its form, one command buffer a repeat, each quad's rect inside the target and each uniform block inside its
quad's 64 KiB region.
"""

import unittest

from tools.benchloop import protocol


def pack(**changes: object) -> dict:
    e = {
        'id': 'iscd-split',
        'kind': 'pack',
        'target': {'format': 'rgba16float', 'width': 256, 'height': 256},
        'template': '618b45175101cd20',
        'readback': 'full',
        'pack': {
            'form': 'split',
            'ca': True,
            'quads': [
                {
                    'rect': [0, 0, 128, 256],
                    'ubos': [{'stage': 2, 'slot': 1, 'offset': 80, 'data': protocol.b64(bytes(64))}],
                },
                {'rect': [128, 0, 256, 256]},
            ],
        },
    }
    e.update(changes)
    return e


def why(e: dict) -> str:
    try:
        protocol.validate_experiment(e)
    except ValueError as invalid:
        return str(invalid)
    return ''


class PackTest(unittest.TestCase):
    def test_every_form_is_valid(self) -> None:
        for form in protocol.PACK_FORMS:
            e = pack()
            e['pack']['form'] = form
            self.assertEqual(why(e), '', form)
        self.assertEqual(protocol.estimated_ms(pack()), 0.0)

    def test_a_pack_names_its_state_and_takes_one_command_buffer_a_repeat(self) -> None:
        e = pack()
        del e['template']
        self.assertIn('template', why(e))
        self.assertIn('draws [1]', why(pack(draws=[2])))
        e = pack()
        e['pack']['form'] = 'twin'
        self.assertIn('pack.form', why(e))

    def test_quads_and_blocks_stay_inside_their_bounds(self) -> None:
        e = pack()
        e['pack']['quads'][1]['rect'] = [128, 0, 257, 256]
        self.assertIn('outside the target', why(e))
        e = pack()
        e['pack']['quads'][0]['ubos'][0]['offset'] = protocol.QUAD_REGION - 32
        self.assertIn('inside 64 KiB', why(e))
        e = pack()
        e['pack']['quads'][0]['ubos'][0]['stage'] = 3
        self.assertIn('stage', why(e))
        e = pack()
        e['pack']['quads'] = [e['pack']['quads'][1]] * (protocol.MAX_QUADS + 1)
        self.assertIn('pack.quads', why(e))

    def test_only_a_pack_carries_pack(self) -> None:
        e = pack(kind='known')
        self.assertIn('only pack experiments', why(e))


if __name__ == '__main__':
    unittest.main()
