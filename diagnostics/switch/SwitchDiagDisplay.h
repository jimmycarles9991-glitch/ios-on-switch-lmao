/* Switch fixed OLED framebuffer renderer. No display-controller access.
 * Font and rotation adapted from boot/handoff/display.h; source identity is
 * retained in the diagnostic build receipt. Addresses are supplied as mapped VAs.
 */
#ifndef SWITCH_DIAG_DISPLAY_H
#define SWITCH_DIAG_DISPLAY_H
#include <stdint.h>
#define SWITCH_FB_PA      UINT64_C(0xf5a00000)
#define SWITCH_FB_BYTES   UINT64_C(0x384000)
#define SWITCH_WHITE      UINT32_C(0xffffffff)
#define SWITCH_GREEN      UINT32_C(0xff70ff70)
#define SWITCH_RED        UINT32_C(0xff7070ff)
#define SWITCH_BACKGROUND UINT32_C(0xff20140c)

static inline int switch_diag_video_matches(uint64_t base, uint64_t display, uint64_t row, uint64_t width,
                                            uint64_t height, uint64_t depth)
{
    return base == SWITCH_FB_PA && display == 1 && row == 2880 && width == 720 && height == 1280 && depth == 32;
}

static inline void switch_diag_pixel(volatile uint32_t *pixels, unsigned x, unsigned y, uint32_t color)
{
    if (x < 1280 && y < 720) {
        /* NOLINTNEXTLINE(clang-analyzer-core.NullDereference): drawing starts once the framebuffer is mapped */
        pixels[(uint64_t)(1279 - x) * 720 + y] = color;
    }
}

static inline void switch_diag_clear(volatile uint32_t *pixels)
{
    for (unsigned i = 0; i < SWITCH_FB_BYTES / 4; ++i)
        pixels[i] = SWITCH_BACKGROUND;
}

// Five-column, seven-row glyphs. Bit 4 is the leftmost pixel of each row.
static const struct {
    char ch;
    uint8_t row[7];
} switch_diag_font[] = {
    {'0', {14, 17, 19, 21, 25, 17, 14}}, {'1', {4, 12, 4, 4, 4, 4, 14}},      {'2', {14, 17, 1, 2, 4, 8, 31}},
    {'3', {30, 1, 1, 14, 1, 1, 30}},     {'4', {2, 6, 10, 18, 31, 2, 2}},     {'5', {31, 16, 16, 30, 1, 1, 30}},
    {'6', {14, 16, 16, 30, 17, 17, 14}}, {'7', {31, 1, 2, 4, 8, 8, 8}},       {'8', {14, 17, 17, 14, 17, 17, 14}},
    {'9', {14, 17, 17, 15, 1, 1, 14}},   {'A', {14, 17, 17, 31, 17, 17, 17}}, {'B', {30, 17, 17, 30, 17, 17, 30}},
    {'C', {14, 17, 16, 16, 16, 17, 14}}, {'D', {30, 17, 17, 17, 17, 17, 30}}, {'E', {31, 16, 16, 30, 16, 16, 31}},
    {'F', {31, 16, 16, 30, 16, 16, 16}}, {'G', {14, 17, 16, 23, 17, 17, 15}}, {'H', {17, 17, 17, 31, 17, 17, 17}},
    {'I', {14, 4, 4, 4, 4, 4, 14}},      {'J', {7, 2, 2, 2, 18, 18, 12}},     {'K', {17, 18, 20, 24, 20, 18, 17}},
    {'L', {16, 16, 16, 16, 16, 16, 31}}, {'M', {17, 27, 21, 21, 17, 17, 17}}, {'N', {17, 25, 21, 19, 17, 17, 17}},
    {'O', {14, 17, 17, 17, 17, 17, 14}}, {'P', {30, 17, 17, 30, 16, 16, 16}}, {'Q', {14, 17, 17, 17, 21, 18, 13}},
    {'R', {30, 17, 17, 30, 20, 18, 17}}, {'S', {15, 16, 16, 14, 1, 1, 30}},   {'T', {31, 4, 4, 4, 4, 4, 4}},
    {'U', {17, 17, 17, 17, 17, 17, 14}}, {'V', {17, 17, 17, 17, 17, 10, 4}},  {'W', {17, 17, 17, 21, 21, 21, 10}},
    {'X', {17, 17, 10, 4, 10, 17, 17}},  {'Y', {17, 17, 10, 4, 4, 4, 4}},     {'Z', {31, 1, 2, 4, 8, 16, 31}},
    {':', {0, 4, 4, 0, 4, 4, 0}},        {'-', {0, 0, 0, 31, 0, 0, 0}},       {'.', {0, 0, 0, 0, 0, 6, 6}},
    {'=', {0, 0, 31, 0, 31, 0, 0}},      {'/', {1, 2, 2, 4, 8, 8, 16}},       {'(', {2, 4, 8, 8, 8, 4, 2}},
    {')', {8, 4, 2, 2, 2, 4, 8}},        {'_', {0, 0, 0, 0, 0, 0, 31}},       {'?', {14, 17, 1, 2, 4, 0, 4}},
    {' ', {0, 0, 0, 0, 0, 0, 0}},
};

/* Bounded logical 1280x720 text. Returns nonzero if text did not fit. */
static inline int switch_diag_text(volatile uint32_t *pixels, unsigned x, unsigned y, unsigned bottom, const char *text,
                                   unsigned limit, uint32_t color)
{
    const unsigned left = x;
    if (bottom > 704)
        bottom = 704;
    for (unsigned n = 0; n < limit; ++n) {
        char ch = text[n];
        if (!ch)
            return 0;
        if (ch == '\n') {
            x = left;
            y += 20;
            continue;
        }
        if (ch == '\r')
            continue;
        if (ch == '\t')
            ch = ' ';
        if (x + 12 > 1264) {
            x = left;
            y += 20;
        }
        if (y + 14 > bottom)
            return 1;
        if (ch >= 'a' && ch <= 'z')
            ch -= 'a' - 'A';
        const uint8_t *glyph = switch_diag_font[sizeof(switch_diag_font) / sizeof(switch_diag_font[0]) - 2].row;
        for (unsigned i = 0; i < sizeof(switch_diag_font) / sizeof(switch_diag_font[0]); ++i) {
            if (switch_diag_font[i].ch == ch) {
                glyph = switch_diag_font[i].row;
                break;
            }
        }
        for (unsigned row = 0; row < 7; ++row) {
            for (unsigned col = 0; col < 6; ++col) {
                for (unsigned dy = 0; dy < 2; ++dy) {
                    for (unsigned dx = 0; dx < 2; ++dx) {
                        switch_diag_pixel(pixels, x + col * 2 + dx, y + row * 2 + dy,
                                          col < 5 && (glyph[row] & (1u << (4 - col))) ? color : SWITCH_BACKGROUND);
                    }
                }
            }
        }
        x += 12;
    }
    return 1;
}
#endif
