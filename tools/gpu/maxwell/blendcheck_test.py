"""The storage forms of testdata/rop_blend run on the host against the blend model, and the check's eye for a change."""

import unittest
from pathlib import Path

from tools.gpu.maxwell import blendcheck

STATES = Path(__file__).with_name('testdata') / 'rop_blend' / 'states.json'


class BlendCheckTest(unittest.TestCase):
    def test_every_storage_form_stores_the_models_texel(self):
        states = blendcheck.records(STATES)
        self.assertEqual(len(states), len(blendcheck.corpus()))
        for r in states:
            if 'rop_frag_unread' in r['key']:
                continue
            words = blendcheck.blend_color_words(r)
            with self.subTest(key=r['key'].split(' | ')[0]):
                verdict = blendcheck.check(bytes.fromhex(r['fs']), r['key'], words, 60, 7, blendcheck.bound_cbuf(r))
                self.assertEqual(verdict, 'identical')

    def test_a_color_written_but_never_read_keeps_the_device_blend(self):
        unread = [r for r in blendcheck.records(STATES) if 'rop_frag_unread' in r['key']]
        self.assertEqual(len(unread), len(blendcheck.UNREAD))
        for r in unread:
            self.assertEqual((r['storage_mask'], r['storage_blend_mask']), (0, 0), r['key'])

    def test_another_blend_is_seen(self):
        r = next(r for r in blendcheck.records(STATES) if 'c0=80:15:1:1:5:0:1:5:0 ' in r['key'])
        key = r['key'].replace('c0=80:15:1:1:5:0:1:5:0', 'c0=80:15:1:1:9:0:1:5:0')
        verdict = blendcheck.check(bytes.fromhex(r['fs']), key, {}, 20, 7)
        self.assertTrue(verdict.startswith('different'), verdict)


if __name__ == '__main__':
    unittest.main()
