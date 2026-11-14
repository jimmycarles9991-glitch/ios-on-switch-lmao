#!/usr/bin/env python3
"""raw/plan/m2v_routes.tsv is the closed list of every way work still reaches m2v.

Its product rows are exactly the routes m2v_routes_scan.py reads from the plugin's sources, each with the files that
raise it; a route with no row, a row whose route is gone and a row whose files moved all fail. The open rows of each
kind are pinned here and may only be lowered. A row that, once m2v is gone, returns Metal's error names the refusal line
it logs (an error line kept in release, kits/SwitchRefusal); a row main already closes must raise that line in its
files. The identifiers that name an m2v twin are counted and pinned too: after the cut the count is 0, and every twin
row left is raw's own form, which the rename step (S4) renames.
"""

import csv
import unittest
from collections import Counter

from m2v_routes_scan import ROOT, Census

TABLE = ROOT / 'drivers/TegraGM20BMetal/raw/plan/m2v_routes.tsv'
COLUMNS = ('kind', 'route', 'name', 'raised_in', 'gone', 'logs', 'owner', 'status', 'boot21', 'note')
KINDS = ('product', 'test', 'tool')
GONE = ('native', 'form', 'error', 'dead', 'none')
# Open rows per kind: lower these as rows close, never raise them
OPEN = {'product': 124, 'test': 31, 'tool': 4}
# Identifiers naming an m2v twin in the plugin's code (route twin, gone other than none): the cut leaves none
M2V_TWINS = 54


def table() -> list[dict[str, str]]:
    with TABLE.open() as f:
        lines = [line for line in f if line.strip() and not line.startswith('#')]
    return list(csv.DictReader(lines, delimiter='\t', quoting=csv.QUOTE_NONE))


class Routes(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.rows = table()
        cls.census = Census()

    def test_columns(self) -> None:
        for row in self.rows:
            self.assertEqual(tuple(row), COLUMNS, row)
            self.assertNotIn(None, row.values(), row)
            self.assertTrue(all(v.strip() for v in row.values()), row)

    def test_vocabulary(self) -> None:
        for row in self.rows:
            where = f'{row["route"]} {row["name"]}'
            self.assertIn(row['kind'], KINDS, where)
            self.assertIn(row['gone'], GONE, where)
            status = row['status']
            self.assertTrue(status in ('open', 'cut', 'main') or '@' in status, f'{where}: status {status}')
            if row['gone'] == 'error':
                self.assertRegex(row['logs'], r'^[a-z]+:[a-z0-9_.]+$', f'{where}: an error row names its line')

    def test_each_route_once(self) -> None:
        seen = Counter((row['route'], row['name']) for row in self.rows)
        self.assertEqual([key for key, n in seen.items() if n > 1], [])

    def test_product_rows_are_the_sources_routes(self) -> None:
        rows = {(r['route'], r['name']): r['raised_in'] for r in self.rows if r['kind'] == 'product'}
        found = {key: ','.join(sorted(files)) for key, files in self.census.routes.items()}
        self.assertEqual(sorted(set(found) - set(rows)), [], 'routes in the sources with no row')
        self.assertEqual(sorted(set(rows) - set(found)), [], 'rows of routes the sources no longer have')
        moved = {key: (rows[key], found[key]) for key in rows.keys() & found.keys() if rows[key] != found[key]}
        self.assertEqual(moved, {}, 'rows whose files are not the files that raise the route (row, sources)')

    def test_every_route_has_a_site(self) -> None:
        empty = sorted(key for key, files in self.census.routes.items() if not files)
        self.assertEqual(empty, [], 'a predicate, need or kernel entry nothing raises: delete it')

    def test_open_rows_pinned(self) -> None:
        open_rows = Counter(row['kind'] for row in self.rows if row['status'] == 'open')
        self.assertEqual({kind: open_rows.get(kind, 0) for kind in KINDS}, OPEN)

    def test_m2v_twins_pinned(self) -> None:
        product = [r for r in self.rows if r['kind'] == 'product' and r['route'] == 'twin']
        twins = [r['name'] for r in product if r['gone'] != 'none']
        self.assertEqual(len(twins), M2V_TWINS, twins)

    def test_closed_error_rows_log_their_line(self) -> None:
        tags = {name: files for (route, name), files in self.census.routes.items() if route == 'tag'}
        tags.update({'raw:' + name: files for (route, name), files in self.census.routes.items() if route == 'pred'})
        for row in self.rows:
            if row['kind'] != 'product' or row['gone'] != 'error' or row['status'] != 'main' or row['route'] == 'sel':
                continue
            files = set(row['raised_in'].split(','))
            self.assertTrue(
                tags.get(row['logs'], set()) & files,
                f'{row["route"]} {row["name"]}: closed, but {row["logs"]} is not raised in {files}',
            )


if __name__ == '__main__':
    unittest.main()
