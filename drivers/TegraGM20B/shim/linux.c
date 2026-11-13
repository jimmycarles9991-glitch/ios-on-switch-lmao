// SPDX-License-Identifier: MIT
/*
 * Linux kernel API used by the vendored nvkm code, implemented on top of kits/SwitchLinux and nvxnu.h
 * (shim/xnu_glue.cpp).
 * Original code; nothing here is copied from Linux (the rbtree is a textbook red-black tree using
 * Linux's node layout so nvkm's rb_link_node()/rb_entry() work unchanged).
 */
#include <nvif/os.h>
#include <core/device.h>

#include "../TegraGM20BComp.h"

struct task_struct nvxnu_task = {0, "kernel"};
struct tegra_sku_info tegra_sku_info;

const char *dev_name(const struct device *dev)
{
	return dev && dev->init_name ? dev->init_name : "gpu";
}

bool mutex_is_locked(struct mutex *m)
{
	if (!mutex_trylock(m))
		return true;
	mutex_unlock(m);
	return false;
}

bool refcount_dec_and_mutex_lock(refcount_t *r, struct mutex *lock)
{
	int old = atomic_read(&r->refs);
	while (old > 1) {
		if (__atomic_compare_exchange_n(&r->refs.counter, &old, old - 1, false,
						__ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
			return false;
	}
	mutex_lock(lock);
	if (!refcount_dec_and_test(r)) {
		mutex_unlock(lock);
		return false;
	}
	return true;
}

long schedule_timeout(long timeout)
{
	if (timeout > 0 && timeout != MAX_SCHEDULE_TIMEOUT) {
		const u64 us = (u64)timeout * (USEC_PER_SEC / HZ);
		switch_linux_usleep_range(us, us);
	}
	return 0;
}

void nvxnu_work_trampoline(void *work)
{
	struct work_struct *w = work;
	w->func(w);
}

/* request_irq() only records the handler; nvxnu_poll_irqs() runs it in thread context, from the GPU interrupt work
 * loop (shim/xnu_glue.cpp) and nowhere else. Never from primary interrupt context: nvkm's handlers take mutexes
 * (lck_mtx) as well as spinlocks. */
static struct {
	irq_handler_t handler;
	void *dev;
	unsigned int irq;
} irq_slot;

static int irq_polling;

int request_irq(unsigned int irq, irq_handler_t h, unsigned long flags, const char *name, void *dev)
{
	(void)flags;
	(void)name;
	irq_slot.irq = irq;
	irq_slot.dev = dev;
	__atomic_store_n(&irq_slot.handler, h, __ATOMIC_RELEASE);
	return 0;
}

const void *free_irq(unsigned int irq, void *dev)
{
	(void)irq;
	if (irq_slot.dev == dev)
		__atomic_store_n(&irq_slot.handler, NULL, __ATOMIC_RELEASE);
	return NULL;
}

/* Run the GPU's interrupt handler (nvkm_intr: every MC leaf) once. Returns its irqreturn_t, or -1 when no handler
 * is registered or another thread is already in it (the line is level-triggered, so what that one missed fires
 * again). */
int nvxnu_poll_irqs(void)
{
	irq_handler_t h = __atomic_load_n(&irq_slot.handler, __ATOMIC_ACQUIRE);
	int idle = 0;
	int ret = 0;
	if (!h || !__atomic_compare_exchange_n(&irq_polling, &idle, 1, false, __ATOMIC_ACQUIRE,
					       __ATOMIC_RELAXED))
		return -1;
	ret = (int)h(irq_slot.irq, irq_slot.dev);
	__atomic_store_n(&irq_polling, 0, __ATOMIC_RELEASE);
	return ret;
}

char *kstrdup(const char *s, gfp_t gfp)
{
	return s ? kmemdup(s, strlen(s) + 1, gfp) : NULL;
}

const char *kstrdup_const(const char *s, gfp_t gfp)
{
	return kstrdup(s, gfp);
}

void kfree_const(const void *p)
{
	kfree(p);
}

char *kstrndup(const char *s, size_t max, gfp_t gfp)
{
	size_t n = 0;
	char *d = NULL;
	if (!s)
		return NULL;
	n = strnlen(s, max);
	d = kmalloc(n + 1, gfp);
	if (d) {
		memcpy(d, s, n);
		d[n] = 0;
	}
	return d;
}

void *kmemdup(const void *p, size_t size, gfp_t gfp)
{
	void *d = kmalloc(size, gfp);
	if (d)
		memcpy(d, p, size);
	return d;
}

void *krealloc(const void *p, size_t s, gfp_t g)
{
	void *d = NULL;
	if (!s) {
		kfree(p);
		return NULL;
	}
	d = kmalloc(s, g);
	if (d && p) {
		size_t old = switch_linux_malloc_size(p);
		memcpy(d, p, old < s ? old : s);
		kfree(p);
	}
	return d;
}

char *kasprintf(gfp_t gfp, const char *fmt, ...)
{
	va_list ap; /* NOLINT(cppcoreguidelines-init-variables): va_start initializes it */
	char *s = NULL;
	int n = 0;
	va_start(ap, fmt);
	n = vsnprintf(NULL, 0, fmt, ap);
	va_end(ap);
	s = kmalloc(n + 1, gfp);
	if (s) {
		va_start(ap, fmt);
		vsnprintf(s, n + 1, fmt, ap);
		va_end(ap);
	}
	return s;
}

size_t strcspn(const char *s, const char *reject)
{
	size_t n = 0;
	while (s[n] && !strchr(reject, s[n]))
		n++;
	return n;
}

size_t strspn(const char *s, const char *accept)
{
	size_t n = 0;
	while (s[n] && strchr(accept, s[n]))
		n++;
	return n;
}

char *strstr(const char *h, const char *n)
{
	size_t l = strlen(n);
	for (; *h; h++) {
		if (!strncmp(h, n, l))
			return (char *)h;
	}
	return l ? NULL : (char *)h;
}

ssize_t strscpy(char *dst, const char *src, size_t size)
{
	size_t n = 0;
	if (!size)
		return -E2BIG;
	n = strnlen(src, size);
	if (n == size) {
		memcpy(dst, src, size - 1);
		dst[size - 1] = 0;
		return -E2BIG;
	}
	memcpy(dst, src, n + 1);
	return n;
}

unsigned long simple_strtoul(const char *cp, char **endp, unsigned int base)
{
	unsigned long v = 0;
	if (!base) {
		base = 10;
		if (cp[0] == '0') {
			base = 8;
			if (((unsigned char)cp[1] | 32u) == 'x') {
				base = 16;
				cp += 2;
			}
		}
	} else if (base == 16 && cp[0] == '0' && ((unsigned char)cp[1] | 32u) == 'x') {
		cp += 2;
	}
	for (;; cp++) {
		unsigned int d = 0;
		if (isdigit(*cp)) {
			d = *cp - '0';
		} else if (((unsigned char)*cp | 32u) >= 'a' && ((unsigned char)*cp | 32u) <= 'f') {
			d = ((unsigned char)*cp | 32u) - 'a' + 10;
		} else {
			break;
		}
		if (d >= base)
			break;
		v = v * base + d;
	}
	if (endp)
		*endp = (char *)cp;
	return v;
}

long simple_strtol(const char *cp, char **endp, unsigned int base)
{
	if (*cp == '-')
		return -(long)simple_strtoul(cp + 1, endp, base);
	return (long)simple_strtoul(cp, endp, base);
}

int kstrtoul(const char *s, unsigned int base, unsigned long *res)
{
	char *end = NULL;
	if (!*s)
		return -EINVAL;
	*res = simple_strtoul(s, &end, base);
	if (*end == '\n')
		end++;
	return *end ? -EINVAL : 0;
}

int kstrtol(const char *s, unsigned int base, long *res)
{
	unsigned long v = 0;
	int neg = *s == '-';
	int ret = kstrtoul(s + neg, base, &v);
	if (!ret)
		*res = neg ? -(long)v : (long)v;
	return ret;
}

/* insertion sort: nvkm only sorts short tables */
void sort(void *base, size_t num, size_t size, int (*cmp)(const void *, const void *),
	  void (*swap_fn)(void *, void *, int))
{
	char *b = base;
	for (size_t i = 1; i < num; i++) {
		for (size_t j = i; j > 0 && cmp(b + (j - 1) * size, b + j * size) > 0; j--) {
			if (swap_fn) {
				swap_fn(b + (j - 1) * size, b + j * size, (int)size);
			} else {
				char *x = b + (j - 1) * size;
				char *y = b + j * size;
				for (size_t k = 0; k < size; k++) {
					char t = x[k];
					x[k] = y[k];
					y[k] = t;
				}
			}
		}
	}
}

void list_sort(void *priv, struct list_head *head,
	       int (*cmp)(void *priv, const struct list_head *a, const struct list_head *b))
{
	LIST_HEAD(sorted);
	while (!list_empty(head)) {
		struct list_head *e = head->next;
		struct list_head *pos = NULL;
		list_del(e);
		for (pos = sorted.prev; pos != &sorted && cmp(priv, pos, e) > 0; pos = pos->prev)
			;
		list_add(e, pos);
	}
	list_splice(&sorted, head);
}

unsigned long find_next_bit(const unsigned long *a, unsigned long size, unsigned long off)
{
	for (; off < size; off++) {
		if (test_bit(off, a))
			return off;
	}
	return size;
}

unsigned long find_next_zero_bit(const unsigned long *a, unsigned long size, unsigned long off)
{
	for (; off < size; off++) {
		if (!test_bit(off, a))
			return off;
	}
	return size;
}

void bitmap_set(unsigned long *map, unsigned int start, unsigned int n)
{
	while (n--)
		__set_bit(start++, map);
}

void bitmap_clear(unsigned long *map, unsigned int start, unsigned int n)
{
	while (n--)
		__clear_bit(start++, map);
}

unsigned int bitmap_weight(const unsigned long *map, unsigned int bits)
{
	unsigned int w = 0;
	for (unsigned int i = 0; i < bits; i++)
		w += (unsigned int)test_bit(i, map);
	return w;
}

#define RB_RED 0
#define RB_BLACK 1
#define rb_color(n) ((n)->__rb_parent_color & 1)
#define rb_is_red(n) ((n) && !rb_color(n))
#define rb_is_black(n) (!(n) || rb_color(n))

static void rb_set_parent(struct rb_node *n, struct rb_node *p)
{
	n->__rb_parent_color = rb_color(n) | (unsigned long)p;
}

static void rb_set_color(struct rb_node *n, int c)
{
	n->__rb_parent_color = (n->__rb_parent_color & ~1UL) | (unsigned long)c;
}

static void rb_replace_child(struct rb_node *old, struct rb_node *new, struct rb_node *parent,
			     struct rb_root *root)
{
	if (!parent) {
		root->rb_node = new;
	} else if (parent->rb_left == old) {
		parent->rb_left = new;
	} else {
		parent->rb_right = new;
	}
}

static void rb_rotate_left(struct rb_node *x, struct rb_root *root)
{
	struct rb_node *y = x->rb_right;
	struct rb_node *p = rb_parent(x);
	x->rb_right = y->rb_left;
	if (y->rb_left)
		rb_set_parent(y->rb_left, x);
	rb_set_parent(y, p);
	rb_replace_child(x, y, p, root);
	y->rb_left = x;
	rb_set_parent(x, y);
}

static void rb_rotate_right(struct rb_node *x, struct rb_root *root)
{
	struct rb_node *y = x->rb_left;
	struct rb_node *p = rb_parent(x);
	x->rb_left = y->rb_right;
	if (y->rb_right)
		rb_set_parent(y->rb_right, x);
	rb_set_parent(y, p);
	rb_replace_child(x, y, p, root);
	y->rb_right = x;
	rb_set_parent(x, y);
}

void rb_insert_color(struct rb_node *n, struct rb_root *root)
{
	struct rb_node *p = NULL;
	struct rb_node *g = NULL;
	struct rb_node *u = NULL;
	rb_set_color(n, RB_RED);
	for (;;) {
		p = rb_parent(n);
		if (!p || !rb_is_red(p))
			break;
		g = rb_parent(p);
		if (p == g->rb_left) {
			u = g->rb_right;
			if (rb_is_red(u)) {
				rb_set_color(p, RB_BLACK);
				rb_set_color(u, RB_BLACK);
				rb_set_color(g, RB_RED);
				n = g;
				continue;
			}
			if (n == p->rb_right) {
				rb_rotate_left(p, root);
				n = p;
				p = rb_parent(n);
			}
			rb_set_color(p, RB_BLACK);
			rb_set_color(g, RB_RED);
			rb_rotate_right(g, root);
		} else {
			u = g->rb_left;
			if (rb_is_red(u)) {
				rb_set_color(p, RB_BLACK);
				rb_set_color(u, RB_BLACK);
				rb_set_color(g, RB_RED);
				n = g;
				continue;
			}
			if (n == p->rb_left) {
				rb_rotate_right(p, root);
				n = p;
				p = rb_parent(n);
			}
			rb_set_color(p, RB_BLACK);
			rb_set_color(g, RB_RED);
			rb_rotate_left(g, root);
		}
	}
	rb_set_color(root->rb_node, RB_BLACK);
}

static void rb_erase_fixup(struct rb_node *x, struct rb_node *parent, struct rb_root *root)
{
	struct rb_node *w = NULL;
	while (x != root->rb_node && rb_is_black(x)) {
		if (x == parent->rb_left) {
			w = parent->rb_right;
			if (rb_is_red(w)) {
				rb_set_color(w, RB_BLACK);
				rb_set_color(parent, RB_RED);
				rb_rotate_left(parent, root);
				w = parent->rb_right;
			}
			/* A red-black tree gives a double-black node a sibling. */
			/* NOLINTNEXTLINE(clang-analyzer-core.NullDereference) */
			if (rb_is_black(w->rb_left) && rb_is_black(w->rb_right)) {
				rb_set_color(w, RB_RED);
				x = parent;
				parent = rb_parent(x);
			} else {
				if (rb_is_black(w->rb_right)) {
					rb_set_color(w->rb_left, RB_BLACK);
					rb_set_color(w, RB_RED);
					rb_rotate_right(w, root);
					w = parent->rb_right;
				}
				rb_set_color(w, rb_color(parent));
				rb_set_color(parent, RB_BLACK);
				if (w->rb_right)
					rb_set_color(w->rb_right, RB_BLACK);
				rb_rotate_left(parent, root);
				x = root->rb_node;
				break;
			}
		} else {
			w = parent->rb_left;
			if (rb_is_red(w)) {
				rb_set_color(w, RB_BLACK);
				rb_set_color(parent, RB_RED);
				rb_rotate_right(parent, root);
				w = parent->rb_left;
			}
			if (rb_is_black(w->rb_left) && rb_is_black(w->rb_right)) {
				rb_set_color(w, RB_RED);
				x = parent;
				parent = rb_parent(x);
			} else {
				if (rb_is_black(w->rb_left)) {
					rb_set_color(w->rb_right, RB_BLACK);
					rb_set_color(w, RB_RED);
					rb_rotate_left(w, root);
					w = parent->rb_left;
				}
				rb_set_color(w, rb_color(parent));
				rb_set_color(parent, RB_BLACK);
				if (w->rb_left)
					rb_set_color(w->rb_left, RB_BLACK);
				rb_rotate_right(parent, root);
				x = root->rb_node;
				break;
			}
		}
	}
	if (x)
		rb_set_color(x, RB_BLACK);
}

void rb_erase(struct rb_node *z, struct rb_root *root)
{
	struct rb_node *child = NULL;
	struct rb_node *parent = NULL;
	int color = 0;

	if (z->rb_left && z->rb_right) {
		struct rb_node *y = z->rb_right;
		while (y->rb_left)
			y = y->rb_left;
		/* y takes z's place in the tree; y's old right child takes y's place */
		child = y->rb_right;
		parent = rb_parent(y);
		color = rb_color(y);
		if (parent == z) {
			parent = y;
		} else {
			if (child)
				rb_set_parent(child, parent);
			parent->rb_left = child;
			y->rb_right = z->rb_right;
			rb_set_parent(z->rb_right, y);
		}
		y->rb_left = z->rb_left;
		rb_set_parent(z->rb_left, y);
		y->__rb_parent_color = z->__rb_parent_color;
		rb_replace_child(z, y, rb_parent(z), root);
	} else {
		child = z->rb_left ? z->rb_left : z->rb_right;
		parent = rb_parent(z);
		color = rb_color(z);
		if (child)
			rb_set_parent(child, parent);
		rb_replace_child(z, child, parent, root);
	}
	if (color == RB_BLACK)
		rb_erase_fixup(child, parent, root);
}

struct rb_node *rb_first(const struct rb_root *root)
{
	struct rb_node *n = root->rb_node;
	if (!n)
		return NULL;
	while (n->rb_left)
		n = n->rb_left;
	return n;
}

struct rb_node *rb_last(const struct rb_root *root)
{
	struct rb_node *n = root->rb_node;
	if (!n)
		return NULL;
	while (n->rb_right)
		n = n->rb_right;
	return n;
}

struct rb_node *rb_next(const struct rb_node *n)
{
	struct rb_node *p = NULL;
	if (RB_EMPTY_NODE(n))
		return NULL;
	if (n->rb_right) {
		n = n->rb_right;
		while (n->rb_left)
			n = n->rb_left;
		return (struct rb_node *)n;
	}
	for (;;) {
		p = rb_parent(n);
		if (!p || n != p->rb_right)
			break;
		n = p;
	}
	return p;
}

struct rb_node *rb_prev(const struct rb_node *n)
{
	struct rb_node *p = NULL;
	if (RB_EMPTY_NODE(n))
		return NULL;
	if (n->rb_left) {
		n = n->rb_left;
		while (n->rb_right)
			n = n->rb_right;
		return (struct rb_node *)n;
	}
	for (;;) {
		p = rb_parent(n);
		if (!p || n != p->rb_left)
			break;
		n = p;
	}
	return p;
}

/* GPU pool pages, DMA, vmap: one physically contiguous pool (nvxnu_pool_*); struct page is an entry of a descriptor
 * array so page_to_phys()/page_address() are O(1). Recent free extents and the fallback bitmap share one spinlock. */
#include "../SwitchGpuPoolAllocation.h"
static struct page *pool_pages;
static unsigned long *pool_map;
/* Free pages not known to hold zeros: set by a free that did not zero (nvkm's, a BO's freed straight back), clear for a
 * page the pool's initial clear or nvxnu_pool_free_zeroed left zero. Client BOs take only ready pages; cold nvkm
 * allocations may claim dirty pages and clear them before exposure. */
static unsigned long *pool_dirty;
static unsigned long pool_npages, pool_used, pool_clean_free;
static struct switch_gpu_pool_extents pool_extents;
static DEFINE_SPINLOCK(pool_lock);

int nvxnu_pool_setup(void)
{
	int zeroed = 0;
	int ret = nvxnu_pool_init(&zeroed);
	if (ret)
		return ret;
	pool_npages = nvxnu_pool_size() >> PAGE_SHIFT;
	pool_pages = kcalloc(pool_npages, sizeof(*pool_pages), GFP_KERNEL);
	pool_map = kcalloc(BITS_TO_LONGS(pool_npages), sizeof(long), GFP_KERNEL);
	pool_dirty = kcalloc(BITS_TO_LONGS(pool_npages), sizeof(long), GFP_KERNEL);
	if (!pool_pages || !pool_map || !pool_dirty)
		return -ENOMEM;
	for (unsigned long i = 0; i < pool_npages; i++)
		pool_pages[i].pa = nvxnu_pool_base_pa() + (i << PAGE_SHIFT);
	/* nothing zeroed them yet: the GPU's scrubber will (gpu_scrub.c "pool scrub") */
	if (!zeroed)
		bitmap_set(pool_dirty, 0, pool_npages);
	switch_gpu_pool_remember(&pool_extents, 0, pool_npages, zeroed);
	pool_clean_free = zeroed ? pool_npages : 0;
	return 0;
}

/* Cold kernel allocations retain a CPU clear path before the CE scrubber is available. */
static void pool_zero_dirty(unsigned long first, unsigned long n, int all)
{
	unsigned long k = 0;
	for (k = 0; k < n; k++) {
		if (all || test_bit(first + k, pool_dirty))
			memset(page_address(&pool_pages[first + k]), 0, PAGE_SIZE);
	}
	spin_lock(&pool_lock);
	bitmap_clear(pool_dirty, first, n);
	spin_unlock(&pool_lock);
}

static long pool_take(unsigned long n, unsigned long align, int ready_only, unsigned long reserve)
{
	long start = -1;
	if (!n || !align || (align & (align - 1)))
		return -EINVAL;
	spin_lock(&pool_lock);
	if (ready_only && (n > pool_clean_free || reserve > pool_clean_free - n)) {
		spin_unlock(&pool_lock);
		return -ENOMEM;
	}
	start = switch_gpu_pool_cached(&pool_extents, n, align, 1);
	if (start < 0)
		start = switch_gpu_pool_find(pool_map, pool_dirty, pool_npages, n, align, 1);
	if (start < 0 && !ready_only)
		start = switch_gpu_pool_cached(&pool_extents, n, align, 0);
	if (start < 0 && !ready_only)
		start = switch_gpu_pool_find(pool_map, pool_dirty, pool_npages, n, align, 0);
	if (start >= 0) {
		switch_gpu_pool_claim(&pool_extents, (unsigned long)start, n);
		pool_clean_free -= switch_gpu_pool_clean_count(pool_dirty, (unsigned long)start, n);
		bitmap_set(pool_map, (unsigned long)start, n);
		pool_used += n;
	}
	spin_unlock(&pool_lock);
	return start < 0 ? -ENOMEM : start;
}

static long pool_alloc(unsigned long n, unsigned long align)
{
	return pool_take(n, align, 0, 0);
}

static void pool_free(unsigned long first, unsigned long n)
{
	spin_lock(&pool_lock);
	bitmap_clear(pool_map, first, n);
	bitmap_set(pool_dirty, first, n);
	switch_gpu_pool_remember(&pool_extents, first, n, 0);
	pool_used -= n;
	spin_unlock(&pool_lock);
}

unsigned long nvxnu_pool_used_pages(void)
{
	return READ_ONCE(pool_used);
}

unsigned long nvxnu_pool_ready_pages(void)
{
	return READ_ONCE(pool_clean_free);
}

static long pool_index(u64 pa)
{
	u64 base = nvxnu_pool_base_pa();
	if (pa < base || pa >= base + ((size_t)pool_npages << PAGE_SHIFT))
		return -1;
	return (pa - base) >> PAGE_SHIFT;
}

struct page *alloc_pages(gfp_t gfp, unsigned int order)
{
	long i = pool_map ? pool_alloc(1UL << order, 1UL << order) : -ENOMEM;
	if (i < 0)
		return NULL;
	/* zeroed whatever the flags: nvkm's callers may rely on pool pages being zero */
	pool_zero_dirty(i, 1UL << order, 0);
	return &pool_pages[i];
}

struct page *alloc_page(gfp_t gfp)
{
	return alloc_pages(gfp, 0);
}

void __free_pages(struct page *p, unsigned int order)
{
	if (p)
		pool_free(p - pool_pages, 1UL << order);
}

void __free_page(struct page *p)
{
	__free_pages(p, 0);
}

void *page_address(const struct page *p)
{
	return (char *)nvxnu_pool_base_va() + ((size_t)(p - pool_pages) << PAGE_SHIFT);
}

struct page *pfn_to_page(unsigned long pfn)
{
	long i = pool_index((u64)pfn << PAGE_SHIFT);
	return i < 0 ? NULL : &pool_pages[i];
}

struct page *vmalloc_to_page(const void *addr)
{
	char *base = nvxnu_pool_base_va();
	if ((const char *)addr < base ||
	    (const char *)addr >= base + ((size_t)pool_npages << PAGE_SHIFT))
		return NULL;
	return &pool_pages[(size_t)((const char *)addr - base) >> PAGE_SHIFT];
}

void *dma_alloc_attrs(struct device *dev, size_t size, dma_addr_t *handle, gfp_t gfp,
		      unsigned long attrs)
{
	unsigned long n = PAGE_ALIGN(size) >> PAGE_SHIFT;
	long i = pool_map ? pool_alloc(n, 1) : -ENOMEM;
	(void)dev;
	(void)gfp;
	(void)attrs;
	if (i < 0)
		return NULL;
	*handle = pool_pages[i].pa;
	pool_zero_dirty(i, n, 0);
	return page_address(&pool_pages[i]);
}

void dma_free_attrs(struct device *dev, size_t size, void *va, dma_addr_t handle,
		    unsigned long attrs)
{
	long i = pool_index(handle);
	(void)dev;
	(void)va;
	(void)attrs;
	if (i >= 0)
		pool_free(i, PAGE_ALIGN(size) >> PAGE_SHIFT);
}

/* Kernel-owned buffers may fall back to a CPU clear; client BOs use nvxnu_pool_alloc_ready. */
void *nvxnu_pool_alloc_clean(unsigned long size, unsigned long align, unsigned long long *pa)
{
	unsigned long n = PAGE_ALIGN(size) >> PAGE_SHIFT;
	long i = pool_map ? pool_alloc(n, align > PAGE_SIZE ? align >> PAGE_SHIFT : 1) : -ENOMEM;
	if (i < 0)
		return NULL;
	pool_zero_dirty(i, n, 0);
	*pa = pool_pages[i].pa;
	return page_address(&pool_pages[i]);
}

void *nvxnu_pool_alloc_ready(unsigned long size, unsigned long align, unsigned long reserve,
			     unsigned long long *pa)
{
	const unsigned long n = PAGE_ALIGN(size) >> PAGE_SHIFT;
	const long i = pool_map ? pool_take(n, align > PAGE_SIZE ? align >> PAGE_SHIFT : 1, 1,
					    reserve >> PAGE_SHIFT)
				: -ENOMEM;
	if (i < 0)
		return NULL;
	*pa = pool_pages[i].pa;
	return page_address(&pool_pages[i]);
}

int nvxnu_pool_alloc_chunks(unsigned long count, unsigned long long *pas)
{
	unsigned long i = 0;
	unsigned long k = 0;
	int dirty = -1;
	if (!pool_map || !count)
		return -ENOMEM;
	/* pas holds the chunk indices first: chunk i's pages go from entry i * TGM_COMP_PAGES on, so expanding from the
	 * last chunk down never overwrites an index not read yet */
	spin_lock(&pool_lock);
	dirty = tgm_comp_pick_chunks(pool_map, pool_dirty, pool_npages, (uint32_t)count, pas);
	if (dirty > 0 || count > pool_clean_free / TGM_COMP_PAGES ||
	    pool_clean_free - count * TGM_COMP_PAGES <
		    SWITCH_GPU_POOL_CONTROL_READY_BYTES / PAGE_SIZE)
		dirty = -ENOMEM;
	for (i = 0; dirty >= 0 && i < count; i++) {
		switch_gpu_pool_claim(&pool_extents, pas[i] * TGM_COMP_PAGES, TGM_COMP_PAGES);
		bitmap_set(pool_map, pas[i] * TGM_COMP_PAGES, TGM_COMP_PAGES);
	}
	if (dirty >= 0) {
		pool_used += count * TGM_COMP_PAGES;
		pool_clean_free -= count * TGM_COMP_PAGES;
	}
	spin_unlock(&pool_lock);
	if (dirty < 0)
		return -ENOMEM;
	for (i = count; i-- > 0;) {
		const unsigned long first = pas[i] * TGM_COMP_PAGES;
		for (k = 0; k < TGM_COMP_PAGES; k++)
			pas[i * TGM_COMP_PAGES + k] = pool_pages[first + k].pa;
	}
	return 0;
}

unsigned long nvxnu_pool_claim_dirty(unsigned long max_bytes, unsigned long long *pa)
{
	unsigned long first = 0;
	unsigned long n = 0;
	unsigned long max = max_bytes >> PAGE_SHIFT;
	unsigned long w = 0;
	unsigned long bits = 0;
	if (!pool_map || !max)
		return 0;
	spin_lock(&pool_lock);
	/* the first free dirty page, a word of the two bitmaps at a time */
	for (w = 0; w < BITS_TO_LONGS(pool_npages); w++) {
		bits = pool_dirty[w] & ~pool_map[w];
		if (bits) {
			first = w * BITS_PER_LONG + __builtin_ctzl(bits);
			break;
		}
	}
	if (w < BITS_TO_LONGS(pool_npages) && first < pool_npages) {
		while (n < max && first + n < pool_npages && test_bit(first + n, pool_dirty) &&
		       !test_bit(first + n, pool_map))
			n++;
		switch_gpu_pool_claim(&pool_extents, first, n);
		bitmap_set(pool_map, first, n);
		pool_used += n;
	}
	spin_unlock(&pool_lock);
	if (!n)
		return 0;
	*pa = pool_pages[first].pa;
	return n << PAGE_SHIFT;
}

unsigned long nvxnu_pool_dirty_free_pages(void)
{
	unsigned long w = 0;
	unsigned long n = 0;
	if (!pool_map)
		return 0;
	spin_lock(&pool_lock);
	for (w = 0; w < BITS_TO_LONGS(pool_npages); w++)
		n += __builtin_popcountl(pool_dirty[w] & ~pool_map[w]);
	spin_unlock(&pool_lock);
	return n;
}

/* A range the caller zeroed (and made visible: the pool is write-combined) goes back clean. */
void nvxnu_pool_free_zeroed(unsigned long long pa, unsigned long size)
{
	long i = pool_index(pa);
	unsigned long n = PAGE_ALIGN(size) >> PAGE_SHIFT;
	if (i < 0)
		return;
	spin_lock(&pool_lock);
	bitmap_clear(pool_map, i, n);
	bitmap_clear(pool_dirty, i, n);
	switch_gpu_pool_remember(&pool_extents, (unsigned long)i, n, 1);
	pool_clean_free += n;
	pool_used -= n;
	spin_unlock(&pool_lock);
}

/* A range a scrub claimed but cannot vouch for goes back dirty, to be claimed and zeroed again. */
void nvxnu_pool_free_dirty(unsigned long long pa, unsigned long size)
{
	long i = pool_index(pa);
	if (i >= 0)
		pool_free((unsigned long)i, PAGE_ALIGN(size) >> PAGE_SHIFT);
}

dma_addr_t dma_map_page(struct device *dev, struct page *page, size_t off, size_t size,
			enum dma_data_direction dir)
{
	(void)dev;
	(void)size;
	(void)dir;
	return page ? page->pa + off : DMA_MAPPING_ERROR;
}

void dma_unmap_page(struct device *dev, dma_addr_t a, size_t s, enum dma_data_direction dir)
{
	(void)dev;
	(void)a;
	(void)s;
	(void)dir;
}

/* pool memory is mapped write-combined, so there is nothing to clean; kept for the fallback pool */
void dma_sync_single_for_device(struct device *dev, dma_addr_t a, size_t s,
				enum dma_data_direction dir)
{
	long i = pool_index(a);
	(void)dev;
	(void)dir;
	if (i >= 0)
		nvxnu_dcache_clean((char *)page_address(&pool_pages[i]) + (a & ~PAGE_MASK), s);
}

void dma_sync_single_for_cpu(struct device *dev, dma_addr_t a, size_t s,
			     enum dma_data_direction dir)
{
	long i = pool_index(a);
	(void)dev;
	(void)dir;
	if (i >= 0) {
		nvxnu_dcache_clean_inval((char *)page_address(&pool_pages[i]) + (a & ~PAGE_MASK),
					 s);
	}
}

void *vmap(struct page **pages, unsigned int count, unsigned long flags, pgprot_t prot)
{
	u64 *pas = NULL;
	void *va = NULL;
	unsigned int i = 0;
	(void)flags;
	(void)prot;
	for (i = 1; i < count && pages[i] == pages[0] + i; i++)
		;
	if (i == count)
		return page_address(pages[0]);
	pas = kmalloc_array(count, sizeof(*pas), GFP_KERNEL);
	if (!pas)
		return NULL;
	for (i = 0; i < count; i++)
		pas[i] = pages[i]->pa;
	va = nvxnu_vmap(pas, count);
	kfree(pas);
	return va;
}

void vunmap(const void *addr)
{
	if (!vmalloc_to_page(addr))
		nvxnu_vunmap((void *)addr);
}

/* scatter-gather: only reachable through NVKM_FIRMWARE_IMG_SGT, which GM20B never uses */
int sg_alloc_table(struct sg_table *t, unsigned int nents, gfp_t gfp)
{
	(void)t;
	(void)nents;
	(void)gfp;
	return -ENOMEM;
}

void sg_free_table(struct sg_table *t)
{
	(void)t;
}

void sg_set_page(struct scatterlist *sg, struct page *page, unsigned int len, unsigned int off)
{
	sg->page_link = (unsigned long)page;
	sg->length = len;
	sg->offset = off;
}

int dma_map_sgtable(struct device *d, struct sg_table *t, enum dma_data_direction dir,
		    unsigned long attrs)
{
	(void)d;
	(void)t;
	(void)dir;
	(void)attrs;
	return -ENOMEM;
}

void dma_unmap_sgtable(struct device *d, struct sg_table *t, enum dma_data_direction dir,
		       unsigned long attrs)
{
	(void)d;
	(void)t;
	(void)dir;
	(void)attrs;
}

void __iomem *ioremap(phys_addr_t pa, size_t size)
{
	return (void __iomem *)switch_linux_ioremap(pa, size, 0);
}

void __iomem *ioremap_wc(phys_addr_t pa, size_t size)
{
	return (void __iomem *)switch_linux_ioremap(pa, size, 1);
}

void iounmap(volatile void __iomem *va)
{
	(void)va; /* BAR mappings live as long as the kernel */
}

struct resource *platform_get_resource(struct platform_device *pdev, unsigned int type,
				       unsigned int n)
{
	for (u32 i = 0; i < pdev->num_resources; i++) {
		if ((pdev->resource[i].flags & type) && !n--)
			return &pdev->resource[i];
	}
	return NULL;
}

int platform_get_irq_byname(struct platform_device *pdev, const char *name)
{
	return !strcmp(name, "stall") ? pdev->irq_stall : pdev->irq_nonstall;
}

void __iomem *devm_platform_ioremap_resource(struct platform_device *pdev, unsigned int index)
{
	struct resource *r = platform_get_resource(pdev, IORESOURCE_MEM, index);
	void __iomem *va = r ? ioremap(r->start, resource_size(r)) : NULL;
	return va ? va : ERR_PTR(-ENOMEM);
}

int request_firmware(const struct firmware **pfw, const char *name, struct device *dev)
{
	struct firmware *fw = NULL;
	const void *data = NULL;
	unsigned long size = 0;
	(void)dev;
	*pfw = NULL;
	if (nvxnu_firmware_find(name, &data, &size))
		return -ENOENT;
	fw = kzalloc(sizeof(*fw), GFP_KERNEL);
	if (!fw)
		return -ENOMEM;
	fw->data = data;
	fw->size = size;
	*pfw = fw;
	return 0;
}

void release_firmware(const struct firmware *fw)
{
	kfree(fw);
}
