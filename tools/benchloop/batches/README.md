# NAK question batches

These batches put seven questions NAK's SM50 backend has about GM20B to the Switch. The generators live in
`tools/benchloop` (`sm53/affinity.py`, `sm53/int3.py`, `sm53/overlap.py`, `sm53/icache.py`, `store2fma.py`,
`sm53/hsetp2.py`, `sm53/interlock_spin.py`). After changing one, rewrite the files with
`python3 -m tools.benchloop.nakbatches` from the repo root. `families_test` fails while a file and its generator
disagree.

## Running them

Install a HW Test at benchloop/6, open its Bench screen, then from the repo root run all of them in order:

```sh
bazel run //tools/benchloop -- run "$PWD"/tools/benchloop/batches/{sr_affinity,int3_xmad,overlap,icache,store2fma,hsetp2,interlock_spin,icache_128k}.json
```

That is 429 experiments, sent as two batches. One batch alone is
`bazel run //tools/benchloop -- run "$PWD"/tools/benchloop/batches/<name>.json`. Each file opens with a warm-up draw
that lifts the GPU off its 76.8 MHz idle clock. After the usual status line the run prints one line per answer, and
`report.json` in the result folder keeps the detail under each family's key.

Every batch goes out as benchloop/6 (`protocol.py`). An app from before it refuses each batch whole, and
`benchloop doctor` names the mismatch.

## What each batch decides

### sr_affinity

5 experiments, 12 KB. Each pixel of a full-screen draw stores SR_AFFINITY. NAK's interlock puts a warp's ticket word at
`(affinity & 0xff) * 256 + ticket id`, and the raw path's ticket buffer is 4 KiB, room for affinities 0 to 3. GM20B has
one GPC, so every line should read max 0:

```text
affinity.screen max 0 histogram {0: 921600} buffer 4 KiB
affinity.tile max 0 histogram {0: 32768} buffer 4 KiB
affinity.screen4 max 0 histogram {0: 921600} buffer 4 KiB
affinity.virtid max 0 histogram {0: 32768} buffer 4 KiB
```

The two screen shapes keep the register's low 16 bits in R32F; the tiles keep all of it in RGBA32F.
`affinity.screen4` draws over the screen four times. A max above 3 means the ticket buffer has to grow to (max + 1) KiB.
`affinity.virtid` reads SR_VIRTID as well and runs last, since nothing has read that register in a pixel shader yet.
`report.json` lists the whole register values seen.

### int3_xmad

221 experiments, 1.2 MB. It covers XMAD (register, immediate, .PSL, .MRG, and .PSL.CBCC with both halves high), LOP3.LUT
(register, immediate), IADD3, and ISCADD (register, immediate).

- Latency comes from dependent chains in one warp at delays 1 to 15. Any single link that reads its stale register
  changes the chain's result. All ten should read like `stall XMAD -----++++++++++ latency 6 (NAK Xmad)`.
- The same chains run over the whole target at delays 5, 6, 7, 8, 10 and 15, beside FFMA (fixed latency) and MUFU.RCP
  (variable latency, no barrier). A variable-latency op can look fixed when its warp is alone, as MUFU did at 8 cycles,
  so this run decides whether NAK may drop XMAD's scoreboard. Expected: `stall-wide XMAD -+++++ right from 6`, and the
  same for FFMA. MUFU.RCP shows what contention does to a variable-latency op.
- Issue rate is `throughput XMAD <c> cycles/warp-instr`, from k = 512, 1024 and 2048 over the whole target. There is
  no prediction. FFMA-class ops measured about 1.06 this way and half-rate ops about 2.06. The k = 2048 programs are
  22 KiB, near the size where the instruction cache started to cost.
- NAK's multiplies (`sm50/xmad.rs`) run over 6,144 operand pairs, 81 of them edge cases and the rest random: the 3-XMAD
  32-bit product, the 4-XMAD 64-bit low and high words, and the signed high word, each checked against the exact
  product. Each instruction issues 30 cycles after the one before, so any latency up to 30 cycles gives the right
  product. Expected: `xmad-mul mul32-hi 6144 pairs, 0/0 words wrong` and `xmad-mul lo-shi 6144 pairs, 0/0 words wrong`.

The batch also carries the clock ladder. Its `clock 769.1 MHz` line (give or take one) comes first, and every cycle
figure in the sitting uses that clock.

### overlap

28 experiments, 160 KB. Looped 256-instruction bodies run over the whole target: FFMA:F2F at 3:1, FFMA:SHL at 1:1,
FFMA:MUFU.RCP at 3:1 and FFMA:ISETP at 1:1, plus each class alone at the same k (512, 1024, 2048). Every stream uses the
same registers and register count, so occupancy doesn't change between them. Each line puts the mix beside two
predictions built from the pure streams: overlap, where the busiest class sets the pace, and add, where the classes'
costs sum.

```text
overlap FFMA3-F2F1 <measured> (overlap ~1.03, add ~1.82) overlap|add
overlap FFMA1-SHL1 <measured> (overlap ~1.03, add ~1.56) ...
overlap FFMA3-MUFU1 <measured> (overlap ~1.03, add ~1.81) ...
overlap FFMA1-ISETP1 <measured> (overlap ~1.13, add ~1.66) ...
```

The last word tells NAK's scheduler whether quarter-rate and half-rate ops are free beside FFMA or cost added to it.

### icache

12 experiments, 340 KB. Every program runs the same path, 128 segments of 92 independent FFMAs (11,776 a pixel), each
segment counting down and then branching to the next of its distinct 1 KiB segments. Only the number of distinct
segments changes (1, 2, 4, 8, 12, 16, 20, 24, 32, 48 and 62), so the code footprint is the one variable. The 1.3 KiB
line is the looped small-body control. Expected: `icache <KiB> KiB <c> cycles/warp-instr` stays flat at about 1.06 up
to the cache size and rises past it, somewhere between 11 and 22 KiB going by the earlier straight-line runs. Where it
rises is the code size NAK's hot shaders should stay under.

### store2fma

53 experiments, 3.7 MB. This is the unorm store with the two-FFMA form NAK's front end now emits,
`t = FFMA(v, top, 2^23)` then `w = FFMA(t, f32(1/top), -2^23 * f32(1/top))`, unblended, into BGRA8, RGBA8, RGB10A2
(alpha top 3, with its own ties and codes), RGBA16Unorm and R16Unorm (every fourth half, tie and code, every special).
RGBA16F takes v as computed.

The inputs are f32 bit patterns: all 65,536 halves, NaN payloads (0x7fffffff, 0xffc00000, 0x7f800001, 0xffffffff),
±inf, -0, -1, 1.0001, 2, denormals, the -8..7 ulps around every tie (2k+1)/(2 top) and the -4..3 ulps around every code
k/top (a superset of -6..6 and -2..2). For RGBA16F the sweeps are instead the -2..1 ulps around the midpoint of every
fourth pair of neighbouring halves.

Metal's rule clamps v to [0, 1] (NaN to 0) and rounds v * top to nearest, ties away from zero. A half is the f32
rounded toward zero, with finite overflow at 65504, infinities kept and every NaN stored as 0x7e00. Rounding both FFMAs
exactly on the host and applying the ROP's measured store rule already gives 0 mismatches for all 1.93 million distinct
inputs. Expected:

```text
store2fma bgra8unorm 71700 values, 0 off Metal, 0 missing
store2fma rgba8unorm 71700 values, 0 off Metal, 0 missing
store2fma rgb10a2unorm 102420 values, 0 off Metal, 0 missing
store2fma rgba16unorm 1638420 values, 0 off Metal, 0 missing
store2fma r16unorm 409618 values, 0 off Metal, 0 missing
store2fma rgba16float 129044 values, 0 off Metal, 0 missing
```

`report.json` also counts the values that differ from the host's prediction (`off_predicted`). A value off Metal but
on the prediction means Metal's rule was misread; a value off both points at the chip.

### interlock_spin

49 experiments, 90 KB. Every pixel adds one to its own counter word inside the interlock, with the steps
`nak_nir_lower_fs_interlock_intrin` emits. The leader election, ticket and release words match a shipped pack's, but
the loop around the spin doesn't: NAK's leader loops alone under SSY/PBK/BRK while the rest of the warp waits at SYNC,
and here the whole warp loops with the read guarded to the leader and the exit decided by a vote. Form A's time is
therefore close to the shipping sequence's without being equal to it; the comparison between spins is like for like.
The leader spins on `ATOM.E.ADD [ticket], 0` (form A, NAK's default), on `LD.E.CG [ticket]`, which skips L1 (form B),
or on `LD.E.CV [ticket]` (`ldcv`, cache op 3), the load NAK's load-spin lowering emits. A fourth form drops the
interlock as a control, to show the test can lose updates. Each target size (64x64, 256x256, 1280x720) gets a
check run of 64 overlapping full-target draws and a timed run of 16, 32 and 64 draws, 3 repeats after 1 warm-up, which
brings every counter to 448. Each of those runs once alone and once (`-compete`) while a second bench device, on its
own GPU channel, draws a constant colour over 1280x720 in a loop (`kit/BenchCompetition.swift`). Expected:

```text
interlock.atom.<size>.check ok lost 0 ns/draw -
interlock.ldcg.<size>.check ok lost 0 ns/draw -
interlock.ldcv.<size>.check ok lost 0 ns/draw -
interlock.none.<size>.check ok lost <more than 0> ns/draw -
interlock.atom.<size>.timed ok lost 0 ns/draw <a>
interlock.ldcg.<size>.timed ok lost 0 ns/draw <b, below a if the atomic spin gets in the release's way>
interlock.ldcv.<size>.timed ok lost 0 ns/draw <c, near b>
interlock.<form>.<size>.<check|timed>-compete ok lost <as alone> ns/draw <above alone> competitor <more than 0> buffers
```

If the load spins lose nothing, alone and beside the other channel, and run faster than the atomic one, NAK can spin on
the load it emits (`ldcv`). A `-compete` line with `competitor 0 buffers` ran alone after all. Sizes run smallest first,
so 720p comes last. If a 720p run is killed (500 ms with no fence), skip the rest of 720p; its result records the kill.

### hsetp2

59 experiments, 440 KB. HSETP2's predicate latency, from chains in one warp at delays 1 to 15 beside FSETP and ISETP
(NAK gives HSETP2 14 cycles; FSETP measured 13), its issue rate, and 99 forms of it over 2,048 half2 pairs: every
comparison plain, with .FTZ and with .H_AND, the h1h0/h0h0/h1h1 swizzles of A and B, |a|, -a, |b| and -b, and the
accumulator (true, false, inverted) under AND, OR and XOR. The pairs put every edge half (±0, ±inf, NaNs, denormals,
the limits, 1 and its neighbours) against every other in the low halves, then random ones. Each form's two predicates
go out through SEL and are checked against `execute.py`'s `hsetp2()`. Expected:

```text
stall HSETP2 ------------+++ latency 13 (NAK HSetP2)     (or -------------++ latency 14: NAK's figure holds)
stall FSETP ------------+++ latency 13 (NAK FSetP)
stall ISETP ------------+++ latency 13 (NAK ISetP)
throughput HSETP2 <c> cycles/warp-instr
hsetp2.forms.0 32 forms x 2048 pairs, 0 predicates off the interpreter
hsetp2.forms.1 32 forms x 2048 pairs, 0 predicates off the interpreter
hsetp2.forms.2 32 forms x 2048 pairs, 0 predicates off the interpreter
hsetp2.forms.3 3 forms x 2048 pairs, 0 predicates off the interpreter
```

A form that disagrees gets its own line under its program's, with how many P and Q predicates are off and the first
operands. The F32 swizzle isn't covered, since the interpreter has no model of it.

### icache_128k

2 experiments: the icache program at 127 distinct segments, past the 64 KiB of code a program had before benchloop/6.
Expected: `icache 127.3 KiB <c> cycles/warp-instr`, above the 62.3 KiB line if the trend holds.

## What the app does for interlock_spin

- Storage: before the first command buffer the bench allocates `storage.bytes` of zeroed write-combined GPU memory,
  writes its address into the shader data at `storage.va_at`, names it in every command buffer (the addressed job ABI
  resolves only the addresses a command buffer names), and after the last returns every distinct word with its count.
- The interlock control: 0x1224 is on the allow-lists, and only the bench's own copy of the raw runtime, in debug
  builds, takes a record that sets it without a storage form (`hw_test_app/bench/bench_completed.cpp`). The ticket
  buffer needs nothing new: the root table holds it at `c[0][0x260]`.
- `compete`: `kit/BenchCompetition.swift` runs the app's known program on a second bench device. Each bench device opens
  its own kernel connection, GPU VM and channel (`raw_gpu.cpp` `Device::open`, `Device::openChannel`), so the two
  channels time-slice the GPU as two apps' would.
