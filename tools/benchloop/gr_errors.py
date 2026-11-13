"""The kernel's GR error lines, each as one readable line.

`gr: DATA_ERROR 000000b5 [] ch 35 ... mthd 0d78 data 00000004` and `gr: GPC0/TPC0/MP trap ... [ILLEGAL_INSTR_ENCODING]`.
The kernel names only the class errors nouveau's table has; gr_class_errors.txt names the rest.
"""

import re
from functools import cache
from pathlib import Path

TABLE = Path(__file__).with_name('gr_class_errors.txt')
DATA_ERROR = re.compile(r'gr: DATA_ERROR ([0-9a-f]{8}) .*?class ([0-9a-f]+) mthd ([0-9a-f]+) data ([0-9a-f]+)')
TRAP = re.compile(r'gr: (\S+ trap\b.*)')


@cache
def names() -> dict[int, str]:
    table = {}
    for line in TABLE.read_text().splitlines():
        if line and not line.startswith('#'):
            code, name = line.split()
            table[int(code, 16)] = name
    return table


def is_gr_line(line: str) -> bool:
    return 'gr: ' in line


def decode(line: str) -> str | None:
    """Give a GR error line decoded, a trap line as its trap text, anything else as None.

    `DATA_ERROR 0xb5 ILLEGAL_SHADER_REGISTER_COUNT1 at class b197 method 0x0d78 data 0x00000004`
    """
    if m := DATA_ERROR.search(line):
        code, method = int(m.group(1), 16), int(m.group(3), 16)
        name = names().get(code, 'unnamed')
        return f'DATA_ERROR {code:#x} {name} at class {m.group(2)} method {method:#06x} data 0x{m.group(4)}'
    if m := TRAP.search(line):
        return m.group(1).strip()
    return None
