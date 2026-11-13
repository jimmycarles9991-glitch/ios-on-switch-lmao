# hw_test_app

A SwiftUI app that tests the Switch's hardware through public iOS APIs: Metal, the hardware video decoders and
encoders, images, audio, display, sensors, input, networking, storage, CPU and crypto.

The first launch on a boot runs every test that needs no person. If the latest run is unfinished, startup waits for
Continue run or Start new run. Continue keeps completed results; Start new run saves the old run as cancelled before
starting fresh. Other launches show this boot's last results until you tap Run All. Interactive runs the tests that need
someone to plug, press or pick something. Each run writes `Documents/hwtest/<boot uuid>-<stamp>.json` with `F_FULLFSYNC`
before every test and after it, so a run the app or the OS doesn't survive still keeps every result and the name of the
test that was running (`run.current`). If the app's process dies mid-run, choosing Continue records that test as a FAIL
with `crashed=1` and runs the rest. A test interrupted by closing/backgrounding the app runs again. Continuing after a
reboot puts the interrupted test last and records the new boot's identity alongside the original run. When a GPU test
times out, a one-fill probe checks the app's Metal device; if the probe hangs too, the later tests that draw are skipped
and named after the test that wedged it. `pull_logs` copies the reports off the card, and `//tools/hwtest` compares each
run with the ones before it.

One library, `HWTestKit` (`kit/` and `tests/`), holds the tests, the runner, the log lines and the report. Each iOS
version has an app that wraps it and adds the tests only that version has: `SwitchHWTest17` (`app17/`, built for iOS
17.4) and `SwitchHWTest27` (`app27/`, built for iOS 27, adding Liquid Glass, QuartzCore 27's glass states, Metal 4 and
VideoToolbox's frame processors). The apps use the compiler's default CPU for their deployment target. From iOS 26 on
that is an A12, so the 27 app carries LSE and FP16 code the A57 lacks, as any app built for iOS 27 does, and
`a64x.self` checks that the a64x loader translated it.

The apps only go on debug images. A kind's debug `//:boot` puts its app in `/Applications/SwitchHWTest.app` through
the overlay, so the manifest and `//tools/devicerun` name a single app. A debug iOS 27 image also gets
`SwitchHWTest27e.app`, the same app built for arm64e, which a64x translates whole. When an app changes,
`bazel run //deploy:sd` lists it for `switch-appregd`, which registers it on the next boot the way installd registers a
built-in app, with no data migration. `system.overlay_app_records` checks that each app's record matches the bundle on
disk. `bazel build //hw_test_app:SwitchHWTest17` (or `:SwitchHWTest27`,
`:SwitchHWTest27e`) builds an app with rules_apple, signed ad hoc.

The test clips and images are pinned in `bazel/pinned/hw_test_app/media`; every frame carries a barcode with its frame
number and a CRC, so the app checks lossy decoder output exactly.
`bazel run //hw_test_app:media_tool -- <dir>` remakes them and prints the SHA256SUMS lines to pin. It encodes H.264
and HEVC with the Mac's VideoToolbox, VP9 with libvpx, the JPEGs with libjpeg-turbo, and MPEG-4 Part 2, MPEG-2, MPEG-1
and MBAFF H.264 with the `ffmpeg` on PATH or in `$FFMPEG` (built with libx264). No encoder here codes field pictures,
so it writes the PAFF H.264 clip itself. The Mac's 4K encodes differ from run to run,
which is why the files are pinned rather than built. Each test logs one `HWTEST <id> <PASS|FAIL|SKIP> <k=v ...>` line
to os_log under `com.brunostjohn.switch.hwtest` (`//tools/devicerun` checks them) and lands in the run's report. The
summary line at the end of a run names the app's iOS version (`ios=`); devicerun reports
a test that only the other version's app has as N/A. Tapping a test in the list reruns it.

`cpu.clock` and `cpu.sustained` read each core's clock from a chain of dependent ADDs (`cpu/switch_cpu_clock.c`, one
cycle each on the A57) divided by the thread's own CPU time, so preemption doesn't lower the reading. `cpu.sustained`
keeps every core busy for 20 s and compares the last two seconds with the first. `gpu.latency` times 120 small
command buffers on each device: the CPU's encode, and commit to completed handler. `gpu.raw_share` runs last and
reads the facade's process-wide counters: how many command buffers went to raw, to m2v or split, and why.

## GPU, compositor and pacing scenes

The `gpu.*` tests are offscreen scenes (`gpu/scenes`, plus `gpu/scenes27` for the ones only iOS 27's app runs). Each
draws, reads every result back and compares it with a reference computed on the CPU (`core/`). Some pictures have no CPU
model, such as Core Animation's states with zero uniforms or a cubic filter. Those are compared with the same picture
drawn on a Mac's GPU with the same IPSW's QuartzCore (`goldens/scenes_<build>.txt`), using the 8x8 block means that
`gpu.ca.sweep` uses below, and each logs `golden_<picture>=exact|close|wrong|missing`. After a scene's picture changes,
`bazel run //hw_test_app:scene_goldens` (again with `--config=ios27`) redraws them. A scene's verdict comes from the
process's default Metal device, the raw GM20B facade, alone. While the facade still wraps an m2v device the scene also
runs there and logs `m2v_` fields and `m2v_same=` (how many pictures it drew in raw's bytes), but that run cannot fail
the test, and a build without m2v skips it. `raw_paths=` lists the path each command buffer took, as `raw`,
`m2v:<reason>:<predicate>` or `split:…`. `raw_cbs`, `m2v_cbs` and `m2v_why` are the facade's process-wide counters
over the run. They come from `switchRawCounters` and `switchM2VDevice` in `RawMetalDevice.mm` and `switchRawPath` in
`RawMetalCommandBuffer.mm`, which only debug builds have. A failed comparison names the first wrong pixel, both values,
the bounding box, an 8x8 hex map of the wrong area and any streak columns, and saves got, want and diff PNGs under
`Documents/gpu/<id>/`.

`gpu.ca.sweep` checks the raw path against a Mac's GPU too. It draws one Core Animation state of
each class in the raw pack's corpus (`ca_states.log`, cut from `precompile/corpus.log` at build time). It compares each
attachment with that state as the same IPSW's QuartzCore draws it on a Mac (`goldens/ca_<build>.txt`). The comparison
is the 8x8 block means of every channel, which another GPU's filtering and rounding move by a step or two and a wrong
blend, format or texture moves by far more. `bazel run //hw_test_app:ca_goldens` (again with `--config=ios27`) redraws
the reference after a harvest adds states; until then a new state counts as `no_golden`. Every state's verdict goes
to `Documents/gpu/gpu.ca.sweep/states.txt`, with PNGs or raw bytes of the wrong ones. Set `HWTEST_SWEEP_ALL=1` in the
app's environment to draw every corpus state instead of one per class.

`bazel test //hw_test_app:gpu_test` runs every scene on the Mac's GPU against the same references. Run it after
changing a scene or a reference, so a failure on the Switch can't come from a wrong reference. It's tagged manual
because CI's runners may have no usable GPU.

`ui.*` snapshot real views through the render server with `drawHierarchy` and compare them with Core Animation's CPU
rendering of the same layers in 4x4 blocks, so anti-aliasing differences pass and noise or streaks fail. Blurs have
no CPU picture, so `ui.blur` checks invariants instead. `pacing.*` animates a scene from a 60 Hz display link for 5 s
and records two series: the link's own intervals (`link_*`) and when the display actually showed a probe Metal
layer's frames (`present_*`). `hist=` counts intervals of 1, 2, 3 and 4+ frames. `display.wake_gpu` is interactive:
sleep and wake the display with the power button, then the test measures the first frame after waking and reruns
the first GPU scene.
