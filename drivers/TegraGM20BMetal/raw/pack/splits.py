#!/usr/bin/env python3
"""Fragment programs split for the raw path (Mesa nvk_switch_split.h): their place in the pack.

A split moves what a fragment program computes from per-draw values alone into a compute prologue, which writes those
values (the frontier) for the per-pixel program to read from a constant buffer of its own. The direct compile makes a
state's split with its record (directpack.mm); directlink.py packs the split of each packed state's fragment program
(runtime/raw_pack.h PackSplit).
"""

import kernels
import packfile

PROGRAM_CBUFS = packfile.PROGRAM_CBUFS
QMD_CBUFS = 8
FRONTIER = 6  # runtime/raw_pack.h CbufFrontier
MAX_FRONTIER_BYTES = 4096  # runtime/raw_split_metadata.h usableSplitMetadata


def usable_frontier(s: dict) -> bool:
    """Whether the runtime takes the split's frontier: 16-byte steps up to 4 KiB (bound in whole 256-byte units)."""
    return 0 < s.get('frontier_bytes', 0) <= MAX_FRONTIER_BYTES and s['frontier_bytes'] % 16 == 0


def of_record(record: bytes, fs: str, prologue: str) -> dict:
    """Give a split from its PackSplit as the direct compile made it (no program indices yet) and its programs' ids."""
    fields = packfile.PACK_SPLIT.unpack(record)
    frontier_bytes, frontier_cbuf, ubos, shader_data_cbuf, prologue_cbufs, ubo = fields[4:10]
    return {
        'fs': fs,
        'prologue': prologue,
        'frontier_bytes': frontier_bytes,
        'frontier_cbuf': frontier_cbuf,
        'prologue_cbufs': prologue_cbufs,
        'shader_data_cbuf': shader_data_cbuf,
        'ubo_cbufs': list(ubo[:ubos]),
        'threshold': fields[0],
        'batch_version': fields[10],
        'batch_threads': fields[11],
        'qmd': list(fields[12:76]),
        'cost': dict(zip(packfile.SPLIT_COST_FIELDS, fields[76:], strict=True)),
    }


def instruction_slots(program: dict) -> int:
    """Give a program's instruction slots: its code past the header, every fourth 8-byte word a scheduling word."""
    return (len(program['image']) - 80) // 32 * 3


def binds_as_whole(s: dict, whole: str, programs: dict[str, dict]) -> bool:
    """Whether the split's per-pixel program binds its constant buffers as `whole` does, its frontier after them.

    A uniform buffer's type holds its set-0 offset (raw_pack.h cbufUboOffset): the twin binds the per-pixel program's
    buffers from its state's set-0 entries, so a type the whole program does not name binds another buffer.
    """
    types = programs[whole]['cbufs']
    return s['frontier_cbuf'] == len(types) and programs[s['fs']]['cbufs'] == [*types, FRONTIER]


def lay_out(
    used: list[str], found: dict[str, dict], programs: dict[str, dict], laid: tuple, report: list[str]
) -> list[dict]:
    """Give the splits of the fragment programs `used`, their programs appended to the heap image `laid` holds."""
    heap, prog_index, prog_recs = laid
    out, before, after = [], 0, 0
    for whole in used:
        s = found.get(whole)
        if not s or s['fs'] not in programs or s['prologue'] not in programs:
            continue
        fs = s['fs']
        if len(programs[fs]['cbufs']) > PROGRAM_CBUFS or s['prologue_cbufs'] > QMD_CBUFS:
            continue
        if not binds_as_whole(s, whole, programs):
            continue
        if not usable_frontier(s):
            report.append(f'{whole}: split left out, a frontier of {s.get("frontier_bytes")} bytes the runtime refuses')
            continue
        packfile.place_graphics(fs, programs, heap, prog_index, prog_recs)
        kernel = {'prog': s['prologue']}
        kernels.lay_out([kernel], programs, heap, prog_index, prog_recs)
        out.append(dict(s, whole=prog_index[whole], fs=prog_index[fs], prologue=kernel['program']))
        before += instruction_slots(programs[whole])
        after += instruction_slots(programs[fs])
    out.sort(key=lambda s: s['whole'])
    report.append(
        f'{len(out)} fragment programs split: {before} -> {after} instruction slots'
        + (f' ({100 * (after - before) / before:+.1f}%)' if before else '')
    )
    return out
