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
@ void rtsResumeArm7(u32* ctx, const u32* wramSrc, u32* wramDst, u32 wramLen)
@ -- never returns to its caller
@
@ ARM7 counterpart of rtsResumeArm9: copies the staged ARM7-WRAM image over
@ 0x03800000 (which holds the live stack, hence strictly stack-free from the
@ first store on), then longjmps into the save-time context. Runs from the
@ ce7 region in shared WRAM, which is never restored. CPSR.I is set.
@ Field offsets follow rtsCpuContext in rts_state.h (see rtsResumeArm9).
@---------------------------------------------------------------------------------
BEGIN_ASM_FUNC rtsResumeArm7
	ldr	r12, =0x04000208
	mov	r4, #0
	str	r4, [r12]		@ REG_IME = 0

	cmp	r3, #0			@ a zero length means "leave ARM7 memory alone"
	beq	.rtsWramDone
.rtsWramCopy:
	ldmia	r1!, {r4-r7}
	stmia	r2!, {r4-r7}
	subs	r3, r3, #16
	bne	.rtsWramCopy
.rtsWramDone:

	@ Interrupt config; CPSR.I still masks until the game's IRQ return
	ldr	r1, =0x04000210
	ldr	r2, [r0, #0x48]
	str	r2, [r1]		@ REG_IE
	ldr	r2, [r0, #0x44]
	str	r2, [r12]		@ REG_IME

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

	@ Capture mode, SPSR, registers; return to the rtsCaptureContext call
	@ site with r0=1 so the save-time VBlank path unwinds
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
