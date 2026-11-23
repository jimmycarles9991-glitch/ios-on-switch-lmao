"""gate.py --selectors: a dump's selector variants against their states' generic programs, and what each costs a pixel.

usage: gate.py --selectors <dump> [--invocations N] [--seed S] [--bound B]

A dump is directcheck's DIRECTCHECK_DUMP with DIRECTCHECK_SELECTORS (runtime/selector_variants.h): per state its
generic fragment program, its uniform bind residual words and, per config of its selector table, the config's value and
program. Each config runs both programs on the same seeded inputs with the config's compared bytes pinned at its
constant buffer from byte 0 and the residual words 0 (what a draw that selects it binds), bit for bit as gate.py
compares (`identical`), and counts the instructions each quad issued: every path a lane of it took, what a pixel costs.
A config fails where its outputs differ or its program issues more than B instructions a quad.
"""

import argparse
import json
import sys
from collections.abc import Callable
from pathlib import Path

from tools.gpu.maxwell import isa, nakstat


def pins(table: dict, value: bytes, residual_words: list[int]) -> dict:
    """Give the constant buffer words a draw selecting `value` binds.

    Each is (bank, byte): word, or (mask, value) where part of the word is the config's and the rest (its padding) any.
    """
    out: dict = {(0, at): 0 for at in residual_words}
    for at in range(0, table['length'], 4):
        mask = word = 0
        for i in range(4):
            byte = at + i
            if byte < table['length'] and table['compared'] >> byte & 1:
                mask |= 0xFF << 8 * i
                word |= value[byte] << 8 * i
        if mask:
            out[table['bank'], at] = word if mask == 0xFFFFFFFF else (mask, word)
    return out


def mean(counts: list[int]) -> float:
    return sum(counts) / len(counts) if counts else 0.0


def check_state(d: dict, compare: Callable, n: int, seed: int, bound: float) -> tuple[list[str], bool]:
    """Give a dumped state's lines, one per config, and whether every config passed."""
    table = d['selectors']
    generic = (bytes.fromhex(d['fs']), d.get('fs_bound_textures', 0) - 1)
    static = nakstat.row('generic', generic[0][isa.HEADER_BYTES :], d.get('fs_gprs', 0))['instructions']
    lines, passed = [], True
    if table['bank'] < 0:
        return [f'{d["key_hash"]}: its selector buffer is no constant buffer of the generic program'], False
    for i, config in enumerate(table['configs']):
        value = bytes.fromhex(config['value'])
        selected = (bytes.fromhex(config['code']), config.get('bound_textures', 0) - 1)
        issued: tuple = ([], [])
        residuals = table.get('residual_words', d.get('residual_words', []))
        verdict = compare(generic, selected, n, seed, pins(table, value, residuals), d['key'], issued)
        g, s = mean(issued[0]), mean(issued[1])
        size = nakstat.row('selected', selected[0][isa.HEADER_BYTES :], config.get('gprs', 0))['instructions']
        ok = verdict == 'identical' and s <= bound
        passed &= ok
        lines.append(
            f'{d["key_hash"]} config {i} {value.hex()}: {verdict}; issued a quad: generic {g:.0f}, selected {s:.0f} '
            f'({g / s if s else 0:.1f}x); static: generic {static}, selected {size}'
            + ('' if s <= bound else f'; over the bound of {bound:.0f}')
        )
    return lines, passed


def main(compare: Callable) -> None:
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('dump', type=Path)
    ap.add_argument('--invocations', type=int, default=64)
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--bound', type=float, default=630)
    a = ap.parse_args()
    failed = False
    configs = 0
    for f in sorted(a.dump.glob('*.json')):
        d = json.loads(f.read_text())
        if not d.get('selectors', {}).get('configs'):
            continue
        d['key_hash'] = f.stem
        lines, passed = check_state(d, compare, a.invocations, a.seed, a.bound)
        print('\n'.join(lines))
        failed |= not passed
        configs += len(d['selectors']['configs'])
    print(f'{configs} selector configs, {"some FAILED" if failed else "all identical within the bound"}')
    sys.exit(1 if failed or not configs else 0)
