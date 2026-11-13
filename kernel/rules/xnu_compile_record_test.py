import tempfile
import unittest
from pathlib import Path

from kernel.rules import xnu_compile_record as record


class Places:
    def spell(self, value: str, _: str | None) -> str:
        return value.replace('/record/obj/RELEASE_ARM64_BCM2837', '{objdir}')


class LinkRecordingTest(unittest.TestCase):
    def parse(self, source: str) -> record.LinkRecipes:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'make.log'
            path.write_text(source)
            return record.link(str(path), Places())

    def test_development_keeps_archive_boundary(self):
        result = self.parse(
            '/usr/bin/clang++ -nostdlib -filelist link.filelist '
            '-o mach.development.bcm2837.unstripped.noctf -lcc_kext /extra/archive.a'
        )
        self.assertIsNone(result.lto)
        self.assertEqual(result.final[-2:], ['-lcc_kext', '@KERNEL_ARCHIVES@'])
        self.assertNotIn('/extra/archive.a', result.final)

    def test_release_extracts_both_links_and_selects_single_object_branch(self):
        result = self.parse(r"""unrelated diagnostic: unmatched " quote
if [ -s ltolink.filelist ]; then \
 /usr/bin/clang++ -nostdlib -arch arm64 -r nonlto.o -filelist ltolink.filelist \
 -Wl,-object_path_lto,/record/obj/RELEASE_ARM64_BCM2837/justlto.o \
 -o /record/obj/RELEASE_ARM64_BCM2837/justlto.tmp.o && \
 if test -d /record/obj/RELEASE_ARM64_BCM2837/justlto.o; then \
 /usr/bin/clang++ -nostdlib -filelist nonltolink.filelist /record/obj/RELEASE_ARM64_BCM2837/justlto.o/*.o \
 -o mach.release.bcm2837.unstripped.noctf -lcc_kext lib.a; \
 else \
 /usr/bin/clang++ -nostdlib -filelist nonltolink.filelist /record/obj/RELEASE_ARM64_BCM2837/justlto.o \
 -o mach.release.bcm2837.unstripped.noctf -lcc_kext lib.a; \
 fi; \
 else \
 /usr/bin/clang++ -nostdlib -filelist nonltolink.filelist \
 -o mach.release.bcm2837.unstripped.noctf -lcc_kext lib.a; \
 fi
""")
        self.assertIn('ltolink.filelist', result.lto)
        self.assertIn('-r', result.lto)
        self.assertIn('{objdir}/justlto.o', result.final)
        self.assertNotIn('{objdir}/justlto.o/*.o', result.final)
        self.assertEqual(result.final[-1], '@KERNEL_ARCHIVES@')
        output = record.recipes_text('RELEASE', {}, result)
        self.assertIn('RELEASE_LINK_LTO = [', output)
        self.assertIn('RELEASE_LINK = [', output)

    def test_xcrun_prefix_and_quoted_paths(self):
        result = self.parse(
            'xcrun -sdk iphoneos "/path with spaces/clang++" -nostdlib '
            '-filelist link.filelist -o mach.release.unstripped -lcc_kext'
        )
        self.assertEqual(result.final[0], '-nostdlib')

    def test_physical_continuations_are_joined_before_parsing(self):
        command = 'clang++ -nostdlib ' + chr(92) + chr(10)
        command += '-filelist link.filelist -o mach.release.unstripped -lcc_kext'
        self.assertEqual(self.parse(command).final[0], '-nostdlib')

    def test_missing_final_link_is_rejected(self):
        with self.assertRaises(SystemExit):
            self.parse('clang++ -r -filelist ltolink.filelist -o justlto.tmp.o')

    def test_different_final_links_are_rejected(self):
        with self.assertRaises(SystemExit):
            self.parse(
                'clang++ -filelist link.filelist -o mach.one.unstripped -lcc_kext\n'
                'clang++ -filelist link.filelist -o mach.two.unstripped -lcc_kext'
            )

    def test_repeated_identical_link_is_one_recipe(self):
        command = 'clang++ -filelist link.filelist -o mach.release.unstripped -lcc_kext'
        self.assertIsNone(self.parse(command + '\n' + command).lto)

    def test_development_output_has_no_lto_definition(self):
        output = record.recipes_text('DEVELOPMENT', {}, record.LinkRecipes(None, ['-nostdlib']))
        self.assertNotIn('LINK_LTO', output)
        self.assertIn('DEVELOPMENT_LINK = [', output)

    def test_truncated_final_output_is_rejected(self):
        with self.assertRaisesRegex(SystemExit, 'expected one kernel link'):
            self.parse('clang++ -filelist link.filelist -o')

    def test_lto_must_be_relocatable(self):
        with self.assertRaisesRegex(SystemExit, 'not relocatable'):
            self.parse('clang++ -filelist ltolink.filelist -o justlto.tmp.o')

    def test_unfinished_continuation_is_rejected(self):
        with self.assertRaisesRegex(SystemExit, 'unfinished shell continuation'):
            self.parse('clang++ -filelist link.filelist ' + chr(92))


if __name__ == '__main__':
    unittest.main()
