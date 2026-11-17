"""ticketcheck.py's verdict on a ticket-reading program, and on the configured kind's raw pack when given one."""

import os
import struct
import sys
import unittest
from pathlib import Path

from tools.gpu.maxwell import ticketcheck

SCHEDULE = 0x001F8000FC0007E0
S2R_TICKET = 0xF0C8 << 48 | ticketcheck.SR_ORDERING_TICKET << 20
S2R_AFFINITY = 0xF0C8 << 48 | 0x1C << 20
ATOM_ADD = 0xED00 << 48
RED_ADD = 0xEBF8 << 48
EXIT = 0xE300 << 48


def ldg(cache_op: int) -> int:
    return 0xEED0 << 48 | cache_op << 46


def code(*instructions: int) -> bytes:
    padded = list(instructions) + [EXIT] * (-len(instructions) % 3)
    words = []
    for i in range(0, len(padded), 3):
        words += [SCHEDULE, *padded[i : i + 3]]
    return struct.pack(f'<{len(words)}Q', *words)


def header(reads_ticket: bool) -> bytes:
    h = bytearray(0x50)
    h[ticketcheck.SPH_READS_INTERLOCK_TICKET // 8] |= reads_ticket << (ticketcheck.SPH_READS_INTERLOCK_TICKET % 8)
    return bytes(h)


class Faults(unittest.TestCase):
    def test_an_l2_load_wait_with_the_header_bit_passes(self):
        self.assertEqual(ticketcheck.faults(header(True), code(S2R_AFFINITY, S2R_TICKET, ldg(1), RED_ADD)), [])

    def test_the_atomic_wait_nak_used_to_emit_fails(self):
        found = ticketcheck.faults(header(True), code(S2R_TICKET, ATOM_ADD, RED_ADD))
        self.assertIn('an atomic ticket wait', found)
        self.assertIn('no LDG.CG ticket wait', found)

    def test_an_uncached_load_wait_fails(self):
        self.assertEqual(ticketcheck.faults(header(True), code(S2R_TICKET, ldg(3), RED_ADD)), ['no LDG.CG ticket wait'])

    def test_the_sections_own_uncached_loads_are_not_the_wait(self):
        self.assertEqual(ticketcheck.faults(header(True), code(S2R_TICKET, ldg(1), ldg(3), RED_ADD)), [])

    def test_a_ticket_reader_without_the_header_bit_fails(self):
        self.assertEqual(
            ticketcheck.faults(header(False), code(S2R_TICKET, ldg(1), RED_ADD)), ['no READS_INTERLOCK_TICKET']
        )

    def test_a_program_reading_no_ticket_is_not_judged(self):
        self.assertEqual(ticketcheck.faults(header(False), code(S2R_AFFINITY, ATOM_ADD, ldg(3))), [])


@unittest.skipUnless(os.environ.get('TICKETCHECK_PACK'), 'no pack given')
class WholePack(unittest.TestCase):
    def test_every_ticket_reader_of_the_pack_waits_on_l2(self):
        sys.argv = ['ticketcheck', str(Path(os.environ['TICKETCHECK_PACK']))]
        self.assertEqual(ticketcheck.main(), 0)


if __name__ == '__main__':
    unittest.main()
