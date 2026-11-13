/* Switch: trimmed by tools/trim_base.py from nouveau nvkm/engine/device/base.c (torvalds/linux d89e1d1):
 * all chipset tables except nv12b (GM20B) and their switch cases removed; nothing else changed. */
/*
 * Copyright 2012 Red Hat Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * Authors: Ben Skeggs
 */
#include "priv.h"
#include "acpi.h"

#include <core/option.h>

#include <subdev/bios.h>
#include <subdev/therm.h>

static DEFINE_MUTEX(nv_devices_mutex);
static LIST_HEAD(nv_devices);

static struct nvkm_device *
nvkm_device_find_locked(u64 handle)
{
	struct nvkm_device *device;
	list_for_each_entry(device, &nv_devices, head) {
		if (device->handle == handle)
			return device;
	}
	return NULL;
}

struct nvkm_device *
nvkm_device_find(u64 handle)
{
	struct nvkm_device *device;
	mutex_lock(&nv_devices_mutex);
	device = nvkm_device_find_locked(handle);
	mutex_unlock(&nv_devices_mutex);
	return device;
}

static const struct nvkm_device_chip
nv12b_chipset = {
	.name = "GM20B",
	.acr      = { 0x00000001, gm20b_acr_new },
	.bar      = { 0x00000001, gm20b_bar_new },
	.bus      = { 0x00000001, gf100_bus_new },
	.clk      = { 0x00000001, gm20b_clk_new },
	.fb       = { 0x00000001, gm20b_fb_new },
	.fuse     = { 0x00000001, gm107_fuse_new },
	.imem     = { 0x00000001, gk20a_instmem_new },
	.ltc      = { 0x00000001, gm20b_ltc_new },
	.mc       = { 0x00000001, gk20a_mc_new },
	.mmu      = { 0x00000001, gm20b_mmu_new },
	.pmu      = { 0x00000001, gm20b_pmu_new },
	.privring = { 0x00000001, gk20a_privring_new },
	.timer    = { 0x00000001, gk20a_timer_new },
	.top      = { 0x00000001, gk104_top_new },
	.volt     = { 0x00000001, gm20b_volt_new },
	.ce       = { 0x00000004, gm200_ce_new },
	.dma      = { 0x00000001, gf119_dma_new },
	.fifo     = { 0x00000001, gm200_fifo_new },
	.gr       = { 0x00000001, gm20b_gr_new },
	.sw       = { 0x00000001, gf100_sw_new },
};

struct nvkm_subdev *
nvkm_device_subdev(struct nvkm_device *device, int type, int inst)
{
	struct nvkm_subdev *subdev;

	list_for_each_entry(subdev, &device->subdev, head) {
		if (subdev->type == type && subdev->inst == inst)
			return subdev;
	}

	return NULL;
}

struct nvkm_engine *
nvkm_device_engine(struct nvkm_device *device, int type, int inst)
{
	struct nvkm_subdev *subdev = nvkm_device_subdev(device, type, inst);
	if (subdev && subdev->func == &nvkm_engine)
		return container_of(subdev, struct nvkm_engine, subdev);
	return NULL;
}

int
nvkm_device_fini(struct nvkm_device *device, enum nvkm_suspend_state suspend)
{
	const char *action;
	struct nvkm_subdev *subdev;
	int ret;
	s64 time;

	switch (suspend) {
	case NVKM_POWEROFF:
	default:
		action = "fini";
		break;
	case NVKM_SUSPEND:
		action = "suspend";
		break;
	case NVKM_RUNTIME_SUSPEND:
		action = "runtime";
		break;
	}
	nvdev_trace(device, "%s running...\n", action);
	time = ktime_to_us(ktime_get());

	nvkm_acpi_fini(device);

	list_for_each_entry_reverse(subdev, &device->subdev, head) {
		ret = nvkm_subdev_fini(subdev, suspend);
		if (ret && suspend)
			goto fail;
	}

	nvkm_therm_clkgate_fini(device->therm, suspend);

	if (device->func->fini)
		device->func->fini(device, suspend);

	nvkm_intr_unarm(device);

	time = ktime_to_us(ktime_get()) - time;
	nvdev_trace(device, "%s completed in %lldus...\n", action, time);
	return 0;

fail:
	list_for_each_entry_from(subdev, &device->subdev, head) {
		int rret = nvkm_subdev_init(subdev);
		if (rret)
			nvkm_fatal(subdev, "failed restart, %d\n", ret);
	}

	nvdev_trace(device, "%s failed with %d\n", action, ret);
	return ret;
}

static int
nvkm_device_preinit(struct nvkm_device *device)
{
	struct nvkm_subdev *subdev;
	int ret;
	s64 time;

	nvdev_trace(device, "preinit running...\n");
	time = ktime_to_us(ktime_get());

	nvkm_intr_unarm(device);

	if (device->func->preinit) {
		ret = device->func->preinit(device);
		if (ret)
			goto fail;
	}

	list_for_each_entry(subdev, &device->subdev, head) {
		ret = nvkm_subdev_preinit(subdev);
		if (ret)
			goto fail;
	}

	ret = nvkm_devinit_post(device->devinit);
	if (ret)
		goto fail;

	ret = nvkm_top_parse(device);
	if (ret)
		goto fail;

	ret = nvkm_fb_mem_unlock(device->fb);
	if (ret)
		goto fail;

	time = ktime_to_us(ktime_get()) - time;
	nvdev_trace(device, "preinit completed in %lldus\n", time);
	return 0;

fail:
	nvdev_error(device, "preinit failed with %d\n", ret);
	return ret;
}

int
nvkm_device_init(struct nvkm_device *device)
{
	struct nvkm_subdev *subdev;
	int ret;
	s64 time;

	ret = nvkm_device_preinit(device);
	if (ret)
		return ret;

	nvkm_device_fini(device, NVKM_POWEROFF);

	nvdev_trace(device, "init running...\n");
	time = ktime_to_us(ktime_get());

	nvkm_intr_rearm(device);

	if (device->func->init) {
		ret = device->func->init(device);
		if (ret)
			goto fail;
	}

	list_for_each_entry(subdev, &device->subdev, head) {
		ret = nvkm_subdev_init(subdev);
		if (ret)
			goto fail_subdev;
	}

	nvkm_acpi_init(device);
	nvkm_therm_clkgate_enable(device->therm);

	time = ktime_to_us(ktime_get()) - time;
	nvdev_trace(device, "init completed in %lldus\n", time);
	return 0;

fail_subdev:
	list_for_each_entry_from(subdev, &device->subdev, head)
		nvkm_subdev_fini(subdev, NVKM_POWEROFF);
fail:
	nvkm_device_fini(device, NVKM_POWEROFF);

	nvdev_error(device, "init failed with %d\n", ret);
	return ret;
}

void
nvkm_device_del(struct nvkm_device **pdevice)
{
	struct nvkm_device *device = *pdevice;
	struct nvkm_subdev *subdev, *subtmp;
	if (device) {
		mutex_lock(&nv_devices_mutex);

		nvkm_intr_dtor(device);

		list_for_each_entry_safe_reverse(subdev, subtmp, &device->subdev, head)
			nvkm_subdev_del(&subdev);

		if (device->pri)
			iounmap(device->pri);
		list_del(&device->head);

		if (device->func->dtor)
			*pdevice = device->func->dtor(device);
		mutex_unlock(&nv_devices_mutex);

		kfree(*pdevice);
		*pdevice = NULL;
	}
}

/* returns true if the GPU is in the CPU native byte order */
static inline bool
nvkm_device_endianness(struct nvkm_device *device)
{
#ifdef __BIG_ENDIAN
	const bool big_endian = true;
#else
	const bool big_endian = false;
#endif

	/* Read NV_PMC_BOOT_1, and assume non-functional endian switch if it
	 * doesn't contain the expected values.
	 */
	u32 pmc_boot_1 = nvkm_rd32(device, 0x000004);
	if (pmc_boot_1 && pmc_boot_1 != 0x01000001)
		return !big_endian; /* Assume GPU is LE in this case. */

	/* 0 means LE and 0x01000001 means BE GPU. Condition is true when
	 * GPU/CPU endianness don't match.
	 */
	if (big_endian == !pmc_boot_1) {
		nvkm_wr32(device, 0x000004, 0x01000001);
		nvkm_rd32(device, 0x000000);
		if (nvkm_rd32(device, 0x000004) != (big_endian ? 0x01000001 : 0x00000000))
			return !big_endian; /* Assume GPU is LE on any unexpected read-back. */
	}

	/* CPU/GPU endianness should (hopefully) match. */
	return true;
}

int
nvkm_device_ctor(const struct nvkm_device_func *func,
		 const struct nvkm_device_quirk *quirk,
		 struct device *dev, enum nvkm_device_type type, u64 handle,
		 const char *name, const char *cfg, const char *dbg,
		 struct nvkm_device *device)
{
	struct nvkm_subdev *subdev;
	u64 mmio_base, mmio_size;
	u32 boot0, boot1, strap;
	int ret = -EEXIST, j;
	unsigned chipset;

	mutex_lock(&nv_devices_mutex);
	if (nvkm_device_find_locked(handle))
		goto done;

	device->func = func;
	device->quirk = quirk;
	device->dev = dev;
	device->type = type;
	device->handle = handle;
	device->cfgopt = cfg;
	device->dbgopt = dbg;
	device->name = name;
	list_add_tail(&device->head, &nv_devices);
	device->debug = nvkm_dbgopt(device->dbgopt, "device");
	INIT_LIST_HEAD(&device->subdev);

	mmio_base = device->func->resource_addr(device, NVKM_BAR0_PRI);
	mmio_size = device->func->resource_size(device, NVKM_BAR0_PRI);

	device->pri = ioremap(mmio_base, mmio_size);
	if (device->pri == NULL) {
		nvdev_error(device, "unable to map PRI\n");
		ret = -ENOMEM;
		goto done;
	}

	/* identify the chipset, and determine classes of subdev/engines */

	/* switch mmio to cpu's native endianness */
	if (!nvkm_device_endianness(device)) {
		nvdev_error(device,
			    "Couldn't switch GPU to CPUs endianness\n");
		ret = -ENOSYS;
		goto done;
	}

	boot0 = nvkm_rd32(device, 0x000000);

	/* chipset can be overridden for devel/testing purposes */
	chipset = nvkm_longopt(device->cfgopt, "NvChipset", 0);
	if (chipset) {
		u32 override_boot0;

		if (chipset >= 0x10) {
			override_boot0  = ((chipset & 0x1ff) << 20);
			override_boot0 |= 0x000000a1;
		} else {
			if (chipset != 0x04)
				override_boot0 = 0x20104000;
			else
				override_boot0 = 0x20004000;
		}

		nvdev_warn(device, "CHIPSET OVERRIDE: %08x -> %08x\n",
			   boot0, override_boot0);
		boot0 = override_boot0;
	}

	/* determine chipset and derive architecture from it */
	if ((boot0 & 0x1f000000) > 0) {
		device->chipset = (boot0 & 0x1ff00000) >> 20;
		device->chiprev = (boot0 & 0x000000ff);
		switch (device->chipset & 0x1f0) {
		case 0x010: {
			if (0x461 & (1 << (device->chipset & 0xf)))
				device->card_type = NV_10;
			else
				device->card_type = NV_11;
			device->chiprev = 0x00;
			break;
		}
		case 0x020: device->card_type = NV_20; break;
		case 0x030: device->card_type = NV_30; break;
		case 0x040:
		case 0x060: device->card_type = NV_40; break;
		case 0x050:
		case 0x080:
		case 0x090:
		case 0x0a0: device->card_type = NV_50; break;
		case 0x0c0:
		case 0x0d0: device->card_type = NV_C0; break;
		case 0x0e0:
		case 0x0f0:
		case 0x100: device->card_type = NV_E0; break;
		case 0x110:
		case 0x120: device->card_type = GM100; break;
		case 0x130: device->card_type = GP100; break;
		case 0x140: device->card_type = GV100; break;
		case 0x160: device->card_type = TU100; break;
		case 0x170: device->card_type = GA100; break;
		case 0x180: device->card_type = GH100; break;
		case 0x190: device->card_type = AD100; break;
		case 0x1a0: device->card_type = GB10x; break;
		case 0x1b0: device->card_type = GB20x; break;
		default:
			break;
		}
	} else
	if ((boot0 & 0xff00fff0) == 0x20004000) {
		if (boot0 & 0x00f00000)
			device->chipset = 0x05;
		else
			device->chipset = 0x04;
		device->card_type = NV_04;
	}

	switch (device->chipset) {
	case 0x12b: device->chip = &nv12b_chipset; break;
	/* T210B01 (Switch OLED) reports GM20B_B; nvgpu runs it on the same gm20b HAL (hal_init.c GK20A_GPUID_GM20B_B,
	 * gk20a.h 0x12E). Its GPCPLL differs (C1), which gpu_bringup.c handles by keeping speedo id 0 (fixed-frequency path). */
	case 0x12e: device->chip = &nv12b_chipset; break;
	default:
		if (!device->chip) {
			nvdev_error(device, "unknown chipset (%08x)\n", boot0);
			ret = -ENODEV;
			goto done;
		}
		break;
	}

	nvdev_info(device, "NVIDIA %s (%08x)\n",
		   device->chip->name, boot0);

	/* vGPU detection */
	boot1 = nvkm_rd32(device, 0x0000004);
	if (device->card_type >= TU100 && (boot1 & 0x00030000)) {
		nvdev_info(device, "vGPUs are not supported\n");
		ret = -ENODEV;
		goto done;
	}

	/* read strapping information */
	strap = nvkm_rd32(device, 0x101000);

	/* determine frequency of timing crystal */
	if ( device->card_type <= NV_10 || device->chipset < 0x17 ||
	    (device->chipset >= 0x20 && device->chipset < 0x25))
		strap &= 0x00000040;
	else
		strap &= 0x00400040;

	switch (strap) {
	case 0x00000000: device->crystal = 13500; break;
	case 0x00000040: device->crystal = 14318; break;
	case 0x00400000: device->crystal = 27000; break;
	case 0x00400040: device->crystal = 25000; break;
	}

	if (!device->name)
		device->name = device->chip->name;

	mutex_init(&device->mutex);
	nvkm_intr_ctor(device);

#define NVKM_LAYOUT_ONCE(type,data,ptr)                                                      \
	if (device->chip->ptr.inst) {                                                        \
		WARN_ON(device->chip->ptr.inst != 0x00000001);                               \
		ret = device->chip->ptr.ctor(device, (type), -1, &device->ptr);              \
		subdev = nvkm_device_subdev(device, (type), 0);                              \
		if (ret) {                                                                   \
			nvkm_subdev_del(&subdev);                                            \
			device->ptr = NULL;                                                  \
			if (ret != -ENODEV) {                                                \
				nvdev_error(device, "%s ctor failed: %d\n",                  \
					    nvkm_subdev_type[(type)], ret);                  \
				goto done;                                                   \
			}                                                                    \
		} else {                                                                     \
			subdev->pself = (void **)&device->ptr;                               \
		}                                                                            \
	}
#define NVKM_LAYOUT_INST(type,data,ptr,cnt)                                                  \
	WARN_ON(device->chip->ptr.inst & ~((1 << ARRAY_SIZE(device->ptr)) - 1));             \
	for (j = 0; device->chip->ptr.inst && j < ARRAY_SIZE(device->ptr); j++) {            \
		if (device->chip->ptr.inst & BIT(j)) {                                       \
			ret = device->chip->ptr.ctor(device, (type), (j), &device->ptr[j]);  \
			subdev = nvkm_device_subdev(device, (type), (j));                    \
			if (ret) {                                                           \
				nvkm_subdev_del(&subdev);                                    \
				device->ptr[j] = NULL;                                       \
				if (ret != -ENODEV) {                                        \
					nvdev_error(device, "%s%d ctor failed: %d\n",        \
						    nvkm_subdev_type[(type)], (j), ret);     \
					goto done;                                           \
				}                                                            \
			} else {                                                             \
				subdev->pself = (void **)&device->ptr[j];                    \
			}                                                                    \
		}                                                                            \
	}
#include <core/layout.h>
#undef NVKM_LAYOUT_INST
#undef NVKM_LAYOUT_ONCE

	ret = nvkm_intr_install(device);
done:
	if (ret && device->pri) {
		iounmap(device->pri);
		device->pri = NULL;
	}
	mutex_unlock(&nv_devices_mutex);
	return ret;
}
