/* Included only by SwitchFiles.c: the same qualified, serialized, read-only
 * FAT reader owns policy loading. No child device exists until this finishes. */
#include "SwitchRootPolicy.h"
#include "SwitchRootExpectations.h"
#include "SwitchKmem.h"
#include <vm/vm_map.h>

extern void switch_root_progress(unsigned, unsigned, uint64_t, uint64_t);
static const struct switch_root_policy *root_policies;
static unsigned root_policy_attempted;

const struct switch_root_policy *switch_root_policy_get(unsigned disk)
{
    return disk < 2 && root_policies ? &root_policies[disk] : NULL;
}

/* Fresh whole-file budget includes directory traversal, FAT index, data,
 * envelope hashing and signature/table verification. No partial publication. */
int switch_root_policy_load(void)
{
    vm_offset_t descriptors = 0;
    vm_offset_t storage[2] = {0, 0};
    vm_offset_t index = 0;
    vm_size_t sizes[2] = {0, 0};
    vm_size_t index_size = 0;
    unsigned code = 400;
    unsigned disk = 0;
    int active = 0;
    if (root_policy_attempted)
        return root_policies ? 0 : 400;
    root_policy_attempted = 1;
    if (source_ready(&reader) != SWITCH_BLOCK_OK)
        return 401;
    if (kmem_alloc(kernel_map, &descriptors, PAGE_SIZE, KMA_KOBJECT | KMA_DATA | KMA_ZERO, VM_KERN_MEMORY_FILE) !=
        KERN_SUCCESS)
        return 402;
    struct switch_root_policy *views = (struct switch_root_policy *)descriptors;
    for (disk = 0; disk < 2; disk++) {
        const struct switch_root_expected *e = &switch_root_expected[disk];
        if (!e->bytes || e->bytes > (uint64_t)64u * 1024u * 1024u ||
            e->image_bytes != switch_storage_volumes[disk].size_bytes) {
            code = 403;
            goto fail;
        }
        sizes[disk] = (vm_size_t)((e->bytes + PAGE_MASK) & ~(uint64_t)PAGE_MASK);
        if (kmem_alloc(kernel_map, &storage[disk], sizes[disk], KMA_KOBJECT | KMA_DATA | KMA_ZERO,
                       VM_KERN_MEMORY_FILE) != KERN_SUCCESS) {
            code = 404;
            goto fail;
        }
        uint32_t count = 0;
        if (switch_fat_index_count(&volume, (uint32_t)e->bytes, &count) || !count || count > 131072) {
            code = 405;
            goto fail;
        }
        index_size = ((vm_size_t)count * sizeof(uint32_t) + PAGE_MASK) & ~(vm_size_t)PAGE_MASK;
        if (kmem_alloc(kernel_map, &index, index_size, KMA_KOBJECT | KMA_DATA | KMA_ZERO, VM_KERN_MEMORY_FILE) !=
            KERN_SUCCESS) {
            code = 406;
            goto fail;
        }
        if (source_begin(&reader, FILES_MAX_SECTORS, FILES_MAX_SECONDS)) {
            code = 407;
            goto fail;
        }
        active = 1;
        switch_root_progress(2, disk, 0, e->bytes);
        struct switch_fat_indexed_file file = {0};
        if (switch_fat_open_indexed(&volume, e->path, &file, (uint32_t *)index, count) || file.file.size != e->bytes) {
            code = 408;
            goto fail;
        }
        SHA256_CTX hash;
        SHA256_Init(&hash);
        for (uint32_t offset = 0; offset < e->bytes;) {
            uint32_t n = (uint32_t)e->bytes - offset;
            if (n > FILES_BUFFER)
                n = FILES_BUFFER;
            uint8_t *dest = (uint8_t *)storage[disk] + offset;
            if (switch_fat_read_indexed(&volume, &file, offset, dest, n)) {
                code = 409;
                goto fail;
            }
            SHA256_Update(&hash, dest, n);
            offset += n;
            if (!(offset & ((1u << 20) - 1)) || offset == e->bytes)
                switch_root_progress(2, disk, offset, e->bytes);
        }
        uint8_t actual[32];
        SHA256_Final(actual, &hash);
        if (!equal(actual, e->envelope_sha256, 32)) {
            code = 410;
            goto fail;
        }
        switch_root_progress(3, disk, e->bytes, e->bytes);
        if (vr_open((const uint8_t *)storage[disk], e->bytes, verified_root_key, 1, &vr_xnu_crypto,
                    &views[disk].view) != VR_OK) {
            code = 411;
            goto fail;
        }
        const struct vr_view *v = &views[disk].view;
        if (v->generation != 1 || v->image_bytes != e->image_bytes ||
            v->block_count != (e->image_bytes + 4095) / 4096 || !equal(v->volume_uuid, e->uuid, 16) ||
            !equal(v->image_sha256, e->image_sha256, 32)) {
            code = 412;
            goto fail;
        }
        int end = source_end(&reader);
        active = 0;
        if (end) {
            code = 413;
            goto fail;
        }
        kmem_free(kernel_map, index, index_size);
        index = 0;
        index_size = 0;
        views[disk].envelope = (const uint8_t *)storage[disk];
        views[disk].envelope_bytes = e->bytes;
        if (switch_vm_map_protect(kernel_map, storage[disk], storage[disk] + sizes[disk], VM_PROT_READ, TRUE) !=
            KERN_SUCCESS) {
            code = 414;
            goto fail;
        }
    }
    if (switch_vm_map_protect(kernel_map, descriptors, descriptors + PAGE_SIZE, VM_PROT_READ, TRUE) != KERN_SUCCESS) {
        code = 415;
        goto fail;
    }
    /* No device publication or concurrent lookup is allowed before this one
     * release point; immutable views retain their immutable envelope storage. */
    __sync_synchronize();
    root_policies = (const struct switch_root_policy *)descriptors;
    switch_root_progress(4, 0, switch_root_expected[0].bytes + switch_root_expected[1].bytes,
                         switch_root_expected[0].bytes + switch_root_expected[1].bytes);
    return 0;
fail:
    if (active)
        (void)source_end(&reader);
    reader.offline = 1;
    if (index)
        kmem_free(kernel_map, index, index_size);
    for (unsigned i = 0; i < 2; i++) {
        if (storage[i])
            kmem_free(kernel_map, storage[i], sizes[i]);
    }
    if (descriptors)
        kmem_free(kernel_map, descriptors, PAGE_SIZE);
    switch_root_progress(0x80000000u | code, disk, 0, 0);
    return (int)code;
}
