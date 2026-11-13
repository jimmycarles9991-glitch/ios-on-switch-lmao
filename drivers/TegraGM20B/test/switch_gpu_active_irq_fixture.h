/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
struct switch_irq_fixture;
typedef void (*switch_irq_hook)(void *);
struct switch_irq_fixture *switch_irq_create(void);
void switch_irq_destroy(struct switch_irq_fixture *);
void switch_irq_add(struct switch_irq_fixture *, unsigned index, int active_only);
void switch_irq_arm(struct switch_irq_fixture *, unsigned index, uint32_t sequence);
void switch_irq_done(struct switch_irq_fixture *, unsigned index, uint32_t sequence);
void switch_irq_dispatch(struct switch_irq_fixture *, int active);
unsigned switch_irq_members(struct switch_irq_fixture *);
unsigned switch_irq_calls(struct switch_irq_fixture *, unsigned index);
unsigned switch_irq_refcount(struct switch_irq_fixture *);
void switch_irq_hook_drop(struct switch_irq_fixture *, switch_irq_hook, void *);
int switch_irq_suspend(struct switch_irq_fixture *, unsigned index);
void switch_irq_resume(struct switch_irq_fixture *, unsigned index);
void switch_irq_remove(struct switch_irq_fixture *, unsigned index);
#ifdef __cplusplus
}
#endif
