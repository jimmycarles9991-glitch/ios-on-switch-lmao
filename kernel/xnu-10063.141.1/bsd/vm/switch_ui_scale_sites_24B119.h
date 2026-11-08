#ifndef _VM_SWITCH_UI_SCALE_SITES_24B119_H_
#define _VM_SWITCH_UI_SCALE_SITES_24B119_H_

/*
 * SwitchUIScale's sites in 24B119's arm64e dyld cache (switch_ui_scale_sites.h), as the image ships it: translated
 * offline (translate/arm64e/pretranslate), which keeps every subcache's UUID and mappings and leaves each edit's bytes
 * as Apple's; the kernel maps those files without the a64x pager, so the pages patched are the translated ones. One
 * 4 KB chunk of cache text per site, applied all or nothing per site:
 * - FrontBoardServices (.11), -[FBSDisplayConfiguration _initWithImmutableDisplay:originalDisplay:isMainDisplay:
 *   assertIfInvalid:] + 0x180: 21H461's site, the same seven instructions (movi v8.2d, #0 for fmov d8, xzr).
 * - libMobileGestalt (.13), MGIOMFBSupport.c's static screen info: 21H461's site. 27 still reads /chosen/display-scale
 *   as an integer and converts it (scvtf), so the ADT has no way to give a fractional answer.
 * - CoreUI (.15), the PDF and SVG vector rasterizers: 21H461's row bytes, unchanged in 27 (both now in d12/d11).
 * - ProgressUI (.65), -[PUIProgressWindow _layoutScreen]'s 239 x 4 point bar, the iPad (default) case: 21H461's site.
 * - CoverSheetKit (.13), +[CSDeviceDetermination categoryFromReferenceBounds:], the device row of every lock screen
 *   metric. Without a Home button (home-button-type 2) an iPad's row comes from its reference height: over 1366, 1210,
 *   1194, 1180 or 1080 points, then 744x1133, then 0x21 over 1024 points, 0x1e for exactly 768x1024 and otherwise
 *   0x22, no device's row. 576x1024 and 450x800 got 0x22; the tbz after the 1024 test becomes a nop, so they take
 *   0x21, the row of J181's own height class (J181, with its Home button, is 0x18 over 1024 points). At 1 the screen
 *   is 1280 points tall (0x1f).
 * - UIKitCore (.01), _UIKeyboardComputeKeyboardIdiomFromScreenTraits' iPad table {768x1024: 26, 810x1080: 25,
 *   834x1112: 24, 820x1180: 24, 834x1194: 24, 954x1366: 23}: a screen at least as large as an entry (width plus
 *   height) takes its idiom unless it is that exact size, so J181's 810x1080 is idiom 26 and 576x1024 and 450x800,
 *   smaller than all, stay the standard class (1), whose layouts are 768 or 1024 points wide and are not derived to
 *   the screen. The first entry's size becomes 0x0: every smaller screen is idiom 26, J181's class, whose keyboard
 *   _UIKeyboardGetDerivedKeyboardForSpecificOrientationAndLayoutClass derives to the screen's width. The table's one
 *   reader is that function; 720x1280 (scale 1) is 24 either way.
 * 21H461's other sites are not here. 27's -[_UIBatteryView _fillLayerFrame:cornerRadius:] fills the body's own
 * frame (no inset), so SpringBoard's and Control Center's batteries have no gap to even out. +[PKIconImageCache
 * settingsIconCacheWithScale:] returns nil at every scale and 24B119 ships no iconCache artwork. The iPad switcher's
 * page scale moved to -[SBGridLayoutSwitcherModifier scale] (-[SBMixedGridSwitcherModifier _fullScreenCardScale] is
 * App Exposé's now), the home screen's grid to a table of 120 screen types (_getRootMetrics), and UIKit's own status
 * bar battery insets through UIRectInsetEdges, a call with no room beside it: not ported.
 */
static const struct switch_us_site switch_us_sites_24B119[] = {
	{   /* dyld_shared_cache_arm64e.11 of 24B119 (text 0x1a8400000, file 0) */
		{ 0x13, 0xd1, 0x27, 0xbc, 0x9e, 0xd2, 0x36, 0x63, 0xa6, 0x80, 0xfd, 0x90, 0x2f, 0xbb, 0xd2, 0x51 },
		0x1a8d20000ULL, 0x920000ULL, 0x1000ULL, 0, 1,
		{
			/* 0x1a8d217d4: bl isMainDisplay; movi v8.2d, #0; tbnz w0, #0, +0x10; mov x0, x26;
			 * bl BKSDisplayServicesGetExternalDisplayScale; fcvt d8, s0; mov x0, x26 */
			{ 0x9217d4ULL, 28, 4, 20, SWITCH_US_FBS_OVERRIDE,
			  { 0xd7, 0xd9, 0xcb, 0x95, 0x08, 0xe4, 0x00, 0x6f, 0x80, 0x00, 0x00, 0x37, 0xe0, 0x03, 0x1a, 0xaa,
			    0x5f, 0xfd, 0xcf, 0x95, 0x08, 0xc0, 0x22, 0x1e, 0xe0, 0x03, 0x1a, 0xaa }, { 0 } },
		},
		"FrontBoardServices", "main display scale override",
	},
	{   /* dyld_shared_cache_arm64e.13 of 24B119 (text 0x1b0400000, file 0) */
		{ 0xa5, 0x8c, 0x59, 0x09, 0x8b, 0xb2, 0x30, 0x14, 0xa7, 0xb5, 0x28, 0xb4, 0x36, 0x43, 0x92, 0x5e },
		0x1b07bc000ULL, 0x3bc000ULL, 0x3000ULL, 0, 2,
		{
			/* 0x1b07bfa08: mov w4, #1; bl (the display-scale read); mov x19, x0 */
			{ 0x3bfa08ULL, 12, 4, 8, SWITCH_US_MG_SCALE,
			  { 0x24, 0x00, 0x80, 0x52, 0x1f, 0x5c, 0xff, 0x97, 0xf3, 0x03, 0x00, 0xaa }, { 0 } },
			/* 0x1b07bfbec: str w20, [sp, #0x68]; scvtf s0, w19; stp s0, s8, [sp, #0x6c] -> fmov s0, w19 */
			{ 0x3bfbecULL, 12, 4, 4, SWITCH_US_FIXED,
			  { 0xf4, 0x6b, 0x00, 0xb9, 0x60, 0x02, 0x22, 0x1e, 0xe0, 0xa3, 0x0d, 0x2d },
			  { 0x60, 0x02, 0x27, 0x1e } },
		},
		"MobileGestalt", "main-screen-scale",
	},
	{   /* .15 (text 0x1b8400000, file 0) */
		{ 0x4f, 0x44, 0xc1, 0x4a, 0x9b, 0x0b, 0x35, 0x03, 0xaa, 0x96, 0x94, 0xef, 0xd6, 0x5b, 0xca, 0x4e },
		0x1bc064000ULL, 0x3c64000ULL, 0x1000ULL, 0, 1,
		{
			/* 0x1bc065b30: fcvtzu x0, d12; fcvtzu x1, d11; ucvtf d0, x24; fmul d0, d12, d0; fcvtzu x3, d0
			 * -> mul x3, x0, x24; nop; nop */
			{ 0x3c65b30ULL, 20, 8, 12, SWITCH_US_FIXED,
			  { 0x80, 0x01, 0x79, 0x9e, 0x61, 0x01, 0x79, 0x9e, 0x00, 0x03, 0x63, 0x9e, 0x80, 0x09, 0x60, 0x1e,
			    0x03, 0x00, 0x79, 0x9e },
			  { 0x03, 0x7c, 0x18, 0x9b, 0x1f, 0x20, 0x03, 0xd5, 0x1f, 0x20, 0x03, 0xd5 } },
		},
		"CoreUI", "PDF vector image row bytes",
	},
	{   /* .15 */
		{ 0x4f, 0x44, 0xc1, 0x4a, 0x9b, 0x0b, 0x35, 0x03, 0xaa, 0x96, 0x94, 0xef, 0xd6, 0x5b, 0xca, 0x4e },
		0x1bc0ec000ULL, 0x3cec000ULL, 0x2000ULL, 1, 1,
		{
			/* 0x1bc0ee530: the PDF site's five instructions -> mul x3, x0, x24; nop; nop */
			{ 0x3cee530ULL, 20, 8, 12, SWITCH_US_FIXED,
			  { 0x80, 0x01, 0x79, 0x9e, 0x61, 0x01, 0x79, 0x9e, 0x00, 0x03, 0x63, 0x9e, 0x80, 0x09, 0x60, 0x1e,
			    0x03, 0x00, 0x79, 0x9e },
			  { 0x03, 0x7c, 0x18, 0x9b, 0x1f, 0x20, 0x03, 0xd5, 0x1f, 0x20, 0x03, 0xd5 } },
		},
		"CoreUI", "SVG vector image row bytes",
	},
	{   /* dyld_shared_cache_arm64e.65 of 24B119 (text 0x2a0400000, file 0) */
		{ 0x06, 0x08, 0xbd, 0x6b, 0xfc, 0x49, 0x35, 0x50, 0xb8, 0xcd, 0x2f, 0x1f, 0x5d, 0x11, 0xc6, 0xcc },
		0x2a1b04000ULL, 0x1704000ULL, 0x0ULL, 0, 1,
		{
			/* 0x2a1b049f8: 239.0f, 4.0f */
			{ 0x17049f8ULL, 8, 0, 8, SWITCH_US_PROGRESS_BAR,
			  { 0x00, 0x00, 0x6f, 0x43, 0x00, 0x00, 0x80, 0x40 }, { 0 } },
		},
		"ProgressUI", "progress bar",
	},
	{   /* .13 */
		{ 0xa5, 0x8c, 0x59, 0x09, 0x8b, 0xb2, 0x30, 0x14, 0xa7, 0xb5, 0x28, 0xb4, 0x36, 0x43, 0x92, 0x5e },
		0x1b45a0000ULL, 0x41a0000ULL, 0x3000ULL, 1, 1,
		{
			/* 0x1b45a365c: bl BSFloatGreaterThanFloat (over 1024); tbz w0, #0, +0x58 (768x1024 or 0x22);
			 * mov w0, #0x21 -> nop */
			{ 0x41a365cULL, 12, 4, 4, SWITCH_US_FIXED,
			  { 0xd9, 0xd3, 0xee, 0x94, 0xc0, 0x02, 0x00, 0x36, 0x20, 0x04, 0x80, 0x52 },
			  { 0x1f, 0x20, 0x03, 0xd5 } },
		},
		"CoverSheetKit", "lock screen layout row",
	},
	{   /* dyld_shared_cache_arm64e.01 of 24B119 (text 0x180400000, file 0) */
		{ 0xe7, 0xc1, 0xb4, 0x11, 0xdb, 0x39, 0x30, 0x4d, 0x91, 0x4a, 0x72, 0xe3, 0x1f, 0xa3, 0x1b, 0x86 },
		0x1864d4000ULL, 0x60d4000ULL, 0x0ULL, 0, 1,
		{
			/* 0x1864d4db0: iPadWidthFilterTable[0], {768.0, 1024.0, 26} -> {0.0, 0.0, 26} */
			{ 0x60d4db0ULL, 24, 0, 16, SWITCH_US_FIXED,
			  { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x88, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x90, 0x40,
			    0x1a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
			  { 0 } },
		},
		"UIKitCore", "keyboard class of the 10.2-inch iPad",
	},
};

static const struct switch_us_cache switch_us_cache_24B119 = {
	"24B119", "dyld_v1  arm64e", switch_us_sites_24B119,
	sizeof(switch_us_sites_24B119) / sizeof(switch_us_sites_24B119[0]),
};

#endif /* _VM_SWITCH_UI_SCALE_SITES_24B119_H_ */
