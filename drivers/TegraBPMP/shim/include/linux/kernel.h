/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Switch TegraBPMP shim: the include/linux/kernel.h subset the vendored L4T BPMP sources use. */
#ifndef BPXNU_LINUX_KERNEL_H
#define BPXNU_LINUX_KERNEL_H
#include <linux/types.h>
#include <linux/errno.h>
#include <linux/spinlock.h>
#include <linux/completion.h>
#include <linux/list.h>   /* Linux gets it through spinlock.h; mailman.c relies on that */

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#ifndef container_of
#define container_of(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))
#endif
#define EXPORT_SYMBOL(sym) extern int bpxnu_export_##sym
#define USEC_PER_SEC 1000000UL
#define S64_MAX INT64_MAX

#define pr_err(fmt, ...) switch_linux_log("TegraBPMP: " fmt, ##__VA_ARGS__)
#define pr_warn(fmt, ...) switch_linux_log("TegraBPMP: " fmt, ##__VA_ARGS__)
#define pr_info(fmt, ...) switch_linux_log("TegraBPMP: " fmt, ##__VA_ARGS__)
#define pr_debug(fmt, ...) do { } while (0)
#define WARN_ON(cond) ({ int __c = !!(cond); if (__c) switch_linux_log("TegraBPMP: WARN_ON at %s:%d\n", __FILE__, __LINE__); __c; })
#define WARN_ON_ONCE(cond) WARN_ON(cond)

void *memcpy(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
int snprintf(char *buf, size_t size, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
int strcmp(const char *a, const char *b);

static inline unsigned long __ffs(unsigned long word) { return (unsigned long)__builtin_ctzl(word); }

/* jiffies are milliseconds here (HZ 1000) */
#define HZ 1000
static inline unsigned long usecs_to_jiffies(unsigned long us) { return (us + 999) / 1000; }
static inline unsigned int jiffies_to_usecs(unsigned long j) { return (unsigned int)(j * 1000); }

static inline ktime_t ktime_get(void) { return (ktime_t)switch_linux_nanotime(); }
static inline s64 ktime_us_delta(ktime_t later, ktime_t earlier) { return (later - earlier) / 1000; }

static inline int irqs_disabled(void) { return switch_linux_irqs_disabled() ? 1 : 0; }
static inline int smp_processor_id(void) { return switch_linux_cpu(); }
static inline int tegra_platform_is_silicon(void) { return 1; }
#endif
