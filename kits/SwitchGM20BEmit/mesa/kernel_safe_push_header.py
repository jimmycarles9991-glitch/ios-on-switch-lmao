"""Make Mesa's generated nv_push_cl*.h safe for the kernel.

class_parser.py's output is used unchanged: its push helpers stay byte for byte. Stdio, util/macros.h and the
method-dump declarations (FILE *, nv_push_cl*.c) go, and the class header is named from Mesa's headers directory
(SWITCH_GM20B_INCLUDES). Its users see it as a system header, as Mesa's build has it.
"""

import re
import sys


def kernel_safe(text: str, class_header: str) -> str:
    replacements = [
        ('#include <assert.h>\n', ''),
        ('#include <stdio.h>\n', ''),
        ('#include "util/macros.h"\n', '#include "nv_push_compat.h"\n'),
        (f'#include "{class_header}"\n', f'#include "nvidia/classes/{class_header}"\n'),
    ]
    for old, new in replacements:
        if old not in text:
            sys.exit(f"kernel_safe_push_header: Mesa's generated header has no {old.strip()!r}")
        text = text.replace(old, new)
    dumps = re.compile(
        r'\nconst char \*P_PARSE_\w+_MTHD\(uint16_t idx\) ATTRIBUTE_COLD;\n'
        r'void P_DUMP_\w+_MTHD_DATA\(FILE \*fp, uint16_t idx, uint32_t data,\n'
        r'\s+const char \*prefix\) ATTRIBUTE_COLD;\n'
    )
    text, count = dumps.subn('\n', text)
    if count != 1:
        sys.exit("kernel_safe_push_header: Mesa's generated header's dump declarations changed")
    guard = 'SWITCH_NV_PUSH_' + class_header.removesuffix('.h').upper() + '_H'
    return f'#ifndef {guard}\n#define {guard}\n#pragma clang system_header\n{text}#endif\n'


def main() -> None:
    source, out, class_header = sys.argv[1:]
    with open(source, encoding='utf-8') as f:
        text = f.read()
    with open(out, 'w', encoding='utf-8') as f:
        f.write(kernel_safe(text, class_header))


if __name__ == '__main__':
    main()
