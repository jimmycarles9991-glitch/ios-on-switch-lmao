# IOSurfaceAccelerator on ios-on-switch-lmao: the contract TegraVIC implements

iPadOS 17.7.11 (21H461). What `IOSurfaceAccelerator.framework` asks of the kernel, who calls it and how, and how
TegraVIC answers it on the Tegra X1's VIC. **[V]** = read in the 21H461 binaries (dyld cache, kernelcache) or the
r7p9d device log; **[I]** = inferred; **[T]** = measured on the unit by TegraVIC's boot self-test, the same on all 19
boots of 11-05 23:53 to 11-06 23:42; the self-test is gone and its results are `kTegraVICTraits` (TegraVICConfig.h).

Sources: dyld cache `out/agent-scratch/r9-hang/dsc` (IOSurfaceAccelerator 147.0.32, QuartzCore 1120.34.13,
VideoToolbox, MediaToolbox); kernelcache `vendor/unixnu/out/ipsw/21H461/kernelcache.decompressed`
(`com.apple.driver.AppleM2ScalerCSCDriver` 265.0.0 at 0xfffffff005440100, `com.apple.iokit.IOSurface`). Working
disassembly is in `out/agent-scratch/r13-scaler/re/` (not committed).

## 1. The crash

`backboardd` aborts in `CA::IOSAManager::create_new_iosa_manager` → `x_log_crash("Failed to create
IOSurfaceAccelerator: 0x%x")` → `abort()` **[V]**, reached from `IOMFBDisplay::clone_update` → `copy_surface` →
`CA::WindowServer::IOSurface::copy_iosurface` → `Display::copy_iosa_manager` when Screen Recording's virtual display
("Wireless", display 2) clones the LCD. 0xe00002c0 is `IOServiceGetMatchingService` finding no `AppleM2ScalerCSCDriver`.
Core Animation has no fallback for a failed create **[V]**; a failed *transform* is survivable (it logs, force-completes
any shared events and, when flag 0x400 is set, copies with Metal) **[V]**.

## 2. Opening it [V]

`IOSurfaceAcceleratorCreate(alloc, options, &accel)`:

- `IOServiceGetMatchingService(IOServiceMatching("AppleM2ScalerCSCDriver"))` ("AppleSRSDriver" only if the options
  have "Sharpener"); none → 0xe00002c0.
- `IOServiceOpen(service, mach_task_self(), type 0, &connect)`; Apple's `newUserClient` ignores the type and checks no
  entitlement; the client class is **`IOSurfaceAcceleratorClient`** (IOUserClient, legacy method table).
- Selector 10 with `{s32 PriorityBand; pad; s64 DutyCycleDuration; s64 HistogramDuration}` (0x18 bytes); its failure is
  only printed. Core Animation passes PriorityBand 0, DutyCycleDuration 200000.
- `IOSurfaceAcceleratorGetRunLoopSource`: `IONotificationPortCreate`, `IOConnectSetNotificationPort(connect, 0, port,
  0)`; Apple's `registerNotificationPort` stores the port whatever the type.

## 3. Selectors [V]

| sel | call | in / out | Apple's driver | TegraVIC |
|---|---|---|---|---|
| 0 | CaptureSurface | struct 0x40 / - | 0xe00002c7 | kIOReturnUnsupported |
| 1 | Transform/Transfer/Blit | struct 0x1b0 / - | §4 | §4-§6 |
| 2, 3 | AbortTransfers, AbortCaptures | scalar 0/0 | 0xe00002c7 | kIOReturnUnsupported |
| 4 | SetCustomFilter | struct 0x20 / - | checks taps 9/15, phases 32 | accepted (VIC filters itself) |
| 5 | GetID | scalar 0 / 1 | accelerator ID from IOSurfaceRoot | 0x5d |
| 6 | KernelTests | struct 0xfa8 | 0xe00002e2 on release kernels | kIOReturnNotPermitted |
| 7 | GetHistogram | struct 8 (user pointer) | 64 bins | kIOReturnUnsupported |
| 8 | GetDiag | struct 8 | - | kIOReturnUnsupported |
| 9 | GetTransformEstimation | struct 0x10 (two pointers) | - | kIOReturnUnsupported |
| 10 | client properties | struct 0x18 | stored | stored |

## 4. The transform request (selector 1, 0x1b0 bytes) [V]

Built by `_prepareTransformBuffersAndOptions`; layout in `TegraVICRequest.h` (`TegraVICRequest`, static-asserted):

| offset | field |
|---|---|
| 0x000 / 0x004 | source / destination IOSurface ID |
| 0x008, 0x010, 0x018 | completion: callback, refcon, info (the caller's values, unwrapped) |
| 0x020 | flags (below) |
| 0x028..0x047 | source rect x, y, w, h: u64 16.16 (TransferSurface: `(x << 16) \| (xfrac << 14)`); w or h 0 → whole source |
| 0x060..0x06f | destination rect x, y, w, h: u32 pixels; w or h 0 → whole destination |
| 0x078..0x0a7 | shared events: src port name, wait, signal; dst port name, wait, signal |
| 0x0a8 | AlphaOverride value |
| 0x0ac..0x0c8 | BorderFill rect, alpha, R/Y G/Cb B/Cr |
| 0x0cc..0x0d3 | WriteOnlyMode A, R/Y, G/Cb, B/Cr (u16) |
| 0x0dc..0x10f | histogram mode, rect, 8 programmable bins |
| 0x110..0x1af | 4 × 0x28 IOSurfaceAcceleratorCommApi records (user pointers) |

Flags: bits 0-2 Transform (bit 0 flip X, bit 1 flip Y, bit 2 quarter turn; Core Animation's table: 90° = 4, 180° = 3,
270° = 7), 6 CustomFilter, 7 NearestFilter, 8 OptOutBlitClip, 9 LockInScaler, 10 SkipAsyncCallback, 12 FixUpscaling
(always set by TransformSurface), 13-14 DitherControl (default 1), 15-17 destination chroma siting (1 Left, 2 Center, 3
TopLeft, 4 Top, 5 BottomLeft, 6 Bottom), 18 AlphaOverride, 28 BorderFill, 29 Histogram, 30 ZeroFill, 31
DisablePseudoLinearScaling, 32 PixelAveraging, 33 EdgeMirroring, 35 UnwireSurface, 36-38 ColorRemapMode, 39
DirectionalScaling, 40 HDR, 41 Enhance, 42 NoiseEstimation, 43 ReadOnly, 44 HtpcLargeHeaders, 45 VraScaling, 46
NoPowerEarlyBail, 48 WriteOnlyMode, 49 FilmGrain, 50 ReducedPrecisionFractionalOffsets, 51 NoCoalesceAsyncIOFenceWrites.
Bad option values never reach the kernel (the framework returns 0xe00002c2) **[V]**.

**Surfaces [V]:** Apple's driver resolves both IDs with `IOSurfaceRoot::lookupSurface(id, current_task())` (the
caller's task, not the opener's): NULL → 0xe00002c2. The result is retained (try-retain) and released at completion.
A protected source into a less protected destination → 0xe00002c7.

**Completion [V]:** callback 0 → synchronous (the call sleeps until done, returns the status). Callback set → the call
returns at once; at completion `sendAsyncResult64(ref, status, {info}, 1)` with
`setAsyncReference64(port, callback, refcon)`, unless SkipAsyncCallback. User space: `IODispatchCalloutFromCFMessage`
calls `callback(refcon, status, info)`; Core Animation's `msr_transfer_complete(IOSAManager*, status, stats*)` reads
`info` (a zero-argument reply would hand it garbage) and only releases the manager / updates statistics.

## 5. Who calls it

**Core Animation, Screen Recording [V, I where marked]:** replayd's FigVirtualDisplay runs in writeback mode
("Uncompressed", codec none, '420f'); backboardd clones the LCD into replayd's writeback buffer
(`IOMobileFramebufferGetWirelessSurfaceWithOptions`), a 720x1280 '420f' surface, with one async TransformSurface per
frame: source the 720x1280 BGRA LCD surface, Transform 0 (the virtual display "supports rotation", so CA passes the
angle to the framebuffer instead), NearestFilter false, ColorRemapMode 0, rects NULL. [I] When the Wireless display
wants full frames and the rect does not cover it, up to four BlitSurface border clears per frame first, with
WriteOnlyMode (255, 0, 0, 0) whatever the destination format (TegraVIC treats it as RGB black, converted).
Shared-event keys are added only when the CA surface owns a SharedEvent (§7).
Normal display path: no use of the service (backboardd has never created the manager in minutes of normal use); the
capability dictionary feeds `ScalerLimits` (CA uses built-in defaults without the service), detached-layer MSR copies
and HDR/backward-DM paths (gated by `IOSurfaceAcceleratorCapabilitiesHDR`, published 0).

**AirPlay mirroring:** the same clone path, landscape (Transform 4/3/7) when the receiver does not rotate.

**VideoToolbox [V]:** `FigRegisterIOSurfacePixelTransferCapabilityOnce` registers ~19 baseline pairs as soon as Create
succeeds (420v/420f/BGRA among themselves, 422/444 ↔ BGRA, L565, l10r→BGRA, RGhA→BGRA) and gated ones by capability
keys (published 0). A failed hardware transfer makes `VTPixelTransferSessionTransferImage` fall back to its
blitter/vImage/Metal path for that call (one wasted kernel round trip, refused before any work).
**Others [V]:** CMPhoto and ImageIO/AppleJPEG (thumbnails, HEIF/JPEG scaling), WebCore (`IOSurface::convertToFormat`,
null-checks the accelerator), VectorKit (Maps), MediaToolbox's FigVideoRenderPipeline (BlitSurface), CMCapture and
camera frameworks (no camera here). ReplayKit, AVFoundation, CoreImage, CoreVideo do not link it.

## 6. The capability dictionary

`IOSurfaceAcceleratorCapabilitiesDict` on the service (OSNumbers). Apple's (A10X, scaler hardware-version 6) [V]:
min 4, max 16384 (dest height 16382), up 4x, down 8x H / 4x V, filter taps 9 V / 15 H, 32 phases, coefficient bits
4.12, rotation, identity CSC, histogram, colour manager, accessibility, many formats. TegraVIC publishes what VIC does
[T]: min 4, max 8192, 16x up and down, Apple's filter geometry (so CA's tables keep their shape; VIC ignores custom
coefficients), rotation iff all 8 transforms proved, IdentityCSC 1, FractionalXYOffsetsSupported 1 (16.16 source
offsets reach SourceRectLeft/Top; probe gpu.x1.vic_fractional decides it), formats ARGB8888, RGBA8888, 2Planes420,
2Planes422, 2Planes444, LumaOnlyL008 and ARGB2101010 in and out, Interleaved422 once its trait is proven, and 0 for
HDR, film grain, enhancement, directional scaling, colour manager, accessibility, histogram, tiling (Apple's GPU
tiling), fractional dimensions, 10-bit YCbCr and compressed formats.
Core Animation reads none of the format keys. On iPadOS 27 its detached-layer copy (`IOMFBDisplay::detach_layer`,
QuartzCore 24B119) asks for '422v'/'422f' from a '420v'/'420f' layer regardless [V].

## 7. The service's identity [V]

`com.apple.driver.AppleM2ScalerCSCDriver` is in this kernel's prelinked set: `IOMobileGraphicsFamily` links against
it (four function pointers into its `IOAsynchronousScheduler`), so it loads with the display and its classes own the
names `AppleM2ScalerCSCDriver` and `IOSurfaceAcceleratorClient`. It never matches (no `scaler,s5l8960x` node in our
device tree), so no instance exists. A class of ours with either name would make that kext, and the display family
with it, fail to load; and matching (`IOServiceMatching` walks instances of the named class) and the sandbox go by
those names. So TegraVIC instantiates Apple's classes (`OSMetaClass::allocClassWithName`) and points each object at a
copy of its class's vtable with these slots replaced (slots found by the IOService/IOUserClient function each holds in
the kernel's own `__ZTV9IOService`/`__ZTV12IOUserClient`; that Apple's overrides sit there is kits/SwitchABI's
AppleM2ScalerCSCDriver and IOSurfaceAcceleratorClient tables, checked against each kind's kernelcache by `:verify`):

| class | slot (method) | Apple's override | in the copy |
|---|---|---|---|
| AppleM2ScalerCSCDriver | 140 newUserClient | 0x...61283e4 | TegraVIC's (an Apple `IOSurfaceAcceleratorClient`, re-pointed) |
| | 85/86/87 probe/start/stop, 155 setPowerState | hardware bring-up | IOService's |
| | 44 setProperties | | kIOReturnUnsupported |
| IOSurfaceAcceleratorClient | 168 externalMethod (not overridden by Apple) | - | TegraVIC's selectors (§3) |
| | 175 registerNotificationPort, 172 clientClose, 14 free | | TegraVIC's (per-connection state beside the object) |
| | 178 clientMemoryForType, 182 getTargetAndMethodForIndex | | IOUserClient's |

Apple's code that still runs: the classes' constructors, `getMetaClass`, the service's `init(OSDictionary*)`
(IOService's plus clearing one byte).

## 8. What TegraVIC does with a request

1. Look up both surfaces in the caller's task (IOSurface kext, by address: kits/SwitchABI's IOSurface tables, each
   function pinned by its words). `IOCoreSurfaceRoot` is the root.
2. Translate (`tegra_vic_plan`, `TegraVICRequest.h`): empty rects → whole surfaces; ZeroFill and BorderFill become fills
   before the transform; WriteOnlyMode becomes a fill of the destination rect (4:2:0 fills widened to even coordinates);
   Transform, NearestFilter and the destination's chroma siting are used; quality hints (dither, enhance, noise
   estimation, directional scaling, pixel averaging, edge mirroring, pseudo-linear, fix-upscaling, fractional precision,
   blit clip, lock-in, read-only, unwire, power, coalescing, Comm records) are accepted and ignored. Refused as
   **kIOReturnUnsupported** (callers fall back): histogram, HDR, film grain, colour remap, shared events, alpha override
   into a surface with alpha, formats other than BGRA/RGBA/420v/420f/422v/422f, protected-to-less-protected, and any
   shape the checker refuses (pitch the unit cannot address, odd 4:2:0 rects or 4:2:2 columns, >16x scaling, sizes
   outside 2..8192, a destination sub-rectangle under a transform other than 0 and 6, whose placement is not in place on
   the unit). Refused as **kIOReturnBadArgument**: unknown surface IDs, Transform bits 3-5.
3. Build and check every VIC job in the calling thread (`tegra_vic_build`, `TegraVICConfig.h`), so refusals are the
   call's own result; then queue (16 deep, FIFO) for the engine thread. Sync calls sleep until their jobs are done.
4. Engine thread: `IOSurface::prepare`, map each surface page by page into its SMMU window (source read-only), clean
   the source's cache lines and clean+invalidate the destination's, run each job (host1x OP_DONE syncpoint, by
   interrupt), invalidate the destination again, unmap, `IOSurface::complete`, release, reply.

Colour: 4:2:0 surfaces of 1280 wide or 720 high and up are BT.709, smaller BT.601 (CoreVideo's default for untagged
buffers) [I: attachments are not read]; '420v' video range, '420f' full range. VIC's pipeline is 10-bit: matrices
are generated in its encoding (`tegra_vic_matrix_*`) and checked on the host against the float conversions (≤ 1 step).

## 9. Gaps and risks

- 4:2:0 -> RGB: the unit widens an 8-bit sample to the 10-bit pipeline as x << 2 | x >> 6 [T]; the matrices assume
  x << 2, so R and B sit up to about one step above BT.709 (chroma zero read as 514, not 512). Building
  `tegra_vic_matrix_ycc_to_rgb` for the measured law is open.
- **Shared events** are refused. If Core Animation sends them on the clone path, each frame's transform fails with
  kIOReturnUnsupported (no crash; CA force-completes the events). The device log says so once per process
  (kits/SwitchRefusal, category TegraVIC: "shared events: IOSurfaceAccelerator transform refused (not on VIC), the
  caller falls back"). Supporting them needs the IOSurface kext's IOSurfaceSharedEvent lookup/listener/signal
  (0x5b974d8, 0x5b97400, 0x5b97718, 0x5b96c88 [V], semantics [I]).
- Colour standard by size, not by the surfaces' YCbCrMatrix attachment.
- Packed 4:2:2 (until `packedYcc` is proven), RGhA, 10-bit YCbCr: refused (VideoToolbox falls back per call). L008,
  L565, l10r, NV24 and three-plane 4:2:0 are table rows that have not run on the unit.
- 4:2:2 biplanar (`Y8___U8V8_N422`, NVB0B6 code 63) has not run on the unit. Like every transposed output, a
  transposed 4:2:2 one is described before the transpose, where chroma halved across memory rows is halved down:
  `Y8___U8V8_N422R` (65), with the siting exchanged (`TegraVICFormats.h`). This rests on the law measured for 4:2:0,
  which cannot tell the two apart. If it is wrong, a turned 4:2:2 picture shows correct luma and misplaced colour.
- GetHistogram, GetTransformEstimation, CaptureSurface: unsupported.
- Rotation direction: TegraVIC takes bit 2 as a clockwise quarter turn applied after the flips (Android's HAL order)
  [I]; which VIC flip/transpose bits implement each IOSA transform was measured on the unit (`vicFlips` [T]), so only
  this convention, not VIC's, is assumed. Screen Recording's clone does not rotate (Transform 0).
- Transposed jobs (`OutputTranspose`): VIC composes the target rectangle before the transpose and writes composed
  pixel (x, y) to memory row x, column y, so the whole OutputSurfaceConfig is given in pre-transpose geometry: surface
  size swapped, `OutLumaWidth` = rows - 1 and `OutLumaHeight` = pitch / bpe - 1 (chroma likewise) [V: hekate's Nyx
  rotation programs exactly that on this SoC; the unit's diag lines with the luma geometry left post-transpose show
  a row stride of `OutLumaHeight` + 1 pixels]. Of the four conventions (rectangles × surface, before or after the
  transpose) the unit copied the card with rectangles and surface before [T]; transposed 4:2:0 output (a BGRA patch
  card into 420f, transforms 4 and 7) matched too [T].
- Destination subrectangles: VIC mirrors around the complete output surface. The measured unadjusted placement mask
  is `subRects` 0x41 [T]. For each flipped axis, the builder now places the pre-transpose target at
  `surface extent - requested origin - rectangle extent`. The same correction applies to the slot destination.
  All eight transforms pass asymmetric cropped RGB/NV12 model checks with padded rows and scaling; the model
  reproduces the unit's original placement diagnostics. New physical subrectangle/filter validation remains.

## 10. NVDEC's pictures (not IOSurfaceAccelerator)

TegraNVDEC's Present method (ABI v7) hands each decoded picture to `tegra_vic_present` (`TegraVICPresent.h`): NVDEC's
NV12 in Tegra block linear (16Bx2 sectors, GOB height 2, the layout `SwitchVideoDecoders/AVD/BlockLinear.h` reads) in
one of its buffers, with that buffer's page list, copied 1:1 by the 4:2:0 pass-through into the decoder's pool IOSurface
(pitch linear, '420v' or '420f', rows and planes on 256 bytes), looked up in the calling task. The picture's pages go
into the source window read-only; it needs no cache maintenance (NVDEC wrote DRAM, no CPU writes its buffers); the
destination is cleaned and invalidated before and invalidated after, as for every job, so a GPU that does not snoop the
CPU's caches reads what VIC wrote. Synchronous on the decoder's thread under the engine lock, with page lists of its own
(32 MiB a side). Block-linear input (`blockLinearIn`) comes with the 4:2:0 pass-through: a 128x64 picture written in
block linear by the CPU copied exactly, to a step, on all 21 boots of 11-05 and 11-06 that checked it, so the check is
gone. Without the pass-through the service still publishes, `tegra_vic_present_ready` is false, TegraNVDECInfo has no
present feature and the decoders copy on the CPU. Refusals (odd sizes, unaddressable surfaces) are
one line per kind per boot and leave the picture invalidated for the decoder's CPU copy. Stats: "TegraVIC: Ns
present=..." per 5 s window with pictures.
