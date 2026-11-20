// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

// Guest-managed MMU (PS2 Linux) diagnostics, enabled by the PCSX2_MMU_TRACE=<file>
// environment variable. Keeps a ring buffer of EE exceptions and TLB operations;
// when the guest kernel enters force_sig_info with SIGSEGV/SIGBUS (address from
// PCSX2_MMU_TRACE_SIGPC, default 0x80036f68), the buffer, registers, TLB and a
// vtlb vs TLB consistency check are appended to the file.

extern bool g_mmuTrace;

enum MmuTraceType : u32
{
	MMUT_EXC = 1,   // a=Cause b=EPC c=BadVAddr d=Status e=EntryHi
	MMUT_TLBW = 2,  // a=index b=EntryHi c=Lo0 d=Lo1 e=old EntryHi
	MMUT_ASID = 3,  // a=old ASID b=new ASID c=pc
	MMUT_ERL = 4,   // a=ERL b=EntryHi c=pc
	MMUT_ERET = 5,  // a=target pc b=Status c=EntryHi
};

extern void mmuTraceInit();
extern void mmuTraceEvent(u32 type, u32 a, u32 b, u32 c, u32 d, u32 e);
extern void mmuTraceCheckPc(u32 pc);
