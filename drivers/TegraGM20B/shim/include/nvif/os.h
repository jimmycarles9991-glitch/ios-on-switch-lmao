/* SPDX-License-Identifier: MIT */
/*
 * Switch replacement for nouveau's include/nvif/os.h: the small slice of the Linux kernel API that
 * the vendored nvkm code uses, implemented on XNU (kits/SwitchLinux, shim/linux.c + shim/xnu_glue.cpp via nvxnu.h).
 * Nothing in here includes an XNU header.
 */
#ifndef __NOUVEAU_OS_H__
#define __NOUVEAU_OS_H__
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "../nvxnu.h"
#include "../../../../../kits/SwitchMMIO/SwitchMMIO.h"

#define CONFIG_NOUVEAU_PLATFORM_DRIVER 1
#define CONFIG_ARCH_TEGRA 1
#define CONFIG_ARCH_TEGRA_210_SOC 1
#define CONFIG_NOUVEAU_DEBUG 5
#define CONFIG_NOUVEAU_DEBUG_DEFAULT 2
#define __ARG_PLACEHOLDER_1 0,
#define __take_second_arg(__ignored, val, ...) val
#define __is_defined(x) ___is_defined(x)
#define ___is_defined(val) ____is_defined(__ARG_PLACEHOLDER_##val)
// NOLINTNEXTLINE(bugprone-macro-parentheses): Linux's IS_ENABLED trick pastes the argument against the 1
#define ____is_defined(arg1_or_junk) __take_second_arg(arg1_or_junk 1, 0)
#define IS_ENABLED(option) __is_defined(option)
#define IS_BUILTIN(option) __is_defined(option)
#define IS_REACHABLE(option) __is_defined(option)

typedef uint8_t u8;
typedef int8_t s8;
typedef uint16_t u16;
typedef int16_t s16;
typedef uint32_t u32;
typedef int32_t s32;
typedef uint64_t u64;
typedef int64_t s64;
typedef u8 __u8;
typedef u16 __u16;
typedef u32 __u32;
typedef u64 __u64;
typedef s8 __s8;
typedef s16 __s16;
typedef s32 __s32;
typedef s64 __s64;
typedef u16 __le16;
typedef u32 __le32;
typedef u64 __le64;
typedef u16 __be16;
typedef u32 __be32;
typedef u64 __be64;
typedef long ssize_t;
typedef u64 phys_addr_t;
typedef u64 dma_addr_t;
typedef u64 resource_size_t;
typedef unsigned int gfp_t;
typedef long long loff_t;
typedef s64 ktime_t;

typedef struct {
	u64 pgprot;
} pgprot_t;

#define __iomem
#define __user
#define __force
#define __rcu
#define __must_check __attribute__((warn_unused_result))
#define __packed __attribute__((packed))
#define __aligned(x) __attribute__((aligned(x)))
#define __always_inline inline __attribute__((always_inline))
#define __maybe_unused __attribute__((unused))
#define __always_unused __attribute__((unused))
#define __printf(a, b) __attribute__((format(printf, a, b)))
#undef __weak
#define __weak __attribute__((weak))
#define __init
#define __exit
#define __initconst
#define __cold
#define __counted_by(m)
#define __acquires(x)
#define __releases(x)
#define __must_hold(x)
#define __nonstring
#define noinline __attribute__((noinline))
#define fallthrough __attribute__((fallthrough))
#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#define barrier() __asm__ __volatile__("" ::: "memory")
#define unreachable() __builtin_unreachable()
#define READ_ONCE(x) (*(const volatile __typeof__(x) *)&(x))
#define WRITE_ONCE(x, v)                               \
	do {                                           \
		*(volatile __typeof__(x) *)&(x) = (v); \
	} while (0)
#define BUILD_BUG_ON(c) _Static_assert(!(c), "BUILD_BUG_ON " #c)
#define BUILD_BUG_ON_ZERO(e) ((int)(sizeof(struct { int : (-!!(e)); })))
#define static_assert(expr, ...) _Static_assert(expr, "" __VA_ARGS__)
#define typeof __typeof__
#define EXPORT_SYMBOL(x)
#define EXPORT_SYMBOL_GPL(x)
#define MODULE_FIRMWARE(x)
#define MODULE_DEVICE_TABLE(a, b)
#define module_param_named(a, b, c, d)
#define MODULE_PARM_DESC(a, b)

#define container_of(ptr, type, member) ((type *)(void *)((char *)(ptr) - offsetof(type, member)))
#define container_of_const(ptr, type, member) container_of(ptr, type, member)
#define sizeof_field(t, m) sizeof(((t *)0)->m)
#define offsetofend(t, m) (offsetof(t, m) + sizeof_field(t, m))
#define __must_be_array(a) 0
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define __struct_group(TAG, NAME, ATTRS, ...) \
	union {                               \
		struct {                      \
			__VA_ARGS__           \
		} ATTRS;                      \
		struct TAG {                  \
			__VA_ARGS__           \
		} ATTRS NAME;                 \
	} ATTRS
#define struct_group(NAME, ...) __struct_group(, NAME, , __VA_ARGS__)
#define struct_group_tagged(TAG, NAME, ...) __struct_group(TAG, NAME, , __VA_ARGS__)
#define TRAILING_OVERLAP(TYPE, NAME, FAM, ...)                                \
	union {                                                               \
		TYPE NAME;                                                    \
		struct {                                                      \
			unsigned char __offset_to_##FAM[offsetof(TYPE, FAM)]; \
			__VA_ARGS__                                           \
		};                                                            \
	}

#define U8_MAX ((u8)~0U)
#define U16_MAX ((u16)~0U)
#define U32_MAX ((u32)~0U)
#define U64_MAX ((u64)~0ULL)
#define S32_MAX ((s32)(U32_MAX >> 1))
#define S64_MAX ((s64)(U64_MAX >> 1))
#ifndef INT_MAX
#define INT_MAX ((int)(~0U >> 1))
#define UINT_MAX (~0U)
#define LONG_MAX ((long)(~0UL >> 1))
#define ULONG_MAX (~0UL)
#endif
#define BITS_PER_LONG 64
#define BITS_PER_BYTE 8
#define BIT(n) (1UL << (n))
#define BIT_ULL(n) (1ULL << (n))
#define GENMASK(h, l) (((~0UL) - (1UL << (l)) + 1) & (~0UL >> (BITS_PER_LONG - 1 - (h))))
#define GENMASK_ULL(h, l) (((~0ULL) - (1ULL << (l)) + 1) & (~0ULL >> (63 - (h))))
#define SZ_1K 0x400
#define SZ_4K 0x1000
#define SZ_8K 0x2000
#define SZ_16K 0x4000
#define SZ_32K 0x8000
#define SZ_64K 0x10000
#define SZ_128K 0x20000
#define SZ_256K 0x40000
#define SZ_512K 0x80000
#define SZ_1M 0x100000
#define SZ_2M 0x200000
#define SZ_4M 0x400000
#define SZ_16M 0x1000000
#define SZ_128M 0x8000000
#define SZ_1G 0x40000000
#define SZ_4G 0x100000000ULL
#define NSEC_PER_USEC 1000L
#define NSEC_PER_MSEC 1000000L
#define NSEC_PER_SEC 1000000000L
#define USEC_PER_MSEC 1000L
#define USEC_PER_SEC 1000000L
#define MSEC_PER_SEC 1000L
#define HZ 100

#define min(a, b)                       \
	({                              \
		__typeof__(a) _a = (a); \
		__typeof__(b) _b = (b); \
		_a < _b ? _a : _b;      \
	})
#define max(a, b)                       \
	({                              \
		__typeof__(a) _a = (a); \
		__typeof__(b) _b = (b); \
		_a > _b ? _a : _b;      \
	})
#define min3(a, b, c) min(min(a, b), c)
#define max3(a, b, c) max(max(a, b), c)
#define min_t(t, a, b)             \
	({                         \
		t _a = (a);        \
		t _b = (b);        \
		_a < _b ? _a : _b; \
	})
#define max_t(t, a, b)             \
	({                         \
		t _a = (a);        \
		t _b = (b);        \
		_a > _b ? _a : _b; \
	})
#define clamp(v, lo, hi) min(max(v, lo), hi)
#define clamp_t(t, v, lo, hi) min_t(t, max_t(t, v, lo), hi)
#define clamp_val(v, lo, hi) clamp_t(__typeof__(v), v, lo, hi)
#define swap(a, b)                      \
	do {                            \
		__typeof__(a) _t = (a); \
		(a) = (b);              \
		(b) = _t;               \
	} while (0)
#define abs(x)                          \
	({                              \
		__typeof__(x) _x = (x); \
		_x < 0 ? -_x : _x;      \
	})
#define DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))
#define DIV_ROUND_UP_ULL(n, d) ((u64)DIV_ROUND_UP((u64)(n), (u64)(d)))
#define DIV_ROUND_CLOSEST(x, d)                                                                  \
	({                                                                                       \
		__typeof__(x) _x = (x);                                                          \
		__typeof__(d) _d = (d);                                                          \
		(((__typeof__(x))-1) > 0 || ((__typeof__(d))-1) > 0 || ((_x) > 0) == ((_d) > 0)) \
			? (((_x) + ((_d) / 2)) / (_d))                                           \
			: (((_x) - ((_d) / 2)) / (_d));                                          \
	})
#define DIV_ROUND_CLOSEST_ULL(x, d)     \
	({                              \
		u64 _t = (x) + (d) / 2; \
		_t / (d);               \
	})
#define do_div(n, base)                        \
	({                                     \
		u32 _b = (base);               \
		u32 _r = (u32)((u64)(n) % _b); \
		(n) = (u64)(n) / _b;           \
		_r;                            \
	})

static inline u64 div_u64(u64 n, u32 d)
{
	return n / d;
}

static inline s64 div_s64(s64 n, s32 d)
{
	return n / d;
}

static inline u64 div64_u64(u64 n, u64 d)
{
	return n / d;
}

static inline s64 div64_s64(s64 n, s64 d)
{
	return n / d;
}

static inline u64 div_u64_rem(u64 n, u32 d, u32 *r)
{
	*r = n % d;
	return n / d;
}

#define mult_frac(x, n, d) (((x) / (d)) * (n) + (((x) % (d)) * (n)) / (d))
#define roundup(x, y) ((((x) + ((y) - 1)) / (y)) * (y))
#define rounddown(x, y) ((x) - ((x) % (y)))
#define __round_mask(x, y) ((__typeof__(x))((y) - 1))
#define round_up(x, y) ((((x) - 1) | __round_mask(x, y)) + 1)
#define round_down(x, y) ((x) & ~__round_mask(x, y))
#define ALIGN(x, a) (((x) + ((__typeof__(x))(a) - 1)) & ~((__typeof__(x))(a) - 1))
#define ALIGN_DOWN(x, a) ((x) & ~((__typeof__(x))(a) - 1))
#define IS_ALIGNED(x, a) (((x) & ((__typeof__(x))(a) - 1)) == 0)
#define PTR_ALIGN(p, a) ((__typeof__(p))ALIGN((uintptr_t)(p), (a)))
#define upper_32_bits(n) ((u32)(((u64)(n)) >> 32))
#define lower_32_bits(n) ((u32)((n) & 0xffffffff))
#define hweight8(x) __builtin_popcount((u8)(x))
#define hweight16(x) __builtin_popcount((u16)(x))
#define hweight32(x) __builtin_popcount((u32)(x))
#define hweight64(x) __builtin_popcountll((u64)(x))
#define hweight_long(x) __builtin_popcountl(x)

static inline int fls(unsigned int x)
{
	return x ? 32 - __builtin_clz(x) : 0;
}

static inline int fls64(u64 x)
{
	return x ? 64 - __builtin_clzll(x) : 0;
}

static inline int fls_long(unsigned long x)
{
	return fls64(x);
}

static inline unsigned long __fls(unsigned long x)
{
	return 63 - __builtin_clzl(x);
}

static inline unsigned long __ffs(unsigned long x)
{
	return __builtin_ctzl(x);
}

static inline unsigned long __ffs64(u64 x)
{
	return __builtin_ctzll(x);
}

#define ffs(x) __builtin_ffs(x)
#define ffz(x) __ffs(~(unsigned long)(x))
#define ilog2(n) ((n) ? 63 - __builtin_clzll((u64)(n)) : -1)
#define is_power_of_2(n) ((n) != 0 && (((n) & ((n) - 1)) == 0))

static inline unsigned long roundup_pow_of_two(unsigned long n)
{
	return n <= 1 ? 1 : 1UL << (unsigned)fls_long(n - 1);
}

static inline unsigned long rounddown_pow_of_two(unsigned long n)
{
	return 1UL << (unsigned)(fls_long(n) - 1);
}

#define order_base_2(n) ilog2(roundup_pow_of_two(n))
#define PAGE_SHIFT 12
#define PAGE_SIZE (1UL << PAGE_SHIFT)
#define PAGE_MASK (~(PAGE_SIZE - 1))
#define PAGE_ALIGN(x) ALIGN(x, PAGE_SIZE)

static inline int get_order(unsigned long size)
{
	return size <= PAGE_SIZE ? 0 : fls_long((size - 1) >> PAGE_SHIFT);
}

static inline size_t size_mul(size_t a, size_t b)
{
	size_t r = 0;
	return __builtin_mul_overflow(a, b, &r) ? SIZE_MAX : r;
}

static inline size_t size_add(size_t a, size_t b)
{
	size_t r = 0;
	return __builtin_add_overflow(a, b, &r) ? SIZE_MAX : r;
}

#define array_size(a, b) size_mul(a, b)
#define array3_size(a, b, c) size_mul(size_mul(a, b), c)
#define struct_size(p, member, count) size_add(sizeof(*(p)), size_mul(sizeof(*(p)->member), count))
#define flex_array_size(p, member, count) size_mul(sizeof(*(p)->member), count)
#define check_add_overflow(a, b, d) __builtin_add_overflow(a, b, d)
#define check_mul_overflow(a, b, d) __builtin_mul_overflow(a, b, d)

#define EPERM 1
#define ENOENT 2
#define ESRCH 3
#define EINTR 4
#define EIO 5
#define ENXIO 6
#define E2BIG 7
#define ENOEXEC 8
#define EBADF 9
#define ECHILD 10
#define EAGAIN 11
#define ENOMEM 12
#define EACCES 13
#define EFAULT 14
#define EBUSY 16
#define EEXIST 17
#define ENODEV 19
#define EINVAL 22
#define ENOSPC 28
#define ERANGE 34
#define EDEADLK 35
#define ENOSYS 38
#define ENODATA 61
#define ETIME 62
#define EPROTO 71
#define EOVERFLOW 75
#define EBADMSG 74
#define EOPNOTSUPP 95
#define ETIMEDOUT 110
#define ECANCELED 125
#define ENOTSUPP 524
#define MAX_ERRNO 4095
#define IS_ERR_VALUE(x) unlikely((unsigned long)(void *)(x) >= (unsigned long)-MAX_ERRNO)

static inline void *ERR_PTR(long error)
{
	return (void *)error;
}

static inline long PTR_ERR(const void *ptr)
{
	return (long)ptr;
}

static inline bool IS_ERR(const void *ptr)
{
	return IS_ERR_VALUE((unsigned long)ptr);
}

static inline bool IS_ERR_OR_NULL(const void *ptr)
{
	return !ptr || IS_ERR_VALUE((unsigned long)ptr);
}

static inline void *ERR_CAST(const void *ptr)
{
	return (void *)ptr;
}

static inline int PTR_ERR_OR_ZERO(const void *ptr)
{
	return IS_ERR(ptr) ? (int)PTR_ERR(ptr) : 0;
}

void *memcpy(void *, const void *, size_t);
void *memmove(void *, const void *, size_t);
void *memset(void *, int, size_t);
int memcmp(const void *, const void *, size_t);
size_t strlen(const char *);
size_t strnlen(const char *, size_t);
int strcmp(const char *, const char *);
int strncmp(const char *, const char *, size_t);
char *strchr(const char *, int);
char *strrchr(const char *, int);
char *strstr(const char *, const char *);
char *strncpy(char *, const char *, size_t);
int strcasecmp(const char *, const char *);
int strncasecmp(const char *, const char *, size_t);
void *memchr(const void *, int, size_t);
ssize_t strscpy(char *dst, const char *src, size_t size);
size_t strlcpy(char *dst, const char *src, size_t size);

static inline ssize_t strscpy_pad(char *d, const char *s, size_t n)
{
	ssize_t r = strscpy(d, s, n);
	size_t l = r < 0 ? n : (size_t)r;
	if (l < n)
		memset(d + l, 0, n - l);
	return r;
}

int snprintf(char *, size_t, const char *, ...) __printf(3, 4);
int scnprintf(char *, size_t, const char *, ...) __printf(3, 4);
int vsnprintf(char *, size_t, const char *, va_list);
int sprintf(char *, const char *, ...) __printf(2, 3);
char *kasprintf(gfp_t gfp, const char *fmt, ...) __printf(2, 3);
char *kstrdup(const char *s, gfp_t gfp);
char *kstrndup(const char *s, size_t max, gfp_t gfp);
size_t strcspn(const char *s, const char *reject);
size_t strspn(const char *s, const char *accept);
const char *kstrdup_const(const char *s, gfp_t gfp);
void kfree_const(const void *p);
void *kmemdup(const void *p, size_t size, gfp_t gfp);
int kstrtol(const char *s, unsigned int base, long *res);
int kstrtoul(const char *s, unsigned int base, unsigned long *res);
long simple_strtol(const char *cp, char **endp, unsigned int base);
unsigned long simple_strtoul(const char *cp, char **endp, unsigned int base);

static inline int isspace(int c)
{
	return c == ' ' || (c >= '\t' && c <= '\r');
}

static inline int isdigit(int c)
{
	return c >= '0' && c <= '9';
}

static inline int isalpha(int c)
{
	return ((unsigned)c | 32u) >= 'a' && ((unsigned)c | 32u) <= 'z';
}

static inline int isalnum(int c)
{
	return isdigit(c) || isalpha(c);
}

static inline int isprint(int c)
{
	return c >= 0x20 && c < 0x7f;
}

static inline int toupper(int c)
{
	return (c >= 'a' && c <= 'z') ? c - 32 : c;
}

static inline int tolower(int c)
{
	return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

static inline char *skip_spaces(const char *s)
{
	while (isspace(*s))
		s++;
	return (char *)s;
}

#define sort nvxnu_sort /* XNU exports its own sort() */
void sort(void *base, size_t num, size_t size, int (*cmp)(const void *, const void *),
	  void (*swap_fn)(void *, void *, int));

#define KERN_EMERG ""
#define KERN_ALERT ""
#define KERN_CRIT ""
#define KERN_ERR ""
#define KERN_WARNING ""
#define KERN_NOTICE ""
#define KERN_INFO ""
#define KERN_DEBUG ""
#define KERN_CONT ""
#define printk(fmt, ...) nvxnu_log(fmt, ##__VA_ARGS__)
#define pr_emerg(fmt, ...) nvxnu_log("tegra-gm20b: " fmt, ##__VA_ARGS__)
#define pr_crit pr_emerg
#define pr_err pr_emerg
#define pr_warn pr_emerg
#define pr_notice(fmt, ...)                            \
	do {                                           \
		if (0)                                 \
			nvxnu_log(fmt, ##__VA_ARGS__); \
	} while (0)
#define pr_info pr_notice
#define pr_debug pr_notice
#define pr_cont pr_notice
#define pr_err_once pr_err
#define pr_warn_once pr_warn
#define pr_info_once pr_info
struct device;
const char *dev_name(const struct device *dev);
#define dev_printk_(dev, fmt, ...) nvxnu_log("tegra-gm20b %s: " fmt, dev_name(dev), ##__VA_ARGS__)
#define dev_error_(dev, fmt, ...) nvxnu_error(dev_name(dev), fmt, ##__VA_ARGS__)
#define dev_emerg dev_error_
#define dev_crit dev_error_
#define dev_alert dev_error_
#define dev_err dev_error_
#define dev_warn dev_printk_
#define dev_notice dev_printk_
#define dev_info dev_printk_
#define dev_dbg dev_printk_
#define dev_err_once dev_error_
#define dev_warn_once dev_printk_
#define dev_info_once dev_printk_
#define dev_err_ratelimited dev_error_
#define dev_warn_ratelimited dev_printk_
#define dev_WARN(dev, fmt, ...) WARN(1, fmt, ##__VA_ARGS__)
#define dev_WARN_ONCE(dev, c, fmt, ...) WARN(c, fmt, ##__VA_ARGS__)
#define WARN(cond, fmt, ...)                                                \
	({                                                                  \
		int _c = !!(cond);                                          \
		if (unlikely(_c))                                           \
			nvxnu_log("tegra-gm20b WARN %s:%d: " fmt, __FILE__, \
				  __LINE__, ##__VA_ARGS__);                 \
		unlikely(_c);                                               \
	})
#define WARN_ON(cond)                                                                            \
	({                                                                                       \
		int _c = !!(cond);                                                               \
		if (unlikely(_c))                                                                \
			nvxnu_log("tegra-gm20b WARN_ON(%s) %s:%d\n", #cond, __FILE__, __LINE__); \
		unlikely(_c);                                                                    \
	})
#define WARN_ONCE WARN
#define WARN_ON_ONCE WARN_ON
#define BUG() nvxnu_log("tegra-gm20b BUG %s:%d\n", __FILE__, __LINE__)
#define BUG_ON(cond)                \
	do {                        \
		if (unlikely(cond)) \
			BUG();      \
	} while (0)
#define might_sleep() \
	do {          \
	} while (0)
#define lockdep_assert_held(l) \
	do {                   \
		(void)(l);     \
	} while (0)
#define lockdep_set_class(l, k) \
	do {                    \
	} while (0)
#define lockdep_set_class_and_name(l, k, n) \
	do {                                \
	} while (0)

struct lock_class_key {
	int dummy;
};

#define mb() __asm__ __volatile__("dsb sy" ::: "memory")
#define rmb() __asm__ __volatile__("dsb ld" ::: "memory")
#define wmb() __asm__ __volatile__("dsb st" ::: "memory")
#define dma_rmb() __asm__ __volatile__("dmb oshld" ::: "memory")
#define dma_wmb() __asm__ __volatile__("dmb oshst" ::: "memory")
#define smp_mb() __asm__ __volatile__("dmb ish" ::: "memory")
#define smp_rmb() __asm__ __volatile__("dmb ishld" ::: "memory")
#define smp_wmb() __asm__ __volatile__("dmb ishst" ::: "memory")
#define smp_mb__before_atomic() smp_mb()
#define smp_mb__after_atomic() smp_mb()
#define cpu_relax() __asm__ __volatile__("yield" ::: "memory")

typedef struct {
	int counter;
} atomic_t;

typedef struct {
	s64 counter;
} atomic64_t;

#define ATOMIC_INIT(i) {(i)}
#define atomic_read(v) __atomic_load_n(&(v)->counter, __ATOMIC_RELAXED)
#define atomic_set(v, i) __atomic_store_n(&(v)->counter, (i), __ATOMIC_RELAXED)
#define atomic_add(i, v) ((void)__atomic_add_fetch(&(v)->counter, (i), __ATOMIC_SEQ_CST))
#define atomic_sub(i, v) ((void)__atomic_sub_fetch(&(v)->counter, (i), __ATOMIC_SEQ_CST))
#define atomic_inc(v) atomic_add(1, v)
#define atomic_dec(v) atomic_sub(1, v)
#define atomic_add_return(i, v) __atomic_add_fetch(&(v)->counter, (i), __ATOMIC_SEQ_CST)
#define atomic_sub_return(i, v) __atomic_sub_fetch(&(v)->counter, (i), __ATOMIC_SEQ_CST)
#define atomic_inc_return(v) atomic_add_return(1, v)
#define atomic_dec_return(v) atomic_sub_return(1, v)
#define atomic_dec_and_test(v) (atomic_sub_return(1, v) == 0)
#define atomic_inc_and_test(v) (atomic_add_return(1, v) == 0)
#define atomic_xchg(v, n) __atomic_exchange_n(&(v)->counter, (n), __ATOMIC_SEQ_CST)
#define atomic_or(i, v) ((void)__atomic_or_fetch(&(v)->counter, (i), __ATOMIC_SEQ_CST))
#define atomic_andnot(i, v) ((void)__atomic_and_fetch(&(v)->counter, ~(i), __ATOMIC_SEQ_CST))

static inline int atomic_cmpxchg(atomic_t *v, int o, int n)
{
	__atomic_compare_exchange_n(&v->counter, &o, n, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
	return o;
}

static inline bool atomic_inc_not_zero(atomic_t *v)
{
	int c = atomic_read(v);
	while (c) {
		if (__atomic_compare_exchange_n(&v->counter, &c, c + 1, false, __ATOMIC_SEQ_CST,
						__ATOMIC_SEQ_CST))
			return true;
	}
	return false;
}

#define atomic64_read(v) __atomic_load_n(&(v)->counter, __ATOMIC_RELAXED)
#define atomic64_set(v, i) __atomic_store_n(&(v)->counter, (i), __ATOMIC_RELAXED)
#define atomic64_inc_return(v) __atomic_add_fetch(&(v)->counter, 1, __ATOMIC_SEQ_CST)
#define xchg(p, n) __atomic_exchange_n((p), (n), __ATOMIC_SEQ_CST)
#define cmpxchg(p, o, n)                                                            \
	({                                                                          \
		__typeof__(*(p)) _o = (o);                                          \
		__atomic_compare_exchange_n((p), &_o, (n), false, __ATOMIC_SEQ_CST, \
					    __ATOMIC_SEQ_CST);                      \
		_o;                                                                 \
	})

typedef struct {
	atomic_t refs;
} refcount_t;

#define REFCOUNT_INIT(n) {.refs = ATOMIC_INIT(n)}
#define refcount_set(r, n) atomic_set(&(r)->refs, (n))
#define refcount_read(r) atomic_read(&(r)->refs)
#define refcount_inc(r) atomic_inc(&(r)->refs)
#define refcount_inc_not_zero(r) atomic_inc_not_zero(&(r)->refs)
#define refcount_dec(r) atomic_dec(&(r)->refs)
#define refcount_dec_and_test(r) atomic_dec_and_test(&(r)->refs)
struct mutex;
struct spinlock;
bool refcount_dec_and_mutex_lock(refcount_t *r, struct mutex *lock);

struct kref {
	refcount_t refcount;
};

#define KREF_INIT(n) {.refcount = REFCOUNT_INIT(n)}

static inline void kref_init(struct kref *k)
{
	refcount_set(&k->refcount, 1);
}

static inline void kref_get(struct kref *k)
{
	refcount_inc(&k->refcount);
}

static inline unsigned int kref_read(const struct kref *k)
{
	return atomic_read(&k->refcount.refs);
}

static inline int kref_put(struct kref *k, void (*release)(struct kref *))
{
	if (refcount_dec_and_test(&k->refcount)) {
		release(k);
		return 1;
	}
	return 0;
}

static inline int kref_get_unless_zero(struct kref *k)
{
	return refcount_inc_not_zero(&k->refcount);
}

struct mutex {
	void *lck;
};

#define DEFINE_MUTEX(n) struct mutex n = {NULL}

static inline void mutex_init(struct mutex *m)
{
	m->lck = switch_linux_mutex_alloc();
}

static inline void mutex_destroy(struct mutex *m)
{
	switch_linux_mutex_free(m->lck);
	m->lck = NULL;
}

static inline void mutex_lock(struct mutex *m)
{
	switch_linux_mutex_lock(switch_linux_mutex_of(&m->lck));
}

static inline void mutex_unlock(struct mutex *m)
{
	switch_linux_mutex_unlock(m->lck);
}

static inline int mutex_trylock(struct mutex *m)
{
	return switch_linux_mutex_trylock(switch_linux_mutex_of(&m->lck)) ? 1 : 0;
}

#define mutex_lock_nested(m, s) mutex_lock(m)
#define __mutex_init(m, n, k)  \
	do {                   \
		(void)(k);     \
		mutex_init(m); \
	} while (0)
bool mutex_is_locked(struct mutex *m);
#define mutex_lock_interruptible(m) (mutex_lock(m), 0)

/* kits/SwitchLinux: a word in the lock, preemption off while held, the _irq forms mask interrupts as Linux's do. */
typedef struct spinlock {
	switch_linux_spinlock lock;
} spinlock_t;

#define DEFINE_SPINLOCK(n) spinlock_t n = {SWITCH_LINUX_SPINLOCK_INIT}
#define __SPIN_LOCK_UNLOCKED(n) {SWITCH_LINUX_SPINLOCK_INIT}
#define spin_lock_init(s) ((s)->lock.holder = 0)
#define spin_lock(s) switch_linux_spin_lock(&(s)->lock)
#define spin_unlock(s) switch_linux_spin_unlock(&(s)->lock)
#define spin_trylock(s) switch_linux_spin_trylock(&(s)->lock)
#define spin_lock_irqsave(s, f) ((f) = switch_linux_spin_lock_irqsave(&(s)->lock))
#define spin_unlock_irqrestore(s, f) switch_linux_spin_unlock_irqrestore(&(s)->lock, (f))
#define spin_lock_irq(s) switch_linux_spin_lock_irq(&(s)->lock)
#define spin_unlock_irq(s) switch_linux_spin_unlock_irq(&(s)->lock)
#define spin_lock_bh(s) spin_lock(s)
#define spin_unlock_bh(s) spin_unlock(s)
#define assert_spin_locked(s) BUG_ON(!switch_linux_spin_is_locked(&(s)->lock))

/* Linux's rwlock_t spins with preemption off: nvkm takes one with interrupts off inside nvkm_intr's spinlock
 * (nvkm_event_ntfy), where a sleeping lck_rw is not allowed (xnu-12377 asserts it). Readers may nest. */
typedef switch_linux_rwlock rwlock_t;

#define rwlock_init(l) ((l)->state = 0)
#define read_lock(l) switch_linux_read_lock(l)
#define read_unlock(l) switch_linux_read_unlock(l)
#define write_lock(l) switch_linux_write_lock(l)
#define write_unlock(l) switch_linux_write_unlock(l)
#define read_lock_irqsave(l, f) ((f) = switch_linux_read_lock_irqsave(l))
#define read_unlock_irqrestore(l, f) switch_linux_read_unlock_irqrestore((l), (f))
#define write_lock_irqsave(l, f) ((f) = switch_linux_write_lock_irqsave(l))
#define write_unlock_irqrestore(l, f) switch_linux_write_unlock_irqrestore((l), (f))
#define read_lock_irq(l) switch_linux_read_lock_irq(l)
#define read_unlock_irq(l) switch_linux_read_unlock_irq(l)
#define write_lock_irq(l) switch_linux_write_lock_irq(l)
#define write_unlock_irq(l) switch_linux_write_unlock_irq(l)

struct rw_semaphore {
	void *lck;
};

static inline void init_rwsem(struct rw_semaphore *s)
{
	s->lck = switch_linux_rwsem_alloc();
}

static inline void down_read(struct rw_semaphore *s)
{
	switch_linux_down_read(switch_linux_rwsem_of(&s->lck));
}

static inline void up_read(struct rw_semaphore *s)
{
	switch_linux_up_read(s->lck);
}

static inline void down_write(struct rw_semaphore *s)
{
	switch_linux_down_write(switch_linux_rwsem_of(&s->lck));
}

static inline void up_write(struct rw_semaphore *s)
{
	switch_linux_up_write(s->lck);
}

#define down_read_nested(s, n) down_read(s)
#define down_write_nested(s, n) down_write(s)
#define local_irq_save(f) ((f) = switch_linux_irq_save())
#define local_irq_restore(f) switch_linux_irq_restore(f)
#define preempt_disable() _disable_preemption()
#define preempt_enable() _enable_preemption()

struct list_head {
	struct list_head *next, *prev;
};

#define LIST_HEAD_INIT(n) {&(n), &(n)}
#define LIST_HEAD(n) struct list_head n = LIST_HEAD_INIT(n)

static inline void INIT_LIST_HEAD(struct list_head *l)
{
	l->next = l;
	l->prev = l;
}

static inline void __list_add(struct list_head *n, struct list_head *p, struct list_head *x)
{
	x->prev = n;
	n->next = x;
	n->prev = p;
	p->next = n;
}

static inline void list_add(struct list_head *n, struct list_head *h)
{
	__list_add(n, h, h->next);
}

static inline void list_add_tail(struct list_head *n, struct list_head *h)
{
	__list_add(n, h->prev, h);
}

static inline void __list_del(struct list_head *p, struct list_head *n)
{
	n->prev = p;
	p->next = n;
}

static inline void list_del(struct list_head *e)
{
	__list_del(e->prev, e->next);
	e->next = (void *)0x100;
	e->prev = (void *)0x122;
}

static inline void list_del_init(struct list_head *e)
{
	__list_del(e->prev, e->next);
	INIT_LIST_HEAD(e);
}

static inline void list_replace(struct list_head *o, struct list_head *n)
{
	n->next = o->next;
	n->next->prev = n;
	n->prev = o->prev;
	n->prev->next = n;
}

static inline void list_move(struct list_head *e, struct list_head *h)
{
	__list_del(e->prev, e->next);
	list_add(e, h);
}

static inline void list_move_tail(struct list_head *e, struct list_head *h)
{
	__list_del(e->prev, e->next);
	list_add_tail(e, h);
}

static inline int list_empty(const struct list_head *h)
{
	return READ_ONCE(h->next) == h;
}

static inline int list_is_last(const struct list_head *e, const struct list_head *h)
{
	return e->next == h;
}

static inline int list_is_first(const struct list_head *e, const struct list_head *h)
{
	return e->prev == h;
}

static inline int list_is_singular(const struct list_head *h)
{
	return !list_empty(h) && h->next == h->prev;
}

static inline void __list_splice(const struct list_head *l, struct list_head *p,
				 struct list_head *n)
{
	struct list_head *f = l->next;
	struct list_head *t = l->prev;
	f->prev = p;
	p->next = f;
	t->next = n;
	n->prev = t;
}

static inline void list_splice(const struct list_head *l, struct list_head *h)
{
	if (!list_empty(l))
		__list_splice(l, h, h->next);
}

static inline void list_splice_tail(struct list_head *l, struct list_head *h)
{
	if (!list_empty(l))
		__list_splice(l, h->prev, h);
}

static inline void list_splice_init(struct list_head *l, struct list_head *h)
{
	if (!list_empty(l)) {
		__list_splice(l, h, h->next);
		INIT_LIST_HEAD(l);
	}
}

static inline void list_splice_tail_init(struct list_head *l, struct list_head *h)
{
	if (!list_empty(l)) {
		__list_splice(l, h->prev, h);
		INIT_LIST_HEAD(l);
	}
}

static inline size_t list_count_nodes(struct list_head *h)
{
	size_t n = 0;
	for (struct list_head *p = h->next; p != h; p = p->next)
		n++;
	return n;
}

#define list_entry(p, t, m) container_of(p, t, m)
#define list_first_entry(h, t, m) list_entry((h)->next, t, m)
#define list_last_entry(h, t, m) list_entry((h)->prev, t, m)
#define list_first_entry_or_null(h, t, m)                   \
	({                                                  \
		struct list_head *_h = (h);                 \
		struct list_head *_n = READ_ONCE(_h->next); \
		_n != _h ? list_entry(_n, t, m) : NULL;     \
	})
#define list_next_entry(p, m) list_entry((p)->m.next, __typeof__(*(p)), m)
#define list_prev_entry(p, m) list_entry((p)->m.prev, __typeof__(*(p)), m)
#define list_entry_is_head(p, h, m) (&(p)->m == (h))
#define list_for_each(p, h) for ((p) = (h)->next; (p) != (h); (p) = (p)->next)
#define list_for_each_safe(p, n, h) \
	for ((p) = (h)->next, (n) = (p)->next; (p) != (h); (p) = (n), (n) = (p)->next)
#define list_for_each_entry(p, h, m)                                                       \
	for ((p) = list_first_entry(h, __typeof__(*(p)), m); !list_entry_is_head(p, h, m); \
	     (p) = list_next_entry(p, m))
#define list_for_each_entry_reverse(p, h, m)                                              \
	for ((p) = list_last_entry(h, __typeof__(*(p)), m); !list_entry_is_head(p, h, m); \
	     (p) = list_prev_entry(p, m))
#define list_for_each_entry_continue(p, h, m) \
	for ((p) = list_next_entry(p, m); !list_entry_is_head(p, h, m); (p) = list_next_entry(p, m))
#define list_for_each_entry_continue_reverse(p, h, m) \
	for ((p) = list_prev_entry(p, m); !list_entry_is_head(p, h, m); (p) = list_prev_entry(p, m))
#define list_for_each_entry_from(p, h, m) \
	for (; !list_entry_is_head(p, h, m); (p) = list_next_entry(p, m))
#define list_for_each_entry_from_reverse(p, h, m) \
	for (; !list_entry_is_head(p, h, m); (p) = list_prev_entry(p, m))
#define list_for_each_entry_safe(p, n, h, m)                                              \
	for ((p) = list_first_entry(h, __typeof__(*(p)), m), (n) = list_next_entry(p, m); \
	     !list_entry_is_head(p, h, m); (p) = (n), (n) = list_next_entry(n, m))
#define list_for_each_entry_safe_reverse(p, n, h, m)                                     \
	for ((p) = list_last_entry(h, __typeof__(*(p)), m), (n) = list_prev_entry(p, m); \
	     !list_entry_is_head(p, h, m); (p) = (n), (n) = list_prev_entry(n, m))
#define list_for_each_entry_safe_from(p, n, h, m)                       \
	for ((n) = list_next_entry(p, m); !list_entry_is_head(p, h, m); \
	     (p) = (n), (n) = list_next_entry(n, m))
#define list_for_each_entry_safe_continue(p, n, h, m)                  \
	for ((p) = list_next_entry(p, m), (n) = list_next_entry(p, m); \
	     !list_entry_is_head(p, h, m); (p) = (n), (n) = list_next_entry(n, m))

static inline void list_rotate_left(struct list_head *h)
{
	if (!list_empty(h))
		list_move_tail(h->next, h);
}

void list_sort(void *priv, struct list_head *head,
	       int (*cmp)(void *priv, const struct list_head *a, const struct list_head *b));

/* rbtree: shim/linux.c's own implementation */
struct rb_node {
	unsigned long __rb_parent_color;
	struct rb_node *rb_right;
	struct rb_node *rb_left;
} __aligned(sizeof(long));

struct rb_root {
	struct rb_node *rb_node;
};

#define RB_ROOT          \
	(struct rb_root) \
	{                \
		NULL     \
	}
#define rb_parent(r) ((struct rb_node *)((r)->__rb_parent_color & ~3UL))
#define rb_entry(p, t, m) container_of(p, t, m)
#define rb_entry_safe(p, t, m)                  \
	({                                      \
		__typeof__(p) _p = (p);         \
		_p ? rb_entry(_p, t, m) : NULL; \
	})
#define RB_EMPTY_ROOT(root) (READ_ONCE((root)->rb_node) == NULL)
#define RB_EMPTY_NODE(n) ((n)->__rb_parent_color == (unsigned long)(n))
#define RB_CLEAR_NODE(n) ((n)->__rb_parent_color = (unsigned long)(n))

static inline void rb_link_node(struct rb_node *n, struct rb_node *parent, struct rb_node **link)
{
	n->__rb_parent_color = (unsigned long)parent;
	n->rb_left = n->rb_right = NULL;
	*link = n;
}

void rb_insert_color(struct rb_node *, struct rb_root *);
void rb_erase(struct rb_node *, struct rb_root *);
struct rb_node *rb_first(const struct rb_root *);
struct rb_node *rb_last(const struct rb_root *);
struct rb_node *rb_next(const struct rb_node *);
struct rb_node *rb_prev(const struct rb_node *);

#define BITS_TO_LONGS(n) DIV_ROUND_UP(n, BITS_PER_LONG)
#define DECLARE_BITMAP(n, bits) unsigned long n[BITS_TO_LONGS(bits)]
#define BIT_WORD(n) ((n) / BITS_PER_LONG)
#define BIT_MASK(n) (1UL << ((n) % BITS_PER_LONG))

static inline void __set_bit(unsigned long n, volatile unsigned long *a)
{
	a[BIT_WORD(n)] |= BIT_MASK(n);
}

static inline void __clear_bit(unsigned long n, volatile unsigned long *a)
{
	a[BIT_WORD(n)] &= ~BIT_MASK(n);
}

/* NOLINTNEXTLINE(readability-non-const-parameter): the atomic below writes through a */
static inline void set_bit(unsigned long n, volatile unsigned long *a)
{
	__atomic_or_fetch(&a[BIT_WORD(n)], BIT_MASK(n), __ATOMIC_SEQ_CST);
}

/* NOLINTNEXTLINE(readability-non-const-parameter): the atomic below writes through a */
static inline void clear_bit(unsigned long n, volatile unsigned long *a)
{
	__atomic_and_fetch(&a[BIT_WORD(n)], ~BIT_MASK(n), __ATOMIC_SEQ_CST);
}

static inline bool test_bit(unsigned long n, const volatile unsigned long *a)
{
	return (a[BIT_WORD(n)] & BIT_MASK(n)) != 0;
}

/* NOLINTNEXTLINE(readability-non-const-parameter): the atomic below writes through a */
static inline bool test_and_set_bit(unsigned long n, volatile unsigned long *a)
{
	return (__atomic_fetch_or(&a[BIT_WORD(n)], BIT_MASK(n), __ATOMIC_SEQ_CST) & BIT_MASK(n)) !=
	       0;
}

/* NOLINTNEXTLINE(readability-non-const-parameter): the atomic below writes through a */
static inline bool test_and_clear_bit(unsigned long n, volatile unsigned long *a)
{
	return (__atomic_fetch_and(&a[BIT_WORD(n)], ~BIT_MASK(n), __ATOMIC_SEQ_CST) &
		BIT_MASK(n)) != 0;
}

unsigned long find_next_bit(const unsigned long *a, unsigned long size, unsigned long off);
unsigned long find_next_zero_bit(const unsigned long *a, unsigned long size, unsigned long off);
#define find_first_bit(a, s) find_next_bit(a, s, 0)
#define find_first_zero_bit(a, s) find_next_zero_bit(a, s, 0)
#define for_each_set_bit(b, a, s) \
	for ((b) = find_first_bit(a, s); (b) < (s); (b) = find_next_bit(a, s, (b) + 1))
#define for_each_clear_bit(b, a, s) \
	for ((b) = find_first_zero_bit(a, s); (b) < (s); (b) = find_next_zero_bit(a, s, (b) + 1))
void bitmap_set(unsigned long *map, unsigned int start, unsigned int n);
void bitmap_clear(unsigned long *map, unsigned int start, unsigned int n);
unsigned int bitmap_weight(const unsigned long *map, unsigned int bits);

static inline void bitmap_zero(unsigned long *d, unsigned int n)
{
	memset(d, 0, BITS_TO_LONGS(n) * sizeof(long));
}

static inline void bitmap_fill(unsigned long *d, unsigned int n)
{
	memset(d, 0xff, BITS_TO_LONGS(n) * sizeof(long));
}

static inline bool bitmap_empty(const unsigned long *s, unsigned int n)
{
	return find_first_bit(s, n) >= n;
}

static inline bool bitmap_full(const unsigned long *s, unsigned int n)
{
	return find_first_zero_bit(s, n) >= n;
}

static inline void bitmap_copy(unsigned long *d, const unsigned long *s, unsigned int n)
{
	memcpy(d, s, BITS_TO_LONGS(n) * sizeof(long));
}

#define GFP_KERNEL 0x01u
#define GFP_ATOMIC 0x02u
#define GFP_NOWAIT 0x04u
#define GFP_DMA32 0x08u
#define GFP_HIGHUSER 0x10u
#define GFP_USER 0x20u
#define __GFP_ZERO 0x100u
#define __GFP_NOWARN 0x200u
#define __GFP_RETRY_MAYFAIL 0x400u
#define __GFP_HIGHMEM 0x800u
#define __GFP_DMA32 GFP_DMA32

static inline void *kmalloc(size_t s, gfp_t g)
{
	return switch_linux_malloc(s, !!(g & __GFP_ZERO));
}

static inline void *kzalloc(size_t s, gfp_t g)
{
	(void)g;
	return switch_linux_malloc(s, 1);
}

static inline void kfree(const void *p)
{
	switch_linux_free(p);
}

static inline void *kmalloc_array(size_t n, size_t s, gfp_t g)
{
	size_t b = 0;
	return __builtin_mul_overflow(n, s, &b) ? NULL : kmalloc(b, g);
}

static inline void *kcalloc(size_t n, size_t s, gfp_t g)
{
	return kmalloc_array(n, s, g | __GFP_ZERO);
}

#define kvmalloc kmalloc
#define kvzalloc kzalloc
#define kvmalloc_array kmalloc_array
#define kvcalloc kcalloc
#define kvfree kfree
#define vmalloc(s) kmalloc(s, GFP_KERNEL)
#define vzalloc(s) kzalloc(s, GFP_KERNEL)
#define vfree kfree
#define kzfree kfree
#define kfree_sensitive kfree
void *krealloc(const void *p, size_t s, gfp_t g);
#define kzalloc_obj(o, ...) ((__typeof__(o) *)kzalloc(sizeof(o), GFP_KERNEL))
#define kmalloc_obj(o, ...) ((__typeof__(o) *)kmalloc(sizeof(o), GFP_KERNEL))
#define kzalloc_objs(o, n, ...) ((__typeof__(o) *)kcalloc((n), sizeof(o), GFP_KERNEL))
#define kmalloc_objs(o, n, ...) ((__typeof__(o) *)kmalloc_array((n), sizeof(o), GFP_KERNEL))
#define kvmalloc_objs(o, n, ...) kmalloc_objs(o, n)
#define kvzalloc_objs(o, n, ...) kzalloc_objs(o, n)
#define kzalloc_flex(o, fam, n, ...) \
	((__typeof__(o) *)kzalloc(struct_size(&(o), fam, (n)), GFP_KERNEL))
#define kmalloc_flex(o, fam, n, ...) \
	((__typeof__(o) *)kmalloc(struct_size(&(o), fam, (n)), GFP_KERNEL))

/* pages come from the GPU pool (nvxnu_pool_*): struct page is a pool page descriptor */
struct page {
	u64 pa;
};

int nvxnu_pool_setup(void);
unsigned long nvxnu_pool_used_pages(void);
/* free pool pages already zeroed: what a ready-only allocation (nvxnu_pool_alloc_ready, chunks) can take */
unsigned long nvxnu_pool_ready_pages(void);
/* BO pages in the pool (shim/linux.c): an allocation zeroes only pages a plain free left dirty; a range the caller zeroed
 * goes back clean. */
void *nvxnu_pool_alloc_clean(unsigned long size, unsigned long align, unsigned long long *pa);
void *nvxnu_pool_alloc_ready(unsigned long size, unsigned long align, unsigned long reserve,
			     unsigned long long *pa);
void nvxnu_pool_free_zeroed(unsigned long long pa, unsigned long size);
void nvxnu_pool_free_dirty(unsigned long long pa, unsigned long size);
/* `count` zeroed 128 KiB chunks for a compressible BO (TegraGM20BComp.h tgm_comp_pick_chunks): the address of each of their
 * 4 KiB pages, chunk by chunk, in `pas`. -ENOMEM, with nothing taken, when the pool has fewer free chunks. */
int nvxnu_pool_alloc_chunks(unsigned long count, unsigned long long *pas);
/* The GPU scrubber's (gpu_scrub.c "pool scrub"): the first run of free pages not known to be zero, at most max_bytes, claimed
 * (allocated) for the copy engine to zero and nvxnu_pool_free_zeroed to give back; its bytes, 0 when none is left. And
 * how many free pages are not known to be zero. */
unsigned long nvxnu_pool_claim_dirty(unsigned long max_bytes, unsigned long long *pa);
unsigned long nvxnu_pool_dirty_free_pages(void);
struct page *alloc_page(gfp_t gfp);
struct page *alloc_pages(gfp_t gfp, unsigned int order);
void __free_page(struct page *p);
void __free_pages(struct page *p, unsigned int order);

static inline phys_addr_t page_to_phys(const struct page *p)
{
	return p->pa;
}

void *page_address(const struct page *p);
struct page *pfn_to_page(unsigned long pfn);

static inline unsigned long page_to_pfn(const struct page *p)
{
	return p->pa >> PAGE_SHIFT;
}

#define VM_MAP 0x4
#define PAGE_KERNEL ((pgprot_t){0})

static inline pgprot_t pgprot_writecombine(pgprot_t p)
{
	return p;
}

static inline pgprot_t pgprot_noncached(pgprot_t p)
{
	return p;
}

void *vmap(struct page **pages, unsigned int count, unsigned long flags, pgprot_t prot);
void vunmap(const void *addr);

struct dev_pm_domain;

struct device {
	const char *init_name;
	void *driver_data;
	struct dev_pm_domain *pm_domain;
	u64 dma_mask_storage;
	u64 *dma_mask;
	u64 coherent_dma_mask;
};

#define IORESOURCE_MEM 0x00000200
#define IORESOURCE_IRQ 0x00000400

struct resource {
	resource_size_t start, end;
	const char *name;
	unsigned long flags;
};

static inline resource_size_t resource_size(const struct resource *r)
{
	return r->end - r->start + 1;
}

struct platform_device {
	const char *name;
	int id;
	struct device dev;
	u32 num_resources;
	struct resource *resource;
	int irq_stall, irq_nonstall;
};
struct resource *platform_get_resource(struct platform_device *, unsigned int type, unsigned int n);
int platform_get_irq_byname(struct platform_device *, const char *);
void __iomem *devm_platform_ioremap_resource(struct platform_device *, unsigned int index);

static inline void *dev_get_drvdata(const struct device *d)
{
	return d->driver_data;
}

static inline void dev_set_drvdata(struct device *d, void *p)
{
	d->driver_data = p;
}

static inline struct platform_device *to_platform_device(struct device *d)
{
	return container_of(d, struct platform_device, dev);
}

#define dev_is_pci(d) false
#define dev_is_platform(d) true

void __iomem *ioremap(phys_addr_t pa, size_t size);
void __iomem *ioremap_wc(phys_addr_t pa, size_t size);
void iounmap(volatile void __iomem *va);

/* kits/SwitchMMIO's load-acquire: the A57's erratum 832075 workaround, and the ordering Linux's trailing dma_rmb gave. */
static inline u8 ioread8(const volatile void __iomem *p)
{
	return switch_mmio_read8((const volatile u8 *)p, 0);
}

static inline u16 ioread16(const volatile void __iomem *p)
{
	return switch_mmio_read16((const volatile u8 *)p, 0);
}

static inline u32 ioread32(const volatile void __iomem *p)
{
	return switch_mmio_read32((const volatile u8 *)p, 0);
}

static inline void iowrite8(u8 v, volatile void __iomem *p)
{
	dma_wmb();
	*(volatile u8 *)p = v;
}

static inline void iowrite16(u16 v, volatile void __iomem *p)
{
	dma_wmb();
	switch_mmio_write16((volatile u8 *)p, 0, v);
}

static inline void iowrite32(u32 v, volatile void __iomem *p)
{
	switch_mmio_write32_publish((volatile u8 *)p, 0, v);
}

#define readl(p) ioread32(p)
#define writel(v, p) iowrite32(v, p)
#define readl_relaxed(p) switch_mmio_read32((const volatile u8 *)(p), 0)
#define writel_relaxed(v, p) switch_mmio_write32((volatile u8 *)(p), 0, (v))
#define ioread16_native ioread16
#define iowrite16_native iowrite16
#define ioread32_native ioread32
#define iowrite32_native iowrite32
#define iowrite64_native(v, p)                               \
	do {                                                 \
		u32 __iomem *_p = (u32 __iomem *)(p);        \
		u64 _v = (v);                                \
		iowrite32_native(lower_32_bits(_v), &_p[0]); \
		iowrite32_native(upper_32_bits(_v), &_p[1]); \
	} while (0)

static inline void memcpy_toio(volatile void __iomem *d, const void *s, size_t n)
{
	memcpy((void *)d, s, n);
}

static inline void memcpy_fromio(void *d, const volatile void __iomem *s, size_t n)
{
	memcpy(d, (const void *)s, n);
}

static inline void *memset32(u32 *s, u32 v, size_t n)
{
	for (size_t i = 0; i < n; i++)
		s[i] = v;
	return s;
}

static inline void *memset64(u64 *s, u64 v, size_t n)
{
	for (size_t i = 0; i < n; i++)
		s[i] = v;
	return s;
}

static inline void memset_io(volatile void __iomem *d, int c, size_t n)
{
	memset((void *)d, c, n);
}

/* DMA: pool backed, no IOMMU */
enum dma_data_direction {
	DMA_BIDIRECTIONAL = 0,
	DMA_TO_DEVICE = 1,
	DMA_FROM_DEVICE = 2,
	DMA_NONE = 3
};

#define DMA_BIT_MASK(n) (((n) == 64) ? ~0ULL : ((1ULL << (n)) - 1))
#define DMA_ATTR_WEAK_ORDERING (1UL << 1)
#define DMA_ATTR_WRITE_COMBINE (1UL << 2)
#define DMA_ATTR_NO_KERNEL_MAPPING (1UL << 4)
#define DMA_ATTR_FORCE_CONTIGUOUS (1UL << 6)
#define DMA_MAPPING_ERROR (~(dma_addr_t)0)
void *dma_alloc_attrs(struct device *, size_t, dma_addr_t *, gfp_t, unsigned long attrs);
void dma_free_attrs(struct device *, size_t, void *, dma_addr_t, unsigned long attrs);
#define dma_alloc_coherent(d, s, h, g) dma_alloc_attrs(d, s, h, g, 0)
#define dma_alloc_noncoherent(d, s, h, dir, g) dma_alloc_attrs(d, s, h, g, 0)
#define dma_free_noncoherent(d, s, v, h, dir) dma_free_attrs(d, s, v, h, 0)
#define dma_free_coherent(d, s, v, h) dma_free_attrs(d, s, v, h, 0)
dma_addr_t dma_map_page(struct device *, struct page *, size_t off, size_t size,
			enum dma_data_direction);
void dma_unmap_page(struct device *, dma_addr_t, size_t, enum dma_data_direction);

static inline int dma_mapping_error(struct device *d, dma_addr_t a)
{
	return a == DMA_MAPPING_ERROR;
}

static inline int dma_set_mask(struct device *d, u64 m)
{
	d->dma_mask_storage = m;
	d->dma_mask = &d->dma_mask_storage;
	return 0;
}

static inline int dma_set_coherent_mask(struct device *d, u64 m)
{
	d->coherent_dma_mask = m;
	return 0;
}

static inline int dma_set_mask_and_coherent(struct device *d, u64 m)
{
	dma_set_mask(d, m);
	return dma_set_coherent_mask(d, m);
}

void dma_sync_single_for_device(struct device *, dma_addr_t, size_t, enum dma_data_direction);
void dma_sync_single_for_cpu(struct device *, dma_addr_t, size_t, enum dma_data_direction);

#define jiffies ((unsigned long)(switch_linux_nanotime() / (NSEC_PER_SEC / HZ)))

static inline ktime_t ktime_get(void)
{
	return (ktime_t)switch_linux_nanotime();
}

#define ktime_get_raw ktime_get
#define ktime_get_ns() ((u64)ktime_get())

static inline s64 ktime_to_ns(ktime_t t)
{
	return t;
}

static inline s64 ktime_to_us(ktime_t t)
{
	return t / 1000;
}

static inline s64 ktime_to_ms(ktime_t t)
{
	return t / 1000000;
}

static inline ktime_t ktime_sub(ktime_t a, ktime_t b)
{
	return a - b;
}

static inline ktime_t ktime_add(ktime_t a, ktime_t b)
{
	return a + b;
}

static inline ktime_t ktime_add_ns(ktime_t a, u64 n)
{
	return a + (s64)n;
}

static inline ktime_t ktime_add_us(ktime_t a, u64 n)
{
	return a + (s64)n * 1000;
}

static inline ktime_t ktime_add_ms(ktime_t a, u64 n)
{
	return a + (s64)n * 1000000;
}

static inline int ktime_compare(ktime_t a, ktime_t b)
{
	return a < b ? -1 : a > b;
}

static inline bool ktime_after(ktime_t a, ktime_t b)
{
	return a > b;
}

static inline bool ktime_before(ktime_t a, ktime_t b)
{
	return a < b;
}

static inline s64 ktime_us_delta(ktime_t a, ktime_t b)
{
	return (a - b) / 1000;
}

#define ns_to_ktime(n) ((ktime_t)(n))
#define ms_to_ktime(m) ((ktime_t)(m) * 1000000)

static inline void udelay(unsigned long us)
{
	switch_linux_udelay(us);
}

static inline void ndelay(unsigned long ns)
{
	switch_linux_udelay(DIV_ROUND_UP(ns, 1000));
}

static inline void mdelay(unsigned long ms)
{
	switch_linux_udelay((u64)ms * 1000);
}

static inline void msleep(unsigned int ms)
{
	switch_linux_usleep_range((u64)ms * 1000, (u64)ms * 1000);
}

static inline unsigned long msleep_interruptible(unsigned int ms)
{
	msleep(ms);
	return 0;
}

static inline void usleep_range(unsigned long lo, unsigned long hi)
{
	switch_linux_usleep_range(lo, hi);
}

static inline void fsleep(unsigned long us)
{
	switch_linux_usleep_range(us, us);
}

static inline unsigned long msecs_to_jiffies(unsigned int m)
{
	return DIV_ROUND_UP(m * HZ, 1000);
}

static inline unsigned long usecs_to_jiffies(unsigned int u)
{
	return DIV_ROUND_UP(u * HZ, 1000000);
}

static inline unsigned int jiffies_to_msecs(unsigned long j)
{
	return (unsigned int)(j * (1000 / HZ));
}

static inline unsigned long nsecs_to_jiffies(u64 n)
{
	return n / (NSEC_PER_SEC / HZ);
}

#define time_after(a, b) ((long)((b) - (a)) < 0)
#define time_before(a, b) time_after(b, a)
#define time_after_eq(a, b) ((long)((a) - (b)) >= 0)
#define time_before_eq(a, b) time_after_eq(b, a)
#define MAX_SCHEDULE_TIMEOUT LONG_MAX

static inline void cond_resched(void)
{
	switch_linux_cond_resched();
}

static inline void schedule(void)
{
	switch_linux_cond_resched();
}

long schedule_timeout(long timeout);

/* Every wait sleeps in nvxnu_idle() until the GPU interrupt work loop (or wake_up()/complete() from a work item)
 * wakes it or its timeout passes; Linux semantics otherwise (the condition is tested once more at the end). */
typedef struct wait_queue_head {
	atomic_t seq;
} wait_queue_head_t;

static inline void init_waitqueue_head(wait_queue_head_t *w)
{
	atomic_set(&w->seq, 0);
}

#define DECLARE_WAIT_QUEUE_HEAD(n) wait_queue_head_t n = {ATOMIC_INIT(0)}

static inline void wake_up(wait_queue_head_t *w)
{
	atomic_inc(&w->seq);
	nvxnu_wake();
}

#define wake_up_all wake_up
#define wake_up_interruptible wake_up
#define wake_up_interruptible_all wake_up
int nvxnu_poll_irqs(void);
/* returns remaining jiffies (>= 1) when cond became true, 0 on timeout */
#define wait_event_timeout(wq, cond, timeout)                                                  \
	({                                                                                     \
		long _left = (long)(timeout);                                                  \
		u64 _end = nvxnu_nanotime() + (u64)_left * (NSEC_PER_SEC / HZ);                \
		for (;;) {                                                                     \
			u32 _seq = nvxnu_wake_seq();                                           \
			if (cond)                                                              \
				break;                                                         \
			if (nvxnu_nanotime() >= _end) {                                        \
				_left = 0;                                                     \
				break;                                                         \
			}                                                                      \
			nvxnu_idle(_seq, _end);                                                \
		}                                                                              \
		if (_left) {                                                                   \
			u64 _n = nvxnu_nanotime();                                             \
			_left = _n >= _end ? 1                                                 \
					   : max_t(long, 1,                                    \
						   (long)((_end - _n) / (NSEC_PER_SEC / HZ))); \
		}                                                                              \
		(void)(wq);                                                                    \
		_left;                                                                         \
	})
#define wait_event(wq, cond)                         \
	do {                                         \
		for (;;) {                           \
			u32 _seq = nvxnu_wake_seq(); \
			if (cond)                    \
				break;               \
			nvxnu_idle(_seq, ~0ULL);     \
		}                                    \
		(void)(wq);                          \
	} while (0)
#define wait_event_interruptible(wq, cond) \
	({                                 \
		wait_event(wq, cond);      \
		0;                         \
	})
#define wait_event_interruptible_timeout wait_event_timeout
#define wait_event_killable(wq, cond) \
	({                            \
		wait_event(wq, cond); \
		0;                    \
	})

struct completion {
	atomic_t done;
};

static inline void init_completion(struct completion *c)
{
	atomic_set(&c->done, 0);
}

static inline void reinit_completion(struct completion *c)
{
	atomic_set(&c->done, 0);
}

static inline void complete(struct completion *c)
{
	atomic_inc(&c->done);
	nvxnu_wake();
}

static inline void complete_all(struct completion *c)
{
	atomic_set(&c->done, 1 << 30);
	nvxnu_wake();
}

#define wait_for_completion_timeout(c, t) wait_event_timeout(*(c), atomic_read(&(c)->done) > 0, t)
#define wait_for_completion(c) wait_event(*(c), atomic_read(&(c)->done) > 0)
#define completion_done(c) (atomic_read(&(c)->done) > 0)

struct work_struct;
typedef void (*work_func_t)(struct work_struct *);

struct work_struct {
	work_func_t func;
	void *thread_call;
};
struct workqueue_struct;
void nvxnu_work_trampoline(void *work);
#define INIT_WORK(w, f)                                                                 \
	do {                                                                            \
		(w)->func = (f);                                                        \
		(w)->thread_call = switch_linux_call_alloc(nvxnu_work_trampoline, (w)); \
	} while (0)

static inline bool schedule_work(struct work_struct *w)
{
	return !switch_linux_call_enter(w->thread_call);
}

#define queue_work(wq, w) schedule_work(w)

/* nvkm's fini paths rely on a flush running a queued item (a runlist's recovery cancelled at suspend left the runlist
 * blocked). */
static inline bool flush_work(struct work_struct *w)
{
	return switch_linux_call_flush(w->thread_call, nvxnu_work_trampoline, w);
}

static inline bool cancel_work_sync(struct work_struct *w)
{
	return switch_linux_call_cancel_wait(w->thread_call);
}

#define system_wq ((struct workqueue_struct *)0)

/* interrupts: the handler runs from nvxnu_poll_irqs */
typedef enum irqreturn {
	IRQ_NONE = 0,
	IRQ_HANDLED = 1,
	IRQ_WAKE_THREAD = 2
} irqreturn_t;

typedef irqreturn_t (*irq_handler_t)(int, void *);
#define IRQF_SHARED 0x80
int request_irq(unsigned int irq, irq_handler_t h, unsigned long flags, const char *name,
		void *dev);
const void *free_irq(unsigned int irq, void *dev);
#define synchronize_irq(i) \
	do {               \
		(void)(i); \
	} while (0)
#define disable_irq(i)     \
	do {               \
		(void)(i); \
	} while (0)
#define enable_irq(i)      \
	do {               \
		(void)(i); \
	} while (0)
#define in_interrupt() 0
#define in_atomic() 0
#define irqs_disabled() (!nvxnu_irqs_enabled())

struct firmware {
	size_t size;
	const u8 *data;
	void *priv;
};

int request_firmware(const struct firmware **fw, const char *name, struct device *dev);
#define request_firmware_direct request_firmware
#define firmware_request_nowarn request_firmware
void release_firmware(const struct firmware *fw);

struct clk;
struct clk *devm_clk_get(struct device *, const char *id);
#define devm_clk_get_optional devm_clk_get
int clk_prepare_enable(struct clk *);
void clk_disable_unprepare(struct clk *);
#define clk_prepare(c) 0
#define clk_unprepare(c)   \
	do {               \
		(void)(c); \
	} while (0)
int clk_enable(struct clk *);
void clk_disable(struct clk *);
unsigned long clk_get_rate(struct clk *);
int clk_set_rate(struct clk *, unsigned long);
long clk_round_rate(struct clk *, unsigned long);
struct regulator;
struct regulator *devm_regulator_get(struct device *, const char *id);
int regulator_enable(struct regulator *);
int regulator_disable(struct regulator *);
int regulator_is_enabled(struct regulator *);
int regulator_get_voltage(struct regulator *);
int regulator_set_voltage(struct regulator *, int min_uv, int max_uv);
struct reset_control;
struct reset_control *devm_reset_control_get(struct device *, const char *id);
#define devm_reset_control_get_exclusive devm_reset_control_get
int reset_control_assert(struct reset_control *);
int reset_control_deassert(struct reset_control *);
int reset_control_reset(struct reset_control *);
struct tegra_pmc;
struct tegra_pmc *devm_tegra_pmc_get(struct device *);
#define TEGRA_POWERGATE_3D 1
int tegra_pmc_powergate_remove_clamping(struct tegra_pmc *, unsigned int id);
int tegra_powergate_remove_clamping(unsigned int id);

struct tegra_sku_info {
	int sku_id, cpu_process_id, cpu_speedo_id, cpu_speedo_value, cpu_iddq_value;
	int soc_process_id, soc_speedo_id, soc_speedo_value;
	int gpu_process_id, gpu_speedo_id, gpu_iddq_value, gpu_speedo_value;
	int revision;
};
extern struct tegra_sku_info tegra_sku_info;
int tegra_fuse_readl(unsigned long offset, u32 *value);
#define TEGRA_REVISION_A01 1

static inline u16 get_unaligned_le16(const void *p)
{
	u16 v = 0;
	memcpy(&v, p, 2);
	return v;
}

static inline u32 get_unaligned_le32(const void *p)
{
	u32 v = 0;
	memcpy(&v, p, 4);
	return v;
}

static inline u64 get_unaligned_le64(const void *p)
{
	u64 v = 0;
	memcpy(&v, p, 8);
	return v;
}

static inline void put_unaligned_le32(u32 v, void *p)
{
	memcpy(p, &v, 4);
}

#define get_unaligned(p)                      \
	({                                    \
		__typeof__(*(p)) _v;          \
		memcpy(&_v, (p), sizeof(_v)); \
		_v;                           \
	})
#define put_unaligned(v, p)                   \
	do {                                  \
		__typeof__(*(p)) _v = (v);    \
		memcpy((p), &_v, sizeof(_v)); \
	} while (0)
#define le16_to_cpu(x) ((u16)(x))
#define le32_to_cpu(x) ((u32)(x))
#define le64_to_cpu(x) ((u64)(x))
#define cpu_to_le16(x) ((u16)(x))
#define cpu_to_le32(x) ((u32)(x))
#define cpu_to_le64(x) ((u64)(x))
#define le32_to_cpup(p) (*(const u32 *)(p))
#define be32_to_cpu(x) __builtin_bswap32(x)
#define cpu_to_be32(x) __builtin_bswap32(x)
#define swab32(x) __builtin_bswap32(x)

/* scatterlists: only carried, never walked on XNU */
struct scatterlist {
	unsigned long page_link;
	unsigned int offset, length;
	dma_addr_t dma_address;
	unsigned int dma_length;
};

struct sg_table {
	struct scatterlist *sgl;
	unsigned int nents, orig_nents;
};

struct sg_dma_page_iter {
	struct scatterlist *sg;
	unsigned long pg;
	unsigned int nents;
};

#define sg_dma_address(sg) ((sg)->dma_address)
#define sg_dma_len(sg) ((sg)->dma_length)

static inline struct scatterlist *sg_next(struct scatterlist *sg)
{
	return (sg->page_link & 0x2) ? NULL : sg + 1;
}

#define for_each_sgtable_dma_sg(t, sg, i) \
	for ((i) = 0, (sg) = (t)->sgl; (i) < (int)(t)->nents; (i)++, (sg) = sg_next(sg))
#define for_each_sgtable_sg for_each_sgtable_dma_sg

static inline void sg_init_one(struct scatterlist *sg, const void *buf, unsigned int len)
{
	memset(sg, 0, sizeof(*sg));
	sg->page_link = 0x2;
	sg->length = len;
	(void)buf;
}

static inline void sg_dma_mark_bus_address(struct scatterlist *sg)
{
	(void)sg;
}

int dma_map_sgtable(struct device *, struct sg_table *, enum dma_data_direction,
		    unsigned long attrs);
void dma_unmap_sgtable(struct device *, struct sg_table *, enum dma_data_direction,
		       unsigned long attrs);
int sg_alloc_table_from_pages(struct sg_table *, struct page **, unsigned int n, unsigned int off,
			      unsigned long size, gfp_t);
void sg_free_table(struct sg_table *);
int sg_alloc_table(struct sg_table *, unsigned int nents, gfp_t);
struct page *vmalloc_to_page(const void *addr);
int dma_map_sg(struct device *, struct scatterlist *, int nents, enum dma_data_direction);
void dma_unmap_sg(struct device *, struct scatterlist *, int nents, enum dma_data_direction);
struct page *sg_page(struct scatterlist *sg);
void sg_set_page(struct scatterlist *sg, struct page *page, unsigned int len, unsigned int off);

/* idr: only embedded in GSP structures that GM20B never creates */
struct idr {
	void *priv;
	unsigned int next;
};

#define idr_init(i)               \
	do {                      \
		(i)->priv = NULL; \
		(i)->next = 0;    \
	} while (0)
#define idr_destroy(i)     \
	do {               \
		(void)(i); \
	} while (0)

/* IOMMU: the GPU's SMMU swgroup stays bypassed; tegra.c never creates a domain, so these only link */
struct iommu_domain {
	unsigned long pgsize_bitmap;
};

#define IOMMU_READ 1
#define IOMMU_WRITE 2

static inline int iommu_map(struct iommu_domain *d, unsigned long iova, phys_addr_t pa, size_t size,
			    int prot, gfp_t gfp)
{
	(void)d;
	(void)iova;
	(void)pa;
	(void)size;
	(void)prot;
	(void)gfp;
	return -ENODEV;
}

static inline size_t iommu_unmap(struct iommu_domain *d, unsigned long iova, size_t size)
{
	(void)d;
	(void)iova;
	(void)size;
	return 0;
}

/* i2c: GM20B has no nvkm i2c subdev; types exist only so shared headers parse */
struct i2c_algorithm;

struct i2c_adapter {
	const struct i2c_algorithm *algo;
	void *algo_data;
	char name[48];
	struct device dev;
};

struct i2c_msg {
	u16 addr;
	u16 flags;
	u16 len;
	u8 *buf;
};

struct i2c_board_info {
	char type[20];
	unsigned short flags;
	unsigned short addr;
	void *platform_data;
};

#define I2C_M_RD 0x0001
// NOLINTNEXTLINE(bugprone-macro-parentheses): t initializes a char array, which takes a bare string literal
#define I2C_BOARD_INFO(t, a) .type = t, .addr = (a)

static inline int i2c_transfer(struct i2c_adapter *a, struct i2c_msg *m, int n)
{
	(void)a;
	(void)m;
	(void)n;
	return -ENODEV;
}

/* power supply: the Switch is battery powered; nvkm only asks for AC presence */
#define power_supply_is_system_supplied() 1

struct module;
#define THIS_MODULE ((struct module *)0)

struct notifier_block {
	int (*notifier_call)(struct notifier_block *, unsigned long, void *);
};

struct dev_pm_domain {
	int dummy;
};

#define NOTIFY_DONE 0
#define NOTIFY_OK 1
#define capable(c) 1
#define CAP_SYS_ADMIN 21

struct task_struct {
	int pid;
	char comm[16];
};
extern struct task_struct nvxnu_task;
#define current (&nvxnu_task)
#define get_current() current
#define task_pid_nr(t) ((t)->pid)
#define ERESTARTSYS 512
#define fatal_signal_pending(t) 0
#define signal_pending(t) 0
#define __GFP_NORETRY 0
#define rcu_read_lock() \
	do {            \
	} while (0)
#define rcu_read_unlock() \
	do {              \
	} while (0)
#define acpi_handle void *
#define kmemleak_ignore(p) \
	do {               \
		(void)(p); \
	} while (0)
#define print_hex_dump(...) \
	do {                \
	} while (0)
#define ACPI_HANDLE(d) NULL

#endif
