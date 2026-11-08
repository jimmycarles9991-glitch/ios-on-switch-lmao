/*
 * Copyright © 2026 Bruno St John
 * SPDX-License-Identifier: MIT
 *
 * Switch: the raw GM20B path's reads of a command buffer NVK recorded
 * (nvk_switch_capture.c). The plugin finds them with dlsym.
 */

#ifndef NVK_SWITCH_CAPTURE_H
#define NVK_SWITCH_CAPTURE_H 1

#include <stdbool.h>
#include <stdint.h>

struct nvk_cbuf;
struct nvk_device;
struct nvk_shader;
struct switch_nvk_programs;

/* A constant buffer as the raw path's pack names it (raw_pack.h PackCbufType) */
uint8_t switch_nvk_cbuf_code(const struct nvk_cbuf *cbuf);

struct switch_nvk_stage;

/* What switch_nvk_compile_stage hands over of a compiled shader */
void switch_nvk_stage_of(const struct nvk_shader *shader,
                         struct switch_nvk_stage *out);

/* nvk_device's and nvk_shader's hooks: the registry the capture names a bound
 * program from
 */
void switch_nvk_programs_init(struct nvk_device *dev);
void switch_nvk_programs_finish(struct nvk_device *dev);
void switch_nvk_program_uploaded(struct nvk_device *dev,
                                 const struct nvk_shader *shader);
void switch_nvk_program_freed(struct nvk_device *dev,
                              const struct nvk_shader *shader);

void switch_nvk_capture_cmd_buffer(uint64_t command_buffer,
                                   void (*segment)(void *ctx, uint64_t addr,
                                                   const uint32_t *cpu,
                                                   uint32_t dwords),
                                   void *ctx);
const void *switch_nvk_capture_map(uint64_t command_buffer, uint64_t addr,
                                   uint64_t bytes);
bool switch_nvk_capture_program(uint64_t command_buffer, uint32_t program,
                                const void **header, uint32_t *header_bytes,
                                const void **code, uint32_t *code_bytes,
                                const void **data, uint32_t *data_bytes,
                                uint8_t cbuf_types[16], uint32_t *cbuf_count);
bool switch_nvk_capture_context(uint64_t command_buffer, uint64_t *shader_heap,
                                uint64_t *zero_page, uint64_t *cb0,
                                uint64_t *images, uint32_t *image_max,
                                uint64_t *samplers, uint32_t *sampler_max);

#endif /* NVK_SWITCH_CAPTURE_H */
