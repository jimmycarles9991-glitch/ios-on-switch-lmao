# Hardware JPEG on ios-on-switch-lmao: the contract TegraNVJPG implements

iPadOS 17.7.11 (21H461). What CMPhoto, ImageIO, UIKit and VideoToolbox's JPEG plug-ins ask of Apple's hardware JPEG
codec driver, and how TegraNVJPG answers on the Tegra X1's NVJPG. **[V]** = read in the 21H461 binaries (dyld cache,
kernelcache), with the address; **[I]** = inferred; **[T]** = proven on the unit by the boot self-test TegraNVJPG ran on
every boot of 11-05 and 11-06 (23 boots, all `J3 ok` with formats 0x3f, sampling 0x17, scales 0x3, whole-MCU writes),
which these facts made unnecessary; it is gone, and those results are TegraNVJPG.cpp's `kTraits`.

Sources: dyld cache `out/agent-scratch/r9-hang/dsc` (CMPhoto, JPEGH1.videodecoder, JPEGH1.videoencoder, ImageIO,
UIKitCore, libMobileGestalt); kernelcache `vendor/unixnu/out/ipsw/21H461/kernelcache.decompressed`
(`com.apple.driver.AppleJPEGDriver` 6.6.2 at 0xfffffff0053c2040; kext addresses below are its `__TEXT_EXEC`
0xfffffff005f40000+, shortened to the last 5 hex digits). Working disassembly: `out/agent-scratch/r15-nvjpg/re/`
(`user/`, `kext/`; not committed).

## 1. The service and its client

- **Names [V].** CMPhoto `__initializeService` 0x1aaee363c and JPEGH1.videodecoder 0x201121554:
  `IOServiceGetMatchingService(IOServiceMatching("AppleJPEGDriver"))` (by class). libMobileGestalt sub_1a0227cc4
  (question `m+FD6mX8VZzP95hOhM+jow`, plain name unknown): the same match, "service exists". JPEGH1.videoencoder
  `H1JPEGEncoder_CheckExistence` 0x215db1b70: `IOServiceNameMatching("AppleJPEGDriver")` (by registry name) before it
  registers VideoToolbox's hardware 'jpeg'/'sjpg' encoders. The client class is `AppleJPEGDriverUserClient` (Apple's
  IOUserClient2022 subclass, 0x110 bytes, vtable 0xfffffff006df5d88); the sandbox's iokit-open rules go by that name
  [I: the only other reference is a CoreFoundation constant string].
- **Apple's kext is not in this kernel [V].** Its personality matches `IONameMatch jpeg,s5l8920x` (j120's jpeg0/jpeg1
  nodes, absent from ours); none of the 237 prelinked kexts lists it in OSBundleLibraries; it is not among the
  prelinker's components (`bazel-bin/.../build/prelink/dma-kernel.json`). So, unlike VIC's AppleM2ScalerCSCDriver
  (TegraVIC/CONTRACT.md §7), the names are free: TegraNVJPG defines `AppleJPEGDriver` and `AppleJPEGDriverUserClient`
  natively. If the kext were ever added to the prelink roots, its classes would collide with these.
- **Opening [V].** Every caller opens type 0 (CMPhoto 0x1aaeed20c, JPEGH1 0x201121684, JPEGH1enc 0x215db1cfc). Apple's
  `newUserClient` 0x43f08 ignores type, securityID and properties, checks no entitlement, refuses past 1000 open
  connections (0xe00002be). CMPhoto probes once per process (`__isHWAvailableHelper` 0x1aaeec660, dispatch_once): an
  open that fails turns hardware off for the process's life.
- **Notification port [V].** `IOConnectSetNotificationPort(conn, 0, port, 0)` (CMPhoto 0x1aaeecba0, JPEGH1 StartSession
  0x20111fbe0); Apple's `registerNotificationPort` 0x531f4 keeps it whatever the type and refuses a second, different
  port (0xe00002bc).

TegraNVJPG: both classes native, the service on IOResources, **registry name `TegraNVJPG`** so JPEGH1.videoencoder finds
nothing and registers no hardware JPEG encoder (NVJPG's encoder is not driven; see §6). Class matching, MobileGestalt
and the sandbox are unaffected. Opening, the 1000 limit and the port follow Apple's.

## 2. Properties [V]

Apple's `start` → 0x433b8 sets, as 32-bit OSNumbers, from its hardware version register. Readers: CMPhoto through
IORegistryEntryCreateCFProperty (bool or nonzero int, 0x1aaee37e8); JPEGH1 reads AppleJPEGNumCores with
IORegistryEntryGetProperty and size 4 (0x20112158c), so it must be 32-bit.

| key | Apple (A10X) | TegraNVJPG | effect in userland |
|---|---|---|---|
| AppleJPEGNumCores | 2 [I: version register not readable statically] | 1 | ≥ 2 with RST logging enables CMPhoto's two-core strip decode (header rewrites, async); 1 keeps the one-shot path |
| AppleJPEGSupportsAppleInterchangeFormats | 1 | 1 if BGRA and RGBA proven | CMPhoto's automatic mode (what ImageIO uses) takes the hardware only with it (0x1aaee0d6c); adds BGRA/RGBA/422f/444f outputs |
| AppleJPEGSupportsMissingEOI | 1 | 1 | userland may send a bitstream with no FFD9 and sets request flag bit 2 |
| AppleJPEGSupportsRSTLogging | 1 | 0 | RST encode tables and the strip decode |
| AppleJPEGSupportsDCTScaling | 1 | 1 if 1/2, 1/4, 1/8 proven | (not read by the callers found) |
| AppleJPEGRequiresMCUAlignment | 0 | 0 | JPEGH1.videoencoder only (default 1 when absent) |
| AppleJPEGSupports12BitsFormat, ...CompressedInterchangeFormats | 0 | 0 | |

## 3. Selectors [V]

Apple's IOExternalMethodDispatch2022 table at 0xfffffff006df63f8: no scalars, exact struct sizes, struct out = struct in
size, no async IOKit calls (completions travel on the notification port, §5).

| sel | in/out | Apple | TegraNVJPG |
|---|---|---|---|
| 0, 2 | 0 | returns 0 | 0 |
| 1 | 0x58 | startDecoder 0x45efc | decode (§4) |
| 3 | 0x58 | startEncoder | kIOReturnUnsupported |
| 4 | 0x1000 | startEncoderExt | kIOReturnUnsupported |
| 5 | 0x1000 | startDecoderExt 0x46890 | decode (§4); a crop rectangle is refused |
| 6 | 0xda0 | 2024 encode | kIOReturnUnsupported |
| 7 | 0xda0 | 2024 decode | decode (§4): bytes 0..0x57 as selector 1, crop as selector 5, sync or async by +0x30 |
| 8, 9 | 0 | power on/off, entitlement com.apple.applejpegdriver.poweron else 0xe00002bc | the same check; a no-op |

Callers seen [V]: decode sel 1 (CMPhoto 0x1aaf45c78, JPEGH1 0x2011216e0), decode sel 5 (CMPhoto 0x1aaeed13c), encode
sel 3 (CMPhoto 0x1aaf45f30, JPEGH1enc 0x215db2f00), encode sel 4 (CMPhoto 0x1aaeed260).

**24B119 (iOS 27) [V].** Every synchronous CMPhoto decode is selector 7 (JPEGDecompressSurfaceHelper 0x1a6b80c9c ->
decodeJPEG2024 0x1a6b80d90), and so are JPEGH1.videodecoder's ('jpeg' sync, 'sjpg' async; 0x2c4988d48). Selector 5 has
no caller; selector 1 survives only on CMPhoto's async decode session (EnableAsyncDecode). Callers zero the 0xda0 bytes
and fill 0x00..0x57 as selector 1 (crop at 0x54..0x6c as selector 5); 0x70..0xc83 (strip index, entropy offset, a marker
blob) are written only on the multi-core strip path, which needs AppleJPEGNumCores >= 2. Apple's kext (startDecoder2024
0xfffffff008b06178) writes back +0x4c/+0x50 (the region) and +0xc94 (microseconds); no caller reads the output. JPEGH1
sends the destination's width and height as the picture's, which this driver refuses as BadArgument when they differ
from the file's (that frame then decodes in software). Properties, formats and the per-image fallback are as on 21H461;
nothing chooses the selector. Debug kernels answer HW Test's route question on selector 7 itself: +0xcac = 0x5050 with
no source surface returns, at out+0, the decodes NVJPG finished for the calling process and at out+8 the output formats
they wrote (bit n: TEGRA_NVJPG_OUT_n).

## 4. The decode request

Selector 1's 0x58 bytes are the first 0x58 of selector 5's 0x1000 (`TegraNVJPGRequest.h`, static-asserted). Filled by
JPEGH1 DecodeFrame (0x20112026c..2fc), CMPhoto JPEGDecompressSurfaceAsync (0x1aaf24ce8..d6c) and
JPEGDecompressSurfaceHelper (0x1aaeecf9c..); read by Apple's startDecoder (0x45f94..0x46004) [V]:

| off | field | notes |
|---|---|---|
| 0x00 | source IOSurface ID | the JPEG file ('JPEG' pixel format surface) [V] |
| 0x04 | source size | sel 1: IOSurfaceGetAllocSize (not the data length; bytes after EOI are garbage); sel 5: min(alloc, data) [V] |
| 0x08 | destination IOSurface ID | its pixel format says the layout: no format field [V] |
| 0x0c | destination alloc size | Apple: ≥ 7 [V] |
| 0x14, 0x18 | the JPEG's width, height | Apple: ≥ 4, ≥ 2 [V] |
| 0x20 | flags | bit 0 high-speed IDCT, bit 1 MCU-aligned 4:2:0 (Apple: no zero-fill), bit 2 no trailing FFD9 [V] |
| 0x24, 0x28 | x, y offset of the picture in the destination | Apple: < 0x2000 [V] |
| 0x2c | subsampling | 0 4:2:0, 1 4:2:2, 2 4:4:4, 3 4:1:1, 4 grey [V] (4:4:0 has no code; CMPhoto refuses such files) |
| 0x30, 0x38, 0x40 | callback, refcon, context | callback 0: synchronous [V] |
| 0x48 | kdebug request ID | |
| 0x4c, 0x50 | decode width, height | DCT scaling: size / 2^n rounded up (CMPhoto JPEGCalculateOutputDimensions); Apple: ≥ 4, ≥ 2 [V] |
| sel 5: 0x54..0x6c | MCUs per row, MCU rows, crop enable, crop x/y/w/h in MCUs | [V] |
| sel 5: 0x7c, 0x80, 0x84.. | header blob length (≤ 0xf7c), entropy offset, Apple's codec registers | the blob is Apple's hardware format; ignored [V] |

- **No tables from userland [V].** Apple's kernel parses no markers (no compare with 0xd8/0xc4/0xdb anywhere): its
  hardware parses the stream. TegraNVJPG parses the file itself (`TegraNVJPGJpeg.h`) from a CPU read of the source.
- **Surfaces [V].** Apple: `IOSurfaceRoot::lookupSurface(id, current_task())` (the caller's task), a protected source →
  0xe00002e2. TegraNVJPG: the same lookup through TegraVIC's bindings into the IOSurface kext
  (`TegraVICSurface.cpp`, checked against the kernelcache on every build by TegraVIC's abi_check.py).
- **Destination formats [V]** (Apple's HAL decoder_req2setup 0x4fb40): 420v/420f, 422v/422f, 444v/444f, 2vuy, yuvs/yuvf,
  32ARGB/BGRA, RGBA (L565 logged unsupported). CMPhoto asks for (CMPhotoJPEGCreateNativeDecodePixelFormatArray
  0x1aaf28f70): 420f, yuvf; with interchange formats also BGRA, RGBA, 422f, 444f. JPEGH1.videodecoder always '420f'
  (0x20111f77c), whatever the file's sampling. UIKit's eager decoder: '420f' or (interchange) 'BGRA'.
- **Output [V].** Sync only: Apple writes out+0x4c (0 for a decode), +0x50/+0x54 (codec registers); sel 5 also +0x70
  (µs) and +0x74 (cycles). CMPhoto's helper reads sel 5's out+0x14/+0x18 back (0x1aaeed064). TegraNVJPG returns the
  request (so +0x14/+0x18 are the JPEG's size) with +0x4c..+0x54 zero and, for sel 5, its engine µs and cycles.
- **Completion [V].** Async: `setAsyncReference64(port, callback, refcon)` and `sendAsyncResult64(status, {context}, 1)`
  (0x44ab4); CMPhoto's `__stripDecodeCompleteCallback` 0x1aaf24dbc and JPEGH1's `jpeg_asyncDecodeComplete` 0x201120f44
  take (refcon, IOReturn, context). Sync: the call sleeps until done (Apple commandSleep 0x46828). Sync callers close
  the connection right after the call (0x1aaf45cc4).

### What TegraNVJPG decodes, and what it refuses (the caller decodes on the CPU)

Accepted [code; hardware facts T]: Huffman sequential 8-bit (SOF0/SOF1), one interleaved scan, 3 components with luma
1 or 2 × chroma 1x1 (4:2:0, 4:2:2, 4:4:4; 4:4:0 parsed but has no request code) or grey 1x1, up to 4 quantisation and
4+4 Huffman tables, restart intervals, Annex K tables when DHT is absent (Motion JPEG), a truncated scan (EOI
appended), any size up to 16384; into '420f' (NV12; grey files: luma by NVJPG, chroma 0x80 by the CPU), BGRA, RGBA,
32ARGB, at 1 and 1/2 (1/4 and 1/8 missed libjpeg-turbo's BGRA on the unit); at any x/y offset that keeps
the plane starts on 256 bytes.

Refused as **kIOReturnUnsupported** (CMPhoto falls back for that image, `__prepare` mode 1, 0x1aaee020c; JPEGH1 decodes
the frame with its software session, 0x201120568; ImageIO's read plugin decodes in software, 0x18d38e118 [V]):
progressive, lossless, hierarchical, arithmetic, JPEG-LS, 12-bit, 16-bit tables, quantisation value 0, CMYK/YCCK, Adobe
RGB, 4:1:1, chroma above 1x1, grey above 1x1, non-interleaved or multiple scans, DNL, sizes above 16384, undefined or
corrupt tables; destination formats other than the four (420v, 422f, 444f, yuvf, ...); a decode size that is not the
file's / 2^n; 1/4 and 1/8; strides not a multiple of 64; plane starts not on 256 bytes; MCU padding that
would run into another plane or past the destination's 1 MiB sink (§7). The destination's pixel format is checked before
anything is copied (under the interchange formats CMPhoto also asks for 422f, 444f and yuvf).; a destination above 255
MiB or a scan above 64 MiB (the SMMU windows); sel 5 crop rectangles. Refused as **kIOReturnBadArgument**: unknown
surface IDs, a request whose size, subsampling or offsets contradict the file or the destination, Apple's own bounds
(dst size < 7, widths < 4, heights < 2, offsets ≥ 0x2000), protected destinations. A protected source is
**kIOReturnNotPermitted** (Apple's 0xe00002e2). TegraNVJPG never returns kIOReturnNotAligned (a wiring failure that says
so becomes Unsupported or IOError): CMPhoto answers that one by copying the bitstream and retrying (0x1aaee0038), and
TegraNVJPG copies it anyway. A decode the engine reports in error (status error, or no bytes consumed) is
**kIOReturnIOError**, also a per-image fallback.

## 5. Who reaches the hardware [V unless marked]

- **ImageIO** (every CGImageSource JPEG decode, so Safari's WebContent [I: WebCore uses ImageIO's default path],
  Files, Messages, wallpapers through UIImage [I]): AppleJPEGReadPlugin uses hardware by default
  (`IIO_use_hw_decoder` default on, `kCGImageSourceUseHardwareAcceleration` honoured, not for progressive, incremental
  or over-large images) through CMPhoto's container API in automatic mode: needs the interchange formats.
- **UIKit** `_UIOldCGImageDecompressor _decompressEagerly:` 0x18a52a7e8 (UIImage prepareForDisplay, thumbnails):
  CMPhoto's JPEG decode session, hardware only, '420f' or 'BGRA'; falls back to CGImageSource.
- **CMPhoto's thresholds**: hardware for ≥ 16384 pixels (`g_cutOffJPEG` 0x4000, CMPhotoShouldUseHWDecoderForSize
  0x1aaee0da0); thumbnails (operation 2) always; CMPhotoJPEGShouldUseHardwareDecode (MediaToolbox's photo path) at
  65536 / 92415 / 262144 pixels. So small images never reach the driver.
- **Photos, PhotoLibraryServices, MediaAnalysis, ImageConversion**: CMPhoto importers (decompression sessions,
  containers) [V imports; thumbnailing through them I].
- **VideoToolbox 'jpeg'** (Motion JPEG playback): JPEGH1.videodecoder, async sel 1 into '420f'.
- **Encode**: ImageIO only with kCGImageDestinationUseHardwareAcceleration; CMPhoto's unified encoder whenever the
  hardware probe succeeded.

## 6. Encode

Not driven here. Selectors 3, 4 and 6 return kIOReturnUnsupported. CMPhoto's unified encoder (`__encodeBuffer`) treats
a failed hardware encode like an unavailable one: the error from `JPEGDeviceInterface_encodeJPEGExt` (other than
Overrun, which retries at lower quality) becomes -16994 and, unless the caller required hardware, the path joins the
"no hardware" one and runs `__encodeSW` (0x1aaeebc90 → 0x1aaeebef4 → 0x1aaeec264 → 0x1aaeec2ac) [V]. The cost is one
refused call (and whatever surface setup precedes it) per CMPhoto JPEG encode [I: measure, next-boot check 6].
JPEGH1.videoencoder never sees the service (registry name, §1). NVJPG's encoder (`nvjpg_enc_drv_pic_setup_s`) is the
next step if encodes matter: sel 3's quality index → Apple's quant tables (_sAppleQualityQMArray 0x1aaf99cb7, order
unconfirmed) and sel 4's custom tables at +0x5c/+0x9c, the compressed size back at out+0x10.

## 7. How TegraNVJPG runs a decode

1. Caller's thread: exact struct size; both surfaces looked up in the caller's task; the source wired, its first 128 KiB
   (up to 4 MiB if the headers run longer) read and parsed to SOS; the scan copied into a kernel buffer, cut at EOI
   (or given one); the plan made (`tegra_nvjpg_plan`: formats, scale, sampling, offsets, strides, extents, every
   plane's writes against every other plane and the surface). Refusals return here.
2. Queued (16 deep, FIFO) for the engine thread; a sync call sleeps until done.
3. Engine thread: the destination wired, mapped page by page into the 256 MiB output window, then **sink pages** (one
   arena page mapped over and over) up to the end of the MCU padding the plan allowed past the surface's last page;
   the scan's buffer into the 64 MiB input window plus one sink page; the destination's lines cleaned and
   invalidated; the job (SET_APPLICATION_ID decoder, SET_CONTROL_PARAMS GP timer + cycle dump, picture index, setup,
   bitstream, status, output planes, EXECUTE awaken; host1x OP_DONE syncpoint by interrupt); the destination
   invalidated again; windows emptied; a grey '420f' gets its chroma from the CPU; surfaces completed and released.
4. Completion: the sync call returns the status, or the async completion goes to the port.

Colour: NVJPG converts with JFIF's full-range BT.601 (T.871), programmed in its 16.16 kernel (TegraNVJPGJpeg.h);
'420f' gets the file's own (full-range) YCbCr. Alpha 0xff.

## 8. The engine (TegraNVJPG.cpp, TegraNVJPGSelfTest.cpp)

| fact | value | source |
|---|---|---|
| registers | 0x54380000 + 0x40000 | tegra210.dtsi nvjpg@54380000 [V] |
| clock / reset | CAR Y bank bit 3 (TEGRA210_CLK_NVJPG 195), CLK_SOURCE_NVJPG 0x69c, PLLP = mux 4 | clk-tegra-periph.c, tegra210-car.h [V] |
| power | PMC partition 26 (TEGRA_POWERGATE_NVJPG) | pmc.h [V] |
| MBIST workaround | LVL2_CLK_GATE_OVRE bits 9, 31 pulsed with NVDEC's clock on | clk-tegra210.c tegra210_pg_mbist_war [V] |
| MC | swgroup nvjpg ASID register 0xac0; nvjpgsrd/nvjpgswr TRANSLATION_ENABLE_3 bits 30/31; hot reset 0x970 bit 8 | memory/tegra/tegra210.c [V] |
| host1x class | 0xc0 | include/linux/host1x.h [V] |
| firmware | linux-firmware nvidia/tegra210/nvjpg.bin (= L4T nvhost_nvjpg010.fw), falcon v1, no FCE | firmware/SOURCE [V] |
| interface | nvjpg_dec_drv_pic_setup_s 0xb2c bytes, nvjpg_dec_status 0x20, cle7d0.h methods | nvidia/SOURCE [V]; works per averne's FFmpeg/oss-nvjpg [I]; on this unit [T] |
| table order | huffTab[0] DC, [1] AC; codeNum = BITS, symbol = HUFFVAL, min codes 0; quant zig-zag | FFmpeg nvtegra_mjpeg.c, oss-nvjpg [I]; [T] |
| outputs | NV12 (memory mode 0), planar, RGB (memory mode 3), downscale 1/2..1/8, pitch linear, strides 64 B | nvjpg_drv.h comments [I]; [T] |
| clock | PLLC2 at the 637 mV point of this SoC process (422.4 MHz for process 1), else PLLP 408 MHz | tegra210b01-core-dvfs.c "nvjpg" row, kits/SwitchCAR/SwitchEngineClock.h [I] |

Stages: J0 host1x + buffers, J1 SMMU (ASID 125 through TegraSMMU; VIC 127, display 126) + power + falcon, J2
IMMEDIATE/OP_DONE by interrupt, J3 publish. Before J3 every failure is one line and the engine is parked (reset, clock
off, swgroup in bypass); after, a job past its bound (2 s queue + 1 µs per 16 grid pixels, ≤ 30 s, GP timer on) panics
with one line and a lost syncpoint interrupt panics in TegraHost1x. Sleep: TegraHost1x's fourth sleep client (after
NVDEC, whose clock the MBIST pulse borrows).

## 9. Gaps and risks

- 1/4 and 1/8 DCT scaling: NVJPG's BGRA missed libjpeg-turbo's scaled decode by up to 7 and 15 steps (J3A) on every
  boot that measured it; off until someone finds whether the setup or the tolerance is wrong.
- Whole-MCU writes: '420f' destinations from CoreVideo leave 16 KiB-aligned room after the luma,
  under 8 padded rows at 1080p, so non-MCU-aligned heights into '420f' are refused (CPU). BGRA uses the sink.
- CMPhoto's native destinations under the interchange formats (CMPhoto fjds_createPixelBufferFromJPEGData: '444f' for
  4:4:4 files, 'yuvf' for 4:2:2, '420f' otherwise [V]) are written by NVJPG itself: '422f' and '444f' semi-planar
  (output_chroma_mode 4:2:2 or 4:4:4, memory_mode 0), 'yuvf' packed (memory_mode 2, YUY2's Y0 Cb Y1 Cr order) [T].
- Encode refused (§6): one wasted call per CMPhoto JPEG encode.
- CPU work per decode: headers + the scan copy (compressed bytes) + cache maintenance of the destination (two passes,
  ~80 µs per MiB measured for VIC) + page-table writes. For the smallest pictures CMPhoto sends (16384 pixels) this is
  close to a software decode [I: measure, next-boot check 5].
- The MobileGestalt answer "AppleJPEGDriver exists" flips to yes; its consumers are unknown.
- Sandbox: which processes Apple's profiles let open AppleJPEGDriverUserClient is unverified; a denied process keeps
  software decoding for its life (CMPhoto's probe).
- A resume that fails (`TegraNVJPG: resume FAIL ...`) leaves the engine refusing work: every later call returns
  kIOReturnNotReady, so each CMPhoto decode costs one refused call before its software decode until the next boot.

## 10. Next-boot checks and the user test

Pull logs read-only after boot (`idevicesyslog -u <udid> archive boot.tar --age-limit 900`, then
`/usr/bin/log show boot.logarchive --predicate 'eventMessage CONTAINS "TegraNVJPG"'`; the kernel's IOLog lines are
also in the boot console).

1. **Bring-up**, in order, one line each, within a few seconds of NVDEC's N3 line:
   `TegraNVJPG: J0 ok firmware 16640 bytes ...`,
   `J1 ok ASID 125 ... partition 26 ... 422400 kHz, falcon started (OS code 0+0x300 data 0x3d00+0x200, 13 apps)`,
   `J2 ok host interface and falcon incremented syncpoint ...`,
   `J3 ok AppleJPEGDriver published (... interchange formats 1, ...)`.
   - A `J<n> FAIL` line ends the bring-up: nothing is published and JPEG stays on the CPU, as before. A
     `J2 FAIL ... no interrupt came` points at host1x/GIC.
2. **The service**: no `JPEGH1`-registered hardware JPEG encoder (VideoToolbox lists only software 'jpeg' encoders),
   and `ioreg`-style property dumps (if available) show AppleJPEGNumCores 1 on the TegraNVJPG entry.
3. **Use**: `TegraNVJPG: first hardware decode done: WxH to wxh (format F, 1/n) in E us engine (C cycles), P us CPU`
   once per output format (F: TEGRA_NVJPG_OUT_*; 6 '422f', 7 '444f' and 8 'yuvf' have no device evidence yet: their
   first lines, and the picture next to the same file decoded on a Mac, are the check); then (debug kernels) every 5 s
   with work an os_log line
   `TegraNVJPG: 5s decodes=N refused=R failed=X engine=..us/decode cpu=..us/decode pixels=..K/decode scan=..KB/decode`.
   Each kind of refusal is one error line per process (kits/SwitchRefusal, subsystem com.brunostjohn.switch.refusal,
   category TegraNVJPG): `<process>[<pid>] <why>: decode refused, the caller decodes on the CPU: <why>`. From the
   caller's side, CMPhoto logs `JPEGDecompressSurface : Picture decode failed: %08x` for a refused or failed call (then
   decodes that image in software): 0xe00002c7 refused (unsupported), 0xe00002c2 bad request, 0xe00002ca engine error.
4. **Failures**: `failed=` should stay 0; any `TegraNVJPG: decode error 0x... after N bytes (MCU x,y)` line names a
   file the engine could not finish (a GP-timer expiry is 0x30000001).
5. **Cost** (never slower): compare `engine=` + `cpu=` per decode with a software decode of the same size (CMPhoto's
   own time for refused images). If the smallest pictures (CMPhoto sends ≥ 16384 pixels) come out slower, the service
   should refuse below a size threshold.
6. **Encode**: saving a JPEG (Photos edit → Save as JPEG, or a Markup export) still works and takes about as long as
   before (CMPhoto's software encoder after one refused call).
7. **Sleep**: after sleep/wake, no `TegraNVJPG: resume FAIL` line; decodes keep counting afterwards.

User test: open Photos and scroll the library and an album at speed (thumbnails: CMPhoto, 1/2..1/8 decodes), open
a few full photos; open a JPEG-heavy page in Safari (a photo gallery or news site) and scroll. Then pull logs: the
5 s lines should show decodes > 0 during both, refused small, failed 0, and no visual corruption (colour cast,
shifted blocks, green or pink tiles would mean a plane or table order the self-test did not catch).
