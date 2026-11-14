#!/bin/sh
# Prepares an EC2 Mac's instance store (/Volumes/ephemeral0, local NVMe) for the worker's build and cache directories
# and prints the directory to pass to install-host.sh as SWITCH_BB_WORKER_DIR. Those directories churn through every
# input of every action: on the EBS root they held a 5,000-IOPS volume at its ceiling while fseventsd spent two cores
# logging the churn. The instance store survives a reboot; a stop/start empties it, which costs the worker its cache.
#
# To TCC the instance store is an external disk: the LaunchDaemons may write there only once bb_worker and bb_runner
# have Full Disk Access (System Settings > Privacy & Security). Without it they fail with "Operation not permitted".
set -eu

volume=/Volumes/ephemeral0
device=$(diskutil info "$volume" | awk -F': *' '/Device Node:/ {print $2}')
if [ ! -e "$volume/.fseventsd/no_log" ]; then
    sudo mkdir -p "$volume/.fseventsd"
    sudo touch "$volume/.fseventsd/no_log"
    # fseventsd reads no_log when the volume mounts.
    sudo diskutil unmount "$volume" >&2
    sudo diskutil mount "$device" >&2
fi
sudo mdutil -i off "$volume" >&2
user=$(id -un)
sudo chown "$user" "$volume"
touch "$volume/.metadata_never_index"
mkdir -p "$volume/buildbarn-worker/build" "$volume/buildbarn-worker/cache"
echo "$volume/buildbarn-worker"
