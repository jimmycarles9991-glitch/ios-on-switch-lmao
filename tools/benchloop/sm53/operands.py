"""The registers each instruction of encode.py's subset reads and writes, by operand slot, and its latency-table key.

Slots are the operand fields the reuse flags name: bit 0 the A field (bits 8-15), bit 1 the B field (20-27), bit 2
the C field (39-46). An FFMA_rc keeps its register B in the C field, so it is read there.
"""

from dataclasses import dataclass

from tools.benchloop.sm53.encode import RZ, Control, Instr, unbundle
from tools.gpu.maxwell import isa
from tools.gpu.maxwell.isa import field as bits

SLOT_FIELDS = (8, 20, 39)
READS_A = {'FADD', 'FMUL', 'FFMA', 'FMNMX', 'FSETP', 'HADD2', 'HMUL2', 'HFMA2', 'MUFU', 'IADD', 'IADD32I', 'IMUL'}
READS_A |= {'LOP', 'LOP32I', 'SHL', 'SHR', 'IMNMX', 'BFE', 'SEL', 'PRMT', 'ISETP', 'IPA'}
READS_C = {'FFMA', 'HFMA2', 'PRMT', 'IPA'}
WRITES_PRED = {'FSETP', 'ISETP'}
NO_DESTINATION = {'NOP', 'EXIT'}
# ir.rs has_fixed_latency for SM5x: these finish whenever they finish and use a scoreboard; the rest do not
VARIABLE = {'MUFU', 'RRO', 'F2F', 'F2I', 'I2F', 'I2I', 'IMUL', 'FLO', 'POPC', 'BREV', 'IPA', 'LDC'}
MUFU_FUNCS = {0: 'COS', 1: 'SIN', 2: 'EX2', 3: 'LG2', 4: 'RCP', 5: 'RSQ', 8: 'SQRT'}


@dataclass(frozen=True)
class Decoded:
    name: str
    word: int
    control: Control

    @property
    def base(self) -> str:
        return self.name.split('_', 1)[0]

    @property
    def key(self) -> str:
        """Give the latency-table key: the base op, MUFU with its function."""
        if self.base == 'MUFU':
            return f'MUFU.{MUFU_FUNCS.get(bits(self.word, 20, 4), "?")}'
        return self.base

    @property
    def fixed(self) -> bool:
        return self.base not in VARIABLE

    def reads(self) -> dict[int, int]:
        """Give the GPRs this instruction reads by slot, RZ left out."""
        slots = {}
        if self.base in READS_A:
            slots[0] = bits(self.word, 8, 8)
        if self.name.endswith('_reg') or self.base == 'IPA':
            slots[1] = bits(self.word, 20, 8)
        if self.base in READS_C:
            slots[2] = bits(self.word, 39, 8)
        return {s: r for s, r in slots.items() if r != RZ}

    def writes(self) -> list[int]:
        if self.base in NO_DESTINATION or self.base in WRITES_PRED:
            return []
        d = bits(self.word, 0, 8)
        return [] if d == RZ else [d]


def decode(code: bytes) -> list[Decoded]:
    return [of(i) for i in unbundle(code)]


def of(instr: Instr) -> Decoded:
    return Decoded(isa.name(instr.word), instr.word, instr.control)


def highest_register(code: bytes) -> int:
    regs = [r for d in decode(code) for r in (*d.reads().values(), *d.writes())]
    return max(regs, default=-1)


def attributes(code: bytes) -> dict[int, int]:
    """Give the attribute addresses the code's IPAs read, each with its interpolation frequency."""
    return {bits(d.word, 28, 10): bits(d.word, 54, 2) for d in decode(code) if d.base == 'IPA'}
