# perf

Turns one session on the device into a report on what was slow and why: frame pacing per scenario, every missed frame
with its timeline and the stage that lost the period, CPU (including the background floor and per-window kprof hot
spots), GPU, state creations and compiles, page-ins, shared-cache unnests, hangs and the stacks in the crash folder.
It reads a debug boot's lines and Apple's own signposts. Pass and fail verdicts stay in `tools/devicerun`.

```sh
bazel run //tools/perf -- pull "$HOME/Library/Application Support/switch-ios/sessions"   # Switch plugged in
bazel run //tools/perf -- report "$PWD/boot.logarchive" --crashes "$PWD/crash" --out "$PWD/report.txt"
bazel run //tools/perf -- report "$PWD/boot.logarchive" --crashes "$PWD/crash" --dsc "$PWD/dsc.tsv"
bazel run //tools/perf -- symbols --build 24B119 --dsc /path/to/dyld_shared_cache_arm64e --kernel /path/to/kernel.macho
```

`pull` makes a dated folder holding `boot.logarchive`, `crash/` and `report.txt`. `bazel run` changes into the runfiles
tree, so give absolute paths. From the repository root, `python3 -m tools.perf.perf` takes the same arguments and needs
nothing built. A 270 MB archive takes about 10 s.

## Reading it

A window is a HW Test test (its result line less its `ms=`), an app launch (FrontBoard's ask to the scene's content
turning ready), a touch run, a hang or a mark. Each section is read per window, and each launch, hang or test dossier
includes `SwitchKprof` hot spots, `SwitchSched page-ins` (by process and file) and `SwitchUnnest` shared-cache unnests
when those lines are present.

The stage that lost a period is the first point of the present's chain past the frame end it was due at: the render
thread's wake, its first GPU submission, its swap, the display queue, the GPU's completion, then the flip. The longest
stage would mislead, because a late frame spends most of a period waiting for the next scanout. Presents these lines
cannot place get their own rows and are never counted as lost. An unanchored present had no frame end before it, because
the display was idle. A gap before it means the present kept its own chain but the frame before it never showed. When
the render thread waited, it spent half a period or more in one block before its swap, waiting for the app's commit or
for a server rather than recording.

A cycle line means the late presents of a window keep to a timer's period, so something periodic is behind them.

The host1x section gives each channel's share of host1x's clocked time spent waiting on a syncpoint, from the channel's
own stall counter. A channel runs its jobs in order, so one wait sits inside the time of every job queued behind it
while the counter counts it once. The share is never subtracted from an engine's job time.

## Symbols

Shared-cache addresses in dumps, stackshots, `SwitchKprof` and `SwitchUnnest` are unslid, so one table per IPSW build
names them without keeping a shared cache on disk. `symbols` writes `dsc.tsv` (image ranges) and
`dsc-functions.tsv.gz` (every code symbol) with `ipsw dyld`, and `kernel.tsv` or per-binary `.tsv` tables with `nm`,
into `~/Library/Application Support/switch-ios/symbols/<build>/`. The first lookup that needs the function table
writes an index beside it (`dsc-functions.idx`; 130 MiB and 11 s for 24B119), and later lookups use the index instead
of loading the table. `report` loads that directory when it knows the build (from `.ips` reports or `--build`), or
reads `--dsc` (a `.tsv` table, a symbol directory, or a raw `dyld_shared_cache` binary), `--kernel`, and `--symbols`.
Without tables, an address prints as `dsc 0x<unslid>`, `k 0x<unslid>`, or `<image>+0x<offset>`.

## Flight recorder

A debug kernel built with `--//kernel:recorder` keeps the last two to three seconds of the scheduler, system calls,
faults, disk reads, the SD queue's picks, the GPU's jobs and waits and the frame timeline in a ring, with every busy
core's kernel and user stack sampled every 4 ms. It also runs kprof for the first four minutes of the boot. It costs
the render thread an estimated 0.04 to 0.2 ms a frame, so measure frame times on a build without it.

Four things ask for a capture: Capture on the left Joy-Con or a Bluetooth controller, a busy stretch of 250 ms on the
main thread of SpringBoard, backboardd or a focal app, a present six periods or more late, and slow work an app reports.
Every press is captured, ahead of anything else waiting. Automatic captures are at least 2 s apart, and 10 s for the
same reason and thread; each refusal is logged with its cause. A capture copies the ring and a stackshot into memory
within tens of milliseconds; the stackshot stops every CPU for about 3 ms. A low-priority thread compresses and writes
the capture later, when nothing else wants a core. Captures wait in memory uncompressed. Once they fill it, as at boot,
a new automatic capture is dropped before it stops any CPU, and a press takes the ring of an automatic one.

A capture is one `Switch-recorder-<n>.rec` in the crash folder, which `pull` copies. Its text header names the boot, the
trigger and how soon each step ended. The store keeps as many files as a tenth of the free space holds. When it is full,
a new capture replaces another boot's file first, then one of this boot's automatic captures after its first eight. An
automatic capture never replaces a press. A file with no header was pulled while it was being written. Older kernels
wrote `Switch-recorder-<n>.ktrace` and `.kcdata`, which the report still reads.

The report names the thread that triggered a capture, what ran in the second before, its stacks, who woke it and what
it waited on. For a hang it gives the stretch's split (on a core, blocked, waiting for a core), the thread's faults
and the disk reads they waited behind, with each read's I/O tier. It also covers page-in I/O, the SD queue, shared-cache
unnests, the GPU's submissions, and the compositor's waits on other processes' work. `report --crashes <folder>` with
no archive summarises the captures alone. `sysctl debug.switch_recorder` gives the ring's span and event rate, the
captures in memory and written, and the refusals by cause.

## A session

1. Boot a debug build (with `--//kernel:recorder` to get dumps, or `--//kernel:kprof` for background CPU attribution).
   Let it settle for ten minutes so the post-boot work is over.
2. Unplug and do the scenario. Note when you started and give that time to `report --start`. Launch each app twice,
   since the first launch after a boot is cold. Press Capture on the left Joy-Con right after a hitch to capture the
   seconds before it. The report lists each press with the hangs that followed it.
3. Plug in and run `pull`. It pulls back to the end of the last pull in the same folder, and the report names any
   stretch with no record at all.
