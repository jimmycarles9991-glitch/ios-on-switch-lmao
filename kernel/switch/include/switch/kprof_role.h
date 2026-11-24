// SPDX-License-Identifier: GPL-2.0-only
/*
 * User-sample thread-role bucketing and line formatting for switch_kprof.c (--//kernel:kprof), free of kernel types so
 * kernel/tests runs it on the host; the including file declares snprintf.
 */
#ifndef SWITCH_KPROF_ROLE_H
#define SWITCH_KPROF_ROLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SWITCH_KPROF_U_DSC       (1ull << 62)
#define SWITCH_KPROF_U_MAIN      (2ull << 62)
#define SWITCH_KPROF_U_OTHER     (3ull << 62)
#define SWITCH_KPROF_U_KIND_MASK (3ull << 62)
#define SWITCH_KPROF_U_ROLE_MAIN (1ull << 60)
#define SWITCH_KPROF_U_ADDR      ((1ull << 60) - 1)
#define SWITCH_KPROF_REGION      (1ull << 20)
#define SWITCH_KPROF_PROBES      16u
#define SWITCH_KPROF_MAX_TOP_PC  8u
#define SWITCH_KPROF_MAX_TOP_IMG 24u

struct switch_kprof_user {
    uint64_t key; /* SWITCH_KPROF_U_* | optional SWITCH_KPROF_U_ROLE_MAIN | bucket; 0: empty */
    int32_t pid;
    uint32_t samples;
};

static inline uint64_t switch_kprof_user_key(uint64_t location, bool is_main)
{
    const uint64_t kind = location & SWITCH_KPROF_U_KIND_MASK;
    const uint64_t role = is_main ? SWITCH_KPROF_U_ROLE_MAIN : 0ull;

    if (kind == 0 || kind == SWITCH_KPROF_U_OTHER) {
        return SWITCH_KPROF_U_OTHER | role;
    }
    return kind | role | ((location & SWITCH_KPROF_U_ADDR) & ~63ull);
}

static inline uint64_t switch_kprof_user_region_key(uint64_t key)
{
    return (key & ~SWITCH_KPROF_U_ADDR) | ((key & SWITCH_KPROF_U_ADDR) & ~(SWITCH_KPROF_REGION - 1));
}

static inline bool switch_kprof_user_is_main(uint64_t key)
{
    return (key & SWITCH_KPROF_U_ROLE_MAIN) != 0;
}

static inline uint64_t switch_kprof_user_kind(uint64_t key)
{
    return key & SWITCH_KPROF_U_KIND_MASK;
}

static inline uint64_t switch_kprof_user_addr(uint64_t key)
{
    return key & SWITCH_KPROF_U_ADDR;
}

static inline struct switch_kprof_user *switch_kprof_user_slot(struct switch_kprof_user *table, unsigned int bits,
                                                               int32_t pid, uint64_t key)
{
    const uint32_t mask = (1u << bits) - 1;
    uint32_t i = (uint32_t)(((key ^ ((uint64_t)(uint32_t)pid << 32)) * 0x9e3779b97f4a7c15ull) >> (64 - bits));

    for (unsigned int probe = 0; probe < SWITCH_KPROF_PROBES; probe++, i = (i + 1) & mask) {
        if (table[i].key == key && table[i].pid == pid) {
            return &table[i];
        }
        if (table[i].key == 0) {
            table[i].key = key;
            table[i].pid = pid;
            return &table[i];
        }
    }
    return NULL;
}

static inline void switch_kprof_user_merge(struct switch_kprof_user *dst, unsigned int dst_bits,
                                           const struct switch_kprof_user *src, unsigned int src_len, uint32_t *lost)
{
    for (unsigned int i = 0; i < src_len; i++) {
        struct switch_kprof_user *slot = NULL;

        if (src[i].key == 0 || src[i].samples == 0) {
            continue;
        }
        slot = switch_kprof_user_slot(dst, dst_bits, src[i].pid, src[i].key);
        if (slot != NULL) {
            slot->samples += src[i].samples;
        } else if (lost != NULL) {
            *lost += src[i].samples;
        }
    }
}

static inline unsigned int switch_kprof_user_top_role(const struct switch_kprof_user *table, unsigned int len,
                                                      int32_t pid, bool is_main, bool include_other, unsigned int *top,
                                                      unsigned int max_top, uint32_t *other_out)
{
    unsigned int ntop = 0;
    uint32_t other = 0;

    for (unsigned int i = 0; i < len; i++) {
        const struct switch_kprof_user *u = &table[i];
        unsigned int at = ntop;

        if (u->key == 0 || u->pid != pid || u->samples == 0 || switch_kprof_user_is_main(u->key) != is_main) {
            continue;
        }
        if (!include_other && switch_kprof_user_kind(u->key) == SWITCH_KPROF_U_OTHER) {
            other += u->samples;
            continue;
        }
        while (at > 0 && (u->samples > table[top[at - 1]].samples ||
                          (u->samples == table[top[at - 1]].samples && u->key < table[top[at - 1]].key))) {
            if (at < max_top) {
                top[at] = top[at - 1];
            }
            at--;
        }
        if (at < max_top) {
            top[at] = i;
            if (ntop < max_top) {
                ntop++;
            }
        }
    }
    if (other_out != NULL) {
        *other_out = other;
    }
    return ntop;
}

static inline size_t switch_kprof_put_advance(size_t size, size_t at, int n)
{
    if (size == 0 || n <= 0) {
        return at;
    }
    if ((size_t)n >= size - at) {
        return size - 1;
    }
    return at + (size_t)n;
}

static inline size_t switch_kprof_format_role_spots(char *buf, size_t size, size_t at, const char *label,
                                                    const struct switch_kprof_user *table, unsigned int table_len,
                                                    int32_t pid, bool is_main, unsigned int max_spots)
{
    unsigned int top[SWITCH_KPROF_MAX_TOP_PC];
    const unsigned int cap = max_spots > SWITCH_KPROF_MAX_TOP_PC ? SWITCH_KPROF_MAX_TOP_PC : max_spots;
    uint32_t other = 0;
    const unsigned int ntop = switch_kprof_user_top_role(table, table_len, pid, is_main, false, top, cap, &other);

    at = switch_kprof_put_advance(size, at, snprintf(buf + at, size - at, " %s:", label));
    for (unsigned int i = 0; i < ntop; i++) {
        const struct switch_kprof_user *u = &table[top[i]];
        const char *kind = switch_kprof_user_kind(u->key) == SWITCH_KPROF_U_DSC ? "dsc" : "main+";
        const unsigned long long addr = (unsigned long long)switch_kprof_user_addr(u->key);
        const int n = snprintf(buf + at, size - at, " %s %llx %u,", kind, addr, u->samples);

        at = switch_kprof_put_advance(size, at, n);
    }
    return switch_kprof_put_advance(size, at, snprintf(buf + at, size - at, " other %u;", other));
}

static inline size_t switch_kprof_format_user_proc(char *buf, size_t size, const char *name, int32_t pid,
                                                   uint32_t total_user, uint32_t main_user,
                                                   const struct switch_kprof_user *table, unsigned int table_len,
                                                   unsigned int max_per_role)
{
    const uint32_t main_clamped = main_user > total_user ? total_user : main_user;
    const uint32_t worker_user = total_user - main_clamped;
    size_t at = 0;
    int n = 0;

    if (size == 0) {
        return 0;
    }
    buf[0] = '\0';
    n = snprintf(buf, size, "%s[%d] %u (main %u, worker %u):", name, pid, total_user, main_clamped, worker_user);
    at = switch_kprof_put_advance(size, at, n);
    if (main_clamped > 0) {
        at = switch_kprof_format_role_spots(buf, size, at, "main", table, table_len, pid, true, max_per_role);
    }
    if (worker_user > 0) {
        at = switch_kprof_format_role_spots(buf, size, at, "worker", table, table_len, pid, false, max_per_role);
    }
    return at;
}

static inline size_t switch_kprof_format_images_role(char *buf, size_t size, const char *name, int32_t pid,
                                                     bool is_main, uint32_t role_user, uint32_t total_user,
                                                     const struct switch_kprof_user *table, unsigned int table_len,
                                                     unsigned int max_regions, uint32_t lost)
{
    unsigned int top[SWITCH_KPROF_MAX_TOP_IMG];
    const unsigned int cap = max_regions > SWITCH_KPROF_MAX_TOP_IMG ? SWITCH_KPROF_MAX_TOP_IMG : max_regions;
    const unsigned int ntop = switch_kprof_user_top_role(table, table_len, pid, is_main, true, top, cap, NULL);
    const char *role = is_main ? "main" : "worker";
    uint32_t logged = 0;
    uint32_t rest = 0;
    size_t at = 0;
    int n = 0;

    if (size == 0) {
        return 0;
    }
    buf[0] = '\0';
    n = snprintf(buf, size, "SwitchKprof images: %s[%d] %s %u/%u:", name, pid, role, role_user, total_user);
    at = switch_kprof_put_advance(size, at, n);
    for (unsigned int i = 0; i < ntop; i++) {
        const struct switch_kprof_user *r = &table[top[i]];
        const uint64_t kind = switch_kprof_user_kind(r->key);
        const char *tag = kind == SWITCH_KPROF_U_DSC ? "dsc" : (kind == SWITCH_KPROF_U_MAIN ? "main+" : "other");
        const unsigned long long mb = (unsigned long long)(switch_kprof_user_addr(r->key) >> 20);

        n = snprintf(buf + at, size - at, " %s %llx %u,", tag, mb, r->samples);
        at = switch_kprof_put_advance(size, at, n);
        logged += r->samples;
    }
    rest = role_user > logged ? role_user - logged : 0u;
    n = snprintf(buf + at, size - at, " rest %u; lost %u", rest, lost);
    return switch_kprof_put_advance(size, at, n);
}

#endif
