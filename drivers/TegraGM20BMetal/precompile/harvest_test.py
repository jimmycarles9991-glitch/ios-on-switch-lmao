"""harvest.py finds a system process's m2v cache by its container's identifier, whatever UUID the install gave it."""

import hashlib
import json
import plistlib
import struct
import tarfile
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from drivers.TegraGM20BMetal.precompile import harvest


def container(volume: Path, uuid: str, identifier: str) -> Path:
    root = volume / 'containers/Data/System' / uuid
    (root / 'Library/Caches/m2v').mkdir(parents=True)
    with (root / harvest.CONTAINER_METADATA).open('wb') as f:
        plistlib.dump({'MCMMetadataIdentifier': identifier}, f)
    (root / 'Library').chmod(0)
    return root


class Lines(unittest.TestCase):
    def test_a_render_line_keeps_its_build_and_its_variants_binds(self):
        line = (
            'R ok=1 label=primitive_image os=24B119 samples=1 topo=0 ds=260/260 t=12 c0=80:0:0:1:0:0:1:0:0'
            ' | v primitive_image_vertex #0:36:4700 | f primitive_image_fragment #0:36:4700'
            ' | cubic s0:0000n t0:80 | ms=4'
        )
        kept, key = harvest.normal(line)
        self.assertEqual(
            kept,
            'R ok=1 label=primitive_image os=24B119 samples=1 topo=0 ds=260/260 c0=80:0:0:1:0:0:1:0:0'
            ' | v primitive_image_vertex #0:36:4700 | f primitive_image_fragment #0:36:4700 | cubic s0:0000n t0:80 | ',
        )
        self.assertNotEqual(key, harvest.normal(line.replace(' os=24B119', ' os=21H461'))[1])

    def test_a_journal_line_names_the_build_it_was_journaled_under(self):
        render = (
            'R ok=1 label=x samples=1 topo=0 ds=0/0 c0=80:15:0:1:0:0:1:0:0 | v a lib=/l.metallib  | f b lib=/l.m  | '
        )
        dispatch = 'D ok=1 tg=32,8,1 edge=0,0,0 threads=0 | form=d opt=0 | k ci lib=/l.metallib  |'
        self.assertEqual(harvest.normal(render, '24B119')[0], render.replace(' samples=', ' os=24B119 samples=', 1))
        self.assertEqual(harvest.normal(dispatch, '24B119')[0], dispatch.replace(' | form', ' os=24B119 | form', 1))
        tagged = render.replace(' samples=', ' os=21H461 samples=', 1)
        self.assertEqual(harvest.normal(tagged, '24B119')[0], tagged)

    def test_a_merge_tags_the_journals_lines_but_leaves_the_corpus_as_it_was(self):
        with tempfile.TemporaryDirectory() as d:
            corpus = Path(d) / 'corpus.log'
            old = 'D ok=1 tg=1,1,1 edge=0,0,0 threads=0 os=21H461 | form=d | k a lib=/l.metallib  |'
            corpus.write_text(old + '\n')
            journal = 'D ok=1 tg=2,1,1 edge=0,0,0 threads=0 | form=d | k a lib=/l.metallib  |'
            lines = harvest.merge(corpus, [('j', journal + '\n')], '24B119')
            self.assertEqual(lines, [old, journal.replace(' | form', ' os=24B119 | form', 1)])


# Where the plugin's "ready in <process>, cache <directory>" lines put each process's cache on 24B119, below the Data
# volume (/private/var); the system containers' as container() makes them.
BOOT_CACHES = {
    'assetsd': 'tmp/m2v-assetsd',
    'iconservicesagent': 'tmp/m2v-iconservicesagent',
    'cameracaptured': 'mobile/tmp/com.apple.cameracaptured/m2v',
    'mediaplaybackd': 'mobile/tmp/com.apple.mediaplaybackd/m2v',
    'photoanalysisd': 'mobile/tmp/com.apple.photoanalysisd/m2v',
    'SwitchHWTest': 'mobile/Containers/Data/Application/5D11DCAB-A17F-4F4C-931F-7363A518CD9D/Library/Caches/'
    'm2v-SwitchHWTest',
    'WidgetRenderer_Default': 'mobile/Containers/Data/Application/A9DE58A1-8235-4C90-B907-F9C626DF9F8D/Library/'
    'Caches/m2v-WidgetRenderer_Default',
    'MercuryPosterExtension': 'mobile/Containers/Data/PluginKitPlugin/7424FFDF-0627-4481-ABBA-A177819B5983/Library/'
    'Caches/m2v-MercuryPosterExtension',
}
BOOT_SYSTEM_CACHES = {
    'backboardd': ('CE929FE0-E622-45FA-8423-A8020D7412A1', 'com.apple.backboardd'),
    'SpringBoard': ('1809AE07-0E79-4BDD-9C39-FCED2631C2D3', 'com.apple.springboard'),
    'PosterBoard': ('516BF64A-046D-4364-A803-DBA413E0000E', 'com.apple.PosterBoard'),
}


class Caches(unittest.TestCase):
    def test_every_cache_a_boot_reported_is_found(self):
        with tempfile.TemporaryDirectory() as d:
            volume = Path(d)
            for where in BOOT_CACHES.values():
                (volume / where).mkdir(parents=True)
            closed = [container(volume, uuid, identifier) for uuid, identifier in BOOT_SYSTEM_CACHES.values()]
            try:
                found, skipped = harvest.caches(volume, set(harvest.SYSTEM_CACHES), False, print)
                expected = {volume / where for where in BOOT_CACHES.values()}
                expected |= {root / 'Library/Caches/m2v' for root in closed}
                self.assertEqual({m2v for _, m2v, _ in found}, expected)
                self.assertEqual(skipped, [])
            finally:
                for root in closed:
                    (root / 'Library').chmod(0o755)

    def test_a_closed_system_container_is_opened_by_its_identifier(self):
        with tempfile.TemporaryDirectory() as d:
            volume = Path(d)
            backboardd = container(volume, 'CE929FE0-E622-45FA-8423-A8020D7412A1', 'com.apple.backboardd')
            other = container(volume, '00000000-0000-0000-0000-000000000000', 'com.apple.locationd')
            try:
                found, skipped = harvest.caches(volume, set(harvest.SYSTEM_CACHES), False, print)
                self.assertEqual([m2v for _, m2v, _ in found], [backboardd / 'Library/Caches/m2v'])
                self.assertEqual(skipped, [other.name])
                found, skipped = harvest.caches(volume, {other.name}, False, print, explicit=True)
                self.assertEqual([m2v for _, m2v, _ in found], [other / 'Library/Caches/m2v'])
                self.assertEqual(skipped, [backboardd.name])
            finally:
                for root in (backboardd, other):
                    (root / 'Library').chmod(0o755)


RENDERBOX = '/System/Library/PrivateFrameworks/RenderBox.framework/default.metallib'
TARGETS = 'samples=1 topo=0 ds=260/260'
COLORS = 'c0=80:0:0:1:0:0:1:0:0 c1=105:0:0:1:0:0:1:0:0 c2=80:15:1:1:5:0:1:5:0'
STATE_ID = '#0:36:470010400080020050f8000000000000'
STAGES = (
    f' | v primitive_image_vertex lib={RENDERBOX} as=vhea3baea3ica4fipa4 {STATE_ID}'
    f' | f primitive_image_fragment lib={RENDERBOX} as=fhea3baea3ica4fipa4 {STATE_ID}'
)


def raw_made(t: int, tid: int, callers: str) -> str:
    """Return a raw-made state's line as native_bridge_metal_journal_render writes it: no cost, no options."""
    return f'R t={t} tid={tid} main=0 us=0 ok=1 {TARGETS} label=primitive_image {COLORS}{STAGES} | {callers}'


class Journal(unittest.TestCase):
    def test_a_raw_made_state_merges_once_in_its_corpus_form(self):
        with tempfile.TemporaryDirectory() as d:
            corpus = Path(d) / 'corpus.log'
            first = raw_made(10, 4403, 'RB::Device::render')
            refused = first.replace(' ok=1 ', ' ok=0 ')
            again = raw_made(93011, 5120, 'RB::Surface::draw')
            journals = [('iconservicesagent', f'{first}\n{refused}'), ('SpringBoard', again)]
            lines = harvest.merge(corpus, journals, '24B119')
            self.assertEqual(lines, [f'R ok=1 label=primitive_image os=24B119 {TARGETS} {COLORS}{STAGES} | '])


class Replies(unittest.TestCase):
    def test_harvest_keeps_provenance_for_already_retained_reply_and_pack(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, store = root / 'cache/msl', root / 'staged/msl'
            source.mkdir(parents=True)
            request, uuid = b'kernel source\0', bytes(range(16))
            key = hashlib.sha256(uuid + struct.pack('<I', 13) + request).hexdigest()
            output = b'MTLB fixture'
            reply = b'NSC1' + struct.pack('<IQ', 0, len(output)) + hashlib.sha256(output).digest() + output
            (source / (key + '.reply')).write_bytes(reply)
            self.assertEqual(harvest.replies(source.parent, store), 1)
            (source / (key + '.request')).write_bytes(request)
            provenance = source / 'provenance/24B119' / (key + '.json')
            provenance.parent.mkdir(parents=True)
            provenance.write_text(
                json.dumps(
                    {
                        'schema': 1,
                        'reply': key + '.reply',
                        'compilerUUID': uuid.hex(),
                        'requestType': 13,
                        'requestBytes': len(request),
                        'requestSHA256': hashlib.sha256(request).hexdigest(),
                        'replySHA256': hashlib.sha256(reply).hexdigest(),
                        'metallibSHA256': hashlib.sha256(output).hexdigest(),
                        'osBuild': '24B119',
                        'targetBuild': '24B119',
                    }
                )
            )
            self.assertEqual(harvest.replies(source.parent, store), 0)
            self.assertEqual((store / provenance.relative_to(source)).read_bytes(), provenance.read_bytes())
            self.assertEqual((store / (key + '.request')).read_bytes(), request)
            corpus = root / 'corpus.log'
            corpus.write_text('')
            with (
                mock.patch.object(harvest, 'DEVICE', root / 'packed'),
                mock.patch.object(harvest, 'embedded_libraries', return_value=set()),
            ):
                harvest.pack(store.parent, corpus)
            archive = next((root / 'packed').glob('*.tar'))
            with tarfile.open(archive) as packed:
                self.assertEqual(
                    set(packed.getnames()),
                    {
                        'msl/' + key + '.reply',
                        'msl/' + key + '.request',
                        'msl/provenance/24B119/' + key + '.json',
                    },
                )
                self.assertEqual(packed.extractfile('msl/' + key + '.request').read(), request)

    def test_harvest_refuses_conflicting_valid_reply(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, target = root / 'cache/msl', root / 'target'
            for folder, output in [(source, b'first'), (target, b'second')]:
                folder.mkdir(parents=True)
                data = b'NSC1' + struct.pack('<IQ', 0, len(output)) + hashlib.sha256(output).digest() + output
                (folder / ('a' * 64 + '.reply')).write_bytes(data)
            with self.assertRaisesRegex(ValueError, 'conflicting'):
                harvest.replies(source.parent, target)


if __name__ == '__main__':
    unittest.main()
