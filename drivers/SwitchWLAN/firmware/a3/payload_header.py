import struct
import sys
from pathlib import Path


def switch_a3_symbols(elf: bytes) -> dict[str, int]:
    if elf[:6] != b'\x7fELF\x01\x01':
        raise ValueError('expected a little-endian ELF32 payload')
    offset = struct.unpack_from('<I', elf, 32)[0]
    stride, count = struct.unpack_from('<HH', elf, 46)
    if stride != 40:
        raise ValueError('unexpected ELF section layout')
    sections = [struct.unpack_from('<10I', elf, offset + index * stride) for index in range(count)]
    symbols = {}
    for section in sections:
        if section[1] != 2:
            continue
        strings = sections[section[6]]
        names = elf[strings[4] : strings[4] + strings[5]]
        if section[9] != 16:
            raise ValueError('unexpected ELF symbol layout')
        for index in range(section[4], section[4] + section[5], 16):
            name, value, _, _, _, defined = struct.unpack_from('<IIIBBH', elf, index)
            if defined and name < len(names):
                end = names.find(b'\0', name)
                if end < 0:
                    raise ValueError('unterminated ELF symbol')
                symbols[names[name:end].decode()] = value
    return symbols


def switch_a3_header(elf: bytes, binary: bytes, *, candidate: bool = False) -> str:
    symbols = switch_a3_symbols(elf)
    begin = symbols['switch_a3_island_start']
    end = symbols['switch_a3_island_end']
    maximum = 16384 if candidate else 8192
    if begin != 0x20E7DC or not 0 < end - begin <= maximum or end & 3:
        raise ValueError('payload exceeds the pinned persistent island')
    if not 0 < len(binary) <= end - begin:
        raise ValueError('payload binary exceeds its reserved memory')
    names = ('heap_init', 'attach', 'rx_entry', 'state')
    if candidate:
        names += ('tx_entry', 'tx_radio_status', 'pm_status_entry', 'channel_gate',
                  'pm_prepare_entry', 'pm_submit_entry', 'pm_set_entry')
    entries = {name: symbols['switch_a3_' + name] & ~1 for name in names}
    if any(not begin <= value < end for value in entries.values()) or entries['state'] + 64 > end:
        raise ValueError('payload entry or state lies outside reserved memory')
    data = binary.ljust(end - begin, b'\0')
    guard = 'SWITCH_WLAN_A3_CANDIDATE_PAYLOAD_H' if candidate else 'SWITCH_WLAN_A3_PAYLOAD_H'
    prefix = 'switch_wlan_a3_candidate' if candidate else 'switch_wlan_a3'
    lines = [f'#ifndef {guard}', f'#define {guard}', '#include <stdint.h>']
    for name, value in {'begin': begin, 'end': end, **entries}.items():
        lines.append(f'static const uint32_t {prefix}_{name} = 0x{value:x};')
    lines.append(f'static const uint8_t {prefix}_payload[] = {{')
    lines.extend(
        '    ' + ', '.join(f'0x{value:02x}' for value in data[offset : offset + 16]) + ','
        for offset in range(0, len(data), 16)
    )
    return '\n'.join([*lines, '};', '#endif', ''])


if __name__ == '__main__':
    elf_path, binary_path, header_path = map(Path, sys.argv[1:4])
    header_path.write_text(
        switch_a3_header(elf_path.read_bytes(), binary_path.read_bytes(), candidate=sys.argv[4:] == ['--candidate'])
    )
