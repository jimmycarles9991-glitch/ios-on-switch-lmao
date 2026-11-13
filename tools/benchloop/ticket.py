"""A pixel program that reads the interlock ticket must say so in its header.

The hardware hands a warp its ordering ticket only when the pixel header's READS_INTERLOCK_TICKET bit (610) is set, as
NAK's sph.rs writes it for any fragment shader that reads SR_ORDERING_TICKET. Without the bit the warp waits on a ticket
word that never moves: on 12 Nov (T-v3i) interlock.atom.64x64.check spun until the kernel killed the channel 30 s later.
"""

from tools.gpu.maxwell import isa

READS_TICKET_BIT = 610
SR_ORDERING_TICKET = 0x0F


def reads_ticket(code: bytes) -> bool:
    return any(
        isa.name(w) in ('S2R', 'CS2R') and isa.field(w, 20, 8) == SR_ORDERING_TICKET for w in isa.instructions(code)
    )


def header_says_so(sph: bytes) -> bool:
    return bool(sph[READS_TICKET_BIT // 8] >> READS_TICKET_BIT % 8 & 1)
