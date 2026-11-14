"""Every generated command's batches as the device receives them, for the app's decoder test (hw_test_app core_test).

The app refuses an experiment its decoder rejects (an `invalid` result); decoding these here makes a first run's
`invalid` a device fact, never a generator's mistake.
"""

import json
import sys

from tools.benchloop import doctor, protocol
from tools.benchloop.generators import GENERATORS

COMMANDS = ('twod', 'resolveties', 'store16', 'compressed', 'cstall', 'iterated', 'interlock_section')


def batches() -> list[dict]:
    out = [protocol.batch('calibration', doctor.calibration())]
    for command in COMMANDS:
        experiments = GENERATORS[command]()
        for start in range(0, len(experiments), protocol.MAX_EXPERIMENTS):
            part = experiments[start : start + protocol.MAX_EXPERIMENTS]
            out.append(protocol.batch(f'{command}-{start // protocol.MAX_EXPERIMENTS}', part))
    return [json.loads(protocol.encode_batch(b)) for b in out]


if __name__ == '__main__':
    json.dump(batches(), sys.stdout, separators=(',', ':'))
