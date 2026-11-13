# tools/emu

An emulated Switch OLED that boots this tree's real boot chain on yellows8's
[tegra_qemu](https://github.com/yellows8/tegra_qemu) (`-machine tegrax1plus`), with the patches in `patches/`. It is
for trying boot, storage and GPU changes before they go to the device, and its results carry less weight than the
device's.

```sh
bazel run //tools/emu:boot -- --kind=ios27                   # build //:boot for iOS 27, boot it for 120 s
bazel run //tools/emu:boot -- --kind=ios17 --sd none --seconds 60 --bootargs -v
bazel run //tools/emu:check -- --kind=ios17 --until launchd  # a gate: 0 milestone, 1 panic, 2 timeout, 3 no kernel log
bazel run //tools/emu:check -- --kind=ios27 --until gpu --sd none  # 4: the GPU kext or its kernel channels failed
bazel run //tools/emu:build                                  # only the emulator: prints its path
bazel test //tools/emu:dc_test                               # the display controller and SMMU under qtest
```

`check` watches the live kernel log and the panic record every 3 s and stops at the first of the milestone (`cores`,
`display`, `gpu`, `sd`, `root`, `launchd`, `early-boot`), a panic or `--timeout`. It prints the panic line or the last
kernel lines, and keeps the run directory. `launchd` is reached at the first kernel line that names a process other than
`init` (pid 1 before its exec) or `kernel_task`, such as launchd's dyld asking for the shared region (`[1(launchd)]`) or
a mount requested by a job launchd spawned. `gpu` is reached when the GPU kext's M3 and M4 have passed and its boot pool
scrub is queued, by which time the self-test's copy-engine fill and fence and the scrubber's pool fills have run on the
model. Some boots never print the scrub's queued line (gpucheck.py says why), so a kernel line 2 s after M3, past the
scrub's own 1 s deadline, also reaches it. Exit 4 means a bring-up FAIL line, a GPU error report from nvkm or the kext,
a fault or stopped channel in `gpu-events.log`, a method its class lacks on GM20B (which the model runs but the GPU
kills the channel for), or a run in which the GPU executed no method or the scrubber no fill. It needs no card
(`--sd none`) and takes about 10 s of wall time. If the kernel runs for 10 polls and no log line comes out, `check`
stops with exit 3 and prints what the reader found: no msgbuf header, or one whose ring is still empty.

`--kind` builds `//:boot` with `--//ipsw:kind` in the output base `bazel run` itself uses. `--bazel-flag` passes flags
to that build (for example `--bazel-flag=--config=agent`). `--package DIR` boots an existing `//:boot` output instead,
and `--kernel` names its prelinked kernel, whose `_msgbufp` locates the live log. A release kernel has no `_msgbufp`, so
without one check finds the msgbuf header by scanning the kernel's memory. Guest time follows executed instructions, one
nanosecond each (`-icount shift=0`, one host thread for all four cores), so neither a busy Mac nor TCG's own speed can
expire a driver's timeout before the work it waits for has run. Kernel log times are then instruction-scaled seconds,
close to the A57's CPU time, not wall time. With one thread per core (`--mttcg`) at load 50, or with guest time kept to
the wall clock (`shift=auto`), drivers panicked on interrupts that had not been lost. `--mttcg` is faster on an idle
Mac. The run directory is `out/emu/<kind>-<time>` unless `--out` names one:

| File | What |
|---|---|
| `summary.txt` | How far it got: firmware, secondary cores, core PCs, the panic, the last kernel lines |
| `uart-a.log` | bl31, U-Boot, the boot script, the exporter and the loader, on UART-A |
| `kernel.log`, `panic.txt` | The kernel's log (XNU's msgbuf, read live through a core's MMU) and its panic record |
| `screen.png`, `framebuffer.png` | What display controller A sends the panel (its windows blended), and hekate's framebuffer |
| `registers.txt` | Every core's registers when the run stopped |
| `gpu-methods.log` | Every class method a GPU channel ran, named from Mesa's class headers, invalid ones flagged |
| `gpu-events.log` | The GPU's bring-up (falcons, PMU and FECS commands, runlists), MMU faults, stopped channels |

## What runs

Core 0 starts in `firmware/regwrite.S`, which replays the register writes in `hekate.py`: hekate's display bring-up,
the memory controller's carveouts, the microsecond timer and the BL31 parameters in PMC scratch. It then enters
switchroot's `bl31.bin` the way hekate's l4t launcher does. From there on everything is the real thing: bl31,
switchroot's U-Boot (its console on UART-A, as hekate's `uart_port=1` sets it), the boot script, the SD exporter, the
loader and XNU. Secondary cores start through bl31's PSCI and the flow controller, on Tegra's own GIC. Idle cores
power down in C7 through bl31's CPU_SUSPEND, and the flow controller brings them back through their reset vector
when the GIC (an SGI or SPI) or the legacy interrupt controller (TMR10-13) has something pending for them.

The SD card is a 2 TiB VMDK of flat extents. It holds hekate's hybrid MBR and GPT, a FAT with the run's boot files
and `bootargs.txt`, and the iOS partition, which maps straight onto the install image from `bazel run //:image`
(`out/sd/switchroot/ios/IOS<NN>.SII.*` in this checkout or the main one, else
`~/Library/Caches/switch-ios/sd-<build>/switchroot/ios/`). QEMU writes to a snapshot overlay, so a run
never changes the image. The card is 2 TiB rather than the real 1 TB because QEMU only takes power-of-two cards.

The GPU (patch 0030) answers our TegraGM20B kext's bring-up with synthesized firmware: the PMU's ACR load and
RTOS messages, FECS's method interface. It runs channels: runlists, GPFIFO and pushbuffer parsing, host semaphores,
syncpoint increments, and the copy engine's copies and fills. Every access goes through a real GMMU walk from the
channel's instance block. A bad or unmapped VA, or a write to a read-only page, raises the fault GM20B reports (the
fault record at 0x2800 + unit, PFIFO's MMU_FAULT interrupt) and stops the channel. 3D, compute and 2D methods are
logged, never drawn, apart from their report semaphores.

## Caches

Every worktree and output base on the Mac shares these, under `~/Library/Caches/switch-ios`:

- `tegra-qemu/entries/<key>` is the emulator. Its key covers the pinned commit, every patch byte and the configure
  flags. A changed patch costs one rebuild on a shared ninja tree, which recompiles only the files the patch touches.
- `sd-base/entries/<key>` is a card base per install image (APFS clones of its parts, the tables, an empty FAT),
  keyed by the image's manifest. The clones keep an old image's blocks on disk after `//:image` replaces it; delete
  old entries to get the space back.

Each entry is built under a lock and renamed into place, so a second builder waits and then reuses it. Building the
emulator needs Homebrew's glib, pixman, libgcrypt, dtc, pkgconf, ninja and mtools, and `uv`.

## How far to trust it

Modelled: the Cortex-A57 cores (CPUECTLR included), GICv2 behind the legacy interrupt controller, the flow
controller, PMC, CAR, MC, fuses, I2C with the PMIC and its RTC, display controller A, the SMMU for the display's reads,
and SDMMC1 as an SD Host Controller 3.00 in Tegra's v4 mode.

Display controller A keeps three copies of each window register. `*_UPDATE` copies the assembly copy to the armed
one, and `*_ACT_REQ` reads 1 until the next frame end makes the armed copy active. Windows A to D scan pitch or
block-linear surfaces in B8G8R8A8, R8G8B8A8, B5G6R5 or 4:2:0 (through the window's CSC), scale by their DDA increments,
turn with `SCAN_COLUMN` and blend by depth. The CRC unit reads 0 at the first frame end after it is turned on and the
first frame's CRC at the second, as the Switch's does.

Different from the hardware:

- Timing. CPU speed, DRAM and every device latency are QEMU's, not the Switch's.
- V_BLANK, FRAME_END, every latch and the CRC happen at one instant per frame, and line-counter latching
  (`HCNTR_SEL`) takes effect on the write. V_PULSE2, underflows, the cursor, colour keys and the filters are
  missing; scaling takes the nearest pixel.
- The CRC is CRC-32C over the active pixels' 24-bit RGB, so its values differ from the device's: no public source
  gives the hardware's algorithm. Our driver only compares CRCs with each other.
- The SMMU has no TLB, so stale-translation bugs don't show. A fault, or a read outside DRAM, reads zeros and logs one
  line per page in `qemu-guest.log`.
- The SD card stays in U-Boot's 48 MHz mode. The controller reports no SDR104, so SwitchSD's SDR104 bring-up does not
  run.
- No BPMP firmware runs, so the EMC keeps its rate.
- No GPU falcon code runs and nothing is drawn. The GMMU has no TLB, so stale-translation bugs don't show. A channel's
  work runs to completion when its GP_PUT is written or the host's 200 us poll comes, in runlist order on one PBDMA,
  without timeslicing or preemption, so PBDMA and engine status always read idle and a channel waiting on a semaphore
  acquire looks idle to the kext's hang checks. There is no L2: every GPU access goes to DRAM, so a missing L2 flush or
  invalidate goes unnoticed (M3's L2-cached target included). An invalid pushbuffer header stops its channel without the
  PBDMA interrupt the hardware raises.
- Missing: security engine keys, TSEC, audio, USB, and the Wi-Fi and Bluetooth radios behind their buses.

A boot that gets further here is a hint. A panic here is a lead to check on the device.

## Patches

`patches/*.patch` change GPL-2.0-or-later QEMU files and are under that licence. The build copies QEMU's COPYING
files next to the binary.
