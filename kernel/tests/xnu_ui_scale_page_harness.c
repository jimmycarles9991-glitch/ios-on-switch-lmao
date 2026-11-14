#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#define SWITCH_IPSW_KIND_24B119 1
#include <vm/switch_ui_scale_sites.h>

#include "xnu_ui_scale_page_harness.h"

#define SWITCH_HG_CHUNK             4096ULL
#define relaxed                     __ATOMIC_RELAXED
#define os_atomic_xchg(p, v, order) __atomic_exchange_n((p), (v), (order))

static char page_log[4096];
static size_t page_log_used;
static unsigned page_log_lines;

static int switch_test_page_printf(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    const int n = vsnprintf(page_log + page_log_used, sizeof(page_log) - page_log_used, format, args);
    va_end(args);
    if (n > 0) {
        page_log_used = strlen(page_log);
        page_log_lines++;
    }
    return n;
}

#define printf switch_test_page_printf

static const struct switch_ui_scale *switch_ui_scale(void)
{
    return &switch_ui_scales[SWITCH_UI_SCALE_DEFAULT];
}

#define switch_us_logged     switch_us_logged_10063
#define switch_ui_scale_page switch_ui_scale_page_10063
static uint32_t switch_us_logged[SWITCH_US_MAX_SITES];
#include "switch_ui_scale_page_10063.h"
#undef switch_ui_scale_page
#undef switch_us_logged

#define switch_us_logged     switch_us_logged_12377
#define switch_ui_scale_page switch_ui_scale_page_12377
static uint32_t switch_us_logged[SWITCH_US_MAX_SITES];
#include "switch_ui_scale_page_12377.h"
#undef switch_ui_scale_page
#undef switch_us_logged

const struct switch_us_site *xnu_ui_scale_page_site(const char *name, const char *applied)
{
    for (size_t i = 0; i < SWITCH_US_CACHE->count; i++) {
        const struct switch_us_site *site = &SWITCH_US_CACHE->sites[i];
        if (strcmp(site->name, name) == 0 && strcmp(site->applied, applied) == 0) {
            return site;
        }
    }
    return NULL;
}

void xnu_ui_scale_page_reset(void)
{
    memset(switch_us_logged_10063, 0, sizeof(switch_us_logged_10063));
    memset(switch_us_logged_12377, 0, sizeof(switch_us_logged_12377));
    page_log[0] = '\0';
    page_log_used = 0;
    page_log_lines = 0;
}

int xnu10063_ui_scale_page(const void *src, void *dst, unsigned long long src_offset, const struct switch_us_site *site)
{
    return switch_ui_scale_page_10063(src, dst, src_offset, (void *)(uintptr_t)site);
}

int xnu12377_ui_scale_page(const void *src, void *dst, unsigned long long src_offset, const struct switch_us_site *site)
{
    return switch_ui_scale_page_12377(src, dst, src_offset, (void *)(uintptr_t)site);
}

const char *xnu_ui_scale_page_log(void)
{
    return page_log;
}

unsigned xnu_ui_scale_page_log_lines(void)
{
    return page_log_lines;
}
