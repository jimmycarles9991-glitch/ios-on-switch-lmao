import json
import socket
import subprocess
import tempfile
import time
import unittest
from pathlib import Path

from drivers.SwitchWLAN.firmware.a3 import payload_header
from drivers.SwitchWLAN.firmware.tools import a3_cpu_harness as switch_cpu
from drivers.SwitchWLAN.firmware.tools.arm_tools import switch_tool

SWITCH_BEGIN = 0x20E670
SWITCH_END = 0x23DF6C
SWITCH_MAX_END = 0x20E7DC + 16384


def switch_heap_results(qmp: socket.socket, qtest: socket.socket, symbols: dict[str, int]) -> tuple[int, ...]:
    with qmp.makefile('rwb', buffering=0) as control, qtest.makefile('rwb', buffering=0) as memory:
        json.loads(control.readline())
        switch_cpu.switch_qmp_command(control, 'qmp_capabilities')
        switch_cpu.switch_qmp_command(control, 'cont')
        time.sleep(0.05)
        switch_cpu.switch_qmp_command(control, 'stop')
        return (
            *switch_cpu.switch_read_words(memory, symbols['switch_heap_test_results'], 4),
            *switch_cpu.switch_read_words(memory, symbols['switch_a3_state'], 16),
        )


def switch_heap_command(gcc: str, source: Path, elf: Path, case: tuple[int, ...], lto: bool) -> list[str]:
    begin, end, boundary, island_end, _ = case
    script = elf.with_suffix('.ld')
    script.write_text(
        'ENTRY(switch_heap_test_start)\nSECTIONS {\n'
        ' .text 0x10000 : { *(.text*) }\n .data : { *(.data*) *(.bss*) }\n'
        ' .heap 0x1dfb9c : { *(.heap_stub) }\n'
        f' switch_a3_island_end = {island_end:#x};\n}}\n',
    )
    return [
        gcc,
        *(['-flto'] if lto else []),
        '-mcpu=cortex-r4',
        '-mthumb',
        '-DSWITCH_A3_CANDIDATE=1',
        f'-DSWITCH_TEST_BEGIN={begin}',
        f'-DSWITCH_TEST_END={end}',
        f'-DSWITCH_TEST_BOUNDARY={boundary}',
        '-nostdlib',
        '-Wl,--build-id=none,--no-warn-rwx-segments',
        '-T',
        str(script),
        str(source / 'bootstrap.S'),
        str(source / 'candidate_heap_test.S'),
        '-o',
        str(elf),
    ]


class CandidateHeap(unittest.TestCase):
    def test_original_layout_and_exact_station_floor(self):
        qemu, gcc = switch_tool('qemu-system-arm'), switch_tool('arm-none-eabi-gcc')
        source = Path(__file__).resolve().parent
        cases = [
            (SWITCH_BEGIN, SWITCH_END, SWITCH_END, SWITCH_MAX_END, 1),
            (SWITCH_BEGIN, SWITCH_END, SWITCH_END, SWITCH_MAX_END - 4, 1),
            (SWITCH_BEGIN, SWITCH_END, SWITCH_END, SWITCH_MAX_END + 1, 0),
            (SWITCH_BEGIN, SWITCH_END, SWITCH_END, 0xFFFFFFFC, 0),
            (SWITCH_BEGIN - 4, SWITCH_END, SWITCH_END, SWITCH_MAX_END, 0),
            (SWITCH_BEGIN + 4, SWITCH_END, SWITCH_END, SWITCH_MAX_END, 0),
            (SWITCH_BEGIN, SWITCH_END - 4, SWITCH_END - 4, SWITCH_MAX_END, 0),
            (SWITCH_BEGIN, SWITCH_END + 4, SWITCH_END + 4, SWITCH_MAX_END, 0),
            (SWITCH_BEGIN, SWITCH_END, SWITCH_END - 4, SWITCH_MAX_END, 0),
            (SWITCH_BEGIN, SWITCH_END, SWITCH_END + 4, SWITCH_MAX_END, 0),
        ]
        with tempfile.TemporaryDirectory(prefix='switch-a3-heap-', dir='/private/tmp') as directory:
            for lto in (False, True):
                for case in cases:
                    with self.subTest(lto=lto, case=case):
                        path = Path(directory)
                        elf = path / 'heap.elf'
                        subprocess.run(
                            switch_heap_command(gcc, source, elf, case, lto), check=True, capture_output=True
                        )
                        symbols = payload_header.switch_a3_symbols(elf.read_bytes())
                        values = switch_cpu.switch_run_arm(qemu, elf, path, symbols, switch_heap_results)
                        _, end, boundary, island_end, accepted = case
                        self.assertEqual(values[7], 1 if accepted else 4)
                        self.assertEqual(values[16], end)
                        if accepted:
                            self.assertEqual(values[:4], (island_end, end, boundary, 0x53574133))
                            self.assertEqual(values[15], island_end)
                        else:
                            self.assertEqual(values[:4], (0, 0, 0, 0))


if __name__ == '__main__':
    unittest.main()
