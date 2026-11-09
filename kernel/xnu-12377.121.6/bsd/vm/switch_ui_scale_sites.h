/*
 * SwitchUIScale's cache text sites, mapped by vm_unix.c's pagers: per IPSW kind, the subcache page each one patches,
 * the bytes it must find there, what it writes and the slot of the cache mapping that maps it. Free of kernel types so
 * kernel/tests can check each kind's table against that kind's cache mappings.
 */
#ifndef _VM_SWITCH_UI_SCALE_SITES_H_
#define _VM_SWITCH_UI_SCALE_SITES_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <vm/switch_ui_scale.h>

/*
 * SwitchUIScale: switch-ui-scale=<1|1.25|1.6> pixels per point on the 720x1280 panel (switch_ui_scale.h), every
 * pixel rendered natively. At 1 every site stays stock.
 *
 * At the panel's 209.8 ppi, 1.6 px/pt gives a point the physical size of a 9.7- to 12.9-inch iPad's (264 ppi at
 * scale 2: 132 points per inch). iPadOS has no fractional scale for the main display: MobileGestalt makes
 * main-screen-scale from the integer /chosen/display-scale, QuartzCore's -[CADisplayMode preferredScale] truncates
 * that answer (fcvtzu), and FrontBoardServices' main display modes take it. Display Zoom and More Space resample a
 * canvas of another size in the display pipe instead. From FrontBoardServices up the scale is a double:
 * -[FBSDisplayMode pointScale] is the mode's scaleOverride when that is nonzero (otherwise its preferredScale),
 * UIScreen.scale is the display configuration's pointScale, _UIRootWindow maps points to pixels with an affine scale
 * by UIScreen.scale under its rotation, and UIKit's layers rasterize at contentsScale = UIScreen.scale (QuartzCore
 * keeps a non-integer contents or rasterization scale in the layer's extension). A non-integer override therefore
 * draws every layer at the scale into the native framebuffer and nothing is resampled. Touches reach UIKit in
 * display pixels and are divided by UIScreen.scale (-[UIWindow _transformDisplayToWindowCoordinates:]).
 * iPadOS 27 is the same: only another display's modes get a scale override, from backboardd.
 *
 * Each kind's sites are in switch_ui_scale_sites_<build>.h; the kernel takes the selected kind's (SWITCH_US_CACHE).
 * MobileGestalt keeps its answer in its build-keyed cache; kern_exec.c drops that cache when the scale changes
 * (native_switch_ui_scale_cache).
 */
struct switch_us_edit {
	uint64_t fileoff;                           /* the first byte that must read as shipped */
	uint8_t length;                             /* bytes that must read as shipped */
	uint8_t at, size;                           /* the bytes replaced, from fileoff + at */
	enum switch_ui_scale_edit_kind kind;
	uint8_t stock[32];
	uint8_t patched[32];                        /* SWITCH_US_FIXED's */
};

struct switch_us_site {
	uint8_t uuid[16];
	uint64_t page_vmaddr;                       /* unslid; one 16 KB page */
	uint64_t page_fileoff;
	uint64_t chunk_offset;                      /* the 4 KB chunk within the page that holds every edit */
	unsigned slot;                              /* switch_shared_cache_text_pager()'s, one per site of a mapping */
	unsigned edits;
	struct switch_us_edit edit[2];
	const char *name, *applied;
	bool every_scale;                           /* applied at scale 1 too; SWITCH_US_FIXED edits only */
};

/* One IPSW kind's cache: the build it shipped in, its header's magic and its sites. */
struct switch_us_cache {
	const char *build;
	char magic[16];
	const struct switch_us_site *sites;
	size_t count;
};

#define SWITCH_US_PAGE_SIZE       0x4000ULL
#define SWITCH_US_MAX_SITES       16u

#include <vm/switch_ui_scale_sites_21H461.h>
#include <vm/switch_ui_scale_sites_24B119.h>

/* The kind the kernel is built for (//kernel's switch_ipsw_volumes.h); host tests take each kind's table directly. */
#if defined(SWITCH_IPSW_KIND_24B119)
#define SWITCH_US_CACHE           (&switch_us_cache_24B119)
#elif defined(SWITCH_IPSW_KIND_21H461)
#define SWITCH_US_CACHE           (&switch_us_cache_21H461)
#elif defined(SWITCH_IPSW_BUILD)
#error "no SwitchUIScale sites for this IPSW kind: add switch_ui_scale_sites_<build>.h"
#endif

/* Whether a cache text mapping (its unslid address, size and file offset) holds the site's page. */
static inline bool
switch_us_site_in_mapping(const struct switch_us_site *site, uint64_t unslid_address, uint64_t size,
    uint64_t file_offset)
{
	return unslid_address <= site->page_vmaddr &&
	       site->page_vmaddr + SWITCH_US_PAGE_SIZE <= unslid_address + size &&
	       file_offset + (site->page_vmaddr - unslid_address) == site->page_fileoff;
}

/* The site of the cache that slot `slot` of a cache text mapping maps, or NULL. */
static inline const struct switch_us_site *
switch_us_site_for_slot(const struct switch_us_cache *cache, unsigned slot, uint64_t unslid_address, uint64_t size,
    uint64_t file_offset)
{
	for (size_t i = 0; i < cache->count; i++) {
		const struct switch_us_site *site = &cache->sites[i];
		if (site->slot == slot && switch_us_site_in_mapping(site, unslid_address, size, file_offset)) {
			return site;
		}
	}
	return NULL;
}

#endif /* _VM_SWITCH_UI_SCALE_SITES_H_ */
