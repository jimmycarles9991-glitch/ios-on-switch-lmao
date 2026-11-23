/*
 * Minimal stand-ins for the parts of Mesa's src/util/macros.h that mme_fermi.{c,h} and mme_fermi_sim.c use (Mesa, MIT).
 * r8-rawca: the pack generator (drivers/TegraGM20BMetal/pack) builds Mesa's Fermi MME simulator without a Mesa tree.
 */
#ifndef SWITCH_METAL_MESA_UTIL_MACROS_H
#define SWITCH_METAL_MESA_UTIL_MACROS_H
#include <assert.h>
#include <stdint.h>
#ifndef ARRAY_SIZE
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#endif
#define UNREACHABLE(str)                                                                                                   \
    do {                                                                                                                   \
        (void)"" str;                                                                                                      \
        assert(!str);                                                                                                      \
        __builtin_unreachable();                                                                                           \
    } while (0)
#define PACKED __attribute__((__packed__))
#define ENUM_PACKED __attribute__((packed))
#define UNUSED __attribute__((unused))
#define MAX2(A, B) ((A) > (B) ? (A) : (B))
#define MIN2(A, B) ((A) < (B) ? (A) : (B))
#define BITFIELD_MASK(b) ((b) == 32 ? (~0u) : (1u << ((b) % 32)) - 1)
#define BITFIELD64_MASK(b) ((b) == 64 ? (~0ull) : (1ull << (b)) - 1)
#endif
