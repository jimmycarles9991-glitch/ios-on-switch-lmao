/*
 * Copyright © 2024 Collabora Ltd. and Red Hat Inc.
 * SPDX-License-Identifier: MIT
 */
#ifndef NVK_DESCRIPTOR_TYPES
#define NVK_DESCRIPTOR_TYPES 1

#include "nvk_private.h"

#include "nil.h"
#include "nv_device_info.h"

#define NVK_IMAGE_DESCRIPTOR_IMAGE_INDEX_MASK   0x000fffff
#define NVK_IMAGE_DESCRIPTOR_SAMPLER_INDEX_MASK 0xfff00000

/** Hardware(ish) descriptor type used for images and buffer views.
 *
 * For all textures and Maxwell+ storage images, this is the hardware
 * descriptor placed in the texture header pool.  For Kepler storage images,
 * it's the su_info produced by NIL and consumed by NAK.
 *
 * All other image descriptor types used by NVK are for descriptor sets and
 * contain indices which indirectly reference the texture header pool.  The
 * only exception is that, on Kepler, we can put the su_info directly in the
 * descriptor set because it's all software anyway.
 */
union nvk_image_descriptor {
   struct nil_descriptor desc;
   struct nil_su_info su_info;
};
static_assert(sizeof(struct nil_descriptor) == NVK_TEXTURE_HEADER_SIZE,
              "All image heap descriptors are 32 bytes");
static_assert(sizeof(struct nil_su_info) == NVK_TEXTURE_HEADER_SIZE,
              "All image heap descriptors are 32 bytes");
static_assert(sizeof(union nvk_image_descriptor) == NVK_TEXTURE_HEADER_SIZE,
              "All image descriptors are 32 bytes");

PRAGMA_DIAGNOSTIC_PUSH
PRAGMA_DIAGNOSTIC_ERROR(-Wpadded)
struct nvk_sampled_image_descriptor {
   unsigned image_index:20;
   unsigned sampler_index:12;
};
PRAGMA_DIAGNOSTIC_POP
static_assert(sizeof(struct nvk_sampled_image_descriptor) == 4,
              "nvk_sampled_image_descriptor has no holes");

PRAGMA_DIAGNOSTIC_PUSH
PRAGMA_DIAGNOSTIC_ERROR(-Wpadded)
struct nvk_storage_image_descriptor {
   unsigned image_index:20;
   unsigned _pad:12;
};
PRAGMA_DIAGNOSTIC_POP
static_assert(sizeof(struct nvk_storage_image_descriptor) == 4,
              "nvk_storage_image_descriptor has no holes");

struct nvk_kepler_storage_image_descriptor {
   struct nil_su_info su_info;
};

struct nvk_kepler_storage_buffer_view_descriptor {
   struct nil_su_info su_info;
};

PRAGMA_DIAGNOSTIC_PUSH
PRAGMA_DIAGNOSTIC_ERROR(-Wpadded)
struct nvk_buffer_view_descriptor {
   unsigned image_index:20;
   unsigned pad:12;
};
PRAGMA_DIAGNOSTIC_POP
static_assert(sizeof(struct nvk_buffer_view_descriptor) == 4,
              "nvk_buffer_view_descriptor has no holes");

PRAGMA_DIAGNOSTIC_PUSH
PRAGMA_DIAGNOSTIC_ERROR(-Wpadded)
/** See also nvk_edb_bview_cache */
struct nvk_edb_buffer_view_descriptor {
   /** Index of the HW descriptor in the texture/image table */
   uint32_t index;
   /** Offset into the HW descriptor in surface elements */
   uint32_t offset_el;
   /** Size of the virtual descriptor in surface elements */
   uint32_t size_el;
   /** Value returned in the alpha channel for OOB buffer access */
   uint32_t oob_alpha;
};
PRAGMA_DIAGNOSTIC_POP
static_assert(sizeof(struct nvk_edb_buffer_view_descriptor) == 16,
              "nvk_edb_buffer_view_descriptor has no holes");

PRAGMA_DIAGNOSTIC_PUSH
PRAGMA_DIAGNOSTIC_ERROR(-Wpadded)
struct nvk_bindless_cbuf {
   uint64_t base_addr_shift_4:45;
   uint64_t size_shift_4:19;
   /* For descriptor buffers, avoid returning garbage data.
    * The descriptor payload must be invariant. */
   uint64_t padding;
};
PRAGMA_DIAGNOSTIC_POP
static_assert(sizeof(struct nvk_bindless_cbuf) == 16,
              "nvk_bindless_cbuf has no holes");

/* Hopper+ uses a new cbuf format */
PRAGMA_DIAGNOSTIC_PUSH
PRAGMA_DIAGNOSTIC_ERROR(-Wpadded)
struct nvk_bindless_cbuf_2 {
   uint64_t base_addr_shift_6:51;
   uint64_t size_shift_4:13;
   /* For descriptor buffers, avoid returning garbage data.
    * The descriptor payload must be invariant. */
   uint64_t padding;
};
PRAGMA_DIAGNOSTIC_POP
static_assert(sizeof(struct nvk_bindless_cbuf_2) == 16,
              "nvk_bindless_cbuf_2 has no holes");

/* This has to match nir_address_format_64bit_bounded_global */
PRAGMA_DIAGNOSTIC_PUSH
PRAGMA_DIAGNOSTIC_ERROR(-Wpadded)
struct nvk_buffer_address {
   uint64_t base_addr;
   uint32_t size;
   uint32_t zero; /* Must be zero! */
};
PRAGMA_DIAGNOSTIC_POP
static_assert(sizeof(struct nvk_buffer_address) == 16,
              "nvk_buffer_address has no holes");

#define NVK_BUFFER_ADDRESS_NULL ((struct nvk_buffer_address) { .size = 0 })

union nvk_buffer_descriptor {
   struct nvk_buffer_address addr;
   struct nvk_bindless_cbuf cbuf;
   struct nvk_bindless_cbuf_2 cbuf2;
   uint32_t values[4];
};

static inline uint32_t
nvk_min_cbuf_alignment(const struct nv_device_info *info)
{
   return info->cls_eng3d >= 0xC597 /* TURING_A */ ? 64 : 256;
}

static inline bool
nvk_use_bindless_cbuf(const struct nv_device_info *info)
{
   return info->cls_eng3d >= 0xC597 /* TURING_A */;
}

static inline bool
nvk_use_bindless_cbuf_2(const struct nv_device_info *info)
{
   return info->cls_eng3d >= 0xCB97 /* HOPPER_A */;
}

#endif /* NVK_DESCRIPTOR_TYPES */
