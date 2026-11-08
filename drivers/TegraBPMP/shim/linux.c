/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/*
 * Switch TegraBPMP shim: the Linux primitives the vendored L4T mail.c / mail_t210.c / mailman.c use beyond
 * kits/SwitchLinux's. Completions and semaphores sleep on an event (switch_linux_wait_prepare under the spin lock, then
 * block), so a waiter is woken by complete()/up() from the doorbell work loop and never polls; jiffies are
 * milliseconds.
 */
#include <linux/kernel.h>
#include <linux/io.h>
#include <linux/of_address.h>
#include <linux/semaphore.h>

void init_completion(struct completion *c)
{
	spin_lock_init(&c->lock);
	c->done = 0;
}

void complete(struct completion *c)
{
	unsigned long flags = 0;
	spin_lock_irqsave(&c->lock, flags);
	c->done++;
	spin_unlock_irqrestore(&c->lock, flags);
	switch_linux_wakeup_one(c);
}

/* Linux returns 0 on timeout, else the jiffies left (at least 1). */
unsigned long wait_for_completion_timeout(struct completion *c, unsigned long timeout)
{
	const unsigned long long deadline =
		switch_linux_nanotime() + (unsigned long long)timeout * 1000000ULL;
	for (;;) {
		unsigned long flags = 0;
		spin_lock_irqsave(&c->lock, flags);
		if (c->done) {
			unsigned long long now = switch_linux_nanotime();
			unsigned long left =
				now < deadline ? (unsigned long)((deadline - now) / 1000000ULL) : 0;
			c->done--;
			spin_unlock_irqrestore(&c->lock, flags);
			return left ? left : 1;
		}
		if (switch_linux_nanotime() >= deadline) {
			spin_unlock_irqrestore(&c->lock, flags);
			return 0;
		}
		switch_linux_wait_prepare(c, deadline);
		spin_unlock_irqrestore(&c->lock, flags);
		(void)switch_linux_wait_block();
	}
}

void sema_init(struct semaphore *s, int count)
{
	spin_lock_init(&s->lock);
	s->count = count;
}

/* Linux returns 0 when it got the semaphore, -ETIME(DOUT) when the timeout (jiffies) ran out. */
int down_timeout(struct semaphore *s, long timeout)
{
	const unsigned long long deadline =
		switch_linux_nanotime() + (unsigned long long)timeout * 1000000ULL;
	for (;;) {
		unsigned long flags = 0;
		spin_lock_irqsave(&s->lock, flags);
		if (s->count > 0) {
			s->count--;
			spin_unlock_irqrestore(&s->lock, flags);
			return 0;
		}
		if (switch_linux_nanotime() >= deadline) {
			spin_unlock_irqrestore(&s->lock, flags);
			return -ETIMEDOUT;
		}
		switch_linux_wait_prepare(s, deadline);
		spin_unlock_irqrestore(&s->lock, flags);
		(void)switch_linux_wait_block();
	}
}

void up(struct semaphore *s)
{
	unsigned long flags = 0;
	spin_lock_irqsave(&s->lock, flags);
	s->count++;
	spin_unlock_irqrestore(&s->lock, flags);
	switch_linux_wakeup_one(s);
}

void memcpy_toio(volatile void *dst, const void *src, size_t n)
{
	volatile u8 *d = dst;
	const u8 *s = src;
	while (n && ((uintptr_t)d & 3)) {
		switch_mmio_write8(d++, 0, *s++);
		n--;
	}
	for (; n >= 4; n -= 4, d += 4, s += 4) {
		u32 w = 0;
		memcpy(&w, s, 4);
		switch_mmio_write32(d, 0, w);
	}
	while (n--)
		switch_mmio_write8(d++, 0, *s++);
}

void memcpy_fromio(void *dst, const volatile void *src, size_t n)
{
	u8 *d = dst;
	const volatile u8 *s = src;
	while (n && ((uintptr_t)s & 3)) {
		*d++ = switch_mmio_read8(s++, 0);
		n--;
	}
	for (; n >= 4; n -= 4, d += 4, s += 4) {
		u32 w = switch_mmio_read32(s, 0);
		memcpy(d, &w, 4);
	}
	while (n--)
		*d++ = switch_mmio_read8(s++, 0);
}

void *of_iomap(struct device_node *np, int index)
{
	if (!np || index < 0 || (unsigned int)index >= np->nreg)
		return NULL;
	return (void *)switch_linux_ioremap(np->reg[index].base, np->reg[index].size, 0);
}
