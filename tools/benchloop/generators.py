"""The commands whose experiments a generator makes with no input, by name."""

from collections.abc import Callable

from tools.benchloop import cbufsize, qmdchain, rop, sweeps, twod, units

GENERATORS: dict[str, Callable[[], list[dict]]] = {
    'precision': sweeps.precision,
    'iterated': sweeps.iterated,
    'twod': twod.generate,
    'store16': rop.generate16,
    'compressed': units.compressed,
    'cstall': units.cstall,
    'qmdchain': qmdchain.generate,
    'cbufsize': cbufsize.generate,
}
