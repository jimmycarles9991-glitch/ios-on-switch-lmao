#ifndef _VM_SWITCH_UI_SCALE_SITES_21H461_H_
#define _VM_SWITCH_UI_SCALE_SITES_21H461_H_

/*
 * SwitchUIScale's sites in 21H461's arm64 dyld cache (switch_ui_scale_sites.h). One 4 KB chunk of cache text per
 * site, applied all or nothing per site:
 * - FrontBoardServices (.09), -[FBSDisplayConfiguration _initWithImmutableDisplay:originalDisplay:assertIfInvalid:]
 *   + 0x1c0: the main display's modes get the scale override num / den. Three instructions make it in the five that
 *   gave every other display BKSDisplayServicesGetExternalDisplayScale(): other displays now get no override, so
 *   their modes keep QuartzCore's integer scale.
 * - libMobileGestalt (.11), MGIOMFBSupport.c's static screen info: main-screen-scale is the scale's float instead of
 *   the property's value converted. IconServices, TextInputCore, GraphicsServices, CoreMaterial, WebCore and others
 *   size bitmaps from MobileGestalt rather than UIScreen; at 1 those would be drawn scaled up. The register that held
 *   the property holds the float's bits (movz/movk over its read, fmov over the scvtf). Its one other use, the screen
 *   class lookup, finds no class for this panel at any scale. QuartzCore's own scale stays 1 (fcvtzu).
 * - CoreUI (.11), -[CUINamedVectorPDFImage rasterizeImageUsingScaleFactor:forTargetSize:] and its SVG twin: the
 *   bitmap is trunc(w * scale) pixels wide but its row bytes were trunc(w * scale * bytes per pixel), which is not a
 *   multiple of the pixel at a fractional pixel width (34 points at 1.6: 54 pixels, 217 bytes). CGBitmapContextCreate
 *   rejects that and frees the buffer through CoreUI's release callback, then CUICGBitmapContextCreate munmaps it
 *   again: EXC_GUARD DEALLOC_GAP, SpringBoard killed laying out Control Center. Row bytes become width * bytes per
 *   pixel (mul x3, x0, x24), the same value wherever the stock code worked. Every other CoreUI bitmap takes its row
 *   bytes from an integer width or lets CUICGBitmapContextCreate choose them.
 * - ProgressUI (.31), -[PUIProgressWindow _layoutScreen]'s progress bar (DataMigrator's, under the boot logo on the
 *   first boot): 239 x 4 points times main-screen-scale in single precision, a fractional frame Core Animation
 *   resamples (382.4 x 6.4 at 1.6). Its two floats become the scale's progress_bar_bits, whose products are whole
 *   (382 x 6 at 1.6, 299 x 5 at 1.25), at the same place. Nothing else reads them.
 * - UIKitCore (.03), -[_UIBatteryView _fillLayerFrame:cornerRadius:] and -[_UIStatusBarBatteryView
 *   _fillLayerFrame:cornerRadius:], two copies of the same code (one site each, the same edits): the battery fill of
 *   SpringBoard's status bar and Control Center (SystemStatusUI's STUIStatusBarBatteryView and
 *   STUIStatusBarStaticBatteryView are _UIBatteryViews) and of UIKit's own _UIStatusBar (_UIStatusBarBatteryItem
 *   makes a _UIStatusBarBatteryView). The 24 x 12 point body is inset on every side by twice
 *   lineWidthAndInterspace (1 point), its width then times the charge. At 1.25 the inset is 2.5 pixels, and Core
 *   Animation starts a non-antialiased edge at the first pixel whose centre it covers, so both edges of the 25 x 10
 *   pixel fill land half a pixel up and left: one pixel of gap above and left of it, two below and right. The inset
 *   becomes whole pixels, rounded to nearest (frintn: the 2.5 tie goes to 2, where Core Animation drew the fill
 *   from), for a 26 x 11 pixel fill with equal gaps. At 0x18a115f78 (0x18ae7fa40) half the inset goes to d13
 *   (saved by the prologue, otherwise unused) and the inset comes twice off the width and height. The origin moves
 *   by it at 0x18a115fc8 (0x18ae7fa90), which runs on the same !_showsPercentage path, in place of
 *   CGRectGetWidth(fill): that is the width's absolute value, fabs. At 1.6 the body is 38.4 x 19.2 pixels, no inset
 *   gives it equal whole-pixel gaps, and 3.2 is no fmov immediate: both sites stay stock.
 * - CoverSheetKit (.13), +[CSDeviceDetermination categoryFromScreenSize], the device row of every lock screen time,
 *   date and widget metric in CSProminentLayoutController (SpringBoard, PosterBoard and PosterKit all lay out through
 *   it). An iPad without a home button (the ADT's home-button-type 2) gets its row from the reference bounds' height,
 *   and at or under 1024 points only from the exact size 768x1024 (row 24); 576x1024 and 450x800 fell through to row
 *   28, a device it does not know, which is neither pad nor phone and takes every table's default: an 84 point time
 *   in a 100 point box from y = 84 under a date box from y = 79 to 115, the date's baseline in the time's cap height.
 *   The tbz that leaves on the size check becomes a nop, so these screens get row 24 as an iPad with a home button
 *   at or under 1024 points gets its row 17 (the same metrics): the date box from y = 60 (landscape) or 85 ends
 *   above the 140 point time's 123 point box from y = 97 or 122, a real iPad's lock screen. At 1 the screen is 1280
 *   points tall (row 25) and the site stays stock.
 * - SpringBoard (.25), -[SBMixedGridSwitcherModifier _fullScreenCardScale], the iPad app switcher's card scale. The
 *   switcher has two rows on every iPad (-[SBFluidSwitcherViewController numberOfRowsInGridSwitcher] is 2; only App
 *   Exposé's grid keeps up to two cards in one row), its cards are gridSwitcherPageScale (0.3) of the screen on every
 *   iPad, and the two rows with their 45 point headers (icon 29 + 16) and 63 point gap are centred between the top
 *   and the dock (floatingDockHeight). Every iPad is 4:3 to 3:2, 32 to 50 points above and below the rows in
 *   landscape; at 16:9 the rows fill the height above the dock (1 point to spare at 1.25), the top row's titles under
 *   the status bar. In landscape the two-row scale becomes grid_num / grid_den, the largest fmov immediate that keeps
 *   the titles clear of the 24 point status bar: 0.25 at 1.25 (30 points above and below), 0.1875 at 1.6 (if its
 *   dock is as tall). Portrait, where 0.3 fits, and App Exposé's single row (0.42) stay as shipped. After
 *   containerViewBounds the width and height come from the returned rect (d2, d3); the landscape scale crosses that
 *   call in d11 (saved by the prologue, otherwise unused); disableFullScreenCardScaleRounding still returns the scale
 *   unrounded. BSFloatIsZero(width) becomes fminnm against the width: a zero width makes 0 / 0, a quiet NaN, which
 *   fminnm drops for the other operand, 0 as before. Three columns of 256 points fit in 1024; the grid stays trailing
 *   aligned, as iPadOS lays out a switcher narrower than the screen.
 * - SystemStatusUI (.25), the icon size 15 row of the battery tables STUIStatusBarStaticBatteryView and
 *   STUIStatusBarBatteryView share: the expanded status bar's battery, the one Control Center shows
 *   (-[STUIStatusBarVisualProvider_Pad styleAttributesForStyle:] gives 15 to mode 1 and 14 to the others).
 *   _UIStaticBatteryView strokes the body into a bitmap of the view's size at the display scale and draws the
 *   fill, inset by twice the line, in a bitmap of its own. At 1.25 the 28 x 14 point body with its 1.25 point line
 *   is 35 x 17.5 pixels with a 1.5625 pixel line: its bottom and every inner edge fall between pixels, and the
 *   11.5 pixel fill comes from a 12 pixel bitmap. The body becomes 14.4 points tall and the line
 *   expanded_battery_bits' (switch_ui_scale.h): a 35 x 18 pixel body, a 2 pixel line and gap and a 27 x 10 pixel
 *   fill, whose inset the UIKitCore fill site then leaves as it is. The width, corner radii and pin stay (the static
 *   view's pin comes from -[STUIStatusBarStaticBatteryView _pinSizeForIconSize:]'s constants, not this table,
 *   and its edges are curves); the bolt scales with the height. At 1.6 the row stays stock, as the fill sites do.
 */
static const struct switch_us_site switch_us_sites_21H461[] = {
	{   /* dyld_shared_cache_arm64.09 of 21H461 (text 0x199130000, file 0) */
		{ 0x59, 0xec, 0x4b, 0x71, 0xf0, 0x27, 0x33, 0xd1, 0xae, 0x39, 0x4e, 0x04, 0xbd, 0x70, 0x7b, 0x6d },
		0x19f3cc000ULL, 0x629c000ULL, 0x3000ULL, 0, 1,
		{
			/* 0x19f3cf8cc: bl isMainDisplay; fmov d8, xzr; tbnz w0, #0, +0x10; mov x0, x28;
			 * bl BKSDisplayServicesGetExternalDisplayScale; fcvt d8, s0; mov x0, x28 */
			{ 0x629f8ccULL, 28, 4, 20, SWITCH_US_FBS_OVERRIDE,
			  { 0xcd, 0x90, 0x02, 0x94, 0xe8, 0x03, 0x67, 0x9e, 0x80, 0x00, 0x00, 0x37, 0xe0, 0x03, 0x1c, 0xaa,
			    0x28, 0x20, 0x24, 0x94, 0x08, 0xc0, 0x22, 0x1e, 0xe0, 0x03, 0x1c, 0xaa }, { 0 } },
		},
		"FrontBoardServices", "main display scale override", false,
	},
	{   /* dyld_shared_cache_arm64.11 of 21H461 (text 0x19fce4000, file 0) */
		{ 0x70, 0x56, 0x7d, 0x89, 0x4c, 0xe6, 0x3e, 0xc6, 0xa4, 0xde, 0xe7, 0xb4, 0xf4, 0xec, 0x60, 0x41 },
		0x1a0214000ULL, 0x530000ULL, 0x1000ULL, 0, 2,
		{
			/* 0x1a0215754: mov w4, #1; bl (the display-scale read); mov x19, x0 */
			{ 0x531754ULL, 12, 4, 8, SWITCH_US_MG_SCALE,
			  { 0x24, 0x00, 0x80, 0x52, 0x05, 0xe9, 0xff, 0x97, 0xf3, 0x03, 0x00, 0xaa }, { 0 } },
			/* 0x1a021594c: str w21, [sp, #0x68]; scvtf s0, w19; stp s0, s8, [sp, #0x6c] -> fmov s0, w19 */
			{ 0x53194cULL, 12, 4, 4, SWITCH_US_FIXED,
			  { 0xf5, 0x6b, 0x00, 0xb9, 0x60, 0x02, 0x22, 0x1e, 0xe0, 0xa3, 0x0d, 0x2d },
			  { 0x60, 0x02, 0x27, 0x1e } },
		},
		"MobileGestalt", "main-screen-scale", false,
	},
	{   /* .11 */
		{ 0x70, 0x56, 0x7d, 0x89, 0x4c, 0xe6, 0x3e, 0xc6, 0xa4, 0xde, 0xe7, 0xb4, 0xf4, 0xec, 0x60, 0x41 },
		0x1a315c000ULL, 0x3478000ULL, 0x0ULL, 1, 1,
		{
			/* 0x1a315c214: fcvtzu x0, d12; fcvtzu x1, d11; ucvtf d0, x24; fmul d0, d12, d0; fcvtzu x3, d0
			 * -> mul x3, x0, x24; nop; nop */
			{ 0x3478214ULL, 20, 8, 12, SWITCH_US_FIXED,
			  { 0x80, 0x01, 0x79, 0x9e, 0x61, 0x01, 0x79, 0x9e, 0x00, 0x03, 0x63, 0x9e, 0x80, 0x09, 0x60, 0x1e,
			    0x03, 0x00, 0x79, 0x9e },
			  { 0x03, 0x7c, 0x18, 0x9b, 0x1f, 0x20, 0x03, 0xd5, 0x1f, 0x20, 0x03, 0xd5 } },
		},
		"CoreUI", "PDF vector image row bytes", false,
	},
	{   /* .11 */
		{ 0x70, 0x56, 0x7d, 0x89, 0x4c, 0xe6, 0x3e, 0xc6, 0xa4, 0xde, 0xe7, 0xb4, 0xf4, 0xec, 0x60, 0x41 },
		0x1a31d0000ULL, 0x34ec000ULL, 0x3000ULL, 2, 1,
		{
			/* 0x1a31d369c: fcvtzu x0, d10; fcvtzu x1, d9; ucvtf d0, x24; fmul d0, d10, d0; fcvtzu x3, d0
			 * -> mul x3, x0, x24; nop; nop */
			{ 0x34ef69cULL, 20, 8, 12, SWITCH_US_FIXED,
			  { 0x40, 0x01, 0x79, 0x9e, 0x21, 0x01, 0x79, 0x9e, 0x00, 0x03, 0x63, 0x9e, 0x40, 0x09, 0x60, 0x1e,
			    0x03, 0x00, 0x79, 0x9e },
			  { 0x03, 0x7c, 0x18, 0x9b, 0x1f, 0x20, 0x03, 0xd5, 0x1f, 0x20, 0x03, 0xd5 } },
		},
		"CoreUI", "SVG vector image row bytes", false,
	},
	{   /* dyld_shared_cache_arm64.31 of 21H461 (text 0x1e221c000, file 0) */
		{ 0x64, 0xa2, 0xf3, 0x51, 0xbe, 0x93, 0x39, 0xa6, 0x93, 0x72, 0xe8, 0x56, 0xb6, 0x64, 0x9c, 0xfc },
		0x1e41f0000ULL, 0x1fd4000ULL, 0x1000ULL, 0, 1,
		{
			/* 0x1e41f1838: 239.0f, 4.0f */
			{ 0x1fd5838ULL, 8, 0, 8, SWITCH_US_PROGRESS_BAR,
			  { 0x00, 0x00, 0x6f, 0x43, 0x00, 0x00, 0x80, 0x40 }, { 0 } },
		},
		"ProgressUI", "progress bar", false,
	},
	{   /* dyld_shared_cache_arm64.03 of 21H461 (text 0x186dac000, file 0) */
		{ 0x1b, 0x9c, 0xcf, 0x0c, 0x7b, 0x68, 0x39, 0x63, 0xaf, 0x64, 0x48, 0x4b, 0xcf, 0xda, 0xc1, 0x4c },
		0x18ae7c000ULL, 0x40d0000ULL, 0x3000ULL, 1, 2,
		{
			/* 0x18ae7fa40: fadd d1, d12, d12; fadd d10, d1, d10; fsub d2, d11, d1; fsub d11, d2, d1;
			 * fadd d9, d1, d9; fsub d2, d8, d1; fsub d8, d2, d1 */
			{ 0x40d3a40ULL, 28, 0, 28, SWITCH_US_BATTERY_FILL,
			  { 0x81, 0x29, 0x6c, 0x1e, 0x2a, 0x28, 0x6a, 0x1e, 0x62, 0x39, 0x61, 0x1e, 0x4b, 0x38, 0x61, 0x1e,
			    0x29, 0x28, 0x69, 0x1e, 0x02, 0x39, 0x61, 0x1e, 0x48, 0x38, 0x61, 0x1e }, { 0 } },
			/* 0x18ae7fa90: fmov d0, d10; fmov d1, d9; fmov d2, d11; fmov d3, d8; bl CGRectGetWidth
			 * -> fadd d1, d13, d13; fadd d10, d10, d1; fadd d9, d9, d1; fabs d0, d11; nop */
			{ 0x40d3a90ULL, 20, 0, 20, SWITCH_US_FIXED,
			  { 0x40, 0x41, 0x60, 0x1e, 0x21, 0x41, 0x60, 0x1e, 0x62, 0x41, 0x60, 0x1e, 0x03, 0x41, 0x60, 0x1e,
			    0x07, 0x24, 0xb0, 0x94 },
			  { 0xa1, 0x29, 0x6d, 0x1e, 0x4a, 0x29, 0x61, 0x1e, 0x29, 0x29, 0x61, 0x1e, 0x60, 0xc1, 0x60, 0x1e,
			    0x1f, 0x20, 0x03, 0xd5 } },
		},
		"UIKitCore", "_UIStatusBarBatteryView fill", false,
	},
	{   /* .03 */
		{ 0x1b, 0x9c, 0xcf, 0x0c, 0x7b, 0x68, 0x39, 0x63, 0xaf, 0x64, 0x48, 0x4b, 0xcf, 0xda, 0xc1, 0x4c },
		0x18a114000ULL, 0x3368000ULL, 0x1000ULL, 2, 2,
		{
			/* 0x18a115f78: fadd d1, d12, d12; fadd d10, d1, d10; fsub d2, d11, d1; fsub d11, d2, d1;
			 * fadd d9, d1, d9; fsub d2, d8, d1; fsub d8, d2, d1 */
			{ 0x3369f78ULL, 28, 0, 28, SWITCH_US_BATTERY_FILL,
			  { 0x81, 0x29, 0x6c, 0x1e, 0x2a, 0x28, 0x6a, 0x1e, 0x62, 0x39, 0x61, 0x1e, 0x4b, 0x38, 0x61, 0x1e,
			    0x29, 0x28, 0x69, 0x1e, 0x02, 0x39, 0x61, 0x1e, 0x48, 0x38, 0x61, 0x1e }, { 0 } },
			/* 0x18a115fc8: fmov d0, d10; fmov d1, d9; fmov d2, d11; fmov d3, d8; bl CGRectGetWidth
			 * -> fadd d1, d13, d13; fadd d10, d10, d1; fadd d9, d9, d1; fabs d0, d11; nop */
			{ 0x3369fc8ULL, 20, 0, 20, SWITCH_US_FIXED,
			  { 0x40, 0x41, 0x60, 0x1e, 0x21, 0x41, 0x60, 0x1e, 0x62, 0x41, 0x60, 0x1e, 0x03, 0x41, 0x60, 0x1e,
			    0xb9, 0xca, 0xe5, 0x94 },
			  { 0xa1, 0x29, 0x6d, 0x1e, 0x4a, 0x29, 0x61, 0x1e, 0x29, 0x29, 0x61, 0x1e, 0x60, 0xc1, 0x60, 0x1e,
			    0x1f, 0x20, 0x03, 0xd5 } },
		},
		"UIKitCore", "_UIBatteryView fill", false,
	},
	{   /* dyld_shared_cache_arm64.13 of 21H461 (text 0x1a67c8000, file 0) */
		{ 0xc0, 0x45, 0x2d, 0x94, 0xa5, 0xf8, 0x3b, 0xe1, 0xbd, 0xfc, 0xe1, 0x5e, 0x71, 0x4a, 0x84, 0xf5 },
		0x1ac8c0000ULL, 0x60f8000ULL, 0x2000ULL, 1, 1,
		{
			/* 0x1ac8c26a0: bl BSSizeEqualToSize; tbz w0, #0, -0x294 (row 28); mov w0, #24 -> nop */
			{ 0x60fa6a0ULL, 12, 4, 4, SWITCH_US_FIXED,
			  { 0x71, 0xda, 0x2e, 0x94, 0x60, 0xeb, 0x07, 0x36, 0x00, 0x03, 0x80, 0x52 },
			  { 0x1f, 0x20, 0x03, 0xd5 } },
		},
		"CoverSheetKit", "lock screen layout row", false,
	},
	{   /* dyld_shared_cache_arm64.25 of 21H461 (text 0x1ce094000, file 0) */
		{ 0xf1, 0x6d, 0xce, 0xf8, 0xc4, 0x46, 0x38, 0x05, 0x91, 0x63, 0xc0, 0x13, 0x8e, 0x4d, 0xca, 0xd1 },
		0x1cfb98000ULL, 0x1b04000ULL, 0x2000ULL, 1, 2,
		{
			/* 0x1cfb9a0ac: mov x0, x20; bl disableFullScreenCardScaleRounding; tbz w0, #0, +0xc;
			 * fmov d9, d8; b +0x2c; mov x0, x20; bl containerViewBounds
			 * -> ...; fmov d9, d8; tbnz w0, #0, +0x30; fmov d11, #(landscape scale); ... */
			{ 0x1b060acULL, 28, 8, 12, SWITCH_US_GRID_SCALE,
			  { 0xe0, 0x03, 0x14, 0xaa, 0x4c, 0x1d, 0x27, 0x94, 0x60, 0x00, 0x00, 0x36, 0x09, 0x41, 0x60, 0x1e,
			    0x0b, 0x00, 0x00, 0x14, 0xe0, 0x03, 0x14, 0xaa, 0x8f, 0xfc, 0x26, 0x94 }, { 0 } },
			/* 0x1cfb9a0c8: bl CGRectGetWidth; fmov d10, d0; bl BSFloatIsZero; fmov d9, xzr;
			 * tbnz w0, #0, +0x10; fmul d0, d8, d10; frinta d0, d0; fdiv d9, d0, d10
			 * -> fmov d0, #0.34375; fcmp d8, d0; fccmp d2, d3, #4, mi; fcsel d8, d11, d8, gt;
			 *    fmul d0, d8, d2; frinta d0, d0; fdiv d9, d0, d2; fminnm d9, d9, d2 */
			{ 0x1b060c8ULL, 32, 0, 32, SWITCH_US_FIXED,
			  { 0x6f, 0xb4, 0x3f, 0x95, 0x0a, 0x40, 0x60, 0x1e, 0xf1, 0xb1, 0x3f, 0x95, 0xe9, 0x03, 0x67, 0x9e,
			    0x80, 0x00, 0x00, 0x37, 0x00, 0x09, 0x6a, 0x1e, 0x00, 0x40, 0x66, 0x1e, 0x09, 0x18, 0x6a, 0x1e },
			  { 0x00, 0xd0, 0x6a, 0x1e, 0x00, 0x21, 0x60, 0x1e, 0x44, 0x44, 0x63, 0x1e, 0x68, 0xcd, 0x68, 0x1e,
			    0x00, 0x09, 0x62, 0x1e, 0x00, 0x40, 0x66, 0x1e, 0x09, 0x18, 0x62, 0x1e, 0x29, 0x79, 0x62, 0x1e } },
		},
		"SpringBoard", "app switcher landscape card scale", false,
	},
	{   /* .25 */
		{ 0xf1, 0x6d, 0xce, 0xf8, 0xc4, 0x46, 0x38, 0x05, 0x91, 0x63, 0xc0, 0x13, 0x8e, 0x4d, 0xca, 0xd1 },
		0x1cf3e8000ULL, 0x1354000ULL, 0x3000ULL, 2, 2,
		{
			/* 0x1cf3eb8b8: the line width and interspace table (0x1cf3eb848) at icon size 15: 1.25 */
			{ 0x13578b8ULL, 8, 0, 8, SWITCH_US_EXPANDED_BATTERY_LINE,
			  { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xf4, 0x3f }, { 0 } },
			/* 0x1cf3ebad8: the body height table (0x1cf3eba68) at icon size 15: 14.0 */
			{ 0x1357ad8ULL, 8, 0, 8, SWITCH_US_EXPANDED_BATTERY_HEIGHT,
			  { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x2c, 0x40 }, { 0 } },
		},
		"SystemStatusUI", "expanded battery row", false,
	},
};

static const struct switch_us_cache switch_us_cache_21H461 = {
	"21H461", "dyld_v1   arm64", switch_us_sites_21H461,
	sizeof(switch_us_sites_21H461) / sizeof(switch_us_sites_21H461[0]),
};

#endif /* _VM_SWITCH_UI_SCALE_SITES_21H461_H_ */
