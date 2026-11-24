/* cpu.atomics: every LSE form an A12 build emits (switch_atomics_list.h), hammered from more threads than cores with
 * invariants a lost or doubled update breaks, and the same operations through Apple's library entry points that hold
 * them in the dyld cache (switch_atomics_library.c). On iOS 27 the app's forms run as the a64x loader translates them
 * (a branch to a stub per site); on iOS 17 the same file assembles the ARMv8.0 sequences those stubs run. */
#ifndef SWITCH_ATOMICS_H
#define SWITCH_ATOMICS_H

#include <stdbool.h>
#include <stdint.h>

struct switch_atomics_row {
    char name[40];
    uint64_t ops, ns;
    uint64_t violations; /* lost, doubled or misordered updates, wrong old values, neighbours written */
    uint64_t clobbers;   /* calls after which NZCV or a register live across the site had changed */
    bool branched;       /* the site's word, as this process maps it, is a B: the a64x loader rewrote it */
};

/* Whether this build's forms are LSE instructions (an A12 build) or ARMv8.0 sequences. */
bool switch_atomics_lse(void);

unsigned switch_atomics_forms(void);
unsigned switch_atomics_library_forms(void);

/* Hammers one form from `threads` threads (2 to 16), `ops` calls each; false for an index or count out of range or a
 * thread that could not be made. */
bool switch_atomics_form(unsigned index, unsigned threads, unsigned ops, struct switch_atomics_row *out);
bool switch_atomics_library_form(unsigned index, unsigned threads, unsigned ops, struct switch_atomics_row *out);

/* Store buffering through an atomic (switch_atomics_litmus.c): runs, and those where both threads read the other's
 * word before its store. */
struct switch_litmus_row {
    char name[40];
    uint64_t runs, forbidden, ns;
    bool judged;   /* the outcome is forbidden for what this build runs: the row must read 0 */
    bool has_site; /* the row holds an AL instruction on an LSE build */
    bool branched; /* the row's AL site, as this process maps it, is a B */
};

unsigned switch_atomics_litmus_tests(void);

/* At least 4096 runs, rounded down to whole batches of 4096. */
bool switch_atomics_litmus(unsigned index, uint64_t runs, struct switch_litmus_row *out);

#endif
