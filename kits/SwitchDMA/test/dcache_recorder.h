// SwitchDcache.h's two leaves replaced by a recorder: every DC with its operation and address, and every barrier.
#ifndef SWITCH_DCACHE_RECORDER_H
#define SWITCH_DCACHE_RECORDER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct switch_dcache_event {
    char op[8];  // "cvac", "civac", "ivac" or "dsb"
    uintptr_t address;
} switch_dcache_event;

void switch_dcache_record(const char *op, uintptr_t address);
size_t switch_dcache_recorded(const switch_dcache_event **events);
void switch_dcache_forget(void);

#ifdef __cplusplus
}
#endif

#define SWITCH_DCACHE_OP(op, address) switch_dcache_record(op, (uintptr_t)(address))
#define SWITCH_DCACHE_BARRIER()       switch_dcache_record("dsb", 0)
#define SWITCH_DMA_ORDER(kind)        switch_dcache_record(kind, 0)

#endif
