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
#include "locations.h"

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
@ region (excluded from restore) inside the IRQ path with CPSR.I set. From
@ the moment the DTCM copy starts the current C stack is gone, so this code
@ is strictly stack-free. Field offsets follow rtsCpuContext in rts_state.h:
@   0x00 r4-r11  0x20 sp  0x24 lr  0x28 cpsr  0x2C spsr
@   0x30 spSys  0x34 lrSys  0x38 spSvc  0x3C lrSvc  0x40 spsrSvc
@   0x44 ime  0x48 ie
@---------------------------------------------------------------------------------
#define RTS_STAGING_DTCM 	(INGAME_MENU_EXT_LOCATION + 0x34000)
#define RTS_STAGING_ITCM 	(INGAME_MENU_EXT_LOCATION + 0x38000)

BEGIN_ASM_FUNC rtsResumeArm9
	ldr	r1, =0x04000208
	mov	r2, #0
	str	r2, [r1]		@ REG_IME = 0

	@ Open MPU region 0 so the staging area and both TCMs are accessible
	@ under the game's MPU config (same trick as the IGM's changeMpu);
	@ the original value is put back before the jump
	mrc	p15, 0, r12, c6, c0, 0
	mov	r2, #0x35
	mcr	p15, 0, r2, c6, c0, 0

	@ DTCM image -> wherever the game mapped its DTCM
	mrc	p15, 0, r2, c9, c1, 0
	mov	r2, r2, lsr #12
	mov	r2, r2, lsl #12
	ldr	r3, =RTS_STAGING_DTCM
	mov	r1, #0x4000
.rtsDtcmCopy:
	ldmia	r3!, {r4-r7}
	stmia	r2!, {r4-r7}
	subs	r1, r1, #16
	bne	.rtsDtcmCopy

	@ ITCM image -> ITCM mirror
	mov	r2, #0x01000000
	ldr	r3, =RTS_STAGING_ITCM
	mov	r1, #0x8000
.rtsItcmCopy:
	ldmia	r3!, {r4-r7}
	stmia	r2!, {r4-r7}
	subs	r1, r1, #16
	bne	.rtsItcmCopy

	mcr	p15, 0, r12, c6, c0, 0	@ MPU region 0 back

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
