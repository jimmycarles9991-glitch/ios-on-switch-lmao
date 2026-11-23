"""logharvest.py over ndjson records shaped as `log show --style ndjson` prints the runtime's lines."""

import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import logharvest

RB = '/System/Library/PrivateFrameworks/RenderBox.framework/default.metallib'
HEAD = 'samples=1 topo=0 ds=260/260 c0=80:15:0:1:0:0:1:0:0 c1=105:0:0:1:0:0:1:0:0 c2=80:0:0:1:0:0:1:0:0 '
COLOR = f'{HEAD} | v primitive__vertex  | f primitive_color_fragment #0:36:4501 | l RenderBox'
GRADIENT = f'{HEAD} | v primitive_gradient_vertex  | f primitive_gradient_fragment  | l RenderBox'
IMAGE = f'{HEAD} | v primitive_image_vertex #0:36:8700 | f primitive_image_fragment #0:36:8700 | l RenderBox'
CUBIC = ' | cubic s0:0000n t0:80'
CA = (
    'samples=1 topo=0 ds=0/0 c0=80:15:1:1:5:0:1:5:0 a0=31:0:1:48:1:1  | v fixed_vert_lph_spc '
    + ' '.join(f'fc_c{i}:49:01' for i in range(40))
    + ' | f fixed_frag_lph_cph '
    + ' '.join(f'fc_d{i}:49:00' for i in range(40))
)
CA_LABEL = 'com.apple.coreanimation.PBGRAXm_A3Xhfc'
CORPUS = (
    f'R ok=1 label=primitive_image os=21H461,24B119 samples=1 topo=0 ds=260/260 c0=80:15:0:1:0:0:1:0:0'
    f' c1=105:0:0:1:0:0:1:0:0 c2=80:0:0:1:0:0:1:0:0 | v primitive_image_vertex lib={RB} #0:36:8700'
    f' | f primitive_image_fragment lib={RB} #0:36:8700 | \n'
    f'R ok=1 label=- os=21H461 {HEAD.rstrip()} | v primitive_gradient_vertex lib={RB}  '
    f'| f primitive_gradient_fragment lib={RB}  | \n'
)


def load_caspec():
    sys.path.insert(0, str(Path(logharvest.make_pack.__file__).parent / 'ca'))
    import caspec  # noqa: PLC0415 (ca/ is only on the path here)

    return caspec


BOOT, OTHER = 'AAAAAAAA-0000-0000-0000-000000000001', 'BBBBBBBB-0000-0000-0000-000000000002'


def record(boot: str, process: str, message: str) -> dict:
    return {
        'eventType': 'logEvent',
        'bootUUID': boot,
        'processImagePath': f'/System/Library/CoreServices/{process}',
        'eventMessage': message,
    }


def made(boot: str, process: str, name: str, key: str, outcome: str = 'made') -> dict:
    message = (
        f'SwitchMetal: a state the pack lacks {outcome} in 120 ms (UI) after 0 ms for a compile slot: {name} '
        f'{key[:400]}'
    )
    return record(boot, process, message)


def parts(boot: str, process: str, name: str, label: str, key: str) -> list[dict]:
    text = f'label={label} {key}'
    chunks = [text[i : i + 700] for i in range(0, len(text), 700)]
    return [
        record(boot, process, f'SwitchMetal: harvest {name} {i + 1}/{len(chunks)} {chunk}')
        for i, chunk in enumerate(chunks)
    ]


def interleaved(first: list[dict], second: list[dict]) -> list[dict]:
    out = []
    for i in range(max(len(first), len(second))):
        out += first[i : i + 1] + second[i : i + 1]
    return out


def variant(boot: str, process: str, state: str, name: str) -> dict:
    hashed = f'{logharvest.make_pack.packfile.fnv(state.encode()):016x}'
    message = (
        f"{process}[323] state.variant_unpacked: a packed state's variant the pack lacks: its draws are refused:"
        f'{name} of {hashed} {state}'
    )
    return record(boot, process, message[:250])


def boot_records() -> list[dict]:
    return [
        record(BOOT, 'CallHistorySyncHelper', 'OS version: 24B119'),
        made(BOOT, 'WidgetRenderer', '0000000000000001', COLOR),
        made(BOOT, 'iconservicesagent', '0000000000000002', GRADIENT),
        made(BOOT, 'iconservicesagent', '0000000000000003', IMAGE + CUBIC),
        made(BOOT, 'SwitchHWTest', '0000000000000004', COLOR.replace('4501', '4502')),
        made(BOOT, 'backboardd', '0000000000000005', CA),
        *parts(BOOT, 'backboardd', '0000000000000005', CA_LABEL, CA),
        made(BOOT, 'backboardd', '0000000000000006', CA.replace('fc_d1:', 'fc_e1:')),
        made(BOOT, 'cameracaptured', '0000000000000007', COLOR.replace('4501', '4503'), 'refused'),
        made(OTHER, 'WidgetRenderer', '0000000000000008', COLOR.replace('4501', '4504')),
    ]


class LogHarvestTest(unittest.TestCase):
    def run_tool(self, records: list[dict], *flags: str) -> tuple[list[str], str]:
        with tempfile.TemporaryDirectory() as scratch:
            corpus = Path(scratch) / 'corpus.log'
            corpus.write_text(CORPUS)
            ndjson = Path(scratch) / 'boot.ndjson'
            ndjson.write_text(''.join(json.dumps(r) + '\n' for r in records))
            logharvest.main(['--ndjson', str(ndjson), '--corpus', str(corpus), *flags])
            return corpus.read_text().splitlines()[2:], corpus.read_text()

    def test_each_made_state_becomes_a_tagged_line_that_gives_back_its_key(self):
        added, _ = self.run_tool(boot_records())
        keys = [logharvest.make_pack.state_key(line) for line in added]
        self.assertEqual(sorted(keys), sorted([COLOR, GRADIENT, IMAGE + CUBIC, CA]))
        self.assertTrue(all(' os=24B119 ' in line for line in added))

    def test_a_ca_state_carries_its_label_from_the_harvest_parts(self):
        added, _ = self.run_tool(boot_records())
        (line,) = [line for line in added if 'fixed_frag_lph_cph' in line]
        self.assertTrue(line.startswith(f'R ok=1 label={CA_LABEL} os=24B119 '))

    def test_a_framework_states_line_names_its_library_on_both_stages(self):
        added, _ = self.run_tool(boot_records())
        (line,) = [line for line in added if 'primitive_color_fragment' in line]
        self.assertEqual(line.count(f' lib={RB}'), 2)

    def test_the_same_key_tagged_with_another_build_is_added_for_this_one(self):
        added, _ = self.run_tool(boot_records())
        self.assertIn(GRADIENT, [logharvest.make_pack.state_key(line) for line in added])

    def test_a_corpus_line_naming_several_builds_counts_for_each(self):
        added, _ = self.run_tool(boot_records())
        self.assertNotIn(IMAGE, [logharvest.make_pack.state_key(line) for line in added])

    def test_a_boot_naming_no_build_is_harvested_only_under_build(self):
        added, _ = self.run_tool([record(BOOT, 'x', 'OS version: 24B119'), *boot_records()[-1:]])
        self.assertEqual(added, [])
        added, _ = self.run_tool(boot_records()[-1:], '--build', '24B119')
        self.assertEqual([logharvest.make_pack.state_key(line) for line in added], [COLOR.replace('4501', '4504')])

    def test_a_boot_naming_another_build_than_build_is_refused(self):
        with self.assertRaises(SystemExit):
            self.run_tool(boot_records(), '--build', '21H461')

    def test_a_variant_line_finds_its_state_by_hash(self):
        records = [record(BOOT, 'x', 'OS version: 24B119'), variant(BOOT, 'iconservicesagent', IMAGE, CUBIC)]
        added, _ = self.run_tool(records)
        self.assertEqual([logharvest.make_pack.state_key(line) for line in added], [IMAGE + CUBIC])

    def test_a_dry_run_leaves_the_corpus_as_it_was(self):
        _, text = self.run_tool(boot_records(), '--dry-run')
        self.assertEqual(text, CORPUS)

    def test_a_second_run_adds_nothing(self):
        with tempfile.TemporaryDirectory() as scratch:
            corpus = Path(scratch) / 'corpus.log'
            corpus.write_text(CORPUS)
            ndjson = Path(scratch) / 'boot.ndjson'
            ndjson.write_text(''.join(json.dumps(r) + '\n' for r in boot_records()))
            for _ in range(2):
                logharvest.main(['--ndjson', str(ndjson), '--corpus', str(corpus)])
            self.assertEqual(len(corpus.read_text().splitlines()), 2 + 4)

    def test_a_variants_test_class_is_left_off_and_its_binds_kept(self):
        records = [
            record(BOOT, 'x', 'OS version: 24B119'),
            made(BOOT, 'x', '0000000000000009', IMAGE + CUBIC + ' | tests=2'),
        ]
        added, _ = self.run_tool(records)
        self.assertEqual([logharvest.make_pack.state_key(line) for line in added], [IMAGE + CUBIC])

    def test_harvest_parts_that_disagree_with_the_made_line_are_left_out(self):
        records = [
            record(BOOT, 'x', 'OS version: 24B119'),
            made(BOOT, 'backboardd', '0000000000000005', CA),
            *parts(BOOT, 'backboardd', '0000000000000005', CA_LABEL, CA.replace('fc_c0:', 'fc_x0:')),
        ]
        added, _ = self.run_tool(records)
        self.assertEqual(added, [])

    def test_a_long_keys_parts_rejoin_out_of_order_and_between_another_states(self):
        long_key = CA + ' ' + ' '.join(f'fc_g{i}:49:00' for i in range(120))
        long_parts = parts(BOOT, 'backboardd', '000000000000000a', CA_LABEL, long_key)
        other_parts = parts(BOOT, 'backboardd', '0000000000000005', CA_LABEL, CA)
        self.assertGreater(len(long_parts), 3)
        remote_made = record(
            BOOT,
            'backboardd',
            'SwitchMetal: a state the pack lacks made in 35 ms (UI, store) after 0 ms for a compile slot: '
            f'000000000000000a {long_key[:400]}',
        )
        records = [
            record(BOOT, 'x', 'OS version: 24B119'),
            remote_made,
            made(BOOT, 'backboardd', '0000000000000005', CA),
            *interleaved(list(reversed(long_parts)), other_parts),
        ]
        added, _ = self.run_tool(records)
        self.assertEqual(sorted(logharvest.make_pack.state_key(line) for line in added), sorted([CA, long_key]))
        self.assertTrue(all(line.startswith(f'R ok=1 label={CA_LABEL} ') for line in added))

    def test_a_refused_states_whole_parts_add_no_line(self):
        records = [
            record(BOOT, 'x', 'OS version: 24B119'),
            made(BOOT, 'backboardd', '0000000000000005', CA, 'refused'),
            *parts(BOOT, 'backboardd', '0000000000000005', CA_LABEL, CA),
        ]
        boots = logharvest.read_boots(records)
        added, report = logharvest.harvest(boots, CORPUS.splitlines(), None, Path('/nonexistent'))
        self.assertEqual(added, [])
        self.assertEqual(report.left['backboardd', 'refused on the device'], 1)

    def test_a_harvested_ca_line_gives_caspec_the_fields_of_its_untagged_line(self):
        added, _ = self.run_tool(boot_records())
        (line,) = [line for line in added if 'fixed_frag_lph_cph' in line]
        caspec = sys.modules['caspec'] if 'caspec' in sys.modules else load_caspec()
        self.assertEqual(caspec.corpus_fields(line), caspec.corpus_fields(line.replace(' os=24B119', '')))

    def test_a_sampled_two_plane_variant_and_its_state_are_harvested_once(self):
        # TegraGM20BMetalCubic.mm renderVariant's name: a test class the pack builder makes of each line
        state = CA.replace('c0=80:', 'c0=550:')
        key = state + ' | tests=2 | xrs=2'
        records = [
            record(BOOT, 'x', 'OS version: 24B119'),
            made(BOOT, 'backboardd', '000000000000000a', key),
            *parts(BOOT, 'backboardd', '000000000000000a', CA_LABEL, key),
        ]
        with tempfile.TemporaryDirectory() as scratch:
            corpus = Path(scratch) / 'corpus.log'
            corpus.write_text(CORPUS)
            ndjson = Path(scratch) / 'boot.ndjson'
            ndjson.write_text(''.join(json.dumps(r) + '\n' for r in records))
            for _ in range(2):
                logharvest.main(['--ndjson', str(ndjson), '--corpus', str(corpus)])
            added = corpus.read_text().splitlines()[2:]
        self.assertEqual([logharvest.make_pack.state_key(line) for line in added], [state, state + ' | xrs=2'])
        self.assertTrue(added[1].startswith(f'R ok=1 label={CA_LABEL} os=24B119 '))
        self.assertTrue(added[1].endswith(' | xrs=2 | '))

    def test_a_packed_states_sampled_and_written_variant_is_harvested_alone(self):
        records = [
            record(BOOT, 'x', 'OS version: 24B119'),
            made(BOOT, 'iconservicesagent', '000000000000000b', IMAGE + CUBIC + ' | xrs=4 | wtex=1:23'),
        ]
        added, _ = self.run_tool(records)
        keys = [logharvest.make_pack.state_key(line) for line in added]
        self.assertEqual(keys, [IMAGE + CUBIC + ' | xrs=4 | wtex=1:23'])
        self.assertEqual(added[0].count(f' lib={RB}'), 2)

    def test_a_variants_state_is_its_key_before_the_variant_name(self):
        self.assertEqual(logharvest.base_key(IMAGE + CUBIC + ' | tests=2 | xrs=2 | wtex=1:23'), IMAGE)
        self.assertEqual(logharvest.untested(IMAGE + CUBIC + ' | tests=2 | xrs=2'), IMAGE + CUBIC + ' | xrs=2')

    def test_an_earlier_drivers_two_plane_pass_is_read_without_it(self):
        # the pack's variant names no longer hold " | xr2p": without the cut its key's state would end in it
        self.assertEqual(logharvest.base_key(IMAGE + CUBIC + ' | tests=2 | xr2p | xrs=2'), IMAGE)
        self.assertEqual(logharvest.untested(IMAGE + CUBIC + ' | tests=2 | xr2p | xrs=2'), IMAGE + CUBIC + ' | xrs=2')
        self.assertEqual(logharvest.untested(IMAGE + ' | xr2p'), IMAGE)

    def test_an_earlier_drivers_shadowed_variant_is_harvested_as_its_state(self):
        # boot 1's iconservicesagent asked RenderBox's states as " | xrsh=1" variants, which a device draws as the
        # states themselves now
        records = [
            record(BOOT, 'x', 'OS version: 24B119'),
            made(BOOT, 'iconservicesagent', '000000000000000c', COLOR + CUBIC + ' | tests=2 | xrsh=1'),
        ]
        added, _ = self.run_tool(records)
        self.assertEqual([logharvest.make_pack.state_key(line) for line in added], [COLOR, COLOR + CUBIC])
        self.assertEqual(logharvest.base_key(IMAGE + CUBIC + ' | tests=2 | xrsh=1 | xrs=2'), IMAGE)

    def test_check_fails_while_an_os_state_is_not_the_corpuss_and_leaves_it_as_it_was(self):
        with tempfile.TemporaryDirectory() as scratch:
            corpus = Path(scratch) / 'corpus.log'
            corpus.write_text(CORPUS)
            ndjson = Path(scratch) / 'boot.ndjson'
            # but the key the log cut, which no line can hold and --check counts
            whole = [r for r in boot_records() if ' 0000000000000006 ' not in r['eventMessage']]
            ndjson.write_text(''.join(json.dumps(r) + '\n' for r in whole))
            check = ['--ndjson', str(ndjson), '--corpus', str(corpus), '--check']
            self.assertEqual(logharvest.main(check), 1)
            self.assertEqual(corpus.read_text(), CORPUS)
            logharvest.main(['--ndjson', str(ndjson), '--corpus', str(corpus)])
            self.assertEqual(logharvest.main(check), 0)

    def test_a_framework_only_the_crawl_names_takes_the_crawls_path(self):
        tungsten = f'{HEAD} | v tungsten_vertex  | f tungsten_fragment  | l Tungsten'
        records = [record(BOOT, 'Photos', 'OS version: 24B119'), made(BOOT, 'Photos', '0000000000000009', tungsten)]
        boots = logharvest.read_boots(records)
        _, report = logharvest.harvest(boots, CORPUS.splitlines(), None, Path('/nonexistent'))
        self.assertEqual(report.left['Photos', 'no library path for Tungsten'], 1)
        with tempfile.TemporaryDirectory() as scratch:
            crawl = Path(scratch) / 'inventory.json'
            library = 'System/Library/PrivateFrameworks/Tungsten.framework/default.metallib'
            crawl.write_text(json.dumps({'libraries': [library]}))
            new, report = logharvest.harvest(boots, CORPUS.splitlines(), None, Path('/nonexistent'), crawl)
        self.assertEqual(report.added['Photos'], 1)
        self.assertIn(' lib=/System/Library/PrivateFrameworks/Tungsten.framework/default.metallib ', new[0])

    def test_a_part_whose_trailing_space_the_log_dropped_rejoins_whole(self):
        head = f'label={CA_LABEL} '
        key = 'k' * (699 - len(head)) + ' fc_has_premultiply:53:00'
        text = head + key
        chunks = [text[i : i + 700] for i in range(0, len(text), 700)]
        logged = [
            record(BOOT, 'backboardd', f'SwitchMetal: harvest 000000000000000a {i + 1}/{len(chunks)} {chunk.rstrip()}')
            for i, chunk in enumerate(chunks)
        ]
        boot = logharvest.read_boots(logged)[BOOT]
        self.assertEqual(logharvest.whole(boot, 'backboardd', '000000000000000a'), (CA_LABEL, key))

    def test_left_out_states_are_counted_by_reason(self):
        boots = logharvest.read_boots(boot_records())
        _, report = logharvest.harvest(boots, CORPUS.splitlines(), None, Path('/nonexistent'))
        self.assertEqual(report.left['SwitchHWTest', 'our own app'], 1)
        self.assertEqual(report.left['cameracaptured', 'refused on the device'], 1)
        self.assertEqual(report.left['backboardd', 'key cut at 400 characters, no whole parts'], 1)


IR = '/System/Library/PrivateFrameworks/IconRendering.framework/default.metallib'
VS = '/System/Library/VideoProcessors/VideoStabilizationV2.bundle/default.metallib'
CLAMPED = ' | fp clampedPlusL_v1 as=custom_blend #2:33:00000000'
LINKED = IMAGE.replace(' | l RenderBox', CLAMPED + ' | l RenderBox')
BLUR = 'samples=1 topo=0 ds=0/0 c0=125:15:0:1:0:0:1:0:0 a0=29:0:0:8:1:1  | v vertexBlur  | f fragmentBlur #0:53:01'
AIR = ' | air 227093b20fef2e1d 021ea2f6f5187e8c'
BLUR_ROW = (
    'unspecialized\tVideoStabilizationV2.bundle\t-\tdevice\tR ok=1 label=- samples=1 topo=0 ds=0/0'
    f' c0=125:15:0:1:0:0:1:0:0 a0=29:0:0:8:1:1 | v vertexBlur lib={VS}  | f fragmentBlur lib={VS} #0:53:01 | '
)
LINKED_MADE = LINKED + ' | linked f3f5e961c3369477d'


def specialization(library: str, function: str) -> str:
    return f'S ok=1 lib={library} name={function} k= | '


class CacheTailTest(unittest.TestCase):
    """States whose made line carries the cache key's tails: a linked function's AIR, or the stages' AIR."""

    def harvest(self, key: str, extra: list[str] = (), rows: list[str] = ()) -> tuple[list[str], logharvest.Report]:
        records = [record(BOOT, 'x', 'OS version: 24B119'), made(BOOT, 'cameracaptured', '000000000000000c', key)]
        with tempfile.TemporaryDirectory() as scratch:
            (Path(scratch) / 'archives').mkdir()
            (Path(scratch) / 'archives' / '24B119.tsv').write_text(''.join(row + '\n' for row in rows))
            boots = logharvest.read_boots(records)
            return logharvest.harvest(boots, [*CORPUS.splitlines(), *extra], None, Path(scratch))

    def test_a_framework_state_linking_a_framework_function_names_the_functions_library(self):
        added, _ = self.harvest(LINKED_MADE, [specialization(IR, 'clampedPlusL_v1')])
        self.assertEqual([logharvest.make_pack.state_key(line) for line in added], [LINKED])
        self.assertIn(f' | fp clampedPlusL_v1 lib={IR} as=custom_blend ', added[0])
        self.assertEqual(added[0].count(f' lib={RB}'), 2)

    def test_a_linked_state_harvested_once_is_held_the_next_time(self):
        corpus = [specialization(IR, 'clampedPlusL_v1')]
        added, _ = self.harvest(LINKED_MADE, corpus)
        again, report = self.harvest(LINKED_MADE, corpus + added)
        self.assertEqual(again, [])
        self.assertEqual(report.present['cameracaptured'], 1)

    def test_an_air_keyed_state_takes_the_library_an_archive_row_names_its_functions_in(self):
        added, _ = self.harvest(BLUR + AIR, rows=[BLUR_ROW])
        self.assertEqual([logharvest.make_pack.state_key(line) for line in added], [BLUR])
        self.assertEqual(added[0].count(f' lib={VS} '), 2)

    def test_an_air_keyed_state_takes_the_library_the_specializations_name(self):
        extra = [specialization(VS, 'vertexBlur'), specialization(VS, 'fragmentBlur')]
        added, _ = self.harvest(BLUR + AIR, extra)
        self.assertEqual([logharvest.make_pack.state_key(line) for line in added], [BLUR])
        self.assertTrue(added[0].startswith('R ok=1 label=- os=24B119 '))

    def test_a_function_two_libraries_are_named_for_is_left_out(self):
        other = VS.replace('VideoStabilizationV2', 'MetalFilter')
        extra = [
            specialization(VS, 'vertexBlur'),
            specialization(VS, 'fragmentBlur'),
            specialization(other, 'vertexBlur'),
        ]
        added, report = self.harvest(BLUR + AIR, extra)
        self.assertEqual(added, [])
        why = "an AIR-keyed state whose functions' one library no corpus line or archive row names"
        self.assertEqual(report.left['cameracaptured', why], 1)

    def test_a_framework_state_keyed_by_its_air_is_left_out(self):
        added, report = self.harvest(LINKED_MADE + AIR, [specialization(IR, 'clampedPlusL_v1')])
        self.assertEqual(added, [])
        why = 'a framework state keyed by its AIR: it links a function no framework library holds'
        self.assertEqual(report.left['cameracaptured', why], 1)

    def test_an_air_keyed_state_that_links_is_left_out(self):
        extra = [specialization(VS, f) for f in ('vertexBlur', 'fragmentBlur', 'clampedPlusL_v1')]
        added, report = self.harvest(BLUR + CLAMPED + ' | linked f3f5e961c3369477d' + AIR, extra)
        self.assertEqual(added, [])
        self.assertEqual(report.left['cameracaptured', 'an AIR-keyed state that links functions'], 1)

    def test_a_linked_function_no_framework_library_is_named_for_is_left_out(self):
        added, report = self.harvest(LINKED_MADE, [specialization(VS, 'clampedPlusL_v1')])
        self.assertEqual(added, [])
        self.assertEqual(report.left['cameracaptured', 'links a function no one framework library is named for'], 1)


if __name__ == '__main__':
    unittest.main()
