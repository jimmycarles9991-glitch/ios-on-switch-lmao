"""frame_weights.py over "SwitchMetal: fw" lines as drivers/TegraGM20BMetal/draw_times.cpp writes them."""

import tempfile
import unittest
from pathlib import Path

from tools.devicerun import frame_weights

SUBSYSTEM = 'com.brunostjohn.switch.TegraGM20BMetal'
GENERIC = 'aaaaaaaaaaaaaaaa'
FILL = '00000000000000f1'

# One capture of two command buffers: a glass state drawn twice full-screen, a fill, the start, pass and tail marks
CAPTURE = [
    'capture backboardd asked 2 stamp 41 ref 3000000 2000000',
    'cb b0 s41 start 1000 gpu 5050000 marks 6 unwritten 0 full 0',
    'passes b0 0:80/1280x720/1/bl',
    f'state 0 {GENERIC} variant fs4000 vs300 r48 x1 PBGRAXm_A2Xghfc',
    f'state 1 {FILL} whole fs40 vs300 r16 x1 PRGhAXm_Fill',
    (
        'd b0 s:-:-:100000 b:0:0:200000 p:0:921600:300000 d0:0:4:3x1:921600:921600:4000000 '
        'd1:0:4:6x1:921600:921600:400000 t:-:-:50000'
    ),
    'cb b1 s42 start 10241000 gpu 4400000 marks 4 unwritten 1 full 0',
    'passes b1 1:80/1280x720/1/bl',
    'd b1 s:-:-:100000 p:1:0:200000 d0:1:4:3x1:921600:921600:4000000 t:-:-:100000',
    'end taken 2 reported 2 dropped 0 lines 10',
]


def record(message: str, pid: int = 77) -> dict:
    return {
        'timestamp': '2026-11-18 12:00:00.000000+0000',
        'bootUUID': 'BOOTA000',
        'processID': pid,
        'processImagePath': '/usr/libexec/backboardd',
        'subsystem': SUBSYSTEM,
        'eventMessage': message,
    }


def records(lines: list[str], serial: int = 1, pid: int = 77, skip: int = -1) -> list[dict]:
    return [record(f'SwitchMetal: fw c{serial}.{n} {text}', pid) for n, text in enumerate(lines) if n != skip]


def one(lines: list[dict]) -> frame_weights.Capture:
    captures = list(frame_weights.parse(lines).values())
    assert len(captures) == 1, captures
    return captures[0]


class FrameWeights(unittest.TestCase):
    def test_a_capture_adds_up_by_drawn_program(self):
        c = one(records(CAPTURE))
        ws = frame_weights.weights(c, {}, {})
        self.assertEqual([w.label for w in ws], ['PBGRAXm_A2Xghfc', 'PRGhAXm_Fill'])
        self.assertEqual((ws[0].draws, ws[0].invocations, ws[0].ns), (2, 2 * 921600, 8000000))
        self.assertAlmostEqual(ws[0].ns_per_pixel(), 8000000 / (2 * 921600))
        text = frame_weights.report(c, {}, {}, 20)
        self.assertIn('2 of 2 command buffers, 0 dropped, lines complete', text)
        # 9.45 ms isolated over two command buffers against 5 ms for the two before: x1.89 each
        self.assertIn(
            'isolated GPU time 9.45 ms over 2 command buffers; the 2 before it took 5.00 ms (x1.89 each)', text
        )
        self.assertIn('unwritten marks 1', text)
        self.assertIn('start 0.20 ms, before passes 0.20 ms, prologues 0.50 ms, tails 0.15 ms', text)
        self.assertIn('command buffers start at 0.00, 16.67 ms on the GPU', text)
        self.assertRegex(text, r'1 PBGRAXm_A2Xghfc +variant +48 +40 +2 +1843  +1843 +8\.000 +84\.7%')

    def test_a_lost_line_is_named(self):
        c = one(records(CAPTURE, skip=5))
        self.assertEqual(c.missing(), [5])
        self.assertIn('MISSING LINES [5]', frame_weights.report(c, {}, {}, 20))

    def test_a_capture_without_its_end_says_so(self):
        c = one(records(CAPTURE[:-1]))
        self.assertEqual(c.missing(), [])
        self.assertIn('no end line yet', frame_weights.report(c, {}, {}, 20))

    def test_static_counts_join_by_key_and_form_and_stand_only_for_the_same_code(self):
        c = one(records(CAPTURE))
        statics = {
            (int(FILL, 16), 'whole'): {'instructions': 29, 'words': 40},
            (int(GENERIC, 16), 'variant'): {'instructions': 41874, 'words': 55832},
        }
        ws = {w.label: w for w in frame_weights.weights(c, {}, statics)}
        self.assertEqual(ws['PRGhAXm_Fill'].instructions, 29)
        # the pack's record under the key is the generic program, not the variant the device drew: words x 3/4
        self.assertEqual(ws['PBGRAXm_A2Xghfc'].instructions, 3000)
        table = '\n'.join(frame_weights.table(list(ws.values()), 9450000, 20))
        self.assertIn('~ 3000', table)
        self.assertNotIn('~   29', table)

    def test_item_10_names_the_programs_whose_registers_cap_occupancy(self):
        lines = [
            *CAPTURE[:3],
            f'state 0 {GENERIC} whole fs55832 vs300 r128 x1 PBGRAXm_A2Xghfc',
            f'state 1 {FILL} whole fs40 vs300 r16 x1 PRGhAXm_Fill',
            *CAPTURE[5:],
        ]
        text = '\n'.join(frame_weights.huge(frame_weights.weights(one(records(lines)), {}, {}), 9450000))
        self.assertIn('item 10: programs of 96+ registers took 84.7% (8.00 ms)', text)
        self.assertIn('PBGRAXm_A2Xghfc', text)
        self.assertIn('r128 warps 16 draws 2 kpx 1843', text)
        self.assertNotIn('PRGhAXm_Fill', text.split('\n', 1)[1])

    def test_unnamed_states_take_their_label_from_the_names_file(self):
        lines = [*CAPTURE[:4], f'state 1 {FILL} whole fs40 vs300 r16 x1 -', *CAPTURE[5:]]
        with tempfile.TemporaryDirectory() as d:
            names = Path(d) / 'names.txt'
            names.write_text(f'{FILL} uber_fragment RenderBox\nnot a line\n')
            labels = frame_weights.read_names(names)
        ws = frame_weights.weights(one(records(lines)), labels, {})
        self.assertEqual(ws[1].label, 'uber_fragment RenderBox')
        self.assertEqual(frame_weights.weights(one(records(lines)), {}, {})[1].label, f'k{FILL}')

    def test_a_draw_the_counter_missed_divides_by_its_box_and_says_so(self):
        lines = [*CAPTURE[:5], 'd b0 s:-:-:100000 d1:0:4:6x1:460800:-:400000 t:-:-:50000', *CAPTURE[6:]]
        ws = {w.label: w for w in frame_weights.weights(one(records(lines)), {}, {})}
        fill = ws['PRGhAXm_Fill']
        self.assertTrue(fill.boxed)
        self.assertEqual(fill.pixels, 460800)
        table = '\n'.join(frame_weights.table(list(ws.values()), 9450000, 20))
        self.assertRegex(table, r'PRGhAXm_Fill +whole +16 +64 +1 +0\* +461 ')

    def test_refused_marks_and_dropped_command_buffers_are_named(self):
        lines = [
            CAPTURE[0],
            'cb b0 s41 start 1000 gpu 5050000 marks 6 unwritten 0 full 3',
            *CAPTURE[2:6],
            'dropped b1 lost',
            'end taken 2 reported 1 dropped 1 lines 8',
        ]
        text = frame_weights.report(one(records(lines)), {}, {}, 20)
        self.assertIn('1 of 2 command buffers, 1 dropped, lines complete', text)
        self.assertIn('dropped b1 lost', text)
        self.assertIn('MARKS REFUSED 3 (their time is in the tails)', text)

    def test_captures_of_two_processes_stay_apart(self):
        captures = frame_weights.parse(records(CAPTURE) + records(CAPTURE, pid=78))
        self.assertEqual(len(captures), 2)

    def test_a_pack_nakstat_cannot_read_gives_no_counts_and_says_why(self):
        with tempfile.TemporaryDirectory() as d:
            pack = Path(d) / 'pack.bin'
            pack.write_bytes(b'NOTAPACK' + bytes(64))
            statics, why = frame_weights.static_rows(pack, {(1, 'whole')})
        self.assertEqual(statics, {})
        self.assertIn('not a raw pack', why)


if __name__ == '__main__':
    unittest.main()
