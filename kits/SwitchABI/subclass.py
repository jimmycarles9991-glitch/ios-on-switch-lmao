"""Checks a driver's subclass of an original class against the selected kind's table (switch_abi_bindings runs it).

    subclass.py vtable --interface I --table T --subclass S [--own NAME]... --object O... --out OK
    subclass.py late --object O --late L --out OK

vtable: the object that defines S's vtable is read through its relocations. The vtable has exactly the table's slot
count and no ___cxa_pure_virtual. Each slot holding one of S's functions names a method in --own (D1, D0 and
getMetaClass always are). Every other slot holds the original's symbol for that slot: the interface method the
table's layout names there, opaque<N> past the superclass's slots, and below them something other than the original
class's (whose overrides the layout names). No object imports the original's D0 or D1 (an abstract base's).
late: a late object (its static initializer staged) has the object's __text and no __mod_init_func.
"""

import argparse
import re
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import gen

from tools.common import macho

STRUCTORS = {'D1', 'D0', 'getMetaClass'}

Section = tuple[str, str, int, bytes, list[tuple[int, int]]]
Symbol = tuple[str, int, int]


def read_object(path: str) -> tuple[list[Section], list[Symbol]]:
    """Read a 64-bit Mach-O object's sections (segment, name, addr, data, relocations) and symbols."""
    data = Path(path).read_bytes()
    try:
        image = macho.macho(data, subtypes=None)
    except ValueError as error:
        raise SystemExit(f'{path}: not a 64-bit ARM64 Mach-O object ({error})') from None
    sections = [
        (
            s['segment'],
            s['name'],
            s['va'],
            data[s['fileoff'] : s['fileoff'] + s['size']],
            [struct.unpack_from('<iI', data, s['reloff'] + 8 * r) for r in range(s['nreloc'])],
        )
        for segment in image['segments']
        for s in segment['sections']
    ]
    return sections, [(s.name, s.type, s.value) for s in macho.symbols(data, image)]


def defines(symbols: list[Symbol], name: str) -> bool:
    return any(n == name and kind & 0x0E == 0x0E for n, kind, _ in symbols)


def vtable_entries(sections: list[Section], symbols: list[Symbol], name: str) -> list[str]:
    """Name the symbols of a vtable's entries (ARM64_RELOC_UNSIGNED, 8 bytes, external), from its first entry on."""
    address = next(v for s, kind, v in symbols if s == name and kind & 0x0E == 0x0E)
    for _, _, addr, data, relocations in sections:
        if addr <= address < addr + len(data):
            named = {
                addr + offset: symbols[info & 0xFFFFFF][0]
                for offset, info in relocations
                if info >> 27 & 1 and info >> 28 == 0 and info >> 25 & 3 == 3
            }
            entries = []
            while address + 16 + 8 * len(entries) in named:
                entries.append(named[address + 16 + 8 * len(entries)])
            return entries
    raise SystemExit(f'{name}: in no section')


def method_name(symbol: str, cls: str) -> str | None:
    """Name the method a symbol of class cls is (D1/D0 for its destructors), or None for another class's."""
    m = re.match(rf'__ZNK?{len(cls)}{cls}(?:(\d+)|(D[012]|C[12]))', symbol)
    if not m:
        return None
    if m.group(2):
        return m.group(2)
    return symbol[m.end() : m.end() + int(m.group(1))]


def original_symbol(interface: dict, table: dict, slot: int) -> str | None:
    """Give the symbol the table puts in a slot, or None below the superclass's slots where the layout names none."""
    cls = interface['class']
    key = table['layout'].get(str(slot))
    if key is not None:
        return interface['methods'][key]['mangled']
    if slot >= table['first_slot']:
        opaque = f'opaque{slot}'
        return f'__ZN{len(cls)}{cls}{len(opaque)}{opaque}Ev'
    return None


def slot_errors(interface: dict, table: dict, subclass: str, own: list[str], entries: list[str]) -> list[str]:
    errors, left = [], set(own) | STRUCTORS
    for slot, symbol in enumerate(entries):
        ours = method_name(symbol, subclass)
        want = original_symbol(interface, table, slot)
        if symbol == '___cxa_pure_virtual':
            errors.append(f'slot {slot} is pure')
        elif ours is not None:
            if ours not in set(own) | STRUCTORS:
                errors.append(f'slot {slot} is {subclass}::{ours}, which --own does not name')
            left.discard(ours)
        elif want is not None and symbol != want:
            errors.append(f'slot {slot} holds {symbol}, the table has {want} there')
        elif want is None and method_name(symbol, interface['class']) is not None:
            errors.append(f'slot {slot} holds {symbol}, which the table does not name there')
    if left - STRUCTORS:
        errors.append('--own names methods no slot holds: ' + ', '.join(sorted(left - STRUCTORS)))
    return errors


def check_vtable(interface: dict, table: dict, subclass: str, own: list[str], objects: list[str]) -> None:
    cls = interface['class']
    found = [(o, read_object(o)) for o in objects]
    name = f'__ZTV{len(subclass)}{subclass}'
    holders = [image for _, image in found if defines(image[1], name)]
    if len(holders) != 1:
        raise SystemExit(f'{subclass}: {len(holders)} objects define its vtable')
    entries = vtable_entries(*holders[0], name)
    errors = []
    if len(entries) != table['slots']:
        errors.append(f'{len(entries)} slots, the table has {table["slots"]}')
    errors += slot_errors(interface, table, subclass, own, entries)
    errors += [
        f"{Path(o).name} calls the abstract original's {n}"
        for o, (_, symbols) in found
        for n, kind, _ in symbols
        if kind == 1 and method_name(n, cls) in ('D0', 'D1')
    ]
    if errors:
        raise SystemExit(f'{subclass} on {cls} ({table["kind"]}):\n  ' + '\n  '.join(errors))


def check_late(plain: str, late: str) -> None:
    def text(sections: list[Section]) -> bytes:
        return b''.join(data for _, name, _, data, _ in sections if name == '__text')

    plain_sections, late_sections = read_object(plain)[0], read_object(late)[0]
    if text(plain_sections) != text(late_sections):
        raise SystemExit(f'{late}: staging the static initializer changed instructions')
    if any(name == '__mod_init_func' for _, name, _, _, _ in late_sections):
        raise SystemExit(f'{late}: still has an automatic initializer')


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest='command', required=True)
    v = sub.add_parser('vtable')
    v.add_argument('--interface', required=True)
    v.add_argument('--table', required=True)
    v.add_argument('--subclass', required=True)
    v.add_argument('--own', action='append', default=[])
    v.add_argument('--object', action='append', required=True)
    v.add_argument('--out', required=True, type=Path)
    late = sub.add_parser('late')
    late.add_argument('--object', required=True)
    late.add_argument('--late', required=True)
    late.add_argument('--out', required=True, type=Path)
    args = parser.parse_args()
    if args.command == 'vtable':
        check_vtable(gen.load(args.interface), gen.load(args.table), args.subclass, args.own, args.object)
    else:
        check_late(args.object, args.late)
    args.out.write_text('ok\n')


if __name__ == '__main__':
    main()
