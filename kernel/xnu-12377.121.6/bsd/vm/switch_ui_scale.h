/*
 * SwitchUIScale (vm_unix.c): the scales boot-arg switch-ui-scale names, and the cache text each one's pager sites
 * write. Free of kernel types so kernel/tests can drive it.
 */
#ifndef _VM_SWITCH_UI_SCALE_H_
#define _VM_SWITCH_UI_SCALE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Pixels per point on the 720x1280 panel. Only scales whose screen is whole points both ways: UIKit and every
 * framework below it assume a point grid that lands on pixels, and 720 x 1280 shares the factor 80 with nothing
 * finer. num and den stay at most 31, the integers fmov encodes.
 */
struct switch_ui_scale {
	const char *name;    /* the boot-arg's value */
	uint8_t     num, den;
	uint32_t    float_bits; /* num / den as MobileGestalt's float */
	/* ProgressUI's 239 x 4 point bar, as two floats whose single-precision products with float_bits are whole */
	uint32_t    progress_bar_bits[2];
	/*
	 * The app switcher's two-row card scale in landscape, grid_num / grid_den of the screen (an fmov
	 * immediate), for 16:9, where iPadOS's 0.3 leaves the cards no room between the status bar and the dock
	 * (switch_ui_scale_sites.h)
	 */
	uint8_t     grid_num, grid_den;
	/*
	 * SystemStatusUI's expanded battery (icon size 15: Control Center's status bar) as doubles' bits, its body
	 * height and its line width and interspace; 0 where the row stays stock (switch_ui_scale_sites.h)
	 */
	uint64_t    expanded_battery_bits[2];
};

static const struct switch_ui_scale switch_ui_scales[] = {
	/* 720x1280 points: the stock screen, every UI scale site left stock */
	{"1", 1, 1, 0x3f800000, {0x436f0000, 0x40800000}, 0, 0, {0, 0}},
	/*
	 * 576x1024; 239.2 x 4: a 299 x 5 pixel bar; switcher cards 256 x 144 (320 x 180 pixels); expanded battery
	 * 14.4 points tall (18 pixels) with a 1.6 point line (2 pixels; 0x3fe999999999999a, 0.8, for 1)
	 */
	{"1.25", 5, 4, 0x3fa00000, {0x436f3333, 0x40800000}, 1, 4, {0x402ccccccccccccdULL, 0x3ff999999999999aULL}},
	/*
	 * 450x800: a 9.7- to 12.9-inch iPad's point at 209.8 ppi; 238.75 x 3.75: a 382 x 6 pixel bar; switcher cards
	 * 150 x 84.375 (240 x 135 pixels)
	 */
	{"1.6", 8, 5, 0x3fcccccd, {0x436ec000, 0x40700000}, 3, 16, {0, 0}},
};
#define SWITCH_UI_SCALES        (sizeof(switch_ui_scales) / sizeof(switch_ui_scales[0]))
#define SWITCH_UI_SCALE_UNIT    0u
#define SWITCH_UI_SCALE_DEFAULT 1u /* 1.25 */

static inline bool
switch_ui_scale_is_unit(const struct switch_ui_scale *scale)
{
	return scale->num == scale->den;
}

/* switch_ui_scales is static, so every file that includes this has its own copy: compare scales by value, never by
 * address. */
static inline bool
switch_ui_scale_same(const struct switch_ui_scale *a, const struct switch_ui_scale *b)
{
	return a->num == b->num && a->den == b->den;
}

/* The scale for the boot-arg's value (NULL: no switch-ui-scale), or NULL for a value that names none. */
static inline const struct switch_ui_scale *
switch_ui_scale_named(const char *value)
{
	if (value == NULL) {
		return &switch_ui_scales[SWITCH_UI_SCALE_DEFAULT];
	}
	for (size_t i = 0; i < SWITCH_UI_SCALES; i++) {
		const char *name = switch_ui_scales[i].name;
		size_t n = 0;
		while (name[n] != '\0' && name[n] == value[n]) {
			n++;
		}
		if (name[n] == '\0' && value[n] == '\0') {
			return &switch_ui_scales[i];
		}
	}
	return NULL;
}

/* The scale a boot uses: the named one, and the stock screen for a value that names none. */
static inline const struct switch_ui_scale *
switch_ui_scale_for_boot_arg(const char *value)
{
	const struct switch_ui_scale *scale = switch_ui_scale_named(value);
	return scale != NULL ? scale : &switch_ui_scales[SWITCH_UI_SCALE_UNIT];
}

/*
 * fmov d<rd>, #(num / den), false when no immediate is num / den. VFPExpandImm: value = (16 + efgh) / 16 * 2^n with
 * imm8 = 0:b:cd:efgh, n = cd + 1 when b is 0 (1..4) and n = cd - 3 when b is 1 (-3..0).
 */
static inline bool
switch_a64_fmov_d(unsigned rd, unsigned num, unsigned den, uint32_t *insn)
{
	for (unsigned p = 0; p < 8; p++) {  /* n = p - 3: num * 128 = (16 + efgh) * (den << p) */
		const unsigned q = num * 128 / (den << p);
		if (q >= 16 && q <= 31 && q * (den << p) == num * 128) {
			const unsigned b_cd = p >= 4 ? p - 4 : p + 4;
			*insn = 0x1e601000u | (((b_cd << 4) | (q - 16)) << 13) | rd;
			return true;
		}
	}
	return false;
}

/* fmov d<rd>, #value for an integer value 1..31 */
static inline uint32_t
switch_a64_fmov_d_int(unsigned rd, unsigned value)
{
	uint32_t insn = 0;
	switch_a64_fmov_d(rd, value, 1, &insn);
	return insn;
}

static inline uint32_t
switch_a64_movz_w(unsigned rd, uint16_t imm16)
{
	return 0x52800000u | ((uint32_t)imm16 << 5) | rd;
}

static inline uint32_t
switch_a64_movk_w_lsl16(unsigned rd, uint16_t imm16)
{
	return 0x72a00000u | ((uint32_t)imm16 << 5) | rd;
}

static inline void
switch_a64_put(uint8_t *out, uint32_t insn)
{
	out[0] = (uint8_t)insn;
	out[1] = (uint8_t)(insn >> 8);
	out[2] = (uint8_t)(insn >> 16);
	out[3] = (uint8_t)(insn >> 24);
}

static inline void
switch_put64(uint8_t *out, uint64_t value)
{
	switch_a64_put(out, (uint32_t)value);
	switch_a64_put(out + 4, (uint32_t)(value >> 32));
}

/* How a site's edit gets its bytes: fixed in the site, or made for the boot's scale. */
enum switch_ui_scale_edit_kind {
	SWITCH_US_FIXED = 0,
	SWITCH_US_FBS_OVERRIDE, /* 20 bytes: fmov d8, #num; fmov d1, #den; fdiv d8, d8, d1; tbnz w0, #0, +8; fmov d8, xzr */
	SWITCH_US_MG_SCALE,     /* 8 bytes: movz w19, #float_bits[15:0]; movk w19, #float_bits[31:16], lsl #16 */
	SWITCH_US_PROGRESS_BAR, /* 8 bytes: progress_bar_bits, little-endian */
	/*
	 * 28 bytes: fmov d2, #(2 * num / den); fmul d1, d12, d2; frintn d1, d1; fdiv d13, d1, d2; fmov d3, #-4.0;
	 * fmadd d11, d13, d3, d11; fmadd d8, d13, d3, d8. None where 2 * num / den is no fmov immediate.
	 */
	SWITCH_US_BATTERY_FILL,
	/*
	 * 12 bytes: fmov d9, d8; tbnz w0, #0, +0x30; fmov d11, #(grid_num / grid_den). None where the scale has no
	 * grid scale or it is no fmov immediate.
	 */
	SWITCH_US_GRID_SCALE,
	/* 8 bytes each: expanded_battery_bits[0] and [1], little-endian; none where the scale has none */
	SWITCH_US_EXPANDED_BATTERY_HEIGHT,
	SWITCH_US_EXPANDED_BATTERY_LINE,
	/*
	 * 32 bytes: fmov d1, #(grid_num / grid_den); fabs d2, d2; fcmp d2, d3; fcsel d8, d1, d8, gt; fmul d0, d8, d2;
	 * frinta d0, d0; fdiv d0, d0, d2; fminnm d0, d0, d2. None where SWITCH_US_GRID_SCALE has none.
	 */
	SWITCH_US_GRID_PAGE_SCALE,
};

/* Writes the edit's bytes for the scale; false when the kind's length is not size or the kind has none at the scale. */
static inline bool
switch_ui_scale_edit_bytes(enum switch_ui_scale_edit_kind kind, const struct switch_ui_scale *scale, uint8_t *out,
    size_t size)
{
	switch (kind) {
	case SWITCH_US_FBS_OVERRIDE:
		if (size != 20) {
			return false;
		}
		switch_a64_put(out, switch_a64_fmov_d_int(8, scale->num));
		switch_a64_put(out + 4, switch_a64_fmov_d_int(1, scale->den));
		switch_a64_put(out + 8, 0x1e611908u);  /* fdiv d8, d8, d1 */
		switch_a64_put(out + 12, 0x37000040u); /* tbnz w0, #0, +0x8 */
		switch_a64_put(out + 16, 0x9e6703e8u); /* fmov d8, xzr */
		return true;
	case SWITCH_US_MG_SCALE:
		if (size != 8) {
			return false;
		}
		switch_a64_put(out, switch_a64_movz_w(19, (uint16_t)scale->float_bits));
		switch_a64_put(out + 4, switch_a64_movk_w_lsl16(19, (uint16_t)(scale->float_bits >> 16)));
		return true;
	case SWITCH_US_PROGRESS_BAR:
		if (size != 8) {
			return false;
		}
		switch_a64_put(out, scale->progress_bar_bits[0]);
		switch_a64_put(out + 4, scale->progress_bar_bits[1]);
		return true;
	case SWITCH_US_BATTERY_FILL: {
		uint32_t pixels_per_two_points = 0;
		if (size != 28 || !switch_a64_fmov_d(2, 2u * scale->num, scale->den, &pixels_per_two_points)) {
			return false;
		}
		switch_a64_put(out, pixels_per_two_points);
		switch_a64_put(out + 4, 0x1e620981u);  /* fmul d1, d12, d2 */
		switch_a64_put(out + 8, 0x1e644021u);  /* frintn d1, d1 */
		switch_a64_put(out + 12, 0x1e62182du); /* fdiv d13, d1, d2 */
		switch_a64_put(out + 16, 0x1e721003u); /* fmov d3, #-4.0 */
		switch_a64_put(out + 20, 0x1f432dabu); /* fmadd d11, d13, d3, d11 */
		switch_a64_put(out + 24, 0x1f4321a8u); /* fmadd d8, d13, d3, d8 */
		return true;
	}
	case SWITCH_US_GRID_SCALE: {
		uint32_t landscape_scale = 0;
		if (size != 12 || scale->grid_den == 0 ||
		    !switch_a64_fmov_d(11, scale->grid_num, scale->grid_den, &landscape_scale)) {
			return false;
		}
		switch_a64_put(out, 0x1e604109u);     /* fmov d9, d8 */
		switch_a64_put(out + 4, 0x37000180u); /* tbnz w0, #0, +0x30 */
		switch_a64_put(out + 8, landscape_scale);
		return true;
	}
	case SWITCH_US_EXPANDED_BATTERY_HEIGHT:
	case SWITCH_US_EXPANDED_BATTERY_LINE: {
		const uint64_t bits = scale->expanded_battery_bits[kind - SWITCH_US_EXPANDED_BATTERY_HEIGHT];
		if (size != 8 || bits == 0) {
			return false;
		}
		switch_put64(out, bits);
		return true;
	}
	case SWITCH_US_GRID_PAGE_SCALE: {
		uint32_t landscape_scale = 0;
		if (size != 32 || scale->grid_den == 0 ||
		    !switch_a64_fmov_d(1, scale->grid_num, scale->grid_den, &landscape_scale)) {
			return false;
		}
		switch_a64_put(out, landscape_scale);
		switch_a64_put(out + 4, 0x1e60c042u);  /* fabs d2, d2 */
		switch_a64_put(out + 8, 0x1e632040u);  /* fcmp d2, d3 */
		switch_a64_put(out + 12, 0x1e68cc28u); /* fcsel d8, d1, d8, gt */
		switch_a64_put(out + 16, 0x1e620900u); /* fmul d0, d8, d2 */
		switch_a64_put(out + 20, 0x1e664000u); /* frinta d0, d0 */
		switch_a64_put(out + 24, 0x1e621800u); /* fdiv d0, d0, d2 */
		switch_a64_put(out + 28, 0x1e627800u); /* fminnm d0, d0, d2 */
		return true;
	}
	case SWITCH_US_FIXED:
		break;
	}
	return false;
}

#endif /* _VM_SWITCH_UI_SCALE_H_ */
