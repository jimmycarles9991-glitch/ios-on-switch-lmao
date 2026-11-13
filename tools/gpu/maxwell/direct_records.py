"""directrun's records (direct/directrun.mm) bound as the raw path binds them, for stage.py.

A draw's or dispatch's root table (NVK's nvk_root_descriptor_table: a kernel's group count, set 0's address and size,
the push words at 0x700), set 0's buffer descriptors (raw/runtime/raw_residual.h bufferDescriptor, each buffer bound on
its base alignment so no residual word is written), and each program's constant buffer banks by its nvk_cbuf_type
(raw_gpu.cpp bindProgramCbufs, raw_compute.cpp qmdWords). A program that reads a bank, an entry or a buffer the test
did not give stops its run.
"""

import struct

from tools.gpu.maxwell import isa
from tools.gpu.maxwell.execute import run_lane
from tools.gpu.maxwell.stage import Memory, Stage

ROOT_BYTES, ROOT_SETS, ROOT_PUSH = 0x800, 0x38, 0x700
CBUF_ROOT, CBUF_SHADER_DATA, CBUF_SET = 1, 2, 3
ENTRY_UBO, ENTRY_FETCH, ENTRY_TESS_BUFFER = 1, 3, 11
KERNEL_BUFFER = 0
SET_ADDRESS = 0xF0_0000_0000
ROOT_ADDRESS = 0xF1_0000_0000
BUFFER_BASE = 0x2_0000_0000 - 0x100
MASK = 0xFFFFFFFF


def f32(x: float) -> int:
    return struct.unpack('<I', struct.pack('<f', x))[0]


def value(word: int) -> float:
    return struct.unpack('<f', struct.pack('<I', word))[0]


def floats(*xs: float) -> bytes:
    return struct.pack(f'<{len(xs)}f', *xs)


def code(program: dict, header: bool = True) -> bytes:
    image = bytes.fromhex(program.get('image', program.get('code', '')))
    return image[isa.HEADER_BYTES :] if header else image


def descriptor_range(storage: bool, window: int, size: int) -> int:
    """Give a buffer descriptor's range for `size` bytes on its base alignment (raw_residual.h bufferDescriptor)."""
    span = (size + 15) & ~15
    if not storage:
        span = min(span, 0x10000)
    if window and window < span:
        span = window
    return (span + 255) & ~255 if not storage else span


class Binding:
    """The buffers a draw or a dispatch binds, by key ((stage, slot), or ('tess', which)), and its root table and set 0.

    Each buffer starts 256 bytes below a multiple of 4 GiB, so an address a program computes past its first 256 bytes
    carries into the high word (its IADD.CC / IADD.X pairs). A buffer's memory runs to its descriptor's range and on to
    the next 256 bytes, zeros past its data, as a GPU allocation's pages do: a program may read a word past the bytes
    bound and within its range.
    """

    def __init__(self) -> None:
        self.memory = Memory()
        self.at: dict = {}
        self.next = BUFFER_BASE
        self.root = self.set0 = b''

    def tables(self, root: bytes, set0: bytes) -> None:
        self.root, self.set0 = root, set0
        self.memory.add(SET_ADDRESS, set0)
        self.memory.add(ROOT_ADDRESS, root)

    def bind(self, key: object, data: bytes) -> int:
        address = self.next
        self.memory.add(address, data + bytes(((len(data) + 255) & ~255) + 256 - len(data)))
        self.at[key] = (address, len(data))
        self.next += 1 << 32
        return address

    def descriptor(self, key: object, storage: bool, window: int) -> bytes:
        if key not in self.at:
            raise ValueError(f'the record reads buffer {key}, which the test binds none at')
        address, size = self.at[key]
        return struct.pack('<IIII', address & MASK, address >> 32, descriptor_range(storage, window, size), 0)


def render_set(record: dict, binding: Binding, attachments: dict[int, int] | None = None) -> bytes:
    """Give set 0: each buffer's descriptor, and each framebuffer fetch's header index of `attachments` by color."""
    data = bytearray(max(256, record['set_bytes']))
    for offset, kind, stage, slot, sampler_kind, sampler, pad in record['entries']:
        if kind == ENTRY_FETCH and attachments is not None and slot in attachments:
            struct.pack_into('<I', data, offset, attachments[slot])
            continue
        if kind == ENTRY_UBO:
            key = (stage, slot)
        elif kind == ENTRY_TESS_BUFFER:
            key = ('tess', slot)
        else:
            raise ValueError(f'a set-0 entry of kind {kind} the test bound nothing at')
        data[offset : offset + 16] = binding.descriptor(key, bool(sampler_kind & 1), (sampler | pad << 8) * 16)
    return bytes(data)


def kernel_set(record: dict, binding: Binding) -> bytes:
    data = bytearray(max(256, (record['set_bytes'] + 255) & ~255))
    for offset, kind, slot, window in record['entries']:
        if kind != KERNEL_BUFFER:
            raise ValueError(f'a kernel entry of kind {kind}: these kernels bind no texture')
        data[offset : offset + 16] = binding.descriptor((3, slot), True, window & 0xFFFFFF)
    return bytes(data)


def root_table(push: dict[int, int], set_bytes: int, words: dict[int, int] | None = None) -> bytes:
    """Give the root table: `words` by byte offset below the push words, set 0 at ROOT_SETS, push word i at 0x700+4i."""
    root = bytearray(ROOT_BYTES)
    for offset, word in (words or {}).items():
        struct.pack_into('<I', root, offset, word & MASK)
    struct.pack_into('<III', root, ROOT_SETS, SET_ADDRESS & MASK, SET_ADDRESS >> 32, set_bytes)
    for i, word in push.items():
        struct.pack_into('<I', root, ROOT_PUSH + 4 * i, word & MASK)
    return bytes(root)


def put(data: bytearray, field: list[int], value: int, base: int = 0) -> None:
    """Write `value` into `field` ([byte offset, bytes] of a directrun layout) of the structure at `base`."""
    offset, size = field
    struct.pack_into({4: '<I', 8: '<Q'}[size], data, base + offset, value & ((1 << 8 * size) - 1))


def counter_records(layout: dict, records: list[tuple[int, int]]) -> bytes:
    """Give the counter samples' records, each (payload, PTIMER units), laid out as raw_counters.h Record."""
    shape = layout['record']
    data = bytearray(shape['bytes'] * len(records))
    for i, (payload, units) in enumerate(records):
        put(data, shape['payload'], payload, i * shape['bytes'])
        put(data, shape['units'], units, i * shape['bytes'])
    return bytes(data)


def resolve_params(layout: dict, count: int) -> bytes:
    data = bytearray(layout['params']['bytes'])
    put(data, layout['params']['count'], count)
    return bytes(data)


def clock_anchors(layout: dict, published: int, slots: dict[int, tuple[int, int]]) -> bytes:
    """Give the kernel's clock anchor table of `published` anchors: each slot's (units, ns) by its index."""
    table, anchor = layout['anchors'], layout['anchor']
    data = bytearray(table['bytes'])
    put(data, table['published'], published)
    for slot, (units, ns) in slots.items():
        base = table['slot'] + slot * anchor['bytes']
        put(data, anchor['units'], units, base)
        put(data, anchor['ns'], ns, base)
    return bytes(data)


def record_push(record: dict) -> dict[int, int]:
    """Give the record's constant push words, by word."""
    return {i: record['push_words'][i] for i in range(32) if record['push_mask'] >> i & 1}


def banks(program: dict, root: bytes, set0: bytes) -> dict[int, bytes]:
    data = bytes.fromhex(program['data'])
    kinds = {CBUF_ROOT: root, CBUF_SHADER_DATA: data, CBUF_SET: set0}
    return {bank: kinds[kind] for bank, kind in enumerate(program['cbufs']) if kind in kinds}


def stage(program: dict, binding: Binding, attribute: object, system: dict) -> Stage:
    return Stage(banks(program, binding.root, binding.set0), binding.memory, attribute, system)


def execute(program: dict, environment: Stage, header: bool = True) -> Stage:
    run_lane(code(program, header), environment.inputs(), environment)
    return environment
