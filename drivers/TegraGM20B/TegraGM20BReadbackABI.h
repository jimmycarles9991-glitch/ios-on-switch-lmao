/* SPDX-License-Identifier: MIT */
/* The scanout read-back, debug kernels only (a release kernel answers kIOReturnUnsupported): a call on a TegraGM20B
 * connection (TegraGM20BABI.h's wire form, direct or tunnelled) through which hw_test_app's display.pocket_strip
 * (tests/ScanoutReadback.swift) gets what the display shows. Kept out of TegraGM20BABI.h, which NVK and the GPU seed
 * build from: the method number is far from theirs.
 *
 * Arm: the next frame the display hands the GPU present (TegraDisplayFlip/TegraScanoutCopy.h) is also copied, on the
 * present channel after the frame's own copy and before its fence, pitch-linear into two kernel buffers: the swapped
 * IOSurface (the compositor's framebuffer, before the display's copy) and the scanout buffer that copy wrote for window
 * A (on direct scanout, where window A scans the swapped surface itself, that surface again). Armed with `reserved` 1
 * (live), the second buffer holds instead what window A scanned for the frame offered before this one (that frame's
 * surface on direct scanout, else the scanout buffer its copy wrote), read at this present, which is the end of that
 * frame's time on screen: compared with that frame's own read-back, any difference was written while it was shown.
 * State: where the read-back is; Done, each buffer is mapped with method 12 (TegraGM20BAbiMap) by its token, and holds
 * height rows of width * 4 bytes, packed, as window A reads them (B8G8R8A8). Disarm: the buffers go. One read-back
 * system-wide, the arming connection's; it goes with that connection. */
#ifndef TEGRA_GM20B_READBACK_ABI_H
#define TEGRA_GM20B_READBACK_ABI_H
#include <stdint.h>

enum {
	TegraGM20BMethodReadback = 0x5242, /* 'RB' */

	TegraGM20BReadbackDisarm = 0,
	TegraGM20BReadbackArm = 1,
	TegraGM20BReadbackQuery = 2,

	/* TegraGM20BAbiReadbackReply.state */
	TegraGM20BReadbackIdle = 0,
	TegraGM20BReadbackArmed = 1,    /* waiting for the display's next GPU present */
	TegraGM20BReadbackInFlight = 2, /* its present is queued */
	TegraGM20BReadbackDone = 3,
	TegraGM20BReadbackFailed =
		4, /* status: the present's (TEGRA_SCANOUT_SOURCE_LOST) or an errno */

	/* TegraGM20BAbiReadbackReply.flags */
	TegraGM20BReadbackDirect = 1u
		<< 0, /* window A scans the swapped surface: both buffers hold it */
	TegraGM20BReadbackSourceBlock = 1u << 1, /* the swapped surface is block-linear */
	TegraGM20BReadbackPrevious = 1u
		<< 2, /* live: the second buffer holds the frame offered before (previous_sequence) */
};

struct TegraGM20BAbiReadback {
	uint32_t op;       /* TegraGM20BReadback{Disarm,Arm,Query} */
	uint32_t reserved; /* Arm: 1 live, else 0 */
};

struct TegraGM20BAbiReadbackReply {
	int32_t state;
	int32_t status;
	uint32_t width, height; /* pixels of the frame read */
	uint32_t flags;
	uint32_t framebuffer_token; /* Done: method 12 tokens */
	uint32_t scanout_token;
	/* TegraGM20BReadbackPrevious: its frame's offer sequence, low 32 bits */
	uint32_t previous_sequence;
	uint64_t sequence; /* the display's offer sequence of the frame read */
};

#ifdef __cplusplus
static_assert(sizeof(TegraGM20BAbiReadback) == 8, "TegraGM20BAbiReadback layout");
static_assert(sizeof(TegraGM20BAbiReadbackReply) == 40, "TegraGM20BAbiReadbackReply layout");
#endif
#endif
