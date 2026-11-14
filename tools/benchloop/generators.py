"""The commands whose experiments a generator makes with no input, by name."""

from collections.abc import Callable

from tools.benchloop import cbufsize, interlock_section, qmdchain, resolve_ties, rop, sweeps, twod, units

GENERATORS: dict[str, Callable[[], list[dict]]] = {
    'precision': sweeps.precision,
    'iterated': sweeps.iterated,
    'twod': twod.generate,
    'resolveties': resolve_ties.generate,
    'store16': rop.generate16,
    'compressed': units.compressed,
    'cstall': units.cstall,
    'qmdchain': qmdchain.generate,
    'cbufsize': cbufsize.generate,
    'interlock_section': interlock_section.generate,
}
