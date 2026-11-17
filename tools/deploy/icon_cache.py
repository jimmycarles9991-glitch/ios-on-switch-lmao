"""The GPU build a deploy staged, named for the kernel, which lets IconServices' rendered-icon cache go when it changes.

iconservicesagent keeps the icons it renders in the system group container systemgroup.com.apple.lsd.iconscache and
empties them at launch unless their __cache_info__ names the system's ProductBuildVersion (-[ISMutableIconCache
initWithURL:], 21H461 and 24B119). On Apple's devices an update that changes how icons draw also changes that build;
here the GPU build changes alone, and the icons it drew before stay. So the deploy writes the staged GPU build (the
stager's whole-list digest, the first line of ngpu/stage.tsv.gz) to <Data>/Switch/icon-cache-gpu, and the next boot's
kernel removes each __cache_info__ (21H461's at the cache folder's top, 24B119's in its salt folders) before launchd
starts, once per GPU build (kernel/switch/boot/icon_cache.c). The deploy does not read the cache
itself: on a Data volume mounted with owners ignored the Mac's user can still be refused it (EACCES at __cache_info__,
14 Nov), and only root on the device always gets in.
"""

import gzip
from collections.abc import Callable
from pathlib import Path

from tools.install import renamed

STAGE_LIST = 'ngpu/stage.tsv.gz'  # drivers/TegraGM20BMetal/stage-tegra-gm20b-metal.py STATE
RECORD = 'icon-cache-gpu'


def staged_gpu(data_root: Path) -> str:
    """Return the GPU build the card holds: the stager's whole-list digest, '' without a staged build."""
    try:
        with gzip.open(Path(data_root) / STAGE_LIST, 'rt') as handle:
            first = handle.readline().strip()
    except (OSError, EOFError):
        return ''
    return first[2:] if first.startswith('# ') else ''


def present(data_root: Path, log: Callable[..., None]) -> str:
    """Name the staged GPU build in <Data>/Switch/icon-cache-gpu; return 'named', or 'unchanged' when it already is."""
    record = renamed.data_folder(data_root) / RECORD
    gpu = staged_gpu(data_root)
    if record.is_file() and record.read_text() == gpu + '\n':
        return 'unchanged'
    record.parent.mkdir(parents=True, exist_ok=True)
    record.write_text(gpu + '\n')
    log(f'icon cache: GPU build {gpu[:12] or "none"} named; the next boot lets the icons go unless it did for it')
    return 'named'
