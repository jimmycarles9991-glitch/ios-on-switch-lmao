# devicerun

Runs every device check the agents wrote against logs pulled from a running Switch and prints one table: check, owner,
verdict, expected, observed. Pull the logs over USB while the Switch is still up, then run the tool on the Mac:

```sh
cd ~/Library/Application\ Support/switch-ios/device-logs/A
idevicesyslog -u <udid> archive archive.tar --age-limit 1800 && tar -xf archive.tar  # rename the folder a.logarchive
idevicecrashreport -u <udid> -k -e crashes
bazel run //tools/devicerun -- "$PWD/a.logarchive" --boot-label A --crashes "$PWD/crashes" --tsv "$PWD/checks.tsv"
```

`bazel run` changes into the runfiles tree, so give absolute paths. From the repository root,
`python3 -m tools.devicerun.devicerun` takes the same arguments. A run over a 160 MB archive takes about a minute,
because `log show` has to read the whole archive once.

The checks live in `checks.py`, one entry per expectation. Its `source` field names the file that logs the line, so a
check can be traced back to its format string when that format changes. INFO rows are open issues and manual steps;
their counts are the baseline for the next fix. NO-DATA means the line never appeared. Usually the
feature wasn't exercised (no USB 3 device plugged in, for example), or a release kernel compiled the debug line out.
Use `--start` and `--end` to scope the launchd rates to the idle minutes. `--boot-label` (A, B tiled cache, K kprof)
prints that boot's manual steps first and drops the checks that only apply to another boot. `--boot-label U` prints the
USB boot's plug order instead.

The line above the table is HW Test's first red caused by a failed command buffer, and how many tests each predicate
reddened, read from the latest boot's HWTEST lines (`hwtest.first-red`, `hwtest.red.<predicate>`). Some HW Test tests
kill other clients' GPU work on purpose (`KILLS_OTHERS` in `hwtest.py`). When such a run reports `killed=` above 0,
`gm20b.failed` and `gm20b.killed` list another process's kill inside it (predicate `none`, code 1 to 3, no description)
as expected and leave it out of the verdict.

The `usb` group also asks `log show` for info and debug lines, because Apple logs its IOPort lines, and perhaps its
enumeration lines, at debug level. Only the `usb` checks see those lines; every other group counts what it counted
before. The `usb-layers` rows walk the host path from the PD attach to the class drivers and name the first layer that
failed. The IOPort and transport layers, the detach order and the controller's power rest on the driver's development
lines (the port found, each root port created, each transport added or removed, each power state change). A release
build reads NO-DATA for those, and the layers around them still decide. Pull with an `--age-limit` that reaches back to
the boot: some lines (the port found, the root ports, the host controller coming up, device mode's SMMU line) are logged
once per boot.

The `usb` group reads one boot. An archive pulled with a long age limit can also hold earlier boots, so the usb checks
look only at the last boot whose kernel version line is in the archive, or at the one `--boot <UUID prefix>` names. The
line above the table names that boot and lists the others.

For each configured USB device the checker follows its driver from the configuration on: the dext requested, launched
and started, then its HID device opened, or a kernel class driver taking it. The first step that never comes fails as
"driver never started", however briefly the device stayed attached. A keyboard that works only through the kernel HID
driver, after its DriverKit dext failed to launch, reads FALLBACK: named, but it does not fail the run.

## Frame weights

`frame_weights` lists the compiled states a captured frame spent its GPU time in, most first, beside their static
instruction counts. When the Mac posts `com.brunostjohn.switch.debug.frame-weights.<process>`, a debug build captures
that process's next four command buffers. The compositor is backboardd:

```sh
idevicenotificationproxy -u <udid> post com.brunostjohn.switch.debug.frame-weights.backboardd   # while the screen moves
# pull the archive as above, then:
bazel run //tools/devicerun:frame_weights -- "$PWD/a.logarchive" --pack <the build's pack.bin>
```

The capture runs each draw alone, starting from an idle GPU, so a state's time is its cost on its own, and states rank
by that number. Captured frames take longer than normal ones, so the report prints both times. Without `--pack`, or with
a pack `nakstat` can't read yet, the instruction columns are estimates marked `~`.
