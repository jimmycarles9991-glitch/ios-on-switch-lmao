// SPDX-License-Identifier: MIT
/*
 * nvkm symbols referenced from the vendored GM20B code paths but defined in nouveau files that are not
 * vendored (VBIOS, VRAM, PCI, therm, GSP-RM, VGA). On Tegra none of these subdevs exist, so the callers
 * never reach them at runtime (device->bios/pci/therm/fb->ram are NULL); the stubs only satisfy the
 * linker and fail safely if ever called. nv50_data_error_names is copied verbatim from
 * nouveau nvkm/engine/gr/nv50.c (Copyright 2012 Red Hat Inc., MIT) because gf100 GR error decoding uses it.
 */
#include <core/device.h>
#include <core/enum.h>
#include <subdev/bios.h>
#include <subdev/bios/bit.h>
#include <subdev/bios/boost.h>
#include <subdev/bios/cstep.h>
#include <subdev/bios/init.h>
#include <subdev/bios/M0203.h>
#include <subdev/bios/P0260.h>
#include <subdev/bios/perf.h>
#include <subdev/bios/vmap.h>
#include <subdev/bios/volt.h>
#include <subdev/bios/vpstate.h>
#include <subdev/fb.h>
#include <subdev/pci.h>
#include <subdev/therm.h>
#include <subdev/mmu.h>
#include <subdev/gsp.h>
#include <engine/disp.h>
#include "../nouveau/nvkm/subdev/fb/ram.h"
#include "../nouveau/nvkm/engine/gr/priv.h"
#include "../nouveau/nvkm/subdev/mmu/vmm.h"

int bit_entry(struct nvkm_bios *b, u8 id, struct bit_entry *e)
{
	(void)b;
	(void)id;
	(void)e;
	return -ENOENT;
}

u8 nvbios_rd08(struct nvkm_bios *b, u32 a)
{
	(void)b;
	(void)a;
	return 0;
}

u32 nvbios_rd32(struct nvkm_bios *b, u32 a)
{
	(void)b;
	(void)a;
	return 0;
}

int nvbios_exec(struct nvbios_init *i)
{
	(void)i;
	return -ENODEV;
}

/* nouveau's prototypes (the nvkm/subdev/bios headers): these stubs keep their signatures. */
/* NOLINTBEGIN(readability-non-const-parameter) */
u32 nvbios_boostEm(struct nvkm_bios *b, u8 i, u8 *v, u8 *h, u8 *c, u8 *l, struct nvbios_boostE *e)
{
	(void)b;
	(void)i;
	(void)v;
	(void)h;
	(void)c;
	(void)l;
	(void)e;
	return 0;
}

u32 nvbios_boostSp(struct nvkm_bios *b, int i, u32 d, u8 *v, u8 *h, u8 c, u8 l,
		   struct nvbios_boostS *s)
{
	(void)b;
	(void)i;
	(void)d;
	(void)v;
	(void)h;
	(void)c;
	(void)l;
	(void)s;
	return 0;
}

u32 nvbios_cstepEm(struct nvkm_bios *b, u8 p, u8 *v, u8 *h, struct nvbios_cstepE *e)
{
	(void)b;
	(void)p;
	(void)v;
	(void)h;
	(void)e;
	return 0;
}

u32 nvbios_cstepXp(struct nvkm_bios *b, int i, u8 *v, u8 *h, struct nvbios_cstepX *x)
{
	(void)b;
	(void)i;
	(void)v;
	(void)h;
	(void)x;
	return 0;
}

u32 nvbios_M0203Em(struct nvkm_bios *b, u8 r, u8 *v, u8 *h, struct nvbios_M0203E *e)
{
	(void)b;
	(void)r;
	(void)v;
	(void)h;
	(void)e;
	return 0;
}

u32 nvbios_P0260Ep(struct nvkm_bios *b, int i, u8 *v, u8 *h, struct nvbios_P0260E *e)
{
	(void)b;
	(void)i;
	(void)v;
	(void)h;
	(void)e;
	return 0;
}

u32 nvbios_P0260Xp(struct nvkm_bios *b, int i, u8 *v, u8 *h, struct nvbios_P0260X *x)
{
	(void)b;
	(void)i;
	(void)v;
	(void)h;
	(void)x;
	return 0;
}

u32 nvbios_perfEp(struct nvkm_bios *b, int i, u8 *v, u8 *h, u8 *c, u8 *l, struct nvbios_perfE *e)
{
	(void)b;
	(void)i;
	(void)v;
	(void)h;
	(void)c;
	(void)l;
	(void)e;
	return 0;
}

u32 nvbios_perfSp(struct nvkm_bios *b, u32 d, int i, u8 *v, u8 *h, u8 c, u8 l,
		  struct nvbios_perfS *s)
{
	(void)b;
	(void)d;
	(void)i;
	(void)v;
	(void)h;
	(void)c;
	(void)l;
	(void)s;
	return 0;
}

u32 nvbios_vmap_parse(struct nvkm_bios *b, u8 *v, u8 *h, u8 *c, u8 *l, struct nvbios_vmap *m)
{
	(void)b;
	(void)v;
	(void)h;
	(void)c;
	(void)l;
	(void)m;
	return 0;
}

u32 nvbios_vmap_entry_parse(struct nvkm_bios *b, int i, u8 *v, u8 *l, struct nvbios_vmap_entry *e)
{
	(void)b;
	(void)i;
	(void)v;
	(void)l;
	(void)e;
	return 0;
}

u32 nvbios_volt_parse(struct nvkm_bios *b, u8 *v, u8 *h, u8 *c, u8 *l, struct nvbios_volt *t)
{
	(void)b;
	(void)v;
	(void)h;
	(void)c;
	(void)l;
	(void)t;
	return 0;
}

u32 nvbios_volt_entry_parse(struct nvkm_bios *b, int i, u8 *v, u8 *l, struct nvbios_volt_entry *e)
{
	(void)b;
	(void)i;
	(void)v;
	(void)l;
	(void)e;
	return 0;
}

/* NOLINTEND(readability-non-const-parameter) */

int nvbios_vpstate_parse(struct nvkm_bios *b, struct nvbios_vpstate_header *h)
{
	(void)b;
	(void)h;
	return -ENODEV;
}

int nvbios_vpstate_entry(struct nvkm_bios *b, struct nvbios_vpstate_header *h, u8 i,
			 struct nvbios_vpstate_entry *e)
{
	(void)b;
	(void)h;
	(void)i;
	(void)e;
	return -ENODEV;
}

/* NOLINTNEXTLINE(misc-use-internal-linkage): nouveau's code links against it */
bool nvkm_lockvgac(struct nvkm_device *d, bool lock)
{
	(void)d;
	(void)lock;
	return false;
}

void nvkm_pci_msi_rearm(struct nvkm_device *d)
{
	(void)d;
}

int nvkm_pcie_set_link(struct nvkm_pci *p, enum nvkm_pcie_speed s, u8 w)
{
	(void)p;
	(void)s;
	(void)w;
	return 0;
}

int gf100_ram_new(struct nvkm_fb *fb, struct nvkm_ram **r)
{
	(void)fb;
	*r = NULL;
	return -ENODEV;
}

int gm200_ram_new(struct nvkm_fb *fb, struct nvkm_ram **r)
{
	(void)fb;
	*r = NULL;
	return -ENODEV;
}

int nvkm_ram_init(struct nvkm_ram *r)
{
	(void)r;
	return 0;
}

void nvkm_ram_del(struct nvkm_ram **r)
{
	*r = NULL;
}

int nvkm_ram_get(struct nvkm_device *d, u8 heap, u8 type, u8 page, u64 size, bool contig, bool back,
		 struct nvkm_memory **m)
{
	(void)d;
	(void)heap;
	(void)type;
	(void)page;
	(void)size;
	(void)contig;
	(void)back;
	*m = NULL;
	return -ENOSYS;
}

void nvkm_therm_clkgate_enable(struct nvkm_therm *t)
{
	(void)t;
}

void nvkm_therm_clkgate_fini(struct nvkm_therm *t, bool s)
{
	(void)t;
	(void)s;
}

void nvkm_therm_clkgate_init(struct nvkm_therm *t, const struct nvkm_therm_clkgate_pack *p)
{
	(void)t;
	(void)p;
}

int nvkm_therm_cstate(struct nvkm_therm *t, int f, int d)
{
	(void)t;
	(void)f;
	(void)d;
	return 0;
}

void r535_mmu_vaspace_del(struct nvkm_vmm *v)
{
	(void)v;
}

int nvkm_gsp_gcx_ready(struct nvkm_gsp *g)
{
	(void)g;
	return -ENODEV;
} /* no GSP on GM20B */

/* verbatim from nouveau, which Linux builds with -Wno-missing-field-initializers */
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
const struct nvkm_enum nv50_data_error_names[] = {
	{0x00000003, "INVALID_OPERATION", NULL},
	{0x00000004, "INVALID_VALUE", NULL},
	{0x00000005, "INVALID_ENUM", NULL},
	{0x00000008, "INVALID_OBJECT", NULL},
	{0x00000009, "READ_ONLY_OBJECT", NULL},
	{0x0000000a, "SUPERVISOR_OBJECT", NULL},
	{0x0000000b, "INVALID_ADDRESS_ALIGNMENT", NULL},
	{0x0000000c, "INVALID_BITFIELD", NULL},
	{0x0000000d, "BEGIN_END_ACTIVE", NULL},
	{0x0000000e, "SEMANTIC_COLOR_BACK_OVER_LIMIT", NULL},
	{0x0000000f, "VIEWPORT_ID_NEEDS_GP", NULL},
	{0x00000010, "RT_DOUBLE_BIND", NULL},
	{0x00000011, "RT_TYPES_MISMATCH", NULL},
	{0x00000012, "RT_LINEAR_WITH_ZETA", NULL},
	{0x00000015, "FP_TOO_FEW_REGS", NULL},
	{0x00000016, "ZETA_FORMAT_CSAA_MISMATCH", NULL},
	{0x00000017, "RT_LINEAR_WITH_MSAA", NULL},
	{0x00000018, "FP_INTERPOLANT_START_OVER_LIMIT", NULL},
	{0x00000019, "SEMANTIC_LAYER_OVER_LIMIT", NULL},
	{0x0000001a, "RT_INVALID_ALIGNMENT", NULL},
	{0x0000001b, "SAMPLER_OVER_LIMIT", NULL},
	{0x0000001c, "TEXTURE_OVER_LIMIT", NULL},
	{0x0000001e, "GP_TOO_MANY_OUTPUTS", NULL},
	{0x0000001f, "RT_BPP128_WITH_MS8", NULL},
	{0x00000021, "Z_OUT_OF_BOUNDS", NULL},
	{0x00000023, "XY_OUT_OF_BOUNDS", NULL},
	{0x00000024, "VP_ZERO_INPUTS", NULL},
	{0x00000027, "CP_MORE_PARAMS_THAN_SHARED", NULL},
	{0x00000028, "CP_NO_REG_SPACE_STRIPED", NULL},
	{0x00000029, "CP_NO_REG_SPACE_PACKED", NULL},
	{0x0000002a, "CP_NOT_ENOUGH_WARPS", NULL},
	{0x0000002b, "CP_BLOCK_SIZE_MISMATCH", NULL},
	{0x0000002c, "CP_NOT_ENOUGH_LOCAL_WARPS", NULL},
	{0x0000002d, "CP_NOT_ENOUGH_STACK_WARPS", NULL},
	{0x0000002e, "CP_NO_BLOCKDIM_LATCH", NULL},
	{0x00000031, "ENG2D_FORMAT_MISMATCH", NULL},
	{0x0000003f, "PRIMITIVE_ID_NEEDS_GP", NULL},
	{0x00000044, "SEMANTIC_VIEWPORT_OVER_LIMIT", NULL},
	{0x00000045, "SEMANTIC_COLOR_FRONT_OVER_LIMIT", NULL},
	{0x00000046, "LAYER_ID_NEEDS_GP", NULL},
	{0x00000047, "SEMANTIC_CLIP_OVER_LIMIT", NULL},
	{0x00000048, "SEMANTIC_PTSZ_OVER_LIMIT", NULL},
	{}};
