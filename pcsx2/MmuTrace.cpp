// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Common.h"
#include "MmuTrace.h"
#include "COP0.h"
#include "R5900.h"
#include "vtlb.h"

#include <cstdio>
#include <cstdlib>

bool g_mmuTrace = false;

namespace
{
	struct Event
	{
		u32 cycle;
		u32 type;
		u32 a, b, c, d, e;
	};

	constexpr u32 RING = 8192;
	Event s_ring[RING];
	u32 s_pos = 0;
	u32 s_count = 0;
	u32 s_sigPc = 0x80036f68;
	u32 s_dumps = 0;
	const char* s_path = nullptr;

	// Physical address of vaddr per the current TLB (installed ASID and global
	// entries); false if there is no valid entry.
	bool expectedPAddr(u32 vaddr, u32 asid, u32* paddr, bool* dirty)
	{
		for (int i = 0; i < 48; i++)
		{
			const tlbs& t = tlb[i];
			if (t.isSPR())
				continue;
			if (!t.isGlobal() && t.EntryHi.ASID != asid)
				continue;
			const u32 pageSize = (t.Mask() + 1) << 12;
			const u32 base = t.VPN2();
			if (vaddr < base || (vaddr - base) >= pageSize * 2)
				continue;
			const bool odd = (vaddr - base) >= pageSize;
			const EntryLo_t& lo = odd ? t.EntryLo1 : t.EntryLo0;
			if (!lo.V)
				return false;
			*paddr = (odd ? t.PFN1() : t.PFN0()) + ((vaddr - base) & (pageSize - 1));
			*dirty = lo.D;
			return true;
		}
		return false;
	}

	const char* typeName(u32 t)
	{
		switch (t)
		{
			case MMUT_EXC: return "EXC ";
			case MMUT_TLBW: return "TLBW";
			case MMUT_ASID: return "ASID";
			case MMUT_ERL: return "ERL ";
			case MMUT_ERET: return "ERET";
		}
		return "????";
	}

	void dump(u32 pc)
	{
		std::FILE* f = std::fopen(s_path, "a");
		if (!f)
			return;

		const u32 asid = cop0GetInstalledASID();
		std::fprintf(f, "\n===== force_sig_info sig=%u pc=%08x cycle=%u dump #%u\n", cpuRegs.GPR.n.a0.UL[0], pc,
			static_cast<u32>(cpuRegs.cycle), s_dumps);
		std::fprintf(f, "EntryHi=%08x installedASID=%02x Status=%08x Cause=%08x EPC=%08x BadVAddr=%08x guestMmu=%d\n",
			cpuRegs.CP0.n.EntryHi, asid, cpuRegs.CP0.n.Status.val, cpuRegs.CP0.n.Cause, cpuRegs.CP0.n.EPC,
			cpuRegs.CP0.n.BadVAddr, g_eeMmuGuestActive ? 1 : 0);

		const u32 n = s_count < RING ? s_count : RING;
		std::fprintf(f, "--- last %u events (oldest first)\n", n);
		for (u32 k = 0; k < n; k++)
		{
			const Event& ev = s_ring[(s_pos + RING - n + k) % RING];
			std::fprintf(f, "%10u %s %08x %08x %08x %08x %08x\n", ev.cycle, typeName(ev.type), ev.a, ev.b, ev.c, ev.d, ev.e);
		}

		std::fprintf(f, "--- TLB\n");
		for (int i = 0; i < 48; i++)
		{
			const tlbs& t = tlb[i];
			std::fprintf(f, "%2d Hi=%08x Lo0=%08x Lo1=%08x Mask=%08x %s\n", i, t.EntryHi.UL, t.EntryLo0.UL, t.EntryLo1.UL,
				t.PageMask.UL, (t.isGlobal() || t.EntryHi.ASID == asid) ? "*" : "");
		}

		// Every mapped kuseg page must match the TLB.
		std::fprintf(f, "--- vtlb vs TLB (kuseg)\n");
		u32 bad = 0;
		for (u32 page = 0; page < 0x80000; page++)
		{
			const u32 va = page << 12;
			u32 got = 0;
			const bool mapped = vtlb_LookupExecPAddr(va, &got);
			u32 exp = 0;
			bool dirty = false;
			const bool want = expectedPAddr(va, asid, &exp, &dirty);
			if (mapped == want && (!mapped || got == exp))
				continue;
			if (bad++ < 64)
				std::fprintf(f, "va=%08x vtlb=%s%08x tlb=%s%08x\n", va, mapped ? "" : "none:", got, want ? "" : "none:", exp);
		}
		std::fprintf(f, "mismatches: %u\n", bad);
		std::fclose(f);
	}
} // namespace

void mmuTraceInit()
{
	s_path = std::getenv("PCSX2_MMU_TRACE");
	g_mmuTrace = (s_path && *s_path);
	if (const char* pcs = std::getenv("PCSX2_MMU_TRACE_SIGPC"))
		s_sigPc = static_cast<u32>(std::strtoul(pcs, nullptr, 16));
}

void mmuTraceEvent(u32 type, u32 a, u32 b, u32 c, u32 d, u32 e)
{
	Event& ev = s_ring[s_pos];
	ev.cycle = static_cast<u32>(cpuRegs.cycle);
	ev.type = type;
	ev.a = a;
	ev.b = b;
	ev.c = c;
	ev.d = d;
	ev.e = e;
	s_pos = (s_pos + 1) % RING;
	s_count++;
}

void mmuTraceCheckPc(u32 pc)
{
	if (pc != s_sigPc)
		return;
	const u32 sig = cpuRegs.GPR.n.a0.UL[0];
	if (sig != 11 && sig != 10)
		return;
	if (s_dumps >= 10)
		return;
	s_dumps++;
	dump(pc);
}
