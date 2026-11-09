"""The decoder against encodings whose meaning the falcon docs and the NVENC firmware fix."""

import unittest

from tools.falcon import isa


def _one(raw: str, address: int = 0) -> str:
    code = bytes.fromhex(raw)
    insn = isa.decode(code, 0, address)
    if insn is None or insn.size != len(code):
        raise AssertionError(raw)
    return str(insn).strip()


class DecodeTest(unittest.TestCase):
    def test_immediate_moves_and_sethi(self):
        self.assertEqual('mov $r9 0xa00', _one('89000a00'))
        self.assertEqual('mov $r9 0xd0b70006', _one('d90600b7d0'))
        self.assertEqual('mov $r5 -0x3', _one('05fd'))
        self.assertEqual('sethi $r9 0x10000', _one('f1930100'))

    def test_loads_stores_and_io(self):
        self.assertEqual('ld b32 $r15 D[$r9]', _one('bf9f'))
        self.assertEqual('ld b8 $r11 D[$r9+0xec]', _one('189bec'))
        self.assertEqual('st b32 D[$r9+0x4] $r15', _one('b59f01'))
        self.assertEqual('ld b32 $r13 D[$sp+0x2c]', _one('b4d00b'))
        self.assertEqual('iowr I[$r15] $r9', _one('faf900'))
        self.assertEqual('iord $r9 I[$r9]', _one('cf9900'))

    def test_transfers_and_specials(self):
        self.assertEqual('xdld $r15 $r9', _one('faf905'))
        self.assertEqual('mov $xdbase $r9', _one('fe9700'))
        self.assertEqual('extr $r9 $r9 8:9', _one('e7990801'))

    def test_branches(self):
        self.assertEqual('bra e 0x8db2', _one('f40b0f', 0x8DA3))
        self.assertEqual('brane b32 $r9 0x0 0x2f6c', _one('b3940010', 0x2F5C))
        self.assertEqual('lcall 0x18', _one('7e180000'))
        lbra = isa.decode(bytes.fromhex('3e410000'), 0, 0x30)
        ret = isa.decode(bytes.fromhex('f800'), 0, 0)
        self.assertTrue(lbra is not None and lbra.ends)
        self.assertTrue(ret is not None and ret.ends)

    def test_encodings_v5_does_not_have(self):
        self.assertIsNone(isa.decode(bytes.fromhex('f5210000'), 0, 0))  # v0-v4 long call


if __name__ == '__main__':
    unittest.main()
