"""Compare the fragment executables for a single directcheck capture."""

import argparse
import json
import re
import signal
import sys
from collections import Counter
from hashlib import sha256
from pathlib import Path
from types import FrameType

sys.path.insert(0, str(Path(__file__).resolve().parents[3]))

from tools.gpu.l4t.paths import coherent_paths
from tools.gpu.maxwell import gate, isa, nakstat


def time_limit(_signum: int, _frame: FrameType | None) -> None:
    raise TimeoutError('quad execution exceeded two seconds')


def metrics(image: bytes, registers: int, bound: int) -> dict:
    code = image[isa.HEADER_BYTES :]
    words = isa.words_of(code)
    forms = Counter(isa.name(word) for word in isa.instructions(code))
    controls = [(words[i - i % 4] >> ((i % 4 - 1) * 21)) & 0x1FFFFF for i in range(len(words)) if i % 4]
    issued = []
    error = None
    for seed in range(20):
        lanes = [gate.Seeded(1, seed).inputs(lane, bound) for lane in range(4)]
        try:
            signal.setitimer(signal.ITIMER_REAL, 2)
            gate.run_pixel(image, lanes, issued=issued)
        except (gate.UnsupportedError, TimeoutError) as exc:
            error = str(exc)
            break
        finally:
            signal.setitimer(signal.ITIMER_REAL, 0)
    return {
        'sha256': sha256(code).hexdigest(),
        'slots': sum(forms.values()),
        'registers': registers,
        'warps': nakstat.warps_per_sm(registers),
        'forms': forms,
        'issued': issued,
        'issue_failure': error,
        'path_model': coherent_paths(code),
        'stall': sum(value & 15 for value in controls),
        'yield_bit_set': sum(bool(value & 16) for value in controls),
        'wait_mask_set': sum(bool(value & (63 << 11)) for value in controls),
        'reuse_set': sum(bool(value & (15 << 17)) for value in controls),
    }


def nak(path: Path) -> dict:
    record = json.loads(path.read_bytes())
    return metrics(bytes.fromhex(record['fs']), record['fs_gprs'], record['fs_bound_textures'] - 1)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('current', type=Path)
    parser.add_argument('paired', type=Path)
    parser.add_argument('translation', type=Path)
    parser.add_argument('nvidia_output', type=Path)
    args = parser.parse_args()
    if args.current.name != args.paired.name:
        parser.error('NAK record identities differ')
    for i in (0, 1):
        path = f'stage{i}.r32.spv'
        if (
            sha256((args.translation / path).read_bytes()).hexdigest()
            != (args.nvidia_output / f'stage{i}.sha256').read_text().strip()
        ):
            parser.error(f'NVIDIA input differs from the paired SPIR-V for {path}')
    out = args.nvidia_output
    stats = (out / 'statistics.txt').read_text()
    fragment = re.search(r'^stage=16 .*?(?=^stage=|\Z)', stats, re.DOTALL | re.MULTILINE).group()
    registers = int(re.search(r'Register Count = (\d+)', fragment)[1])
    image = (out / 'frag.sph').read_bytes() + (out / 'frag.sass').read_bytes()
    if int(re.search(r'Binary Size = (\d+)', fragment)[1]) != len(image):
        parser.error('driver binary size differs from extracted fragment')
    signal.signal(signal.SIGALRM, time_limit)
    sys.setrecursionlimit(16000)
    print(
        json.dumps(
            {'current': nak(args.current), 'paired': nak(args.paired), 'nvidia': metrics(image, registers, -1)},
            indent=2,
        )
    )


if __name__ == '__main__':
    main()
