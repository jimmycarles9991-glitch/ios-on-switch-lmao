# NativeSwitchBlock

Tegra X1 SDMMC1 block driver and direct cryptex read driver for the Switch's SD card APFS partition.

## What it does

`NativeSwitchBlock` brings the card from the inherited 48 MHz 3.3 V bus to UHS-I SDR104 (1.8 V, 199.68 MHz, hardware
tuned) unless `switch-no-sd-uhs` is set, then exposes the APFS GPT partition as an `IOBlockStorageDevice`. Data moves by
SDHCI v4 64-bit ADMA2 straight into request pages with Auto CMD23 framing when the SCR lists CMD23; refused read
segments stage individually through a 1 MiB bounce buffer inside ADMA2, and other bounce cases fall back to whole-chain
SDMA. Queued reads and contiguous writes merge across the queue (reads also allow bounded read-through gaps into a 32
KiB sink buffer; writes carry no gaps), and completions run on the worker thread while the next command runs on the
card. `SwitchCryptex` maps cryptex image extents onto the block device below DiskImages and the image files' page cache.

Writes run write-through. The driver never turns the card's volatile write cache on, and `setup()` flushes and disables
any cache left enabled by an earlier boot stage (panicking if the card fails to clear both bits). At shutdown and
restart, when the card exposes the SD 6.00 Power Management extension with Power Off Notification support, the driver
quiesces the worker, rejects new I/O, writes `POFN=1`, waits for CMD49 busy by interrupt, and reads `POFR` (re-reading
every 10 ms up to 1.5 s if the card is not ready yet) before power goes off.

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
