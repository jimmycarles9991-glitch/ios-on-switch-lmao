# translate/arm64e/xnu: the kernel side, for xnu-12377.121.6

Not built. This is what an iPadOS 26 kernel needs from the translator. `kernel/xnu-12377.121.6/` holds pristine
xnu-12377.121.6 (selected by `--//kernel:xnu_version=12377.121.6`, macro `XNU_12377_121_6`); these files land there
as commits after the BCM2837 restore and the ios-on-switch-lmao series, with `a64x` added to `config/MASTER.arm64.bcm2837`
next to `config_code_decryption`.

## Contents

| Path | What |
|---|---|
| `bsd/kern/a64x_xnu.c`, `bsd/sys/a64x_xnu.h` | The loader glue: load-time translation of arm64e/arm64 images through `apple_protect` pagers, the dyld-cache pager with the plan, the EL0 undefined-instruction fallback, `kern.a64x.*` sysctls |
| `a64x-hooks-xnu-12377.121.6.diff` | The hooks in XNU itself, against pristine xnu-12377.121.6 plus UniXNU's series (below): `bsd/conf/files`, `config/MASTER` (`CONFIG_A64X`), `mach_loader.c`, `kern_mman.c` (mmap of an arm64e `__TEXT`, MAP_JIT refused for arm64e processes), `cpuid.c` (reports ARMv8E), `copyio.c` and `machine_routines_asm.s` (user atomics for the fallback), `sleh.c`, `vm_dyld_pager.c` (identity `signPointer`), `vm_shared_region.c` (auth rebases written plain) |
| `unixnu-arm64e-series.txt` | UniXNU's `profiles/arch/arm64e/patches/xnu/series`, the order its `patches/xnu-12377/*` apply in |
| `notes/` | Building UniXNU's series as plain arm64 (`unixnu-12377-arm64-fixes.md`), the toolchain at an iOS 26+ deployment target (`toolchain-notes-12377.md`), what iPadOS 26 still needs (`ios26-gap-list.md`), the plan of the 10063 to 12377 port (`switch-12377-port-plan.md`) |
| `port-12377/` | ios-on-switch-lmao's own XNU changes, merged onto 12377 + UniXNU + a64x: `switch-on-12377.diff` (58 files), the per-hunk record `hunks.md` and the reason for every conflict resolved `conflict-decisions.json` |
| `prelink26/` | The iPadOS 26.5.2 kext prelinker: names Apple's stripped kernel addresses (vtable PAC discriminators, GOT order, symbolicator signatures), translates kext text in kernel mode through `arm64e kext`, writes every chained fixup plain, re-aims kext-to-kernel branches; `bindings/` holds the 26.5.2 bindings it found. Paths are those of the scratch layout it ran in |

## The core is the package's

`bsd/conf/files` (in the hooks diff) compiles `bsd/kern/a64x/a64x_{classify,rewrite,image}.c`. Those files and their
headers are `translate/arm64e/a64x/` copied as they are: one source of truth, tested on the host by
`//translate/arm64e/test`. `a64x_xnu.c` includes `"a64x/a64x.h"`.

## Making the tree

How it was built on 2026-11-04, outside Bazel:

1. Pristine xnu-12377.121.6 (now `kernel/xnu-12377.121.6/`).
2. UniXNU's `patches/xnu-12377/*` in the order of `unixnu-arm64e-series.txt`, skipping `native-pac-keys.patch`,
   `arm64e-threaded-starts.patch` and `native-arm-pan.patch` (PAC hardware only; the A57 has no PAN). The fixes for a
   plain arm64 build are in `notes/unixnu-12377-arm64-fixes.md` (TrustCache headers from xnu-8796.101.5, one
   `__builtin_ptrauth_strip` in `debug.c`).
3. `patch -p1 < a64x-hooks-xnu-12377.121.6.diff`; copy `bsd/` here and `translate/arm64e/a64x/*` to `bsd/kern/a64x/`;
   add `a64x` to `config/MASTER.arm64.bcm2837` next to `config_code_decryption`.
4. `patch -p1 < port-12377/switch-on-12377.diff` for ios-on-switch-lmao's board and driver changes.

Built this way on 2026-11-04 (`MACHINE_CONFIGS=BCM2837`, DEVELOPMENT), the kernel linked: Mach-O arm64 (subtype ALL),
0 undefined symbols, Darwin 25.5.0. It has never booted.

## The A57 pin

At an iOS 26+ deployment target Apple clang's `-arch arm64` is an A12 (LSE, LDAPR, FP16, JSCVT, FCMA, RDM) and
`-march=armv8-a` does not undo it. The 12377 kernel needs `-mcpu=cortex-a57` in `MACHINE_FLAGS_ARM64_BCM2837` (so `.s`
files get it too) and the C++ recipes must keep `-mcpu`. Without it the first link held 2,464 LSE and 403 LDAPR words
and `cswitch.s` read UAO. Any binary built for this core (the `__TEXT,__a64x_target` marker) carries the same flag.

## Not in the repository

Everything derived from Apple's binaries stays out (as for the IPSW): the 26.5.2 kernelcache, the prelinked kernel,
`kernel-names.json` and the discriminator cache. They were made from iPad11,6 (J171aAP); for iPad12,1 (J181AP, which
has AppleUSBXHCI) they are redone from its kernelcache. On 27.2 the A13 also ships SPTM and TXM, and its kexts were
built against an SPTM/TXM kernel; our kernel keeps XNU's non-SPTM pmap.

The build redoes the prelink: `bazel build --//ipsw:kind=24B119 //kernel:kernel_12377` runs `prelink26/prelink.py`,
which runs the ipsw CLI over the kernelcache, then `classify.py`, `pcrel.py`, `names.py`, `emit26.py` and
`verify26.py` in a temporary directory (`PRELINK_WORK`, see `paths.py`). To run one of those steps by hand, give it
that directory as `prelink.py` fills it, `prelink-lists.json` included: the roots, cuts and deferred personalities
that `kernel/rules/prelink_fileset.bzl` takes from the drivers' `prelink`, `prelink_cuts` and `prelink_deferred`.
