# hwtest

Compares HW Test runs (`hw_test_app`) across boots. `pull_logs` handles it: it copies every report from the app's data
container on the card into `~/Library/Application Support/switch-ios/device-logs/hwtest/`, writes a `.mac.json`
sidecar next to each new report, and prints the newest new run of each app against the runs before it. The same text
goes to `hwtest-summary.txt` in that pull's folder.

```sh
bazel run //tools/hwtest                      # the newest run of each app and iOS build in the history
bazel run //tools/hwtest -- /abs/path/run.json
bazel run //tools/hwtest -- --import-log "$PWD/hwtest-boot14.txt" --boot-log "$PWD/boot14.txt" --build 24B119 \
    --nak-debug sm53_probe                    # a run from before the app wrote reports, from its HWTEST lines
```

The pull also copies the app's `Documents/gpu` (the GPU scenes' got, want and diff PNGs) into `hwtest-gpu/`.

The sidecar holds what the device can't see from inside its sandbox. That's the commit `deploy` wrote to
`<Data>/Switch/deployed.json`, plus the crash reports for that boot (matched by `bootSessionUUID`, with the Core
Animation clients that aborted in `+[CADisplay displays]` counted apart), the panics the kernel saved after the run
started, the lockdown pair records, and the boot time: `kern.boottime` to backboardd's "Dismissing boot logo", read
from the pull's `boot.logarchive` (about 3 s of `log show`). Builds are never stamped, which is why the commit is
recorded at deploy time. A deploy made after the boot leaves the commit unknown.

A run is compared only with runs of the same bundle ID on the same iOS build. A regression is a test that passed last
time and fails now. A new failure fails now and didn't last time. A number counts as slower or faster when it moves
by more than 10% or twice the spread of the earlier runs, whichever is larger. Key names decide which way is better:
`ms`, `us`, `t` and `missed` tokens are times or losses, and `fps`, `gbps`, `gflops`, `gpix` and `*_mb_s` are rates. A
GPU test's numbers and verdict aren't compared across a change in `nak_debug` (the app's NAK_DEBUG) or in any
`*_gpu_clock` metric, because the same key then measures something else. An unfinished run is reported as stopped at
`run.current`, with HW Test's crash reports and any saved panic from that boot.
