"""The committed ABI tables agree with their interfaces, and gen.py lays out headers and bindings from them."""

import struct
import tempfile
import unittest
from pathlib import Path

from kits.SwitchABI import gen

ROOT = Path(__file__).resolve().parents[1]


def tables():
    for path in sorted((ROOT / 'kinds').glob('*/*.json')):
        yield path, gen.load(ROOT / 'interface' / path.name), gen.load(path)


def macho_object(undefined: list[str]) -> bytes:
    """Build a minimal MH_OBJECT whose symbol table holds only these undefined external symbols."""
    strings = b'\0' + b''.join(s.encode() + b'\0' for s in undefined)
    symoff = 32 + 24
    stroff = symoff + 16 * len(undefined)
    head = struct.pack('<IiiIIIII', 0xFEEDFACF, 0x0100000C, 0, 1, 1, 24, 0, 0)
    symtab = struct.pack('<6I', 2, 24, symoff, len(undefined), stroff, len(strings))
    nlist, at = b'', 1
    for s in undefined:
        nlist += struct.pack('<IBBHQ', at, 0x01, 0, 0, 0)
        at += len(s) + 1
    return head + symtab + nlist + strings


INTERFACE = {
    'class': 'Base',
    'superclass': 'IOService',
    'structors': 'abstract',
    'includes': ['IOKit/IOService.h'],
    'methods': {
        'stop': {'decl': 'void stop(IOService *provider) override', 'mangled': '__ZN4Base4stopEP9IOService'},
        'kick': {'decl': 'virtual void kick() = 0'},
    },
    'fields': {'armed': {'type': 'bool', 'access': 'get set'}},
    'functions': {
        'switch_base_check': {'decl': 'void switch_base_check(Base *base)'},
        'helperReset': {'symbols': ['_ZN6Helper5resetEv']},
    },
}
TABLE = {
    'class': 'Base',
    'kind': 'TEST',
    'size': '0x100',
    'first_slot': 168,
    'slots': 171,
    'slide': '-0x1000',
    'layout': {'87': 'stop', '169': 'kick'},
    'fields': {'armed': '0x90'},
    'addresses': {
        'gMetaClass': '0xfffffff007000000',
        'stop': '0xfffffff006000000',
        'opaque168': '0xfffffff006000100',
        'opaque170': 'unported',
        'switch_base_check': '0xfffffff006000200',
        'getMetaClass': '0xfffffff006000300',
        'helperReset': '0xfffffff006000400',
    },
}


class TablesTest(unittest.TestCase):
    def test_every_table_matches_its_interface(self):
        for path, interface, table in tables():
            with self.subTest(table=str(path.relative_to(ROOT))):
                self.assertEqual(table['class'], interface['class'])
                self.assertEqual(table['kind'], path.parent.name)
                self.assertTrue(set(table['layout'].values()) <= set(interface['methods']))
                self.assertEqual(set(table['fields']), set(interface.get('fields', {})))
                self.assertEqual(set(table.get('constants', {})), set(interface.get('constants', {})))
                self.assertLessEqual(table['first_slot'], table['slots'])
                if interface.get('header', True):
                    gen.header(interface, table, path.name)

    def test_one_slide_per_kind(self):
        slides = {}
        for _path, _, table in tables():
            slides.setdefault(table['kind'], set()).add(table['slide'])
        self.assertEqual({k: len(v) for k, v in slides.items()}, dict.fromkeys(slides, 1))

    def test_every_virtual_the_class_adds_has_a_slot(self):
        for path, interface, table in tables():
            with self.subTest(table=str(path.relative_to(ROOT))):
                added = {k for k, m in interface['methods'].items() if not m['decl'].endswith(' override')}
                self.assertLessEqual(added, set(table['layout'].values()))


class HeaderTest(unittest.TestCase):
    def test_slots_in_order_with_opaque_fillers(self):
        text = gen.header(INTERFACE, TABLE, 'kinds/TEST/Base.json')
        body = text[text.index('public:') :]
        self.assertLess(body.index('void stop('), body.index('SWITCH_ABI_OPAQUE(168)'))
        self.assertLess(body.index('SWITCH_ABI_OPAQUE(168)'), body.index('virtual void kick() = 0'))
        self.assertLess(body.index('virtual void kick() = 0'), body.index('SWITCH_ABI_OPAQUE(170)'))
        self.assertIn('static_assert(sizeof(Base) == 0x100', text)
        self.assertIn('#define SWITCH_ABI_KIND "TEST"', text)
        self.assertIn('switch_abi_load<uint8_t>(this, 0x90) != 0', text)

    def test_declarations_between_the_includes_and_the_class(self):
        interface = dict(INTERFACE, declarations=['struct BaseKey {', '    unsigned bits;', '};'])
        text = gen.header(interface, TABLE, 'kinds/TEST/Base.json')
        self.assertLess(text.index('#include <IOKit/IOService.h>'), text.index('struct BaseKey {\n    unsigned bits;'))
        self.assertLess(text.index('struct BaseKey {'), text.index('class Base : public IOService {'))

    def test_constants_take_the_kind_values(self):
        interface = dict(INTERFACE, constants={'swapNext': {'doc': 'the next link'}})
        text = gen.header(interface, dict(TABLE, constants={'swapNext': '0xb90'}), 'kinds/TEST/Base.json')
        self.assertIn('    static constexpr unsigned long swapNext = 0xb90;', text)
        with self.assertRaises(SystemExit):
            gen.header(interface, TABLE, 'kinds/TEST/Base.json')

    def test_a_functions_only_class_is_a_struct_of_its_constants(self):
        interface = {
            'class': 'Table',
            'functions_only': True,
            'methods': {},
            'constants': {'entries': {'doc': 'the entry array'}},
            'functions': {'switch_table_find': {'decl': 'void *switch_table_find(unsigned id)'}},
        }
        text = gen.header(interface, {'kind': 'TEST', 'constants': {'entries': '0x100'}}, 'kinds/TEST/Table.json')
        body = text[text.index('struct OriginalTable {') : text.index('};')]
        self.assertIn('    static constexpr unsigned long entries = 0x100;', body)
        self.assertIn('void *switch_table_find(unsigned id);', text[text.index('extern "C"') :])

    def test_a_function_without_a_declaration_is_not_declared(self):
        text = gen.header(INTERFACE, TABLE, 'kinds/TEST/Base.json')
        self.assertIn('void switch_base_check(Base *base);', text)
        self.assertNotIn('reset', text)

    def test_a_slot_the_interface_lacks_is_refused(self):
        table = dict(TABLE, layout={'168': 'missing'})
        with self.assertRaises(SystemExit):
            gen.header(INTERFACE, table, 'kinds/TEST/Base.json')


GUARDED = dict(INTERFACE, guard=True, fields={}, methods={'kick': {'decl': 'virtual void kick() = 0'}})
GUARDED_TABLE = dict(TABLE, layout={'169': 'kick'}, fields={}, hints={'inherit': {'170': 'the original runs it'}})


class GuardTest(unittest.TestCase):
    def test_unnamed_slots_are_pure_and_inherited_ones_bound(self):
        text = gen.header(GUARDED, GUARDED_TABLE, 'kinds/TEST/Base.json')
        body = text[text.index('public:') : text.index('};')]
        self.assertIn('    virtual void opaque168() = 0;', body)
        self.assertIn('    SWITCH_ABI_OPAQUE(170);', body)
        self.assertNotIn('opaque169', body)

    def test_the_guard_macro_traps_each_pure_slot(self):
        text = gen.header(GUARDED, dict(GUARDED_TABLE, slots=172), 'kinds/TEST/Base.json')
        start = text.index('#define SWITCH_ABI_GUARDS(trap)')
        macro = text[start : text.index('\n', text.index('trap(171)'))]
        self.assertEqual(
            macro.splitlines()[1:],
            ['    void opaque168() override { trap(168); } \\', '    void opaque171() override { trap(171); }'],
        )

    def test_inheriting_a_named_slot_is_refused(self):
        table = dict(GUARDED_TABLE, hints={'inherit': {'169': 'named'}})
        with self.assertRaises(SystemExit):
            gen.header(GUARDED, table, 'kinds/TEST/Base.json')

    def test_no_guard_no_macro(self):
        self.assertNotIn('SWITCH_ABI_GUARDS', gen.header(INTERFACE, TABLE, 'kinds/TEST/Base.json'))


NUB = {
    'class': 'Nub',
    'superclass': 'IOService',
    'structors': 'default',
    'metaclass_only': True,
    'includes': ['IOKit/IOService.h'],
    'methods': {},
    'fields': {'owner': {'type': 'IOService *', 'access': 'get', 'public': True}},
    'functions': {'switch_nub_read': {'decl': 'uint32_t switch_nub_read(Nub *nub)'}},
}
NUB_TABLE = {
    'class': 'Nub',
    'kind': 'TEST',
    'size': '0x740',
    'first_slot': 168,
    'slots': 168,
    'slide': '-0x1000',
    'layout': {},
    'fields': {'owner': '0x88'},
    'addresses': {'gMetaClass': '0xfffffff007000000', 'switch_nub_read': '0xfffffff006000000'},
}


class MetaclassOnlyTest(unittest.TestCase):
    def test_public_fields_and_functions_without_slots(self):
        text = gen.header(NUB, NUB_TABLE, 'kinds/TEST/Nub.json')
        public = text[text.index('public:') : text.index('};')]
        self.assertIn('IOService *owner() const { return switch_abi_load<IOService *>(this, 0x88); }', public)
        self.assertNotIn('protected:', text)
        self.assertNotIn('SWITCH_ABI_OPAQUE(', public)
        self.assertIn('uint32_t switch_nub_read(Nub *nub);', text[text.index('extern "C"') :])

    def test_its_functions_and_metaclass_are_bound(self):
        with tempfile.TemporaryDirectory() as tmp:
            obj = Path(tmp) / 'a.o'
            obj.write_bytes(macho_object(['_switch_nub_read']))
            text = gen.bindings([(NUB, NUB_TABLE, 'kinds/TEST/Nub.json')], [str(obj)])
        self.assertIn('.set _switch_nub_read, 0xfffffff005fff000', text)
        self.assertIn('.set __ZN3Nub10gMetaClassE, 0xfffffff006fff000', text)


class BindingsTest(unittest.TestCase):
    def bind(self, undefined: list[str]) -> str:
        with tempfile.TemporaryDirectory() as tmp:
            obj = Path(tmp) / 'a.o'
            obj.write_bytes(macho_object(undefined))
            return gen.bindings([(INTERFACE, TABLE, 'kinds/TEST/Base.json')], [str(obj)])

    def test_every_table_address_is_bound_with_the_slide(self):
        text = self.bind(['__ZN4Base4stopEP9IOService'])
        self.assertIn('.set __ZN4Base4stopEP9IOService, 0xfffffff005fff000', text)
        self.assertIn('.set __ZN4Base10gMetaClassE, 0xfffffff006fff000', text)
        self.assertIn('.set _switch_base_check, 0xfffffff005fff200', text)
        self.assertIn('.set __ZNK4Base12getMetaClassEv, 0xfffffff005fff300', text)
        self.assertIn('.set __ZN6Helper5resetEv, 0xfffffff005fff400', text)

    def test_unported_becomes_a_named_trap(self):
        text = self.bind(['__ZN4Base9opaque170Ev'])
        self.assertIn('__ZN4Base9opaque170Ev:', text)
        self.assertIn('b _switch_abi_unported', text)
        self.assertIn('.asciz "__ZN4Base9opaque170Ev"', text)

    def test_bind_imports_binds_only_what_the_objects_import(self):
        interface = dict(INTERFACE, bind='imports')
        with tempfile.TemporaryDirectory() as tmp:
            obj = Path(tmp) / 'a.o'
            obj.write_bytes(macho_object(['__ZN4Base4stopEP9IOService']))
            text = gen.bindings([(interface, TABLE, 'kinds/TEST/Base.json')], [str(obj)])
        self.assertIn('.set __ZN4Base4stopEP9IOService, 0xfffffff005fff000', text)
        self.assertNotIn('gMetaClass', text)
        self.assertNotIn('opaque170', text)

    def test_an_import_the_table_lacks_is_refused(self):
        with self.assertRaises(SystemExit):
            self.bind(['__ZN4Base7unknownEv'])


if __name__ == '__main__':
    unittest.main()
