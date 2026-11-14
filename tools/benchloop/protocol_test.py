"""benchloop/6's fields where they mean something: compete on draws, code to 128 KiB, a ROP3 code and a grid invert."""

import unittest

from tools.benchloop import protocol, twod

POSITION_W = bytes(23) + b'\x80' + bytes(56)


def draw(kind: str = 'fragment', code_bytes: int = 32, **extra: object) -> dict:
    program = {'sph': protocol.b64(POSITION_W), 'code': protocol.b64(bytes(code_bytes)), 'gprs': 4}
    return (
        {'id': 'd', 'kind': kind, 'target': {'format': 'rgba32float', 'width': 4, 'height': 4}}
        | ({'program': program} if kind in ('fragment', 'readback') else {})
        | extra
    )


def by_scale(fmt: str, **op: object) -> dict:
    production = {'op': 'resolve', 'samples': 4, 'byScale': True, 'du': 2 << 32, 'dv': 2 << 32, 'sx': 1 << 32}
    production |= {'sy': 1 << 32, 'bilinear': True, 'corner': True}
    return twod._experiment('by-scale', fmt, (32, 32), production | op, source=twod.random_source(fmt))


class ProtocolTest(unittest.TestCase):
    def test_compete_is_a_flag_of_draws(self) -> None:
        protocol.validate_experiment(draw(compete=True))
        protocol.validate_experiment(draw('readback', compete=False))
        for bad in (draw(compete=1), draw('noop', compete=True), draw('compute', compete=True)):
            with self.assertRaises(protocol.InvalidError, msg=bad['kind']):
                protocol.validate_experiment(bad)

    def test_code_runs_to_128_kib(self) -> None:
        protocol.validate_experiment(draw(code_bytes=protocol.MAX_CODE))
        with self.assertRaises(protocol.InvalidError):
            protocol.validate_experiment(draw(code_bytes=protocol.MAX_CODE + 32))

    def test_a_rop_on_a_fill_a_scale_or_a_by_scale_resolve(self) -> None:
        fill = twod.own_fill('bgra8unorm')
        fill['twod']['rop'] = 0x55
        copy = twod.scale('1to1', 'bgra8unorm', bilinear=False, corner=False)
        copy['twod']['rop'] = 0x33
        protocol.batch('rops', [fill, copy, by_scale('bgra8unorm', rop=0x33, invertGrid=True)])
        driver = twod.resolve('bgra8unorm', 4)
        driver['twod']['rop'] = 0x33
        past = twod.own_fill('bgra8unorm')
        past['twod']['rop'] = 256
        scale_invert = twod.scale('1to1', 'bgra8unorm', bilinear=False, corner=False)
        scale_invert['twod']['invertGrid'] = True
        for bad in (driver, past, scale_invert):
            with self.assertRaises(protocol.InvalidError, msg=bad['id']):
                protocol.validate_experiment(bad)

    def test_a_grid_is_inverted_only_in_a_format_a_solid_fill_writes(self) -> None:
        with self.assertRaisesRegex(protocol.InvalidError, 'solid fill'):
            protocol.validate_experiment(by_scale('bgra8unorm_srgb', invertGrid=True))
        protocol.validate_experiment(by_scale('bgra8unorm_srgb', rop=0x33))


if __name__ == '__main__':
    unittest.main()
