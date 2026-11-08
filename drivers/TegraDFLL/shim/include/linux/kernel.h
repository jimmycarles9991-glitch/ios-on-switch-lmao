/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Switch shim: the slice of the Linux 4.9 kernel API that the vendored L4T DFLL sources use (clk-dfll.c,
 * clk-tegra124-dfll-fcpu.c, cvb.c, speedo-tegra210.c), freestanding. Everything that is not a subsystem of its own
 * lives here; the other shim headers include this one. Semantics follow Linux 4.9 (include/linux/kernel.h, err.h,
 * compiler.h, printk.h, bug.h, jiffies.h, ktime.h, spinlock.h, io.h, iopoll.h) wherever the vendored code depends on
 * them, notably DIV_ROUND_CLOSEST and abs() on signed/unsigned operands, which the CVB arithmetic relies on.
 * Every global symbol is renamed by tdfll_rename.h (force-included first), because TegraGM20B's nouveau shim links
 * into the same kernel and defines the same Linux names.
 */
#ifndef TDFLL_LINUX_KERNEL_H
#define TDFLL_LINUX_KERNEL_H
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "../../../../../kits/SwitchLinux/SwitchLinuxLock.h"
#include "../../../../../kits/SwitchMMIO/SwitchMMIO.h"

/* config: what the L4T build enables here */
#define CONFIG_ARCH_TEGRA 1
#define CONFIG_ARCH_TEGRA_210_SOC 1
#define CONFIG_COMMON_CLK 1
/* not set: CONFIG_DEBUG_FS (clk-dfll.c debugfs nodes), CONFIG_OF (speedo-tegra210.c /chosen lookups),
 * CONFIG_TEGRA210_EMC (tegra_emc.h falls back to its static inlines), CONFIG_PM_SLEEP */
#define __ARG_PLACEHOLDER_1 0,
#define __take_second_arg(__ignored, val, ...) val
#define __is_defined(x) ___is_defined(x)
#define ___is_defined(val) ____is_defined(__ARG_PLACEHOLDER_##val)
#define ____is_defined(arg1_or_junk) __take_second_arg(arg1_or_junk 1, 0)
#define IS_ENABLED(option) __is_defined(option)

typedef uint8_t u8;   typedef int8_t s8;
typedef uint16_t u16; typedef int16_t s16;
typedef uint32_t u32; typedef int32_t s32;
typedef uint64_t u64; typedef int64_t s64;
typedef u8 __u8; typedef u16 __u16; typedef u32 __u32; typedef u64 __u64;
typedef s8 __s8; typedef s16 __s16; typedef s32 __s32; typedef s64 __s64;
typedef long ssize_t;
typedef u64 phys_addr_t;
typedef u64 dma_addr_t;
typedef u64 resource_size_t;
typedef unsigned int gfp_t;
typedef unsigned short umode_t;
typedef long long loff_t;
typedef s64 ktime_t;
typedef unsigned long ulong;
typedef unsigned int uint;
typedef struct { int counter; } atomic_t;

#define __iomem
#define __user
#define __force
#define __rcu
#define __init
#define __exit
#define __initdata
#define __initconst
#define __cold
#define __must_check __attribute__((warn_unused_result))
#define __packed __attribute__((packed))
#define __aligned(x) __attribute__((aligned(x)))
#define __always_inline inline __attribute__((always_inline))
#define __maybe_unused __attribute__((unused))
#define __always_unused __attribute__((unused))
#define __printf(a, b) __attribute__((format(printf, a, b)))
#define noinline __attribute__((noinline))
#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#define barrier() __asm__ __volatile__("" ::: "memory")
#define READ_ONCE(x) (*(const volatile __typeof__(x) *)&(x))
#define WRITE_ONCE(x, v) do { *(volatile __typeof__(x) *)&(x) = (v); } while (0)
#define BUILD_BUG_ON(c) _Static_assert(!(c), "BUILD_BUG_ON " #c)
#define typeof __typeof__
#define EXPORT_SYMBOL(x) _Static_assert(1, "EXPORT_SYMBOL " #x)
#define EXPORT_SYMBOL_GPL(x) _Static_assert(1, "EXPORT_SYMBOL_GPL " #x)
#define MODULE_DEVICE_TABLE(a, b) _Static_assert(1, "MODULE_DEVICE_TABLE")
#define MODULE_LICENSE(x) _Static_assert(1, "MODULE_LICENSE")
#define MODULE_DESCRIPTION(x) _Static_assert(1, "MODULE_DESCRIPTION")
#define MODULE_AUTHOR(x) _Static_assert(1, "MODULE_AUTHOR")
/* speedo-tegra210.c's sku_id/cspd_id/sspd_id/gspd_id overrides (kernel command line in L4T): none here, so they stay -1 */
#define module_param(name, type, perm) _Static_assert(1, "module_param " #name)
#define MODULE_PARM_DESC(a, b) _Static_assert(1, "MODULE_PARM_DESC")

#define container_of(ptr, type, member) ((type *)(void *)((char *)(ptr) - offsetof(type, member)))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

#ifndef INT_MAX
#define INT_MAX ((int)(~0U >> 1))
#define INT_MIN (-INT_MAX - 1)
#define UINT_MAX (~0U)
#define LONG_MAX ((long)(~0UL >> 1))
#define ULONG_MAX (~0UL)
#endif
#define U8_MAX ((u8)~0U)
#define U32_MAX ((u32)~0U)
#define S32_MAX ((s32)(U32_MAX >> 1))
#define BITS_PER_LONG 64
#define BIT(n) (1UL << (unsigned int)(n))
#define NSEC_PER_USEC 1000L
#define NSEC_PER_MSEC 1000000L
#define USEC_PER_MSEC 1000L
#define USEC_PER_SEC 1000000L
#define MSEC_PER_SEC 1000L
#define NSEC_PER_SEC 1000000000L

/* Linux 4.9 min/max/clamp without the distinct-type check (the vendored code passed gcc's) */
#define min(x, y) ({ __typeof__(x) _min1 = (x); __typeof__(y) _min2 = (y); _min1 < _min2 ? _min1 : _min2; })
#define max(x, y) ({ __typeof__(x) _max1 = (x); __typeof__(y) _max2 = (y); _max1 > _max2 ? _max1 : _max2; })
#define min_t(type, x, y) ({ type __min1 = (x); type __min2 = (y); __min1 < __min2 ? __min1 : __min2; })
#define max_t(type, x, y) ({ type __max1 = (x); type __max2 = (y); __max1 > __max2 ? __max1 : __max2; })
#define clamp(val, lo, hi) min((__typeof__(val))max(val, lo), hi)
#define clamp_t(type, val, lo, hi) min_t(type, max_t(type, val, lo), hi)
#define clamp_val(val, lo, hi) clamp_t(__typeof__(val), val, lo, hi)
/* Linux 4.9 abs(): long-sized operands as long, everything else converted to int first */
#define abs(x) ({ \
	long __abs_ret; \
	if (sizeof(x) == sizeof(long)) { long __x = (x); __abs_ret = (__x < 0) ? -__x : __x; } \
	else { int __x = (x); __abs_ret = (__x < 0) ? -__x : __x; } \
	__abs_ret; })
#define DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))
/* Linux 4.9 include/linux/kernel.h: rounds negative quotients away from zero too */
#define DIV_ROUND_CLOSEST(x, divisor) ({ \
	__typeof__(x) __x = x; \
	__typeof__(divisor) __d = divisor; \
	(((__typeof__(x))-1) > 0 || ((__typeof__(divisor))-1) > 0 || (((__x) > 0) == ((__d) > 0))) ? \
		(((__x) + ((__d) / 2)) / (__d)) : (((__x) - ((__d) / 2)) / (__d)); })
#define round_down(x, y) ((x) & ~((__typeof__(x))((y) - 1)))
#define round_up(x, y) ((((x) - 1) | ((__typeof__(x))((y) - 1))) + 1)

/* errno values: include/uapi/asm-generic/errno*.h */
#define EPERM 1
#define ENOENT 2
#define EIO 5
#define ENXIO 6
#define EAGAIN 11
#define ENOMEM 12
#define EACCES 13
#define EFAULT 14
#define EBUSY 16
#define EEXIST 17
#define ENODEV 19
#define EINVAL 22
#define ERANGE 34
#define ENOSYS 38
#define ENODATA 61
#define EOPNOTSUPP 95
#define ETIMEDOUT 110
#define EPROBE_DEFER 517
#define ENOTSUPP 524
#define MAX_ERRNO 4095
#define IS_ERR_VALUE(x) unlikely((unsigned long)(void *)(x) >= (unsigned long)-MAX_ERRNO)
static inline void *ERR_PTR(long error) { return (void *)error; }
static inline long PTR_ERR(const void *ptr) { return (long)ptr; }
static inline bool IS_ERR(const void *ptr) { return IS_ERR_VALUE((unsigned long)ptr); }
static inline bool IS_ERR_OR_NULL(const void *ptr) { return !ptr || IS_ERR_VALUE((unsigned long)ptr); }
static inline void *ERR_CAST(const void *ptr) { return (void *)(uintptr_t)ptr; }

/* strings: XNU's libkern, linked by symbol */
void *memcpy(void *, const void *, size_t);
void *memset(void *, int, size_t);
size_t strlen(const char *);
int strcmp(const char *, const char *);
int strncmp(const char *, const char *, size_t);
int snprintf(char *, size_t, const char *, ...) __printf(3, 4);
int vsnprintf(char *, size_t, const char *, va_list);

#define KERN_EMERG ""
#define KERN_ALERT ""
#define KERN_CRIT ""
#define KERN_ERR ""
#define KERN_WARNING ""
#define KERN_NOTICE ""
#define KERN_INFO ""
#define KERN_DEBUG ""
int printk(const char *fmt, ...) __printf(1, 2);
#define no_printk(fmt, ...) ({ if (0) printk(fmt, ##__VA_ARGS__); 0; })
#define pr_err(fmt, ...) printk(fmt, ##__VA_ARGS__)
#define pr_warn(fmt, ...) printk(fmt, ##__VA_ARGS__)
#define pr_warning pr_warn
#define pr_info(fmt, ...) printk(fmt, ##__VA_ARGS__)
#define pr_notice(fmt, ...) printk(fmt, ##__VA_ARGS__)
#define pr_debug(fmt, ...) no_printk(fmt, ##__VA_ARGS__)
#define pr_cont(fmt, ...) printk(fmt, ##__VA_ARGS__)

/* BUG panics with one line */
__attribute__((noreturn)) void panic(const char *fmt, ...);
#define BUG() panic("TegraDFLL: BUG at %s:%d", __FILE__, __LINE__)
#define BUG_ON(c) do { if (unlikely(c)) BUG(); } while (0)
#define WARN_ON(c) ({ int __w = !!(c); if (unlikely(__w)) printk("WARNING at %s:%d\n", __FILE__, __LINE__); unlikely(__w); })
#define WARN_ON_ONCE(c) WARN_ON(c)
#define WARN(c, fmt, ...) ({ int __w = !!(c); if (unlikely(__w)) printk("WARNING: " fmt, ##__VA_ARGS__); unlikely(__w); })

/* RCU: the OPP table here is static after build */
#define rcu_read_lock() do { } while (0)
#define rcu_read_unlock() do { } while (0)

#define udelay(us) switch_linux_udelay((u64)(us))
#define ndelay(ns) switch_linux_udelay((u64)DIV_ROUND_UP((ns), 1000))
#define mdelay(ms) switch_linux_udelay((u64)(ms) * 1000u)
#define msleep(ms) switch_linux_usleep_range((u64)(ms) * 1000u, (u64)(ms) * 1000u)
#define usleep_range(lo, hi) switch_linux_usleep_range((u64)(lo), (u64)(hi))

/* time: jiffies at HZ 1000 over XNU's clock */
#define HZ 1000
#define jiffies ((unsigned long)(switch_linux_nanotime() / NSEC_PER_MSEC))
#define msecs_to_jiffies(ms) ((unsigned long)(ms))
#define jiffies_to_msecs(j) ((unsigned int)(j))
#define usecs_to_jiffies(us) ((unsigned long)DIV_ROUND_UP((us), 1000))
#define jiffies_to_usecs(j) ((unsigned int)(j) * 1000u)
/* no suspend here (clk-dfll.c only defers hrtimers and one-shot calibration while it is set) */
#define timekeeping_suspended 0
static inline ktime_t ktime_get(void) { return (ktime_t)switch_linux_nanotime(); }
static inline ktime_t ktime_set(const s64 secs, const unsigned long nsecs) { return secs * NSEC_PER_SEC + (s64)nsecs; }
static inline s64 ktime_to_us(const ktime_t kt) { return kt / NSEC_PER_USEC; }
static inline s64 ktime_to_ms(const ktime_t kt) { return kt / NSEC_PER_MSEC; }
static inline s64 ktime_us_delta(const ktime_t later, const ktime_t earlier) { return ktime_to_us(later - earlier); }
static inline s64 ktime_ms_delta(const ktime_t later, const ktime_t earlier) { return ktime_to_ms(later - earlier); }

/* MMIO: kits/SwitchMMIO (load-acquire reads for the A57's erratum 832075, which also order what follows) */
#define __raw_readl(a) switch_mmio_read32((const volatile u8 *)(a), 0)
#define __raw_writel(v, a) switch_mmio_write32((volatile u8 *)(a), 0, (v))
#define readl_relaxed(a) __raw_readl(a)
#define writel_relaxed(v, a) __raw_writel((v), (a))
#define readl(a) __raw_readl(a)
#define writel(v, a) switch_mmio_write32_publish((volatile u8 *)(a), 0, (v))

/* spinlocks: kits/SwitchLinux */
typedef switch_linux_spinlock spinlock_t;
#define __SPIN_LOCK_UNLOCKED(x) SWITCH_LINUX_SPINLOCK_INIT
#define DEFINE_SPINLOCK(x) spinlock_t x = __SPIN_LOCK_UNLOCKED(x)
#define spin_lock_init(l) do { (l)->holder = 0; } while (0)
#define spin_lock_irqsave(l, flags) do { (flags) = switch_linux_spin_lock_irqsave(l); } while (0)
#define spin_unlock_irqrestore(l, flags) switch_linux_spin_unlock_irqrestore((l), (flags))
#define lockdep_assert_held(l) do { } while (0)

#endif
