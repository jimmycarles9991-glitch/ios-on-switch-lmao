/*
 * Copyright (c) 2007-2022 Apple Inc. All rights reserved.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 *
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. The rights granted to you under the License
 * may not be used to create, or enable the creation or redistribution of,
 * unlawful or unlicensed copies of an Apple operating system, or to
 * circumvent, violate, or enable the circumvention or violation of, any
 * terms of an Apple operating system software license agreement.
 *
 * Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this file.
 *
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 */
#include "assym.s"
#include <arm64/asm.h>
#include <arm64/proc_reg.h>
#include <arm64/machine_machdep.h>
#include <arm64/proc_reg.h>
#include <pexpert/arm64/board_config.h>
#include <mach_assert.h>
#include <machine/asm.h>
#if defined(APPLE_ARM64_ARCH_FAMILY)
#include <arm64/tunables/tunables.s>
#endif
#include <arm64/exception_asm.h>
#if defined(XNU_NATIVE_ARCH_TIMER) && XNU_NATIVE_ARCH_TIMER
#include <arm64/switch_smp.h>
#endif
#if defined(SWITCH_XNU_BOARD) && SWITCH_XNU_BOARD
#include <switch/boot_layout.h>
#endif

#if __ARM_KERNEL_PROTECT__
#include <arm/pmap.h>
#endif /* __ARM_KERNEL_PROTECT__ */



.macro MSR_VBAR_EL1_X0
#if defined(KERNEL_INTEGRITY_KTRR)
	mov	x1, lr
	bl		EXT(pinst_set_vbar)
	mov	lr, x1
#else
	msr		VBAR_EL1, x0
#endif
.endmacro

.macro MSR_TCR_EL1_X1
#if defined(KERNEL_INTEGRITY_KTRR)
	mov		x0, x1
	mov		x1, lr
	bl		EXT(pinst_set_tcr)
	mov		lr, x1
#else
	msr		TCR_EL1, x1
#endif
.endmacro

.macro MSR_TTBR1_EL1_X0
#if defined(KERNEL_INTEGRITY_KTRR)
	mov		x1, lr
	bl		EXT(pinst_set_ttbr1)
	mov		lr, x1
#else
	msr		TTBR1_EL1, x0
#endif
.endmacro

.macro MSR_SCTLR_EL1_X0
#if defined(KERNEL_INTEGRITY_KTRR)
	mov		x1, lr

	// This may abort, do so on SP1
	bl		EXT(pinst_spsel_1)

	bl		EXT(pinst_set_sctlr)
	msr		SPSel, #0									// Back to SP0
	mov		lr, x1
#else
	msr		SCTLR_EL1, x0
#endif /* defined(KERNEL_INTEGRITY_KTRR) */
.endmacro


/* Diagnostic-only, fixed OLED experiment. The flag is enabled only after the
 * exact incoming Video and memory-window contract has been checked with MMU off.
 * x16-x18 and condition flags are scratch at every call site. No stack or calls.
 * Every stage is recorded (switch_early_record[1], shown on an early exception or panic screen); only a failure
 * mark (any colour but the default green) is drawn, a logical 16x16 block at y=280..295, x=16+40*(stage-1)..+15,
 * so a normal boot leaves the screen to the console.
 */
#if defined(SWITCH_XNU_BOARD) && SWITCH_XNU_BOARD
.macro SWITCH_EARLY_MARK stage, color=0xff70ff70
	cbz x28, 99f
	cbnz x21, 99f
	adrp x16, EXT(switch_early_record)@page
	add x16, x16, EXT(switch_early_record)@pageoff
	mov x17, #\stage
	str x17, [x16, #8]
.if \color != 0xff70ff70
	MOV64 x16, (0xf5a00000 + (1279 - (16 + 40 * (\stage - 1))) * 2880 + 280 * 4)
	MOV64 x17, \color
	mov x18, #16
98:
	str w17, [x16, #0]
	str w17, [x16, #4]
	str w17, [x16, #8]
	str w17, [x16, #12]
	str w17, [x16, #16]
	str w17, [x16, #20]
	str w17, [x16, #24]
	str w17, [x16, #28]
	str w17, [x16, #32]
	str w17, [x16, #36]
	str w17, [x16, #40]
	str w17, [x16, #44]
	str w17, [x16, #48]
	str w17, [x16, #52]
	str w17, [x16, #56]
	str w17, [x16, #60]
	sub x16, x16, #2880
	subs x18, x18, #1
	b.ne 98b
	dsb sy
.endif
99:
.endmacro
#else
.macro SWITCH_EARLY_MARK stage, color=0xff70ff70
.endmacro
#endif

/*
 * Checks the reset handler for global and CPU-specific reset-assist functions,
 * then jumps to the reset handler with boot args and cpu data. This is copied
 * to the first physical page during CPU bootstrap (see cpu.c).
 *
 * Variables:
 *	x19 - Reset handler data pointer
 *	x20 - Boot args pointer
 *	x21 - CPU data pointer
 */
	.text
	.align 12
	.globl EXT(LowResetVectorBase)
LEXT(LowResetVectorBase)
	/*
	 * On reset, both RVBAR_EL1 and VBAR_EL1 point here.  SPSel.SP is 1,
	 * so on reset the CPU will jump to offset 0x0 and on exceptions
	 * the CPU will jump to offset 0x200, 0x280, 0x300, or 0x380.
	 * In order for both the reset vector and exception vectors to
	 * coexist in the same space, the reset code is moved to the end
	 * of the exception vector area.
	 */
	b		EXT(reset_vector)

	/* EL1 SP1: These vectors trap errors during early startup on non-boot CPUs. */
	.align	9
	b		.
	.align	7
	b		.
	.align	7
	b		.
	.align	7
	b		.

	.align	7
	.globl EXT(reset_vector)
LEXT(reset_vector)
	// Preserve x0 for start_first_cpu, if called
	// Unlock the core for debugging
	msr		OSLAR_EL1, xzr
	msr		DAIFSet, #(DAIFSC_ALL)				// Disable all interrupts

#if !(defined(KERNEL_INTEGRITY_KTRR) || defined(KERNEL_INTEGRITY_CTRR) || defined(KERNEL_INTEGRITY_PV_CTRR))
	// Set low reset vector before attempting any loads
#if defined(XNU_NATIVE_ARCH_TIMER) && XNU_NATIVE_ARCH_TIMER
	/* Only our validated PSCI wrapper supplies a physical record in x27.
	 * Keep its EL1 vectors through the cache-off reset code. */
	cbz x27, Lswitch_reset_normal_vectors
	adrp x16, EXT(switch_secondary_records)@page
	add x16, x16, EXT(switch_secondary_records)@pageoff
	sub x17, x27, x16
	cmp x17, #(SWITCH_SMP_CORE_COUNT * SWITCH_SMP_RECORD_BYTES)
	b.hs Lswitch_reset_normal_vectors
	tst x17, #(SWITCH_SMP_RECORD_BYTES - 1)
	b.ne Lswitch_reset_normal_vectors
	ldr x17, [x27, #SWITCH_SMP_STAGE_OFF]
	cmp x17, #SWITCH_SMP_STAGE_RESET
	b.ne Lswitch_reset_normal_vectors
	adrp x0, EXT(switch_secondary_vectors)@page
	add x0, x0, EXT(switch_secondary_vectors)@pageoff
	b Lswitch_reset_vectors_ready
Lswitch_reset_normal_vectors:
	mov x27, xzr
#endif
	adrp    x0, EXT(LowExceptionVectorBase)@page
	add     x0, x0, EXT(LowExceptionVectorBase)@pageoff
#if defined(XNU_NATIVE_ARCH_TIMER) && XNU_NATIVE_ARCH_TIMER
Lswitch_reset_vectors_ready:
#endif
	msr     VBAR_EL1, x0
#if defined(XNU_NATIVE_ARCH_TIMER) && XNU_NATIVE_ARCH_TIMER
	isb
#endif
#endif



	// Process reset handlers
	adrp	x19, EXT(ResetHandlerData)@page			// Get address of the reset handler data
	add		x19, x19, EXT(ResetHandlerData)@pageoff
	mrs		x15, MPIDR_EL1						// Load MPIDR to get CPU number
#if HAS_CLUSTER || defined(BCM2837)
	and		x0, x15, #0xFFFF					// CPU number in Affinity0, cluster ID in Affinity1 (SM8550 numbers cores in Affinity1)
#else
	and		x0, x15, #0xFF						// CPU number is in MPIDR Affinity Level 0
#endif
	ldr		x1, [x19, CPU_DATA_ENTRIES]			// Load start of data entries
	add		x3, x1, MAX_CPUS * 16				// end addr of data entries = start + (16 * MAX_CPUS)
Lcheck_cpu_data_entry:
	ldr		x21, [x1, CPU_DATA_PADDR]			// Load physical CPU data address
	cbz		x21, Lnext_cpu_data_entry
	ldr		w2, [x21, CPU_PHYS_ID]				// Load ccc cpu phys id
	cmp		x0, x2						// Compare cpu data phys cpu and MPIDR_EL1 phys cpu
	b.eq	Lfound_cpu_data_entry				// Branch if match
Lnext_cpu_data_entry:
	add		x1, x1, #16					// Increment to the next cpu data entry
	cmp		x1, x3
	b.eq	Lskip_cpu_reset_handler				// Not found
	b		Lcheck_cpu_data_entry	// loop
Lfound_cpu_data_entry:

#ifdef APPLEEVEREST
	/*
	 * On H15, we need to configure PIO-only tunables and to apply
	 * PIO lockdown as early as possible.
	 */
	SET_PIO_ONLY_REGISTERS x21, x2, x3, x4, x5, x6
#endif /* APPLEEVEREST */

	adrp	x20, EXT(const_boot_args)@page
	add		x20, x20, EXT(const_boot_args)@pageoff
	ldr		x0, [x21, CPU_RESET_HANDLER]		// Call CPU reset handler
	cbz		x0, Lskip_cpu_reset_handler

	// Validate that our handler is one of the two expected handlers
	adrp	x2, EXT(resume_idle_cpu)@page
	add		x2, x2, EXT(resume_idle_cpu)@pageoff
	cmp		x0, x2
	beq		1f
	adrp	x2, EXT(start_cpu)@page
	add		x2, x2, EXT(start_cpu)@pageoff
	cmp		x0, x2
	bne		Lskip_cpu_reset_handler
1:

#if HAS_BP_RET
	bl		EXT(set_bp_ret)
#endif

#if __ARM_KERNEL_PROTECT__ && defined(KERNEL_INTEGRITY_KTRR)
	/*
	 * Populate TPIDR_EL1 (in case the CPU takes an exception while
	 * turning on the MMU).
	 */
	ldr		x13, [x21, CPU_ACTIVE_THREAD]
	msr		TPIDR_EL1, x13
#endif /* __ARM_KERNEL_PROTECT__ */

	blr		x0
Lskip_cpu_reset_handler:
	b		.									// Hang if the handler is NULL or returns

	.align 3
	.global EXT(LowResetVectorEnd)
LEXT(LowResetVectorEnd)
	.global	EXT(SleepToken)
#if WITH_CLASSIC_S2R
LEXT(SleepToken)
	.space	(stSize_NUM),0
#endif

	.section __DATA_CONST,__const
	.align	3
	.globl  EXT(ResetHandlerData)
LEXT(ResetHandlerData)
	.space  (rhdSize_NUM),0		// (filled with 0s)
	.text


/*
 * __start trampoline is located at a position relative to LowResetVectorBase
 * so that iBoot can compute the reset vector position to set IORVBAR using
 * only the kernel entry point.  Reset vector = (__start & ~0xfff)
 */
	.align	3
	.globl EXT(_start)
LEXT(_start)
	ARM64_PROLOG
	b	EXT(start_first_cpu)


/*
 * Provides an early-boot exception vector so that the processor will spin
 * and preserve exception information (e.g., ELR_EL1) when early CPU bootstrap
 * code triggers an exception. This is copied to the second physical page
 * during CPU bootstrap (see cpu.c).
 */
	.align 12, 0
	.global	EXT(LowExceptionVectorBase)
LEXT(LowExceptionVectorBase)
	/* EL1 SP 0 */
#if defined(SWITCH_XNU_BOARD) && SWITCH_XNU_BOARD
	mov x0, #0
	b Lswitch_early_exception
#else
	b		.
#endif
	.align	7
#if defined(SWITCH_XNU_BOARD) && SWITCH_XNU_BOARD
	mov x0, #1
	b Lswitch_early_exception
#else
	b		.
#endif
	.align	7
#if defined(SWITCH_XNU_BOARD) && SWITCH_XNU_BOARD
	mov x0, #2
	b Lswitch_early_exception
#else
	b		.
#endif
	.align	7
#if defined(SWITCH_XNU_BOARD) && SWITCH_XNU_BOARD
	mov x0, #3
	b Lswitch_early_exception
#else
	b		.
#endif
	/* EL1 SP1 */
	.align	7
#if defined(SWITCH_XNU_BOARD) && SWITCH_XNU_BOARD
	mov x0, #4
	b Lswitch_early_exception
#else
	b		.
#endif
	.align	7
#if defined(SWITCH_XNU_BOARD) && SWITCH_XNU_BOARD
	mov x0, #5
	b Lswitch_early_exception
#else
	b		.
#endif
	.align	7
#if defined(SWITCH_XNU_BOARD) && SWITCH_XNU_BOARD
	mov x0, #6
	b Lswitch_early_exception
#else
	b		.
#endif
	.align	7
#if defined(SWITCH_XNU_BOARD) && SWITCH_XNU_BOARD
	mov x0, #7
	b Lswitch_early_exception
#else
	b		.
#endif
	/* EL0 64 */
	.align	7
#if defined(SWITCH_XNU_BOARD) && SWITCH_XNU_BOARD
	mov x0, #8
	b Lswitch_early_exception
#else
	b		.
#endif
	.align	7
#if defined(SWITCH_XNU_BOARD) && SWITCH_XNU_BOARD
	mov x0, #9
	b Lswitch_early_exception
#else
	b		.
#endif
	.align	7
#if defined(SWITCH_XNU_BOARD) && SWITCH_XNU_BOARD
	mov x0, #10
	b Lswitch_early_exception
#else
	b		.
#endif
	.align	7
#if defined(SWITCH_XNU_BOARD) && SWITCH_XNU_BOARD
	mov x0, #11
	b Lswitch_early_exception
#else
	b		.
#endif
	/* EL0 32 */
	.align	7
#if defined(SWITCH_XNU_BOARD) && SWITCH_XNU_BOARD
	mov x0, #12
	b Lswitch_early_exception
#else
	b		.
#endif
	.align	7
#if defined(SWITCH_XNU_BOARD) && SWITCH_XNU_BOARD
	mov x0, #13
	b Lswitch_early_exception
#else
	b		.
#endif
	.align	7
#if defined(SWITCH_XNU_BOARD) && SWITCH_XNU_BOARD
	mov x0, #14
	b Lswitch_early_exception
#else
	b		.
#endif
	.align	7
#if defined(SWITCH_XNU_BOARD) && SWITCH_XNU_BOARD
	mov x0, #15
	b Lswitch_early_exception
#else
	b		.
#endif
	.align 12, 0

#if defined(KERNEL_INTEGRITY_KTRR) || defined(KERNEL_INTEGRITY_CTRR) || defined(KERNEL_INTEGRITY_PV_CTRR)
/*
 * Provide a global symbol so that we can narrow the V=P mapping to cover
 * this page during arm_vm_init.
 */
.align ARM_PGSHIFT
.globl EXT(bootstrap_instructions)
LEXT(bootstrap_instructions)

#endif /* defined(KERNEL_INTEGRITY_KTRR) || defined(KERNEL_INTEGRITY_CTRR) || defined(KERNEL_INTEGRITY_PV_CTRR) */
#if defined(XNU_NATIVE_ARCH_TIMER) && XNU_NATIVE_ARCH_TIMER
/*
 * First-start PSCI entry for the four homogeneous Switch Cortex-A57 cores.
 * Firmware supplies x0=context PA, no stack, M/C/I off, EL1h or EL2h.
 * This never changes coherency mode or performs a cache set/way operation.
 */
.macro SWITCH_SMP_CONTEXT context, entry, base, end, scratch, failed
	lsr \scratch, \entry, #32
	cbnz \scratch, \failed
	lsr \scratch, \base, #32
	cbnz \scratch, \failed
	lsr \scratch, \end, #32
	cbnz \scratch, \failed
	cmp \context, \base
	b.lo \failed
	cmp \context, \end
	b.hs \failed
	sub \scratch, \context, \base
	tst \scratch, #(SWITCH_SMP_RECORD_BYTES - 1)
	b.ne \failed
.endmacro
.macro SWITCH_SMP_CACHE_OFF sctlr, scratch, failed
	mov \scratch, #0x1005
	movk \scratch, #0x200, lsl #16
	tst \sctlr, \scratch
	b.ne \failed
.endmacro
.macro SWITCH_SMP_COUNTER_DOMAIN before, virtual, after, failed
	cmp \virtual, \before
	b.lo \failed
	cmp \virtual, \after
	b.hi \failed
.endmacro
	.align 11
	.globl EXT(switch_secondary_vectors)
LEXT(switch_secondary_vectors)
.macro SWITCH_SECONDARY_VECTOR number
	mov x0, #\number
	b Lswitch_secondary_exception
	.space 120
.endmacro
	SWITCH_SECONDARY_VECTOR 0
	SWITCH_SECONDARY_VECTOR 1
	SWITCH_SECONDARY_VECTOR 2
	SWITCH_SECONDARY_VECTOR 3
	SWITCH_SECONDARY_VECTOR 4
	SWITCH_SECONDARY_VECTOR 5
	SWITCH_SECONDARY_VECTOR 6
	SWITCH_SECONDARY_VECTOR 7
	SWITCH_SECONDARY_VECTOR 8
	SWITCH_SECONDARY_VECTOR 9
	SWITCH_SECONDARY_VECTOR 10
	SWITCH_SECONDARY_VECTOR 11
	SWITCH_SECONDARY_VECTOR 12
	SWITCH_SECONDARY_VECTOR 13
	SWITCH_SECONDARY_VECTOR 14
	SWITCH_SECONDARY_VECTOR 15

	.align 2
	.globl EXT(switch_secondary_entry)
LEXT(switch_secondary_entry)
	msr DAIFSet, #15
	/* A numeric PA bound cannot justify a store through inherited unknown
	 * translation tables. Prove this EL's M/C/I and endianness first. */
	mrs x20, CurrentEL
	cmp x20, #4
	b.eq Lswitch_secondary_check_el1_off
	cmp x20, #8
	b.ne Lswitch_secondary_halt
	mrs x21, SCTLR_EL2
	b Lswitch_secondary_check_off
Lswitch_secondary_check_el1_off:
	mrs x21, SCTLR_EL1
Lswitch_secondary_check_off:
	SWITCH_SMP_CACHE_OFF x21, x2, Lswitch_secondary_halt
	/* Validate an exact member of our owned array BEFORE any record write.
	 * PC-relative references denote PA here and KVA after normal entry. */
	adrp x19, EXT(switch_secondary_records)@page
	add x19, x19, EXT(switch_secondary_records)@pageoff
	adrp x1, EXT(switch_secondary_records_end)@page
	add x1, x1, EXT(switch_secondary_records_end)@pageoff
	adr x22, EXT(switch_secondary_entry)
	SWITCH_SMP_CONTEXT x0, x22, x19, x1, x2, Lswitch_secondary_halt
	mov x19, x0
	mov x27, x0
	mov x28, xzr
	/* Establish a real current-EL vector BEFORE implementation-defined MRS. */
	str x20, [x19, #SWITCH_SMP_ENTRY_EL_OFF]
	str x21, [x19, #SWITCH_SMP_SCTLR_OFF]
	adrp x1, EXT(switch_secondary_vectors)@page
	add x1, x1, EXT(switch_secondary_vectors)@pageoff
	lsr x2, x1, #32
	cbnz x2, Lswitch_secondary_bad_control
	cmp x20, #4
	b.eq Lswitch_secondary_set_el1_vectors
	cmp x20, #8
	b.ne Lswitch_secondary_bad_el
	msr VBAR_EL2, x1
	isb
	b Lswitch_secondary_record_entry
Lswitch_secondary_set_el1_vectors:
	msr VBAR_EL1, x1
	isb
Lswitch_secondary_record_entry:
	mrs x1, MPIDR_EL1
	MOV64 x2, 0xff00ffffff
	and x1, x1, x2
	str x1, [x19, #SWITCH_SMP_SEEN_MPIDR_OFF]
	str x20, [x19, #SWITCH_SMP_ENTRY_EL_OFF]
	mov x2, #SWITCH_SMP_STAGE_ENTRY
	str x2, [x19, #SWITCH_SMP_STAGE_OFF]
	dsb sy
	ldr x2, [x19, #SWITCH_SMP_MAGIC_OFF]
	MOV64 x3, SWITCH_SMP_MAGIC
	cmp x2, x3
	b.ne Lswitch_secondary_bad_control
	ldr x2, [x19, #SWITCH_SMP_RESERVED0_OFF]
	ldr x3, [x19, #SWITCH_SMP_RESERVED1_OFF]
	orr x2, x2, x3
	cbnz x2, Lswitch_secondary_bad_control
	cmp x1, #SWITCH_SMP_CORE_COUNT
	b.hs Lswitch_secondary_bad_mpidr
	ldr x2, [x19, #SWITCH_SMP_MPIDR_OFF]
	cmp x1, x2
	b.ne Lswitch_secondary_bad_mpidr
	ldr x2, [x19, #SWITCH_SMP_LOGICAL_OFF]
	cmp x1, x2
	b.ne Lswitch_secondary_bad_mpidr
	adrp x2, EXT(switch_secondary_records)@page
	add x2, x2, EXT(switch_secondary_records)@pageoff
	add x2, x2, x1, lsl #7
	cmp x2, x19
	b.ne Lswitch_secondary_bad_mpidr
	ldr x1, [x19, #SWITCH_SMP_CNTVOFF_OFF]
	cbnz x1, Lswitch_secondary_bad_cntvoff
	ldr x1, [x19, #SWITCH_SMP_RESET_OFF]
	adrp x2, EXT(LowResetVectorBase)@page
	add x2, x2, EXT(LowResetVectorBase)@pageoff
	cmp x1, x2
	b.ne Lswitch_secondary_bad_reset
	lsr x1, x2, #32
	cbnz x1, Lswitch_secondary_bad_reset
	/* Only A57: preserve firmware SMPEN, never set/clear it on a live L2. */
	mrs x1, MIDR_EL1
	MOV64 x2, 0xff0ffff0
	and x1, x1, x2
	MOV64 x2, 0x410fd070
	cmp x1, x2
	b.ne Lswitch_secondary_bad_midr
	mrs x1, S3_1_C15_C2_1
	tbz x1, #6, Lswitch_secondary_bad_smpen
	cmp x20, #8
	b.ne Lswitch_secondary_el1
	mrs x1, SCTLR_EL2
	str x1, [x19, #SWITCH_SMP_SCTLR_OFF]
	SWITCH_SMP_CACHE_OFF x1, x2, Lswitch_secondary_bad_sctlr
	/* Match the measured primary A57 EL2->EL1 handoff. No GICv3 SRE access. */
	MOV64 x1, 0x80000000
	msr HCR_EL2, x1
	msr VTTBR_EL2, xzr
	MOV64 x1, 0x33ff
	msr CPTR_EL2, x1
	msr MDCR_EL2, xzr
	msr HSTR_EL2, xzr
	mrs x1, MIDR_EL1
	msr VPIDR_EL2, x1
	mrs x1, MPIDR_EL1
	msr VMPIDR_EL2, x1
	mov x1, #3
	msr CNTHCTL_EL2, x1
	msr CNTVOFF_EL2, xzr
	mov x1, #2
	msr CNTHP_CTL_EL2, x1
	MOV64 x1, 0x30d00800
	msr SCTLR_EL1, x1
	msr TTBR0_EL1, xzr
	msr TTBR1_EL1, xzr
	msr TCR_EL1, xzr
	msr MAIR_EL1, xzr
	msr CONTEXTIDR_EL1, xzr
	msr CPACR_EL1, xzr
	msr CNTKCTL_EL1, xzr
	adrp x1, EXT(switch_secondary_vectors)@page
	add x1, x1, EXT(switch_secondary_vectors)@pageoff
	msr VBAR_EL1, x1
	adrp x1, Lswitch_secondary_el1@page
	add x1, x1, Lswitch_secondary_el1@pageoff
	msr ELR_EL2, x1
	mov x1, #0x3c5
	msr SPSR_EL2, x1
	dsb sy
	isb
	eret
Lswitch_secondary_el1:
	msr DAIFSet, #15
	msr SPSel, #1
	mrs x1, SCTLR_EL1
	str x1, [x19, #SWITCH_SMP_SCTLR_OFF]
	SWITCH_SMP_CACHE_OFF x1, x2, Lswitch_secondary_bad_sctlr
	/* Direct EL1 entry is permitted only with the same accessible zero-offset
	 * counter domain. EL1 cannot read CNTVOFF_EL2: bracket virtual by physical
	 * reads rather than pretending an EL2 register was observed here. */
	mrs x1, CNTFRQ_EL0
	ldr x2, [x19, #SWITCH_SMP_CNTFRQ_OFF]
	cmp x1, x2
	b.ne Lswitch_secondary_bad_frequency
	MOV64 x2, 19200000
	cmp x1, x2
	b.ne Lswitch_secondary_bad_frequency
	isb
	mrs x1, CNTPCT_EL0
	mrs x2, CNTVCT_EL0
	mrs x3, CNTPCT_EL0
	SWITCH_SMP_COUNTER_DOMAIN x1, x2, x3, Lswitch_secondary_bad_cntvoff
	mov x1, #2
	msr CNTP_CTL_EL0, x1
	msr CNTV_CTL_EL0, x1
	MOV64 x1, 0x30d00800
	msr SCTLR_EL1, x1
	msr CPACR_EL1, xzr
	msr CNTKCTL_EL1, xzr
	isb
	mov x1, #SWITCH_SMP_STAGE_EL1
	str x1, [x19, #SWITCH_SMP_STAGE_OFF]
	dsb sy
	mov x1, #SWITCH_SMP_STAGE_RESET
	str x1, [x19, #SWITCH_SMP_STAGE_OFF]
	dsb sy
	b EXT(LowResetVectorBase)

Lswitch_secondary_bad_control:
	mov x1, #SWITCH_SMP_ERROR_CONTROL
	b Lswitch_secondary_failed
Lswitch_secondary_bad_el:
	mov x1, #SWITCH_SMP_ERROR_EL
	b Lswitch_secondary_failed
Lswitch_secondary_bad_mpidr:
	mov x1, #SWITCH_SMP_ERROR_MPIDR
	b Lswitch_secondary_failed
Lswitch_secondary_bad_sctlr:
	mov x1, #SWITCH_SMP_ERROR_SCTLR
	b Lswitch_secondary_failed
Lswitch_secondary_bad_midr:
	mov x1, #SWITCH_SMP_ERROR_MIDR
	b Lswitch_secondary_failed
Lswitch_secondary_bad_smpen:
	mov x1, #SWITCH_SMP_ERROR_SMPEN
	b Lswitch_secondary_failed
Lswitch_secondary_bad_frequency:
	mov x1, #SWITCH_SMP_ERROR_FREQUENCY
	b Lswitch_secondary_failed
Lswitch_secondary_bad_cntvoff:
	mov x1, #SWITCH_SMP_ERROR_CNTVOFF
	b Lswitch_secondary_failed
Lswitch_secondary_bad_reset:
	mov x1, #SWITCH_SMP_ERROR_RESET
Lswitch_secondary_failed:
	str x1, [x19, #SWITCH_SMP_ERROR_OFF]
	mov x1, #SWITCH_SMP_STAGE_FAILED
	str x1, [x19, #SWITCH_SMP_STAGE_OFF]
	add x1, x19, #SWITCH_SMP_STATUS_OFFSET
	dc cvac, x1
	dsb sy
Lswitch_secondary_halt:
	wfe
	b Lswitch_secondary_halt

/* Works at physical EL1/EL2 and at the KVA vector after MMU enable.
 * Derive the record anew: an interrupted C function may use x27 as a local. */
Lswitch_secondary_exception:
	msr DAIFSet, #15
	mov x18, x0
	mrs x1, MPIDR_EL1
	MOV64 x2, 0xff00ffffff
	and x1, x1, x2
	cmp x1, #SWITCH_SMP_CORE_COUNT
	b.hs Lswitch_secondary_halt
	adrp x19, EXT(switch_secondary_records)@page
	add x19, x19, EXT(switch_secondary_records)@pageoff
	add x19, x19, x1, lsl #7
	ldr x2, [x19, #SWITCH_SMP_MPIDR_OFF]
	cmp x1, x2
	b.ne Lswitch_secondary_halt
	ldr x2, [x19, #SWITCH_SMP_ERROR_OFF]
	cbnz x2, Lswitch_secondary_halt
	/* Error also prevents recursive faults; FAILED is published last, after
	 * ESR/ELR/FAR/SCTLR. Only FAILED makes the complete fault payload valid. */
	add x18, x18, #SWITCH_SMP_ERROR_EXCEPTION
	str x18, [x19, #SWITCH_SMP_ERROR_OFF]
	str x1, [x19, #SWITCH_SMP_SEEN_MPIDR_OFF]
	mrs x1, CurrentEL
	cmp x1, #8
	b.eq Lswitch_secondary_exception_el2
	cmp x1, #4
	b.ne Lswitch_secondary_halt
	mrs x1, ESR_EL1
	mrs x2, ELR_EL1
	mrs x3, FAR_EL1
	mrs x4, SCTLR_EL1
	b Lswitch_secondary_exception_save
Lswitch_secondary_exception_el2:
	mrs x1, ESR_EL2
	mrs x2, ELR_EL2
	mrs x3, FAR_EL2
	mrs x4, SCTLR_EL2
Lswitch_secondary_exception_save:
	str x1, [x19, #SWITCH_SMP_ESR_OFF]
	str x2, [x19, #SWITCH_SMP_ELR_OFF]
	str x3, [x19, #SWITCH_SMP_FAR_OFF]
	str x4, [x19, #SWITCH_SMP_SCTLR_OFF]
	mov x1, #SWITCH_SMP_STAGE_FAILED
	str x1, [x19, #SWITCH_SMP_STAGE_OFF]
	add x1, x19, #SWITCH_SMP_STATUS_OFFSET
	dc cvac, x1
	dsb sy
	b Lswitch_secondary_halt

	.align 2
	.globl EXT(switch_secondary_vectors_leave)
LEXT(switch_secondary_vectors_leave)
	mrs x0, MPIDR_EL1
	MOV64 x1, 0xff00ffffff
	and x0, x0, x1
	cmp x0, #SWITCH_SMP_CORE_COUNT
	b.hs 9f
	adrp x1, EXT(switch_secondary_records)@page
	add x1, x1, EXT(switch_secondary_records)@pageoff
	add x1, x1, x0, lsl #7
	ldr x2, [x1, #SWITCH_SMP_STAGE_OFF]
	cmp x2, #SWITCH_SMP_STAGE_MMU
	b.ne 9f
	adrp x0, EXT(ExceptionVectorsBase)@page
	add x0, x0, EXT(ExceptionVectorsBase)@pageoff
	msr VBAR_EL1, x0
	isb
	mov x2, #SWITCH_SMP_STAGE_C
	str x2, [x1, #SWITCH_SMP_STAGE_OFF]
	add x1, x1, #SWITCH_SMP_STATUS_OFFSET
	dc cvac, x1
	dsb sy
9:	ret

	.section __DATA,__data
	.align 7
	.globl EXT(switch_secondary_records)
LEXT(switch_secondary_records)
	.space (SWITCH_SMP_CORE_COUNT * SWITCH_SMP_RECORD_BYTES), 0
	.globl EXT(switch_secondary_records_end)
LEXT(switch_secondary_records_end)
	.text
#endif /* XNU_NATIVE_ARCH_TIMER */
	.align 2
	.globl EXT(resume_idle_cpu)
LEXT(resume_idle_cpu)
	adrp	lr, EXT(arm_init_idle_cpu)@page
	add		lr, lr, EXT(arm_init_idle_cpu)@pageoff
	b		start_cpu

	.align 2
	.globl EXT(start_cpu)
LEXT(start_cpu)
	adrp	lr, EXT(arm_init_cpu)@page
	add		lr, lr, EXT(arm_init_cpu)@pageoff
	b		start_cpu


#if defined(SWITCH_XNU_BOARD) && SWITCH_XNU_BOARD
/* PC-relative references work at the physical entry and the linked KVA. A
 * recursive fault while drawing halts without touching the stack again. */
Lswitch_early_exception:
	msr DAIFSet, #15
	adrp x9, EXT(switch_early_record)@page
	add x9, x9, EXT(switch_early_record)@pageoff
	ldr x10, [x9]
	cbz x10, Lswitch_early_halt
	ldr x10, [x9, #16]
	cbnz x10, Lswitch_early_halt
	mov x10, #1
	str x10, [x9, #16]
	str x0, [x9, #24]
	mrs x1, ESR_EL1
	str x1, [x9, #32]
	mrs x2, FAR_EL1
	str x2, [x9, #40]
	mrs x3, ELR_EL1
	str x3, [x9, #48]
	mrs x4, SCTLR_EL1
	str x4, [x9, #56]
	msr SPSel, #1
	adrp x10, EXT(switch_early_stack)@page
	add x10, x10, EXT(switch_early_stack)@pageoff
	add x10, x10, #4096
	mov sp, x10
	b EXT(switch_diag_early_fault)
Lswitch_early_halt:
	wfe
	b Lswitch_early_halt
#endif


	.align 2
start_cpu:
#if defined(KERNEL_INTEGRITY_KTRR) || defined(KERNEL_INTEGRITY_CTRR) || defined(KERNEL_INTEGRITY_PV_CTRR)
	// This is done right away in reset vector for pre-KTRR devices
	// Set low reset vector now that we are in the KTRR-free zone
	adrp	x0, EXT(LowExceptionVectorBase)@page
	add		x0, x0, EXT(LowExceptionVectorBase)@pageoff
	MSR_VBAR_EL1_X0
#endif /* defined(KERNEL_INTEGRITY_KTRR) || defined(KERNEL_INTEGRITY_CTRR) || defined(KERNEL_INTEGRITY_PV_CTRR) */

	// x20 set to BootArgs phys address
	// x21 set to cpu data phys address

	// Get the kernel memory parameters from the boot args
	ldr		x22, [x20, BA_VIRT_BASE]			// Get the kernel virt base
	ldr		x23, [x20, BA_PHYS_BASE]			// Get the kernel phys base
	ldr		x24, [x20, BA_MEM_SIZE]				// Get the physical memory size
	adrp	x25, EXT(bootstrap_pagetables)@page	// Get the start of the page tables
	ldr		x26, [x20, BA_BOOT_FLAGS]			// Get the kernel boot flags


	// Set TPIDR_EL0 with cached CPU info
	ldr		x0, [x21, CPU_TPIDR_EL0]
	msr		TPIDR_EL0, x0

	// Set TPIDRRO_EL0 to 0
	msr		TPIDRRO_EL0, xzr


	// Set the exception stack pointer
	ldr		x0, [x21, CPU_EXCEPSTACK_TOP]


	// Set SP_EL1 to exception stack
#if defined(KERNEL_INTEGRITY_KTRR) || defined(KERNEL_INTEGRITY_CTRR) || defined(KERNEL_INTEGRITY_PV_CTRR)
	mov		x1, lr
	bl		EXT(pinst_spsel_1)
	mov		lr, x1
#else
	msr		SPSel, #1
#endif
	mov		sp, x0

	// Set the interrupt stack pointer
	ldr		x0, [x21, CPU_INTSTACK_TOP]
	msr		SPSel, #0
	mov		sp, x0

	// Convert lr to KVA
	add		lr, lr, x22
	sub		lr, lr, x23

	b		common_start

/*
 * create_l1_table_entry
 *
 * Given a virtual address, creates a table entry in an L1 translation table
 * to point to an L2 translation table.
 *   arg0 - Virtual address
 *   arg1 - L1 table address
 *   arg2 - L2 table address
 *   arg3 - Scratch register
 *   arg4 - Scratch register
 *   arg5 - Scratch register
 */
.macro create_l1_table_entry
	and		$3,	$0, #(ARM_PTE_T1_REGION_MASK(TCR_EL1_BOOT))
	lsr		$3, $3, #(ARM_TT_L1_SHIFT)			// Get index in L1 table for L2 table
	lsl		$3, $3, #(TTE_SHIFT)				// Convert index into pointer offset
	add		$3, $1, $3							// Get L1 entry pointer
	mov		$4, #(ARM_TTE_BOOT_TABLE)			// Get L1 table entry template
	and		$5, $2, #(ARM_TTE_TABLE_MASK)		// Get address bits of L2 table
	orr		$5, $4, $5 							// Create table entry for L2 table
	str		$5, [$3]							// Write entry to L1 table
.endmacro

/*
 * create_l2_block_entries
 *
 * Given base virtual and physical addresses, creates consecutive block entries
 * in an L2 translation table.
 *   arg0 - Virtual address
 *   arg1 - Physical address
 *   arg2 - L2 table address
 *   arg3 - Number of entries
 *   arg4 - Scratch register
 *   arg5 - Scratch register
 *   arg6 - Scratch register
 *   arg7 - Scratch register
 */
.macro create_l2_block_entries
	and		$4,	$0, #(ARM_TT_L2_INDEX_MASK)
	lsr		$4, $4, #(ARM_TTE_BLOCK_L2_SHIFT)	// Get index in L2 table for block entry
	lsl		$4, $4, #(TTE_SHIFT)				// Convert index into pointer offset
	add		$4, $2, $4							// Get L2 entry pointer
	mov		$5, #(ARM_TTE_BOOT_BLOCK_LOWER)		// Get L2 block entry template
	orr		$5, $5, #(ARM_TTE_BOOT_BLOCK_UPPER)
	and		$6, $1, #(ARM_TTE_BLOCK_L2_MASK)	// Get address bits of block mapping
	orr		$6, $5, $6
	mov		$5, $3
	mov		$7, #(ARM_TT_L2_SIZE)
1:
	str		$6, [$4], #(1 << TTE_SHIFT)			// Write entry to L2 table and advance
	add		$6, $6, $7							// Increment the output address
	subs	$5, $5, #1							// Decrement the number of entries
	b.ne	1b
.endmacro

/*
 *  arg0 - virtual start address
 *  arg1 - physical start address
 *  arg2 - number of entries to map
 *  arg3 - L1 table address
 *  arg4 - free space pointer
 *  arg5 - scratch (entries mapped per loop)
 *  arg6 - scratch
 *  arg7 - scratch
 *  arg8 - scratch
 *  arg9 - scratch
 */
.macro create_bootstrap_mapping
	/* calculate entries left in this page */
	and	$5, $0, #(ARM_TT_L2_INDEX_MASK)
	lsr	$5, $5, #(ARM_TT_L2_SHIFT)
	mov	$6, #(TTE_PGENTRIES)
	sub	$5, $6, $5

	/* allocate an L2 table */
3:	add	$4, $4, PGBYTES
	/* Refuse before a new table can overwrite adjacent kernel globals. */
	add $6, x25, #(BOOTSTRAP_TABLE_BYTES >> 12), lsl #12
	cmp $4, $6
	b.hs Lbootstrap_tables_exhausted

	/* create_l1_table_entry(virt_base, L1 table, L2 table, scratch1, scratch2, scratch3) */
	create_l1_table_entry	$0, $3, $4, $6, $7, $8

	/* determine how many entries to map this loop - the smaller of entries
	 * remaining in page and total entries left */
	cmp	$2, $5
	csel	$5, $2, $5, lt

	/* create_l2_block_entries(virt_base, phys_base, L2 table, num_ents, scratch1, scratch2, scratch3) */
	create_l2_block_entries	$0, $1, $4, $5, $6, $7, $8, $9

	/* subtract entries just mapped and bail out if we're done */
	subs	$2, $2, $5
	beq	2f

	/* entries left to map - advance base pointers */
	add 	$0, $0, $5, lsl #(ARM_TT_L2_SHIFT)
	add 	$1, $1, $5, lsl #(ARM_TT_L2_SHIFT)

	mov	$5, #(TTE_PGENTRIES)  /* subsequent loops map (up to) a whole L2 page */
	b	3b
2:
.endmacro

/*
 * _start_first_cpu
 * Cold boot init routine.  Called from __start
 *   x0 - Boot args
 */
	.align 2
	.globl EXT(start_first_cpu)
LEXT(start_first_cpu)

	mov x20, x0
	mov x21, #0
#if defined(XNU_NATIVE_ARCH_TIMER) && XNU_NATIVE_ARCH_TIMER
	mov x27, xzr
#endif
#if defined(SWITCH_XNU_BOARD) && SWITCH_XNU_BOARD
	mov x28, #0
	// Enable physical early rendering only for the two exact native cold
	// boot geometries. Neither branch changes the stock console/HUD policy.
	mrs x16, SCTLR_EL1
	MOV64 x17, 0x1005
	tst x16, x17
	b.ne Lswitch_early_disabled
	ldr x16, [x20, BA_PHYS_BASE]
	MOV64 x17, SWITCH_KERNEL_PHYS_BASE
	cmp x16, x17
	b.eq Lswitch_early_legacy_ram
	MOV64 x17, SWITCH_BOOT_PHYS_BASE
	cmp x16, x17
	b.ne Lswitch_early_disabled
	ldr x16, [x20, BA_MEM_SIZE]
	MOV64 x17, SWITCH_BOOT_MEM_SIZE
	cmp x16, x17
	b.ne Lswitch_early_disabled
	ldr x16, [x20, BA_VIRT_BASE]
	MOV64 x17, SWITCH_BOOT_VIRT_BASE
	cmp x16, x17
	b.ne Lswitch_early_disabled
	b Lswitch_early_ram_ready
Lswitch_early_legacy_ram:
	ldr x16, [x20, BA_MEM_SIZE]
	mov x17, #0x30000000
	cmp x16, x17
	b.ne Lswitch_early_disabled
	ldr x16, [x20, BA_VIRT_BASE]
	MOV64 x17, SWITCH_KERNEL_VIRT_BASE
	cmp x16, x17
	b.ne Lswitch_early_disabled
Lswitch_early_ram_ready:
	ldr x16, [x20, #0x28]
	MOV64 x17, 0xf5a00000
	cmp x16, x17
	b.ne Lswitch_early_disabled
	ldr x16, [x20, #0x38]
	cmp x16, #2880
	b.ne Lswitch_early_disabled
	ldr x16, [x20, #0x40]
	cmp x16, #720
	b.ne Lswitch_early_disabled
	ldr x16, [x20, #0x48]
	cmp x16, #1280
	b.ne Lswitch_early_disabled
	ldr x16, [x20, #0x30]
	cbz x16, Lswitch_early_text_video
	cmp x16, #1
	b.ne Lswitch_early_disabled
	ldr x16, [x20, #0x50]
	cmp x16, #32
	b.ne Lswitch_early_disabled
	b Lswitch_early_video_ready
Lswitch_early_text_video:
	ldr x16, [x20, #0x50]
	cmp x16, #0x320
	b.ne Lswitch_early_disabled
Lswitch_early_video_ready:
	adrp x16, EXT(switch_early_record)@page
	add x16, x16, EXT(switch_early_record)@pageoff
	mov x28, #1
	str x28, [x16]
	SWITCH_EARLY_MARK 1
Lswitch_early_disabled:
#endif
	// Unlock the core for debugging
	msr		OSLAR_EL1, xzr
	msr		DAIFSet, #(DAIFSC_ALL)				// Disable all interrupts

	// Set low reset vector before attempting any loads
	adrp	x0, EXT(LowExceptionVectorBase)@page
	add		x0, x0, EXT(LowExceptionVectorBase)@pageoff
	MSR_VBAR_EL1_X0


	// Get the kernel memory parameters from the boot args
	ldr		x22, [x20, BA_VIRT_BASE]			// Get the kernel virt base
	ldr		x23, [x20, BA_PHYS_BASE]			// Get the kernel phys base
	ldr		x24, [x20, BA_MEM_SIZE]				// Get the physical memory size
	adrp	x25, EXT(bootstrap_pagetables)@page	// Get the start of the page tables
	ldr		x26, [x20, BA_BOOT_FLAGS]			// Get the kernel boot flags
	SWITCH_EARLY_MARK 2

	// Clear the registers that will be used to store the userspace thread pointer and CPU number.
	// We may not actually be booting from ordinal CPU 0, so this register will be updated
	// in ml_parse_cpu_topology(), which happens later in bootstrap.
	msr		TPIDRRO_EL0, xzr
	msr		TPIDR_EL0, xzr

	// Set up exception stack pointer
	adrp	x0, EXT(excepstack_top)@page		// Load top of exception stack
	add		x0, x0, EXT(excepstack_top)@pageoff
	add		x0, x0, x22							// Convert to KVA
	sub		x0, x0, x23

	// Set SP_EL1 to exception stack
#if defined(KERNEL_INTEGRITY_KTRR) || defined(KERNEL_INTEGRITY_CTRR) || defined(KERNEL_INTEGRITY_PV_CTRR)
	bl		EXT(pinst_spsel_1)
#else
	msr		SPSel, #1
#endif

	mov		sp, x0

	// Set up interrupt stack pointer
	adrp	x0, EXT(intstack_top)@page			// Load top of irq stack
	add		x0, x0, EXT(intstack_top)@pageoff
	add		x0, x0, x22							// Convert to KVA
	sub		x0, x0, x23
	msr		SPSel, #0							// Set SP_EL0 to interrupt stack
	mov		sp, x0

	// Load address to the C init routine into link register
	adrp	lr, EXT(arm_init)@page
	add		lr, lr, EXT(arm_init)@pageoff
	add		lr, lr, x22							// Convert to KVA
	sub		lr, lr, x23

	/*
	 * Set up the bootstrap page tables with a single block entry for the V=P
	 * mapping, a single block entry for the trampolined kernel address (KVA),
	 * and all else invalid. This requires four pages:
	 *	Page 1 - V=P L1 table
	 *	Page 2 - V=P L2 table
	 *	Page 3 - KVA L1 table
	 *	Page 4 - KVA L2 table
	 */

	// Invalidate all entries in the bootstrap page tables
	mov		x0, #(ARM_TTE_EMPTY)				// Load invalid entry template
	mov		x1, x25								// Start at V=P pagetable root
	mov		x2, #(BOOTSTRAP_TABLE_BYTES / ARM_PGBYTES)
	lsl		x2, x2, #(ARM_PGSHIFT - TTE_SHIFT) // Clear the whole reserved pool

Linvalidate_bootstrap:							// do {
	str		x0, [x1], #(1 << TTE_SHIFT)			//   Invalidate and advance
	subs	x2, x2, #1							//   entries--
	b.ne	Linvalidate_bootstrap				// } while (entries != 0)

	/*
	 * In order to reclaim memory on targets where TZ0 (or some other entity)
	 * must be located at the base of memory, iBoot may set the virtual and
	 * physical base addresses to immediately follow whatever lies at the
	 * base of physical memory.
	 *
	 * If the base address belongs to TZ0, it may be dangerous for xnu to map
	 * it (as it may be prefetched, despite being technically inaccessible).
	 * In order to avoid this issue while keeping the mapping code simple, we
	 * may continue to use block mappings, but we will only map the kernelcache
	 * mach header to the end of memory.
	 *
	 * Given that iBoot guarantees that the unslid kernelcache base address
	 * will begin on an L2 boundary, this should prevent us from accidentally
	 * mapping TZ0.
	 */
	adrp	x0, EXT(_mh_execute_header)@page	// address of kernel mach header
	add		x0, x0, EXT(_mh_execute_header)@pageoff
	ldr		w1, [x0, #0x18]						// load mach_header->flags
	tbz		w1, #0x1f, Lkernelcache_base_found	// if MH_DYLIB_IN_CACHE unset, base is kernel mach header
	ldr		w1, [x0, #0x20]						// load first segment cmd (offset sizeof(kernel_mach_header_t))
	cmp		w1, #0x19							// must be LC_SEGMENT_64
	bne		.
	ldr		x1, [x0, #0x38]						// load first segment vmaddr
	sub		x1, x0, x1							// compute slide
	MOV64	x0, VM_KERNEL_LINK_ADDRESS
	add		x0, x0, x1							// base is kernel link address + slide

Lkernelcache_base_found:
	/*
	 * Adjust physical and virtual base addresses to account for physical
	 * memory preceeding xnu Mach-O header
	 * x22 - Kernel virtual base
	 * x23 - Kernel physical base
	 * x24 - Physical memory size
	 */
	sub		x18, x0, x23
	sub		x24, x24, x18
	add		x22, x22, x18
	add		x23, x23, x18
#if defined(SWITCH_XNU_BOARD) && SWITCH_XNU_BOARD
    // Full native DRAM is described by boot_args and the live ADT ledger.
    // These temporary block mappings must stop before the Device framebuffer
    // slots; C builds the complete exclusion-aware aperture before low steals.
    ldr x16, [x20, BA_PHYS_BASE]
    MOV64 x17, SWITCH_BOOT_PHYS_BASE
    cmp x16, x17
    b.ne Lswitch_bootstrap_span_ready
    ldr x16, [x20, BA_MEM_SIZE]
    MOV64 x17, SWITCH_BOOT_MEM_SIZE
    cmp x16, x17
    b.ne Lswitch_bootstrap_span_ready
    ldr x16, [x20, BA_VIRT_BASE]
    MOV64 x17, SWITCH_BOOT_VIRT_BASE
    cmp x16, x17
    b.ne Lswitch_bootstrap_span_ready
    MOV64 x17, 0xe207c000
    sub x24, x17, x23
Lswitch_bootstrap_span_ready:
#endif

	/*
	 * x0  - V=P virtual cursor
	 * x4  - V=P physical cursor
	 * x14 - KVA virtual cursor
	 * x15 - KVA physical cursor
	 */
	mov		x4, x0
	mov		x14, x22
	mov		x15, x23

	/*
	 * Allocate L1 tables
	 * x1 - V=P L1 page
	 * x3 - KVA L1 page
	 * x2 - free mem pointer from which we allocate a variable number of L2
	 * pages. The maximum number of bootstrap page table pages is limited to
	 * BOOTSTRAP_TABLE_SIZE. For a 2G 4k page device, assuming the worst-case
	 * slide, we need 1xL1 and up to 3xL2 pages (1GB mapped per L1 entry), so
	 * 8 total pages for V=P and KVA.
	 */
	mov		x1, x25
	add		x3, x1, PGBYTES
	mov		x2, x3

	/*
	 * Setup the V=P bootstrap mapping
	 * x5 - total number of L2 entries to allocate
	 */
	lsr		x5,  x24, #(ARM_TT_L2_SHIFT)
	/* create_bootstrap_mapping(vbase, pbase, num_ents, L1 table, freeptr) */
	create_bootstrap_mapping x0,  x4,  x5, x1, x2, x6, x10, x11, x12, x13

	/* Setup the KVA bootstrap mapping */
	lsr		x5,  x24, #(ARM_TT_L2_SHIFT)
	create_bootstrap_mapping x14, x15, x5, x3, x2, x9, x10, x11, x12, x13

	b Lbootstrap_tables_ready
Lbootstrap_tables_exhausted:
	// MMU is still off. Stop with the offending cursor/end preserved for
	// the early debugger rather than corrupting memory or forging a map.
	b Lbootstrap_tables_exhausted
Lbootstrap_tables_ready:
#if defined(SWITCH_XNU_BOARD) && SWITCH_XNU_BOARD
	cbz x28, Lswitch_fb_map_done
	// The fixed window uses TTBR0 L1 index 3 but stops below the framebuffer.
	// Reuse only its private L2 page and require both new block slots empty.
	ldr x16, [x25, #24]
	and x17, x16, #3
	cmp x17, #3
	b.ne Lswitch_fb_map_failed
	and x16, x16, #(ARM_TTE_TABLE_MASK)
	add x17, x25, #2, lsl #12
	cmp x16, x17
	b.lo Lswitch_fb_map_failed
	add x17, x25, #(BOOTSTRAP_TABLE_BYTES >> 12), lsl #12
	cmp x16, x17
	b.hs Lswitch_fb_map_failed
	ldr x17, [x16, #0xd68]
	cbnz x17, Lswitch_fb_map_failed
	ldr x17, [x16, #0xd70]
	cbnz x17, Lswitch_fb_map_failed
	// Kernel-only RW, PXN+UXN, AF, outer-shareable Device-nGnRnE. The
	// reservation is 4MiB, but every renderer write stays in the real 0x384000.
	MOV64 x17, (0xf5a00000 | ARM_TTE_VALID | ARM_TTE_BLOCK_AF | ARM_TTE_BLOCK_PNX | ARM_TTE_BLOCK_NX | ARM_TTE_BLOCK_SH(SH_OUTER_MEMORY) | ARM_TTE_BLOCK_ATTRINDX(CACHE_ATTRINDX_DISABLE))
	str x17, [x16, #0xd68]
	MOV64 x17, (0xf5c00000 | ARM_TTE_VALID | ARM_TTE_BLOCK_AF | ARM_TTE_BLOCK_PNX | ARM_TTE_BLOCK_NX | ARM_TTE_BLOCK_SH(SH_OUTER_MEMORY) | ARM_TTE_BLOCK_ATTRINDX(CACHE_ATTRINDX_DISABLE))
	str x17, [x16, #0xd70]
	SWITCH_EARLY_MARK 3
	b Lswitch_fb_map_done
Lswitch_fb_map_failed:
	SWITCH_EARLY_MARK 3, 0xff7070ff
	b Lswitch_early_halt
Lswitch_fb_map_done:
#endif
	/* Ensure TTEs are visible */
	dsb		ish


	b		common_start

/*
 * Begin common CPU initialization
 *
 * Regster state:
 *	x20 - PA of boot args
 *	x21 - zero on cold boot, PA of cpu data on warm reset
 *	x22 - Kernel virtual base
 *	x23 - Kernel physical base
 *	x25 - PA of the V=P pagetable root
 *	 lr - KVA of C init routine
 *	 sp - SP_EL0 selected
 *
 *	SP_EL0 - KVA of CPU's interrupt stack
 *	SP_EL1 - KVA of CPU's exception stack
 *	TPIDRRO_EL0 - CPU number
 */
common_start:

#if HAS_NEX_PG
	mov x19, lr
	bl		EXT(set_nex_pg)
	mov lr, x19
#endif

	// Set the translation control register.
	MOV64	x1, TCR_EL1_BOOT
	MSR_TCR_EL1_X1


	/* Set up translation table base registers.
	 *	TTBR0 - V=P table @ top of kernel
	 *	TTBR1 - KVA table @ top of kernel + 1 page
	 */
#if defined(KERNEL_INTEGRITY_KTRR) || defined(KERNEL_INTEGRITY_CTRR) || defined(KERNEL_INTEGRITY_PV_CTRR)
	/* Note that for KTRR configurations, the V=P map will be modified by
	 * arm_vm_init.c.
	 */
#endif
	and		x0, x25, #(TTBR_BADDR_MASK)
	mov		x19, lr
	bl		EXT(set_mmu_ttb)
	mov		lr, x19
	add		x0, x25, PGBYTES
	and		x0, x0, #(TTBR_BADDR_MASK)
	MSR_TTBR1_EL1_X0

	// Set up MAIR attr0 for normal memory, attr1 for device memory
	mov		x0, xzr
	mov		x1, #(MAIR_WRITEBACK << MAIR_ATTR_SHIFT(CACHE_ATTRINDX_WRITEBACK))
	orr		x0, x0, x1
	mov		x1, #(MAIR_WRITETHRU << MAIR_ATTR_SHIFT(CACHE_ATTRINDX_WRITETHRU))
	orr		x0, x0, x1
	mov		x1, #(MAIR_WRITECOMB << MAIR_ATTR_SHIFT(CACHE_ATTRINDX_WRITECOMB))
	orr		x0, x0, x1
	mov		x1, #(MAIR_WRITEBACK << MAIR_ATTR_SHIFT(CACHE_ATTRINDX_RESERVED))
#if HAS_MTE
	mov		x1, #(MAIR_MTE_WRITEBACK << MAIR_ATTR_SHIFT(CACHE_ATTRINDX_RESERVED))
#endif /* HAS_MTE */
	orr		x0, x0, x1
	mov		x1, #(MAIR_POSTED_COMBINED_REORDERED << MAIR_ATTR_SHIFT(CACHE_ATTRINDX_POSTED_COMBINED_REORDERED))
	orr		x0, x0, x1
	mov		x1, #(MAIR_DISABLE << MAIR_ATTR_SHIFT(CACHE_ATTRINDX_DISABLE))
	orr		x0, x0, x1
#if HAS_FEAT_XS
	mov		x1, #(MAIR_DISABLE_XS << MAIR_ATTR_SHIFT(CACHE_ATTRINDX_DISABLE_XS))
	orr		x0, x0, x1
	mov		x1, #(MAIR_POSTED_COMBINED_REORDERED_XS << MAIR_ATTR_SHIFT(CACHE_ATTRINDX_POSTED_COMBINED_REORDERED_XS))
	orr		x0, x0, x1
#else
	mov		x1, #(MAIR_POSTED << MAIR_ATTR_SHIFT(CACHE_ATTRINDX_POSTED))
	orr		x0, x0, x1
	mov		x1, #(MAIR_POSTED_REORDERED << MAIR_ATTR_SHIFT(CACHE_ATTRINDX_POSTED_REORDERED))
	orr		x0, x0, x1
#endif /* HAS_FEAT_XS */
	msr		MAIR_EL1, x0
	isb
	tlbi	vmalle1
	dsb		ish


#ifndef __ARM_IC_NOALIAS_ICACHE__
	/* Invalidate the TLB and icache on systems that do not guarantee that the
	 * caches are invalidated on reset.
	 */
	tlbi	vmalle1
	ic		iallu
#endif

#if defined(XNU_NATIVE_ARCH_TIMER) && XNU_NATIVE_ARCH_TIMER
	/* Finish local maintenance before a first-start secondary enables M/C/I. */
	dsb sy
	isb
#endif

	/* If x21 is not 0, then this is either the start_cpu path or
	 * the resume_idle_cpu path.  cpu_ttep should already be
	 * populated, so just switch to the kernel_pmap now.
	 */

	cbz		x21, 1f
	adrp	x0, EXT(cpu_ttep)@page
	add		x0, x0, EXT(cpu_ttep)@pageoff
	ldr		x0, [x0]
	MSR_TTBR1_EL1_X0
1:

	// Set up the exception vectors
#if __ARM_KERNEL_PROTECT__
	/* If this is not the first reset of the boot CPU, the alternate mapping
	 * for the exception vectors will be set up, so use it.  Otherwise, we
	 * should use the mapping located in the kernelcache mapping.
	 */
	MOV64	x0, ARM_KERNEL_PROTECT_EXCEPTION_START

	cbnz		x21, 1f
#endif /* __ARM_KERNEL_PROTECT__ */
	adrp	x0, EXT(ExceptionVectorsBase)@page			// Load exception vectors base address
	add		x0, x0, EXT(ExceptionVectorsBase)@pageoff
	add		x0, x0, x22									// Convert exception vector address to KVA
	sub		x0, x0, x23
1:
#if defined(SWITCH_XNU_BOARD) && SWITCH_XNU_BOARD
	cbnz x21, Lswitch_normal_vectors
	cbz x28, Lswitch_normal_vectors
	// Keep physical vectors while MMU is off; this identity alias survives enable.
	adrp x0, EXT(LowExceptionVectorBase)@page
	add x0, x0, EXT(LowExceptionVectorBase)@pageoff
Lswitch_normal_vectors:
#endif
#if defined(XNU_NATIVE_ARCH_TIMER) && XNU_NATIVE_ARCH_TIMER
	cbz x27, Lswitch_secondary_vbar_selected
	/* This identity vector remains valid across M=0 -> M=1 via TTBR0. */
	adrp x0, EXT(switch_secondary_vectors)@page
	add x0, x0, EXT(switch_secondary_vectors)@pageoff
Lswitch_secondary_vbar_selected:
#endif
	MSR_VBAR_EL1_X0

#if HAS_APPLE_PAC
	PAC_INIT_KEY_STATE tmp=x0, tmp2=x1
#endif /* HAS_APPLE_PAC */

	SWITCH_EARLY_MARK 4
	// Enable caches, MMU, ROP and JOP
	MOV64   x0, SCTLR_EL1_DEFAULT
	MSR_SCTLR_EL1_X0
	isb		sy
#if defined(XNU_NATIVE_ARCH_TIMER) && XNU_NATIVE_ARCH_TIMER
	cbz x27, Lswitch_secondary_mmu_live
	/* MMU is now live: keep faults in our KVA vectors until arm_init_cpu
	 * installs its thread, then switch_secondary_vectors_leave restores XNU. */
	add x27, x27, x22
	sub x27, x27, x23
	adrp x16, EXT(switch_secondary_vectors)@page
	add x16, x16, EXT(switch_secondary_vectors)@pageoff
	add x16, x16, x22
	sub x16, x16, x23
	msr VBAR_EL1, x16
	isb
	mov x16, #SWITCH_SMP_STAGE_MMU
	str x16, [x27, #SWITCH_SMP_STAGE_OFF]
	add x16, x27, #SWITCH_SMP_STATUS_OFFSET
	dc cvac, x16
	dsb sy
Lswitch_secondary_mmu_live:
#endif
#if defined(SWITCH_XNU_BOARD) && SWITCH_XNU_BOARD
	cbnz x21, Lswitch_vectors_live
	cbz x28, Lswitch_vectors_live
	// Now switch to the KVA alias; keep x0 intact for the SCTLR check below.
	adrp x16, EXT(LowExceptionVectorBase)@page
	add x16, x16, EXT(LowExceptionVectorBase)@pageoff
	add x16, x16, x22
	sub x16, x16, x23
	msr VBAR_EL1, x16
	isb sy
Lswitch_vectors_live:
#endif
	SWITCH_EARLY_MARK 5

#if !VMAPPLE
	MOV64	x1, SCTLR_EL1_DEFAULT
	cmp		x0, x1
	bne		.
#endif /* !VMAPPLE */

#if (!CONFIG_KERNEL_INTEGRITY || (CONFIG_KERNEL_INTEGRITY && !defined(KERNEL_INTEGRITY_WT)))
	/* Watchtower
	 *
	 * If we have a Watchtower monitor it will setup CPACR_EL1 for us, touching
	 * it here would trap to EL3.
	 */

	// Enable NEON
	mov		x0, #(CPACR_FPEN_ENABLE)
	msr		CPACR_EL1, x0
	isb		sy					// Synchronize FP/SIMD access before C code
#endif

	// Clear thread pointer
	msr		TPIDR_EL1, xzr						// Set thread register


#if defined(APPLE_ARM64_ARCH_FAMILY)
	mrs		x12, MDSCR_EL1
	orr		x12, x12, MDSCR_TDCC
	msr		MDSCR_EL1, x12
	// Initialization common to all non-virtual Apple targets
#endif  // APPLE_ARM64_ARCH_FAMILY


#if defined(APPLE_ARM64_ARCH_FAMILY)
	// Read MIDR before start of per-SoC tunables
	mrs x12, MIDR_EL1

	APPLY_TUNABLES x12, x13, x14
#endif

#if HAS_CLUSTER && !NO_CPU_OVRD
	// Unmask external IRQs if we're restarting from non-retention WFI
	mrs		x9, CPU_OVRD
	and		x9, x9, #(~(ARM64_REG_CYC_OVRD_irq_mask | ARM64_REG_CYC_OVRD_fiq_mask))
	msr		CPU_OVRD, x9
#endif

	// If x21 != 0, we're doing a warm reset, so we need to trampoline to the kernel pmap.
	cbnz	x21, Ltrampoline

	SWITCH_EARLY_MARK 6
	// Set KVA of boot args as first arg
	add		x0, x20, x22
	sub		x0, x0, x23

#if KASAN
	mov	x20, x0
	mov	x21, lr

	// x0: boot args
	// x1: KVA page table phys base
	mrs	x1, TTBR1_EL1
	bl	EXT(kasan_bootstrap)

	mov	x0, x20
	mov	lr, x21
#endif

	// Return to arm_init()
	ret

Ltrampoline:
	// Load VA of the trampoline
	adrp	x0, arm_init_tramp@page
	add		x0, x0, arm_init_tramp@pageoff
	add		x0, x0, x22
	sub		x0, x0, x23

	// Branch to the trampoline
	br		x0

/*
 * V=P to KVA trampoline.
 *	x0 - KVA of cpu data pointer
 */
	.text
	.align 2
arm_init_tramp:
	ARM64_JUMP_TARGET
	/* On a warm boot, the full kernel translation table is initialized in
	 * addition to the bootstrap tables. The layout is as follows:
	 *
	 *  +--Top of Memory--+
	 *         ...
	 *  |                 |
	 *  |  Primary Kernel |
	 *  |   Trans. Table  |
	 *  |                 |
	 *  +--Top + 5 pages--+
	 *  |                 |
	 *  |  Invalid Table  |
	 *  |                 |
	 *  +--Top + 4 pages--+
	 *  |                 |
	 *  |    KVA Table    |
	 *  |                 |
	 *  +--Top + 2 pages--+
	 *  |                 |
	 *  |    V=P Table    |
	 *  |                 |
	 *  +--Top of Kernel--+
	 *  |                 |
	 *  |  Kernel Mach-O  |
	 *  |                 |
	 *         ...
	 *  +---Kernel Base---+
	 */


	mov		x19, lr
	// Convert CPU data PA to VA and set as first argument
	mov		x0, x21
	bl		EXT(phystokv)

	mov		lr, x19

	/* Return to arm_init() */
	ret

//#include	"globals_asm.h"

/* vim: set ts=4: */
