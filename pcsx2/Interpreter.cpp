// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Common.h"
#include "MmuTrace.h"
#include "R5900OpcodeTables.h"
#include "VMManager.h"
#include "Elfheader.h"
#include "Cache.h"

#include "DebugTools/Breakpoints.h"

#include "common/FastJmp.h"

#include <float.h>

using namespace R5900;		// for OPCODE and OpcodeImpl

extern int vu0branch, vu1branch;

static int branch2 = 0;
static u32 cpuBlockCycles = 0;		// 3 bit fixed point version of cycle count
static std::string disOut;
static bool intExitExecution = false;
static fastjmp_buf intJmpBuf;
static u32 intLastBranchTo;

void intEventTest();

void intUpdateCPUCycles()
{
	const bool lowcycles = (cpuBlockCycles <= 40);
	const s8 cyclerate = EmuConfig.Speedhacks.EECycleRate;
	u32 scale_cycles = 0;

	if (cyclerate == 0 || lowcycles || cyclerate < -99 || cyclerate > 3)
		scale_cycles = cpuBlockCycles >> 3;

	else if (cyclerate > 1)
		scale_cycles = cpuBlockCycles >> (2 + cyclerate);

	else if (cyclerate == 1)
		scale_cycles = (cpuBlockCycles >> 3) / 1.3f; // Adds a mild 30% increase in clockspeed for value 1.

	else if (cyclerate == -1) // the mildest value.
		// These values were manually tuned to yield mild speedup with high compatibility
		scale_cycles = (cpuBlockCycles <= 80 || cpuBlockCycles > 168 ? 5 : 7) * cpuBlockCycles / 32;

	else
		scale_cycles = ((5 + (-2 * (cyclerate + 1))) * cpuBlockCycles) >> 5;

	// Ensure block cycle count is never less than 1.
	cpuRegs.cycle += (scale_cycles < 1) ? 1 : scale_cycles;

	if (cyclerate > 1)
	{
		cpuBlockCycles &= (0x1 << (cyclerate + 2)) - 1;
	}
	else
	{
		cpuBlockCycles &= 0x7;
	}
}

// These macros are used to assemble the repassembler functions

void intBreakpoint(bool memcheck)
{
	const u32 pc = cpuRegs.pc;
 	if (CBreakPoints::CheckSkipFirst(BREAKPOINT_EE, pc) != 0)
	{
		CBreakPoints::ClearSkipFirst(BREAKPOINT_EE);
		return;
	}

	if (!memcheck)
	{
		auto cond = CBreakPoints::GetBreakPointCondition(BREAKPOINT_EE, pc);
		if (cond && !cond->Evaluate())
			return;

		if(!CBreakPoints::HandleBreakpointHit(BREAKPOINT_EE, pc))
			return;
	}

	CBreakPoints::SetBreakpointTriggered(true, BREAKPOINT_EE);
	VMManager::SetPaused(true);
	Cpu->ExitExecution();
}

void intMemcheck(u32 op, u32 bits, bool store)
{
	// compute accessed address
	u32 start = cpuRegs.GPR.r[(op >> 21) & 0x1F].UL[0];
	if (static_cast<s16>(op) != 0)
		start += static_cast<s16>(op);
	if (bits == 128)
		start &= ~0x0F;

	start = standardizeBreakpointAddress(start);
	const u32 end = start + bits/8;

	const auto checks = CBreakPoints::GetMemChecks(BREAKPOINT_EE);
	for (size_t i = 0; i < checks.size(); i++)
	{
		const auto& check = checks[i];

		if (check.result == 0)
			continue;
		if ((check.memCond & MEMCHECK_WRITE) == 0 && store)
			continue;
		if ((check.memCond & MEMCHECK_READ) == 0 && !store)
			continue;

		if (check.hasCond)
		{
			if (!check.cond.Evaluate())
				continue;
		}

		if (start < check.end && check.start < end)
			intBreakpoint(true);
	}
}

void intCheckMemcheck()
{
	const u32 pc = cpuRegs.pc;
	const int needed = isMemcheckNeeded(pc);
	if (needed == 0)
		return;

	const u32 op = memRead32(needed == 2 ? pc + 4 : pc);
	const OPCODE& opcode = GetInstruction(op);

	const bool store = (opcode.flags & IS_STORE) != 0;
	switch (opcode.flags & MEMTYPE_MASK)
	{
		case MEMTYPE_BYTE:
			intMemcheck(op, 8, store);
			break;
		case MEMTYPE_HALF:
			intMemcheck(op, 16, store);
			break;
		case MEMTYPE_WORD:
			intMemcheck(op, 32, store);
			break;
		case MEMTYPE_DWORD:
			intMemcheck(op, 64, store);
			break;
		case MEMTYPE_QWORD:
			intMemcheck(op, 128, store);
			break;
	}
}

// --------------------------------------------------------------------------------------
//  Fast interpreter mode
// --------------------------------------------------------------------------------------
// Used when the EE recompiler is requested on a host without one (ARM64). It keeps the
// interpreter's semantics but drops per-instruction overhead:
//  - instruction fetch reads straight from a cached host pointer of the current code page,
//    re-resolved only when the vtlb map changes (nothing to invalidate on code writes,
//    the words are always read from guest RAM);
//  - decoding goes through a small cache keyed by the instruction word itself, so it never
//    needs invalidation;
//  - event tests run when nextEventCycle is reached, as with the recompilers, instead of
//    after every branch;
//  - short polling loops are detected and fast-forwarded to the next event.
static bool s_fastMode = false;

static u32 s_fetchVPage = 0xFFFFFFFFu;
static u32 s_fetchGeneration = 0;
static const u8* s_fetchHost = nullptr;

struct DecodeCacheEntry
{
	u32 code;
	const R5900::OPCODE* opcode;
};
static constexpr u32 DECODE_CACHE_BITS = 16;
static DecodeCacheEntry s_decodeCache[1u << DECODE_CACHE_BITS];

static __fi const R5900::OPCODE& DecodeCached(u32 code)
{
	DecodeCacheEntry& e = s_decodeCache[(code * 0x9E3779B1u) >> (32 - DECODE_CACHE_BITS)];
	if (e.opcode && e.code == code) [[likely]]
		return *e.opcode;

	const R5900::OPCODE& op = R5900::GetInstruction(code);
	e.code = code;
	e.opcode = &op;
	return op;
}

static __fi u32 FetchFast(u32 pc)
{
	if ((pc >> 12) == s_fetchVPage && s_fetchGeneration == vtlb_MapGeneration) [[likely]]
		return *reinterpret_cast<const u32*>(s_fetchHost + (pc & 0xFFF));

	// Goes through the vtlb, so TLB misses and address errors are raised as usual.
	const u32 code = memRead32(pc);

	if (const u8* host = vtlb_GetExecPage(pc))
	{
		s_fetchVPage = pc >> 12;
		s_fetchGeneration = vtlb_MapGeneration;
		s_fetchHost = host;
	}
	else
	{
		s_fetchVPage = 0xFFFFFFFFu;
	}
	return code;
}

static void FlushFastModeCaches()
{
	s_fetchVPage = 0xFFFFFFFFu;
	std::memset(s_decodeCache, 0, sizeof(s_decodeCache));
}

// A loop that only re-reads memory and recomputes the same values each iteration can't
// make progress until something outside the EE (an interrupt, DMA, the IOP, a timer)
// changes what it reads, so the cycles until the next event can be skipped.
// Every register the loop reads must either be written earlier in the same iteration or
// never be written by the loop; otherwise it carries state (a delay counter) and the loop
// has to run for real.
static bool IsWaitLoop(u32 start, u32 end)
{
	static constexpr u32 MAX_INSNS = 10;
	u32 reads[MAX_INSNS], writes[MAX_INSNS];
	u32 count = 0;

	for (u32 a = start; a < end; a += 4)
	{
		if (count == MAX_INSNS)
			return false;
		const u8* host = vtlb_GetExecPage(a);
		if (!host)
			return false;

		const u32 code = *reinterpret_cast<const u32*>(host + (a & 0xFFF));
		const u32 op = code >> 26, rs = (code >> 21) & 31, rt = (code >> 16) & 31, rd = (code >> 11) & 31;
		u32 r = 0, w = 0;
		switch (op)
		{
			case 0: // SPECIAL
				switch (code & 63)
				{
					case 0: case 2: case 3: // SLL SRL SRA
						r = 1u << rt; w = 1u << rd; break;
					case 15: // SYNC
						break;
					case 33: case 35: case 36: case 37: case 38: case 39: case 42: case 43: // ADDU SUBU AND OR XOR NOR SLT SLTU
						r = (1u << rs) | (1u << rt); w = 1u << rd; break;
					default:
						return false;
				}
				break;
			case 1: // REGIMM
				if (rt > 3) // only BLTZ BGEZ BLTZL BGEZL
					return false;
				r = 1u << rs;
				break;
			case 4: case 5: case 20: case 21: // BEQ BNE BEQL BNEL
				r = (1u << rs) | (1u << rt); break;
			case 6: case 7: case 22: case 23: // BLEZ BGTZ BLEZL BGTZL
				r = 1u << rs; break;
			case 9: case 10: case 11: case 12: case 13: case 14: // ADDIU SLTI SLTIU ANDI ORI XORI
			case 32: case 33: case 35: case 36: case 37: case 39: case 55: // LB LH LW LBU LHU LWU LD
				r = 1u << rs; w = 1u << rt; break;
			case 15: // LUI
				w = 1u << rt; break;
			default:
				return false;
		}
		reads[count] = r & ~1u;
		writes[count] = w & ~1u;
		count++;
	}

	u32 loopWrites = 0;
	for (u32 i = 0; i < count; i++)
		loopWrites |= writes[i];

	u32 written = 0;
	for (u32 i = 0; i < count; i++)
	{
		if (reads[i] & ~written & loopWrites)
			return false;
		written |= writes[i];
	}
	return true;
}

static void execI()
{
	// execI is called for every instruction so it must remains as light as possible.
	// If you enable the next define, Interpreter will be much slower (around
	// ~4fps on 3.9GHz Haswell vs ~8fps (even 10fps on dev build))
	// Extra note: due to some cycle count issue PCSX2's internal debugger is
	// not yet usable with the interpreter
//#define EXTRA_DEBUG
#if defined(EXTRA_DEBUG) || defined(PCSX2_DEVBUILD)
	// check if any breakpoints or memchecks are triggered by this instruction
	if (isBreakpointNeeded(cpuRegs.pc))
		intBreakpoint(false);

	intCheckMemcheck();

	CBreakPoints::CommitClearSkipFirst(BREAKPOINT_EE);
#endif

	const u32 pc = cpuRegs.pc;
	if (g_mmuTrace) [[unlikely]]
		mmuTraceCheckPc(pc);
	// We need to increase the pc before executing the memRead32. An exception could appears
	// and it expects the PC counter to be pre-incremented
	cpuRegs.pc += 4;

	// interprete instruction
	cpuRegs.code = s_fastMode ? FetchFast(pc) : memRead32(pc);

	const OPCODE& opcode = s_fastMode ? DecodeCached(cpuRegs.code) : GetCurrentInstruction();
#if 0
	static long int runs = 0;
	//use this to find out what opcodes your game uses. very slow! (rama)
	runs++;
	 //leave some time to startup the testgame
	if (runs > 1599999999)
	{
		 //find all opcodes beginning with "L"
		if (opcode.Name[0] == 'L')
		{
			Console.WriteLn ("Load %s", opcode.Name);
		}
	}
#endif

#if 0
	static long int print_me = 0;
	// Based on cycle
	// if ( cpuRegs.cycle > 0x4f24d714 )
	// Or dump from a particular PC (useful to debug handler/syscall)
	if (pc == 0x80000000)
	{
		print_me = 2000;
	}
	if (print_me)
	{
		print_me--;
		disOut.clear();
		disR5900Fasm(disOut, cpuRegs.code, pc);
		CPU_LOG( disOut.c_str() );
	}
#endif


	cpuBlockCycles += opcode.cycles * (2 - ((cpuRegs.CP0.n.Config >> 18) & 0x1));

	opcode.interpret();
}

static __fi void _doBranch_shared(u32 tar)
{
	branch2 = cpuRegs.branch = 1;
	execI();

	// branch being 0 means an exception was thrown, since only the exception
	// handler should ever clear it.

	if( cpuRegs.branch != 0 )
	{
		if (Cpu == &intCpu)
		{
			if (s_fastMode)
			{
				if (intLastBranchTo == tar && EmuConfig.Speedhacks.WaitLoop && tar < cpuRegs.pc &&
					(cpuRegs.pc - tar) <= (4 * 10) && IsWaitLoop(tar, cpuRegs.pc))
				{
					intUpdateCPUCycles();
					if (static_cast<s64>(cpuRegs.nextEventCycle - cpuRegs.cycle) > 0)
						cpuRegs.cycle = cpuRegs.nextEventCycle;
				}
			}
			else if (intLastBranchTo == tar && EmuConfig.Speedhacks.WaitLoop)
			{
				intUpdateCPUCycles();
				bool can_skip = true;
				if (tar != 0x81fc0)
				{
					if ((cpuRegs.pc - tar) < (4 * 10))
					{
						for (u32 i = tar; i < cpuRegs.pc; i += 4)
						{
							if (PSM(i) != 0)
							{
								can_skip = false;
								break;
							}
						}
					}
					else
						can_skip = false;
				}

				if (can_skip)
				{
					if (static_cast<s64>(cpuRegs.nextEventCycle - cpuRegs.cycle) > 0)
						cpuRegs.cycle = cpuRegs.nextEventCycle;
					else
						cpuRegs.nextEventCycle = cpuRegs.cycle;
				}
			}
		}
		intLastBranchTo = tar;
		cpuRegs.pc = tar;
		cpuRegs.branch = 0;
	}
}

static __fi bool intEventTestDue()
{
	return !s_fastMode || static_cast<s64>(cpuRegs.cycle - cpuRegs.nextEventCycle) >= 0;
}

static void doBranch( u32 target )
{
	_doBranch_shared( target );
	intUpdateCPUCycles();
	if (intEventTestDue())
		intEventTest();
}

void intDoBranch(u32 target)
{
	//Console.WriteLn("Interpreter Branch ");
	_doBranch_shared( target );

	if( Cpu == &intCpu )
	{
		intUpdateCPUCycles();
		if (intEventTestDue())
			intEventTest();
	}
}

void intSetBranch()
{
	branch2 = /*cpuRegs.branch =*/ 1;
}

////////////////////////////////////////////////////////////////////
// R5900 Branching Instructions!
// These are the interpreter versions of the branch instructions.  Unlike other
// types of interpreter instructions which can be called safely from the recompilers,
// these instructions are not "recSafe" because they may not invoke the
// necessary branch test logic that the recs need to maintain sync with the
// cpuRegs.pc and delaySlot instruction and such.

namespace R5900 {
namespace Interpreter {
namespace OpcodeImpl {

/*********************************************************
* Jump to target                                         *
* Format:  OP target                                     *
*********************************************************/
// fixme: looking at the other branching code, shouldn't those _SetLinks in BGEZAL and such only be set
// if the condition is true? --arcum42

void J()
{
	doBranch(_JumpTarget_);
}

void JAL()
{
	// 0x3563b8 is the start address of the function that invalidate entry in TLB cache
	if (EmuConfig.Gamefixes.GoemonTlbHack) {
		if (_JumpTarget_ == 0x3563b8)
			GoemonUnloadTlb(cpuRegs.GPR.n.a0.UL[0]);
	}
	_SetLink(31);
	doBranch(_JumpTarget_);
}

/*********************************************************
* Register branch logic                                  *
* Format:  OP rs, rt, offset                             *
*********************************************************/

void BEQ()  // Branch if Rs == Rt
{
	if (cpuRegs.GPR.r[_Rs_].SD[0] == cpuRegs.GPR.r[_Rt_].SD[0])
		doBranch(_BranchTarget_);
	else
		intEventTest();
}

void BNE()  // Branch if Rs != Rt
{
	if (cpuRegs.GPR.r[_Rs_].SD[0] != cpuRegs.GPR.r[_Rt_].SD[0])
		doBranch(_BranchTarget_);
	else
		intEventTest();
}

/*********************************************************
* Register branch logic                                  *
* Format:  OP rs, offset                                 *
*********************************************************/

void BGEZ()    // Branch if Rs >= 0
{
	if(cpuRegs.GPR.r[_Rs_].SD[0] >= 0)
	{
		doBranch(_BranchTarget_);
	}
}

void BGEZAL() // Branch if Rs >= 0 and link
{
	_SetLink(31);
	if (cpuRegs.GPR.r[_Rs_].SD[0] >= 0)
	{
		doBranch(_BranchTarget_);
	}
}

void BGTZ()    // Branch if Rs >  0
{
	if (cpuRegs.GPR.r[_Rs_].SD[0] > 0)
	{
		doBranch(_BranchTarget_);
	}
}

void BLEZ()   // Branch if Rs <= 0
{
	if (cpuRegs.GPR.r[_Rs_].SD[0] <= 0)
	{
		doBranch(_BranchTarget_);
	}
}

void BLTZ()    // Branch if Rs <  0
{
	if (cpuRegs.GPR.r[_Rs_].SD[0] < 0)
	{
		doBranch(_BranchTarget_);
	}
}

void BLTZAL()  // Branch if Rs <  0 and link
{
	_SetLink(31);
	if (cpuRegs.GPR.r[_Rs_].SD[0] < 0)
	{
		doBranch(_BranchTarget_);
	}
}

/*********************************************************
* Register branch logic  Likely                          *
* Format:  OP rs, offset                                 *
*********************************************************/


void BEQL()    // Branch if Rs == Rt
{
	if(cpuRegs.GPR.r[_Rs_].SD[0] == cpuRegs.GPR.r[_Rt_].SD[0])
	{
		doBranch(_BranchTarget_);
	}
	else
	{
		cpuRegs.pc +=4;
		intEventTest();
	}
}

void BNEL()     // Branch if Rs != Rt
{
	if(cpuRegs.GPR.r[_Rs_].SD[0] != cpuRegs.GPR.r[_Rt_].SD[0])
	{
		doBranch(_BranchTarget_);
	}
	else
	{
		cpuRegs.pc +=4;
		intEventTest();
	}
}

void BLEZL()    // Branch if Rs <= 0
{
	if(cpuRegs.GPR.r[_Rs_].SD[0] <= 0)
	{
		doBranch(_BranchTarget_);
	}
	else
	{
		cpuRegs.pc +=4;
		intEventTest();
	}
}

void BGTZL()     // Branch if Rs >  0
{
	if(cpuRegs.GPR.r[_Rs_].SD[0] > 0)
	{
		doBranch(_BranchTarget_);
	}
	else
	{
		cpuRegs.pc +=4;
		intEventTest();
	}
}

void BLTZL()     // Branch if Rs <  0
{
	if(cpuRegs.GPR.r[_Rs_].SD[0] < 0)
	{
		doBranch(_BranchTarget_);
	}
	else
	{
		cpuRegs.pc +=4;
		intEventTest();
	}
}

void BGEZL()     // Branch if Rs >= 0
{
	if(cpuRegs.GPR.r[_Rs_].SD[0] >= 0)
	{
		doBranch(_BranchTarget_);
	}
	else
	{
		cpuRegs.pc +=4;
		intEventTest();
	}
}

void BLTZALL()   // Branch if Rs <  0 and link
{
	_SetLink(31);
	if(cpuRegs.GPR.r[_Rs_].SD[0] < 0)
	{
		doBranch(_BranchTarget_);
	}
	else
	{
		cpuRegs.pc +=4;
		intEventTest();
	}
}

void BGEZALL()   // Branch if Rs >= 0 and link
{
	_SetLink(31);
	if(cpuRegs.GPR.r[_Rs_].SD[0] >= 0)
	{
		doBranch(_BranchTarget_);
	}
	else
	{
		cpuRegs.pc +=4;
		intEventTest();
	}
}

/*********************************************************
* Register jump                                          *
* Format:  OP rs, rd                                     *
*********************************************************/
void JR()
{
	// 0x33ad48 and 0x35060c are the return address of the function (0x356250) that populate the TLB cache
	if (EmuConfig.Gamefixes.GoemonTlbHack) {
		const u32 add = cpuRegs.GPR.r[_Rs_].UL[0];
		if (add == 0x33ad48 || add == 0x35060c)
			GoemonPreloadTlb();
	}
	doBranch(cpuRegs.GPR.r[_Rs_].UL[0]);
}

void JALR()
{
	const u32 temp = cpuRegs.GPR.r[_Rs_].UL[0];

	if (_Rd_)  _SetLink(_Rd_);

	doBranch(temp);
}

} } }		// end namespace R5900::Interpreter::OpcodeImpl


// --------------------------------------------------------------------------------------
//  R5900cpu/intCpu interface (implementations)
// --------------------------------------------------------------------------------------

static void intReserve()
{
	// fixme : detect cpu for use the optimize asm code
}

static void intReset()
{
	cpuRegs.branch = 0;
	branch2 = 0;

#ifndef _M_X86
	// No EE recompiler on this host: "recompiler" selects the fast interpreter.
	s_fastMode = CHECK_EEREC;
#endif
	FlushFastModeCaches();
}

void intEventTest()
{
	// Perform counters, ints, and IOP updates:
	_cpuEventTest_Shared();

	if (intExitExecution)
	{
		intExitExecution = false;
		if (CHECK_EEREC)
			writebackCache();
		fastjmp_jmp(&intJmpBuf, 1);
	}
}

static void intSafeExitExecution()
{
	// If we're currently processing events, we can't safely jump out of the interpreter here, because we'll
	// leave things in an inconsistent state. So instead, we flag it for exiting once cpuEventTest() returns.
	if (eeEventTestIsActive)
		intExitExecution = true;
	else
	{
		if (CHECK_EEREC)
			writebackCache();
		fastjmp_jmp(&intJmpBuf, 1);
	}
}

static void intCancelInstruction()
{
	// See execute function.
	fastjmp_jmp(&intJmpBuf, 0);
}

static void intExecute()
{
	// This will come back as zero the first time it runs, or on instruction cancel.
	// It will come back as nonzero when we exit execution.
	if (fastjmp_set(&intJmpBuf) != 0)
		return;

	for (;;)
	{
		if (!VMManager::Internal::HasBootedELF())
		{
			// Avoid reloading every instruction.
			u32 elf_entry_point = VMManager::Internal::GetCurrentELFEntryPoint();
			u32 eeload_main = g_eeloadMain;
			u32 eeload_exec = g_eeloadExec;

			while (true)
			{
				execI();

				if (cpuRegs.pc == EELOAD_START)
				{
					// The EELOAD _start function is the same across all BIOS versions afaik
					const u32 mainjump = memRead32(EELOAD_START + 0x9c);
					if (mainjump >> 26 == 3) // JAL
						g_eeloadMain = ((EELOAD_START + 0xa0) & 0xf0000000U) | (mainjump << 2 & 0x0fffffffU);

					eeload_main = g_eeloadMain;
				}
				else if (cpuRegs.pc == eeload_main)
				{
					eeloadHook();
					if (VMManager::Internal::IsFastBootInProgress())
					{
						// See comments on this code in iR5900.cpp's recRecompile()
						const u32 typeAexecjump = memRead32(EELOAD_START + 0x470);
						const u32 typeBexecjump = memRead32(EELOAD_START + 0x5B0);
						const u32 typeCexecjump = memRead32(EELOAD_START + 0x618);
						const u32 typeDexecjump = memRead32(EELOAD_START + 0x600);
						if ((typeBexecjump >> 26 == 3) || (typeCexecjump >> 26 == 3) || (typeDexecjump >> 26 == 3)) // JAL to 0x822B8
							g_eeloadExec = EELOAD_START + 0x2B8;
						else if (typeAexecjump >> 26 == 3) // JAL to 0x82170
							g_eeloadExec = EELOAD_START + 0x170;
						else
							Console.WriteLn("intExecute: Could not enable launch arguments for fast boot mode; unidentified BIOS version! Please report this to the PCSX2 developers.");

						eeload_exec = g_eeloadExec;
					}

					elf_entry_point = VMManager::Internal::GetCurrentELFEntryPoint();
				}
				else if (cpuRegs.pc == eeload_exec)
				{
					eeloadHook2();
				}
				else if (cpuRegs.pc == elf_entry_point)
				{
					VMManager::Internal::EntryPointCompilingOnCPUThread();
					break;
				}
			}
		}
		else
		{
			while (true)
				execI();
		}
	}
}

static void intStep()
{
	execI();
}

static void intClear(u32 Addr, u32 Size)
{
}

static void intShutdown() {
}

R5900cpu intCpu =
{
	intReserve,
	intShutdown,

	intReset,
	intStep,
	intExecute,

	intSafeExitExecution,
	intCancelInstruction,

	intClear
};
