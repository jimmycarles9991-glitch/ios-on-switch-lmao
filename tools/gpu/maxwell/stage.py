"""A vertex, tessellation or compute stage's environment for interp.py's lane (execute.run_lane's hook).

The instructions a fragment invocation never meets, with the field layouts NAK's sm50.rs encoder writes: attribute
loads and stores (ALD, AST) over the stage's input and output attribute words, the attribute buffer map (ISBERD: the
identity), system values (S2R) the caller names, global, local and shared memory (LDG, STG, LDL, STL, LDS, STS) over
byte arrays at addresses, IMAD and SHF. Every other instruction is execute.alu's. Nothing a stage does is a model: an
address no buffer holds, an attribute the caller gave no word for and a system value it did not name stop the run
(UnsupportedError), so a program that reads something the test did not set up never passes on a zero.
"""

import struct
from collections.abc import Callable
from dataclasses import dataclass, field

from tools.gpu.maxwell.execute import operand_b
from tools.gpu.maxwell.interp import Inputs, Lane, UnsupportedError, i20
from tools.gpu.maxwell.isa import field as bits

MEM_BYTES = {0: 1, 1: 1, 2: 2, 3: 2, 4: 4, 5: 8, 6: 16}  # sm50.rs set_mem_type: U8 I8 U16 I16 B32 B64 B128
SIGNED = {1: 8, 3: 16}


class Memory:
    """Byte arrays at GPU addresses; an access must lie within one."""

    def __init__(self) -> None:
        self.regions: list[tuple[int, bytearray]] = []

    def add(self, address: int, data: bytes) -> int:
        self.regions = [r for r in self.regions if r[0] != address]
        self.regions.append((address, bytearray(data)))
        return address

    def region(self, address: int) -> bytearray:
        for base, data in self.regions:
            if base <= address < base + len(data):
                return data
        raise UnsupportedError(f'no buffer at {address:#x}')

    def _at(self, address: int, size: int) -> tuple[bytearray, int]:
        for base, data in self.regions:
            if base <= address and address + size <= base + len(data):
                return data, address - base
        raise UnsupportedError(f'{size} bytes at {address:#x} past every buffer')

    def read(self, address: int, size: int) -> bytes:
        data, at = self._at(address, size)
        return bytes(data[at : at + size])

    def write(self, address: int, value: bytes) -> None:
        data, at = self._at(address, len(value))
        data[at : at + len(value)] = value

    def words(self, address: int, count: int) -> list[int]:
        return list(struct.unpack(f'<{count}I', self.read(address, 4 * count)))


@dataclass
class Stage:
    """One invocation's environment.

    `cbufs`: each constant buffer bank's bytes; `attribute(address, vertex, patch)`: an input attribute word (ALD of a
    vertex's or the patch's attributes); `system`: S2R's values by index; `outputs` and `patch_outputs` the words its
    AST wrote, by (vertex, attribute address) and by attribute address.
    """

    cbufs: dict[int, bytes]
    memory: Memory
    attribute: Callable[[int, int, bool], int]
    system: dict[int, int]
    outputs: dict = field(default_factory=dict)
    patch_outputs: dict = field(default_factory=dict)
    local: bytearray = field(default_factory=lambda: bytearray(0x10000))
    shared: bytearray = field(default_factory=lambda: bytearray(0x10000))

    def cbuf(self, bank: int, offset: int) -> int:
        data = self.cbufs.get(bank)
        if data is None or offset + 4 > len(data) or offset % 4:
            raise UnsupportedError(f'c{bank}[{offset:#x}] read where no constant buffer has it')
        return struct.unpack_from('<I', data, offset)[0]

    def inputs(self) -> Inputs:
        def no_ipa(_address: int) -> int:
            raise UnsupportedError('IPA in a stage of no interpolation')

        def no_texture(key: tuple) -> list[int]:
            raise UnsupportedError(f'a texture read {key[0]} in a stage of no textures')

        return Inputs(no_ipa, self.cbuf, no_texture)

    CLAIMED = ('ALD', 'AST', 'S2R', 'LDG', 'STG', 'LDL', 'STL', 'LDS', 'STS', 'IMAD', 'SHF', 'ISBERD', 'MEMBAR', 'BAR')

    def claims(self, name: str) -> bool:
        return name.split('_', 1)[0] in self.CLAIMED

    def execute(self, name: str, w: int, lane: Lane) -> None:
        base = name.split('_', 1)[0]
        if base == 'ALD':
            self.ald(w, lane)
        elif base == 'AST':
            self.ast(w, lane)
        elif base == 'S2R':
            index = bits(w, 20, 8)
            if index not in self.system:
                raise UnsupportedError(f'S2R {index:#x}, a system value the stage was not given')
            lane.w(bits(w, 0, 8), self.system[index])
        elif base in ('LDG', 'STG', 'LDL', 'STL', 'LDS', 'STS'):
            self.access(base, w, lane)
        elif base == 'IMAD':
            self.imad(name, w, lane)
        elif base == 'SHF':
            self.shf(name, w, lane)
        elif base == 'ISBERD':
            # the attribute buffer map of NAK's per-vertex input read (nak_nir_lower_vtg_io.c): its vertices in order
            lane.w(bits(w, 0, 8), lane.r(bits(w, 8, 8)))
        elif base == 'BAR' and bits(w, 32, 3) != 0:
            raise UnsupportedError('BAR other than a sync')

    def ald(self, w: int, lane: Lane) -> None:
        d, address = bits(w, 0, 8), bits(w, 20, 10) + lane.r(bits(w, 8, 8))
        vertex, patch, output = lane.r(bits(w, 39, 8)), bool(bits(w, 31, 1)), bool(bits(w, 32, 1))
        for k in range(bits(w, 47, 2) + 1):
            at = address + 4 * k
            if output:
                written = self.patch_outputs if patch else self.outputs
                where = at if patch else (vertex, at)
                if where not in written:
                    raise UnsupportedError(f'ALD.O {at:#x}, an output not yet written')
                lane.w(d + k, written[where])
            else:
                lane.w(d + k, self.attribute(at, vertex, patch))

    def ast(self, w: int, lane: Lane) -> None:
        data, address = bits(w, 0, 8), bits(w, 20, 10) + lane.r(bits(w, 8, 8))
        vertex, patch = lane.r(bits(w, 39, 8)), bool(bits(w, 31, 1))
        for k in range(bits(w, 47, 2) + 1):
            at = address + 4 * k
            if patch:
                self.patch_outputs[at] = lane.r(data + k)
            else:
                self.outputs[(vertex, at)] = lane.r(data + k)

    def access(self, base: str, w: int, lane: Lane) -> None:
        kind = bits(w, 48, 3)
        size = MEM_BYTES.get(kind)
        if size is None:
            raise UnsupportedError(f'{base} of memory type {kind}')
        offset = bits(w, 20, 24)
        offset -= (1 << 24) if offset & (1 << 23) else 0
        a = bits(w, 8, 8)
        if base in ('LDG', 'STG') and bits(w, 45, 1):
            address = lane.r(a) | lane.r(a + 1) << 32 if a != 255 else 0
        else:
            address = lane.r(a)
        address = (address + offset) & 0xFFFFFFFFFFFFFFFF
        store = base[0] == 'S'
        r = bits(w, 0, 8)
        if store:
            value = b''.join(struct.pack('<I', lane.r(r + k)) for k in range(max(1, size // 4)))[:size]
            self.space(base, address, size, value)
            return
        loaded = self.space(base, address, size)
        if size < 4:
            v = int.from_bytes(loaded, 'little')
            width = SIGNED.get(kind)
            if width and v >> (width - 1):
                v -= 1 << width
            lane.w(r, v)
            return
        for k in range(size // 4):
            lane.w(r + k, struct.unpack_from('<I', loaded, 4 * k)[0])

    def space(self, base: str, address: int, size: int, value: bytes | None = None) -> bytes:
        if base in ('LDG', 'STG'):
            if value is not None:
                self.memory.write(address, value)
                return value
            return self.memory.read(address, size)
        array = self.local if base in ('LDL', 'STL') else self.shared
        if address + size > len(array):
            raise UnsupportedError(f'{base} at {address:#x} past its memory')
        if value is not None:
            array[address : address + size] = value
            return value
        return bytes(array[address : address + size])

    def imad(self, name: str, w: int, lane: Lane) -> None:
        """Execute IMAD: d = a * b + c, its low 32 bits (sm50.rs OpIMad).

        Signed sources at bits 48 and 53, a negated product and addend at 51 and 52; b and c as execute.ffma_operands
        reads them.
        """
        a = lane.r(bits(w, 8, 8))
        if name.endswith('_rc'):
            b, c = lane.r(bits(w, 39, 8)), self.cbuf(bits(w, 34, 5), bits(w, 20, 14) * 4)
        elif name.endswith('_cr'):
            b, c = self.cbuf(bits(w, 34, 5), bits(w, 20, 14) * 4), lane.r(bits(w, 39, 8))
        else:
            b, c = operand_b(lane, w, name, self.inputs(), i20), lane.r(bits(w, 39, 8))
        if bits(w, 49, 2) or bits(w, 54, 2):
            raise UnsupportedError('IMAD of a field NAK leaves 0 (.HI, .SAT, .X)')

        def signed(v: int, on: int) -> int:
            return v - (1 << 32) if on and v & 0x80000000 else v

        product = signed(a, bits(w, 48, 1)) * signed(b, bits(w, 53, 1))
        product = -product if bits(w, 51, 1) else product
        lane.w(bits(w, 0, 8), product + (-c if bits(w, 52, 1) else c))

    def shf(self, name: str, w: int, lane: Lane) -> None:
        """Execute SHF, a funnel shift of {high, low} (sm50.rs OpShf).

        .L gives the shifted pair's high word, .R its low word (its high word with .HI, bit 48); a 64-bit type (bits
        37-38: 2 U64, 3 I64) shifts by up to 63, a 32-bit one by up to 32, the count wrapped (bit 50) or clamped.
        """
        if bits(w, 47, 1) or bits(w, 49, 1):
            raise UnsupportedError('SHF .CC or .X')
        kind = bits(w, 37, 2)
        if kind == 1:
            raise UnsupportedError(f'SHF of data type {kind}')
        wide = kind >= 2
        n = operand_b(lane, w, name, self.inputs(), i20)
        limit = 64 if wide else 32
        n = n & (limit - 1) if bits(w, 50, 1) else min(n, limit)
        pair = lane.r(bits(w, 39, 8)) << 32 | lane.r(bits(w, 8, 8))
        if '_r_' in name:
            if kind == 3 and pair >> 63:
                pair -= 1 << 64
            shifted = (pair >> n) & 0xFFFFFFFFFFFFFFFF
            lane.w(bits(w, 0, 8), shifted >> 32 if bits(w, 48, 1) else shifted)
        else:
            lane.w(bits(w, 0, 8), (pair << n) >> 32)
