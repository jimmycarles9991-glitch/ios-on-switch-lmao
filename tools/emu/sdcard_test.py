import json
import struct
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from tools.emu import sdcard


class FilesCardTest(unittest.TestCase):
    def test_only_hybrid_type_changes_between_filesystems(self):
        plan = sdcard.layout(sdcard.NO_IMAGE_SECTORS)
        fat, fat_tail = sdcard.tables(plan)
        exfat, exfat_tail = sdcard.tables(plan, 'exfat')
        self.assertEqual(exfat[450], 7)
        self.assertEqual(fat[450], 12)
        self.assertEqual(fat[:450] + fat[451:], exfat[:450] + exfat[451:])
        self.assertEqual(fat_tail, exfat_tail)
        self.assertEqual(struct.unpack_from('<II', exfat, 454), (plan['fat'][0], sdcard.FAT_SECTORS))

    def test_exfat_seed_uses_run_head_and_rejects_unknown_filesystem(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            entry = root / 'shared-base'
            entry.mkdir()
            (entry / 'layout.json').write_text(json.dumps({'plan': sdcard.layout(1), 'ios': [[1, None, 0]]}))
            bootargs = root / 'bootargs.txt'
            bootargs.write_text('-v\n')
            files = {'switchroot/ios/bootargs.txt': bootargs}

            def image(run: Path, _files: dict[str, Path]) -> Path:
                target = run / 'card-fat.img'
                target.write_bytes(b'original volume')
                return target

            with (
                patch.object(sdcard, 'base', return_value=entry),
                patch.object(sdcard, '_make_exfat', side_effect=image),
            ):
                card = sdcard.card_for_run(None, root / 'run', files, 'exfat')
                descriptor = card.read_text()
                self.assertIn('FLAT "head.img"', descriptor)
                self.assertIn('FLAT "card-fat.img"', descriptor)
                self.assertIn(str(entry / 'tail.img'), descriptor)
            with self.assertRaises(ValueError):
                sdcard.card_for_run(None, root / 'bad', files, 'ntfs')


if __name__ == '__main__':
    unittest.main()
