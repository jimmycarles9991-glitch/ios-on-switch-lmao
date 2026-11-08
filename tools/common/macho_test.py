import struct
import unittest

from tools.common import macho

UUID = bytes(range(16))


def image(subtype: int = 0, symbols: list[tuple[bytes, int, int]] = (), fileset: bool = False) -> bytes:
    """Make a Mach-O: one __TEXT segment with a __text section, a symbol table, an LC_UUID, a fileset entry."""
    names = b'\0' + b''.join(name + b'\0' for name, _, _ in symbols)
    section = struct.pack(
        '<16s16sQQIIIIIIII', b'__text', b'__TEXT', 0x1000, 0x10, 0x400, 2, 0x500, 3, 0x80000400, 0, 0, 0
    )
    segment = struct.pack('<II16s4Q4I', 0x19, 152, b'__TEXT', 0x1000, 0x1000, 0, 0x1000, 5, 5, 1, 0) + section
    entry_name = b'com.apple.kernel\0'
    entry = struct.pack('<IIQQII', 0x80000035, 0, 0x1000, 0, 32, 0) + entry_name
    entry = entry + bytes(-len(entry) % 8)
    entry = entry[:4] + struct.pack('<I', len(entry)) + entry[8:]
    commands = [segment, struct.pack('<II16s', 0x1B, 24, UUID)] + ([entry] if fileset else [])
    symtab_at = 32 + sum(map(len, commands)) + 24
    symoff = symtab_at + 8 - symtab_at % 8
    stroff = symoff + 16 * len(symbols)
    commands.append(struct.pack('<6I', 0x2, 24, symoff, len(symbols), stroff, len(names)))
    sizeofcmds = sum(map(len, commands))
    head = struct.pack('<8I', 0xFEEDFACF, 0x100000C, subtype, 12 if fileset else 2, len(commands), sizeofcmds, 0, 0)
    data = bytearray(head + b''.join(commands))
    data += bytes(symoff - len(data))
    at = 1
    for name, kind, value in symbols:
        data += struct.pack('<IBBHQ', at, kind, 1, 0, value)
        at += len(name) + 1
    return bytes(data + names + bytes(max(0, 0x1000 - len(data) - len(names))))


SYMBOLS = [
    (b'_local', 0x0E, 0x1000),
    (b'_exported', 0x0F, 0x1004),
    (b'_absolute', 0x03, 0x2000),
    (b'_imported', 0x01, 0),
    (b'_stab', 0x24, 0x1008),
    (b'_also_exported', 0x0F, 0x1004),
]


class Reader(unittest.TestCase):
    def test_segments_sections_and_commands(self):
        data = image(symbols=SYMBOLS)
        img = macho.macho(data)
        self.assertEqual((img['kind'], img['cpusubtype']), (macho.MH_EXECUTE, 0))
        self.assertEqual([c for c, _, _ in img['commands']], [0x19, 0x1B, 0x2])
        (text,) = img['segments']
        self.assertEqual((text['name'], text['va'], text['fileoff'], text['filesize']), ('__TEXT', 0x1000, 0, 0x1000))
        (section,) = text['sections']
        self.assertEqual(section['name'], '__text')
        self.assertEqual(section['segment'], '__TEXT')
        self.assertEqual((section['va'], section['size'], section['fileoff']), (0x1000, 0x10, 0x400))
        self.assertEqual((section['reloff'], section['nreloc'], section['flags']), (0x500, 3, 0x80000400))
        self.assertEqual(data[macho.uuid_offset(img) : macho.uuid_offset(img) + 16], UUID)
        self.assertEqual(macho.backed_offset(img['segments'], 0x1010, 8, len(data)), 0x10)

    def test_symbols_and_exports(self):
        data = image(symbols=SYMBOLS)
        img = macho.macho(data)
        everything = macho.symbols(data, img)
        self.assertEqual([s.name for s in everything], [n.decode() for n, _, _ in SYMBOLS])
        self.assertEqual([s.name for s in macho.symbols(data, img, macho.undefined_external)], ['_imported'])
        self.assertEqual(macho.symbols(data, img, lambda _kind: False), [])
        self.assertEqual(macho.exports(data, img), {0x1004: ['_exported', '_also_exported'], 0x2000: ['_absolute']})

    def test_every_symbol_type_is_picked_by_its_own_byte(self):
        symbols = [(f'_s{k}'.encode(), k, k) for k in range(256)]
        data = image(symbols=symbols)
        img = macho.macho(data)
        for k in range(256):
            self.assertEqual([s.value for s in macho.symbols(data, img, lambda kind, k=k: kind == k)], [k])

    def test_subtypes_and_fileset_entries(self):
        data = image(subtype=0x80000002, fileset=True)
        with self.assertRaises(ValueError):
            macho.macho(data)
        img = macho.macho(data, subtypes=None)
        self.assertEqual(img['kind'], macho.MH_FILESET)
        self.assertEqual(macho.fileset_entries(data, img), [('com.apple.kernel', 0x1000, 0)])

    def test_damage_is_refused(self):
        data = bytearray(image())
        for at, value in ((0, 0xFEEDFACE), (4, 7), (20, 0x10000), (36, 9)):
            with self.subTest(at=at):
                broken = bytearray(data)
                struct.pack_into('<I', broken, at, value)
                with self.assertRaises(ValueError):
                    macho.macho(broken)


if __name__ == '__main__':
    unittest.main()
