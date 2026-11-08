"""What still falls off the raw path, per boot: the sum of RawMetalRates.mm's 5 s rates lines.

Every 5 s (debug builds) each process with raw on logs
`TegraGM20BMetal: <process> raw 5s c=N(r/s) m2v=N(reason:n,...) ... why=pred:hit/alone,...`: c counts the command
buffers raw ran, m2v those it sent to m2v, the reasons why, and why= the ten predicates most hit in that window
(command buffers that hit it / those it alone sent to m2v). The row per boot adds them up, so a boot's predicate counts
cover only the windows where that predicate was in the top ten.
"""

import re
from collections import Counter, defaultdict

from tools.devicerun.rules import INFO, NO_DATA, REPORT, Check, Result

OWNER = 'r31-rawrt'
LINE = re.compile(
    r'TegraGM20BMetal: (?P<process>\S+) raw \d+s c=(?P<c>\d+)\([^)]*\) m2v=(?P<m2v>\d+)(?:\((?P<reasons>[^)]*)\))?'
)
WHY = re.compile(r' why=(?P<why>\S+)')
SHOWN = 8

CHECK = Check(
    'raw.falls-off',
    OWNER,
    'raw ',
    REPORT,
    subsystem='com.brunostjohn.switch.TegraGM20BMetal',
    regex=LINE.pattern,
    source='drivers/TegraGM20BMetal/raw/runtime/RawMetalRates.mm (raw_metal_rates)',
)


class Boot:
    def __init__(self) -> None:
        self.lines = 0
        self.raw = 0
        self.m2v = 0
        self.processes: Counter = Counter()
        self.reasons: Counter = Counter()
        self.hits: Counter = Counter()
        self.alone: Counter = Counter()

    def add(self, fields: dict) -> None:
        self.lines += 1
        self.raw += int(fields['c'])
        self.m2v += int(fields['m2v'])
        self.processes[fields['process']] += int(fields['m2v'])
        for name, count in pairs(fields.get('reasons') or ''):
            self.reasons[name] += int(count)
        why = WHY.search(fields['message'])
        for name, counts in pairs(why['why'] if why else ''):
            hit, _, alone = counts.partition('/')
            self.hits[name] += int(hit)
            self.alone[name] += int(alone or 0)

    def describe(self) -> str:
        total = self.raw + self.m2v
        share = f' ({100 * self.m2v / total:.1f}%)' if total else ''
        parts = [f'{self.lines} lines: raw {self.raw}, m2v {self.m2v}{share}']
        parts.append('reasons ' + (listed(self.reasons) or '-'))
        predicates = ', '.join(f'{p} {self.hits[p]}/{self.alone[p]}' for p, _ in self.hits.most_common(SHOWN))
        parts.append('predicates hit/alone ' + (predicates or '-'))
        parts.append('m2v by process ' + (listed(+self.processes) or '-'))
        return '; '.join(parts)


def pairs(text: str) -> list[tuple[str, str]]:
    return [tuple(item.rsplit(':', 1)) for item in text.split(',') if ':' in item]


def listed(counts: Counter) -> str:
    return ', '.join(f'{name} {n}' for name, n in counts.most_common(SHOWN))


def rows(result: Result) -> list[Result]:
    """Return one INFO row per boot that logged rates lines, in log order; NO-DATA when none did."""
    boots: dict[str, Boot] = defaultdict(Boot)
    for fields in result.matches:
        boots[fields['boot']].add(fields)
    if not boots:
        return [Result(CHECK, NO_DATA, 'no raw rates lines (release builds compile them out)')]
    return [
        Result(
            Check(f'{CHECK.name}.{boot or "-"}', OWNER, CHECK.contains, REPORT, source=CHECK.source), INFO, b.describe()
        )
        for boot, b in boots.items()
    ]
