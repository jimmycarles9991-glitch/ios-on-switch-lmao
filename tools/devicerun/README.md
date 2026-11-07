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
prints that boot's manual steps first and drops the checks that only apply to another boot.
