# translate/arm64e: arm64e code on the Cortex-A57

iPadOS 26 and 27 ship their userland, their dyld shared cache and their kernel extensions as arm64e only: code built
for Apple A12 (ARMv8.3). The Switch's Tegra X1+ has Cortex-A57 cores (ARMv8.0 with CRC32 and the crypto extensions),
which have no pointer authentication, no LSE atomics, no RCpc, no FP16 arithmetic, no FCMA, no JSCVT and no RDM.
This package rewrites such code into ARMv8.0 code that does the same thing. It is not wired into the iPadOS 17 boot:
nothing under `//:boot` or `//:gpu` depends on it.

```sh
bazel run //translate/arm64e:arm64e -- translate IN OUT STUBS   # one Mach-O, as the a64x loader maps it
bazel run //translate/arm64e:arm64e -- check FILE               # what the loader does with a Mach-O or a fileset
bazel run //translate/arm64e:arm64e -- fixups IN [OUT]          # authenticated chained fixups, written plain
bazel run //translate/arm64e:arm64e -- plan -o PLAN CACHE       # the dyld cache's plan for the kernel
bazel run //translate/arm64e:arm64e -- kext ...                 # kexts of a prelinked kernel, kernel mode
bazel test //translate/arm64e/...
```

## Layout and name

`translate/` is a new top-level directory: this is neither a driver, nor a kit, nor an XNU tree, but one library with
three users (the host tools, XNU's pager, the dyld-cache planner), and it leaves room beside it. The library is
called `a64x` (AArch64 translate), the name every symbol, sysctl and file already has.

| Path | What |
|---|---|
| `a64x/` | `:a64x`, the translator: classifier, one-word rewrites, out-of-line stubs, X and V register liveness, page translation, dead PAC checks, data islands. Freestanding C11 (stdint/stdbool/stddef only); the same files compile into XNU |
| `macho/` | `:macho`, host only: Mach-O and fat parsing, the loader's per-image translation done offline (byte for byte what the kernel maps), verification of every stub path, chained-fixup decoding and the auth-to-plain rewrite |
| `tools/` | `:arm64e`, the CLI (`translate`, `fixups`, `check`, `xlate`, `scan`, `kext`, `plan`, `holes`, `class`, `stub`, `gen-toptable`), and `:oracle` (the classifier and translated words against `llvm-mc`) |
| `test/` | gtest: classifier and rewrites, stubs executed next to the instructions they replace, synthetic Mach-Os, the kernel header's loader decision against the host's |
| `xnu/` | The kernel side for xnu-12377.121.6, the 10063 to 12377 port record and the 26.5.2 kext prelinker ([xnu/README.md](xnu/README.md)); not built |
| `NOTES.md` | The measurements behind this design (2026-11-04), per image and per class |

## What is translated where

| Code | When | How | Signatures |
|---|---|---|---|
| Kexts of the iPadOS 26 kernelcache | Offline, at prelink (`arm64e kext`, `xnu/prelink26`) | Kernel mode: PAC as the identity, LSE/FP16/SB stubs in a block after each kext's text, MSR/MRS of PAN/UAO/DIT/SSBS/TCO as no-ops, every chained fixup written as a plain pointer | We build the kernelcache; nothing to keep |
| The dyld shared cache | Planned offline (`arm64e plan`: 21 MB plan file on Preboot, its SHA-256 in the ADT); applied at page-in by the kernel | One-word rewrites in place; LSE/FP stubs placed into dead PAC-failure checks inside the cache's own text | Apple's: every page is validated (`vm_page_validate_cs_mapped`) before it is rewritten |
| Every other arm64e Mach-O (executables, dylibs outside the cache, dyld) | At load (`a64x_load_image`, `a64x_mmap_exec`), page by page at page-in | One-word rewrites; stubs in a region mapped right after the image | Apple's, as above |
| arm64 (ALL/V8) Mach-Os | At load, only if a scan finds something the A57 lacks | As above but PAC words left alone | Unchanged; a clean image keeps its own mapping |
| Binaries built for this core (`__TEXT,__a64x_target`) | Never | Mapped as is | Unchanged |
| JIT code of an arm64e process | Never: MAP_JIT is refused for arm64e processes | JavaScriptCore falls back to its interpreter | |
| Anything left (unplaced sites, code generated at run time) | Trapped | EL0 undefined-instruction fallback emulates LSE with exclusive loops on the user address, counted in `kern.a64x.traps`; anything else is logged and faults | |

The split follows who owns the bytes. Where we own the layout (the kernelcache we prelink), translation is static and
free at run time. Where Apple's code signature, the system volume's seal and the trust caches must stay valid (the
OS volume and the cryptexes), the files are never changed: the kernel translates validated pages at page-in, with
the hard part (where every stub goes in the packed cache text) computed offline once per IPSW.

The alternative, which Bergamota and IOS-26-Hot use, is a fully offline rewrite of every file. It costs nothing at
page-in but needs ad hoc re-signing, regenerated trust caches and a resealed volume for every IPSW, and it does not
solve the cache's lack of room for stubs either. `arm64e translate` already produces those bytes (the image plus its
stub region), so switching is a packaging decision, not a code change.

## Rewrites

- Pointer authentication is the identity: nothing is ever signed, so every authentication succeeds.
  RETAA/RETAB to RET; BRAA/BRAB/BRAAZ/BRABZ to BR; BLRAA.. to BLR; PAC*/AUT* register forms and XPACI/XPACD to NOP;
  PACGA to `MOV Xd, XZR`; LDRAA/LDRAB to LDR, LDUR or pre-index LDR, else a 3-word stub. The hint-space forms
  (PACIBSP, AUTIBSP, XPACLRI) are NOPs on ARMv8.0 already and stay.
- LDAPR to LDAR (RCpc acquire to RCsc acquire: stronger), LDLAR/STLLR to LDAR/STLR, DC CVAP to DC CVAC.
- LSE (CAS, CASP, LD<op>, ST<op>, SWP): the site becomes `B stub` (or `BL shared_stub` where x30 is dead); the stub is
  what a compiler emits for ARMv8.0, an exclusive loop with the same acquire/release bits, ending in a branch back.
  Scratch registers come from a path-sensitive liveness pass (over the whole function when LC_FUNCTION_STARTS gives its
  bounds); otherwise they are spilled below Darwin's 128-byte red zone.
- FJCVTZS, FCMLA/FCADD, FP16 arithmetic, SQRDMLAH/SQRDMLSH: stubs. FP16 widens to single, operates and narrows once
  (exact: 24 >= 2 x 11 + 2); fused forms go through double with round-to-odd narrowing. V-register liveness picks dead
  temporaries before spilling.
- A13+ classes (SVE/SME, AMX, dot product, SHA3, i8mm, BF16, ...) are left alone: A12-baseline code only runs them
  behind feature checks, which our kernel answers with 0.
- Data in `__text` is excluded before anything is rewritten: LC_DATA_IN_CODE, and an island pass (runs between
  function starts and terminators with at least one unallocated word per 32 are data). For the cache, the planner
  instead decides code by recursive descent from function starts, LSDA landing pads and jump tables.

## Correctness

- **Validated before rewritten.** The pager gets the source page from the file, `vm_page_validate_cs_mapped` checks it
  against Apple's hashes, then the transform runs; a page that does not validate is not marked validated. At page-in
  each text page regenerates its own stubs and compares them with the stub region laid out at load time; a mismatch
  keeps the original words (trap path) and counts `kern.a64x.stub_mismatch`.
- **Deterministic.** The same page and stub cursor give the same output: the offline `arm64e translate` and the
  kernel's pager produce the same bytes (`macho_test` regenerates every page through the pager's window form and
  compares).
- **Every stub path returns.** `a64x_verify_image` (and the planner's simulation for the cache) follows every path of
  every stub, both ways of each conditional branch: each stays in stub words until it branches to site + 4. Over the
  whole 26.5.2 cache plan: 897,714 sites, 0 broken.
- **Semantics against the hardware.** Apple silicon has every instruction being replaced, so the tests run the
  original and its stub from the same random state and compare X0-X30, SP, NZCV, Q0-Q31, the target memory and the
  red zone: systematic and random LSE encodings in spill and dead-register modes, 4 threads hammering one counter
  through a translated LDADDAL, every FCMA/JSCVT/RDM/FP16 encoding form found by sampling; a flipped stub bit must be
  caught. In 2026-11-04's runs over the cache's own words: 74,256 LSE checks, 20,360 FP16 words, 1,012 FCMA words,
  0 mismatches (FP16 differs from A12 only in which NaN payload comes out for signaling-NaN inputs).
- **Classifier against LLVM.** `bazel run //translate/arm64e:oracle -- features` samples 3.8 M encodings and asks
  `llvm-mc` which need each feature (valid for apple-m4 only with it on): a64x misses none for fp16, rdm, jsconv,
  complxnum, lse or pauth (2026-11-05). Over the 26.5.2 cache, no PAC, LSE, LDAPR, FP16, FCMA or JSCVT word is left in
  the 43.2 M distinct translated words.
- **The kernel and the host decide alike.** `kernel_kind_test` compiles `xnu/bsd/sys/a64x_xnu.h` unchanged and checks
  `a64x_image_kind` against the host's `a64x_kind_of` for every type and subtype.
- **Contracts relied on**: x16/x17/NZCV are clobbered by compiler PAC check sequences and x16/x17/x30 are dead at calls
  and returns (liveness and the dead-check rewrite depend on it); PACGA's result is only ever compared with another
  PACGA of the same inputs (see Open issues).
- **A dead check is a hole only while nothing enters it.** Compilers share one tail-call LR check between the signed
  path and an unsigned early return that branches straight to its EOR (24B119's libsystem_trace `_logDestructor`:
  `cbz x0` there, so fsck's exit ran a VideoToolbox FP16 stub piece). The planners keep stub words off every word a
  branch, ADR or ADRP+ADD, jump table, landing pad, function start or rebase refers to, and both verifiers count the
  stub words where the original code branches.

Words that never occur in a cache are invisible to tests over cache words, so the tests also sample the whole
encoding space. That is what checks, for instance, FMINV/FMINP on half precision (the min forms of the max encodings,
bit 23), BFMLALT/BFMMLA/BFDOT (BF16, not FCMA) and FCMLA by element with L=1 (unallocated for 4S); the fast-reject
table follows the decoder (`arm64e gen-toptable`, checked by `classify_test`).

## Cost

Measured on an M-series Mac unless marked; the A57 figures are estimates until the device runs it.

| What | Cost |
|---|---|
| PAC words, LDAPR, DC CVAP (98% of rewritten words) | None: one word for one word |
| An LSE atomic, `BL shared_stub` form (89% of cache sites) | What an arm64 build's inline LDAXR/STLXR loop costs plus about two predicted branches (3.88 vs 3.97 ns inline, native LDADDAL 1.82) |
| An LSE stub that spills (2.3% of cache sites) | About 4 extra memory operations |
| A stub split over holes (about 10%, mostly FP16) | 2.4 taken branches per site on average, plus a cold I-cache line now and then |
| FP16 vector FMLA | 26-29 words instead of 1 (the costliest; BNNS, VideoToolbox, CoreGraphics' half-float compositing) |
| Page translation | About 5 us per 4 KB page here, about 20 us estimated on the A57, once per page-in (a 4 KB SD read is 100-300 us) |
| Trap path | About 500-1500 cycles per instruction (estimate); 0 planned cache sites take it |

Nothing faster fits in place: the site is one word and an LL/SC sequence is 4-6. A per-region stub island cannot reach
(the cache text is three packed regions, 0.3-1.7 GB each, B reaches 128 MB), so stubs live in the 4.46 M dead PAC
checks (`brk #0xc47x` failure paths, 14.8 M free words) inside the text.

## State of the art (surveyed 2026-11-05)

| Project | Hardware | What it does with arm64e | Licence |
|---|---|---|---|
| IOS-26-Hot (github.com/nithinyadavamaraboina-online/IOS-26-Hot) | Snapdragon 855 (Kryo 485, ARMv8.2: LSE, no PAC) | iOS 26.5.2 (23F84) userland on xnu-12377.121.6. Offline in-place PAC strip of every Mach-O and the cache (`tools/de-pac-arm64e.py` + `de_pac_fast.c`: the common forms of the 1:1 table here; LDRA with writeback and PACGA are left to the kernel), code signing off (`cs_integrity_skip=1`, an AMFI stand-in); kernel: identity `signPointer` in `vm_dyld_pager.c`, arm64e grading, a `sleh.c` emulator and a ~45-cycle `locore.s` fast path for what the strip left (PACGA, LDRA writeback, JSCVT, FCMA, LRCPC2, FlagM, SHA3) | MIT for its own code (it builds on dipa-studio/ios-sweet, MIT); its XNU patches APSL 2.0 |
| Bergamota / Renegade (github.com/ry-diffusion/Bergamota) | Poco F1, Snapdragon 845 (ARMv8.2) | iOS 18.7.8 and, on its `ios26` branch, iOS 26.6.2 to the lock screen: Renegade rewrites every Mach-O and the whole cache offline, 4 bytes for 4 bytes, re-signs ad hoc, fixes the cache page hashes, re-HMACs corecrypto; the kernel emulates FJCVTZS | No LICENSE file: nothing may be copied (its own NOTICE says so) |
| superboyyy/hakios-arm64e-dada and other HakiOS iOS 26 ports (OnePlus 13, SM8750) | Snapdragon 8 Elite (Oryon: has PAC) | Run arm64e natively: no translation. `base/docs/ARM64E.md` is a good account of what xnu-12377 expects of arm64e | Per directory: APSL 2.0 (kernel, HakiOS base), MIT (tools) |
| UniXNU, UniXNU-sheng | PAC hardware only for arm64e | `docs/porting/arm64e.md`; the xnu-12377 patch series used here | UniXNU: per file |
| SongXiaoXi/Reductant (2022) | arm64 iPhones | Translates arm64e binaries at run time on a jailbroken device | None stated |

No other project translates for an ARMv8.0 core: all the Snapdragon ones have LSE and RCpc in hardware, so they only
need the PAC table and a few emulated instructions. Nothing of theirs is adopted: the PAC mapping is dictated by the
ISA and was already the same, and their offline-rewrite approach is the trade-off discussed above. Two things are
worth taking from IOS-26-Hot as checks, not code: its identity `signPointer` and arm64e `match_subtype` agree with the
hooks here, and it emulates PACGA as a non-zero hash, naming libobjc as a user (below).

## Open issues

- **PACGA is 0.** By the project's decision (2026-11-04) PACGA writes 0. That is right wherever a PACGA result is
  compared with a PACGA of the same inputs (jmp_buf and other integrity cookies), but any table keyed by a PACGA hash
  collapses into one bucket. The 26.5.2 cache has 687 sites (Foundation 332, CoreGraphics 212, CoreFoundation 99,
  libobjc 7). Check at bring-up whether any is a hash; if so, a stub with a real mix into bits 63:32 costs about as much
  as an LSE stub.
- **The classifier over-approximates in some groups.** Of random encodings it flags, these are unallocated: FP16
  group 44,166 of 97,057, RDM 11,790 of 23,448, LSE 1,144 of 32,490, PAuth 42 of 25,170. Compiled code never holds an
  unallocated word, so this only widens what the island pass must keep away from data in `__text`. Tightening the
  FP16 two-register-misc and three-same groups and RDM's is the next classifier job.
- **The stub region of a dylib outside the cache** goes right after the image; if dyld already mapped something there,
  the image gets no stubs and every LSE site takes the trap path. Per-(vnode, slice) contexts would also stop each
  process translating a shared dylib again.
- **Data in `__text` for images without a plan** relies on the island heuristic: validated on both caches (all 11
  data words that looked like code caught in iPadOS 26.5.2, 0 missed in macOS 27), not proved.
- **Kexts**: 22% of the Apple kernel addresses the 48 kept kexts use have no name in our kernel yet; calls there panic
  with the address. corecrypto's SHA3/SVE paths stay behind its CPU checks.
- **The plan, the kext names and the bindings are J171aAP's and predate the classifier above.** They were made from
  iPad11,6's (iPad 8, A12) 26.5.2 files on 2026-11-04. The target is now iPad12,1 (iPad 9, J181AP, A13) for 26 and
  27, so all three are redone from J181AP's files with this package's `arm64e plan`, `kext` and `xnu/prelink26`; the
  "no PAC/LSE/FP word left" figure over the cache is re-measured then.
- **A13 code outside feature checks.** a64x treats A13+ classes (dot product, FHM, SHA3, FlagM, RCpc2) as
  feature-gated and leaves them. Before J181AP's 26.5.2 cache is used, `arm64e scan -i dsc CACHE` must show none of
  them in hot libraries outside CPU-feature-gated code; if it does, 26 stays on J171aAP. J181AP's 26.5.2 and 27.2
  kexts hold the same classes as J171aAP's (dot product, FHM, RCpc2: 0).
- Nothing has run on the Switch.

## Not in the repository

Nothing derived from Apple's binaries is committed (the IPSW policy): the plan file, the 26.5.2 kernelcache, the
prelinked kernel and the cache-derived word lists the 2026-11-04 tests also ran on. To redo them: fetch the SystemOS
cryptex of iPadOS 26.5.2 (23F84; iPad12,1 now, iPad11,6 then) by range request and decrypt it with `aea decrypt` (key
from `ipsw fw aea --key`), then `arm64e plan -o dyld_shared_cache_arm64e.a64xplan .../dyld_shared_cache_arm64e` and
`arm64e scan -u words.txt dsc .../dyld_shared_cache_arm64e`; the kernel side is in [xnu/README.md](xnu/README.md).
