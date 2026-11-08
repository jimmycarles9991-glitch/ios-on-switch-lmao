# install-ios.bin

A hekate payload that writes one of the install images on the FAT (`switchroot/ios/NAME.SII.*`, from
`bazel run //:image`) onto the card's iOS partition, fits the image's container to that partition, and makes the
image's boot files the ones hekate's iOS entry starts. It runs on the BPMP with hekate's BDK. hekate is GPL-2.0-only,
and so is this payload.

```sh
bazel build //boot/installer:install-ios.bin
bazel test //boot/installer/test:all
bazel test --strategy=TestRunner=local //boot/installer/test:apfs_fit_test   # the fit against macOS's APFS
```

It lists every `switchroot/ios/*.SII.001` it finds with its label, size and minimum partition. An image whose manifest
doesn't decode or that is missing a part is listed with the reason and can't be picked. VOL+ and VOL- move between the
rest and POWER picks one. The target is the card's Linux (`l4t`) or Android (`userdata`, `UDA`) partition, or the iOS
partition an earlier install made, and it must be at least the image's minimum. A warning screen follows: POWER
installs (it ignores buttons for the first second), VOL+ or VOL- goes back. Then:

1. It reads the image's boot files and checks them against the manifest's SHA-256s. Nothing has been written yet.
2. The GPT entry becomes Linux-typed `iOS-setup`, so a half-written partition never boots or gets offered to
   Linux or Android.
3. The boot files go into `switchroot/ios/`, each 8 MiB chunk checked before it's written and read back after.
   `bootargs.txt` is never touched.
4. It reads the container 8 MiB at a time, and the security engine checks each chunk's SHA-256 before any of it is
   written. The checkpoint descriptor area is written whole, because APFS scans it for the newest superblock. The
   partition's other blocks keep their old bytes, which the new container never reads.
5. `apfs.c` fits the container to the partition, the way `diskutil apfs resizeContainer` shrinks one, but in place.
   It checks everything first and writes the bitmaps first and the superblocks last. Then it measures the container
   again from the card.
6. The entry becomes APFS `iOS`, the name the kernel looks for. The image stays on the FAT for the next switch.

The container is composed for 1 TiB. The fit sets what newfs_apfs and the kernel expect for the new size: the
internal pool at three blocks per chunk and per chunk-info block, its free queue's node limit, and `max_fs`. Those
values were measured on macOS 27 and are written up at the top of `apfs.c`.

Every SD write goes through `__wrap_sdmmc_storage_write`, so FatFs's and the BDK's writes go there too. It allows
only the range the current step needs: the GPT's four ranges for steps 2 and 6, the FAT for step 3, the partition for
steps 4 and 5. Everything else is refused. The screen is drawn 1280 x 720, upright when the Switch is held in handheld
mode.
