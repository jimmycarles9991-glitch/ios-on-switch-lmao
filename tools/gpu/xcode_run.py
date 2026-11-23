#!/usr/bin/env python3
"""Run an Xcode tool from the pinned Xcode.

usage: xcode_run.py <xcode.env> [--cwd-map] [--env NAME=VALUE ...] -- <tool> [args...]

Runs an Xcode tool (`clang`, `clang++`, `strip`, `dsymutil`, `codesign`, ...) from the pinned Xcode named by
@xcode//:xcode.env, with DEVELOPER_DIR set, `@IPHONEOS_SDK@`/`@MACOSX_SDK@`/`@PWD@`/`@FILE:<path>@` replaced in the
arguments, and ZERO_AR_DATE=1 (no object/archive timestamps in a link). A bare tool name is looked up in the Xcode
toolchain, then /usr/bin (codesign lives there).
"""

import os
import subprocess
import sys
from pathlib import Path


def main() -> None:
    argv = sys.argv[1:]
    split = argv.index('--')
    options, command = argv[:split], argv[split + 1 :]
    xcode = dict(line.split('=', 1) for line in Path(options[0]).read_text().splitlines() if '=' in line)
    env = {
        'PATH': f'{xcode["XCODE_TOOLCHAIN"]}:/usr/bin:/bin:/usr/sbin:/sbin',
        'DEVELOPER_DIR': xcode['DEVELOPER_DIR'],
        'ZERO_AR_DATE': '1',
        'TMPDIR': os.environ.get('TMPDIR', '/tmp'),
        'HOME': os.environ.get('HOME', '/var/empty'),
    }
    for option in options[1:]:
        if option.startswith('NAME=') or '=' not in option:
            sys.exit('bad option ' + option)
        name, value = option.split('=', 1)
        env[name] = value
    replace = {'@IPHONEOS_SDK@': xcode['IPHONEOS_SDK'], '@MACOSX_SDK@': xcode['MACOSX_SDK'], '@PWD@': os.getcwd()}
    for i, original in enumerate(command):
        arg = original
        for key, value in replace.items():
            arg = arg.replace(key, value)
        # @FILE:<path>@ is the file's content (stripped): a value an earlier action computed, e.g. the stack key.
        while '@FILE:' in arg:
            start = arg.index('@FILE:')
            end = arg.index('@', start + 6)
            arg = arg[:start] + Path(arg[start + 6 : end]).read_text().strip() + arg[end + 1 :]
        command[i] = arg
    tool = command[0]
    if '/' not in tool:
        for directory in (xcode['XCODE_TOOLCHAIN'], '/usr/bin'):
            if Path(directory, tool).is_file():
                command[0] = str(Path(directory, tool))
                break
    sys.exit(subprocess.run(command, env=env, check=False).returncode)


if __name__ == '__main__':
    main()
