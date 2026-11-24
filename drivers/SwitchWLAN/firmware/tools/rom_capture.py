import argparse
import hashlib
import json
import re
import struct
from pathlib import Path

FIRMWARE = Path(__file__).resolve().parents[1]
IMAGE_SHA256 = '32a16e7b4391ccd248d3677c17cde940f398aaaaa6163571fcbff2bbee026ade'
IMAGE_BYTES = 583641
START = 0x80
HEADER = 0x100
DATA = 0x1000
MAGIC = 0x4D525753


def header_bytes(name: str) -> bytes:
    text = (FIRMWARE / 'rom_capture.h').read_text()
    match = re.search(rf'\b{name}\[\d*\]\s*=\s*\{{([^}}]+)\}};', text)
    if not match:
        raise ValueError(f'missing byte array: {name}')
    return bytes(int(value, 16) for value in re.findall(r'0x([0-9a-fA-F]+)', match[1]))


def build_image(image: bytes, rom_bytes: int, ram_bytes: int) -> bytes:
    if len(image) != IMAGE_BYTES or hashlib.sha256(image).hexdigest() != IMAGE_SHA256:
        raise ValueError('firmware fingerprint changed')
    if not 0 < rom_bytes <= ram_bytes - DATA - 4 or rom_bytes & 3 or ram_bytes > 0xFFFFFFFF:
        raise ValueError('ROM copy does not fit the measured RAM range')
    original = header_bytes('switch_wlan_rom_start_original')
    code = header_bytes('switch_wlan_rom_start_code')
    if image[START : START + len(original)] != original or START + len(code) > HEADER:
        raise ValueError('startup patch site changed')
    output = bytearray(image)
    output[START : START + len(code)] = code
    struct.pack_into('<6I', output, HEADER, MAGIC, 0, rom_bytes, ram_bytes, 0, 0)
    return bytes(output)


def extract(text: str) -> tuple[bytes, dict]:
    begin = re.search(r'ROM capture begin: (\d+) bytes, overwritten prefix (\d+), sha256 ([0-9a-f]{64})', text)
    if not begin:
        raise ValueError('missing capture header')
    length, overwritten = map(int, begin.group(1, 2))
    digest = begin[3]
    if not 0 < length <= 0x1000000 or not 0 <= overwritten <= length:
        raise ValueError('invalid capture length')
    end = re.search(rf'ROM capture end: {digest}\b', text[begin.end() :])
    if not end:
        raise ValueError('missing matching capture end')
    body = text[begin.end() : begin.end() + end.start()]
    data = bytearray(length)
    seen = bytearray(length)
    for match in re.finditer(r'ROM chunk: ([0-9a-f]+) ([0-9a-f]+)\b', body):
        offset = int(match[1], 16)
        chunk = bytes.fromhex(match[2])
        if not chunk or len(chunk) > 64 or offset > length - len(chunk):
            raise ValueError('capture chunk outside declared range')
        for index, value in enumerate(chunk, offset):
            if seen[index] and data[index] != value:
                raise ValueError('conflicting capture chunks')
            data[index] = value
            seen[index] = 1
    if not all(seen):
        raise ValueError(f'incomplete capture: {sum(seen)} of {length} bytes')
    if hashlib.sha256(data).hexdigest() != digest:
        raise ValueError('capture SHA256 mismatch')
    return bytes(data), {'bytes': length, 'overwritten_prefix_bytes': overwritten, 'sha256': digest}


def main() -> None:
    parser = argparse.ArgumentParser()
    commands = parser.add_subparsers(dest='command', required=True)
    image = commands.add_parser('image', help='build a diagnostic image from measured ROM/RAM sizes')
    image.add_argument('firmware', type=Path)
    image.add_argument('output', type=Path)
    image.add_argument('--rom-bytes', type=lambda value: int(value, 0), required=True)
    image.add_argument('--ram-bytes', type=lambda value: int(value, 0), required=True)
    capture = commands.add_parser('extract', help='verify and reconstruct a complete development capture')
    capture.add_argument('log', type=Path)
    capture.add_argument('output', type=Path)
    args = parser.parse_args()
    try:
        if args.command == 'image':
            data = build_image(args.firmware.read_bytes(), args.rom_bytes, args.ram_bytes)
            metadata = {'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()}
        else:
            data, metadata = extract(args.log.read_text())
        with args.output.open('xb') as output:
            output.write(data)
        print(json.dumps(metadata, sort_keys=True))
    except (OSError, ValueError) as error:
        parser.exit(1, f'{error}\n')


if __name__ == '__main__':
    main()
