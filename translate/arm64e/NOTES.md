# Lab notes: the measurements behind README.md

Recorded while the translator was built (2026-11-04), in the order the work was done; README.md is the current
design and wins where the two differ. Paths are those of the scratch layout of the time: `a64x/`, `tools/` and `tests/`
are this package's `a64x/`, `tools/` and `test/`; `kernel/src/` is `xnu/bsd/`; `prelink26/` is `xnu/prelink26/`.

Everything below is measured unless marked "plan". Section 9 summarises the second half of the work; where an
older paragraph disagrees, the later section wins.

## 0. Which project is "the samsung one"

- oosexgang (hakios) has no Samsung device. The Samsung HakiOS ports are **Scion-Project** (Galaxy S9 `starlte`,
  S9+ `star2lte`, Note9 `crownlte`, all Exynos 9810) and Luminous418/hakios-a32 (Galaxy A32, MT6769T). All run the
  **iOS 16.5 arm64** (iPhone X) userland. None has an arm64e tool.
- What the Samsung one (Scion starlte) *does* teach: its Exynos M3 big cores are **ARMv8.0** like our A57. Its README:
  a count over the iPhone X arm64 cache found LDAXR/STLXR (8.0 atomics) and no CAS/LDADD/SWP; they had to (1) disable
  PAN (8.1) in the kernel (`pan=0`) and (2) lower the ID registers user space sees (`isa_floor`, patch 1510) so
  runtime-dispatched code does not pick 8.2 paths. We need neither (all four A57s are 8.0 and XNU reports the boot CPU).
- The arm64e -> arm64 tool in this family is **Bergamota's Renegade** (github.com/ry-diffusion/bergamota,
  `tools/renegade-rs/src/pacstrip.rs` + `dsc.rs`; Poco F1 / SDM845, iOS 18.7.8 and an iOS 26.6.2 branch). It rewrites
  every arm64e Mach-O and the whole dyld cache **offline**, 4 bytes for 4 bytes, PAC only (SDM845 is v8.2 and has LSE):
  RETA*->RET, BRA*->BR, BLRA*->BLR, PAC*/AUT*/XPAC*->NOP, PACGA->EOR, LDRA*->LDR/LDUR (unencodable ones left),
  FJCVTZS left, then fixes the cache's page hashes, re-HMACs corecrypto's FIPS `__fips_hmacs`, re-signs ad hoc.
  Its LDRA rewrite is only applied to an allowlist of images because some images carry data in `__text`.
- Others read: superboyyy/hakios-arm64e-dada (iOS 26 on Snapdragon 8 Elite: **native PAC**, no translation; its
  docs/ARM64E.md is the best write-up of what xnu-12377 expects of arm64e), UniXNU (`docs/porting/arm64e.md`: arm64e
  only on PAC hardware; it has an `ipados-26.5.2-23F84` profile and a 68-patch xnu-12377.121.6 series for the BCM2837
  board) and UniXNU-sheng (iPadOS 27.0.1 on xnu-12377 with Darwin 26 shims, SM8550 = PAC hardware).
- So nobody translates for an ARMv8.0 core. Everything ARMv8.1+ below is new work; the PAC table follows Renegade
  (with the user's identity semantics: PACGA writes 0).

## 1. Target build

- iPadOS 27 needs A13+ (iPad12,1 and up); the A12 iPads stop at 26.x. Published XNU stops at **xnu-12377.121.6**
  (26.5). Picked **iPadOS 26.5.2 (23F84), iPad11,6 (iPad 8, A12, J171aAP)**: lowest-end A12 iPad, home button, the
  kernelcache says `xnu-12377.122.8~1/RELEASE_ARM64_T8020`, i.e. the published 12377.121.6 is its source (26.7.2 is
  newer but its kernel source is not public). UniXNU already has a `ipados-26.5.2-23F84` profile (for iPad17,3).
- IPSW (not downloaded whole): 094-56745-108.dmg.aea OS 7.18 GB, 094-54813-109.dmg.aea SystemOS cryptex 1.98 GB
  (5.19 GB decrypted, dyld cache 4.6 GB, 84 subcaches), 094-54871-112.dmg AppOS 14 MB, kernelcache 19.7 MB.
  Fetched with HTTP range requests (scripts/remotezip.py) and streamed through `aea decrypt` (key from `ipsw fw aea
  --key` on the first 1 MB): nothing but the decrypted cryptex ever hit the disk.

## 2. Inventory (measured): iPadOS 26.5.2 dyld shared cache, every instruction section of all 4242 images

`build/a64scan dsc .../dyld_shared_cache_arm64e` (tools/a64scan.c + a64x/a64x_classify.c), 781,640,235 code words
(3.0 GB). Classifier cross-checked with LLVM 23 (`tools/oracle.py`): on all 56,772 distinct flagged words and a
2.68 M-word sample of all code, **0 instructions that LLVM decodes for apple-m4 but not cortex-a57 are missed**
for the A12 classes; the only "missed" are SVE/SME/CSSC/MTE/LRCPC3 (A15+), which are feature-gated.

| class | sites | what to do |
|---|---:|---|
| BRAA/BRAB/BRAAZ/BRABZ | 3,998,313 | -> BR Xn (1:1) |
| BLRAA/BLRAB/BLRAAZ/BLRABZ | 6,708,073 | -> BLR Xn (1:1) |
| RETAA/RETAB | 5,539,447 | -> RET (1:1) |
| PAC*/AUT* register forms | 11,714,607 | -> NOP (1:1) |
| XPACI/XPACD | 1,437,055 | -> NOP (1:1) |
| PACGA | 687 | -> MOV Xd, XZR (1:1, user: PACGA writes 0) |
| LDRAA/LDRAB | 1,650 | -> LDR/LDUR, pre-index LDR; far offsets need a 2-insn stub |
| hint-space PAC (PACIBSP...) | 11,610,972 | already NOP on v8.0 |
| **LSE CAS** | 42,251 | stub (LL/SC loop) |
| **LSE CASP** | 2,364 | stub (LDXP/STXP loop) |
| **LSE LD{ADD,CLR,EOR,SET,SMAX,...}/ST*** | 750,393 | stub |
| **LSE SWP** | 7,470 | stub |
| LDAPR{B,H} (RCpc) | 133,939 | -> LDAR (1:1, stronger) |
| FP16 arithmetic | 69,752 | stub or trap (libBNNS 23k, VideoToolbox 16k, CoreGraphics 2.8k rgbaf16 compositing) |
| FCMA (FCMLA/FCADD) | 25,046 | 25,037 in libBLAS (CPU-dispatched kernels) |
| FJCVTZS | 461 | JavaScriptCore 449, WebCore 12 |
| RDM | 2 | |
| A13+ (feature-gated): AMX 664k, SVE/SME 554k, msr.pstate 4951, sysreg 3.7k, dotprod 995, sha3 1389, ... | | left alone; trap fallback logs |

Per 16 KB page: 195,786 code pages, **191,882 (98%) need at least one 1:1 rewrite**, 26,587 (13.6%) hold
stub-needing sites; max 1,466 such sites in one page. macOS 27 (this Mac's arm64e cache, 909 M words) looks the same.

LSE in the hot libraries (sites): libobjc 486 CAS/26 LDOP, libsystem_platform 42/30, libsystem_pthread 85 CAS,
libdispatch 219 CAS/399 LDOP/69 SWP, libsystem_malloc 427 CAS, libswiftCore 147 CAS/128 CASP/146 LDOP,
CoreFoundation 221/53, Foundation 54/331, QuartzCore 144/2116, UIKitCore 9/54. The biggest LSE users are WebCore
(141k LDOP, 23.6k CAS), WebKit, JavaScriptCore, Recon3D, AppleCV3D, libusd.

Data in `__text`: some images embed tables (e.g. libsystem_m doubles 0x3ff00000...): the classifier only flags
encodings that are allocated on A12 (checked against LLVM), which removes most data hits; TextToSpeechMauiSupport's
1,628 LDRAA look like real code (`ldraa x9, [x8, #56]!` runs).

## 3. The dyld cache has no room for stubs (measured)

`tools/dsclayout.py`: the 26.5.2 cache is three packed regions: text 1620 MB, text 1755 MB, text 345 MB, each
followed by data/readonly/linkedit; the only VA gaps are six 32 MB holes inside the data areas. A B/BL reaches
+-128 MB, so a per-region stub island cannot reach most text, and moving subcaches breaks ADRP (text->data) and
every slid pointer. **Stubs for cache code must live inside the cache text.**

Dead space inside translated cache text (`tools/a64holes.c`, code sections only):

- 4,464,535 `brk #0xc47x` PAC-failure traps. 2,568,458 are the LR check
  `autibsp; eor x16,x30,x30,lsl #1; tbz x16,#62,+8; brk` before tail calls and 1,424,436 the
  `aut; mov x17,x16; xpac x17; cmp x16,x17; b.eq +8; brk` pointer check. With nothing ever signed these checks always
  pass, so each becomes `B ok` + 3 free words. Total free: ~12.0 M words (46 MB) in 3-word holes.
- Stub demand: 897,738 sites x ~6-12 words. Only 3.4% of sites fit in holes of their own 16 KB page, so placement
  must be global (any hole within +-128 MB), i.e. planned ahead -> see design.

## 4. Design (decided)

### 4.1 Instruction rewriting (a64x/, implemented, tested)

- `a64x_classify.c`: class of a word (table in section 2). `a64x_rewrite.c`: one-word rewrites and stubs.
- **PAC is the identity** (user direction 2026-11-04): PAC*/AUT* register forms, XPACI/XPACD -> NOP;
  PACGA -> `MOV Xd, XZR`; BRAA/BRAB/BRAAZ/BRABZ -> BR; BLRAA.. -> BLR; RETAA/RETAB -> RET; LDRAA/LDRAB ->
  LDR (offset >= 0), LDUR (-256..-8), LDR pre-index (writeback, -256..255), else a 3-word stub (ADD/SUB + LDR).
  Hint-space PACIBSP/AUTIBSP/XPACLRI stay (NOPs). dyld's auth fixups must write plain targets (kernel side: 4.3).
- LDAPR -> LDAR, LDLAR -> LDAR, STLLR -> STLR, DC CVAP -> DC CVAC (1:1).
- LSE -> out-of-line stub, the site becomes `B stub`, the stub ends `B site+4`. The stub is what LLVM emits for
  ARMv8.0: `ld{a}xr / op / st{l}xr / cbnz` (acquire = A bit, release = R bit); CAS adds `eor/(and mask)/cbnz fail`
  - `clrex` on failure; CASP uses LDXP/STXP and stores the old pair back on a failed compare (the 128-bit read is
  only single-copy atomic if a store-exclusive succeeds); SMAX/SMIN/UMAX/UMIN use CMP+CSEL with NZCV saved via
  MRS/MSR; B/H forms sign/zero-extend for the compare. Scratch: registers proven dead by a path-sensitive scan of the
  page (`a64x_dead_regs_window`: writes-before-reads, x16/x17/x30 dead at calls, BRK paths ignored); otherwise
  spilled with `sub sp,sp,#(128+16n)` (skips Darwin's 128-byte red zone) / `stp` / ... / `ldp` / `add sp`.
- FP16, FCMA, FJCVTZS and RDM now have stubs too (section 9.3). Not rewritten (left for the kernel's
  undefined-instruction path, counted): A13+ feature-gated classes (AMX, SVE/SME, DotProd, SHA3, ...), which
  A12-baseline code only runs after checking hw.optional/commpage bits that our kernel reports as 0 on the A57.

Tests (all pass):

- `tests/test_semantics.c`: runs the original instruction natively (this Mac has LSE) and the stub from the same
  random state; compares x0-x30, SP, NZCV, target memory and the red zone. **All 6,188 distinct LSE/LDRA words of the
  iPadOS 26.5.2 cache x 6 trials x {spill, dead-register} modes = 74,256 checks, 0 failures**; 1,536 systematic
  encodings (every op/size/order, Rt=XZR, Rt=Rs, Rt=Rn, Rn=SP, Rs=XZR) x 4 x 2 = 12,288 checks, 0 failures. A mutation
  (one bit flipped in a stub) is caught. 4 threads x 1M translated LDADDAL/LDADD/LDADDAL(w) on one counter: exact.
  (3 CASP words with odd registers in the cache are unallocated encodings = data; correctly refused.)
- `tools/a64xlate.c` (page-by-page, stubs after the image, the kernel's per-page algorithm): 17 iPadOS 26 dylibs
  (14.8 M code words): 276,486 one-word rewrites, 43,994 stubs (mean 10.9 words), every site/stub/return branch
  verified; LLVM (cortex-a57) on all 1,755,905 distinct output words: everything it cannot decode is either a
  deliberately-left class or data (invalid on apple-m4 too). 1,402 macOS 27 executables (67.5 M words): 2,443,086
  one-word rewrites, 5,608 stubs, 0 bad.

### 4.2 Dyld cache stubs: planned into dead PAC checks (tools/a64plan.c, measured)

- Holes: 2,568,458 LR checks (3 free words each) + 1,424,434 pointer checks (5 words) = 14.8 M words (56.6 MB).
- Allocation: per site the nearest 1 MB bin (+-120 MB) with room; all pieces of one stub in one bin (CBNZ +-1 MB);
  each piece ends with `B next`; internal branches relocated. Hot libraries first.
- Result (26.5.2 cache): **all 10,379 sites in the hot libraries placed** (libsystem_*, libobjc, libdispatch, libc++,
  libswiftCore, CoreFoundation, Foundation, QuartzCore, UIKitCore, CoreGraphics, IOKit, Metal, IOSurface,
  Front/BackBoard*, RunningBoardServices, CoreText, Security, CFNetwork, ImageIO, sqlite, SwiftUI, AttributeGraph,
  CoreUI, ...). Overall 436,754 of 802,479 (54%); mean 3.8 pieces, 9.8 words. The 365,725 left are in LSE-dense
  images (WebCore 165k, WebKit 63k, Recon3D 58k, AppleCV3D 49k, libusd 46k, JSC 40k): their +-120 MB neighbourhood
  runs out of holes. Next steps, not done: (a) x30 is provably dead at 215,386 sites -> `BL shared_stub` + `RET`
  (one copy per 128 MB region per distinct word), (b) 1-word hops to reach bins 240 MB away, (c) stop spilling where
  liveness can be proven with function bounds (LC_FUNCTION_STARTS); until then those sites take the trap path.
- Holes are found with the same function the kernel runs (`a64x_dead_checks`, only checks wholly inside one 4 KB
  page): 2,560,993 + 1,417,469 holes; placement unchanged (435,823 sites, hot libraries 10,379/10,379).
- Plan file `out/dsc26.a64xplan` (kernel format, 49.7 MB): 7,721 merged code ranges, 276,935 pages, 5,940,725 words.
- End-to-end check (`a64plan ... dump`): simulating the kernel transform on all 952,848 executable pages
  (one-word rewrites + dead checks + plan words) changes 37,386,650 code words; **all 435,822 planned LSE sites
  follow planned words back to site+4, 0 broken**. LLVM on all 43,249,485 distinct translated code words: what a
  cortex-a57 cannot decode is only: unplaced LSE (2,610 distinct words; trap path), FP16 (20,363), FCMA (1,013),
  FJCVTZS (35), A13+ feature-gated (SVE/SME 24,753, i8mm/bf16 521, dotprod 359, cssc 301, mte 120, sha3 120, fhm 56,
  rcpc2 7) and data (invalid on apple-m4 too). **No PAC instruction is left.**

### 4.3 Kernel (xnu-12377.121.6 + `scripts/patch_xnu_a64x.py`; tree `src/xnu-a64x`, diff `kernel/a64x-xnu-12377.diff`)

- One mechanism for every arm64e mapping: the **apple_protect pager** (FairPlay's). Its data_request faults the
  source page from the file object, runs `vm_page_validate_cs_mapped` on it (the image's or the cache's own code
  signature: original Apple hashes) and only then calls the crypt_info's `page_decrypt`, which is our translator;
  the destination page inherits cs_validated/tainted from the source. So code is verified before it is rewritten,
  and nothing on disk changes (APFS seals, cdhashes, trust caches all stay Apple's).
- `bsd/kern/a64x_xnu.c`:
  - `a64x_load_image()` (called at the end of `parse_machfile` for arm64e: main executable and dyld) and
    `a64x_mmap_exec()` (called from `mmap()` after an executable file mapping that starts with an arm64e header:
    dyld mapping a dylib's __TEXT): parse the load commands; read __TEXT once to lay out the stubs (page cursors);
    map the stub region right after the image (same distance in every process), backed by validated pages of the same
    file through a second apple_protect pager (`a64x_stub_page`); then `vm_map_apple_protected()` the __TEXT range
    with `a64x_text_page`. At page-in a text page regenerates its own stubs from the *validated* words and compares
    with the stub region; if they differ (file changed under us) the page keeps its LSE words (trap path).
  - `a64x_shared_cache_pager()`: for each executable dyld-cache mapping (vm_shared_region.c), an apple_protect pager
    with `a64x_cache_page`: one-word rewrites in code ranges, `a64x_dead_checks`, then the plan's words for that
    4 KB page. The plan is read once from `/private/preboot/a64x/dyld_shared_cache_arm64e.a64xplan` into pageable
    kernel memory and used only if its SHA-256 equals `/chosen/a64x-plan-sha256` (written by the packager).
    No plan: the cache still runs (all one-word rewrites), every LSE site traps.
  - `a64x_emulate_user()` (from `handle_uncategorized` in sleh.c, EL0 only): LSE that has no stub, emulated with
    exclusive loops on the user address under a copyio recovery range (`_a64x_user_rmw/_cas/_casp` in
    machine_routines_asm.s, wrappers in copyio.c). PAC is never emulated (user direction): it is rewritten, and
    MAP_JIT is refused for arm64e processes so no untranslated arm64e code can exist (JavaScriptCore falls back
    to its interpreter). FP16/FCMA/FJCVTZS/RDM are not emulated yet: one log line each for the first 32, then the
    usual EXC_BAD_INSTRUCTION. sysctl kern.a64x.{images,text_pages,cache_pages,stub_words,stub_mismatch,traps,
    traps_unhandled}.
- Other edits: `osfmk/arm/cpuid.c` reports CPU_ARCH_ARMv8E (so `hw.cpusubtype`, exec grading and the shared region
  are arm64e); `vm_shared_region.c` slide v3/v5 write authenticated rebases as plain pointers on a non-ptrauth
  kernel (stock code skips the write: auth pointers would stay encoded); `vm_dyld_pager.c` (page-in linking) handles
  the DYLD_CHAINED_PTR_ARM64E* formats with an identity `signPointer`; `CONFIG_A64X` option (`<a64x>` in MASTER,
  enabled in MASTER.arm64.bcm2837 next to config_code_decryption).

### 4.4 Why not the alternatives (measured reasons)

- **Trap-and-emulate everything**: RETAB/BLRAA/BRAA are in nearly every function (16 M sites) and LSE sits on every
  lock and refcount (os_unfair_lock, objc/Swift/CF retain-release, dispatch). An EL0 undefined-instruction round
  trip is hundreds of cycles; an inline LL/SC atomic is tens. Kept only as the fallback, counted.
- **Offline rewrite of the image (Bergamota's way)**: same bytes in memory, but every binary and cache file gets new
  page hashes, so new cdhashes, a regenerated trust cache and a re-sealed system volume, redone for every IPSW; and
  it does not solve the cache's lack of address space either. The load-time way leaves Apple's signatures, seal and
  trust caches untouched and verifies Apple's hashes at every page-in.
- **Stubs in a region next to each cache region**: impossible, the cache text is packed (section 3).

### 4.5 Cost (measured on this Mac; relative)

- One atomic increment, single thread: native LDADDAL 3.26 ns; the inline LDAXR/ADD/STLXR/CBNZ an arm64 build would
  have 7.95 ns; a64x stub with free registers 8.68 ns (+9%); spilling stub 9.33 ns (+17%). So against plain arm64
  code on the A57, translated atomics cost ~10-20% more (two extra taken branches, plus a spill when no register is
  provably dead). Cache stubs split over 3-4 holes add 2-3 more taken branches and usually a distant I-cache line.
- Page translation: ~4.5-5 us per 4 KB page here (CoreFoundation, whole file), after an exhaustive 16-bit-prefix
  fast-reject bitmap (8 KB; 11,025 of 65,536 prefixes can hold a class). Expect ~4x on an A57: ~20 us per page,
  once per page-in (a 4 KB random SD read is ~100-300 us).
- Trap path (unplaced cache LSE, ~46% of sites, all in WebKit/3D/ML frameworks): exception + emulation, roughly
  0.1-0.3 us per op on the A57 (estimate, not measured).

## 5. iPadOS 26.5.2 on ios-on-switch-lmao: state and plan

What exists:

- Translator + tests (sections 2-4), the cache plan for 23F84 (`out/dsc26.a64xplan`, SHA-256 in section 7), the
  kernel patch for xnu-12377.121.6 (`kernel/a64x-xnu-12377.diff`) and a build check (section 6).

What a bootable 26.5.2 still needs (in order):

1. **Kernel**: xnu-12377.121.6 + UniXNU's `patches/xnu-12377` series as in its arm64e profile minus
   `native-pac-keys.patch`, `arm64e-threaded-starts.patch`, `native-arm-pan.patch` (A57 has no PAC, no PAN), built
   **arm64** for `MACHINE_CONFIGS=BCM2837` with `a64x` enabled + the ios-on-switch-lmao board/driver changes. The
   ios-on-switch-lmao XNU diff (`tree/patches/xnu-switch.diff`, against xnu-10063.141.1) has to be ported to 12377 hunk
   by hunk (raw SD mount, SwitchRawMount/cryptex binding, ArmCpu SMP fixes, interrupt plumbing, display hooks...).
   UniXNU-sheng's iPadOS 27 bring-up patch shows the 12377-era equivalents (bindfs/vm_unix cryptex paths, shared-region
   4K slide, posix_spawnattr layout).
2. **Kexts**: the 26.5.2 kernelcache is an arm64e MH_FILESET (240 kexts). UniXNU's 26.5.2 profile lists the prelink
   roots (apfs, AMFI, sandbox, IOSurface, IOGPUFamily, IOHIDFamily, corecrypto, ...). Our kernel is arm64, the kexts
   arm64e: their text must be translated **at prelink time** (host side: same a64x rewrite in kernel mode; PAC
   identity, LSE stubs appended after each kext's __TEXT_EXEC; XNU is built without a red zone, the stubs move SP
   first anyway) and their auth fixups written as plain pointers by the prelinker. Not done.
   Measured (`tools/kc_scan.py`, 222 fileset entries, 9.29 M code words incl. Apple's kernel 2.2 M): PAC 476k
   (pac/aut 196k, blraa 159k, retab 74k, xpac 25k, braa 22k, pacga 17), LSE 21.3k (CAS 12.9k mostly in the kernel
   itself, which we replace; LDOP 8.3k), LDAPR 1k, FP16 15. APFS: 336k words, 1,472 LDOP, 16 CAS, 3 LDAPR;
   AMFI 2 CAS; sandbox 51 LDOP + 4 CAS + 1 SWP; IOSurface 9 + 6; corecrypto has SHA3/SVE paths behind its CPU checks.
   Here the layout is ours (the prelinker places each kext), so every LSE site gets an adjacent stub: no planner.
3. **Drivers**: every ios-on-switch-lmao IOKit driver (SD, GIC, display, GPU, touch, buttons, audio, Wi-Fi, BT, power)
   rebuilt against the 12377 KPIs; the m2v/NVK Metal plugin rebuilt for the iOS 26 SDK (its shader cache keys change).
4. **Identity**: ADT as J171aAP/iPad11,6 (or keep the iPad7,1-style generic iPad identity with the 26 product keys);
   delete the MobileGestalt cache on Data after the identity change (springboard-blockers note).
5. **Image**: OS volume (094-56745-108.dmg.aea, 7.18 GB encrypted), SystemOS and AppOS cryptexes and their
   trust caches (`Firmware/*.trustcache`, appended to the static trust cache as for 17.7), Preboot with
   `/a64x/dyld_shared_cache_arm64e.a64xplan`, and `/chosen/a64x-plan-sha256` set by the packager.
6. **Daemons**: the 17.7 workarounds (disabled.plist set, AKS stand-in, keybag dirs, setup-skip prefs) re-checked
   against 26; JIT is refused for arm64e processes so WebKit runs JavaScriptCore without JIT.

Booting it later without touching 17.7 (single entry `switchroot/ios`, as the user wants):

- Keep **one** Hekate entry and one folder. Put the 26 build next to the 17.7 one inside it:
  `switchroot/ios/kernel-window.bin` (17.7) and `switchroot/ios/kernel-window-26.bin`, and give the
  native loader one choice: if **Vol-** is held when it starts (or a file `switchroot/ios/boot-26` exists),
  load `kernel-window-26.bin`, else the 17.7 window. The ini, the menu and the entry name stay the same.
- In the APFS container on the SD's data partition, add the 26 volume group (OS, Data, Preboot) next to 17.7's; the
  26 kernel-window's ADT names the 26 volume UUIDs (`root_match_volume_uuid`), so 17.7 never sees 26's volumes. Space:
  ~16 GB for the 26 OS volume decrypted plus its Data.
- Try order: (1) 26 kernel with a 17.7-style test userland to check the kernel alone; (2) `hello` arm64e as
  launchd (translator end to end: exec, dyld, cache, plan); (3) launchd + daemons; (4) SpringBoard.

Layout facts for 23F84: the dyld cache lives in the SystemOS cryptex (`/System/Library/Caches/com.apple.dyld/`,
main + 83 subcaches + .symbols; text 3.63 GB in three packed regions), AppOS cryptex 14 MB; same cryptex scheme as
17.7, so SwitchRawMount's cryptex binding carries over. Kernelcache `kernelcache.release.ipad11b` (MH_FILESET,
arm64e, 240 kexts, `xnu-12377.122.8~1/RELEASE_ARM64_T8020`). Use the iPad11,6 / J171aAP identity: the userland and
its MobileGestalt answers were built for the ipad11b device class.

## 6. Build check (xnu-12377.121.6, MACHINE_CONFIGS=BCM2837, plain arm64, CONFIG_A64X)

Tree `src/xnu-build` = pristine + UniXNU's 61 xnu-12377 patches (arm64e profile order, minus native-pac-keys,
arm64e-threaded-starts; native-arm-pan does not apply without them and is not wanted on an A57) + `EXTERNAL_HEADERS/
TrustCache` from xnu-8796.101.5 (as UniXNU's profile does) + the a64x edits. `make SDKROOT=iphoneos ARCH_CONFIGS=ARM64
MACHINE_CONFIGS=BCM2837 KERNEL_CONFIGS=DEVELOPMENT ARCH_STRING_FOR_CURRENT_MACHINE_CONFIG=arm64 ...` (availability.pl
and libfirehose from the main tree's `.cache/fakeroot`, read only; OBJROOT etc. in `r8/build/xnu`).

- Every file the a64x patch adds or changes compiles with the kernel's own flags (target arm64-apple-ios, no PAC):
  `a64x_classify.c`, `a64x_rewrite.c`, `a64x_image.c`, `a64x_xnu.c`, `mach_loader.c`, `kern_mman.c`, `cpuid.c`,
  `copyio.c`, `machine_routines_asm.s` (`_a64x_user_rmw/_cas/_casp` present), `sleh.c`, `vm_shared_region.c`,
  `vm_dyld_pager.c`. No warning in the a64x files beyond the toolchain-wide `-Wreserved-identifier` noise that
  every XNU file shows with this clang.
- **The kernel links**: `build/xnu/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837.unstripped`, Mach-O
  arm64 (subtype ALL: no PAC), 21.5 MB, NOUNDEFS, `Darwin Kernel Version 25.5.0`, a64x inside (kern.a64x sysctls).
  Two arm64 adaptations of UniXNU's series were needed (kernel/unixnu-12377-arm64-fixes.md): TrustCache headers
  from xnu-8796.101.5 and an unconditional `__builtin_ptrauth_strip` in `unixnu_fileset_unmapped` (debug.c); and
  `IPHONEOS_DEPLOYMENT_TARGET=27.0` in the environment for the final link. Only the post-link symbol-set export
  check fails (Apple DART/UAT symbols absent on BCM2837), which does not affect the kernel binary.
- Not in it yet: the ios-on-switch-lmao board/driver port to 12377, UniXNU's component archives (ArmCpu, ArmDTPlatform,
  source corecrypto, pthread) and the prelinked kexts. Never booted.

## 7. Files and disk

`r8/` in this worktree (copied to `out/agent-scratch/r8-arm64e/`):

- `a64x/` core (classify, rewrite, stubs, liveness, page translation, dead checks); `tools/` a64scan (inventory),
  oracle.py + oracle_a57.py (LLVM checks), a64xlate (standalone images), a64holes, a64plan (+ a64plan_emit.inc;
  cache plan + simulation), dsclayout.py, gen_toptable.c, a64class.c; `tests/` test_semantics.c, bench_stub.c;
  `scripts/` remotezip.py (IPSW members over HTTP ranges), ghfetch.py, patch_xnu_a64x.py, a64x_user_atomics.s,
  prepare_xnu_build.py, rerun_compile.py, compile_like.py; `kernel/a64x-xnu-12377.diff` (against pristine
  xnu-12377.121.6); `out/` scan results; `out/dsc26.a64xplan` (49.7 MB, sha256
  d11887c8a12a0fbcd2c8724379977f096e3339259af7eb77c0659ba348de7753; regenerate with
  `build/a64plan -o PLAN .../dyld_shared_cache_arm64e`).
- Downloads: only IPSW members via range requests: SystemOS cryptex 1.98 GB streamed into `aea decrypt` (5.19 GB
  DMG, mounted read-only), kernelcache 19.7 MB, BuildManifest, AppOS 14 MB, restore ramdisk 235 MB. The IPSW itself
  was never downloaded.
- Disk now: 1.3 GB in `r8/` (XNU objects + linked kernel 700 MB in `build/xnu`, three XNU source trees 288 MB,
  17 extracted iPadOS 26 dylibs 182 MB, kernelcache 56 MB, plan + scan outputs 70 MB); 3 MB in
  `out/agent-scratch/r8-arm64e`. The decrypted cryptex (5.2 GB) and the ramdisk were deleted after use: the plan can
  be regenerated by re-streaming the cryptex (see 1). `build/xnu` can go once nobody needs incremental rebuilds.

## 8. Risks and open items (honest list)

- (Round 3) FJCVTZS/FP16/FCMA/RDM have stubs and every cache site is stubbed (9.1, 9.3). FP16 results differ from
  A12 only for signaling-NaN inputs.
- Data in `__text`: solved for the cache (9.2); the kernel's page-by-page translation of other images still rewrites
  any A12-valid PAC/LSE encoding inside instruction sections (no function starts or LSDA used there yet).
- The dead-check (`b ok`) and the liveness rules rely on the compiler contract that x16/x17/NZCV are clobbered by
  PAC check expansions and that x16/x17/x30 are dead at calls and returns.
- Text pages and stubs of non-cache images are per mapping (not shared across processes): fine for main
  executables, wasteful for a dylib loaded by many processes. A per-(vnode, slice) context cache fixes it.
- Kexts: kernel-mode translation at prelink time is designed (section 5) but not written.
- Nothing here ran on the Switch.

## 9. Round 3 (2026-11-04 pm): atomics at 100%, data in `__text`, FP stubs, A57-built binaries, kexts

### 9.1 Atomics: every cache site gets a stub (measured)

Planner `tools/a64plan2.c` (replaces a64plan for the cache). What changed and why it mattered:

- **Liveness over the whole function** (`a64x_dead_regs_flow_fn`): bounds from LC_FUNCTION_STARTS (13.6 M starts,
  read through the cache's LINKEDIT mapping); loops back to before the site are followed; a branch out of the
  function is a tail call (x16/x17 dead across it, x30 live). x30 is now provably dead at 778,935 of 802,467 LSE
  sites (was 640,759) -> `BL shared_stub ... RET`. Spot-checked by hand (libobjc, a CoreFoundation-area function): the
  newly-dead x30 is reloaded by `ldp x29, x30` or a later `bl` on every path. Union with the old window analysis would
  add 24 sites; not worth the time.
- Result for LSE alone: **802,467 / 802,467 placed (100%), 0 traps**, shared BL 778,935, one hole 834, pieces
  22,699 (mean 3.70 holes), 0 veneers; 2.10 taken branches per site on average; spills at 7,602 sites.
- With the FP classes added (9.3), final plan: **897,714 stub sites, 100% placed, 0 traps**, shared BL 801,255
  (18,532 shared copies, 164,209 words), one hole 7,965, pieces 88,453 (mean 5.36), 41 one-hop veneers; 2.49 taken
  branches per site; hot libraries 13,954/13,954. Plan `out/dsc26.a64xplan` 24.8 MB (9,655 code ranges, 142,435
  pages, 2,796,767 words), SHA-256 `f6b0e295f211f2568746b26e05938892909e9616a349b468fa64065c25a5a15d`.
- Verification (simulates the kernel transform on all 952,848 executable pages): 34,626,827 code words change;
  **every path of every stub site (conditional branches included, `follow()` explores both ways) stays in plan
  words and ends at the site's return: 897,714 ok, 0 broken**; 0 duplicate plan addresses. LLVM (cortex-a57) on all
  43,192,529 distinct translated code words: **no PAC, LSE, LDAPR, FP16, FCMA or FJCVTZS word is left**; what the A57
  cannot decode is A13+ feature-gated code (SVE/SME 24,685, i8mm 520, dotprod 359, cssc 300, mte 117, sha3 116, fhm
  56) and data (invalid on apple-m4 too), plus 3 junk words in unreached obfuscated code.
- LSE stub semantics re-run after all changes: 6,188 cache words x 6 x {spill, dead} = 74,256 checks, 0 failures;
  1,536 systematic encodings, 12,288 checks, 0 failures; 4 threads x 1 M on one counter exact.

Fastest emulation, measured on this Mac (`tests/bench_atomics.c`, `out/bench_atomics.txt`; ns per `ldaddal`,
single thread, uncontended, median of 5) and estimated for the A57 (C = an uncontended LDAXR/STLXR pair in L1,
assumed 20-30 cycles on Cortex-A57, **not measured**; the A57 adds ~1 fetch bubble per predicted taken branch):

| approach | Mac ns/op | A57 estimate | where used (cache) |
|---|---:|---|---|
| native LDADDAL (A12, reference) | 1.82 | n/a | |
| inline LDAXR/ADD/STLXR/CBNZ (what an arm64 build has) | 3.97 | C | impossible in place: 1 word -> 4-6 |
| stub, B there / B back, free registers | 3.98 | C + 2 | one-hole and piece stubs |
| **BL shared stub + RET (x30 dead)** | **3.88** | **C + 2** (RET predicted) | **89% of all sites** |
| stub that spills (STP/LDP below the red zone) | 4.01 | C + 4..6 (STLXR waits for the spill store) | 2.3% of sites |
| stub in 3 pieces (4 KB / 1 MB apart) | 4.18 / 4.15 | C + 4..5, + ~20 per cold I-line | ~10% (mostly FP16) |
| one-hop veneer in and out | 3.87 | C + 4 | 41 sites |
| 2048 sites, stubs 1 MB / 32 MB away (I-cache) | 3.93 / 4.03 (native 2.87) | C + 2, +L2 misses when cold | |
| trap: kernel emulation (sleh -> a64x_emulate_user) | n/a here | ~500-1500 cycles | 0 sites now |
| trap: UDF + user SIGILL handler | 2344 | far worse | not used |

So the translated atomic costs what the inline LL/SC an arm64 build would have, plus about two predicted branches.
Nothing faster fits: the site is one word, the LL/SC sequence 4-6. Options looked at and dropped: inline into the
dead PAC check right after the site (holes are almost never adjacent; the BL is as cheap); per-region stub islands
(the cache text is packed, 3.); trap fast path (500+ cycles is 20x the stub).

### 9.2 Data in `__text`: found and excluded (this was a real bug)

A recursive-descent pass now decides what is code before anything is rewritten (`reach_pass` in a64plan2.c):
roots = function starts that pass a probe (no unallocated encoding in their first straight-line run; 127 data symbols
rejected), **4,359,493 landing pads** from 687,729 LSDAs (__unwind_info LSDA index -> call-site tables), and
**86,712 arm64e hardened jump tables** (`cmp/csel/adrp/add/ldrsw/adr/add/br x16`, 1,879,590 targets, 0 unresolved);
fall-through stops at the next function start. 97.3% of code words are reached; no reached word is an unallocated
encoding. What is not reached is cut into runs at UDF/zero padding; a run with >= 1 unallocated word per 32, or that
starts at a rejected function start, is data and leaves the code map (and the plan's code ranges, so the kernel does
not touch it either). Dense obfuscated code reached only through register branches (CoreALD, SoftPosReader,
MFAAuthentication, Swift value-witness tables) carries ~1 junk word per 500 and stays code.

- Result: 1,935 data runs, 18,805 words. **11 would-be LSE rewrites in them: six copies of SHA-256's K256 table
  (corecrypto and friends), libm's sincos coefficients, AudioCodecs' Hann windows.** The previous planner turned 8 of
  those into `BL stub`, i.e. it would have corrupted SHA-256 constants. Fixed in the plan above.
- Same risk exists for non-cache images translated by the kernel page by page (no function starts there): open item.

### 9.3 FJCVTZS, FCMA, FP16, RDM: stubs, checked against this Mac's own instructions

`tests/test_p2.c` runs the original word natively and its stub (generated twice: nothing dead, and x16/x17 dead) from
the same random state and compares X0-X30, NZCV and Q0-Q31 (FPSR not compared). Lane values mix specials (+-0,
+-inf, qNaN/sNaN with payloads, subnormals, extremes, int32 boundaries, 2^63/2^84 for JS conversions).

- **FJCVTZS** (461 sites, JavaScriptCore/WebCore): exact JS ToInt32 (truncate mod 2^32, NaN/inf -> 0) and Z = "was an
  int32 other than -0.0", N=C=V=0 (built with CCMN so the flags are exact). Fast path 10 instructions for
  1 <= |x| < 2^31; other ranges via the raw bits. 6 encodings x 200,000 states: 0 mismatches.
- **FCMA** (25,045 sites, libBLAS): FCADD/FCMLA vector and by element, single and double: lane-for-lane FADD/FMLA on
  operands permuted with REV64/EXT/TRN/FNEG/DUP, so each lane is the same fused operation on the same values (NaN sign
  included). All 1,012 distinct cache words x 3,000 states + 116 synthetic (all rotations, 2s/4s/2d, aliasing) x
  20,000: 0 mismatches, sNaN included.
- **FP16** (69,741 sites, ~100 forms: BNNS, VideoToolbox, CoreGraphics, ...; `a64x/a64x_fp16.h`): widen (exact),
  operate in single, narrow once; exact because 24 >= 2*11+2. Fused ops (FMADD family, FMLA/FMLS incl. by element,
  FRECPS/FRSQRTS) go through double and narrow with FCVTXN (round to odd) then FCVTN. FABS/FNEG/FMOV/FCSEL work on raw
  bits. **All 20,360 distinct cache words have a stub; 300 states each: 0 mismatches without signaling NaNs.** With
  sNaN inputs 1.2% of runs differ: widening quiets an sNaN before the operation, so payload choice (and FMAXNM's
  "sNaN is not missing") can differ. Exhaustive half FMA (`tests/test_fma16.c`): all a x b for 8 addends, 33.3 G
  cases, scalar and vector paths vs native FMADD h: 0 mismatches.
- **RDM**: 0 reachable sites in the cache (the 2 the scanner saw are data). Implemented anyway (SMULL + SSHLL + ADD/SUB
  in 2e-bit lanes, which cannot overflow, then one SQRSHRN): 104 synthetic forms (4h/8h/2s/4s, scalar, every
  element index, add/sub, aliasing) x 50,000 states with saturation edges: 0 mismatches.
- Every stub word for all of the above decodes on cortex-a57 (LLVM). Vector temporaries are always spilled (Q
  STP/LDP below the red zone; no SIMD liveness yet), so FP16 vector stubs cost 4-8 extra memory ops; FP16 FMLA .8h in
  double is ~45 instructions. Correct first; a V-register liveness pass is the obvious next speed-up.
- The kernel's per-image translator (`a64x_image.c`) now gives these classes stubs too; A64X_STUB_MAX is 64.

### 9.4 Binaries built for the A57 (user requirement): loaded as is

Marker: **a section `__TEXT,__a64x_target`**, added with one linker flag
(`-Wl,-sectcreate,__TEXT,__a64x_target,a64x_target.txt`; contents are free text naming the target). Chosen over a
CPU subtype (dyld/exec grading accept ARM64_ALL everywhere; ARM64_V8 is not produced by the toolchain), over an
LC_BUILD_VERSION tool entry (needs post-link editing) and over LC_NOTE (needs post-link editing, untested in dyld).
It sits inside __TEXT, so the code signature covers it.
Loader rules (`a64x_image_kind()` in sys/a64x_xnu.h, `a64x_map_image()` in a64x_xnu.c, same for exec and mmap):

- marker present (any subtype): mapped as is, never scanned (`kern.a64x.images_marked`). If it still executes
  something the A57 lacks, the undefined-instruction path emulates LSE (`kern.a64x.traps`) and logs anything else.
- arm64 (ALL/V8) without marker: scanned once at load like arm64e; **no PAC rewrites** (`A64X_MAP_NO_PAC`); if the
  scan changes nothing the file's own mapping stays (byte-identical, no pager, `kern.a64x.images_native`), otherwise
  only the words the A57 lacks go through the translating pager. An arm64 image the translator cannot lay out runs
  untranslated instead of failing to load.
- arm64e: full translation, as before. MAP_JIT is still refused only for arm64e processes.
- Signing and trust cache: unchanged and identical for all three; translated pages are produced from source pages
  that `vm_page_validate_cs_mapped` checked first, untouched images are paged in by the vnode pager as usual.
`tools/a64check` reports what the loader will do with any Mach-O (thin, fat or an MH_FILESET). Test
`tests/passthrough/run.sh` (hello world with an atomic counter, a double->int and a function pointer):

| binary | loader decision | words changed | runs |
|---|---|---:|---|
| `hello_a57`: arm64, `-march=armv8-a -mcpu=cortex-a57 -mbranch-protection=none`, marker | as is (not scanned); `-a` scan: 0 of 4,096 | 0 | yes |
| `hello_a57_unmarked` | scanned, nothing to do, as is | 0 | yes |
| `hello_a12`: arm64 `-mcpu=apple-a12` | 1 LDADDAL -> stub, no PAC work | 1 | yes |
| `hello_a12e`: arm64e | full (2 BRAA, 1 BLRAA, 1 RETAB, 1 LDADDAL) | 5 | yes |
| `hello_a57_ios`: same flags, `-target arm64-apple-ios26.0`, marker | as is; `-a`: 0 of 8,192 | 0 | (not runnable here) |

**Marker contract, build side (round 5 finding):** at an iOS 26+ deployment target Apple clang (2100.3.20.102) gives
`-arch arm64` an A12 feature set (LSE, LDAPR, FP16, JSCVT, FCMA, RDM; `__ARM_ARCH_8_2__/8_3__` defined) and
`-march=armv8-a` does **not** take it away (measured: `__atomic_fetch_or` -> LDSET, acquire load -> LDAPR).
A marked binary must therefore be built with **`-mcpu=cortex-a57`** (as tests/passthrough does); `-march=armv8-a`
alone breaks the promise, and the trap path emulates LSE (slowly) but not LDAPR (SIGILL).

### 9.5 Kexts (prelink time, kernel mode): scanned, not integrated

`build/a64check dl/ipsw-26.5.2/kernelcache.decompressed` translates every fileset entry of the 26.5.2 kernelcache in
kernel mode with stubs laid out after each entry's text (`out/kernelcache-kext-check.txt`): 220 kexts (Apple's
kernel is replaced and skipped), 422,677 words change (PAC 420k), 1,984 LSE stubs + 13 FP16 = 14,431 stub words
(57 KB) for the whole set. Left: corecrypto `msr dit`/`mrs dit` (21) and one `sb` -> need kernel-mode rules (MSR DIT/
SSBS/PAN #imm -> NOP, MRS DIT -> MOV #0, SB -> DSB SY + ISB) before corecrypto can load; SVE/SME/SHA3 paths behind
CPU checks (corecrypto, Compression, zlib, AVE2, DispH11P); Apple-impdef system registers only in SoC drivers we
replace (CLPC, PMGR, watchdog, VortexErrorHandler); kec.Libm's leftovers are data tables (needs the 9.2 pass).
Still to do: the prelinker step that appends the stub section per kext and writes auth fixups as plain pointers.

### 9.6 Not done this round

- Porting the ios-on-switch-lmao XNU diff (10063 -> 12377): not started.
- The kernel files were edited without a tree to compile them (the pristine 12377 tree was deleted for disk): the
  shared a64x/*.c compile and are tested on the host; `a64x_xnu.c` changes are small but **not compiled**.
  `kernel/src/` holds every new kernel file; `scripts/diff_newfiles.py` rewrites them into the diff.

### 9.7 Files and disk after round 3

- Code: `a64x/` (now with `a64x_fp16.h`), `tools/a64plan2.c` (cache planner: function-scoped liveness, data islands,
  landing pads, jump tables, FP classes, full-path verification), `tools/a64check.c` (loader decision for any
  Mach-O/fileset), `tools/mnemhist.py`, `tests/test_p2.c`, `tests/test_fma16.c`, `tests/passthrough/`,
  `kernel/src/` + `scripts/diff_newfiles.py` (the diff's new files), `kernel/a64x-xnu-12377.diff` (regenerated).
- Results in `out/`: `dsc26.a64xplan` (24.8 MB, sha256 above), `dsc26-plan-final.txt`, `dsc26-translated-oracle.txt`,
  `dsc26-translated-a57invalid.tsv`, `dsc26-fp-sites.txt`, `kernelcache-kext-check.txt`, `bench_atomics.txt`.
- Deleted after use: the decrypted SystemOS cryptex (5.2 GB, `sysos.dmg`, unmounted), the 389 MB translated-word dump
  and other intermediates. The scratch dir is 87 MB (56 MB of it the 26.5.2 kernelcache, kept for the kext work).
  To re-plan: re-stream the cryptex (section 1) and run `build/a64plan2 -o PLAN .../dyld_shared_cache_arm64e`.

## 10. Round 4 (2026-11-04 evening)

### 10.1 The kernel side compiles and links (measured)

Re-fetched pristine xnu-12377.121.6 (apple-oss-distributions tarball, 18 MB), UniXNU's 68 xnu-12377 patches + the arm64e
profile series (GitHub API), TrustCache headers from xnu-8796.101.5. `scripts/prepare_xnu_build.py` (now taking the
kernel files from `kernel/src/`): 61 patches applied, 0 failed (native-pac-keys, arm64e-threaded-starts, native-arm-pan
skipped), a64x edits applied, debug.c ptrauth fix. Build as in kernel/unixnu-12377-arm64-fixes.md (17 min first time).
Result:
**`mach.development.bcm2837.unstripped`, Mach-O arm64 (subtype ALL), 21.5 MB, NOUNDEFS, with every round-3/4 symbol**
(correction, 11.3: built with `-march=armv8-a` at an iOS 27 target, it still had LSE/LDAPR words) (a64x_map_image,
a64x_find_islands, a64x_islands_page, a64x_rewrite1_kernel, a64x_stub_v, a64x_dead_vregs_flow,
kern.a64x.{images_native,images_marked,island_words}). Every a64x object recompiled with the kernel's own flags:
**0 errors, 0 warnings** outside the toolchain-wide -Wreserved-identifier noise (one missing-field warning found and
fixed). Only the post-link Private.symbolset export check fails, as before (Apple DART/UAT symbols). The final build was
made from exactly the sources in the diff (cmp-checked). `a64x_image_kind()` unit test (`tests/test_image_kind.c`
compiles the kernel header unchanged): hello_a57 and hello_a57_ios -> marked, hello_a57_unmarked and hello_a12 ->
arm64-scan, hello_a12e -> arm64e; all as expected.

### 10.2 corecrypto kernel-mode rules

`a64x_rewrite1_kernel()` (code-map flag A64X_MAP_KERNEL): MSR UAO/PAN/SSBS/DIT/TCO, #imm -> NOP; MRS Xt of those -> MOVZ
Xt, #0; MSR of those, Xt -> NOP; SB -> stub `dsb sy; isb` (both modes; checked natively). corecrypto had `mrs x8, DIT`
x10, `msr DIT, #0/#1` x11 and one SB: all handled. Kext scan now (`out/kernelcache-kext-check.txt`, with the 10.4 island
guard): 422,691 words change, 1,981 LSE + 8 FP16 + 1 SB stubs (14,333 words), 2,599 words left as data islands (two of
corecrypto's "LSE" words and Libm's are K256/coefficient tables, as in the cache). What remains, by kext: corecrypto
sha3/sha512 272 + sve 1 (behind its CPU checks); SoC drivers we replace: CLPCv3 46, VortexErrorHandler 20, watchdog 4,
PMGR 1 (Apple impdef registers); one SVE/SME word each in zlib, Compression, UDIFDiskImage, DispH11P, 3 in AVE2, 8 in
Libm (CPU-gated paths).

### 10.3 V-register liveness (FP stubs stop spilling)

`a64x/a64x_vlive.h`: per instruction, the V registers read and those written in full (loads/stores, scalar FP,
every Advanced SIMD group; partial writes and accumulators count as reads; unknown reads all it names; SVE/SME read
everything); calls kill V16-V31 and read V0-V15; same path walk as the X-register analysis.
**Checked natively** (`tests/test_vlive.c`, this Mac's own dyld cache): every claim tested by re-running the word with
the register changed: 737,541 data-processing words and 22,072 SIMD load/store words executed, 437,828 full-write
claims, **0 unsound** (two classes were wrong at first and fixed: SUQADD/USQADD accumulate; the permute decode).
Stubs take dead V registers first (any of V0-V31 except the instruction's operands) and save only the rest; the
FP stub tests rerun with random dead masks: FP16 20,360 words, FCMA 1,128, RDM 104, FJCVTZS/SB: 0 mismatches.
In the iPadOS cache plan: **94,786 FP sites; Q-register spills at 84,644 sites without V liveness, 16,985 with it
(-80%); mean stub 13.4 -> 10.6 words.** The kernel's page translator uses the same analysis within the page.
Stub cost per instruction, BL + RET form, on this Mac (`tests/bench_fp.c`; "spill" = no V liveness, "dead" = temps
dead; the Mac hides store/load latency, the A57 will not, so the word counts are the better A57 guide):

| instruction (cache rank by sites) | native ns | stub words: spill -> dead | stub ns: spill -> dead |
|---|---:|---|---|
| `fmla v2.8h, v1.8h, v0.h[0]` | 1.13 | 36 -> 26 | 6.24 -> 6.00 |
| `fcmp h0, h1` | 0.27 | 6 -> 4 | 2.23 -> 0.85 |
| `fadd v0.8h, v0.8h, v1.8h` | 0.68 | 13 -> 9 | 3.26 -> 3.40 |
| `fmla v1.8h, v0.8h, v2.8h` | 0.92 | 37 -> 29 | 6.26 -> 6.09 |
| `fmul v0.8h, v0.8h, v1.8h` | 0.94 | 13 -> 9 | 3.56 -> 3.52 |
| `fmax v0.8h, v1.8h, v0.8h` | 0.55 | 13 -> 9 | 2.98 -> 3.02 |
| `ucvtf h0, w0` | 0.27 | 3 -> 3 | 0.83 -> 0.86 |
| `fmul v0.8h, v1.8h, v0.h[0]` | 1.07 | 14 -> 10 | 4.27 -> 4.13 |
| `fmin v0.8h, v1.8h, v0.8h` | 0.55 | 13 -> 9 | 3.05 -> 3.03 |
| `fmov w0, h0` | 0.27 | 3 -> 3 | 0.85 -> 0.85 |
| `fcvtzu v0.8h, v0.8h` | 0.83 | 9 -> 7 | 3.31 -> 3.30 |
| `ucvtf h0, h0` | 0.80 | 4 -> 4 | 2.24 -> 2.23 |
| `fcvtzs w0, h0` | 0.27 | 5 -> 3 | 1.95 -> 0.85 |
| `fadd h0, h0, h1` | 0.70 | 7 -> 5 | 2.55 -> 2.48 |
| `fmov h0, #2.0` | 0.28 | 3 -> 3 | 0.83 -> 0.83 |
| `fmul h0, h0, h1` | 0.96 | 7 -> 5 | 2.81 -> 2.76 |
| `fcsel h0, h1, h0, eq` | 0.55 | 4 -> 4 | 1.64 -> 1.64 |
| `fadd v0.4h, v0.4h, v1.4h` | 0.70 | 7 -> 5 | 2.55 -> 2.55 |
| `fmadd h0, h0, h1, h2` | 0.96 | 10 -> 6 | 2.81 -> 2.80 |
| `fcadd v16.4s, v18.4s, v16.4s, #90` | 0.81 | 7 -> 5 | 2.40 -> 2.37 |
| `fcadd v12.2d, v13.2d, v12.2d, #90` | 0.78 | 7 -> 5 | 2.40 -> 2.43 |
| `fcmla v17.4s, v16.4s, v0.s[0], #0` | 1.06 | 6 -> 4 | 2.24 -> 1.04 |
| `fcmla v17.2d, v16.2d, v0.2d, #0` | 1.04 | 5 -> 3 | 2.24 -> 1.09 |
| `fjcvtzs w0, d0` | 0.26 | 38 -> 38 | 1.25 -> 1.29 |

### 10.4 Data in __text for images without a plan (kernel guard, implemented)

`a64x_islands_*()` (a64x_image.c), run by the kernel at load before the scan (`a64x_find_islands()`): runs of words
cut at UDF/zero padding, non-code words and LC_FUNCTION_STARTS starts; pieces end after unconditional terminators;
a piece with >= 1 unallocated word per 32 is data and makes the rest of its run data; LC_DATA_IN_CODE ranges are
data. Data words leave `img_is_code()`. Cost: one extra read pass over the text at load (the scan's pages are then in
the UBC), 1 bit per word. Validated against the full analysis (reachability + landing pads + jump tables) on both
caches: **iPadOS 26.5.2: all 11 would-be-rewritten data words caught, 4 code words in MFAAuthentication's obfuscated
code wrongly called data; macOS 27: 0 missed, 0 lost.** (A stricter variant lost no code but missed one data word in
each cache; silent corruption is the worse failure, so it was not taken.)

### 10.5 Plan, verification

Re-planned with V liveness: 897,714 sites, 100% placed, 0 traps, 0 veneers, 2.38 branches/site, plan 20.9 MB
(`out/dsc26.a64xplan`, sha256 `da87ada71c13fe129f49e3fe2a8dd6fc80c768080f934ca4b1e815b3e650c841`); every path of
every stub verified (0 broken); LLVM cortex-a57 over all 43,200,598 distinct translated words: same as round 3 (no
PAC/LSE/LDAPR/FP16/FCMA/FJCVTZS left). LSE regression: 74,256 + 12,288 checks, 0 failures.

### 10.6 ios-on-switch-lmao XNU diff 10063 -> 12377: plan only

`kernel/switch-12377-port-plan.md`: dry run of the 120-file / 331-hunk diff against the 12377 tree: 128 hunks fail in
57 files, 27 already present, 36 files clean, 31 new files; conflicts grouped by area with an order of work. Nothing
was changed.

### 10.7 Not done

- The kext prelinker step (append a stub section per kext, plain auth pointers): not started.
- Nothing ran on the Switch.

## 11. Round 5 (2026-11-04 night): ios-on-switch-lmao's XNU diff on 12377, linked

### 11.1 Port

Regenerated the diff from the XNU working repo (read only): `git diff d8b8029..ios-on-switch-lmao` at 3515591 (home grid
pager included) -> `port/xnu-switch-3515591.diff`, 120 files / 336 hunks. 3-way merge per file (base 10063, ours =
12377+UniXNU+a64x, theirs = ios-on-switch-lmao), conflicts resolved by region with a reason each; record, per-hunk table
and the one-line reasons for every place ours was kept: `kernel/switch-12377-port-plan.md`, `port/hunks.md`,
`port/conflict-decisions.json`. Hunks: applied 212 (+2 with a 12377 API change), kept ours 77 (26 as is, 23 hand-merged,
15 relocated into 12377's code, 10 ported by hand, 3 next to 12377's), already in 12377/UniXNU 24, took UniXNU/12377's
equivalent 12, dropped 9 (PGZ x3, I/O submap, RPi timer routing, uat_bootstrap guard, 3 DriverKit headers). Home grid
(3515591) chained onto the a64x cache pager: the apple_protect pager backs onto the a64x pager's object when the text is
translated, onto the file otherwise. ios-on-switch-lmao's own code (299 build steps: drivers, diagnostics, kits, 11
archives) is built against the 12377 headers by `port/ext_build.py` from the main CMake build's commands (read only),
with recipes regenerated from the 12377 build's compile records.

### 11.2 The kernel links (measured)

`build/xnu-port/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837(.unstripped)` (copied to `out/xnu-12377-port/`):
Mach-O arm64 subtype ALL, NOUNDEFS, 0 undefined symbols, Darwin 25.5.0, 26,703,160 bytes unstripped (sha256
5ead9776...71cac), 22,163,120 stripped (e336fc76...a4da). Same 15 link inputs as the main 10063 kernel. Symbols: of the
614 main-kernel symbols the ios-on-switch-lmao diff defines or touches, 602 are present; the 12 absent are PGZ (4, gone
in 12377), `vm_map_set_page_shift` (12377 passes the shift to `vm_map_create_with_page_shift`; same 4K rule),
`proc_ref_tracking_disabled` (TUNABLE replaced by a constant, same behaviour), 3 now-static VM counters and 3 inline
queue helpers. 115 a64x symbols. Full XNU rebuild: 38.6 s wall; ios-on-switch-lmao's archives: 7.6 s.

### 11.3 The A57 pin (bug found and fixed; also affects 10.1)

First link: **2,464 LSE + 403 LDAPR words in the kernel**, and `cswitch.s` reading UAO under `__ARM_ARCH_8_2__` (would
fault at the first context switch). Cause: 12377 builds at an iOS 26+ deployment target, where Apple clang's arm64
default is an A12 and `-march=armv8-a` does not undo it (measured; see the contract note in 9.4). The 10063 tree builds
at 17.0 and never needed it. Round 4's 12377 kernel (10.1) had the same flags and the same problem. Fix:
`MACHINE_FLAGS_ARM64_BCM2837 = -DARM64_BOARD_CONFIG_BCM2837 -mcpu=cortex-a57` (in MACHINE_FLAGS so .s files get it;
SFLAGS has no CFLAGS_EXTRA), and the `iokit_cxx_generic` recipe keeps `-mcpu` (main's Drivers.cmake drops it: harmless
at 17.0, wrong at 26+). After: the classifier finds 40 non-v8.0 words in the port kernel,
**the same 40 at the same symbols as the main 10063 kernel** (K256 and lz4 tables, data after exception vectors, guarded
Apple-impdef sysregs in awl_* and `_switch_secondary_entry`): `port/verify/flagged-{port,main}.txt`.
ios-on-switch-lmao's 302 objects: 0. When the main tree or the Bazel build moves to 12377, both changes have to go with
it.

### 11.4 Not verified

- Nothing booted; nothing will until the iOS 26 install path exists.
- `original-bindings.S` still binds iOS 17 kernelcache addresses (regenerate against the 26 kernelcache with the audit
  scripts); libpthread_kernel.a, the corecrypto-2023 archive and libfirehose_kernel are 10063-era prebuilts of the
  main tree, linked as they are.
- Semantic drift in the 212 clean merges (VM and exec most of all); the UniXNU helpers dropped with the kern_exec cut
  (cryptex OS view, gestalt/LS store drops, staged apps) may turn out to be needed by iOS 26 userland.
- TegraGM20BUserClient.o compiled from the main tree's sources at 18:02 (they were mid-edit earlier in the round).

### 11.5 Kext prelinker step: not started (shape)

Per fileset entry of the 26 kernelcache (220 kexts, 9.5): translate in kernel mode (`A64X_MAP_KERNEL`: PAC identity,
LSE stubs, corecrypto rules from 10.2), append the entry's stubs as a section after its `__TEXT_EXEC` (14,431 stub
words = 57 KB for all), rewrite every arm64e kernel chained-fixup pointer with auth=1 as a plain rebase/bind (key and
diversity dropped, target unchanged), set the entries' cpusubtype to ALL, and replace Apple's kernel entry with ours.
Inputs and numbers exist (`tools/a64check` fileset mode, `out/kernelcache-kext-check.txt`); the writer does not.

### 11.6 Files and disk after round 5

Kept: `src/xnu-port` (the ported tree, 96 MB), `out/xnu-12377-port/` (the two kernels, 47 MB), `port/` (diffs, merge
inputs, scripts, decisions, `hunks.md`, `verify/`). Removed: the pristine 12377 tree (re-fetch the
apple-oss-distributions tarball), `build/xnu-port` and `build/ext` (rebuild: `KEEPGOING=1 port/build-switch.sh`
(compiles; the link needs the archives), then `port/ext_build.py -j 10`, then
`LD_ARCHIVES="$(cat port/ld-archives.txt)" port/build-switch.sh` after deleting the linked kernel). Scratch dir: 259
MB; disk: 32 GB free.

## 12. Round 6 (2026-11-04 night): the 26.5.2 kexts prelinked into our 12377 kernel

### 12.1 What the 26.5.2 kernelcache is

`kernelcache.release.ipad11b` (23F84, J171aAP): arm64e MH_FILESET, 222 entries, 239 prelink-info entries,
**fully stripped** (kernel and kexts: 0 symbols), 456,852 chained fixups (DYLD_CHAINED_PTR_64_KERNEL_CACHE), and the
kexts have **no stubs**: the collection builder made every call into the kernel a direct BL. So relinking against our
kernel needs a name for every Apple kernel address the kexts use. The 17 roots of the main tree's prelink give a
closure of **48 kexts** (with the main tree's 4 dependency cuts; exactly 1 + 1 cut sites, as in 21H461).
`com.apple.driver.usb.AppleUSBXHCI` is not in this kernelcache (Lightning iPad).

### 12.2 Naming Apple's kernel (prelink26/names.py) - measured

2,046 distinct kernel addresses are used by the 48 kexts (79,829 pointer sites, 62,366 branch sites). Sources:

- **vtables, PAC-verified**: Apple class C slot i <-> our `__ZTV<C>`; each match requires the arm64e PAC
  discriminator of the slot = `ptrauth_string_discriminator` (clang's, computed by clang itself) of the mangled name
  of the method that introduced the slot in our kernel. 9,678 slots matched (45 at a different index: e.g. OSKext has
  2 more virtuals in ours), 61 not; a target shared by different methods (__cxa_pure_virtual) is never named this way.
- **GOT order**: kext `__auth_got`/`__got` entries are sorted by imported name (checked on 21H461: 15,353 of 15,619
  adjacent pairs ascending, the rest aliases). Gaps between named entries take the names the same kext imported in
  21H461 (or any 21H461 kext, or the 26.5.2 KPI symbol sets) when the count fits and every other GOT holding the same
  address agrees with the order.
- **symbolicator 25.5 signatures** (blacktop/symbolicator, `ipsw kernel symbolicate`): metaclasses, vtables, C names.
- call graph (Apple vs ours, same callees in the same order): used only for non-exported functions; it was right for
  5 of 8 held-out exported names, so it never names a KPI symbol.
Hold-out (half of the symbolicator's C names withheld): GOT-derived names agree **42 of 43**; vtable names disagreed
with the symbolicator 3 times, all 3 PAC-verified (the symbolicator is the wrong one there).
Result: **1,599 of 2,046 targets named and present in our kernel (78%); 130,704 of 142,195 sites (91.9%)**.

### 12.3 The prelinked kernel (prelink26/emit26.py, tools/a64kext.c) - measured

`prelink26/out/kernel.macho`: 69,009,408 bytes, sha256 c59fe5aa...de0384, our 12377 port kernel + 48 kexts in the
main tree's format (sparse prelink index ABI 1, coalesced __PRELINK_TEXT/__PLK_DATA_CONST/__PLK_TEXT_EXEC/
__PRELINK_DATA, XML prelink info with `_PrelinkLinkKASLROffsets`; kext segments slid by -0x3888000, below our kernel).

- code: 2,158,153 kext instruction words translated in kernel mode, 139,970 changed (PAC: 59,211 pac/aut, 47,196
  blraa, 20,838 retab, 6,459 xpac, 4,497 braa -> identity; LDAPR 34; MSR/MRS DIT/SSBS/PAN 21), 1,707 LSE + 6 FP16 + 1 SB
  stubs = 11,938 stub words, each kext's in its own block after the kexts' text;
- pointers: all 125,057 fixups written plain (45,226 kext->kext, 78,716 kext->our kernel by name, 2 cuts,
  898 + 215 unresolved -> trampoline / `_switch_prelink_cut_null`), all in the KASLR list;
- branches: 51,988 kext->kernel BL/B re-aimed at our kernel, 10,378 unresolved -> a per-address trampoline
  (`adr x0, msg; b _panic`: "kext called Apple kernel function 0x..., which has no name here"), 33 in data islands left;
- `prelink26/verify26.py` from the output bytes: every one of the 125,057 KASLR pointers lands in a mapped segment;
  175,947 of 175,981 kext branch encodings land in executable memory (the 34 others are data words in Libm 30,
  zlib 2 and corecrypto 2 constant tables); stubs: 0 non-v8.0
  words. Left untranslated in kext text (classifier): corecrypto SHA3/SHA512 272 + SVE 24 (behind corecrypto's CPU
  checks), Libm/zlib data tables, AppleARMWatchdogTimer's 4 impdef registers (Apple SoC; no DT node here).

### 12.4 Bindings for 26.5.2 (prelink26/bindings26.py) - measured

510 symbols in the main tree's 10 bindings files (375 are `opaque<slot>` vtable slots). Found from 26.5.2's bytes:
**211**: 14 metaclass/vtable addresses, 193 vtable slots (21H461 slot -> 26.5.2 slot through PAC-named anchors;
equal-length runs between anchors pair in order), 4 by signature. Not resolved: 79 AppleUSBXHCI (kext absent) + 70
AppleUSBHostController slots (only SwitchXHCI uses them), ~112 slots in vtable runs that changed size
(IOMobileFramebufferLegacy 358 -> 369 slots, IOUSBDeviceController 234 -> 242: the drivers' class declarations must be
regenerated from the 26 vtables), 38 non-virtual members with no 25.5 signature. Files: `prelink26/bindings/*.S`.

### 12.5 Not verified

Nothing ran. Whether our 12377 OSKext accepts these kexts (arm64 subtype ALL headers rewritten from arm64e, fileset
kpi entries emitted without executables, no `_PrelinkExecutable*` for them) is untested; the AMFI early security
initializer has no 23F84 address yet; the loader must accept kexts placed from 0xfffffff0038c4000 (10 MB lower than
the main tree's); the 22% unnamed kernel targets panic by name when called. Gap list: `kernel/ios26-gap-list.md`;
toolchain notes: `kernel/toolchain-notes-12377.md`.

### 12.6 Rerun

`ipsw kernel cpp --json --methods dl/ipsw-26.5.2/kernelcache.decompressed > prelink26/cpp26.json` (100 MB, deleted),
`ipsw kernel cpp --inheritance ... > prelink26/inherit26.txt`, `ipsw kernel symbolicate --signatures dl/symbolicator
--json -o prelink26/sym2 ...`, then `prelink26/kcinfo.py` / `classify.py` / `pcrel.py` (closure, fixups, branches),
`prelink26/names.py`, `prelink26/emit26.py <our kernel> <kc> <out>`,
`prelink26/verify26.py <out> <kc>`, `prelink26/bindings26.py`. `clang -O2 -o build/a64kext tools/a64kext.c
a64x/a64x_classify.c a64x/a64x_rewrite.c a64x/a64x_image.c`.
