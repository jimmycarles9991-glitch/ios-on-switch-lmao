"""Express m2v's half conversions using the R32 entry point's supported float controls."""

import argparse
import re
import subprocess
from pathlib import Path


def check_conversions(code: str, rounding: re.Pattern) -> None:
    definitions = dict(re.findall(r'^\s*(%\d+) = ([^\n]+)', code, re.MULTILINE))
    for result, mode in rounding.findall(code):
        instruction, target, *_ = definitions[result].split()
        ty = definitions[target].split()
        if ty[0] == 'OpTypeVector':
            ty = definitions[ty[1]].split()
        if mode != 'RTE' or instruction != 'OpFConvert' or ty != ['OpTypeFloat', '16']:
            raise ValueError(f'unhandled rounding on {result}')


def convert(source: Path, dest: Path) -> None:
    code = subprocess.check_output(['spirv-dis', '--raw-id', source], text=True)
    entries = re.findall(r'OpEntryPoint \w+ (%\d+)', code)
    if len(entries) != 1:
        raise ValueError('expected one stage entry point')
    if 'MaximallyReconvergesKHR' in code or 'OpFmaKHR' in code:
        raise ValueError('translate with the R32 feature profile first')
    rounding = re.compile(r'^\s*OpDecorate (%\d+) FPRoundingMode (\w+)\s*$', re.MULTILINE)
    check_conversions(code, rounding)
    if rounding.search(code):
        if 'RoundingModeRTZ 16' in code:
            raise ValueError('entry already requires RTZ')
        anchor = f'OpExecutionMode {entries[0]} SignedZeroInfNanPreserve 16'
        if code.count(anchor) != 1:
            raise ValueError('expected float16 preservation mode')
        code = rounding.sub('', code)
        if 'OpCapability RoundingModeRTE' not in code:
            code = code.replace('OpCapability Shader\n', 'OpCapability RoundingModeRTE\nOpCapability Shader\n', 1)
        if f'OpExecutionMode {entries[0]} RoundingModeRTE 16' not in code:
            code = code.replace(anchor, anchor + f'\nOpExecutionMode {entries[0]} RoundingModeRTE 16')
    subprocess.run(
        ['spirv-as', '--preserve-numeric-ids', '--target-env', 'spv1.5', '-o', dest, '-'],
        input=code,
        text=True,
        check=True,
    )
    subprocess.run(['spirv-val', '--uniform-buffer-standard-layout', '--target-env', 'vulkan1.2', dest], check=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('dest', type=Path)
    args = parser.parse_args()
    convert(args.source, args.dest)
