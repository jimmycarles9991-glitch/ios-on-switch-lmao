// SPDX-License-Identifier: BSD-2-Clause-Patent
// avd_host's MPEG sessions (mpeg_session.c).
#ifndef MPEG_SESSION_H
#define MPEG_SESSION_H
#include <CoreFoundation/CoreFoundation.h>
#include <VideoToolbox/VideoToolbox.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// One result line; true when every frame came out without an error from a session on (or off) hardware. 'mp4v' over
// test/MPEG4Clip.h's clip A (Advanced Simple) or B (Simple); 'mp2v' over test/MPEG2Clip.h's clip A or 'mp1v' over
// its clip C, on hardware.
bool mpeg4_session_check(CFDictionaryRef spec, bool simple, bool wantHardware, char *line, size_t size);
bool mpeg2_session_check(CFDictionaryRef spec, bool mpeg1, char *line, size_t size);
// 'mp2v' clip A decoded with `flags`: true when every frame came out, in decode order, from a hardware session; the
// session's SwitchPictureRoutes for `keys` (-1: absent) read before it is invalidated.
bool mpeg2_session_order(CFDictionaryRef spec, VTDecodeFrameFlags flags, const char *const *keys, int32_t *values,
                         unsigned count, char *line, size_t size);

#endif
