"""What a Maxwell program costs a scheduler: GM20B's measured issue cycles per warp-instruction and register-bank reads.

The rates are the device bench's (out of the bench loop, 11 Nov: tput1/tput2 over 1,024 warps, loop1's 769.1 MHz clock,
dual1's reuse runs): an instruction of the FP32 class issues in one cycle, the half-rate class in two, compares in 2.25,
the multi-function and conversion unit in four and a 32-bit IMUL in 130. An instruction reading more than one source
register from the same bank (register % 4) pays one cycle per extra read. Classes the bench did not time count one cycle
and are counted apart, so the estimate says how much of it is a guess.
"""

from tools.gpu.maxwell import isa

CLOCK_HZ = 769.1e6
LANES_PER_CYCLE = 256  # 2 SMs x 4 schedulers x 32 lanes
RZ = 255

MEASURED = {
    **dict.fromkeys(
        (
            'FFMA',
            'FADD',
            'FMUL',
            'FFMA32I',
            'FADD32I',
            'FMUL32I',
            'HADD2',
            'HMUL2',
            'HFMA2',
            'IADD',
            'IADD32I',
            'LOP',
            'LOP32I',
            'MOV',
            'MOV32I',
        ),
        1.0,
    ),
    **dict.fromkeys(('SHL', 'SHR', 'SEL', 'BFE', 'PRMT', 'FMNMX', 'IMNMX', 'RRO'), 2.0),
    **dict.fromkeys(('FSETP', 'ISETP'), 2.25),
    **dict.fromkeys(('MUFU', 'F2F', 'F2I', 'I2F', 'FLO'), 4.0),
    **dict.fromkeys(('IMUL', 'IMUL32I'), 130.0),
}

# Third sources (bits 39-46) of the register forms that have one; FFMA_rc and XMAD_rc read their B there instead.
THREE_SOURCE = ('FFMA', 'HFMA2', 'XMAD', 'LOP3', 'IADD3', 'PRMT', 'BFI', 'FCMP', 'ICMP', 'SHF', 'DFMA')
NO_A = ('MOV', 'MOV32I', 'F2F', 'F2I', 'I2F', 'I2I', 'FLO', 'POPC', 'S2R', 'CS2R', 'NOP', 'BRA', 'SSY', 'SYNC', 'EXIT')


def base(name: str) -> str:
    return name.split('_', 1)[0]


def issue_cycles(name: str) -> float | None:
    """Give a warp-instruction's measured issue cycles, None where the bench did not time its class."""
    return MEASURED.get(base(name))


def gpr_sources(word: int, name: str) -> list[int]:
    """Give the GPR sources an ALU instruction reads from the register file, RZ left out."""
    b = base(name)
    form = name.rsplit('_', 1)[-1] if '_' in name else ''
    regs = []
    if b not in NO_A:
        regs.append(isa.field(word, 8, 8))
    if form == 'reg':
        regs.append(isa.field(word, 20, 8))
    if b in THREE_SOURCE and form in ('reg', 'rc', 'cr', 'imm'):
        regs.append(isa.field(word, 39, 8))
    return [r for r in regs if r != RZ]


def bank_extra(regs: list[int], distinct: bool) -> int:
    """Extra register-file reads `regs` costs: one for each read past the first from a bank."""
    if distinct:
        regs = list(dict.fromkeys(regs))
    per_bank = [0, 0, 0, 0]
    for r in regs:
        per_bank[r % 4] += 1
    return sum(max(0, n - 1) for n in per_bank)


def program(code: bytes) -> dict:
    """Give a program's static cost.

    Estimated cycles without and with overlap, unmeasured instructions, same-bank extra reads (distinct registers, and
    every read), and its code bytes.
    """
    cycles = 0.0
    unmeasured = bank = bank_reads = slots = 0
    occupancy = {}
    for word in isa.instructions(code):
        name = isa.name(word)
        c = issue_cycles(name)
        if c is None:
            unmeasured += 1
            c = 1.0
        regs = gpr_sources(word, name)
        extra = bank_extra(regs, distinct=True)
        bank += extra
        bank_reads += bank_extra(regs, distinct=False)
        cycles += c + extra
        slots += 1 + extra
        occupancy[c] = occupancy.get(c, 0.0) + c
    return {
        'cycles': round(cycles, 2),
        'cycles_overlap': round(max([float(slots), *occupancy.values()]), 2),
        'unmeasured': unmeasured,
        'bank': bank,
        'bank_reads': bank_reads,
        'bytes': len(isa.words_of(code)) * 8,
        'stalls': stalls(code),
    }


def stalls(code: bytes) -> int:
    words = isa.words_of(code)
    total = 0
    for at in range(0, len(words), 4):
        for k in range(min(3, len(words) - at - 1)):
            total += (words[at] >> (21 * k)) & 0xF
    return total


def predicted_ms(cycles: float, pixels: int) -> float:
    return cycles * pixels / (LANES_PER_CYCLE * CLOCK_HZ) * 1e3
