/*
 * A stub split over several holes of dead space: each piece but the last ends in `B next piece`, and the stub's own
 * branches follow the words they target. The dyld cache planner (tools/a64plan2.c) and the offline translation of
 * single images (pretranslate.c) place stubs this way when no one hole holds them.
 */
#ifndef A64X_STUB_SPLIT_H
#define A64X_STUB_SPLIT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* B/BL, CBZ/CBNZ, B.cond, TBZ/TBNZ: *is set, and the byte offset to the target returned */
int64_t a64x_branch_offset(uint32_t w, bool *is);

struct a64x_placed {
    uint64_t at;
    uint32_t word;
};

/*
 * st[0..n) as a64x_stub_v laid it out at `laid`, moved word by word to addr[0..n). A branch to a word of the stub
 * goes to that word's new place; any other keeps its target. Where addr[j + 1] does not follow addr[j], the next of
 * links[0..nlinks) becomes `B addr[j + 1]`. out (room for n + nlinks) gets every word with its place. Returns how
 * many, or -1 if a branch cannot reach from its new place.
 */
int a64x_stub_split(const uint32_t *st, int n, uint64_t laid, const uint64_t *addr, const uint64_t *links, int nlinks,
                    struct a64x_placed *out);

#ifdef __cplusplus
}
#endif

#endif /* A64X_STUB_SPLIT_H */
