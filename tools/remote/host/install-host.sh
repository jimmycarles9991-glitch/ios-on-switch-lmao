#!/bin/sh
# Installs (or updates) Buildbarn on a Mac build host as LaunchDaemons running as the invoking user, and starts them.
#
#   install-host.sh <dir with bb_storage bb_scheduler bb_worker bb_runner> <name> <concurrent actions> [first host's private IP]
#
# Without a first host's IP this is the first host: bb_storage (CAS + action cache), bb_scheduler, bb_worker and
# bb_runner, listening on 127.0.0.1 only (clients come in through an SSH forward). With one, it is an extra host that
# runs only bb_worker and bb_runner against the first host's storage and scheduler over the private network; the first
# host then needs its own private IP in SWITCH_BB_PRIVATE_IP when it is (re)installed, so it listens there too.
set -eu

bin=${1:?usage: install-host.sh <bb binaries dir> <name> <concurrent actions> [first host private IP]}
name=${2:?}
concurrency=${3:?}
first=${4:-}

here=$(cd "$(dirname "$0")" && pwd)
user=$(id -un)
home="$HOME/Library/Application Support/switch-ios/buildbarn"
data="$HOME/Library/Caches/switch-ios-buildbarn"
# The worker's build and cache directories: the instance store setup-volume.sh prepares, where the host has one.
work=${SWITCH_BB_WORKER_DIR:-$data/worker}

if [ -n "$first" ]; then
    roles="runner worker"
    storage=127.0.0.1:8980
    scheduler=127.0.0.1:8983
    listen="'127.0.0.1'"
else
    roles="storage scheduler runner worker"
    storage=127.0.0.1:8980
    scheduler=127.0.0.1:8983
    listen="'127.0.0.1'"
    [ -n "${SWITCH_BB_PRIVATE_IP:-}" ] && listen="$listen, '$SWITCH_BB_PRIVATE_IP'"
fi

mkdir -p "$home" "$work/build" "$work/cache"
for store in cs cms ac; do
    mkdir -p "$data/storage/$store/persistent_state"
done
touch "$data/.metadata_never_index"

# A new file, never one written over: macOS kills a binary whose signed pages changed under the same vnode.
for file in "$bin/bb_storage" "$bin/bb_scheduler" "$bin/bb_worker" "$bin/bb_runner" "$here/switch-buildbarn" \
    "$here/storage.jsonnet" "$here/scheduler.jsonnet" "$here/worker.jsonnet" "$here/runner.jsonnet"; do
    base=$(basename "$file")
    cp "$file" "$home/.$base.new"
    mv -f "$home/.$base.new" "$home/$base"
done

cat >"$home/.host.libsonnet.new" <<EOF
{
  name: '$name',
  dataDir: '$data',
  listenAddresses: [$listen],
  storageAddress: '$storage',
  schedulerAddress: '$scheduler',
  casGiB: 400,
  workerCacheGiB: 100,
  concurrency: $concurrency,
}
EOF
mv -f "$home/.host.libsonnet.new" "$home/host.libsonnet"

# macOS keeps a non-Apple binary in a user's LaunchDaemon off the local network (Local Network privacy: "no route to
# host"), and a root worker would own the output directories its actions must chmod. So an extra host's worker runs as
# the user and reaches the first host through loopback, where launchd hands each connection to /usr/bin/nc (an Apple
# binary, exempt).
if [ -n "$first" ]; then
    for port in 8980 8983; do
        label=com.brunostjohn.switch.bb-relay-$port
        sudo tee "/Library/LaunchDaemons/$label.plist" >/dev/null <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>Label</key>
	<string>$label</string>
	<key>ProgramArguments</key>
	<array>
		<string>/usr/bin/nc</string>
		<string>$first</string>
		<string>$port</string>
	</array>
	<key>inetdCompatibility</key>
	<dict>
		<key>Wait</key>
		<false/>
	</dict>
	<key>Sockets</key>
	<dict>
		<key>Listeners</key>
		<dict>
			<key>SockNodeName</key>
			<string>127.0.0.1</string>
			<key>SockServiceName</key>
			<string>$port</string>
			<key>SockType</key>
			<string>stream</string>
			<key>SockFamily</key>
			<string>IPv4</string>
		</dict>
	</dict>
</dict>
</plist>
EOF
        sudo launchctl bootout "system/$label" 2>/dev/null || true
        while sudo launchctl print "system/$label" >/dev/null 2>&1; do
            sleep 0.2
        done
        sudo launchctl bootstrap system "/Library/LaunchDaemons/$label.plist"
    done
fi

for role in $roles; do
    label=com.brunostjohn.switch.bb-$role
    plist=/Library/LaunchDaemons/$label.plist
    sudo tee "$plist" >/dev/null <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>Label</key>
	<string>$label</string>
	<key>UserName</key>
	<string>$user</string>
	<key>GroupName</key>
	<string>staff</string>
	<key>ProgramArguments</key>
	<array>
		<string>/bin/sh</string>
		<string>$home/switch-buildbarn</string>
		<string>$role</string>
	</array>
	<key>EnvironmentVariables</key>
	<dict>
		<key>HOME</key>
		<string>$HOME</string>
		<key>PATH</key>
		<string>/usr/bin:/bin:/usr/sbin:/sbin</string>
		<key>SWITCH_BB_WORKER_DIR</key>
		<string>$work</string>
		<key>SWITCH_BB_WORKER_FALLBACK_DIR</key>
		<string>$data/worker</string>
	</dict>
	<key>RunAtLoad</key>
	<true/>
	<!-- A clean exit (the worker's store not mounted yet) waits for StartOnMount instead of a restart loop. -->
	<key>KeepAlive</key>
	<dict>
		<key>SuccessfulExit</key>
		<false/>
	</dict>
	<key>StartOnMount</key>
	<true/>
	<!-- Without it launchd throttles a daemon's CPU and I/O as background work. -->
	<key>ProcessType</key>
	<string>Interactive</string>
	<key>SoftResourceLimits</key>
	<dict>
		<key>NumberOfFiles</key>
		<integer>65536</integer>
	</dict>
	<key>StandardOutPath</key>
	<string>$data/$role.log</string>
	<key>StandardErrorPath</key>
	<string>$data/$role.log</string>
</dict>
</plist>
EOF
    sudo launchctl bootout "system/$label" 2>/dev/null || true
    # bootout returns before the job is gone, and bootstrapping it again before then fails.
    while sudo launchctl print "system/$label" >/dev/null 2>&1; do
        sleep 0.2
    done
    sudo launchctl bootstrap system "$plist"
done
echo "installed $roles: $home; data: $data"
