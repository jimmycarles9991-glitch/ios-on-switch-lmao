// xnu_ui_scale_page_harness.c: each tree's switch_ui_scale_page (bsd/vm/vm_unix.c), the apple_protect decrypt hook
// that writes a UI scale site's edits into a 4 KiB chunk of cache text, compiled as the kernel compiles it (C) over the
// 24B119 table, for xnu_ui_scale_page_test.cpp.
#ifndef SWITCH_XNU_UI_SCALE_PAGE_HARNESS_H
#define SWITCH_XNU_UI_SCALE_PAGE_HARNESS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct switch_us_site;

// The 24B119 site of that name and applied text, as the harness's copy of the table holds it (the page hook finds a
// site's log slot by its place in that copy).
const struct switch_us_site *xnu_ui_scale_page_site(const char *name, const char *applied);

// Forgets every site's logged line and the captured log.
void xnu_ui_scale_page_reset(void);

int xnu10063_ui_scale_page(const void *src, void *dst, unsigned long long src_offset,
                           const struct switch_us_site *site);
int xnu12377_ui_scale_page(const void *src, void *dst, unsigned long long src_offset,
                           const struct switch_us_site *site);

// What the hooks printed since the last reset, and how many lines.
const char *xnu_ui_scale_page_log(void);
unsigned xnu_ui_scale_page_log_lines(void);

#ifdef __cplusplus
}
#endif

#endif
