// SPDX-License-Identifier: MIT
#include "nvk_switch_ir_internal.h"

static bool
scope(uint32_t value, mesa_scope *out)
{
   switch (value) {
   case SpvScopeDevice: *out = SCOPE_DEVICE; return true;
   case SpvScopeWorkgroup: *out = SCOPE_WORKGROUP; return true;
   case SpvScopeSubgroup: *out = SCOPE_SUBGROUP; return true;
   case SpvScopeInvocation: *out = SCOPE_INVOCATION; return true;
   default: return false;
   }
}

static bool
emit_barrier(struct switch_nvk_ir_builder *s, bool control,
             uint32_t execution_scope, uint32_t memory_scope, uint32_t semantics)
{
   mesa_scope exec = SCOPE_NONE, memory;
   if ((control && (!scope(execution_scope, &exec) || exec < SCOPE_INVOCATION || exec > SCOPE_WORKGROUP)) ||
       !scope(memory_scope, &memory))
      return false;
   const uint32_t ordering = SpvMemorySemanticsAcquireMask | SpvMemorySemanticsReleaseMask |
      SpvMemorySemanticsAcquireReleaseMask;
   const uint32_t visibility = SpvMemorySemanticsMakeAvailableMask | SpvMemorySemanticsMakeVisibleMask;
   const uint32_t storage = SpvMemorySemanticsUniformMemoryMask | SpvMemorySemanticsWorkgroupMemoryMask |
      SpvMemorySemanticsImageMemoryMask;
   if (semantics & ~(ordering | storage | visibility) || util_bitcount(semantics & ordering) > 1)
      return false;
   nir_memory_semantics order = 0;
   if (semantics & (SpvMemorySemanticsAcquireMask | SpvMemorySemanticsAcquireReleaseMask))
      order |= NIR_MEMORY_ACQUIRE;
   if (semantics & (SpvMemorySemanticsReleaseMask | SpvMemorySemanticsAcquireReleaseMask))
      order |= NIR_MEMORY_RELEASE;
   if (semantics & SpvMemorySemanticsMakeAvailableMask) order |= NIR_MEMORY_MAKE_AVAILABLE;
   if (semantics & SpvMemorySemanticsMakeVisibleMask) order |= NIR_MEMORY_MAKE_VISIBLE;
   nir_variable_mode modes = 0;
   if (semantics & SpvMemorySemanticsUniformMemoryMask) modes |= nir_var_mem_ssbo | nir_var_mem_global;
   if (semantics & SpvMemorySemanticsWorkgroupMemoryMask) modes |= nir_var_mem_shared;
   if (semantics & SpvMemorySemanticsImageMemoryMask) modes |= nir_var_image;
   const nir_memory_semantics vis_avail = NIR_MEMORY_MAKE_AVAILABLE | NIR_MEMORY_MAKE_VISIBLE;
   if (modes == nir_var_mem_shared)
      order &= ~vis_avail;
   if ((order & vis_avail) == order)
      modes &= ~nir_var_mem_shared;
   if (!control && (!order || !modes))
      return true;
   if (control && (!order || !modes))
      memory = SCOPE_NONE;
   if (control && memory <= SCOPE_INVOCATION && exec <= SCOPE_INVOCATION)
      return true;
   nir_barrier(&s->b, .execution_scope = exec, .memory_scope = memory,
               .memory_semantics = order, .memory_modes = modes);
   return true;
}

bool
switch_nvk_ir_barrier(struct switch_nvk_ir_builder *s,
                      const struct switch_nvk_ir_record *r)
{
   bool control = r->opcode == SpvOpControlBarrier;
   if (r->count != (control ? 3 : 2) || r->result != UINT32_MAX || r->flags || r->auxiliary)
      return false;
   const uint32_t *a = switch_nvk_ir_args(s, r);
   return emit_barrier(s, control, control ? a[0] : 0, a[control], a[control + 1]);
}

bool
switch_nvk_ir_access_barrier(struct switch_nvk_ir_builder *s,
                             nir_deref_instr *deref, uint32_t memory_scope, bool load)
{
   uint32_t semantics;
   if (nir_deref_mode_is_one_of(deref, nir_var_mem_ssbo | nir_var_mem_global))
      semantics = SpvMemorySemanticsUniformMemoryMask;
   else if (nir_deref_mode_is(deref, nir_var_mem_shared))
      semantics = SpvMemorySemanticsWorkgroupMemoryMask;
   else if (nir_deref_mode_is(deref, nir_var_image))
      semantics = SpvMemorySemanticsImageMemoryMask;
   else
      return false;
   semantics |= load ? SpvMemorySemanticsMakeVisibleMask : SpvMemorySemanticsMakeAvailableMask;
   return emit_barrier(s, false, 0, memory_scope, semantics);
}
