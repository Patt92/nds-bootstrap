/*---------------------------------------------------------------------------------

  Copyright (C) 2005
  	Dave Murphy (WinterMute)

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any
  damages arising from the use of this software.

  Permission is granted to anyone to use this software for any
  purpose, including commercial applications, and to alter it and
  redistribute it freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you
     must not claim that you wrote the original software. If you use
     this software in a product, an acknowledgment in the product
     documentation would be appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and
     must not be misrepresented as being the original software.
  3. This notice may not be removed or altered from any source
     distribution.

---------------------------------------------------------------------------------*/
#include <nds/asminc.h>

	.text

	.arm

@---------------------------------------------------------------------------------
BEGIN_ASM_FUNC getCPSR
@---------------------------------------------------------------------------------
	mrs	r0,cpsr
	bx	r14

@---------------------------------------------------------------------------------
BEGIN_ASM_FUNC getDtcmBase
@---------------------------------------------------------------------------------
@ Experimental RTS: where the game mapped the ARM9 data TCM
	mrc	p15,0,r0,c9,c1,0
	mov	r0, r0, lsr #12
	mov	r0, r0, lsl #12
	bx	r14
