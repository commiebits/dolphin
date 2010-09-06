// Copyright (C) 2003 Dolphin Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official SVN repository and contact information can be found at
// http://code.google.com/p/dolphin-emu/
#include "Common.h"
#include "Thunk.h"

#include "../../Core.h"
#include "../PowerPC.h"
#include "../../CoreTiming.h"
#include "../PPCTables.h"
#include "x64Emitter.h"

#include "Jit.h"
#include "JitRegCache.h"
#include "JitAsm.h"

// The branches are known good, or at least reasonably good.
// No need for a disable-mechanism.

// If defined, clears CR0 at blr and bl-s. If the assumption that
// flags never carry over between functions holds, then the task for 
// an optimizer becomes much easier.

// #define ACID_TEST

// Zelda and many more games seem to pass the Acid Test. 

using namespace Gen;

void Jit64::sc(UGeckoInstruction inst)
{
	INSTRUCTION_START
	JITDISABLE(Branch)

	gpr.Flush(FLUSH_ALL);
	fpr.Flush(FLUSH_ALL);
	MOV(32, M(&PC), Imm32(js.compilerPC + 4));
	LOCK();
	OR(32, M((void *)&PowerPC::ppcState.Exceptions), Imm32(EXCEPTION_SYSCALL));
	WriteExceptionExit();
}

void Jit64::rfi(UGeckoInstruction inst)
{
	INSTRUCTION_START
	JITDISABLE(Branch)

	gpr.Flush(FLUSH_ALL);
	fpr.Flush(FLUSH_ALL);
	// See Interpreter rfi for details
	const u32 mask = 0x87C0FFFF;
	const u32 clearMSR13 = 0xFFFBFFFF; // Mask used to clear the bit MSR[13]
	// MSR = ((MSR & ~mask) | (SRR1 & mask)) & clearMSR13;
	AND(32, M(&MSR), Imm32((~mask) & clearMSR13));
	MOV(32, R(EAX), M(&SRR1));
	AND(32, R(EAX), Imm32(mask & clearMSR13));
	OR(32, M(&MSR), R(EAX));
	// NPC = SRR0;
	MOV(32, R(EAX), M(&SRR0));
	if (Core::g_CoreStartupParameter.bAlternateRFI)
		WriteExitDestInEAX();
	else
		WriteRfiExitDestInEAX();
}

void Jit64::bx(UGeckoInstruction inst)
{
	INSTRUCTION_START
	JITDISABLE(Branch)

	if (inst.LK)
		MOV(32, M(&LR), Imm32(js.compilerPC + 4));
	gpr.Flush(FLUSH_ALL);
	fpr.Flush(FLUSH_ALL);

	if (js.isLastInstruction)
	{
		u32 destination;
		if (inst.AA)
			destination = SignExt26(inst.LI << 2);
		else
			destination = js.compilerPC + SignExt26(inst.LI << 2);
#ifdef ACID_TEST
		if (inst.LK)
			AND(32, M(&PowerPC::ppcState.cr), Imm32(~(0xFF000000)));
#endif
		if (destination == js.compilerPC)
		{
			//PanicAlert("Idle loop detected at %08x", destination);
		//	CALL(ProtectFunction(&CoreTiming::Idle, 0));
		//	JMP(Asm::testExceptions, true);
			// make idle loops go faster
			js.downcountAmount += 8;
		}
		WriteExit(destination, 0);
	}
	else {
		// TODO: investigate the good old method of merging blocks here.
		PanicAlert("bx not last instruction of block"); // this should not happen
	}
}

// TODO - optimize to hell and beyond
// TODO - make nice easy to optimize special cases for the most common
// variants of this instruction.
void Jit64::bcx(UGeckoInstruction inst)
{
	INSTRUCTION_START
	JITDISABLE(Branch)

	// USES_CR
	_assert_msg_(DYNA_REC, js.isLastInstruction, "bcx not last instruction of block");

	gpr.Flush(FLUSH_ALL);
	fpr.Flush(FLUSH_ALL);

	FixupBranch pCTRDontBranch;
	if ((inst.BO & BO_DONT_DECREMENT_FLAG) == 0)  // Decrement and test CTR
	{
		SUB(32, M(&CTR), Imm8(1));
		if (inst.BO & BO_BRANCH_IF_CTR_0)
			pCTRDontBranch = J_CC(CC_NZ);
		else
			pCTRDontBranch = J_CC(CC_Z);
	}

	FixupBranch pConditionDontBranch;
	if ((inst.BO & BO_DONT_CHECK_CONDITION) == 0)  // Test a CR bit
	{
		TEST(8, M(&PowerPC::ppcState.cr_fast[inst.BI >> 2]), Imm8(8 >> (inst.BI & 3)));
		if (inst.BO & BO_BRANCH_IF_TRUE)  // Conditional branch 
			pConditionDontBranch = J_CC(CC_Z);
		else
			pConditionDontBranch = J_CC(CC_NZ);
	}
	
	if (inst.LK)
		MOV(32, M(&LR), Imm32(js.compilerPC + 4));

	u32 destination;
	if(inst.AA)
		destination = SignExt16(inst.BD << 2);
	else
		destination = js.compilerPC + SignExt16(inst.BD << 2);
	WriteExit(destination, 0);

	if ((inst.BO & BO_DONT_CHECK_CONDITION) == 0)
		SetJumpTarget( pConditionDontBranch );
	if ((inst.BO & BO_DONT_DECREMENT_FLAG) == 0)
		SetJumpTarget( pCTRDontBranch );
	WriteExit(js.compilerPC + 4, 1);
}

void Jit64::bcctrx(UGeckoInstruction inst)
{
	INSTRUCTION_START
	JITDISABLE(Branch)

	gpr.Flush(FLUSH_ALL);
	fpr.Flush(FLUSH_ALL);

	// bcctrx doesn't decrement and/or test CTR
	_dbg_assert_msg_(POWERPC, inst.BO_2 & BO_DONT_DECREMENT_FLAG, "bcctrx with decrement and test CTR option is invalid!");

	if (inst.BO_2 & BO_DONT_CHECK_CONDITION)
	{
		// BO_2 == 1z1zz -> b always

		//NPC = CTR & 0xfffffffc;
		MOV(32, R(EAX), M(&CTR));
		if (inst.LK_3)
			MOV(32, M(&LR), Imm32(js.compilerPC + 4)); //	LR = PC + 4;
		AND(32, R(EAX), Imm32(0xFFFFFFFC));
		WriteExitDestInEAX();
	}
	else
	{
		// Rare condition seen in (just some versions of?) Nintendo's NES Emulator

		// BO_2 == 001zy -> b if false
		// BO_2 == 011zy -> b if true

		// Ripped from bclrx
		TEST(8, M(&PowerPC::ppcState.cr_fast[inst.BI >> 2]), Imm8(8 >> (inst.BI & 3)));
		Gen::CCFlags branch;
		if (inst.BO_2 & BO_BRANCH_IF_TRUE)
			branch = CC_Z;
		else
			branch = CC_NZ; 
		FixupBranch b = J_CC(branch, false);
		MOV(32, R(EAX), M(&CTR));
		AND(32, R(EAX), Imm32(0xFFFFFFFC));
		//MOV(32, M(&PC), R(EAX)); => Already done in WriteExitDestInEAX()
		if (inst.LK_3)
			MOV(32, M(&LR), Imm32(js.compilerPC + 4)); //	LR = PC + 4;
		WriteExitDestInEAX();
		// Would really like to continue the block here, but it ends. TODO.
		SetJumpTarget(b);
		WriteExit(js.compilerPC + 4, 1);
	}
}

void Jit64::bclrx(UGeckoInstruction inst)
{
	INSTRUCTION_START
	JITDISABLE(Branch)

	gpr.Flush(FLUSH_ALL);
	fpr.Flush(FLUSH_ALL);

	FixupBranch pCTRDontBranch;
	if ((inst.BO & BO_DONT_DECREMENT_FLAG) == 0)  // Decrement and test CTR
	{
		SUB(32, M(&CTR), Imm8(1));
		if (inst.BO & BO_BRANCH_IF_CTR_0)
			pCTRDontBranch = J_CC(CC_NZ);
		else
			pCTRDontBranch = J_CC(CC_Z);
	}

	FixupBranch pConditionDontBranch;
	if ((inst.BO & BO_DONT_CHECK_CONDITION) == 0)  // Test a CR bit
	{
		TEST(8, M(&PowerPC::ppcState.cr_fast[inst.BI >> 2]), Imm8(8 >> (inst.BI & 3)));
		if (inst.BO & BO_BRANCH_IF_TRUE)  // Conditional branch 
			pConditionDontBranch = J_CC(CC_Z);
		else
			pConditionDontBranch = J_CC(CC_NZ);
	}

		// This below line can be used to prove that blr "eats flags" in practice.
		// This observation will let us do a lot of fun observations.
#ifdef ACID_TEST
		AND(32, M(&PowerPC::ppcState.cr), Imm32(~(0xFF000000)));
#endif

	MOV(32, R(EAX), M(&LR));	
	AND(32, R(EAX), Imm32(0xFFFFFFFC));
	if (inst.LK)
		MOV(32, M(&LR), Imm32(js.compilerPC + 4));
	WriteExitDestInEAX();

	if ((inst.BO & BO_DONT_CHECK_CONDITION) == 0)
		SetJumpTarget( pConditionDontBranch );
	if ((inst.BO & BO_DONT_DECREMENT_FLAG) == 0)
		SetJumpTarget( pCTRDontBranch );
	WriteExit(js.compilerPC + 4, 1);
}
