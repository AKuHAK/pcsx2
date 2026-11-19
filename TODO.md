# EE Recompiler Guest-MMU Performance TODO

Performance bottlenecks observed when running guest-managed MMU OSes (PS2 Linux / PSBBN) under the EE Recompiler (`g_eeMmuGuestActive`), causing periodic stutters and audio underruns:

- [ ] **Move cold exception-state flush in `DynGen_HandlerTest` into a shared stub** (`pcsx2/x86/ix86-32/recVTLB.cpp`)
  - Currently, when `g_eeMmuGuestActive` is enabled, every load/store inlines the full register/XMM/const writeback and PC/cycle update in the cold handler branch.
  - Extracting this into a shared stub or trampoline will drastically shrink emitted x86 code size and reduce frequent `EE/iR5900 Recompiler Reset` cache overflows.

- [ ] **Optimize reads from `EntryLo.D == 0` (write-protected) virtual pages** (`pcsx2/vtlb.cpp`, `pcsx2/COP0.cpp`)
  - Clean/read-only/CoW pages mapped with `EntryLo.D == 0` currently go through `WriteProtectedVirtHandler` in `vtlbdata.vmap`, forcing all data reads from `.text`/`.rodata`/CoW pages through a C++ handler instead of direct host memory access.
  - Consider separate read/write vTLB mapping or fastmem/read-only host page protection so reads execute directly without C++ handler overhead.

- [ ] **Reduce `recClear` / `BaseBlocks::Remove` overhead and fix `mmap_ClearCpuBlock` clear size** (`pcsx2/x86/ix86-32/iR5900.cpp`, `pcsx2/x86/BaseblockEx.h`, `pcsx2/vtlb.cpp`)
  - `s_physPageVAddr[p_page] != v_page` triggers `recClear(p_page << 12, 0x400)` whenever shared physical pages (e.g. shared libraries across Linux processes) are executed at different virtual addresses.
  - `BaseBlocks::Remove` scans the entire `links` multimap per removal batch, and `recClear` performs a linear verification loop over `recBlocks`. Consider keying virtual-page blocks by `(v_page, p_page)` or optimizing `recClear`/`links` invalidation.
  - In `mmap_ClearCpuBlock` (`pcsx2/vtlb.cpp`), `Cpu->Clear(m_PageProtectInfo[rampage].ReverseRamMap, __pagesize)` passes `__pagesize` (4096 bytes) instead of `__pagesize / 4` (1024 dwords), clearing 16 KiB of blocks per 4 KiB page fault.

- [ ] **Speed up cross-page `kuseg` dispatch (`UnmappedRecLUTPage` / `recDispatchVirtual`)** (`pcsx2/x86/ix86-32/iR5900.cpp`)
  - Cross-page calls and indirect jumps in `kuseg` currently fall back to `xFastCall(recDispatchVirtual)` and `vtlb_LookupExecPAddr`.
  - Add a fast assembly lookup path (e.g. direct `s_execPAddr` + `recRAM` probe in emitted x86) before falling back to C++ `recDispatchVirtual`.

- [ ] **Re-enable dead-register elimination safely when guest MMU is active** (`pcsx2/x86/iCore.h`)
  - `EEINST_LIVETEST` and `FPUINST_LIVETEST` currently force all values live when `g_eeMmuGuestActive` is true, increasing register spills and code bloat. Restrict forced liveness only to instructions that can actually trap or where exception state requires it.
