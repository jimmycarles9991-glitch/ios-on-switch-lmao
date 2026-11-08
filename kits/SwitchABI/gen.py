"""Original<Class>.h and a driver's bindings from the ABI tables (interface/<Class>.json, kinds/<kind>/<Class>.json).

    gen.py header --interface I --table T --out Original<Class>.h
    gen.py bindings --pair I T [--pair I T]... --object O... --out bindings.S

The header declares the original class as the kind lays it out: its instance size, its virtuals in slot order (a slot
no driver names is opaque<N>), field accessors at the kind's offsets, the kind's constants, the members drivers call
and SWITCH_ABI_KIND (the kind's name, for messages), after the interface's declarations (the types its methods name
that no included header declares). A function without a "decl" is only bound: a C++ member the driver's own header
declares. A "guard" class's unnamed slots are pure, except the ones its table inherits (hints.inherit), and
SWITCH_ABI_GUARDS(trap) overrides each pure one in the subclass with trap(<slot>): no original code runs in a slot the
table did not choose. The bindings bind every symbol of the tables' addresses (only those the objects import, for an
interface with "bind": "imports"), and refuse an original import they lack, at its kernelcache address plus the
kind's prelink slide; an entry the table marks "unported" becomes a function that panics naming it.
"""

import argparse
import json
import re
import sys
from pathlib import Path

from tools.common import macho

JsonObject = dict[str, object]

M64 = (1 << 64) - 1
ACCESS = """#ifndef SWITCH_ABI_ACCESS
#define SWITCH_ABI_ACCESS
template <typename T> static inline T switch_abi_load(const void *object, unsigned long offset)
{
    T value;
    __builtin_memcpy(&value, static_cast<const unsigned char *>(object) + offset, sizeof(value));
    return value;
}
template <typename T> static inline void switch_abi_store(void *object, unsigned long offset, T value)
{
    __builtin_memcpy(static_cast<unsigned char *>(object) + offset, &value, sizeof(value));
}
#endif
"""


def load(path: str | Path) -> JsonObject:
    return json.loads(Path(path).read_text())


def number(value: str | int) -> int:
    return int(value, 0) if isinstance(value, str) else value


def mangled_class(name: str) -> str:
    return f'{len(name)}{name}'


def doc(entry: JsonObject, indent: str) -> list[str]:
    """Return an entry's "doc" as comment lines, wrapped at 120 columns."""
    words, lines, line = entry.get('doc', '').split(), [], indent + '//'
    for word in words:
        if len(line) + 1 + len(word) > 120:
            lines.append(line)
            line = indent + '//'
        line += ' ' + word
    return [*lines, line] if words else lines


def declaration(decl: str, indent: str) -> list[str]:
    """decl; wrapped after its commas at 120 columns."""
    text = indent + decl + ';'
    if len(text) <= 120 or '(' not in decl:
        return [text]
    head, _, rest = decl.partition('(')
    parts, lines, line = rest.split(', '), [], indent + head + '('
    pad = ' ' * len(line)
    for i, part in enumerate(parts):
        piece = part + (', ' if i + 1 < len(parts) else ';')
        if len(line) + len(piece.rstrip()) > 120 and line.strip():
            lines.append(line.rstrip())
            line = pad
        line += piece
    return [*lines, line]


def slot_declarations(interface: JsonObject, table: JsonObject) -> list[str]:
    """List the class body's virtuals: the interface's overrides of inherited slots, then every slot the class adds."""
    methods = interface['methods']
    first, count = table['first_slot'], table['slots']
    named = {int(slot): key for slot, key in table['layout'].items()}
    unknown = sorted(set(named.values()) - set(methods))
    if unknown:
        raise SystemExit(f'{table["class"]}: slots name methods the interface lacks: {unknown}')
    lines = []
    for slot, key in sorted(named.items()):
        if slot < first:
            lines += doc(methods[key], '    ') + declaration(methods[key]['decl'], '    ')
    guarded = guarded_slots(interface, table)
    for slot in range(first, count):
        if slot in named:
            lines += doc(methods[named[slot]], '    ') + declaration(methods[named[slot]]['decl'], '    ')
        elif slot in guarded:
            lines.append(f'    virtual void opaque{slot}() = 0;')
        else:
            lines.append(f'    SWITCH_ABI_OPAQUE({slot});')
    return lines


def guarded_slots(interface: JsonObject, table: JsonObject) -> list[int]:
    """Return a guard class's added slots the table neither names nor inherits: pure, and trapped by the subclass."""
    if not interface.get('guard'):
        return []
    first, count = table['first_slot'], table['slots']
    named = {int(slot) for slot in table['layout']}
    inherit = {int(slot) for slot in table.get('hints', {}).get('inherit', {})}
    stray = sorted(s for s in inherit if s in named or not first <= s < count)
    if stray:
        raise SystemExit(
            '{}: inherited slots that are named or not among the added ones: {}'.format(table['class'], stray)
        )
    return [s for s in range(first, count) if s not in named and s not in inherit]


def guards(interface: JsonObject, table: JsonObject) -> list[str]:
    """Return SWITCH_ABI_GUARDS(trap): the subclass's override of every guarded slot, calling trap(<slot>)."""
    slots = guarded_slots(interface, table)
    if not slots:
        return []
    body = [f'    void opaque{s}() override {{ trap({s}); }}' for s in slots]
    return ['#define SWITCH_ABI_GUARDS(trap) \\'] + [line + ' \\' for line in body[:-1]] + body[-1:]


def accessors(interface: JsonObject, table: JsonObject, *, public: bool) -> list[str]:
    lines = []
    for key, field in interface.get('fields', {}).items():
        if bool(field.get('public')) != public:
            continue
        offset = table['fields'].get(key)
        if offset is None:
            raise SystemExit(f'{table["class"]}: no offset for field {key}')
        kind, access = field['type'], field['access'].split()
        stored = 'uint8_t' if kind == 'bool' else kind
        setter = 'set' + key[0].upper() + key[1:]
        name = kind + ('' if kind.endswith('*') else ' ')
        lines += doc(field, '    ')
        if 'get' in access:
            value = f'switch_abi_load<{stored}>(this, {number(offset):#x})'
            if kind == 'bool':
                value += ' != 0'
            lines.append(f'    {name}{key}() const {{ return {value}; }}')
        if 'set' in access:
            lines.append(
                f'    void {setter}({kind} value) {{ switch_abi_store<{stored}>(this, {number(offset):#x}, value); }}'
            )
    return lines


def constant_declarations(interface: dict, table: dict) -> list[str]:
    """Declare the interface's constants as static members, at the kind's values."""
    lines = []
    for key, constant in interface.get('constants', {}).items():
        value = table.get('constants', {}).get(key)
        if value is None:
            raise SystemExit(f'{table["class"]}: no value for constant {key}')
        lines += [*doc(constant, '    '), f'    static constexpr unsigned long {key} = {number(value):#x};']
    return lines


def function_declarations(interface: JsonObject) -> list[str]:
    functions = [f for f in interface.get('functions', {}).values() if 'decl' in f]
    if not functions:
        return []
    out = ['', 'extern "C" {']
    for f in functions:
        out += doc(f, '') + declaration(f['decl'], '')
    return [*out, '}']


def constants_header(interface: JsonObject, table: JsonObject, table_name: str) -> str:
    """Declare a functions_only class, which has no layout drivers use, as Original<Class> holding its constants."""
    out = [f'// Generated by kits/SwitchABI/gen.py from {table_name}: do not edit.', *doc(interface, '')]
    out += ['#pragma once', '', f'#define SWITCH_ABI_KIND "{table["kind"]}"', '']
    out += [f'struct Original{interface["class"]} {{', *constant_declarations(interface, table), '};']
    return '\n'.join([*out, *function_declarations(interface)]) + '\n'


def header(interface: JsonObject, table: JsonObject, table_name: str) -> str:
    if interface.get('functions_only'):
        return constants_header(interface, table, table_name)
    cls, sup = interface['class'], interface['superclass']
    size = number(table['size'])
    structors = 'OSDeclareAbstractStructors' if interface['structors'] == 'abstract' else 'OSDeclareDefaultStructors'
    out = [f'// Generated by kits/SwitchABI/gen.py from {table_name}: do not edit.', *doc(interface, '')]
    out += ['#pragma once'] + [f'#include <{h}>' for h in interface['includes']]
    if interface.get('declarations'):
        out += [''] + interface['declarations']
    out += ['', ACCESS.rstrip('\n'), '', '#define SWITCH_ABI_KIND "{}"'.format(table['kind']), '']
    out.append(
        '#define SWITCH_ABI_OPAQUE(n) '
        'virtual __attribute__((deprecated("inherited original slot: never call"))) void opaque##n()'
    )
    out += ['', f'class {cls} : public {sup} {{', f'    {structors}({cls})']
    out.append(f'    unsigned char switchAbiStorage_[{size:#x} - sizeof({sup})];')
    fields = accessors(interface, table, public=False)
    if fields:
        out += ['', 'protected:', *fields]
    out += [
        '',
        'public:',
        *accessors(interface, table, public=True),
        *constant_declarations(interface, table),
        *slot_declarations(interface, table),
    ]
    out += [f'    {decl};' for decl in interface.get('members', [])]
    out += ['};', '#undef SWITCH_ABI_OPAQUE']
    out.append('static_assert(sizeof({}) == {:#x}, "{} as {} lays it out");'.format(cls, size, cls, table['kind']))
    trap = guards(interface, table)
    if trap:
        out += ['', *trap]
    return '\n'.join([*out, *function_declarations(interface)]) + '\n'


def symbol_map(interface: JsonObject, table: JsonObject) -> dict[str, tuple[int | None, str]]:
    """Mangled symbol -> (address or None for unported, what it is)."""
    cls = mangled_class(interface['class'])
    addresses = table['addresses']
    out = {}

    def put(symbol: str, key: str) -> None:
        if key in addresses:
            value = addresses[key]
            out[symbol] = (None if value == 'unported' else number(value), key)

    put(f'__ZN{cls}10gMetaClassE', 'gMetaClass')
    put(f'__ZNK{cls}12getMetaClassEv', 'getMetaClass')
    put(f'__ZTV{cls}', 'vtable')
    for structor, suffix in (('C2', 'C2EPK11OSMetaClass'), ('D2', 'D2Ev'), ('D1', 'D1Ev'), ('D0', 'D0Ev')):
        put(f'__ZN{cls}{suffix}', structor)
    for key, method in interface['methods'].items():
        if 'mangled' in method:
            put(method['mangled'], key)
    for key in addresses:
        opaque = re.fullmatch(r'opaque(\d+)', key)
        if opaque:
            put(f'__ZN{cls}{mangled_class(key)}Ev', key)
    for name, function in interface.get('functions', {}).items():
        for symbol in function.get('symbols', [name]):
            put('_' + symbol, name)
    return out


def undefined_symbols(path: str | Path) -> set[str]:
    data = Path(path).read_bytes()
    try:
        image = macho.macho(data, subtypes=None)
    except ValueError as error:
        raise SystemExit(f'{path}: not a 64-bit ARM64 Mach-O object ({error})') from None
    return {s.name for s in macho.symbols(data, image, macho.undefined_external)}


def bindings(pairs: list[tuple[JsonObject, JsonObject, str]], objects: list[str]) -> str:
    imports = set().union(*(undefined_symbols(o) for o in objects))
    lines, traps, sources = [], [], []
    for interface, table, table_name in pairs:
        cls = mangled_class(interface['class'])
        known = symbol_map(interface, table)
        slide = number(table['slide'])
        sources.append(table_name)
        ours = {s for s in imports if s in known or re.match(r'__Z(N|NK|TV)' + cls, s)}
        unknown = sorted(s for s in ours if s not in known)
        if unknown:
            raise SystemExit('{}: imports the table does not bind: {}'.format(table_name, ', '.join(unknown)))
        bound = sorted(s for s in known if s in ours) if interface.get('bind') == 'imports' else sorted(known)
        for symbol in bound:
            address, key = known[symbol]
            if address is None:
                traps.append(symbol)
            else:
                lines.append(f'.globl {symbol}\n.set {symbol}, {(address + slide) & M64:#x}  // {key}')
        imports -= set(ours)
    out = [f'// Generated by kits/SwitchABI/gen.py from {", ".join(sources)}: do not edit.', *lines]
    for i, symbol in enumerate(traps):
        out += [
            '.text',
            '.p2align 2',
            f'.globl {symbol}',
            f'{symbol}:',
            f'    adrp x0, Lswitch_abi_trap{i}@PAGE',
            f'    add x0, x0, Lswitch_abi_trap{i}@PAGEOFF',
            '    b _switch_abi_unported',
            '.section __TEXT,__cstring,cstring_literals',
            f'Lswitch_abi_trap{i}:',
            f'    .asciz "{symbol}"',
        ]
    return '\n'.join(out) + '\n'


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest='command', required=True)
    h = sub.add_parser('header')
    h.add_argument('--interface', required=True)
    h.add_argument('--table', required=True)
    h.add_argument('--out', required=True, type=Path)
    b = sub.add_parser('bindings')
    b.add_argument('--pair', nargs=2, action='append', required=True, metavar=('INTERFACE', 'TABLE'))
    b.add_argument('--object', action='append', required=True)
    b.add_argument('--out', required=True, type=Path)
    args = parser.parse_args()
    if args.command == 'header':
        args.out.write_text(header(load(args.interface), load(args.table), table_label(args.table)))
    else:
        pairs = [(load(i), load(t), table_label(t)) for i, t in args.pair]
        args.out.write_text(bindings(pairs, args.object))


def table_label(path: str) -> str:
    parts = Path(path).parts
    return '/'.join(parts[parts.index('kinds') :]) if 'kinds' in parts else path


if __name__ == '__main__':
    sys.exit(main())
