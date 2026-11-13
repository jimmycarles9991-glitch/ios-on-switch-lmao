# benchloop

Runs small GPU experiments on the Switch without a rebuild or a reboot. The Mac writes a batch of experiments into a
folder on the device over USB, HW Test's Bench screen runs each one on the GPU, and the results come back as files next
to the batch. Each result has the GPU time of every command buffer, from the raw path's PTIMER stamps, and the target's
texels. It exists to measure things nothing documents: SM53 instruction latencies, dual issue and operand reuse, the
ROP's iterated blend unit and its float-to-unorm rounding.

## On the Switch

The apps are on debug images only. Open HW Test. The first launch after a boot runs every test, so let that finish,
then tap Bench at the top left. The screen keeps the display awake and has to stay open while batches run.

The Self-check line should start with `PASS`. It times empty passes, draws the app's own known-answer program and runs a
kernel that only exits, all without the Mac. The Inboxes section lists the folders the app watches. If
`/private/var/mobile/Media/Switch/benchloop` is missing, run `doctor` once (it creates the folder over AFC), then close
the screen and tap Bench again.

## On the Mac

```sh
bazel run //tools/benchloop -- doctor                  # finds the device and a folder both sides reach, runs the calibration batch
bazel run //tools/benchloop -- latency                 # SM53 latency, throughput, dual issue, reuse
bazel run //tools/benchloop -- latency --family stall --op FFMA
bazel run //tools/benchloop -- blend "$PWD/table.json" --blend-enable both
bazel run //tools/benchloop -- rop                     # float to unorm rounding
bazel run //tools/benchloop -- floatadd                # the ROP's fp32 and fp16 ONE, ONE, ADD against the shader's sum
bazel run //tools/benchloop -- precision               # store and blend under each ROP option set the other way
bazel run //tools/benchloop -- iterated "$PWD/microcode.tsv"  # iterated blend, colour 0 bound alone
bazel run //tools/benchloop -- twod                    # 2D-engine fills, scaled copies and resolves, with compares
bazel run //tools/benchloop -- store16                 # float to unorm16 rounding (R16, RGBA16)
bazel run //tools/benchloop -- compressed              # draws into compressible memory against a plain target
bazel run //tools/benchloop -- cstall                  # the stall family as kernels storing their results
bazel run //tools/benchloop -- qmdchain                # dependent QMDs: one launch runs a chain, and a kernel steers it
bazel run //tools/benchloop -- cbufsize                # a constant buffer bound at 16..768 bytes: which words read back
bazel run //tools/benchloop -- run "$PWD/batch.json"   # any batch in protocol.py's format
bazel run //tools/benchloop -- latency --fake /tmp/fake  # the same loop against fakedevice.py, no Switch needed
```

Give absolute paths, since `bazel run` changes directory. Results go to
`~/Library/Application Support/switch-ios/benchloop/<time>/`: the device's result files, `results.tsv` (GPU ns per draw,
the fixed cost, the fit's largest residual and whether the texels were right) and `report.json`. A latency run also
writes `sm50_latency.rs`, the measured table in the shape of NAK's `instr_latency`.

`doctor` tries HW Test's Documents through house_arrest first, then the AFC root, and remembers the first folder the
calibration batch comes back through. Nobody has checked yet whether house_arrest serves an app that lives in
`/Applications`, which is why the AFC folder exists. While a batch runs, the Mac follows the app's `BENCH` log lines
through `idevicesyslog`. If the result files can't be pulled, those lines are the fallback, with medians and texel
CRCs only.

A blend table is JSON in the `iterated-blend/1` format described in `blend.py`: the microcode words (whole, or by the
field names in `clb197.h`), the pass count, any extra blend state, and the blend it should compute as a formula such as
`src.rgb * dst.rgb + src.rgb * (1 - dst.a) + dst.rgb * (1 - src.a)`. `testdata/multiply_example.json` shows the format.
Its microcode follows the fake device's guess at what the instruction fields mean, which nobody has run on hardware.

## Why it is built this way

The app draws through a raw device with its own connection and GPU VM. The app's Metal device already holds the
process's NVK VA window, and a channel the kernel kills during an experiment must not take the app down with it.
Programs replace the pixel program of a plain pack state, so the vertex program, the registers and the readback are
the ones the raw path already uses for Core Animation. No command buffer may have an estimated time over 50 ms, so a
program that hangs is caught by the kernel's stuck-channel rule (500 ms) and hang recovery, and the app needs no
timeout of its own. A kill is recorded in that experiment's result and the next experiment gets a fresh device.

On Maxwell, fixed-latency instructions don't interlock: the delay encoded with an instruction is the whole wait. A timed
chain of dependent FADDs therefore measures the delay written into it. The latency of those ops is the smallest delay at
which the chain still writes the right texel, which is why every timing experiment also returns its texels.

A readback experiment can bind colour 0 alone (`"targets": 1`) for the iterated blend unit, which rejected every draw
made with the template's two colour targets bound. It can also name the plain pack state to draw with (`"template"`, a
key hash). After every experiment the app submits one more empty pass: a trap in a lone command buffer can land after
its fence, and that pass never completes on a killed channel, so the experiment is reported as killed.

A kernel can write a buffer the result returns (`compute.output`): its address is the first 8 bytes of the shader
data. A 2D experiment can read a source the batch carries, resolve a multisampled image built from that source, run
nouveau's reference fill, clip to a rectangle, follow a clear-only pass in the same command buffer, or leave out the
waits the production path puts around 2D work. `compare` runs the same work a second way (the pixel program over the
rect, the copy engine, the pack's resolve state, the pack's mip state sampling the source, or a plain target for a
compressed one), and the result carries both and a histogram of their differences. A program whose bound TEX or TLD
names texture 0x1fff samples the experiment's source: the app draws it with the plainest state that has a sampled
texture and puts that texture's index in. A resolve with `byScale` reads its sample grid with the operation's own
sample mode, filter and origin instead of the driver's resolve. A result's `form` says what the app
did, so a cell proves its own setup. Unknown fields make an experiment invalid on both sides, and
`hw_test_app:core_test` decodes every generated batch with the app's own decoder.

The generated code follows two rules learned from traps on the device. A wait on a barrier must come at least two cycles
after the instruction that set it, or it passes before the barrier is up; `encode.barrier_hazards` refuses such code.
LDC has no 128-bit form on GM20B. Timed chains also loop a short body, since a lone warp on straight-line code longer
than a few KiB runs at instruction-fetch speed.

## When something is wrong

- `doctor` finds no device: iOS isn't running, or the Switch isn't paired. Unlock it and tap Trust.
- `no done.json after N s`: the Bench screen isn't open, the app was suspended, or the batch went to a folder the app
  isn't watching. While the Bench screen is open the app keeps `status.json` in every folder it watches, and
  `afcclient ls /Switch/benchloop` shows whether the Media folder has one.
- Self-check `FAIL ... no raw device`: the app couldn't open its own connection to the GPU. The line gives the reason.
- `refused` results name the draw predicate or state the raw path turned down. `killed` results carry the kernel's
  error code and fault address, and the kernel log has the decoded hang.
- `invalid` results never reached the GPU. Their `why` names the field that failed.
