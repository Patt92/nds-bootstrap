@ Experimental RTS: CPU context capture (see docs/rts-architecture.md §3).
@
@ int rtsCaptureContext(u32* ctx)
@
@ setjmp-style: returns 0 after capturing, and the M4 resume trampoline
@ re-enters at the call's return address with r0=1 so the surrounding IRQ
@ path unwinds through the restored (save-time) stack frames. Called at a
@ function-call boundary, so caller-saved registers are dead by definition;
@ what matters is r4-r11, sp/lr of this mode, CPSR, the interrupted game's
@ SPSR, and the banked SYS and SVC state (games sit in either user/system
@ code or an svc wait like swiIntrWait when the IRQ hits).
@
@ Store order must match rtsCpuContext in rts_state.h.

#include <nds/asminc.h>

// Keep the size-constrained TWLSDK/GSDD engine variants untouched
#if !defined(TWLSDK) && !defined(GSDD)

	.text
	.arm

BEGIN_ASM_FUNC rtsCaptureContext
	stmia	r0!, {r4-r11}
	str	sp, [r0], #4
	str	lr, [r0], #4
	mrs	r2, cpsr
	str	r2, [r0], #4
	mrs	r1, spsr		@ interrupted game CPSR (valid in IRQ/SVC mode,
	str	r1, [r0], #4		@ meaningless if the SDK dispatched in system mode)

	bic	r3, r2, #0x1F
	orr	r1, r3, #0x1F		@ system mode: user-visible sp/lr
	msr	cpsr_c, r1
	str	sp, [r0], #4
	str	lr, [r0], #4

	orr	r1, r3, #0x13		@ svc mode
	msr	cpsr_c, r1
	str	sp, [r0], #4
	str	lr, [r0], #4
	mrs	r1, spsr
	str	r1, [r0], #4

	msr	cpsr_c, r2		@ back to the capture mode
	mov	r0, #0
	bx	lr

@---------------------------------------------------------------------------------
@ void rtsResumeArm9(u32* ctx)   -- never returns to its caller
@
@ The load-side longjmp (docs/rts-architecture.md §6/§8). Runs from the ce9
@ region (excluded from restore) inside the IRQ path with CPSR.I set. The
@ stack it was called on is about to become save-time memory, so this code is
@ strictly stack-free. Field offsets follow rtsCpuContext in rts_state.h:
@   0x00 r4-r11  0x20 sp  0x24 lr  0x28 cpsr  0x2C spsr
@   0x30 spSys  0x34 lrSys  0x38 spSvc  0x3C lrSvc  0x40 spsrSvc
@   0x44 ime  0x48 ie
@---------------------------------------------------------------------------------
BEGIN_ASM_FUNC rtsResumeArm9
	ldr	r1, =0x04000208
	mov	r2, #0
	str	r2, [r1]		@ REG_IME = 0

	@ TCM images are not restored (RTS_RESTORE_TCM): staging them would
	@ clobber memory that can no longer be paged back in at this point.
	@ The ARM9 keeps its current TCM contents.

	@ No stale line may shadow the restored world
	mov	r2, #0
	mcr	p15, 0, r2, c7, c5, 0	@ invalidate icache
	mcr	p15, 0, r2, c7, c6, 0	@ invalidate dcache
	mcr	p15, 0, r2, c7, c10, 4	@ drain write buffer

	@ Interrupt config; CPSR.I still masks anything until the game's own
	@ IRQ return re-enables interrupts with its restored CPSR
	ldr	r1, =0x04000210
	ldr	r2, [r0, #0x48]
	str	r2, [r1]		@ REG_IE
	ldr	r1, =0x04000208
	ldr	r2, [r0, #0x44]
	str	r2, [r1]		@ REG_IME

	@ Banked SVC and SYS state
	mrs	r9, cpsr
	bic	r8, r9, #0x1F
	orr	r2, r8, #0x13
	msr	cpsr_c, r2
	ldr	sp, [r0, #0x38]
	ldr	lr, [r0, #0x3C]
	ldr	r3, [r0, #0x40]
	msr	spsr_cxsf, r3
	orr	r2, r8, #0x1F
	msr	cpsr_c, r2
	ldr	sp, [r0, #0x30]
	ldr	lr, [r0, #0x34]

	@ Back to the capture mode with its SPSR and registers; return to the
	@ rtsCaptureContext call site with r0=1 so the save-time IRQ path
	@ unwinds through the just-restored stacks
	ldr	r2, [r0, #0x28]
	msr	cpsr_c, r2
	ldr	r3, [r0, #0x2C]
	msr	spsr_cxsf, r3
	ldr	sp, [r0, #0x20]
	ldr	lr, [r0, #0x24]
	ldmia	r0, {r4-r11}
	mov	r0, #1
	bx	lr
.pool

#endif // !TWLSDK && !GSDD
