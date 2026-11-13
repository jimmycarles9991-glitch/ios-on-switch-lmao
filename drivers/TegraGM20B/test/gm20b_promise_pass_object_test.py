"""The end of every GPU interrupt pass as the kernel build compiles it (gpu_promise_giveup.c tgm_promise_pass).

While no writer promise give-up is armed, which is nearly always, the pass costs one acquire load of armed_ns and one
branch: no stack frame, no call, no store. The test follows that path through the built object's disassembly.
"""

import re
import subprocess
import unittest

from python.runfiles import runfiles

OBJECT = '_main/drivers/TegraGM20B/build/TegraGM20B/gpu_promise_giveup.c.o'
SYMBOL = '_tgm_promise_pass'
INSTRUCTION = re.compile(r'^\s*([0-9a-f]+):\s+(\S+)\s*(.*)$')
MOST = 5


def disassemble() -> list[tuple[int, str, str]]:
    """Return the symbol's instructions as (address, mnemonic, operands)."""
    files = runfiles.Create()
    objdump = files.Rlocation('llvm/bin/llvm-objdump')
    out = subprocess.run(
        [objdump, '-d', '--no-show-raw-insn', f'--disassemble-symbols={SYMBOL}', files.Rlocation(OBJECT)],
        check=True,
        capture_output=True,
        text=True,
    ).stdout
    code = []
    for line in out.splitlines():
        match = INSTRUCTION.match(line)
        if match:
            code.append((int(match.group(1), 16), match.group(2), match.group(3).split(';')[0].strip()))
    return code


def target(operands: str) -> int:
    """Return a branch's target address (llvm-objdump prints it as 0x... <symbol+offset>)."""
    return int(re.search(r'0x([0-9a-f]+)', operands).group(1), 16)


def unarmed(code: list[tuple[int, str, str]]) -> list[str]:
    """Return the instructions run while armed_ns is 0, from the entry to ret or a branch out of the symbol."""
    at = {address: i for i, (address, _, _) in enumerate(code)}
    path = []
    loaded = None
    i = 0
    while i in range(len(code)) and len(path) <= 2 * MOST:
        _, op, operands = code[i]
        path.append(f'{op} {operands}'.strip())
        if op == 'ret':
            break
        if op == 'ldar':
            loaded = operands.split(',')[0]
        if op in {'cbz', 'cbnz'}:
            taken = (op == 'cbz') == (operands.split(',')[0] == loaded)
            if taken:
                if target(operands) not in at:
                    break
                i = at[target(operands)]
                continue
        elif op == 'b':
            if target(operands) not in at:
                break
            i = at[target(operands)]
            continue
        i += 1
    return path


class UnarmedPass(unittest.TestCase):
    """tgm_promise_pass with no give-up armed."""

    def test_is_one_acquire_load_and_a_branch(self) -> None:
        """The path: the address of armed_ns, its ldar, the cbz or cbnz on it, and ret."""
        path = unarmed(disassemble())
        print(f'{SYMBOL} with nothing armed, {len(path)} instructions: ' + '; '.join(path))
        ops = [line.split()[0] for line in path]
        self.assertEqual(ops[-1:], ['ret'], path)
        self.assertEqual([op for op in ops if op.startswith('ld')], ['ldar'], path)
        self.assertFalse([op for op in ops if op.startswith('st')], path)
        self.assertFalse([line for line in path if re.search(r'\bsp\b', line)], path)
        self.assertFalse([op for op in ops if op in {'bl', 'blr', 'b', 'br'}], path)
        branches = [op for op in ops if op in {'cbz', 'cbnz'}]
        self.assertEqual(len(branches), 1, path)
        self.assertLess(ops.index('ldar'), ops.index(branches[0]), path)
        self.assertLessEqual(len(path), MOST, path)


if __name__ == '__main__':
    unittest.main()
