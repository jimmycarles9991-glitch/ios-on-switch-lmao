"""abi_check.py <Apple's kernel cache> <our prelinked kernel> <gaps file>: the kernel ABI iOS user space reaches.

Fails unless our kernel answers every system call, Mach trap, MIG routine and sysctl that Apple's kernel cache of the
same IPSW answers, with the same arguments, except what the gaps file names. The sysctls compared are XNU's and those
of the Apple kexts we prelink, for a fileset kernel cache (iOS 26 and later: an entry names each oid's owner). A gap
that no longer occurs fails too, so the file stays the list of what is missing.

Gaps file: one fnmatch pattern per line over the keys below, '#' comments.
    sysent <n> [args]      a BSD system call (args: its argument count or bytes differ)
    trap <n> [args]        a Mach trap
    mig <msgh_id> [args|reply]   a kernel MIG routine (reply: ours replies with fewer bytes)
    sysctl <name> [type]   a sysctl (type: its CTLTYPE differs)
"""

import fnmatch
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from kernel_image import KernelImage
from kernel_tables import mach_traps, mig_routines, sysctls, system_calls

# Fewer than this means the scan missed the sysctl tree, not that the kernel lost it (xnu-10063 and 12377 have ~2500).
MIN_SYSCTLS = 1500

Gap = tuple[str, str]


def _call_gaps(apple: KernelImage, ours: KernelImage) -> list[Gap]:
    found = []
    for table in (system_calls, mach_traps):
        theirs, mine = table(apple), table(ours)
        for key, shape in theirs.items():
            if key not in mine:
                found.append((key, shape))
            elif mine[key] != shape:
                found.append((f'{key} args', f'{shape}, ours {mine[key]}'))
    return found


def _mig_gaps(apple: KernelImage, ours: KernelImage) -> list[Gap]:
    found = []
    mine = mig_routines(ours)
    for key, (argc, descr, reply_descr, max_reply) in mig_routines(apple).items():
        shape = f'argc {argc} descriptors {descr}/{reply_descr} reply {max_reply}'
        if key not in mine:
            found.append((key, shape))
        elif mine[key][:3] != (argc, descr, reply_descr):
            found.append((f'{key} args', f'{shape}, ours {mine[key]}'))
        elif mine[key][3] < max_reply:
            found.append((f'{key} reply', f'{shape}, ours {mine[key][3]}'))
    return found


def _sysctl_gaps(apple: KernelImage, ours: KernelImage) -> list[Gap]:
    if not apple.fileset:
        return []
    theirs = sysctls(apple, {'com.apple.kernel'} | ours.prelinked_bundles())
    mine = sysctls(ours, {'kernel'})
    if len(theirs) < MIN_SYSCTLS or len(mine) < MIN_SYSCTLS:
        raise LookupError(f"only {len(theirs)} of Apple's and {len(mine)} of our sysctls found")
    found = []
    for key, (owner, kind) in theirs.items():
        if key not in mine:
            found.append((key, owner))
        elif mine[key][1] != kind:
            found.append((f'{key} type', f'CTLTYPE {kind}, ours {mine[key][1]}'))
    return found


def main(apple_path: str, ours_path: str, gaps_path: str) -> int:
    """Print every gap the gaps file does not name and every pattern that names none; 1 if there is either."""
    apple, ours = KernelImage(apple_path), KernelImage(ours_path)
    with Path(gaps_path).open() as f:
        patterns = [line.strip() for line in f if line.strip() and not line.startswith('#')]
    found = sorted(_call_gaps(apple, ours) + _mig_gaps(apple, ours) + _sysctl_gaps(apple, ours))
    unknown = [(key, what) for key, what in found if not any(fnmatch.fnmatchcase(key, p) for p in patterns)]
    stale = [p for p in patterns if not any(fnmatch.fnmatchcase(key, p) for key, _ in found)]
    for key, what in unknown:
        print(f'{key}: Apple answers it ({what}), we do not')
    for pattern in stale:
        print(f'{gaps_path}: "{pattern}" matches nothing any more: remove it')
    print(f'{len(found)} gaps, {len(found) - len(unknown)} of them known')
    return 1 if unknown or stale else 0


if __name__ == '__main__':
    sys.exit(main(*sys.argv[1:]))
