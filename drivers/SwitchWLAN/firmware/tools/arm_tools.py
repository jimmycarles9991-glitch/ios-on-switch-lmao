"""The Arm GNU Toolchain and QEMU the chip's CPU tests run, from the runfiles :arm_tools brings."""

import os

from python.runfiles import runfiles

_RUNFILES = runfiles.Create()
_TOOLS = {
    'arm-none-eabi-gcc': 'arm_none_eabi/bin/arm-none-eabi-gcc',
    'arm-none-eabi-nm': 'arm_none_eabi/bin/arm-none-eabi-nm',
    'arm-none-eabi-objcopy': 'arm_none_eabi/bin/arm-none-eabi-objcopy',
    'arm-none-eabi-objdump': 'arm_none_eabi/bin/arm-none-eabi-objdump',
    'qemu-system-arm': 'qemu_arm/bin/qemu-system-arm',
}


def switch_tool(name: str) -> str:
    path = _RUNFILES.Rlocation(_TOOLS[name], source_repo='')
    if not path or not os.access(path, os.X_OK):
        raise FileNotFoundError(f'{name} is not in the runfiles ({path})')
    return path
