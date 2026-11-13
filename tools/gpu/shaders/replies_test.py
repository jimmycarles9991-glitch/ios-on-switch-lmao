import hashlib
import json
import struct
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from tools.gpu.shaders import replies


def reply(output: bytes = b'MTLB fixture', warnings: bytes = b'warning') -> bytes:
    body = warnings + output
    return b'NSC1' + struct.pack('<IQ', len(warnings), len(output)) + hashlib.sha256(body).digest() + body


def capture(
    root: Path, os_build: str = '24B119', target: str = '24B119', kind: int = 13, source: bytes = b'captured source\0'
) -> tuple[str, Path, dict]:
    uuid = bytes(range(16))
    key = replies.sha(uuid + struct.pack('<I', kind) + source)
    name = key + '.reply'
    output = b'MTLB fixture'
    data = reply(output)
    root.mkdir(parents=True, exist_ok=True)
    (root / name).write_bytes(data)
    (root / (key + '.request')).write_bytes(source)
    record = {
        'schema': 1,
        'reply': name,
        'compilerUUID': uuid.hex(),
        'requestType': kind,
        'requestSHA256': replies.sha(source),
        'requestBytes': len(source),
        'replySHA256': replies.sha(data),
        'metallibSHA256': replies.sha(output),
        'osBuild': os_build,
        'targetBuild': target,
    }
    path = root / 'provenance' / os_build / (key + '.json')
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(record))
    return name, path, record


class Replies(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name) / 'msl'

    def test_legacy_retained_but_unattributed(self):
        self.root.mkdir()
        (self.root / ('a' * 64 + '.reply')).write_bytes(reply())
        report = replies.inventory(self.root, strict=True)
        self.assertEqual(report['replies'], 1)
        self.assertEqual(report['unattributedReplies'], 1)
        self.assertEqual(report['perOS'], {})
        self.assertEqual(report['unseenRequests'], 'unknown')

    def test_same_request_observed_on_two_os_builds(self):
        capture(self.root, '21H461', '21H461')
        capture(self.root, '24B119')
        report = replies.inventory(self.root, strict=True)
        self.assertEqual(report['replies'], 1)
        self.assertEqual(report['attributedReplies'], 1)
        self.assertEqual(report['unattributedReplies'], 0)
        self.assertEqual({key: value['requests'] for key, value in report['perOS'].items()}, {'21H461': 1, '24B119': 1})
        self.assertEqual(len(report['files']), 4)

    def test_actual_host_os_separate_from_selected_target(self):
        capture(self.root, '25A123', '24B119')
        report = replies.inventory(self.root, strict=True)
        self.assertEqual(report['perOS']['25A123']['targetBuilds'], ['24B119'])
        self.assertNotIn('24B119', report['perOS'])

    def test_request_key_recomputed_even_if_request_hash_updated(self):
        name, path, record = capture(self.root)
        replacement = b'changed bytes'
        (self.root / name.replace('.reply', '.request')).write_bytes(replacement)
        record.update(requestSHA256=replies.sha(replacement), requestBytes=len(replacement))
        path.write_text(json.dumps(record))
        with self.assertRaisesRegex(ValueError, 'reproduce reply key'):
            replies.inventory(self.root, strict=True)

    def test_each_provenance_contract_fails_closed(self):
        _name, path, record = capture(self.root)
        for key, value in [
            ('schema', 2),
            ('schema', True),
            ('compilerUUID', '0' * 32),
            ('requestType', -1),
            ('requestType', 1 << 32),
            ('requestType', True),
            ('requestBytes', replies.REQUEST_LIMIT + 1),
            ('requestBytes', 999),
            ('requestSHA256', '0' * 64),
            ('replySHA256', '0' * 64),
            ('metallibSHA256', '0' * 64),
            ('osBuild', '../escape'),
            ('targetBuild', ''),
        ]:
            with self.subTest(key=key, value=value):
                path.write_text(json.dumps(record | {key: value}))
                with self.assertRaises(ValueError):
                    replies.inventory(self.root, strict=True)
        path.write_text(json.dumps(record))
        self.assertTrue(replies.inventory(self.root, strict=True)['inputComplete'])

    def test_duplicate_metadata_fields_are_refused(self):
        _, path, record = capture(self.root)
        path.write_text(json.dumps(record)[:-1] + ', "requestType": 13}')
        with self.assertRaisesRegex(ValueError, 'duplicate provenance field'):
            replies.inventory(self.root, strict=True)

    def test_declared_input_missing_or_unlisted_is_refused(self):
        capture(self.root)
        expected = sorted(p for p in self.root.rglob('*') if p.is_file())
        self.assertTrue(replies.inventory(self.root, expected, strict=True)['inputComplete'])
        with self.assertRaisesRegex(ValueError, 'absent from declared'):
            replies.inventory(self.root, expected[:-1], strict=True)
        with self.assertRaisesRegex(ValueError, 'declared input missing'):
            replies.inventory(self.root, [*expected, self.root / 'missing.reply'], strict=True)

    def test_missing_request_or_reply_refused(self):
        name, _, _ = capture(self.root)
        source = self.root / name.replace('.reply', '.request')
        data = source.read_bytes()
        source.unlink()
        with self.assertRaises(ValueError):
            replies.inventory(self.root, strict=True)
        source.write_bytes(data)
        (self.root / name).unlink()
        with self.assertRaises(ValueError):
            replies.inventory(self.root, strict=True)

    def test_uncommitted_and_unknown_files_not_silently_dropped(self):
        self.root.mkdir()
        for name in ['a.request', 'unknown', 'nested/a.reply']:
            with self.subTest(name=name):
                path = self.root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(b'partial')
                with self.assertRaises(ValueError):
                    replies.inventory(self.root, strict=True)
                path.unlink()

    def test_interrupted_capture_is_retained_without_os_attribution(self):
        self.root.mkdir()
        name = 'a' * 64
        (self.root / (name + '.reply')).write_bytes(reply())
        (self.root / (name + '.request')).write_bytes(b'bounded source')
        (self.root / (name + '.reply.123.tmp')).write_bytes(b'incomplete reply')
        report = replies.inventory(self.root, strict=True)
        self.assertTrue(report['inputComplete'])
        self.assertEqual(report['unattributedReplies'], 1)
        self.assertEqual(report['attributedReplies'], 0)
        self.assertEqual(report['perOS'], {})
        self.assertEqual(len(report['pending']), 2)
        target = self.root.parent / 'target'
        replies.copy_inventory(self.root, target, report)
        replies.verify(target, report)
        self.assertEqual((target / (name + '.request')).read_bytes(), b'bounded source')

    def test_pending_capture_still_obeys_size_bound(self):
        self.root.mkdir()
        path = self.root / ('a' * 64 + '.request')
        with path.open('wb') as output:
            output.truncate(replies.REQUEST_LIMIT + 1)
        with self.assertRaisesRegex(ValueError, 'exceeds size bound'):
            replies.inventory(self.root, strict=True)

    def test_bad_envelope_or_path_is_refused(self):
        name, path, _record = capture(self.root)
        original = (self.root / name).read_bytes()
        for content in [original[:-1], original + b'?', b'bad', original[:16] + b'0' * 32 + original[48:]]:
            (self.root / name).write_bytes(content)
            with self.assertRaises(ValueError):
                replies.inventory(self.root, strict=True)
        (self.root / name).write_bytes(original)
        path.rename(path.with_name('b' * 64 + '.json'))
        with self.assertRaisesRegex(ValueError, 'provenance path'):
            replies.inventory(self.root, strict=True)

    def test_copy_conflict_checked_before_any_publication(self):
        name, _, _ = capture(self.root)
        report = replies.inventory(self.root, strict=True)
        target = self.root.parent / 'target'
        target.mkdir()
        (target / name).write_bytes(reply(b'other output'))
        with self.assertRaisesRegex(ValueError, 'conflicting'):
            replies.copy_inventory(self.root, target, report)
        self.assertEqual(list(target.iterdir()), [target / name])

    def test_copy_changed_source_refused(self):
        name, _, _ = capture(self.root)
        report = replies.inventory(self.root, strict=True)
        (self.root / name).write_bytes(reply(b'other output'))
        with self.assertRaisesRegex(ValueError, 'input changed'):
            replies.copy_inventory(self.root, self.root.parent / 'target', report)

    def test_interrupted_copy_never_commits_metadata_before_source(self):
        name, metadata, _ = capture(self.root)
        report = replies.inventory(self.root, strict=True)
        target = self.root.parent / 'target'
        replace = replies.os.replace

        def interrupted(source: str, destination: Path) -> None:
            if destination.suffix == '.json':
                raise OSError('interrupted before metadata commit')
            replace(source, destination)

        with mock.patch.object(replies.os, 'replace', side_effect=interrupted), self.assertRaises(OSError):
            replies.copy_inventory(self.root, target, report)
        partial = replies.inventory(target, strict=True)
        self.assertEqual(partial['unattributedReplies'], 1)
        self.assertEqual(len(partial['pending']), 1)
        self.assertEqual(partial['perOS'], {})
        self.assertFalse((target / metadata.relative_to(self.root)).exists())
        self.assertTrue((target / name.replace('.reply', '.request')).is_file())
        replies.copy_inventory(self.root, target, report)
        replies.verify(target, report)
        self.assertEqual(replies.inventory(target, strict=True)['attributedReplies'], 1)

    def test_installed_exact_inventory_verified(self):
        name, _, _ = capture(self.root)
        report = replies.inventory(self.root, strict=True)
        target = self.root.parent / 'target'
        self.assertEqual(replies.copy_inventory(self.root, target, report), 1)
        self.assertEqual(replies.copy_inventory(self.root, target, report), 0)
        replies.verify(target, report)
        (target / ('b' * 64 + '.reply')).write_bytes(reply())
        with self.assertRaisesRegex(ValueError, 'differ'):
            replies.verify(target, report)
        (target / ('b' * 64 + '.reply')).unlink()
        (target / name).write_bytes(reply(b'changed'))
        with self.assertRaises(ValueError):
            replies.verify(target, report)


if __name__ == '__main__':
    unittest.main()
