#!/bin/sh
# Installs (or updates) Buildbarn on a Mac build host as LaunchDaemons running as the invoking user, and starts them.
#
#   install-host.sh <dir with bb_storage bb_scheduler bb_worker bb_runner> <name> <concurrent actions|auto> [first host's private IP]
#
# Without a first host's IP this is the first host: bb_storage (CAS + action cache), bb_scheduler, bb_worker and
# bb_runner, listening on 127.0.0.1 only (clients come in through an SSH forward). With one, it is an extra host that
# runs only bb_worker and bb_runner against the first host's storage and scheduler over the private network; the first
# host then needs its own private IP in SWITCH_BB_PRIVATE_IP when it is (re)installed, so it listens there too.
# auto runs one action per 2 GiB of memory.
set -eu

bin=${1:?usage: install-host.sh <bb binaries dir> <name> <concurrent actions|auto> [first host private IP]}
name=${2:?}
concurrency=${3:?}
first=${4:-}

here=$(cd "$(dirname "$0")" && pwd)
user=$(id -un)
home="$HOME/Library/Application Support/switch-ios/buildbarn"
data="$HOME/Library/Caches/switch-ios-buildbarn"
# The worker's build and cache directories: the instance store setup-volume.sh prepares, where the host has one.
work=${SWITCH_BB_WORKER_DIR:-$data/worker}
daemons=/Library/LaunchDaemons
prefix=com.brunostjohn.switch.bb

if [ "$concurrency" = auto ]; then
    concurrency=$(($(sysctl -n hw.memsize) / 2147483648))
fi

scheduler=127.0.0.1:8983
binaries="bb_storage bb_worker bb_runner"
if [ -n "$first" ]; then
    roles="runner worker"
    storage=127.0.0.1:8981
    listen="'127.0.0.1'"
else
    binaries="$binaries bb_scheduler"
    roles="storage scheduler runner worker"
    storage=127.0.0.1:8980
    listen="'127.0.0.1'"
    [ -n "${SWITCH_BB_PRIVATE_IP:-}" ] && listen="$listen, '$SWITCH_BB_PRIVATE_IP'"
fi

mkdir -p "$home" "$work/build" "$work/cache"
for store in cs cms ac; do
    mkdir -p "$data/storage/$store/persistent_state"
done
touch "$data/.metadata_never_index"

# A new file, never one written over: macOS kills a binary whose signed pages changed under the same vnode.
for file in $binaries switch-buildbarn storage.jsonnet proxy.jsonnet scheduler.jsonnet worker.jsonnet runner.jsonnet; do
    case $file in
        bb_*) source=$bin/$file ;;
        *) source=$here/$file ;;
    esac
    cp "$source" "$home/.$file.new"
    mv -f "$home/.$file.new" "$home/$file"
done

{
    echo "{"
    echo "  name: '$name',"
    echo "  dataDir: '$data',"
    echo "  listenAddresses: [$listen],"
    echo "  storageAddress: '$storage',"
    [ -n "$first" ] && echo "  upstreamStorageAddress: '$first:8980',"
    echo "  schedulerAddress: '$scheduler',"
    echo "  casGiB: 400,"
    echo "  workerCacheGiB: 100,"
    echo "  concurrency: $concurrency,"
    echo "}"
} >"$home/.host.libsonnet.new"
mv -f "$home/.host.libsonnet.new" "$home/host.libsonnet"

stop() {
    sudo launchctl bootout "system/$1" 2>/dev/null || true
    # bootout returns before the job is gone, and bootstrapping it again before then fails.
    while sudo launchctl print "system/$1" >/dev/null 2>&1; do
        sleep 0.2
    done
}

# The worker first: bootout gives it its ExitTimeOut to finish the actions it runs, and everything those need (the
# runner, the storage it uploads to) stays up until it has.
for role in worker runner scheduler storage proxy relay-8983 relay-8980; do
    stop "$prefix-$role"
done
sudo rm -f "$daemons/$prefix-relay-8980.plist"

# macOS keeps a non-Apple binary in a user's LaunchDaemon off the local network (Local Network privacy: "no route to
# host"), and a root worker would own the output directories its actions must chmod. So an extra host's worker runs as
# the user and reaches the first host through loopback: its CAS through bb_storage running as root (exempt), passing
# everything through; the scheduler's worker port through launchd handing each connection to /usr/bin/nc (an Apple
# binary, exempt), which is too slow for the CAS but enough for Synchronize.
if [ -n "$first" ]; then
    sudo tee "$daemons/$prefix-relay-8983.plist" >/dev/null <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>Label</key>
	<string>$prefix-relay-8983</string>
	<key>ProgramArguments</key>
	<array>
		<string>/usr/bin/nc</string>
		<string>$first</string>
		<string>8983</string>
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
			<string>8983</string>
			<key>SockType</key>
			<string>stream</string>
			<key>SockFamily</key>
			<string>IPv4</string>
		</dict>
	</dict>
</dict>
</plist>
EOF
    sudo tee "$daemons/$prefix-proxy.plist" >/dev/null <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>Label</key>
	<string>$prefix-proxy</string>
	<key>ProgramArguments</key>
	<array>
		<string>/bin/sh</string>
		<string>$home/switch-buildbarn</string>
		<string>proxy</string>
	</array>
	<key>RunAtLoad</key>
	<true/>
	<key>KeepAlive</key>
	<true/>
	<key>ProcessType</key>
	<string>Interactive</string>
	<key>SoftResourceLimits</key>
	<dict>
		<key>NumberOfFiles</key>
		<integer>65536</integer>
	</dict>
	<key>StandardOutPath</key>
	<string>$data/proxy.log</string>
	<key>StandardErrorPath</key>
	<string>$data/proxy.log</string>
</dict>
</plist>
EOF
    sudo launchctl bootstrap system "$daemons/$prefix-relay-8983.plist"
    sudo launchctl bootstrap system "$daemons/$prefix-proxy.plist"
fi

for role in $roles; do
    exit_timeout=
    # An hour, the scheduler's default action timeout: long enough for any action the worker is running to finish.
    [ "$role" = worker ] && exit_timeout="	<key>ExitTimeOut</key>
	<integer>3600</integer>"
    sudo tee "$daemons/$prefix-$role.plist" >/dev/null <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>Label</key>
	<string>$prefix-$role</string>
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
$exit_timeout
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
    sudo launchctl bootstrap system "$daemons/$prefix-$role.plist"
done
echo "installed $roles ($concurrency actions): $home; data: $data"
