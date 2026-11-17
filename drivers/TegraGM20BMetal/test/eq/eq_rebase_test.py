"""eq_rebase.py's rules on draws made by hand."""

import unittest

import eq_fields
import eq_rebase

TARGET = {'rt0': 'rt0=N1 64x64 fmt=d5 mem=1000 pitch=0', 'draw': 'DRAW 0 4 00000000'}


def draw(registers: dict, barriers: set, first: bool = False) -> eq_fields.Draw:
    return eq_fields.Draw(dict(TARGET), registers, {}, barriers, first)


class Form(unittest.TestCase):
    def test_a_storage_form_drawn_by_its_barrier_twin_is_the_twin(self):
        tag, why = eq_rebase.form({'baseline', 'storage'}, draw({}, {'PSB', 'TEXINV'}), [])
        self.assertEqual((tag, why), ('twin', None))

    def test_a_storage_form_drawn_by_its_fetch_free_form_is_that(self):
        tag, _ = eq_rebase.form({'storage'}, draw({'1360': '1'}, set()), [])
        self.assertEqual(tag, 'fetchfree')

    def test_no_form_moves_its_targets_or_geometry(self):
        tag, why = eq_rebase.form({'storage'}, draw({}, {'PSB'}), ['rt0', 'stage5'])
        self.assertIsNone(tag)
        self.assertEqual(why, "took another form and moved ['rt0']")

    def test_a_draw_still_in_its_storage_form_or_never_in_one_is_no_form(self):
        self.assertEqual(eq_rebase.form({'storage'}, draw({'1224': '1'}, set()), []), (None, None))
        self.assertEqual(eq_rebase.form({'baseline'}, draw({}, {'PSB'}), []), (None, None))


class Opening(unittest.TestCase):
    def test_a_pass_opened_by_the_pixel_barrier_or_after_a_planner_entry(self):
        self.assertEqual(eq_rebase.opening(draw({}, {'PSB', 'TEXINV'}, True), {'SHINV', 'CBINV'}), 'opening')
        self.assertEqual(eq_rebase.opening(draw({}, {'CBINV', 'SHINV'}, True), {'WFI', 'TEXINV'}), 'opening')

    def test_nothing_else_a_pass_opening_drops(self):
        self.assertIsNone(eq_rebase.opening(draw({}, {'PSB'}, True), {'TICINV'}))
        self.assertIsNone(eq_rebase.opening(draw({}, {'PSB'}, False), {'SHINV'}))
        self.assertIsNone(eq_rebase.opening(draw({}, set(), True), {'SHINV'}), 'neither the pixel barrier nor a wait')


class Follows(unittest.TestCase):
    def test_the_shader_barrier_after_a_draw_that_took_another_form_goes(self):
        draws = [draw({}, set(), True), draw({}, {'PSB'}), draw({}, set())]
        self.assertEqual(eq_rebase.follows(draws, 2, {1}, {'SHINV', 'TEXINV'}), 'follows')

    def test_not_past_the_pass_nor_for_other_barriers(self):
        draws = [draw({}, set(), True), draw({}, set(), True), draw({}, set())]
        self.assertIsNone(eq_rebase.follows(draws, 2, {0}, {'SHINV'}))
        self.assertIsNone(eq_rebase.follows(draws, 2, {1}, {'TSCINV'}))


class PushWord(unittest.TestCase):
    def test_push_words_after_a_draw_that_took_another_form_move_through_the_pushed_draws_of_its_pass(self):
        draws = [draw({}, set(), True), draw({}, set()), draw({}, set()), draw({}, set())]
        self.assertEqual(eq_rebase.pushword(draws, 3, {1}, {2, 3}, ['stage1', 'stage5']), ('pushword', None))

    def test_not_past_a_draw_untagged_push_nor_the_pass_nor_more_than_its_stages(self):
        draws = [draw({}, set(), True), draw({}, set()), draw({}, set(), True), draw({}, set())]
        self.assertEqual(eq_rebase.pushword(draws, 3, {1}, {2, 3}, []), (None, None))
        draws[2].first = False
        self.assertEqual(eq_rebase.pushword(draws, 3, {1}, {3}, []), (None, None))
        self.assertEqual(eq_rebase.pushword(draws, 3, {1}, {2, 3}, ['R']), (None, "moved ['R']"))


def run(uniform_hash: str, set_hash: str = 'bbbb') -> list[str]:
    return [
        'DRAW first=0 count=4 begin=00000000 barriers=PSB',
        '  stage5 prog=p regs=8 group=4',
        '    cb1 type=5 size=10 data=' + uniform_hash + ':10 @0=ptr{c0ffee:100}',
        '    cb2 type=3 size=10 data=' + set_hash + ':10',
    ]


KEPT_WORDS = ['U 5 1 1000005 7 | 0:000260db02000291']


class TscRenumber(unittest.TestCase):
    def test_a_sampler_header_at_another_index_holding_the_same_words_is_a_renumber(self):
        words = ['U 5 1 1e00005 7 | 0:000260db02000291']
        self.assertEqual(eq_rebase.tsc_renumber(run('aaaa'), KEPT_WORDS, run('cccc'), words), [])

    def test_another_sampler_header_or_texture_header_is_refused(self):
        other_sampler = ['U 5 1 1e00005 7 | 0:0002609202000291']
        other_texture = ['U 5 1 1e00006 7 | 0:000260db02000291']
        for words in (other_sampler, other_texture):
            refused = eq_rebase.tsc_renumber(run('aaaa'), KEPT_WORDS, run('cccc'), words)
            self.assertEqual(len(refused), 1, words)
            self.assertIn('word 0', refused[0])

    def test_a_buffer_with_no_handle_word_is_read_too(self):
        self.assertEqual(
            eq_rebase.tsc_renumber(run('aaaa'), ['U 5 1 7 |'], run('cccc'), ['U 5 1 8 |']),
            ['tscrenumber: uniform buffer 0 word 0 7 -> 8 is no sampler header renumbered'],
        )

    def test_a_word_that_names_no_header_is_refused(self):
        words = ['U 5 1 1000005 8 | 0:000260db02000291']
        refused = eq_rebase.tsc_renumber(run('aaaa'), KEPT_WORDS, run('cccc'), words)
        self.assertEqual(refused, ['tscrenumber: uniform buffer 0 word 1 7 -> 8 is no sampler header renumbered'])

    def test_any_other_line_that_moved_is_refused(self):
        refused = eq_rebase.tsc_renumber(run('aaaa'), KEPT_WORDS, run('aaaa', 'dddd'), KEPT_WORDS)
        self.assertEqual(len(refused), 1)
        self.assertIn('no uniform buffer', refused[0])

    def test_dumps_that_do_not_list_the_logs_uniform_buffers_are_refused(self):
        words = ['U 1 1 1e00005 7 | 0:000260db02000291']
        refused = eq_rebase.tsc_renumber(run('aaaa'), KEPT_WORDS, run('cccc'), words)
        self.assertEqual(refused, ["tscrenumber: the word dumps do not list the logs' uniform buffers"])


if __name__ == '__main__':
    unittest.main()
