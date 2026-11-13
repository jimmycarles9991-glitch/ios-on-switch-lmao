// SPDX-License-Identifier: MIT
#include "SwitchGM20BEmitCopy.h"
#include "TegraGM20BCopyEmit.h"
#include "TegraGM20BRelayout.h"
#include "nv_push_clb06f.h"
#include "nv_push_clb0b5.h"

_Static_assert(TGM_COPY_RECT_DWORDS == SWITCH_GM20B_COPY_RECT_WORDS(1), "the kit's copy bound");

static struct nv_push tgm_push(uint32_t *w, uint32_t words)
{
	struct nv_push p;
	nv_push_init(&p, w, words, SUBC_MASK_ALL);
	return p;
}

static uint32_t tgm_copy(uint32_t *w, const struct switch_gm20b_copy *copy)
{
	struct nv_push p = tgm_push(w, TGM_COPY_RECT_DWORDS);
	switch_gm20b_copy_rect(&p, copy, NVB0B5_LAUNCH_DMA_DATA_TRANSFER_TYPE_NON_PIPELINED);
	return (uint32_t)nv_push_dw_count(&p);
}

static struct switch_gm20b_copy_surface tgm_side(const struct tgm_copy_side *s, uint32_t height)
{
	struct switch_gm20b_copy_surface c = {0};
	c.base_addr = s->va;
	c.bpp = 4;
	c.row_stride = s->pitch;
	c.tiled = s->block_log2 >= 0;
	c.y_log2 = c.tiled ? (uint8_t)s->block_log2 : 0;
	c.extent_el.height = height;
	c.extent_el.depth = 1;
	return c;
}

/* The present's copy and its read-backs. LAUNCH_DMA's BYPASS_L2 stays USE_PTE_SETTING, as nvgpu and NVK leave it: the
 * present's mappings are volatile PTEs already (gpu_present.c tgm_present_map), so its writes reach DRAM at the fence's
 * FB_FLUSH with no L2 flush (chan_fence_volatile). Device boot T-v3 set FORCE_VOLATILE here and its copy engine never
 * finished switching off the present's context once another copy channel wanted the engine. */
uint32_t tgm_copy_texels(uint32_t *w, const struct tgm_copy_side *src,
			 const struct tgm_copy_side *dst, uint32_t width, uint32_t height)
{
	struct switch_gm20b_copy copy = {0};
	copy.src = tgm_side(src, height);
	copy.dst = tgm_side(dst, height);
	copy.extent_el.width = width;
	copy.extent_el.height = height;
	copy.extent_el.depth = copy.extent_el.array_len = 1;
	return tgm_copy(w, &copy);
}

uint32_t tgm_copy_rows(uint32_t *w, uint64_t src, uint32_t src_pitch, uint64_t dst,
		       uint32_t dst_pitch, uint32_t row_bytes, uint32_t rows)
{
	struct switch_gm20b_copy copy = {0};
	copy.src.base_addr = src;
	copy.src.row_stride = src_pitch;
	copy.dst.base_addr = dst;
	copy.dst.row_stride = dst_pitch;
	copy.src.bpp = copy.dst.bpp = 1;
	copy.extent_el.width = row_bytes;
	copy.extent_el.height = rows;
	copy.extent_el.depth = copy.extent_el.array_len = 1;
	return tgm_copy(w, &copy);
}

uint32_t tgm_relayout_copy(uint32_t *w, const struct tgm_layout *l, uint64_t src, int src_tiled,
			   uint64_t dst, int dst_tiled, uint32_t pitch)
{
	struct switch_gm20b_copy_surface block = {0}, linear = {0};
	block.bpp = linear.bpp = l->bpp;
	block.tiled = true;
	block.row_stride = l->row_bytes;
	block.extent_el.height = l->height;
	block.extent_el.depth = 1;
	block.y_log2 = l->y_log2;
	linear.row_stride = pitch;
	struct switch_gm20b_copy copy = {0};
	copy.src = src_tiled ? block : linear;
	copy.dst = dst_tiled ? block : linear;
	copy.src.base_addr = src;
	copy.dst.base_addr = dst;
	copy.remap.comp_size = (uint8_t)(l->bpp < 4 ? l->bpp : 4);
	for (uint8_t i = 0; i < 4; i++)
		copy.remap.dst[i] = i;
	copy.extent_el.width = l->width;
	copy.extent_el.height = l->height;
	copy.extent_el.depth = copy.extent_el.array_len = 1;
	return tgm_copy(w, &copy);
}

static void tgm_fill_remap(struct nv_push *p)
{
	P_NVB0B5_SET_REMAP_COMPONENTS(p, {
						 .dst_x = DST_X_CONST_A,
						 .component_size = COMPONENT_SIZE_FOUR,
						 .num_src_components = NUM_SRC_COMPONENTS_ONE,
						 .num_dst_components = NUM_DST_COMPONENTS_ONE,
					 });
}

static void tgm_fill_launch(struct nv_push *p, int multi_line)
{
	P_IMMD(p, NVB0B5, LAUNCH_DMA,
	       {
		       .data_transfer_type = DATA_TRANSFER_TYPE_NON_PIPELINED,
		       .flush_enable = FLUSH_ENABLE_TRUE,
		       .src_memory_layout = SRC_MEMORY_LAYOUT_PITCH,
		       .dst_memory_layout = DST_MEMORY_LAYOUT_PITCH,
		       .multi_line_enable =
			       multi_line ? MULTI_LINE_ENABLE_TRUE : MULTI_LINE_ENABLE_FALSE,
		       .remap_enable = REMAP_ENABLE_TRUE,
	       });
}

uint32_t tgm_fill_setup(uint32_t *w, uint32_t value)
{
	struct nv_push p = tgm_push(w, TGM_FILL_SETUP_DWORDS);
	P_MTHD(&p, NVB0B5, SET_REMAP_CONST_A);
	P_NVB0B5_SET_REMAP_CONST_A(&p, value);
	P_MTHD(&p, NVB0B5, SET_REMAP_COMPONENTS);
	tgm_fill_remap(&p);
	return (uint32_t)nv_push_dw_count(&p);
}

uint32_t tgm_fill_pitch(uint32_t *w, uint32_t pitch)
{
	struct nv_push p = tgm_push(w, TGM_FILL_PITCH_DWORDS);
	P_MTHD(&p, NVB0B5, PITCH_IN);
	P_NVB0B5_PITCH_IN(&p, pitch);
	P_NVB0B5_PITCH_OUT(&p, pitch);
	return (uint32_t)nv_push_dw_count(&p);
}

uint32_t tgm_fill_lines(uint32_t *w, uint64_t va, uint32_t line_bytes, uint32_t lines,
			int multi_line)
{
	struct nv_push p = tgm_push(w, TGM_FILL_LINES_DWORDS);
	P_MTHD(&p, NVB0B5, OFFSET_OUT_UPPER);
	P_NVB0B5_OFFSET_OUT_UPPER(&p, (uint32_t)(va >> 32));
	P_NVB0B5_OFFSET_OUT_LOWER(&p, (uint32_t)va);
	P_MTHD(&p, NVB0B5, LINE_LENGTH_IN);
	P_NVB0B5_LINE_LENGTH_IN(&p, line_bytes / 4);
	P_NVB0B5_LINE_COUNT(&p, lines);
	tgm_fill_launch(&p, multi_line);
	return (uint32_t)nv_push_dw_count(&p);
}

uint32_t tgm_relayout_clear(uint32_t *w, uint64_t va, uint64_t bytes)
{
	struct nv_push p = tgm_push(w, TGM_RELAYOUT_CLEAR_DWORDS);
	P_MTHD(&p, NVB0B5, SET_REMAP_CONST_A);
	P_NVB0B5_SET_REMAP_CONST_A(&p, 0);
	P_NVB0B5_SET_REMAP_CONST_B(&p, 0);
	tgm_fill_remap(&p);
	P_MTHD(&p, NVB0B5, OFFSET_OUT_UPPER);
	P_NVB0B5_OFFSET_OUT_UPPER(&p, (uint32_t)(va >> 32));
	P_NVB0B5_OFFSET_OUT_LOWER(&p, (uint32_t)va);
	P_NVB0B5_PITCH_IN(&p, 4096);
	P_NVB0B5_PITCH_OUT(&p, 4096);
	P_NVB0B5_LINE_LENGTH_IN(&p, 1024);
	P_NVB0B5_LINE_COUNT(&p, (uint32_t)(bytes / 4096));
	tgm_fill_launch(&p, 1);
	return (uint32_t)nv_push_dw_count(&p);
}

uint32_t tgm_relayout_acquire(uint32_t *w, const struct tgm_relayout_wait *wait)
{
	struct nv_push p = tgm_push(w, TGM_RELAYOUT_ACQUIRE_DWORDS);
	P_MTHD(&p, NVB06F, SEMAPHOREA);
	P_NVB06F_SEMAPHOREA(&p, (uint32_t)(wait->va >> 32));
	P_NVB06F_SEMAPHOREB(&p, (uint32_t)wait->va >> 2);
	P_NVB06F_SEMAPHOREC(&p, wait->seqno);
	P_NVB06F_SEMAPHORED(&p, {
					.operation = OPERATION_ACQ_GEQ,
					.acquire_switch = ACQUIRE_SWITCH_ENABLED,
				});
	return (uint32_t)nv_push_dw_count(&p);
}

uint32_t tgm_relayout_checkpoint(uint32_t *w, uint64_t address)
{
	if (!address)
		return 0;
	struct nv_push p = tgm_push(w, TGM_RELAYOUT_CHECKPOINT_DWORDS);
	P_MTHD(&p, NVB06F, WFI);
	P_NVB06F_WFI(&p, NVB06F_WFI_SCOPE_CURRENT_SCG_TYPE);
	P_MTHD(&p, NVB06F, FB_FLUSH);
	P_NVB06F_FB_FLUSH(&p, 0);
	P_MTHD(&p, NVB06F, MEM_OP_C);
	P_NVB06F_MEM_OP_C(&p, {0});
	P_NVB06F_MEM_OP_D(&p, {.operation = OPERATION_L2_FLUSH_DIRTY});
	P_MTHD(&p, NVB06F, FB_FLUSH);
	P_NVB06F_FB_FLUSH(&p, 0);
	P_MTHD(&p, NVB06F, SEMAPHOREA);
	P_NVB06F_SEMAPHOREA(&p, (uint32_t)(address >> 32));
	P_NVB06F_SEMAPHOREB(&p, (uint32_t)address >> 2);
	P_NVB06F_SEMAPHOREC(&p, 1);
	P_NVB06F_SEMAPHORED(&p, {
					.operation = OPERATION_RELEASE,
					.release_size = RELEASE_SIZE_4BYTE,
				});
	return (uint32_t)nv_push_dw_count(&p);
}
