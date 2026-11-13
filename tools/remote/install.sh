#!/bin/sh
# Installs (or updates) this Mac's remote-execution worker as two LaunchAgents and starts them.
#
#   install.sh <dir with bb_worker and bb_runner> <frontend host:port> <scheduler host:port> <cache GiB> <actions> [floor GiB]
#
# cache.token (aud bazel-cache) and worker.token (aud bazel-worker) must already be in the install directory.
set -eu

bin=${1:?usage: install.sh <bb binaries dir> <frontend> <scheduler> <cache GiB> <concurrent actions>}
frontend=${2:?}
scheduler=${3:?}
cache_gib=${4:?}
concurrency=${5:?}
# Free GiB under which the worker takes no work. Leave it above the cache's room to grow plus the agents' own floor.
floor_gib=${6:-20}

here=$(cd "$(dirname "$0")" && pwd)
home="$HOME/Library/Application Support/switch-ios/bazel-worker"
work="$HOME/Library/Caches/switch-ios-worker"
agents="$HOME/Library/LaunchAgents"
uid=$(id -u)
host=$(scutil --get LocalHostName)

for token in cache.token worker.token; do
    [ -s "$home/$token" ] || {
        echo "no $home/$token" >&2
        exit 1
    }
done
mkdir -p "$home" "$agents"
# A new file, never one written over: macOS kills a binary whose signed pages changed under the same vnode.
for file in "$bin/bb_worker" "$bin/bb_runner" "$here/switch-worker" "$here/worker.jsonnet" "$here/runner.jsonnet"; do
    name=$(basename "$file")
    cp "$file" "$home/.$name.new"
    mv -f "$home/.$name.new" "$home/$name"
done
chmod 600 "$home/cache.token" "$home/worker.token"

plist() {
    role=$1
    # The runner and the worker stay up; the floor check runs every minute.
    if [ "$role" = floor ]; then
        schedule='<key>StartInterval</key><integer>60</integer>'
    else
        schedule='<key>KeepAlive</key><true/>'
    fi
    cat >"$agents/com.brunostjohn.switch.bazel-$role.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>Label</key>
	<string>com.brunostjohn.switch.bazel-$role</string>
	<key>ProgramArguments</key>
	<array>
		<string>/bin/sh</string>
		<string>$home/switch-worker</string>
		<string>$role</string>
	</array>
	<key>EnvironmentVariables</key>
	<dict>
		<key>SWITCH_WORKER_DIR</key>
		<string>$work</string>
		<key>SWITCH_WORKER_FRONTEND</key>
		<string>$frontend</string>
		<key>SWITCH_WORKER_SCHEDULER</key>
		<string>$scheduler</string>
		<key>SWITCH_WORKER_CACHE_GIB</key>
		<string>$cache_gib</string>
		<key>SWITCH_WORKER_CONCURRENCY</key>
		<string>$concurrency</string>
		<key>SWITCH_WORKER_HOST</key>
		<string>$host</string>
		<key>SWITCH_WORKER_FLOOR_GIB</key>
		<string>$floor_gib</string>
	</dict>
	<key>RunAtLoad</key>
	<true/>
	$schedule
	<key>Nice</key>
	<integer>5</integer>
	<key>StandardErrorPath</key>
	<string>$work/$role.log</string>
</dict>
</plist>
EOF
    launchctl bootout "gui/$uid/com.brunostjohn.switch.bazel-$role" 2>/dev/null || true
    # bootout returns before the job is gone, and bootstrapping it again before then fails.
    while launchctl print "gui/$uid/com.brunostjohn.switch.bazel-$role" >/dev/null 2>&1; do
        sleep 0.2
    done
    launchctl bootstrap "gui/$uid" "$agents/com.brunostjohn.switch.bazel-$role.plist"
}

mkdir -p "$work"
plist floor
plist runner
plist worker
echo "worker installed: $home; build directory and cache: $work"
