"""The direct install's container writer against a plain file standing in for the partition (no device)."""

import contextlib
import hashlib
import io
import os
import random
import shutil
import struct
import sys
import tempfile
import unittest
import uuid
from pathlib import Path
from unittest import mock

from tools.install import apfs, card as cards, direct, direct_stream as streams, folder, manifest, pack

MIB = 1 << 20
CHUNK = manifest.HASH_CHUNK
ZERO = manifest.ZERO
CONTAINER = uuid.UUID('6f1c1a52-0d55-4bd5-9a3e-0c1f00b2a7e1')
EXTENTS = [(0, 0, 4 * MIB), (4 * MIB, ZERO, 2 * MIB), (8 * MIB, 4 * MIB, 12 * MIB), (24 * MIB, ZERO, MIB)]
EXTENTS += [(32 * MIB, 16 * MIB, 8 * MIB)]
PARTITION = 48 * MIB
BOOT = {'bl31.bin': b'trusted firmware\n' * 100, 'bl33.bin': b'kernel' * 5000}


def superblock(container: uuid.UUID, blocks: int) -> bytes:
    block = bytearray(4096)
    block[32:36] = apfs.NX_MAGIC
    struct.pack_into('<IQ', block, 36, 4096, blocks)
    block[72:88] = container.bytes
    return apfs.with_checksum(bytes(block))


def make(root: Path, name: str = 'IOS99', container: uuid.UUID = CONTAINER) -> tuple[manifest.Manifest, bytes]:
    """NAME.SII.* under `root`: a 24 MiB body (three chunks, across two parts) for EXTENTS, and the BOOT files."""
    body = superblock(CONTAINER, (1 << 30) // 4096) + random.Random(name).randbytes(24 * MIB - 4096)
    directory = root / folder.SII_DIR
    directory.mkdir(parents=True)
    sources = []
    for file_name, data in BOOT.items():
        (root / file_name).write_bytes(data)
        sources.append((file_name, root / file_name))
    sizes = manifest.split(manifest.BODY_OFFSET + len(body), 80 * MIB)
    writer = pack.Writer(directory, name, sizes)
    writer.write(body)
    writer.close()
    hashes = [hashlib.sha256(body[i : i + CHUNK]).digest() for i in range(0, len(body), CHUNK)]
    parts = [manifest.Part(f'{folder.SII_DIR}/{pack.part_name(name, i)}', n) for i, n in enumerate(sizes)]
    m = manifest.Manifest(1 << 30, PARTITION, 4096, container, f'{name} test', parts, EXTENTS, hashes)
    m.boot = pack.boot_records(sources)
    pack.write_head(directory, m, sources)
    return m, body


def flip(root: Path, stream_offset: int, name: str = 'IOS99') -> None:
    """Change one byte of the image at `stream_offset` (in .001 or .002)."""
    first = root / folder.SII_DIR / f'{name}.SII.001'
    path, at = (first, stream_offset) if stream_offset < first.stat().st_size else (None, 0)
    if path is None:
        path, at = root / folder.SII_DIR / f'{name}.SII.002', stream_offset - first.stat().st_size
    with open(path, 'r+b') as f:
        f.seek(at)
        byte = f.read(1)
        f.seek(at)
        f.write(bytes([byte[0] ^ 0xFF]))


class WriterTest(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.root)
        self.before = random.Random(7).randbytes(PARTITION)
        self.partition = self.root / 'partition'
        self.partition.write_bytes(self.before)

    def install(self, m: manifest.Manifest, size: int = PARTITION) -> int:
        stream = streams.Stream(self.root / folder.SII_DIR, m)
        fd = os.open(self.partition, os.O_RDWR)
        try:
            return streams.install(m, stream, fd, size, lambda _n: None)
        finally:
            os.close(fd)
            stream.close()

    def test_rebuilds_the_container_and_zeroes_the_zero_extents(self):
        m, body = make(self.root)
        self.assertEqual(self.install(m), m.write_bytes)
        after = self.partition.read_bytes()
        covered = 0
        for at, source, n in EXTENTS:
            want = bytes(n) if source == ZERO else body[source : source + n]
            self.assertEqual(after[at : at + n], want, f'extent at {at:#x}')
            covered = at + n
        self.assertEqual(after[6 * MIB : 8 * MIB], self.before[6 * MIB : 8 * MIB])
        self.assertEqual(after[covered:], self.before[covered:])

    def test_a_chunk_that_does_not_match_is_never_written(self):
        m, _ = make(self.root)
        flip(self.root, manifest.BODY_OFFSET + CHUNK + 5)
        with self.assertRaisesRegex(streams.StreamError, 'chunk 1'):
            self.install(m)
        after = self.partition.read_bytes()
        self.assertNotEqual(after[: 4 * MIB], self.before[: 4 * MIB])
        self.assertEqual(after[12 * MIB :], self.before[12 * MIB :])

    def test_the_first_block_must_be_the_manifests_container(self):
        m, _ = make(self.root, container=uuid.uuid4())
        with self.assertRaisesRegex(streams.StreamError, 'first block'):
            self.install(m)
        self.assertEqual(self.partition.read_bytes(), self.before)

    def test_refuses_a_partition_smaller_than_the_image_before_writing(self):
        m, _ = make(self.root)
        with self.assertRaisesRegex(streams.StreamError, 'partition has'):
            self.install(m, 32 * MIB)
        self.assertEqual(self.partition.read_bytes(), self.before)

    def test_refuses_a_missing_part(self):
        m, _ = make(self.root)
        (self.root / folder.SII_DIR / 'IOS99.SII.002').unlink()
        with self.assertRaisesRegex(streams.StreamError, 'missing'):
            streams.Stream(self.root / folder.SII_DIR, m)


class BootFilesTest(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.root)

    def test_reads_the_boot_files_from_the_head(self):
        m, _ = make(self.root)
        stream = streams.Stream(self.root / folder.SII_DIR, m)
        self.addCleanup(stream.close)
        self.assertEqual(dict(streams.boot_files(stream, m)), BOOT)
        self.assertTrue(all(streams.matches(BOOT[b.name], b) for b in m.boot))

    def test_refuses_a_boot_file_that_does_not_match(self):
        m, _ = make(self.root)
        flip(self.root, m.boot[1].offset + 3)
        stream = streams.Stream(self.root / folder.SII_DIR, m)
        self.addCleanup(stream.close)
        with self.assertRaisesRegex(streams.StreamError, 'bl33.bin'):
            streams.boot_files(stream, m)


class ImageTest(unittest.TestCase):
    def test_needs_a_name_when_the_folder_holds_several(self):
        root = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, root)
        make(root, 'IOS17')
        make(root / 'other', 'IOS27')
        for path in (root / 'other' / folder.SII_DIR).iterdir():
            path.rename(root / folder.SII_DIR / path.name)
        with self.assertRaises(cards.RefusedError):
            direct.image(root, None)
        name, m = direct.image(root, 'IOS27')
        self.assertEqual((name, m.label), ('IOS27', 'IOS27 test'))


class ReexecTest(unittest.TestCase):
    def test_reruns_itself_under_sudo_with_absolute_inputs_and_the_confirmation(self):
        args = direct.parse(['disk7', '--yes', '--out', 'relative/sd', '--apfs-tool', '/tools/apfs_tool'])
        args.confirm, args.image = 'disk7', 'IOS27'
        with (
            mock.patch.object(direct.os, 'execvp', side_effect=SystemExit) as execvp,
            contextlib.redirect_stdout(io.StringIO()),
            self.assertRaises(SystemExit),
        ):
            direct.reexec_as_root(args)
        command = execvp.call_args.args[1]
        self.assertEqual(
            command[:6], ['sudo', '--', sys.executable, os.path.abspath(direct.__file__), 'disk7', '--yes']
        )
        flags = dict(flag.split('=', 1) for flag in command[6:])
        self.assertEqual(set(flags), {'--confirm', '--out', '--image', '--apfs-tool', '--log'})
        self.assertEqual(flags['--confirm'], 'disk7')
        self.assertEqual(flags['--image'], 'IOS27')
        self.assertEqual(flags['--out'], os.path.abspath('relative/sd'))
        self.assertTrue(os.path.isabs(flags['--log']))
        args.partition, args.no_eject = 'userdata', True
        with mock.patch.object(direct.os, 'execvp') as execvp, contextlib.redirect_stdout(io.StringIO()):
            direct.reexec_as_root(args)
        self.assertEqual(execvp.call_args.args[1][-2:], ['--log=' + flags['--log'], '--no-eject'])
        self.assertIn('--partition=userdata', execvp.call_args.args[1])


if __name__ == '__main__':
    unittest.main()
