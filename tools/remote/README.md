# Remote execution

Builds run their actions on a Mac build host: an EC2 Mac that runs Buildbarn's storage (the CAS and the action cache
on its own disk), scheduler, worker and runner. A worktree's output base then holds only what a command brings back,
so many worktrees build at once without each keeping gigabytes of intermediate files.

## Using it

Each Mac keeps an SSH forward to the host open (LaunchAgent `com.brunostjohn.switch.bazel-tunnel`, script
`~/Library/Application Support/switch-ios/bazel-cache/bazel-tunnel`). Through it, Bazel's cache and executor are
`grpc://127.0.0.1:18980`; the host's storage and scheduler diagnostics (`/-/healthy`, `/metrics`) are on 18990 and
18992. Every build on the Mac uses the host: its private `remote.bazelrc`, which `~/.bazelrc` imports, says so for
every command.

```text
build --config=remote
build:remote --remote_cache=grpc://127.0.0.1:18980
build:rbe --remote_executor=grpc://127.0.0.1:18980
build:rbe --noremote_local_fallback
build:rbe --noremote_upload_local_results
build:remote --disk_cache=
build:rbe --remote_download_outputs=minimal
build:rbe "--remote_download_regex=.*/(gpu/raw_stack\.txt|testlogs/.+/test\.(log|xml))$"
build:noremote --remote_cache= --remote_executor=
```

Nothing builds on the Mac behind a dead tunnel: the build fails within seconds with `Failed to query remote execution
capabilities: Connection refused: /127.0.0.1:18980`. With the local fallback on, the same case quietly ran everything
here. `--config=noremote` builds one command on the Mac without the host or any cache.

Outputs stay on the host. `bazel run` fetches what it runs and tests bring back their logs; a command or script that
reads `bazel-bin` adds `--remote_download_toplevel` (tools/emu does; the repro checker's config does).

A restarted bb_scheduler forgets its operations, and Bazel fails every action it had in flight with `NOT_FOUND:
Operation with name ... not found` instead of sending it again. Restart the scheduler and storage only while the
queue is empty. A worker drains on SIGTERM (`launchctl bootout`): it takes no new work and finishes what runs.

Repository rules (IPSW members, toolchains) still run on the Mac, and an action's inputs reach the host from the Mac
that first needs them, over the home uplink. The host's own client (`~/src/switch-ios`, `~/.bazelrc` pointing at
127.0.0.1:8980) fetches the same pinned files at AWS speed: building a commit there first puts its inputs in the CAS,
and the Macs then upload only what they changed.

## The build host

It needs Xcode at `/Applications/Xcode.app`, selected with `xcode-select`, byte for byte the Macs' (actions name that
path; the worker advertises its build as the `xcode` exec property), the Metal toolchain at the pinned version
(`xcodebuild -downloadComponent MetalToolchain -buildVersion <build>`), and Xcode registered with LaunchServices
(`lsregister -f -R /Applications/Xcode.app`): a headless Mac never registers it, and rules_swift's xcode-locator then
finds no Xcode.

1. Build `bb_storage` from `buildbarn/bb-storage`, and `bb_scheduler`, `bb_worker` and `bb_runner` from
   `buildbarn/bb-remote-execution` at the commit whose go.mod names that bb-storage, with `go build -trimpath`. On
   macOS this needs bb-storage's own patches that its Bazel build applies, `o-search.diff` for `golang.org/x/sys` and
   `service-registrar.diff` for `googleapis/bytestream` (go.mod `replace` lines to patched copies), and
   `bb_worker-clonefile.diff` from this directory. Upstream hard-links each input from the worker's cache into the
   action's directory, and macOS can report a hard-linked executable under any of its names. The hermetic Python and
   rustc find their libraries next to themselves, so they looked in the cache directory and failed. An APFS clone is
   a separate file that takes no space. Then `bb_worker-slots.diff`: each executor keeps its input tree between
   actions and fetches only what changed; a file an action wrote, chmodded or replaced is fetched again. The trees
   sit outside the cache's size limit.
2. Run `tools/remote/host/install-host.sh <dir with the four binaries> <name> <concurrent actions>`. It installs four
   LaunchDaemons that run as the invoking user (`com.brunostjohn.switch.bb-storage`, `-scheduler`, `-runner`,
   `-worker`) and starts them. Everything listens on 127.0.0.1 only: clients come in through SSH. The CAS (400 GiB)
   and the action cache (16 GiB) are block files under `~/Library/Caches/switch-ios-buildbarn/storage`, kept across
   restarts; their sizes are fixed at first start, and changing one discards that store.

Logs are `~/Library/Caches/switch-ios-buildbarn/<service>.log`. `sudo launchctl kickstart -k
system/com.brunostjohn.switch.bb-<service>` restarts one; restarting the worker empties its input cache.

What sets the host's speed, measured on 14 Nov (10-core M4, 16 executors):

- Gatekeeper. Every action gets new APFS clones of its tools, and macOS assesses each new executable or library the
  first time it runs (syspolicyd and XProtect, one at a time, 0.4-2 s each): about 2 actions a second with half the
  CPU idle. The host runs with assessments off (`spctl --global-disable`, confirmed once in System Settings over
  Screen Sharing through an SSH forward).
- IOPS. Creating and deleting every input of every action held the 5,000-IOPS root volume at its ceiling; at 16,000
  IOPS the host does about 4 actions a second, CPU-bound, half of it in the kernel (bb_worker and fseventsd).
  `setup-volume.sh` moves that churn to the instance store once the daemons have Full Disk Access.
- Reuse. Each executor keeps its input tree between actions (`bb_worker-slots.diff`), so files that did not change
  stay the same files: a third of the scans, worker and fseventsd time, and 40-58% more actions a second.
- Developer Tools. XProtect still scans each new executable with Gatekeeper off; bb_runner in Privacy & Security >
  Developer Tools exempts the actions it starts.

Full Disk Access and Developer Tools are granted to the binaries' code hashes: a rebuilt bb_worker or bb_runner needs
both again.

- Automatic macOS updates are off, so the host never installs one and reboots in the middle of a build.

Bluetooth has no working controller on an EC2 Mac and bluetoothd restarts every ten seconds, which keeps audiomxd and
configd at about a core between them; SIP does not let launchd disable it.

`@gpu_device_data` comes from the host's own `~/Library/Application Support/switch-ios/gpu-device-data/` (gpu/deps.bzl).
A client without the pinned tar there gives the seed's probe actions other inputs and keys, so every Mac and the host
keep the same tar.

### Another host

An extra host runs only the worker and the runner against the first host's storage and scheduler over the private
network: reinstall the first host with `SWITCH_BB_PRIVATE_IP=<its private IP>` in the environment, so its storage (8980)
and the scheduler's worker port (8983) also listen there, and run
`install-host.sh <bins> <name> <actions> <first host's private IP>` on the new one. The security group then needs TCP
8980-8983 from itself. Those ports take no credentials. macOS keeps a non-Apple binary in a user's LaunchDaemon off the
local network ("no route to host"), so the extra host's worker talks to 127.0.0.1, where launchd hands each connection
to `/usr/bin/nc` (exempt) relaying to the first host. Running the worker as root instead would make the output
directories root's, and actions that chmod their own output directory (the native seed) fail.

## The cluster cache (until 14 Nov)

Buildbarn's frontend, scheduler and storage also run in the homelab cluster (`k8s/bazel-cache`), with the Mac Studio
as a worker (`install.sh`, `worker.jsonnet`, `runner.jsonnet`, `switch-worker` here; tokens in
`~/Library/Application Support/switch-ios/bazel-worker/`). To go back to it, point the private `remote.bazelrc` and
`bazel-cache-toggle` at `10.0.0.10:30980` again; both files keep the old lines as comments. To stop the Studio's
worker, boot out its three jobs: `launchctl bootout gui/$(id -u)/com.brunostjohn.switch.bazel-worker`, then the same
for `-runner` and `-floor`.
