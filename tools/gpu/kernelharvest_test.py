"""kernelharvest.py over ndjson records shaped as `log show --style ndjson` prints the runtime's kernel lines."""

import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import kernelharvest

AIR = 'd3e7ea7520861b4f4d00d54546adbc1fab62b0e38d0137f9c10455b68263730b'
OTHER_AIR = '11' * 32
CI_LIB = 'aa' * 32
SHAPE = (
    ' | max=0 tgm=0 | tg=32,8,1 | tgmem= | tex=0:115:2:1:1:02030405,1:588:2:1:1:02030405,2:589:2:1:1:02030405'
    ' | smp=0:1:1:0:0:0:0:0:7f7fffff:1:0:0:1:0'
)
REFUSED = (
    'SwitchMetal: a kernel the pack lacks is refused at 32,8,1 threads: m2v refused its translation: plan: '
    'Unsupported(...)'
    f' | ci_srgb_to_linear_colormatrix {AIR}{SHAPE} | constants '
)
MADE = (
    'SwitchMetal: a kernel the pack lacks, made at its first dispatch of 32,32,1 threads in 166826 us (IN):'
    f' VTMTSComputeFunction1x1 {OTHER_AIR} | max=0 tgm=0 | tg=32,32,1 | tgmem= | tex=0:80:2:1:1:02030405 | smp='
    ' | constants #0:33:41524742 #3:29:ffffffff'
)
VT = '/System/Library/Frameworks/VideoToolbox.framework/default.metallib'
CORPUS = (
    f'D ok=1 tg=32,8,1 edge=0,0,0 threads=0 os=24B119 | form=f opt=0 | k VTMTSComputeFunction1x1 lib={VT} #0:33:1 | '
)
LINE = (
    'D ok=1 tg=32,8,1 edge=0,0,0 threads=0 tex=0:115:2:1:1:02030405,1:588:2:1:1:02030405,2:589:2:1:1:02030405'
    ' smp=0:1:1:0:0:0:0:0:7f7fffff:1:0:0:1:0 os=24B119 | form=d opt=0 max=0 tgm=0 label=-'
    f' | k ci_srgb_to_linear_colormatrix lib=data:{CI_LIB}  | '
)


def record(message: str, process: str = 'SwitchHWTest', boot: str = 'boot-1') -> dict:
    return {
        'eventType': 'logEvent',
        'eventMessage': message,
        'processImagePath': f'/Applications/{process}.app/{process}',
        'bootUUID': boot,
    }


def library(functions: dict[str, str]) -> bytes:
    """A thin metallib whose function list names each function with its AIR hash (metallib.cpp's tags)."""
    entries = b''
    for name, air in functions.items():
        tags = b'NAME' + struct.pack('<H', len(name) + 1) + name.encode() + b'\0'
        tags += b'HASH' + struct.pack('<H', 32) + bytes.fromhex(air) + b'ENDT'
        entries += struct.pack('<I', 4 + len(tags)) + tags
    header = bytearray(b'MTLB' + bytes(0x54))
    struct.pack_into('<QQ', header, 24, 0x58, len(entries))
    return bytes(header) + struct.pack('<I', len(functions)) + entries


def harvest_parts(text: str, name: str = '0123456789abcdef') -> list[dict]:
    parts = [text[i : i + kernelharvest.PART_CHARS] for i in range(0, len(text), kernelharvest.PART_CHARS)]
    # the log drops a part's trailing spaces, which the reader pads back
    return [
        record(f'SwitchMetal: kernel harvest {name} {i + 1}/{len(parts)} {part.rstrip()}', 'mediaanalysisd')
        for i, part in enumerate(parts)
    ]


def facts_harvest_parts(text: str, name: str = '0123456789abcdef', process: str = 'cameracaptured') -> list[dict]:
    parts = [text[i : i + kernelharvest.PART_CHARS] for i in range(0, len(text), kernelharvest.PART_CHARS)]
    return [
        record(f'SwitchMetal: facts harvest {name} {i + 1}/{len(parts)} {part.rstrip()}', process)
        for i, part in enumerate(parts)
    ]


class KernelHarvestTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.libraries = Path(self.dir.name)
        self.addCleanup(self.dir.cleanup)

    def run_harvest(self, records: list[dict], corpus: list[str] | None = None, build: str | None = '24B119'):
        return kernelharvest.harvest(kernelharvest.read_boots(records), corpus or [], build, [self.libraries])

    def test_a_refused_kernel_is_made_of_the_data_library_holding_its_air(self):
        (self.libraries / f'{CI_LIB}.metallib').write_bytes(library({'ci_srgb_to_linear_colormatrix': AIR}))
        new, report = self.run_harvest([record(REFUSED)])
        self.assertEqual(new, [LINE])
        self.assertEqual(report.formats, {'115': 1, '588': 1, '589': 1})

    def test_a_kernel_no_library_holds_is_named_for_its_library_to_be_pulled(self):
        (self.libraries / f'{CI_LIB}.metallib').write_bytes(library({'ci_srgb_to_linear_colormatrix': OTHER_AIR}))
        new, report = self.run_harvest([record(REFUSED)], corpus=[LINE.replace('max=0', 'max=1')])
        self.assertEqual(new, [])
        why = f'no library holds ci_srgb_to_linear_colormatrix {AIR} (textures of 115,588,589)'
        self.assertEqual(report.left, {('SwitchHWTest', why): 1})

    def test_a_kernel_the_corpus_names_one_library_for_takes_it_and_its_line_is_held_once(self):
        older = MADE.replace(' us (IN):', ' us:')
        new, report = self.run_harvest([record(MADE, 'mediaanalysisd'), record(older, 'other')], corpus=[CORPUS])
        self.assertEqual(
            new,
            [
                'D ok=1 tg=32,32,1 edge=0,0,0 threads=0 tex=0:80:2:1:1:02030405 os=24B119 | form=d opt=0 max=0 tgm=0'
                f' label=- | k VTMTSComputeFunction1x1 lib={VT} #0:33:41524742 #3:29:ffffffff | '
            ],
        )
        self.assertEqual(report.added, {'mediaanalysisd': 1})
        self.assertEqual(report.present, {'other': 1})
        again, report = self.run_harvest([record(MADE, 'mediaanalysisd')], corpus=[CORPUS, *new])
        self.assertEqual((again, report.present), ([], {'mediaanalysisd': 1}))
        other_build, _ = self.run_harvest([record(MADE, 'mediaanalysisd')], corpus=[CORPUS, *new], build='21H461')
        self.assertEqual(len(other_build), 1)

    def test_a_harvest_lines_parts_carry_its_library_specialization_and_stage_input(self):
        constants = '#0:29:01000000' + ''.join(f' #{i}:29:ffffffff' for i in range(1, 60))
        constants += ' | in a0=29:0:1:12:5:1 i=0:4'
        shape = SHAPE.replace('tex=0:115', 'tex=0:500').replace(':02030405,1', ':02030405:am1,1')
        key = f'kernel | {AIR} | {constants}{shape}'
        new, report = self.run_harvest(harvest_parts(f'lib={VT} function=vt_scale spec {key}'))
        self.assertEqual(len(new), 1, report.left)
        line = new[0]
        self.assertIn(' tex=0:500:2:1:1:02030405:am1,1:588:', line)
        self.assertTrue(line.endswith(f' | k vt_scale lib={VT} spec {constants} | '), line)

    def test_a_region_dispatch_a_linked_kernel_and_a_cut_line_are_left_out_by_name(self):
        region = REFUSED.replace(' | smp=0:1:1:0:0:0:0:0:7f7fffff:1:0:0:1:0', ' | smp= | region=1')
        linked = f'kernel | {AIR} |  | kl {OTHER_AIR}{SHAPE}'
        records = [record(region), *harvest_parts(f'lib={VT} function=k {linked}', 'fedcba9876543210')]
        records += [record('SwitchMetal: a kernel the pack lacks is refused at 32,8,1 threads: m2v refused | ci_x')]
        records += [record(REFUSED.replace('tex=0:115:2:1:1', 'tex=0:115:2:3:1'), 'levels')]
        records += [record('SwitchHWTest[524] compute.kernel: a kernel the pack lacks (-, threads 32,8,1)', 'route')]
        _, report = self.run_harvest(records)
        self.assertEqual(
            dict(report.left),
            {
                ('SwitchHWTest', 'a region dispatch (its edges are not in its key)'): 1,
                ('mediaanalysisd', 'a kernel that links functions'): 1,
                ('SwitchHWTest', 'a kernel line the log cut'): 1,
                ('levels', 'a texture of another type than a one-level 2D one (kernel_shapes.h parseTextures)'): 1,
                ('route', 'a compute.kernel route line, which names no key'): 1,
            },
        )

    def test_a_boot_naming_no_build_is_skipped(self):
        new, report = self.run_harvest([record(REFUSED)], build=None)
        self.assertEqual((new, report.boots), ([], ['boot-1: names no OS build, skipped (--build names it)']))

    def test_a_facts_harvest_line_becomes_a_facts_only_corpus_line(self):
        constants = '#0:29:01000000' + ''.join(f' #{i}:29:ffffffff' for i in range(1, 60))
        constants += ' | in a0=29:0:1:12:5:1 i=0:4'
        key = f'facts | {AIR} | {constants} | max=256 tgm=1'
        new, report = self.run_harvest(facts_harvest_parts(f'lib={VT} function=cam_isp spec {key}'))
        self.assertEqual(len(new), 1, report.left)
        self.assertEqual(
            new[0],
            f'D ok=1 tg=0,0,0 edge=0,0,0 threads=0 os=24B119 | form=d opt=0 max=256 tgm=1 label=-'
            f' | k cam_isp lib={VT} spec {constants} | ',
        )
        self.assertEqual(report.added, {'cameracaptured': 1})
        again, report = self.run_harvest(facts_harvest_parts(f'lib={VT} function=cam_isp spec {key}'), corpus=new)
        self.assertEqual((again, report.present), ([], {'cameracaptured': 1}))

    def test_a_facts_harvest_already_covered_by_a_dispatch_is_not_added_but_a_dispatch_adds_over_facts(self):
        facts_key = f'facts | {OTHER_AIR} | #0:33:1 | max=0 tgm=0'
        new, report = self.run_harvest(
            facts_harvest_parts(f'lib={VT} function=VTMTSComputeFunction1x1 {facts_key}'), corpus=[CORPUS]
        )
        self.assertEqual((new, report.present), ([], {'cameracaptured': 1}))
        facts_line = (
            'D ok=1 tg=0,0,0 edge=0,0,0 threads=0 os=24B119 | form=d opt=0 max=0 tgm=0 label=-'
            f' | k VTMTSComputeFunction1x1 lib={VT} #0:33:41524742 #3:29:ffffffff | '
        )
        dispatched, report = self.run_harvest([record(MADE, 'mediaanalysisd')], corpus=[facts_line])
        self.assertEqual(len(dispatched), 1)
        self.assertIn('tg=32,32,1', dispatched[0])

    def test_a_facts_harvest_linking_functions_is_left_out(self):
        linked = f'facts | {AIR} |  | kl {OTHER_AIR} | max=0 tgm=0'
        new, report = self.run_harvest(facts_harvest_parts(f'lib={VT} function=cam_linked {linked}'))
        self.assertEqual(new, [])
        self.assertEqual(report.left, {('cameracaptured', 'a kernel that links functions'): 1})


if __name__ == '__main__':
    unittest.main()
