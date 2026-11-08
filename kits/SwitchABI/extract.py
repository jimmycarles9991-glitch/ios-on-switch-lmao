"""kinds/<kind>/<Class>.json from a fileset (arm64e) kernelcache: what can be read, the rest from the table's hints.

    extract.py --kernelcache KC --ipsw IPSW [--kind K] [--class C]... [--check]
    bazel run [--config=ios27] //kits/SwitchABI:extract     (rewrites the selected kind's tables)
    bazel run [--config=ios27] //kits/SwitchABI:verify      (compares them)

The kind is the one whose pinned kernelcache sha256 (ipsw/pins.bzl) the kernelcache has.

Read from the kernelcache: the instance size, gMetaClass and the vtable (the metaclass initialisers), the slot count,
each interface method's slot (its PAC discriminator, from Apple's mangled root name), every other slot the class adds
(opaque<N>, or a refusal if it is pure: a new contract), the C2 constructor and D2 destructor (the functions storing
the vtable), and every function a hint locates by a string it references or by its first words at an address. Hints
are what no reading can name: a renamed method's slot, the string that finds a function, a field's offset or a
constant with the instruction that shows it (checked here). A "guard" class's subclass keeps its superclass's slots
and traps the added ones, so neither its overrides of inherited slots nor its pure ones need declaring, and only the
added slots the table inherits (hints.inherit) get an address. The same kernelcache always gives the same JSON;
--check compares instead of writing.

A kernelcache that is not a fileset (iOS 17's arm64 prelinked kernel, unstripped of nothing but without PAC) cannot
name slots; its tables are checked instead: the vtable has the table's slot count, every bound slot holds the
table's address, and getMetaClass returns the table's gMetaClass (for a table that binds the vtable); each function hint
with a body holds those words at the address the table binds.
"""

import argparse
import collections
import hashlib
import json
import os
import sys
from collections.abc import Callable
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import kernelcache

from tools.common.macho import backed_offset, macho

HERE = Path(__file__).parent
JsonObject = dict[str, object]


# Slots OSDeclare*Structors fills for every class: the D1 and D0 destructors and getMetaClass.
STRUCTOR_SLOTS = {0, 1, 7}


def pure_virtual(fs: kernelcache.Fileset) -> int:
    """___cxa_pure_virtual: the kernel function most slots a class adds (beyond its superclass's) point at."""
    by_meta = {info['meta']: name for name, info in fs.inheritance.items()}
    targets = collections.Counter()
    for cls, info in fs.inheritance.items():
        parent = by_meta.get(info['parent'])
        if cls in fs.classes and parent in fs.classes:
            first = len(fs.classes[parent]['methods'])
            targets.update(t for i, t, _ in fs.slots(cls) if i >= first and fs.owner(t) == 'com.apple.kernel')
    return targets.most_common(1)[0][0]


def pac_root(method: JsonObject) -> str:
    return method.get('pac') or method['mangled'][1:]


def layout(
    fs: kernelcache.Fileset, interface: JsonObject, table: JsonObject, pure: int
) -> tuple[int, list[kernelcache.Slot], dict[int, str], dict[str, str], list[str]]:
    cls = interface['class']
    first = len(fs.classes[interface['superclass']]['methods'])
    parent = {i: t for i, t, _ in fs.slots(interface['superclass'])}
    slots = fs.slots(cls)
    by_pac = collections.defaultdict(list)
    for i, _, pac in slots:
        by_pac[pac].append(i)
    hints = {int(k): v for k, v in table.get('hints', {}).get('layout', {}).items()}
    guard = interface.get('guard')
    named, found = {}, {}
    for key, method in interface['methods'].items():
        hit = [i for i in by_pac.get(kernelcache.discriminator(pac_root(method)), []) if i not in hints]
        if len(hit) == 1:
            named[hit[0]] = key
            found[key] = f'slot {hit[0]} by PAC of {pac_root(method)}'
    for slot, key in hints.items():
        named[slot] = key
        found[key] = f'slot {slot} by hint'
    errors = []
    for i, target, _ in slots:
        own = parent.get(i) != target
        key = named.get(i)
        if i < first and own and key is None and i not in STRUCTOR_SLOTS and not guard:
            errors.append(
                'slot {} overrides {} at {:#x}; the interface must declare it'.format(
                    i, interface['superclass'], target
                )
            )
        if i < first and key is not None and not own:
            del named[i]
            found.pop(key)
        if i >= first and target == pure and key is None and not guard:
            errors.append(f'slot {i} is a pure virtual the interface does not declare')
        if key is not None and i < len(slots) and (target == pure) != interface['methods'][key]['decl'].endswith('= 0'):
            errors.append('{} (slot {}) is {}pure in the kernelcache'.format(key, i, '' if target == pure else 'not '))
    errors += uninheritable(table, slots, first, named, pure)
    missing = sorted(set(interface['methods']) - set(named.values()))
    errors += [
        f'{k}: no slot (no PAC match and no hint)'
        for k in missing
        if interface['methods'][k]['decl'].endswith('= 0') or 'override' not in interface['methods'][k]['decl']
    ]
    return first, slots, named, found, errors


def uninheritable(
    table: JsonObject, slots: list[kernelcache.Slot], first: int, named: dict[int, str], pure: int
) -> list[str]:
    """Refuse each hints.inherit slot that is not an added, unnamed slot with code of its own."""
    targets = {i: t for i, t, _ in slots}
    inherit = sorted(int(k) for k in table.get('hints', {}).get('inherit', {}))
    return [
        f'slot {i} cannot be inherited: not an added slot with code of its own'
        for i in inherit
        if i < first or i in named or targets.get(i, pure) == pure
    ]


def structors(fs: kernelcache.Fileset, cls: str) -> dict[str, int]:
    """Find C2 and the D2 right after it.

    C2 is the first of the constructors that store the vtable after calling the superclass's; D2 is a bare branch to
    the superclass's D2.
    """
    info = fs.inheritance[cls]
    lo, hi = fs.text(info['bundle'])
    words = fs.words(lo, hi)
    stores = sorted({fs.function(pc) for pc, a in kernelcache.references(words) if a == info['vtable']})
    plain = [f for f in stores if f[1] - f[0] == min(e - s for s, e in stores)]
    if not plain:
        return {}
    c2 = plain[0][0]
    starts = fs.function_starts(info['bundle'])
    after = starts[starts.index(plain[-1][0]) + 1]
    body = [w for _, w in fs.words(*fs.function(after)) if w != 0xD503245F]
    out = {'C2': c2}
    if body and kernelcache.branch(after, body[0]) and not kernelcache.branch(after, body[0])[1]:
        out['D2'] = after
    return out


def locate(fs: kernelcache.Fileset, bundle: str, hint: JsonObject) -> int:
    """Return the function referencing a string (hint "string": its first characters), then hint "up" callers up."""
    lo, hi = fs.text(bundle)
    words = fs.words(lo, hi)
    strings = set(fs.strings(hint['string']))
    hits = sorted({fs.function(pc)[0] for pc, a in kernelcache.references(words) if a in strings})
    if len(hits) != 1:
        raise SystemExit(f'{hint["string"]!r}: {len(hits)} functions reference it')
    function = hits[0]
    for _ in range(hint.get('up', 0)):
        callers = sorted(
            {
                fs.function(pc)[0]
                for pc, w in words
                if kernelcache.branch(pc, w) and kernelcache.branch(pc, w)[0] == function
            }
        )
        if len(callers) != 1:
            raise SystemExit(f'{function:#x}: {len(callers)} callers')
        function = callers[0]
    return function


BTI_C = 0xD503245F

Body = Callable[[int, int], list[int] | None]


def fileset_body(fs: kernelcache.Fileset) -> Body:
    """Read the first words (BTI landing pads skipped) of the function starting at an address, or None if none does."""

    def body(at: int, count: int) -> list[int] | None:
        start, end = fs.function(at)
        return [w for _, w in fs.words(start, end) if w != BTI_C][:count] if start == at else None

    return body


def plain_body(word: Callable[[int], int]) -> Body:
    """Read the words at an address of a kernelcache that has no function starts (iOS 17's)."""
    return lambda at, count: [word(at + 4 * i) for i in range(count)]


def functions(body: Body, find: Callable[[dict], int], table: dict) -> tuple[dict, dict]:
    """Locate hints.functions: the one path for every kind and table.

    An entry is "unported", a function whose code starts as described ("at", "body", "why"), or the one function
    referencing a string ("string", "up" callers up), which find() locates.
    """
    addresses, found = {}, {}
    for key, hint in sorted(table.get('hints', {}).get('functions', {}).items()):
        if hint == 'unported':
            addresses[key], found[key] = 'unported', 'not located in this kind yet'
        elif 'at' in hint:
            at, want = int(hint['at'], 16), [int(w, 16) for w in hint['body'].split()]
            got = body(at, len(want))
            if got != want:
                words = 'no function starts there' if got is None else ' '.join(f'{w:08x}' for w in got)
                raise SystemExit(f'{key} at {at:#x}: {words}; the hint says {hint["body"]}')
            addresses[key], found[key] = hex(at), hint['why']
        else:
            addresses[key] = hex(find(hint))
            up = f', {hint["up"]} caller(s) up' if hint.get('up') else ''
            found[key] = f'references "{hint["string"]}"{up}'
    return addresses, found


def fileset_functions(fs: kernelcache.Fileset, bundle: str, table: dict) -> tuple[dict, dict]:
    return functions(fileset_body(fs), lambda hint: locate(fs, bundle, hint), table)


def fields(fs: kernelcache.Fileset, table: JsonObject) -> tuple[dict, dict]:
    out, found = {}, {}
    for key, hint in sorted(table.get('hints', {}).get('fields', {}).items()):
        at, word = int(hint['at'], 16), int(hint['word'], 16)
        if fs.word(at) != word:
            raise SystemExit(f'field {key}: the word at {at:#x} is {fs.word(at):08x}, not {word:08x}')
        out[key] = hint['offset']
        found[key] = f'offset shown by {word:08x} at {at:#x}'
    return out, found


def constants(fs: kernelcache.Fileset, table: dict) -> tuple[dict, dict]:
    """Read hints.constants: each value with the instruction word that shows it, checked like a field's."""
    out, found = {}, {}
    for key, hint in sorted(table.get('hints', {}).get('constants', {}).items()):
        at, word = int(hint['at'], 16), int(hint['word'], 16)
        if fs.word(at) != word:
            raise SystemExit(f'constant {key}: the word at {at:#x} is {fs.word(at):08x}, not {word:08x}')
        out[key] = hint['value']
        found[key] = f'value shown by {word:08x} at {at:#x}'
    return out, found


def functions_only_table(fs: kernelcache.Fileset, cls: str, table: JsonObject, kind: str) -> str:
    info = fs.inheritance[cls]
    addresses, found = fileset_functions(fs, info['bundle'], table)
    values, value_found = constants(fs, table)
    found.update(value_found)
    out = {
        'class': cls,
        'kind': kind,
        'size': hex(info['size']),
        'first_slot': 0,
        'slots': len(fs.slots(cls)),
        'slide': table['slide'],
        'layout': {},
        'fields': {},
    }
    if values:
        out['constants'] = values
    out.update(addresses=addresses, found=dict(sorted(found.items())), hints=table.get('hints', {}))
    return dump(out)


def extract(fs: kernelcache.Fileset, interface: JsonObject, table: JsonObject, kind: str, pure: int) -> str:
    cls = interface['class']
    info = fs.inheritance[cls]
    if interface.get('functions_only'):
        return functions_only_table(fs, cls, table, kind)
    if interface.get('metaclass_only'):
        addresses, found = fileset_functions(fs, info['bundle'], table)
        addresses['gMetaClass'], found['gMetaClass'] = hex(info['meta']), 'metaclass initialiser'
        offsets, field_found = fields(fs, table)
        found.update(field_found)
        return dump(
            {
                'class': cls,
                'kind': kind,
                'size': hex(info['size']),
                'first_slot': len(fs.classes[interface['superclass']]['methods']),
                'slots': len(fs.slots(cls)),
                'slide': table['slide'],
                'layout': {},
                'fields': offsets,
                'addresses': dict(sorted(addresses.items())),
                'found': dict(sorted(found.items())),
                'hints': table.get('hints', {}),
            }
        )
    first, slots, named, found, errors = layout(fs, interface, table, pure)
    if errors:
        raise SystemExit(f'{cls} ({kind}):\n  ' + '\n  '.join(errors))
    targets = {i: t for i, t, _ in slots}
    addresses = {'gMetaClass': info['meta'], 'vtable': info['vtable'] - 16}
    found.update(gMetaClass='metaclass initialiser', vtable='metaclass initialiser')
    for slot, key in named.items():
        if targets[slot] != pure:
            addresses[key] = targets[slot]
    inherit = {int(k) for k in table.get('hints', {}).get('inherit', {})}
    for i, target, _ in slots:
        if i >= first and i not in named and (not interface.get('guard') or i in inherit):
            addresses[f'opaque{i}'] = target
    for key, address in structors(fs, cls).items():
        addresses[key] = address
        found[key] = 'stores the vtable' if key == 'C2' else 'branch after the constructors'
    located, located_found = fileset_functions(fs, info['bundle'], table)
    addresses.update({k: int(v, 16) for k, v in located.items() if v != 'unported'})
    unported = sorted(k for k, v in located.items() if v == 'unported')
    found.update(located_found)
    offsets, field_found = fields(fs, table)
    found.update(field_found)
    values, value_found = constants(fs, table)
    found.update(value_found)
    out = {
        'class': cls,
        'kind': kind,
        'size': hex(info['size']),
        'first_slot': first,
        'slots': len(slots),
        'slide': table['slide'],
        'layout': {str(i): named[i] for i in sorted(named)},
        'fields': offsets,
    }
    if values:
        out['constants'] = values
    out.update(
        addresses={k: hex(v) for k, v in sorted(addresses.items(), key=lambda kv: (kv[1], kv[0]))}
        | dict.fromkeys(unported, 'unported'),
        found=dict(sorted(found.items())),
        hints=table.get('hints', {}),
    )
    return dump(out)


def dump(table: JsonObject) -> str:
    return json.dumps(table, indent=2) + '\n'


def check_plain(path: Path, interface: dict, table: dict) -> None:
    data = path.read_bytes()
    segments = macho(data)['segments']

    def read(va: int, size: int) -> int:
        return int.from_bytes(data[backed_offset(segments, va, size, len(data)) :][:size], 'little')

    addresses = {k: int(v, 16) for k, v in table['addresses'].items() if v != 'unported'}
    located, _ = functions(plain_body(lambda va: read(va, 4)), plain_string_hint, table)
    errors = [
        f'{key}: the hint locates {value}, the table binds {table["addresses"].get(key)}'
        for key, value in located.items()
        if bound(value) != bound(table['addresses'].get(key))
    ]
    if 'vtable' in addresses:
        code = [(g['va'], g['va'] + g['size']) for g in segments if g['initprot'] & 4]
        errors += plain_vtable_errors(read, code, addresses, table)
    errors += plain_constant_errors(lambda va: read(va, 4), table)
    if errors:
        joined = '\n  '.join(errors)
        raise SystemExit(f'{interface["class"]} ({table["kind"]}):\n  {joined}')


def plain_vtable_errors(
    read: Callable[[int, int], int], code: list[tuple[int, int]], addresses: dict[str, int], table: dict
) -> list[str]:
    """Check that the vtable has the slot count, every bound slot its address and getMetaClass the gMetaClass."""
    vtable = addresses['vtable'] + 16
    entries = [read(vtable + 8 * i, 8) for i in range(table['slots'] + 1)]
    errors = []
    if not all(any(lo <= e < hi for lo, hi in code) for e in entries[:-1]) or any(
        lo <= entries[-1] < hi for lo, hi in code
    ):
        errors.append(f'the vtable does not hold exactly {table["slots"]} code slots')
    for slot, key in table['layout'].items():
        if key in addresses and entries[int(slot)] != addresses[key]:
            errors.append(f'slot {slot} holds {entries[int(slot)]:#x}, the table binds {key} at {addresses[key]:#x}')
    if getter_target(read, entries[7]) != addresses['gMetaClass']:
        errors.append("getMetaClass does not return the table's gMetaClass")
    return errors


def plain_constant_errors(word: Callable[[int], int], table: dict) -> list[str]:
    """Check each hints.constants entry: the instruction word at its address and the value the table holds."""
    errors, values = [], table.get('constants', {})
    for key, hint in sorted(table.get('hints', {}).get('constants', {}).items()):
        at, want = int(hint['at'], 16), int(hint['word'], 16)
        if word(at) != want:
            errors.append(f'constant {key}: the word at {at:#x} is {word(at):08x}, not {want:08x}')
        if values.get(key) != hint['value']:
            errors.append(f'constant {key}: the hint shows {hint["value"]}, the table holds {values.get(key)}')
    return errors


def bound(value: str | None) -> int | str | None:
    return value if value in (None, 'unported') else int(value, 16)


def plain_string_hint(hint: dict) -> int:
    raise SystemExit(f'"{hint["string"]}": a string hint needs a fileset kernelcache')


def getter_target(read: Callable[[int, int], int], getter: int) -> int | None:
    """Return the address getMetaClass forms with ADRP then ADD (after an unoptimised kext's stack spill, if any)."""
    words = [read(getter + 4 * i, 4) for i in range(6)]
    for i, adrp in enumerate(words[:-1]):
        if (adrp & 0x9F000000) == 0x90000000:
            pc, add = getter + 4 * i, words[i + 1]
            page = (pc & ~0xFFF) + (kernelcache.sx(((adrp >> 5) & 0x7FFFF) << 2 | (adrp >> 29) & 3, 21) << 12)
            return page + ((add >> 10) & 0xFFF)
    return None


def is_fileset(path: Path) -> bool:
    with open(path, 'rb') as f:
        return int.from_bytes(f.read(16)[12:16], 'little') == 12


def kind_of(kernelcache: Path, pins: Path) -> str:
    scope = {}
    exec(compile(Path(pins).read_text(), str(pins), 'exec'), {'True': True, 'False': False}, scope)
    digest = hashlib.sha256(Path(kernelcache).read_bytes()).hexdigest()
    kinds = [k for k, v in scope['IPSWS'].items() if v.get('derived', {}).get('kernelcache_sha256') == digest]
    if len(kinds) != 1:
        raise SystemExit(f'{kernelcache}: no pinned IPSW kind has this kernelcache')
    return kinds[0]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--kind')
    parser.add_argument('--pins', type=Path, default=HERE.parents[1] / 'ipsw/pins.bzl')
    parser.add_argument('--kernelcache', required=True, type=Path)
    parser.add_argument('--ipsw', required=True)
    parser.add_argument('--class', dest='classes', action='append')
    workspace = os.environ.get('BUILD_WORKSPACE_DIRECTORY')
    parser.add_argument(
        '--tables', type=Path, default=Path(workspace) / 'kits/SwitchABI/kinds' if workspace else HERE / 'kinds'
    )
    parser.add_argument('--check', action='store_true', help='compare with the committed tables instead of writing')
    args = parser.parse_args()
    kind = kind_of(args.kernelcache, args.pins)
    if args.kind and args.kind != kind:
        raise SystemExit(f'the kernelcache is {kind}, not {args.kind}: build with that kind (--config)')
    args.kind = kind
    directory = args.tables / args.kind
    classes = args.classes or sorted(p.stem for p in directory.glob('*.json'))
    if not is_fileset(args.kernelcache):
        for cls in classes:
            check_plain(
                args.kernelcache,
                json.loads((HERE / 'interface' / (cls + '.json')).read_text()),
                json.loads((directory / (cls + '.json')).read_text()),
            )
        return
    fs = kernelcache.Fileset(args.kernelcache, args.ipsw)
    pure = pure_virtual(fs)
    stale = []
    for cls in classes:
        path = directory / (cls + '.json')
        table = json.loads(path.read_text())
        text = extract(fs, json.loads((HERE / 'interface' / (cls + '.json')).read_text()), table, args.kind, pure)
        if args.check:
            if path.read_text() != text:
                stale.append(cls)
        else:
            path.write_text(text)
    if stale:
        raise SystemExit(f'kinds/{args.kind}: differs from the kernelcache: {", ".join(stale)}')


if __name__ == '__main__':
    main()
