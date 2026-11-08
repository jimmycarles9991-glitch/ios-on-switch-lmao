# Install image

`bazel run //:image` makes `out/sd/switchroot/ios/IOS17.SII.001`, `.002`, ...: one install image of iPadOS 17.7.11,
split only because FAT32 caps a file below 4 GiB. With `--config=ios27` it makes `IOS27.SII.*` beside it.
`bazel run //:image_to_card -- diskN --yes` copies the folder's images to the card along with
`bootloader/payloads/install-ios.bin` and the hekate entry. In hekate, run `install-ios.bin` from Payloads, pick an
image with the volume buttons and POWER, and it writes that image onto the card's Linux, Android or iOS partition and
makes the image's boot files the ones hekate's iOS entry starts. Both images stay on the card, so switching versions
is another run of the installer.

```sh
bazel run //:image                              # out/sd/switchroot/ios/IOS17.SII.*
bazel run //:image --config=ios27               # IOS27.SII.* beside it
bazel run //:image_dry_run                      # the plan, without the large IPSW downloads
bazel run //:image_to_card -- disk4             # what copying would write and remove
bazel run //:image_to_card -- disk4 --yes       # copy, then eject
bazel run //:image_to_card -- disk4 --direct --yes  # one image straight onto the iOS partition, no Switch step
bazel run //:image_boot                         # new boot files into an image, after a kernel-only change
```

No card has to be in the Mac. The container is composed for 1 TiB, and `install-ios.bin` fits it to the partition
after writing it (`boot/installer/apfs.c`). Any partition from the image's minimum up works: the highest block the
container uses, plus 4 GiB for iOS to run in, and never below 16 GiB. The build prints the minimum, and the installer
shows it and refuses smaller partitions. Before packing, the build makes that fit on an APFS clone of the container and
fails unless macOS attaches it and `fsck_apfs` passes it.

Each stage is kept with a key of what it was made from (`tools/install/keys.py`): compose, populate, pack, and the
head of `.001` (the manifest and boot files). An unchanged rerun computes the keys and stops. New boot files rewrite
only the head, and a changed overlay repopulates a clone of the kept container, `composed.img` in
`~/Library/Caches/switch-ios/image` (`image-<build>` for other builds). Nothing runs as root: populate writes iOS's
files as you and sets their owners in the detached image. For iOS 27, translating each volume and rebuilding the two
cryptexes are Bazel actions, cached like any other, so a rerun after a later failure translates nothing. They take
about 13 GB for iOS 27 (the System cryptex's delta and image alone 11 GB).
Each stage measures what it will write from its inputs and refuses to start unless that much is free with 2 GiB to
spare. A run first detaches whatever an earlier run left attached and deletes its partial files. Files left by older
runs under sudo belong to root, so it prints the `sudo rm -rf` that removes them.

`//:image_to_card` writes only what the card lacks, judged by the manifests' chunk hashes, so a kernel-only change
rewrites the head of `.001` and nothing else. Without `--yes` it shows the plan, a new `install-ios.bin` included, and
writes nothing.

`--direct` does the installer's work on the Mac instead (`tools/install/direct.py`): it writes one image onto the
iOS partition at the card's raw speed, without copying it onto the FAT first, fits it with the installer's own
`apfs_tool`, and runs `fsck_apfs`. It asks for your password because writing a raw disk needs root. The card stays in
the reader the whole time: macOS lets go of the old iOS container when the partition is retyped, and attaches the new
one at the end. Add `--image IOS27` when `out/sd` holds both images.

Every install writes a new container with a new Data volume, so nothing from an earlier install survives. The first
boot takes a few minutes longer while iOS finishes setting itself up behind its own progress bar under the Apple logo.

The card's `switchroot/ios/bl31.bin` and `bl33.bin` are what hekate starts. Only `install-ios.bin`, `--direct` and
`bazel run //deploy:sd` write them, and deploy refuses a card whose iOS is the other version. Copying an image never
changes what boots.

`--//install:prebuilt=release` takes this project's own parts from the GitHub release of `MODULE.bazel`'s version
instead of building them (`--repo_env=SWITCH_RELEASE_DIR=<dir>` for a local copy of the assets). The release carries
nothing of Apple's: the IPSW is downloaded and assembled only on your Mac.
