"""Kext imports of one kernelcache that our kernel answers with a function that is not the one Apple's kernel has.

names.py binds each after every other source, where nothing named the address; emit26.py refuses any import that
neither a name nor this table answers. The functions are ours (kernel/switch/prelink/shims.c, or a Darwin 26 KPI
stand-in of kernel/xnu-12377), each entry with why it is the right answer here.
"""

import hashlib
from pathlib import Path

KERNELCACHE_SHA256 = '250337d45598da0e5cc578413704981368afe92f84b3450dc956e21da1b21bd4'

_ENOSYS = '_switch_prelink_enosys'
_UNREACHABLE = '_switch_prelink_unreachable'
_PMGR = "ApplePMGR's SoC power manager, which starts only on an Apple pmgr node; the console's ADT has none"
_M2 = (
    "AppleM2ScalerCSCDriver's scaler, which starts only on an Apple scaler node; IOMobileGraphicsFamily's calls into "
    'it reach no such import'
)

SHIMS = {
    # apfs: p_vfs_iopolicy bit 0x8000, which xnu-12377 neither defines nor sets, so it is never in effect.
    0xFFFFFFF00A3225C0: ('_vfs_context_iopolicy_unassigned_0x8000', 'an iopolicy bit no process of this kernel has'),
    # Image4's start hands each image4 object to the code-signing monitor (selector 0x34) and ignores the result;
    # 0x35 is the same kind of monitor call. This kernel runs without a monitor (xnu.c's CSM), so nothing takes them.
    0xFFFFFFF00A66D468: (_ENOSYS, 'a code-signing monitor call (0x34) with no monitor to take it'),
    0xFFFFFFF00A66D4E0: (_ENOSYS, 'a code-signing monitor call (0x35) with no monitor to take it'),
    0xFFFFFFF00A15D33C: (_UNREACHABLE, _PMGR),
    0xFFFFFFF00A15D634: (_UNREACHABLE, _PMGR),
    0xFFFFFFF00A2A0D6C: (_UNREACHABLE, _PMGR),
    0xFFFFFFF00A7FDB5C: (_UNREACHABLE, _M2),
    # Also ApplePMGR's and AppleSMC's: none of the three starts here.
    0xFFFFFFF00A7FEAF8: (_UNREACHABLE, _M2),
    # AppleARMPlatform's AppleARMFunction, which evaluates ADT function-* properties for Apple's drivers; the only
    # ones here (/buttons) are for MobileGestalt, and SwitchButtons, not AppleM68Buttons, owns the buttons.
    0xFFFFFFF00A7FF9AC: (_UNREACHABLE, 'AppleARMFunction, which no driver here asks to evaluate a function-* property'),
}


def shims_for(kernelcache: str) -> dict[int, tuple[str, str]]:
    with Path(kernelcache).open('rb') as data:
        digest = hashlib.file_digest(data, 'sha256').hexdigest()
    return SHIMS if digest == KERNELCACHE_SHA256 else {}
