/* SPDX-License-Identifier: MIT */
#pragma once
#include <core/os.h>

struct nvkm_object;
struct nvkm_uevent;
struct nvkm_client;

enum nvkm_suspend_state {
	NVKM_POWEROFF,
	NVKM_SUSPEND
};

struct nvkm_object_func {
	void *(*dtor)(struct nvkm_object *);
	int (*init)(struct nvkm_object *);
	int (*fini)(struct nvkm_object *, enum nvkm_suspend_state);
	int (*mthd)(struct nvkm_object *, u32, void *, u32);
	int (*uevent)(struct nvkm_object *, void *, u32, struct nvkm_uevent *);
};

struct nvkm_object {
	const struct nvkm_object_func *func;
	struct nvkm_client *client;
	u64 object;
};

struct nvkm_client {
	int (*event)(u64, void *, u32);
};

struct nvkm_oclass {
	struct nvkm_object *parent;
	struct nvkm_client *client;
	u64 object;
};

static inline void nvkm_object_ctor(const struct nvkm_object_func *func,
				    const struct nvkm_oclass *oclass, struct nvkm_object *object)
{
	object->func = func;
	object->client = oclass->client;
	object->object = oclass->object;
}
