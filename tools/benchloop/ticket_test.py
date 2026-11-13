"""A pixel program that reads SR_ORDERING_TICKET goes out only with its header's READS_INTERLOCK_TICKET bit."""

import unittest

from tools.benchloop import protocol, ticket
from tools.benchloop.sm53 import encode, operands, sph
from tools.gpu.maxwell import isa


def s2r(d: int, sr: int) -> encode.Instr:
    pattern = next(p for p in isa.PATTERNS if p.name == 'S2R')
    return encode.Instr(pattern.value | d | sr << 20)


def readback(code: bytes, header: bytes) -> dict:
    return {
        'id': 'ticket',
        'kind': 'readback',
        'target': {'format': 'rgba32float', 'width': 8, 'height': 2},
        'program': {'sph': protocol.b64(header), 'code': protocol.b64(code), 'data': '', 'gprs': 8},
    }


class TicketTest(unittest.TestCase):
    def test_a_ticket_reader_needs_the_bit(self) -> None:
        body = [
            s2r(0, ticket.SR_ORDERING_TICKET).sched(delay=15),
            *[encode.mov32i(r, 0).sched(delay=15) for r in (1, 2, 3)],
        ]
        code = encode.bundle(encode.finish(body))
        self.assertTrue(ticket.reads_ticket(code))
        with self.assertRaises(protocol.InvalidError):
            protocol.validate_experiment(readback(code, sph.pixel(operands.attributes(code))))
        header = sph.pixel(operands.attributes(code), reads_ticket=True)
        self.assertTrue(ticket.header_says_so(header))
        protocol.validate_experiment(readback(code, header))

    def test_other_system_registers_need_nothing(self) -> None:
        body = [s2r(0, 0x1C).sched(delay=15), *[encode.mov32i(r, 0).sched(delay=15) for r in (1, 2, 3)]]
        code = encode.bundle(encode.finish(body))
        self.assertFalse(ticket.reads_ticket(code))
        protocol.validate_experiment(readback(code, sph.pixel(operands.attributes(code))))


if __name__ == '__main__':
    unittest.main()
