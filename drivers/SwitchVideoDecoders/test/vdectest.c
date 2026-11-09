// SPDX-License-Identifier: BSD-2-Clause-Patent
// vdectest: the clip check (AVD/ClipCheck.c) as a command: test/VDECClip.h's H.264 clip decoded through VideoToolbox
// twice, once requiring a hardware decoder and once with hardware disabled, every output frame's luma CRC compared
// with x264's reconstruction in display order; then test/HEVCClip.h's HEVC clips (Main on hardware, Main 10 on
// hardware as 'x420' against the reference >> 2, Main in software) against FFmpeg's. On a Mac it checks the clip and the check itself against Apple's
// decoders (tests/test_switch_video.py). On the Switch it proves NVDEC's whole path, logged under com.brunostjohn.switch.avd.
#include <os/log.h>
#include <stdio.h>
#include "../AVD/ClipCheck.h"

int main(void)
{
    os_log_t log = os_log_create("com.brunostjohn.switch.avd", "vdectest");
    char line[512];
    int failed = 0;
    static const ClipCheckMode kModes[] = {ClipCheckHardware, ClipCheckSoftware, ClipCheckHEVC, ClipCheckHEVC10,
                                           ClipCheckHEVCSoftware};
    for (size_t i = 0; i < sizeof(kModes) / sizeof(kModes[0]); i++) {
        failed += !clip_check(kModes[i], NULL, line, sizeof(line));
        os_log(log, "vdectest: %{public}s", line);
        printf("vdectest: %s\n", line);
    }
    return failed != 0;
}
