#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <mach/machine.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <CommonCrypto/CommonDigest.h>
#include <switch/app_code.h>
#include <switch/entitlements_factory.h>

typedef uintptr_t vm_offset_t;
typedef size_t vm_size_t;
union cs_hash_union { CC_SHA256_CTX sha256; };
struct cs_hash {
    size_t cs_size;
    void (*cs_init)(union cs_hash_union *);
    void (*cs_update)(union cs_hash_union *, const void *, size_t);
    void (*cs_final)(void *, union cs_hash_union *);
};
struct cs_blob {
    struct cs_blob *csb_next;
    struct vnode *csb_vnode;
    void *csb_ro_addr;
    cpu_type_t csb_cpu_type;
    cpu_subtype_t csb_cpu_subtype;
    int64_t csb_base_offset, csb_end_offset;
    void *csb_mem_kaddr;
    size_t csb_mem_size;
    const CS_CodeDirectory *csb_cd;
    const char *csb_teamid;
    const CS_GenericBlob *csb_entitlements_blob, *csb_der_entitlements_blob;
    void *csb_entitlements;
    int csb_hash_pageshift;
    const struct cs_hash *csb_hashtype;
    uint8_t csb_cdhash[CS_CDHASH_LEN];
    unsigned csb_flags, csb_reconstituted, csb_validation_category;
};
struct ubc_info { struct cs_blob *cs_blobs; };
struct vnode { struct ubc_info *v_ubcinfo; };
struct model_entitlements { const void *der, *xml; const char *identity; };
static unsigned scenario, payloads, objects, stats, locks;
#define KERN_SUCCESS 0
#define ZONE_ID_CS_BLOB 1
#define Z_WAITOK 0
#define Z_ZERO 0
#define PAGE_SHIFT 14
#define UBCINFOEXISTS(vp) ((vp)->v_ubcinfo != NULL)
#define kalloc_data(size, flags) calloc(1, size)
#define kfree_data(data, size) free(data)
#define zalloc_ro(zone, flags) (scenario == 5 ? NULL : calloc(1, sizeof(struct cs_blob)))
#define zfree_ro(zone, data) free(data)
#define zalloc_ro_update_elem(zone, dest, source) memcpy(dest, source, sizeof(*(dest)))
#define os_atomic_thread_fence(order) atomic_thread_fence(memory_order_seq_cst)

static void hash_init(union cs_hash_union *context) { CC_SHA256_Init(&context->sha256); }
static void hash_update(union cs_hash_union *context, const void *data, size_t size)
{ CC_SHA256_Update(&context->sha256, data, (CC_LONG)size); }
static void hash_final(void *data, union cs_hash_union *context) { CC_SHA256_Final(data, &context->sha256); }
static const struct cs_hash hash = {32, hash_init, hash_update, hash_final};
static const struct cs_hash *cs_find_md(unsigned type) { assert(type == CS_HASHTYPE_SHA256); return &hash; }
static struct cs_blob *ubc_cs_blob_get(struct vnode *vp, int cpu, int subtype, int64_t slice)
{
    (void)cpu; (void)subtype;
    for (struct cs_blob *blob = vp->v_ubcinfo ? vp->v_ubcinfo->cs_blobs : NULL; blob; blob = blob->csb_next)
        if (blob->csb_base_offset == slice) return blob;
    return NULL;
}
bool switch_app_code_image(struct vnode *vp) { (void)vp; return true; }
int switch_app_code_coverage(struct vnode *vp, int64_t slice, int64_t *coverage)
{ (void)vp; assert(slice == 0x4000); *coverage = 0x8000; return 0; }
int switch_app_code_arch(struct vnode *vp, int64_t slice, int32_t *cpu, int32_t *subtype)
{ (void)vp; (void)slice; *cpu = CPU_TYPE_ARM64; *subtype = CPU_SUBTYPE_ARM64E; return 0; }
int switch_app_code_metadata(struct vnode *vp, void **data, size_t *size)
{
    (void)vp; *data = NULL; *size = 0;
    if (scenario == 1) return ENOENT;
    *size = 4; *data = malloc(*size); memcpy(*data, "DER!", *size); return 0;
}
int switch_app_code_capability_decode(const void *data, size_t size, struct switch_app_code_capabilities *result)
{
    assert(size == 4 && !memcmp(data, "DER!", 4));
    if (scenario == 2) return EINVAL;
    strcpy(result->identity, "TEAM.example.app");
    if (scenario != 6) strcpy(result->team, "DECLAREDTEAM");
    const char xml[] = "<dict><key>application-identifier</key><string>TEAM.example.app</string></dict>";
    result->xml_size = sizeof(xml); result->xml = malloc(sizeof(xml)); memcpy(result->xml, xml, sizeof(xml));
    return 0;
}
void *switch_app_code_native_entitlements(const void *der, size_t size, const char *identity, struct cs_blob *blob)
{
    assert(size == 4 && !memcmp(der, "DER!", 4));
    const CS_GenericBlob *xml = blob->csb_entitlements_blob;
    assert(blob->csb_cd && identity == (const char *)blob->csb_cd + ntohl(blob->csb_cd->identOffset));
    assert((blob->csb_flags & CS_VALID) && blob->csb_cpu_type == CPU_TYPE_ARM64 && blob->csb_hashtype);
    if (scenario == 3) return NULL;
    struct model_entitlements *object = malloc(sizeof(*object));
    *object = (struct model_entitlements){der, xml, identity}; ++objects; return object;
}
static int ubc_cs_blob_allocate(vm_offset_t *memory, vm_size_t *size)
{ *memory = (uintptr_t)calloc(1, *size); ++payloads; return KERN_SUCCESS; }
static void cs_blob_cleanup(struct cs_blob *blob)
{
    if (blob->csb_entitlements) { free(blob->csb_entitlements); --objects; }
    free(blob->csb_mem_kaddr); --payloads;
}
static void vnode_lock(struct vnode *vp) { (void)vp; ++locks; }
static void vnode_unlock(struct vnode *vp) { (void)vp; assert(locks); --locks; }
static void ubc_cs_blob_adjust_statistics(struct cs_blob *blob) { (void)blob; ++stats; }

#include "../switch/include/switch/app_code_blob.h"

#define CHECK(condition) do { if (!(condition)) return __LINE__; } while (0)
int switch_app_code_blob_proof(unsigned selected)
{
    scenario = selected; payloads = objects = stats = locks = 0;
    struct ubc_info info = {};
    struct vnode vnode = { selected == 4 ? NULL : &info };
    int error = switch_app_code_blob(&vnode, 0x4000);
    if (selected >= 1 && selected <= 5) {
        CHECK(error == (selected == 5 ? ENOMEM : selected == 1 || selected == 4 ? ENOENT : EINVAL));
        CHECK(!info.cs_blobs && !objects && !payloads && !stats && !locks);
        return 0;
    }
    CHECK(!error && info.cs_blobs && payloads == 1 && objects == 1 && stats == 1 && !locks);
    struct cs_blob *blob = info.cs_blobs;
    CHECK(blob->csb_ro_addr == blob && blob->csb_vnode == &vnode);
    CHECK(blob->csb_base_offset == 0x4000 && blob->csb_end_offset == 0x8000);
    CHECK(blob->csb_cpu_subtype == CPU_SUBTYPE_ARM64E);
    CHECK(blob->csb_flags == (CS_VALID | CS_SIGNED | CS_ENTITLEMENTS_VALIDATED));
    CHECK(!(blob->csb_flags & (CS_PLATFORM_BINARY | CS_PLATFORM_PATH | CS_ENTITLEMENT_FLAGS | CS_DEBUGGED)));
    CHECK(blob->csb_reconstituted == SWITCH_APP_CODE_METADATA);
    CHECK(!blob->csb_cd->nCodeSlots && !blob->csb_cd->codeLimit);
    struct model_entitlements *object = blob->csb_entitlements;
    const uintptr_t start = (uintptr_t)blob->csb_mem_kaddr, end = start + blob->csb_mem_size;
    CHECK((uintptr_t)object->identity >= start && (uintptr_t)object->identity < end);
    CHECK((uintptr_t)object->der >= start && (uintptr_t)object->der < end);
    CHECK(!strcmp(object->identity, "TEAM.example.app"));
    if (selected == 6) CHECK(!blob->csb_teamid && !blob->csb_cd->teamOffset);
    else {
        CHECK((uintptr_t)blob->csb_teamid >= start && (uintptr_t)blob->csb_teamid < end);
        CHECK(!strcmp(blob->csb_teamid, "DECLAREDTEAM"));
        CHECK((const char *)blob->csb_cd + ntohl(blob->csb_cd->teamOffset) == blob->csb_teamid);
    }
    for (unsigned i = 0; i < 2; ++i) {
        const CS_GenericBlob *ent = i ? blob->csb_der_entitlements_blob : blob->csb_entitlements_blob;
        const unsigned slot = i ? CSSLOT_DER_ENTITLEMENTS : CSSLOT_ENTITLEMENTS;
        uint8_t digest[CC_SHA256_DIGEST_LENGTH]; CC_SHA256(ent, ntohl(ent->length), digest);
        const uint8_t *expected = (const uint8_t *)blob->csb_cd + ntohl(blob->csb_cd->hashOffset) - slot * 32;
        CHECK(!memcmp(digest, expected, sizeof(digest)));
    }
    CHECK(!switch_app_code_blob(&vnode, 0x4000) && payloads == 1 && objects == 1 && stats == 1);
    cs_blob_cleanup(blob); free(blob);
    CHECK(!payloads && !objects);
    return 0;
}
