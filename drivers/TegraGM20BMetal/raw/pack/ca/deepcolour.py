"""The colour precision this device's QuartzCore names its fixed-function states with: their fragment flag c.

QuartzCore's get_pipeline_spec (24B119 0x1843882b4-0x184388410, 21H461 0x18942f260-0x18942f4f8) sets a fixed-function
state's c (the full-precision colour function, fixed_frag_lph_cpf over fixed_frag_lph_cph) where the state lacks
h, where CADeviceUseCIF10 holds, or where one of its images' formats asks for it. CADeviceUseCIF10 is MobileGestalt's
HasExtendedColorDisplay, overridden by the CAEnableDeepFramebuffer preference where that is set; the stager sets it
false for every process (stage-tegra-gm20b-metal.py PREFS). Apple's offline compilation manifest names its states for
extended-colour devices, with c: without CADeviceUseCIF10 this device asks for each such state without c, and with c
only where the state has an image whose format sets it.
"""

import ast
from pathlib import Path

import caspec

PREFERENCE = 'CAEnableDeepFramebuffer'


def cif10(stager: Path) -> bool | None:
    """Give CADeviceUseCIF10 as the stager's preferences fix it, None where MobileGestalt's answer decides it."""
    for node in ast.parse(stager.read_text()).body:
        if isinstance(node, ast.Assign) and any(isinstance(t, ast.Name) and t.id == 'PREFS' for t in node.targets):
            return None if ast.literal_eval(node.value).get(PREFERENCE, True) else False
    raise ValueError(f'{stager} sets no PREFS: the preferences the device runs with are unknown')


def _flag_order(tables: caspec.Tables) -> str:
    letters = ''.join(row['letter'] for row in tables.t['name_tables']['fragment'])
    return letters[letters.index('X') + 1 :]


def fragment_with_flag(tables: caspec.Tables, fragment: str, letter: str, on: bool) -> str:
    """Give a name's fragment and image segments with its flag `letter` set or cleared, in QuartzCore's order."""
    head, *images = fragment.split('_')
    at = head.rindex('X')
    flags = set(head[at + 1 :]) - {letter} | ({letter} if on else set())
    return '_'.join([head[: at + 1] + ''.join(c for c in _flag_order(tables) if c in flags), *images])


def with_flag(tables: caspec.Tables, name: str, letter: str, on: bool) -> str:
    """Give `name` with its fragment segment's flag `letter` set or cleared."""
    pipeline, _, fragment = name.partition('_')
    return pipeline + '_' + fragment_with_flag(tables, fragment, letter, on)


def asked(tables: caspec.Tables, name: str, cif10_answer: bool | None) -> list[str]:
    """Give the names this device's QuartzCore gives the state another device's QuartzCore named `name`."""
    s = caspec.decode(tables, name)
    if not s.flag('h') or s.flag('g'):
        return [name]
    twin = with_flag(tables, name, 'c', not s.flag('c'))
    if cif10_answer is None:
        return [name, twin]
    if cif10_answer:
        return [twin] if not s.flag('c') else [name]
    if not s.flag('c'):
        return [name]
    return [twin, name] if tables.images(s.T) else [twin]
