// SPDX-License-Identifier: MIT
#pragma once

void nvkm_memory_tags_put(struct nvkm_memory *memory, struct nvkm_device *device,
			  struct nvkm_tags **ptags)
{
	struct nvkm_fb *fb = device->fb;
	struct nvkm_tags *tags = *ptags;
	if (tags) {
		mutex_lock(&fb->tags.mutex);
		if (refcount_dec_and_test(&tags->refcount)) {
			nvkm_mm_free(&fb->tags.mm, &tags->mn);
			kfree(memory->tags);
			memory->tags = NULL;
			memory->switch_tags_held = false;
		}
		mutex_unlock(&fb->tags.mutex);
		*ptags = NULL;
	}
}

int nvkm_memory_tags_get(struct nvkm_memory *memory, struct nvkm_device *device, u32 nr,
			 void (*clr)(struct nvkm_device *, u32, u32), struct nvkm_tags **ptags)
{
	struct nvkm_fb *fb = device->fb;
	struct nvkm_tags *tags;

	mutex_lock(&fb->tags.mutex);
	if ((tags = memory->tags)) {
		/* If comptags exist for the memory, but a different amount
		 * than requested, the buffer is being mapped with settings
		 * that are incompatible with existing mappings.
		 */
		if (tags->mn && tags->mn->length != nr) {
			mutex_unlock(&fb->tags.mutex);
			return -EINVAL;
		}

		refcount_inc(&tags->refcount);
		mutex_unlock(&fb->tags.mutex);
		*ptags = tags;
		return 0;
	}

	if (!(tags = kmalloc_obj(*tags))) {
		mutex_unlock(&fb->tags.mutex);
		return -ENOMEM;
	}

	if (!nvkm_mm_head(&fb->tags.mm, 0, 1, nr, nr, 1, &tags->mn)) {
		if (clr)
			clr(device, tags->mn->offset, tags->mn->length);
	} else {
		/* Failure to allocate HW comptags is not an error, the
		 * caller should fall back to an uncompressed map.
		 *
		 * As memory can be mapped in multiple places, we still
		 * need to track the allocation failure and ensure that
		 * any additional mappings remain uncompressed.
		 *
		 * This is handled by returning an empty nvkm_tags.
		 */
		tags->mn = NULL;
	}

	refcount_set(&tags->refcount, 1);
	*ptags = memory->tags = tags;
	mutex_unlock(&fb->tags.mutex);
	return 0;
}

void nvkm_memory_tags_hold(struct nvkm_memory *memory, struct nvkm_device *device,
			   struct nvkm_tags **ptags)
{
	struct nvkm_tags *tags = *ptags;
	if (!tags)
		return;
	mutex_lock(&device->fb->tags.mutex);
	if (memory->switch_tags_held)
		WARN_ON(refcount_dec_and_test(&tags->refcount));
	else
		memory->switch_tags_held = true;
	mutex_unlock(&device->fb->tags.mutex);
	*ptags = NULL;
}
