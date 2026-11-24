// SPDX-License-Identifier: GPL-2.0-only
#ifndef SWITCH_PREFETCH_VM_H
#define SWITCH_PREFETCH_VM_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct mount;
struct vnode;

enum switch_prefetch_volume {
    SWITCH_PREFETCH_VOL_SYSTEM = 0,
    SWITCH_PREFETCH_VOL_APP = 1,
    SWITCH_PREFETCH_VOL_ROOT = 2,
    SWITCH_PREFETCH_VOL_DATA = 3,
    SWITCH_PREFETCH_VOLUMES = 4,
};

enum switch_prefetch_run_state {
    SWITCH_PREFETCH_RUN_UNUSED = 0,
    SWITCH_PREFETCH_RUN_USED = 1,
    SWITCH_PREFETCH_RUN_EVICTED = 2,
};

/* Layout written by vfs_cluster.c on the page-in path and read by prefetch_save.c after quiesce. */
struct switch_record_entry {
    struct vnode *vp;
    uint64_t offset;
    uint32_t length;
    uint32_t vid;
    uint16_t index;
    uint16_t when;
    uint32_t reserved;
};

#define SWITCH_PREFETCH_STOP_NONE          0u
#define SWITCH_PREFETCH_STOP_PRESSURE      1u
#define SWITCH_PREFETCH_STOP_SHUTDOWN      2u
#define SWITCH_PREFETCH_STOP_BOOT_COMPLETE 3u

#ifdef SWITCH_PREFETCH_IMPL
#define SWITCH_PREFETCH_WEAK
#else
#define SWITCH_PREFETCH_WEAK __attribute__((weak))
#endif

/* Weak hooks called from XNU VM and shutdown paths; defined in kernel/switch/vm/prefetch_vm.c and prefetch.c.
 * The _locked hooks run under page/object locks: they only update atomic counters and report whether the caller
 * must call switch_vm_prefetch_wake() after dropping those locks. */
extern bool switch_vm_prefetch_used_locked(void) SWITCH_PREFETCH_WEAK;
extern bool switch_vm_prefetch_reclaimed_locked(void) SWITCH_PREFETCH_WEAK;
extern void switch_vm_prefetch_wake(void) SWITCH_PREFETCH_WEAK;
extern bool switch_vm_prefetch_is_replay_thread(void *thread) SWITCH_PREFETCH_WEAK;
extern void switch_vm_prefetch_inflight_add(uint32_t bytes) SWITCH_PREFETCH_WEAK;
extern void switch_vm_prefetch_committed(uint32_t pages) SWITCH_PREFETCH_WEAK;
extern void switch_vm_prefetch_inflight_done(uint32_t bytes, uint64_t elapsed_abs) SWITCH_PREFETCH_WEAK;
extern void switch_vm_pressure_changed(unsigned int old_level, unsigned int new_level) SWITCH_PREFETCH_WEAK;
extern void switch_prefetch_volumes_mounted(struct mount *root_mount, struct mount *data_mount) SWITCH_PREFETCH_WEAK;
extern void switch_prefetch_shutdown(void) SWITCH_PREFETCH_WEAK;

#define SWITCH_VM_PREFETCH_FLUSH_WAKE(flag)        \
    do {                                           \
        if (__builtin_expect((flag) != 0, 0)) {    \
            (flag) = 0;                            \
            if (switch_vm_prefetch_wake != NULL) { \
                switch_vm_prefetch_wake();         \
            }                                      \
        }                                          \
    } while (0)

/* XNU-side helpers exposed to kernel/switch/vm/. */
extern uint64_t switch_vm_pressure_headroom_pages(void);
extern int switch_vfs_rename(const char *from, const char *to);
extern void switch_cluster_register_mount(unsigned int index, struct mount *mp);
extern void switch_cluster_register_cryptex_mount(unsigned int index, struct mount *mp);
extern void switch_cluster_record_start(struct switch_record_entry *log, uint32_t capacity);
extern uint32_t switch_cluster_record_stop(void);
extern void switch_cluster_record_set_skip_thread(void *thread);

/* osfmk-side services exposed by prefetch_vm.c to bsd prefetch.c / prefetch_save.c. */
extern bool switch_vm_prefetch_start_thread(void (*entry)(void *, int), void *arg);
extern uint64_t switch_vm_prefetch_begin(uint64_t list_vm_pages, void *replay_thread);
extern void switch_vm_prefetch_end_replay(void);
extern void switch_vm_prefetch_wait_inflight_drain(void);
extern void switch_vm_prefetch_finish(void);
extern bool switch_vm_prefetch_can_issue_now(void);
extern bool switch_vm_prefetch_wait_can_issue(void);
extern uint32_t switch_vm_prefetch_wait_stop(void);
extern uint32_t switch_vm_prefetch_stop(uint32_t reason);
extern uint32_t switch_vm_prefetch_replay_stop_reason(void);
extern uint32_t switch_vm_prefetch_boot_stop_reason(void);
extern enum switch_prefetch_run_state switch_vm_prefetch_check_run(void *control, uint64_t offset, uint64_t length);

#if DEVELOPMENT || DEBUG
struct switch_cluster_io_snapshot {
    uint64_t demand_bytes[SWITCH_PREFETCH_VOLUMES];
    uint64_t replay_reqs;
    uint64_t replay_gt64k;
    uint64_t demand_reqs;
    uint64_t demand_gt64k;
    uint64_t demand_abs;
};

struct switch_prefetch_pace_snapshot {
    uint64_t replayed_pages;
    uint64_t used_pages;
    uint64_t reclaimed_pages;
    uint64_t lead_pages;
    uint64_t lead_bound;
    uint64_t replay_abs;
    uint64_t demand_upl_abs;
};

extern void switch_vm_prefetch_mark_demand_upl(void *upl);
extern void switch_vm_prefetch_demand_done(uint64_t elapsed_abs) SWITCH_PREFETCH_WEAK;
extern void switch_cluster_prefetch_snapshot(struct switch_cluster_io_snapshot *out);
extern void switch_vm_prefetch_pace_snapshot(struct switch_prefetch_pace_snapshot *out);
#endif

#ifdef __cplusplus
}
#endif

#endif
