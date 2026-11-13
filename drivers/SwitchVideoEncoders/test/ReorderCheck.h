// SPDX-License-Identifier: BSD-2-Clause-Patent
// The reordering check: EncodeCheck.h's clip through a non-real-time H.264 session (CAVLC, key frames every 10
// pictures and one forced at 15), with AllowFrameReordering and MaxFrameDelayCount as given (-1: not set). The
// samples must come out in decode order, the n-th decoding at the n-th frame's presentation time (AVEReorder.h), the
// decode and presentation times apart exactly when there are B pictures; each picture's type (its slice parsed with
// AVD's H264Syntax.c, against the sample attachments) must follow the pattern AVEReorder.h describes for that many B
// pictures; Apple's decoder must give every frame back exact. On this Mac it puts them out in decode order even with
// temporal processing (its own encoder's B pictures too: 0 4 2 1 3), so display order is the HW Test's to check.
#ifndef REORDER_CHECK_H
#define REORDER_CHECK_H
#include <CoreFoundation/CoreFoundation.h>
#include <stdbool.h>
#include <stddef.h>

// `bPictures`: the B pictures between anchors the session should code (0: none).
bool reorder_check(CFDictionaryRef spec, int allowReordering, int maxFrameDelay, int bPictures, char *line,
                   size_t size);
// The same clip with reordering unset over an engine that refuses every B picture: the first group's two frames come
// out as unreferenced I pictures after their anchor, every later one as an IDR or P picture in display order, and all
// decode back exact.
bool reorder_refusal_check(CFDictionaryRef spec, char *line, size_t size);

#endif
