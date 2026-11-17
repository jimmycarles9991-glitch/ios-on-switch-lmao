#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include "../switch/include/switch/capabilities.h"

struct CERuntime;
typedef const struct CERuntime *CERuntime_t;
struct CERuntime { uint64_t version; void (*abort)(CERuntime_t, const char *, ...); };
typedef struct { int valid; } CEValidationResult;
struct CEQueryContext { int valid; };
typedef struct {
    int (*Validate)(CERuntime_t, CEValidationResult *, const uint8_t *, const uint8_t *);
    int (*AcquireUnmanagedContext)(CERuntime_t, CEValidationResult, struct CEQueryContext *);
    int kNoError;
} coreentitlements_t;
struct Amfi {
    coreentitlements_t CoreEntitlements;
    void *(*query_context_to_object)(struct CEQueryContext *);
    void *(*OSEntitlements_asdict)(void *);
    void (*OSEntitlements_invalidate)(void *);
};
static struct Amfi *amfi;
struct Native { bool invalidated; };
struct Dictionary { unsigned value; };
static unsigned selected, validate_calls, acquire_calls, legacy_calls, native_calls, copy_calls, invalidations, releases;
#define CE_RUNTIME_VERSION 1
static void switch_entitlements_abort(CERuntime_t runtime, const char *format, ...)
{ (void)runtime; (void)format; abort(); }
static int validate(CERuntime_t runtime, CEValidationResult *result, const uint8_t *start, const uint8_t *end)
{ (void)runtime; assert(end == start + 1); ++validate_calls; result->valid = 1; return selected == 9; }
static int acquire(CERuntime_t runtime, CEValidationResult input, struct CEQueryContext *result)
{ (void)runtime; assert(input.valid); ++acquire_calls; result->valid = 1; return selected == 10; }
static void *legacy(struct CEQueryContext *context)
{ assert(context->valid); ++legacy_calls; struct Dictionary *result = malloc(sizeof(*result)); result->value = 17; return result; }
static void *native(const void *data, size_t size)
{ assert(data && size == 1); ++native_calls; return selected == 6 ? NULL : calloc(1, sizeof(struct Native)); }
static uintptr_t switch_entitlements_factory(const void *data, size_t size)
{ assert(data && size == 1); return (uintptr_t)native; }
static void *copy_dictionary(void *object)
{
    assert(object && !((struct Native *)object)->invalidated); ++copy_calls;
    if (selected == 7) return NULL;
    struct Dictionary *result = malloc(sizeof(*result)); result->value = 27; return result;
}
static void invalidate(void *object) { assert(!((struct Native *)object)->invalidated); ((struct Native *)object)->invalidated = true; ++invalidations; }
static void osobject_release(void *object) { assert(((struct Native *)object)->invalidated); ++releases; free(object); }

#define XNU_10063_141_1 1
#define XNU_12377_121_6 0
#define switch_app_code_native_dictionary decode17
#include "app_entitlements_decode.h"
#undef switch_app_code_native_dictionary
#undef XNU_10063_141_1
#undef XNU_12377_121_6
#define XNU_10063_141_1 0
#define XNU_12377_121_6 1
#define switch_app_code_native_dictionary decode27
#include "app_entitlements_decode.h"
#undef switch_app_code_native_dictionary
#undef XNU_10063_141_1
#undef XNU_12377_121_6

#define CHECK(condition) do { if (!(condition)) return __LINE__; } while (0)
int switch_entitlements_decode_proof(unsigned scenario)
{
    selected = scenario;
    validate_calls = acquire_calls = legacy_calls = native_calls = copy_calls = invalidations = releases = 0;
    struct Amfi bridge = {{validate, acquire, 0}, legacy, copy_dictionary, invalidate};
    amfi = &bridge;
    uint8_t der = 0;
    struct Dictionary *result = NULL;
    switch (scenario) {
    case 0:
        bridge.query_context_to_object = NULL;
        bridge.CoreEntitlements.Validate = NULL;
        bridge.CoreEntitlements.AcquireUnmanagedContext = NULL;
        result = decode27(&der, 1);
        CHECK(result && result->value == 27);
        CHECK(native_calls == 1 && copy_calls == 1 && invalidations == 1 && releases == 1);
        CHECK(!validate_calls && !acquire_calls && !legacy_calls);
        break;
    case 1: bridge.query_context_to_object = NULL; result = decode17(&der, 1); break;
    case 2:
        result = decode17(&der, 1);
        CHECK(result && result->value == 17);
        CHECK(validate_calls == 1 && acquire_calls == 1 && legacy_calls == 1 && !native_calls);
        break;
    case 3: bridge.CoreEntitlements.Validate = NULL; result = decode17(&der, 1); break;
    case 4: bridge.CoreEntitlements.AcquireUnmanagedContext = NULL; result = decode17(&der, 1); break;
    case 5: bridge.OSEntitlements_asdict = NULL; result = decode27(&der, 1); break;
    case 6: result = decode27(&der, 1); CHECK(native_calls == 1 && !copy_calls && !invalidations && !releases); break;
    case 7: result = decode27(&der, 1); CHECK(native_calls == 1 && copy_calls == 1 && invalidations == 1 && releases == 1); break;
    case 8:
        amfi = NULL;
        CHECK(!decode17(&der, 1) && !decode27(&der, 1));
        amfi = &bridge;
        CHECK(!decode17(NULL, 1) && !decode27(NULL, 1));
        CHECK(!decode17(&der, 0) && !decode27(&der, 0));
        break;
    case 9: result = decode17(&der, 1); CHECK(validate_calls == 1 && !acquire_calls && !legacy_calls); break;
    case 10: result = decode17(&der, 1); CHECK(validate_calls == 1 && acquire_calls == 1 && !legacy_calls); break;
    default: return __LINE__;
    }
    if (scenario != 0 && scenario != 2) CHECK(!result);
    if (scenario >= 1 && scenario <= 5 && scenario != 2) CHECK(!validate_calls && !acquire_calls && !legacy_calls && !native_calls);
    free(result);
    return 0;
}
