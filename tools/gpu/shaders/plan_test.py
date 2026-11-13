"""A corpus line changes only its program's bucket: every other bucket keeps its bytes, so Bazel keeps its compile."""

import unittest

from tools.gpu.shaders import plan

LIB = '/System/Library/Frameworks/X.framework/default.metallib'


def _render(vertex: str, fragment: str, target: str = 'samples=1 topo=0 ds=0/0 c0=80:0:0:1:0:0:1:0:0') -> str:
    return f'R ok=1 label=- {target} | v {vertex} lib={LIB} | f {fragment} lib={LIB} | '


CORPUS = [_render(f'v{i}', f'f{i}') for i in range(40)] + [
    f'S ok=1 lib={LIB} name=f{i} k=#0:53:0{i % 2} | ' for i in range(20)
]


def _buckets(corpus: list[str], count: int = 16) -> list[list[str]]:
    return plan.bucket(plan.lines_for('corpus', corpus, {}), count)[0]


class BucketTest(unittest.TestCase):
    def test_an_appended_line_rewrites_only_its_programs_bucket(self):
        before = _buckets(CORPUS)
        line = _render('v3', 'f3', 'samples=4 topo=0 ds=0/0 c0=80:0:0:1:0:0:1:0:0')
        after = _buckets([*CORPUS, line])
        changed = [i for i, (old, new) in enumerate(zip(before, after, strict=True)) if old != new]
        self.assertEqual(len(changed), 1)
        self.assertIn(line, after[changed[0]])
        self.assertIn(_render('v3', 'f3'), after[changed[0]], 'a program keeps its pipelines in one bucket')

    def test_the_order_of_the_corpus_does_not_move_lines_between_buckets(self):
        self.assertEqual([sorted(b) for b in _buckets(CORPUS)], [sorted(b) for b in _buckets(list(reversed(CORPUS)))])

    def test_a_repeated_line_is_made_once(self):
        self.assertEqual(_buckets(CORPUS), _buckets([*CORPUS, CORPUS[0], CORPUS[-1]]))

    def test_linked_function_variants_are_not_deduplicated(self):
        lines = [
            _render('v', 'f') + f'{tag} helper{i} lib={LIB} | ' for tag in ('vl', 'vp', 'fl', 'fp') for i in range(2)
        ]
        buckets = _buckets(lines)
        self.assertEqual(sorted(line for bucket in buckets for line in bucket if line != plan.DERIVED), sorted(lines))
        self.assertEqual(len({plan._program(line) for line in lines}), len(lines))

    def test_specialized_entry_names_are_part_of_the_key(self):
        lines = [_render('v', 'f as=' + name) for name in ('first', 'second')]
        self.assertEqual(sum(len(b) - 1 for b in _buckets(lines)), 2)
        self.assertNotEqual(plan._program(lines[0]), plan._program(lines[1]))

    def test_specialization_records_keep_their_entry_name(self):
        lines = [f'S ok=1 lib={LIB} name=f k=#0:53:01 as={name} | ' for name in ('first', 'second')]
        self.assertEqual(sum(len(b) - 1 for b in _buckets(lines)), 2)
        self.assertNotEqual(plan._program(lines[0]), plan._program(lines[1]))

    def test_an_apps_lines_go_to_the_apps_buckets_and_move_no_other_line(self):
        app = '/Applications/Fixture.app/default.metallib'
        lines = [
            _render('va', 'fa').replace(f'f fa lib={LIB}', f'f fa lib={app}'),
            f'C ok=1 form=f opt=0 | k probe lib={app} | ',
        ]
        without = _buckets(CORPUS)
        with_apps = plan.bucket(plan.lines_for('corpus', [*CORPUS, *lines], {}), 16, 2)[0]
        self.assertEqual(with_apps[:16], without)
        self.assertEqual(sorted(line for b in with_apps[16:] for line in b if line != plan.DERIVED), sorted(lines))

    def test_compute_linked_functions_are_part_of_the_key(self):
        lines = [
            f'C ok=1 form=d opt=0 | k kernel lib={LIB} | {tag} helper{i} lib={LIB} | '
            for tag in ('l', 'p')
            for i in range(2)
        ]
        self.assertEqual(sum(len(b) - 1 for b in _buckets(lines)), 4)
        self.assertEqual(len({plan._program(line) for line in lines}), 4)


if __name__ == '__main__':
    unittest.main()
