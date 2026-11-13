"""A batch with one experiment of every kind, kept as testdata/golden_batch.json.

golden_test.py checks protocol.encode_batch still writes these bytes; hw_test_app/host_tests/BenchProtocolTests.swift
checks the app decodes every experiment of the same file, so the two sides cannot drift apart unnoticed.
"""

import struct

from tools.benchloop import known, protocol

SET_ITERATED_BLEND = 0x0DD0
SET_ITERATED_BLEND_PASS = 0x0DD4
LOAD_ITERATED_BLEND_INSTRUCTION_POINTER = 0x0DF0
LOAD_ITERATED_BLEND_INSTRUCTION = 0x0DF4


def program() -> dict:
    return {
        'sph': protocol.b64(known.KNOWN_SPH),
        'code': protocol.b64(known.KNOWN_CODE),
        'data': protocol.b64(known.KNOWN_DATA_BYTES),
        'gprs': known.KNOWN_GPRS,
    }


def golden_batch() -> dict:
    float_target = {'format': 'rgba32float', 'width': 16, 'height': 8}
    unorm_target = {'format': 'bgra8unorm', 'width': 4, 'height': 2}
    fill = b''.join(struct.pack('<4B', 32 * x, 64 * y, 128, 255) for y in range(2) for x in range(4))
    experiments = [
        {'id': 'noop', 'kind': 'noop', 'target': float_target, 'draws': [0], 'repeats': 3, 'readback': 'none'},
        {'id': 'known', 'kind': 'known', 'target': float_target, 'readback': 'full'},
        {
            'id': 'fragment',
            'kind': 'fragment',
            'target': float_target,
            'rect': [0, 0, 8, 4],
            'clear': [0, 0, 0, 1],
            'program': program(),
            'draws': [1, 2, 4],
            'repeats': 5,
            'warmup': 2,
        },
        {
            'id': 'compute',
            'kind': 'compute',
            'target': float_target,
            'compute': {
                'code': protocol.b64(known.KNOWN_KERNEL),
                'gprs': 1,
                'threads': [32, 1, 1],
                'groups': [4, 1, 1],
            },
            'draws': [1, 8],
            'repeats': 3,
            'readback': 'none',
        },
        {
            'id': 'readback',
            'kind': 'readback',
            'target': unorm_target,
            'fill': protocol.b64(fill),
            'program': program(),
            'methods': [
                [SET_ITERATED_BLEND_PASS, 1],
                [LOAD_ITERATED_BLEND_INSTRUCTION_POINTER, 0],
                [LOAD_ITERATED_BLEND_INSTRUCTION, 0x302A0000],
                [SET_ITERATED_BLEND, 1],
            ],
            'restore': [[SET_ITERATED_BLEND, 0], [SET_ITERATED_BLEND_PASS, 0]],
            'readback': 'full',
        },
    ]
    return protocol.batch('golden', experiments)
