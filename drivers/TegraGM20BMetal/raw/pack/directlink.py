#!/usr/bin/env python3
"""directlink: the raw pack (runtime/raw_pack.h) from the direct compile's records (directpack.mm), by mkpack's rules.

A record holds the 3D registers raw_emit makes for its state. Every other register is the context a draw meets: the
warm-up draw's registers (one probe draw: the context init and what NVK writes alike for every state), over which each
state's own are laid as the probe saw them. mkpack.py's split_registers then tells the per-pipeline registers from the
baseline, and the programs, constexpr samplers and records are laid out as mkpack.py lays out the probe's.

usage: directlink.py <out dir> <bucket dir>... (rawpack.py's buckets: records.bin; bucket 0's warm.out and warm.log; a
bucket's compute.out, compute.log and programs.bin where it probed kernels)
writes <out dir>/pack.bin, pack.json, layouts.txt, programs.bin (every render program, for nvdec) and report.txt
"""

import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mkpack
import packfile
import probelog

PIPELINE = struct.Struct('<QIHHIIBBBBHHI')
# SET_PIPELINE_PROGRAM(vertex B, pixel): where each program sits in the shader heap, which the probe saw differ by state
PROGRAM_OFFSETS = (0x2044, 0x2144)
PIPELINE_BYTES = PIPELINE.size + packfile.PACK_ENTRIES * 8 + 4 + 32 * 4
ENTRY_UBO, ENTRY_COLOR_TABLE = 1, 5
TEXTURE_KINDS = {2: 'slot', 3: 'fetch', 4: 'tex3d'}
SAMPLER_SLOT, SAMPLER_CONSTEXPR = 1, 2


class Reader:
    def __init__(self, data: bytes):
        self.data, self.at = data, 0

    def take(self, n: int) -> bytes:
        self.at += n
        return self.data[self.at - n : self.at]

    def u32(self) -> int:
        return struct.unpack('<I', self.take(4))[0]

    def blob(self) -> bytes:
        return self.take(self.u32())

    def done(self) -> bool:
        return self.at >= len(self.data)


def load_records(path: str) -> list[dict]:
    """Give a records.bin's states (directpack.mm's layout)."""
    with open(path, 'rb') as f:
        r = Reader(f.read())
    out = []
    while not r.done():
        if r.take(4) != b'DREC':
            raise ValueError(f'{path}: no record at byte {r.at - 4}')
        outcome = r.u32()
        rec = {'outcome': outcome, 'key': r.blob().decode(), 'why': r.blob().decode()}
        if outcome == 0:
            rec['pipeline'] = r.blob()
            rec['key_string'] = r.blob().decode()
            words = struct.unpack(f'<{len(b) // 4}I', b) if (b := r.blob()) else ()
            rec['regs'] = dict(zip(words[0::2], words[1::2], strict=True))
            rec['programs'] = [{'image': r.blob(), 'data': r.blob(), 'cbufs': list(r.blob())} for _ in range(2)]
            headers = r.blob()
            rec['samplers'] = [list(struct.unpack_from('<8I', headers, 32 * i)) for i in range(len(headers) // 32)]
        out.append(rec)
    return out


def program_id(program: dict) -> str:
    """Name a program by its header, code, shader data and constant buffer types (FNV-1a).

    One code can read its uniform buffers at other set-0 offsets in another state (its constant buffer types): each is
    a program of its own, so neither state binds the other's.
    """
    return f'{packfile.fnv(program["image"] + program["data"] + bytes(program["cbufs"])):x}'


def entries_of(pipeline: bytes, count: int, samplers: list[list[int]], constexpr_tsc: list[list[int]]) -> list[dict]:
    """Give a record's set-0 entries as mkpack.py's pipeline() makes them, constexpr samplers numbered in the pack's."""
    out = []
    for i in range(count):
        off, kind, stage, slot, sampler_kind, sampler, pad = struct.unpack_from(
            '<HBBBBBB', pipeline, PIPELINE.size + 8 * i
        )
        if kind == ENTRY_COLOR_TABLE:
            out.append({'off': off, 'kind': 'color_table'})
        elif kind == ENTRY_UBO:
            residual = sampler_kind >> 2
            out.append(
                {
                    'off': off,
                    'kind': 'ubo',
                    'stage': 'v' if stage == 1 else 'f',
                    'slot': slot,
                    'ssbo': bool(sampler_kind & 1),
                    'residual': 0x700 + 4 * (residual - 1) if residual else None,
                    'window': (sampler | pad << 8) * 16,
                }
            )
        else:
            entry = {'off': off, 'kind': 'handle', 'tex': (TEXTURE_KINDS[kind], slot) if kind else None, 'smp': None}
            if sampler_kind == SAMPLER_SLOT:
                entry['smp'] = ('slot', sampler)
            elif sampler_kind == SAMPLER_CONSTEXPR:
                header = samplers[sampler]
                if header not in constexpr_tsc:
                    constexpr_tsc.append(header)
                entry['smp'] = ('constexpr', constexpr_tsc.index(header))
            out.append(entry)
    return out


def pipeline_of(rec: dict, varying: list[int], prog_index: dict[str, int], constexpr_tsc: list[list[int]]) -> dict:
    """Give a state's pipeline as mkpack.py's pipeline() gives the probe's."""
    p = rec['pipeline']
    _hash, _key, _vs, _fs, _blob, _dwords, entries, _pushes, slot, _pad, stride, set_bytes, flags = (
        PIPELINE.unpack_from(p)
    )
    mask, *words = struct.unpack_from('<33I', p, PIPELINE.size + packfile.PACK_ENTRIES * 8)
    regs = rec['probe']['records'][0]['regs']
    return {
        'index': rec['probe']['index'],
        'key': rec['key'],
        'form': rec['probe']['form'],
        'vs': prog_index[program_id(rec['programs'][0])],
        'fs': prog_index[program_id(rec['programs'][1])],
        'blob': [
            (m, regs.get(m, 0))
            for m in varying
            if m not in PROGRAM_OFFSETS and not mkpack.foreign(m, mkpack.targets(rec['key']))
        ],
        'push': [(0x700 + 4 * i, words[i]) for i in range(32) if mask >> i & 1],
        'set_size': set_bytes,
        'entries': entries_of(p, entries, rec['samplers'], constexpr_tsc),
        'vbs': [{'stream': 0, 'slot': slot, 'stride': stride}] if slot != 0xFF else [],
        'fetch': bool(flags & 1),
        'clears': 0,
        'problems': [],
    }


def warm_registers(out_path: str, log_path: str) -> dict[int, int]:
    """Give the registers the warm-up probe's draw met (its context: every register a state does not set)."""
    for p in probelog.load(out_path, log_path):
        if p['records']:
            return p['records'][0]['regs']
    raise ValueError(f'{log_path}: no draw')


def gather(buckets: list[str], context: dict[int, int]) -> tuple[list[dict], list[dict]]:
    """Give the buckets' made states, each (form, key) once, their registers laid over `context`, and those refused."""
    states, seen, refused = [], set(), []
    for d in buckets:
        for rec in load_records(os.path.join(d, 'records.bin')):
            form = 'barrier' if rec['outcome'] == 0 and PIPELINE.unpack_from(rec['pipeline'])[12] & 2 else ''
            if (form, rec['key']) in seen:
                continue
            seen.add((form, rec['key']))
            if rec['outcome']:
                refused.append(rec)
                continue
            n = mkpack.targets(rec['key'])
            regs = dict(context)
            regs.update(rec['regs'])
            for method, program in zip(PROGRAM_OFFSETS, rec['programs'], strict=True):
                regs[method] = int(program_id(program), 16) & 0xFFFFFFFF
            regs = {m: 0 if mkpack.disabled_stream(m, v) else v for m, v in regs.items() if not mkpack.foreign(m, n)}
            ids = [program_id(program) for program in rec['programs']]
            rec['probe'] = {
                'index': len(states),
                'key': rec['key'],
                'form': form,
                'records': [{'regs': regs, 'prog': {1: (ids[0],), 5: (ids[1],)}}],
            }
            states.append(rec)
    return states, refused


def write_programs(path: str, programs: dict[str, dict]) -> None:
    """Write the programs as mkprograms.py does (nvdec names a bound program by them: test/eq.sh RAWCAP_PROGRAMS)."""
    with open(path, 'wb') as f:
        for p in programs.values():
            f.write(b'NVPG' + struct.pack('<III', len(p['image']) - 80, len(p['cbufs']), len(p['data'])))
            f.write(bytes(p['cbufs']) + p['image'] + p['data'])


def main() -> None:
    out_dir, *buckets = sys.argv[1:]
    warm = next(d for d in buckets if os.path.exists(os.path.join(d, 'warm.log')))
    warm_log = os.path.join(warm, 'warm.log')
    init, post = mkpack.load_init(warm_log)
    states, refused = gather(buckets, warm_registers(os.path.join(warm, 'warm.out'), warm_log))
    programs = {program_id(p): p for rec in states for p in rec['programs']}
    simple = [rec['probe'] for rec in states]
    report = [f'{len(states) + len(refused)} states made directly, {len(states)} records, {len(refused)} refused']
    varying, baseline = mkpack.split_registers(simple, post)
    report.append(f'{len(varying)} per-pipeline registers: ' + ' '.join(f'{m:x}' for m in varying))
    report.append(f'{len(baseline)} baseline registers over post-init')
    laid = mkpack.lay_out_programs(simple, programs, report)
    heap, prog_index, prog_recs = laid
    constexpr_tsc: list[list[int]] = []
    pipes = sorted((pipeline_of(rec, varying, prog_index, constexpr_tsc) for rec in states), key=packfile.key_hash)
    kernel_programs = dict(programs)
    for d in buckets:
        if os.path.exists(os.path.join(d, 'programs.bin')):
            kernel_programs.update(mkpack.load_programs(os.path.join(d, 'programs.bin')))
    computes = mkpack.compute_kernels(buckets, constexpr_tsc, kernel_programs, laid, report)
    init_recs = [(sc, m, d, mkpack.RELOC_CODE.get(mkpack.RELOC.get((sc, m)), 0)) for sc, m, d in init]
    pack = packfile.Pack(pipes, prog_recs, constexpr_tsc, baseline, init_recs, heap, computes)
    data, blob = packfile.pack_bytes(pack)
    os.makedirs(out_dir, exist_ok=True)
    with open(os.path.join(out_dir, 'pack.bin'), 'wb') as f:
        f.write(data)
    packfile.write_json(os.path.join(out_dir, 'pack.json'), pack, varying)
    packfile.write_layouts(os.path.join(out_dir, 'layouts.txt'), pack)
    write_programs(os.path.join(out_dir, 'programs.bin'), programs)
    report.append(
        f'pack.bin {len(data) // 1024} KiB: {len(pipes)} pipelines, {len(computes)} kernels, '
        f'{len(prog_recs)} programs, {len(constexpr_tsc)} constexpr samplers, {len(blob)} blob dwords, '
        f'{len(baseline)} baseline regs'
    )
    report += [f'  refused ({rec["why"]}): {rec["key"][:150]}' for rec in refused]
    with open(os.path.join(out_dir, 'report.txt'), 'w') as f:
        f.write('\n'.join(report) + '\n')
    print('\n'.join(line for line in report if not line.startswith('  refused')))


if __name__ == '__main__':
    main()
