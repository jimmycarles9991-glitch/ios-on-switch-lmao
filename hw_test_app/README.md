# hw_test_app

A SwiftUI app that tests the Switch's hardware through public iOS APIs: Metal, the hardware video decoders and
encoders, images, audio, display, sensors, input, networking, storage, CPU and crypto. It runs the whole suite once
at launch. Debug images only: the debug `//:boot` puts it in `/Applications` through the overlay, and
`bazel run //deploy:sd` leaves datamigrator's migration sentinel when it changes, so LaunchServices registers it on the
next boot the way it registers an OS update's apps. `bazel build //hw_test_app:SwitchHWTest` builds it with
rules_apple, signed ad hoc. The test clips and images are pinned in `bazel/pinned/hw_test_app/media`; every frame
carries a barcode with its frame number and a CRC, so the app checks lossy decoder output exactly.
`bazel run //hw_test_app:media_tool -- <dir>` remakes them (VideoToolbox on the Mac for H.264 and HEVC, libvpx for
VP9, libjpeg-turbo for the JPEGs) and prints the SHA256SUMS lines to pin; the Mac's 4K encodes differ from run to run,
which is why the files are pinned rather than built. Each test logs one `HWTEST <id> <PASS|FAIL|SKIP> <k=v ...>` line
to os_log under `com.brunostjohn.switch.hwtest` (`//tools/devicerun` checks them) and lands in `report.json` in the
app's Documents folder. Tapping a test in the list reruns it.

## GPU, compositor and pacing scenes

The `gpu.*` tests are offscreen scenes (`gpu/scenes`): each draws, reads every result back and compares it with a
reference computed on the CPU (`core/`). On the Switch each one runs twice: on the process's default Metal device
(the raw GM20B facade) and on the m2v device that facade wraps. Fields prefixed `raw_` and `m2v_` belong
to those two runs. `raw_paths=` lists the path each command buffer took, as `raw`, `m2v:<reason>:<predicate>` or
`split:…`. `raw_cbs`, `m2v_cbs` and `m2v_why` are the facade's process-wide counters over the run; they come from
`switchRawCounters` and `switchM2VDevice` in `RawMetalDevice.mm` and `switchRawPath` in `RawMetalCommandBuffer.mm`,
which only debug builds have. The app's
own shaders aren't in the raw pack, so draws with them always go to m2v. Clears, blits, uploads and the `gpu.ca.*`
scenes, which build Core Animation's own pipeline states from QuartzCore's library, are what reach raw. A failed
comparison names the first wrong pixel, both values, the bounding box, an 8x8 hex map of the wrong area and any streak
columns, and saves got, want and diff PNGs under `Documents/gpu/<id>/`.

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
