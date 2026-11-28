# NativeSwitchBlock

Tegra X1 SDMMC1 block driver and direct cryptex read driver for the Switch's SD card APFS partition.

## What it does

`NativeSwitchBlock` brings the card from the inherited 48 MHz 3.3 V bus to UHS-I (1.8 V) unless `switch-no-sd-uhs` is
set, then exposes the APFS GPT partition as an `IOBlockStorageDevice`. When the card advertises SD 6.00 DDR200 in CMD6
group 1, `setup()` selects DDR200 (`PLLC4_OUT0` / 2.5 at 399.36 MHz divided by 2 in SDHCI for 199.68 MHz DDR) and runs a
128-tap manual tuning sweep that requires at least 8 passing taps; cards without DDR200 or without a passing window
power-cycle and fall back to SDR104 (199.68 MHz SDR, hardware tuned).

Data moves by SDHCI v4 64-bit ADMA2 straight into request pages with Auto CMD23 framing when the SCR lists CMD23.
Refused read segments stage individually through a 1 MiB bounce buffer inside ADMA2, and other bounce cases fall back to
whole-chain SDMA. Queued reads and contiguous writes merge across the queue (reads also allow bounded read-through gaps
into a 32 KiB sink buffer; writes carry no gaps). The driver keeps two prepared ADMA2 slots so the primary interrupt
filter rotates a clean completion and issues the next prepared command directly from the ISR while the worker thread
finishes cache invalidation, staged copies, and client completions.

Requests leave the queue in I/O tier order (`SwitchSdPick.h`), and within a tier reads go before writes. A request can
pass older requests of a worse rank unless it overlaps an older write. A write also may not overlap any request it
passes, and never passes an older write. While a synchronize waits, the writes it counts rank with tier 0. A request
that may not pass an older one lends that one its rank, so a page-in inside a queued write, or a write a synchronize
waits for, is not held to the share turn. A request passed over for 100 ms goes first once, and again after every 50 ms
or 8 chains. Both block devices publish IOStorage's Priority feature and implement `doSetPriority`, so XNU raises a
lower-tier page-in that a foreground thread is waiting on. Both XNU trees keep the software throttle on for them
(`bsd/vfs/vfs_subr.c`).

When the card exposes the SD 6.00 Performance Enhancement extension with cache support, `setup()` enables the card's
volatile write cache and `doSynchronize` / `doSynchronizeCache` flush it with CMD49. At suspend, shutdown, and restart,
the driver quiesces the worker, flushes any dirty writes, and sends Power Off Notification (`POFN=1`, waiting for busy
by interrupt and polling `POFR` up to 1.5 s) when the card supports it. On cards without discard, unmapped allocation
units are erased in small runs during idle periods after boot completes.

`SwitchCryptex` maps cryptex image extents onto the block device below DiskImages and the image files' page cache. Small
reads (up to 16 KiB) go through an 8 MiB cache of 32 KiB windows, and larger misses go straight to the card by ADMA2.
Window fills read at `kIOStoragePriorityHigh` for tier-0 callers and at Default for everyone else; a tier-0 read that
waits on a fill raises it. A raise that comes before the fill is queued marks its window, and the fill's issuer raises
it once queued.

## Tests

```sh
bazel test //drivers/NativeSwitchBlock/test:all
```

## Why SD command queueing is absent

Tegra X1's SDMMC1 controller is an SDHOST 4.0 host without a hardware command-queue engine (CQHCI). Every queue protocol
command must be issued in software and waited for by interrupt.

- Sequential mode (SD 6.00 4.19.1.2) assigns every task with CMD44 and CMD45 before any CMD46 data transfer starts.
  Across six device boots on a SanDisk A2 card (SR01T, queue depth 32), 16 queued random 4 KiB reads were 46 to 121 us
  per task slower than 16 single-command reads (36 to 111 us net of host cache work) because the card's overlap in
  sequential mode did not cover the two extra interrupt-driven commands per task.
- Voluntary mode (SD 6.00 4.19.6), where A2 random read performance is defined, polls the Task Status Register with
  CMD13 until a task reports ready (up to 250 ms). That violates the no-polling rule and adds at least one more software
  command per task.

Running one data command at a time with queue merging and pipelined completions uses one interrupt per command and
avoids both costs.
