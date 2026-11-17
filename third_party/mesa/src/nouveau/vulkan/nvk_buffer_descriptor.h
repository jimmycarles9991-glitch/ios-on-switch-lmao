/*
 * Copyright © 2024 Collabora Ltd. and Red Hat Inc.
 * SPDX-License-Identifier: MIT
 */
#ifndef NVK_BUFFER_DESCRIPTOR_H
#define NVK_BUFFER_DESCRIPTOR_H 1

#include "nvk_descriptor_types.h"
#include "nvk_instance.h"
#include "nvk_physical_device.h"

static inline union nvk_buffer_descriptor
nvk_ubo_descriptor(const struct nvk_physical_device *pdev,
                   VkDeviceAddressRangeEXT addr_range)
{
   const uint32_t min_cbuf_alignment = nvk_min_cbuf_alignment(&pdev->info);

   assert(addr_range.address % min_cbuf_alignment == 0);
   assert(addr_range.size <= NVK_MAX_CBUF_SIZE);

   addr_range.address = ROUND_DOWN_TO(addr_range.address, min_cbuf_alignment);
   addr_range.size = align(addr_range.size, min_cbuf_alignment);

   if (nvk_use_bindless_cbuf_2(&pdev->info)) {
      return (union nvk_buffer_descriptor) { .cbuf2 = {
         .base_addr_shift_6 = addr_range.address >> 6,
         .size_shift_4 = addr_range.size >> 4,
      }};
   } else if (nvk_use_bindless_cbuf(&pdev->info)) {
      return (union nvk_buffer_descriptor) { .cbuf = {
         .base_addr_shift_4 = addr_range.address >> 4,
         .size_shift_4 = addr_range.size >> 4,
      }};
   } else {
      return (union nvk_buffer_descriptor) { .addr = {
         .base_addr = addr_range.address,
         .size = addr_range.size,
      }};
   }
}

static inline struct nvk_buffer_address
nvk_ubo_descriptor_addr(const struct nvk_physical_device *pdev,
                        union nvk_buffer_descriptor desc)
{
   if (nvk_use_bindless_cbuf_2(&pdev->info)) {
      return (struct nvk_buffer_address) {
         .base_addr = desc.cbuf2.base_addr_shift_6 << 6,
         .size = desc.cbuf2.size_shift_4 << 4,
      };
   } else if (nvk_use_bindless_cbuf(&pdev->info)) {
      return (struct nvk_buffer_address) {
         .base_addr = desc.cbuf.base_addr_shift_4 << 4,
         .size = desc.cbuf.size_shift_4 << 4,
      };
   } else {
      return desc.addr;
   }
}

static inline union nvk_buffer_descriptor
nvk_ssbo_descriptor(const struct nvk_physical_device *pdev,
                    VkDeviceAddressRangeEXT addr_range)
{
   const struct nvk_instance *instance = nvk_physical_device_instance(pdev);
   const uint32_t min_ssbo_alignment = nvk_min_ssbo_alignment(instance);
   assert(addr_range.address % min_ssbo_alignment == 0);
   assert(addr_range.size <= UINT32_MAX);

   addr_range.address = ROUND_DOWN_TO(addr_range.address, min_ssbo_alignment);
   addr_range.size = align(addr_range.size, NVK_SSBO_BOUNDS_CHECK_ALIGNMENT);

   return (union nvk_buffer_descriptor) { .addr = {
      .base_addr = addr_range.address,
      .size = addr_range.size,
   }};
}

#endif
