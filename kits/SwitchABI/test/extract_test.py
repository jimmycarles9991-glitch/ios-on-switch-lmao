"""extract.py's one path for function hints, over a fake kernelcache."""

import unittest

from kits.SwitchABI import extract

CODE = {0x1000: [0xD65F03C0], 0x2000: [0xB9401C00, 0xD65F03C0]}


def body(at: int, count: int) -> list[int] | None:
    return CODE[at][:count] if at in CODE else None


def find(hint: dict) -> int:
    return {'a string': 0x1000}[hint['string']]


def located(hints: dict) -> tuple[dict, dict]:
    return extract.functions(body, find, {'hints': {'functions': hints}})


class FunctionsTest(unittest.TestCase):
    def test_every_hint_kind(self):
        addresses, found = located(
            {
                'gone': 'unported',
                'direction': {'at': '0x2000', 'body': 'b9401c00 d65f03c0', 'why': 'ldr w0, [x0, #0x1c]'},
                'named': {'string': 'a string', 'up': 0},
            }
        )
        self.assertEqual(addresses, {'direction': '0x2000', 'gone': 'unported', 'named': '0x1000'})
        self.assertEqual(found['direction'], 'ldr w0, [x0, #0x1c]')
        self.assertEqual(found['named'], 'references "a string"')

    def test_other_words_are_refused(self):
        with self.assertRaises(SystemExit):
            located({'direction': {'at': '0x2000', 'body': 'b9402000', 'why': ''}})

    def test_an_address_no_function_starts_at_is_refused(self):
        with self.assertRaises(SystemExit):
            located({'direction': {'at': '0x2004', 'body': 'd65f03c0', 'why': ''}})

    def test_a_plain_kernelcache_reads_the_words_where_the_table_binds(self):
        words = {0x2000: 0xB9401C00, 0x2004: 0xD65F03C0}
        plain = extract.plain_body(words.__getitem__)
        self.assertEqual(plain(0x2000, 2), [0xB9401C00, 0xD65F03C0])


CONSTANT_HINT = {'value': '0x1c', 'at': '0x2000', 'word': 'b9401c00', 'why': 'ldr w0, [x0, #0x1c]'}


class PlainConstantsTest(unittest.TestCase):
    def errors(self, table_value: str, word: int = 0xB9401C00) -> list[str]:
        table = {'constants': {'count': table_value}, 'hints': {'constants': {'count': CONSTANT_HINT}}}
        return extract.plain_constant_errors({0x2000: word}.__getitem__, table)

    def test_a_matching_word_and_value_pass(self):
        self.assertEqual(self.errors('0x1c'), [])

    def test_another_word_is_refused(self):
        self.assertEqual(len(self.errors('0x1c', word=0xB9402000)), 1)

    def test_a_value_the_hint_does_not_show_is_refused(self):
        self.assertEqual(len(self.errors('0x20')), 1)


if __name__ == '__main__':
    unittest.main()
