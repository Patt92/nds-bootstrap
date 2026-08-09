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

#endif // !TWLSDK && !GSDD
