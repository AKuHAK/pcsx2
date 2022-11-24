/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2021  PCSX2 Dev Team
 *
 *  PCSX2 is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU Lesser General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  PCSX2 is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with PCSX2.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#include "PrecompiledHeader.h"
#include "R3000A.h"
#include "Common.h"
#include "IopHw.h"
#include "IopDma.h"

#include <cctype>
#include <ctime>
#include <memory>

#include "common/FileSystem.h"
#include "common/Path.h"
#include "common/StringUtil.h"
#include "common/Threading.h"

#include "Ps1CD.h"
#include "CDVD.h"
#include "CDVD_internal.h"
#include "IsoFileFormats.h"

#include "GS.h" // for gsVideoMode
#include "Elfheader.h"
#include "ps2/BiosTools.h"
#include "des.h"

#ifndef DISABLE_RECORDING
#include "Recording/InputRecording.h"
#endif

#ifndef PCSX2_CORE
#include "gui/AppCoreThread.h"
#include "gui/SysThreads.h"
u32 getCdvdOffset()
{
	return GameInfo::cdvdOffset;
}
#else
#include "VMManager.h"
u32 getCdvdOffset()
{
	return VMManager::GetCdvdOffset();
}
#endif

// This typically reflects the Sony-assigned serial code for the Disc, if one exists.
//  (examples:  SLUS-2113, etc).
// If the disc is homebrew then it probably won't have a valid serial; in which case
// this string will be empty.
std::string DiscSerial;

cdvdStruct cdvd;

s64 PSXCLK = 36864000;

std::string IlinkIdPath;

u8 monthmap[13] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

#pragma pack(push, 1)
struct KeyStore
{
	uint8_t CardKeyLow[3][8];
	uint8_t CardKeyHi[3][8];
	uint8_t CardKey2Low[3][8];
	uint8_t CardKey2Hi[3][8];
	uint8_t CardIV[3][8];
	uint8_t CardIV2[3][8];
	uint8_t KbitMasterKey[16];
	uint8_t KcMasterKey[16];
	uint8_t KbitIv[8];
	uint8_t KcIv[8];
	uint8_t icvps2LowKey[16];
	uint8_t icvps2HiKey[16];
	uint8_t icvps2LowIV[8];
	uint8_t icvps2HiIV[8];
	uint8_t SignatureMasterKey[8];
	uint8_t SignatureHashKey[8];
	uint8_t RootSigHashKey[16];
	uint8_t RootSigMasterKey[8];
	uint8_t ContentIV[8];
	uint8_t ContentTableIV[8];
	uint8_t ChallengeIV[8];
};
#pragma pack(pop)
KeyStore g_keyStore;

uint16_t g_EncryptedKeyStore[0x200] = {
	/* SHA256: 04bcc6b13827829fb5cc8dbd86420d30f69a2bfd3b7719398b341e15368bd365 */
};

uint32_t ks_index = 0;

class ks_reg
{
	uint16_t value;

public:
	ks_reg() {}

	operator uint16_t() { return g_EncryptedKeyStore[ks_index++]; }
};
ks_reg ks_data;

uint16_t g_MemoryCardKeyIndexes[72] = {
	0x0018, 0xFFFF, 0xFFFF, 0x001C, 0xFFFF, 0xFFFF, 0x0020, 0xFFFF, 0xFFFF, 0x0024, 0xFFFF, 0xFFFF, 0x0028, 0xFFFF, 0xFFFF, 0x002C, 0xFFFF, 0xFFFF,
	0x0030, 0x0048, 0x0060, 0x0034, 0x004C, 0x0064, 0x0038, 0x0050, 0x0068, 0x003C, 0x0054, 0x006C, 0x0040, 0x0058, 0x0070, 0x0044, 0x005C, 0x0074,
	0x0000, 0x1000, 0x1001, 0x0004, 0x1002, 0x1003, 0x0008, 0x1004, 0x1005, 0x000C, 0x1006, 0x1007, 0x0010, 0x1008, 0x1009, 0x0014, 0x100A, 0x100B,
	0x0090, 0x00A8, 0x00A8, 0x0094, 0x00AC, 0x00AC, 0x0098, 0x00B0, 0x00B0, 0x009C, 0x00B4, 0x00B4, 0x00A0, 0x00B8, 0x00B8, 0x00A4, 0x00BC, 0x00BC};

uint16_t g_KelfKeysIndex[4] = {0x110, 0x110, 0xC4, 0x15C};

uint16_t g_cardKeyStore[48] = {
	/* SHA256: 04bcc6b13827829fb5cc8dbd86420d30f69a2bfd3b7719398b341e15368bd365 */
};

uint8_t g_KeyStoreKey[16] = {/* SHA256: 04bcc6b13827829fb5cc8dbd86420d30f69a2bfd3b7719398b341e15368bd365 */};

uint8_t MG_IV_NULL[8] = {0};

u8 cdvdParamLength[16] = {0, 0, 0, 0, 0, 4, 11, 11, 11, 1, 255, 255, 7, 2, 11, 1};

u8 temp_mechaver[4] = {0, 0, 0, 0};

static __fi void SetSCMDResultSize(u8 size)
{
	cdvd.SCMDResultC = size;
	cdvd.SCMDResultP = 0;
	cdvd.sDataIn &= ~0x40;
}

static void CDVDSECTORREADY_INT(u32 eCycle)
{
	if (psxRegs.interrupt & (1 << IopEvt_CdvdSectorReady))
		return;

	if (EmuConfig.Speedhacks.fastCDVD)
	{
		if (eCycle < Cdvd_FullSeek_Cycles && eCycle > 1)
			eCycle *= 0.5f;
	}

	PSX_INT(IopEvt_CdvdSectorReady, eCycle);
}

static void CDVDREAD_INT(u32 eCycle)
{
	// Give it an arbitary FAST value. Good for ~5000kb/s in ULE when copying a file from CDVD to HDD
	// Keep long seeks out though, as games may try to push dmas while seeking. (Tales of the Abyss)
	if (EmuConfig.Speedhacks.fastCDVD)
	{
		if (eCycle < Cdvd_FullSeek_Cycles && eCycle > 1)
			eCycle *= 0.5f;
	}

	PSX_INT(IopEvt_CdvdRead, eCycle);
}

static void CDVD_INT(int eCycle)
{
	if (eCycle == 0)
		cdvdActionInterrupt();
	else
		PSX_INT(IopEvt_Cdvd, eCycle);
}

// Sets the cdvd IRQ and the reason for the IRQ, and signals the IOP for a branch
// test (which will cause the exception to be handled).
static void cdvdSetIrq(uint id = (1 << Irq_CommandComplete))
{
	cdvd.IntrStat |= id;
	cdvd.AbortRequested = false;
	iopIntcIrq(2);
	psxSetNextBranchDelta(20);
}

static int mg_BIToffset(u8* buffer)
{
	int i, ofs = 0x20;
	for (i = 0; i < *(u16*)&buffer[0x1A]; i++)
		ofs += 0x10;

	if (*(u16*)&buffer[0x18] & 1)
		ofs += buffer[ofs];
	if ((*(u16*)&buffer[0x18] & 0xF000) == 0)
		ofs += 8;

	return ofs + 0x20;
}

static void cdvdGetMechaVer(u8* ver)
{
	std::string mecfile(Path::ReplaceExtension(BiosPath, "mec"));
	auto fp = FileSystem::OpenManagedCFile(mecfile.c_str(), "rb");
	if (!fp || FileSystem::FSize64(fp.get()) < 4)
	{
		mecfile = Path::ReplaceExtension(BiosPath, "MEC");
		fp.reset();
		fp = FileSystem::OpenManagedCFile(mecfile.c_str(), "rb");
		if (!fp || FileSystem::FSize64(fp.get()) < 4)
		{
			Console.Warning("MEC File Not Found, creating substitute...");

			fp.reset();
			fp = FileSystem::OpenManagedCFile(mecfile.c_str(), "w+b");
			if (!fp)
			{
				Console.Error("Failed to read/write NVM/MEC file. Check your BIOS setup/permission settings.");
				return;
			}

			u8 version[4] = {0x3, 0x6, 0x2, 0x0};
			std::fwrite(version, sizeof(version), 1, fp.get());
			FileSystem::FSeek64(fp.get(), 0, SEEK_SET);
		}
	}

	auto ret = std::fread(ver, 1, 4, fp.get());
	if (ret != 4)
		Console.Error("Failed to read from %s. Did only %zu/4 bytes", mecfile.c_str(), ret);
}

NVMLayout* getNvmLayout()
{
	NVMLayout* nvmLayout = NULL;

	if (nvmlayouts[1].biosVer <= BiosVersion)
		nvmLayout = &nvmlayouts[1];
	else
		nvmLayout = &nvmlayouts[0];

	return nvmLayout;
}

static void cdvdCreateNewNVM(std::FILE* fp)
{
	u8 zero[1024] = {};
	std::fwrite(zero, sizeof(zero), 1, fp);

	// Write NVM ILink area with dummy data (Age of Empires 2)
	// Also write language data defaulting to English (Guitar Hero 2)

	NVMLayout* nvmLayout = getNvmLayout();
	u8 ILinkID_Data[8] = {0x00, 0xAC, 0xFF, 0xFF, 0xFF, 0xFF, 0xB9, 0x86};

	std::fseek(fp, *(s32*)(((u8*)nvmLayout) + offsetof(NVMLayout, ilinkId)), SEEK_SET);
	std::fwrite(ILinkID_Data, sizeof(ILinkID_Data), 1, fp);

	u8 biosLanguage[16];
	memcpy(biosLanguage, &biosLangDefaults[BiosRegion][0], 16);
	// Config sections first 16 bytes are generally blank expect the last byte which is PS1 mode stuff
	// So let's ignore that and just write the PS2 mode stuff
	std::fseek(fp, *(s32*)(((u8*)nvmLayout) + offsetof(NVMLayout, config1)) + 0x10, SEEK_SET);
	std::fwrite(biosLanguage, sizeof(biosLanguage), 1, fp);

	if (BiosVersion >= 0x200)
	{
		char regs[] = {'J', 'J', 'J', 'A', 'E', 'J', 'J', 'C'};
		std::fseek(fp, *(s32*)(((u8*)nvmLayout) + offsetof(NVMLayout, regparams)) + 6, SEEK_SET);
		std::fputc(regs[BiosRegion], fp);
	}
}

static void cdvdNVM(u8* buffer, int offset, size_t bytes, bool read)
{
	std::string nvmfile(Path::ReplaceExtension(BiosPath, "nvm"));
	auto fp = FileSystem::OpenManagedCFile(nvmfile.c_str(), "r+b");
	if (!fp || FileSystem::FSize64(fp.get()) < 1024)
	{
		nvmfile = Path::ReplaceExtension(BiosPath, "NVM");
		fp.reset();
		fp = FileSystem::OpenManagedCFile(nvmfile.c_str(), "r+b");
		if (!fp || FileSystem::FSize64(fp.get()) < 1024)
		{
			fp.reset();
			fp = FileSystem::OpenManagedCFile(nvmfile.c_str(), "w+b");
			if (!fp)
			{
				Console.Error("Failed to open NVM file '%s' for writing", nvmfile.c_str());
				if (read)
					std::memset(buffer, 0, bytes);
				return;
			}
			cdvdCreateNewNVM(fp.get());
		}
	}
	else
	{
		u8 LanguageParams[16];
		u8 RegParams[12];
		u8 zero[16] = {0};
		NVMLayout* nvmLayout = getNvmLayout();

		if (std::fseek(fp.get(), *(s32*)(((u8*)nvmLayout) + offsetof(NVMLayout, config1)) + 0x10, SEEK_SET) != 0 ||
			std::fread(LanguageParams, 16, 1, fp.get()) != 1 ||
			std::memcmp(LanguageParams, zero, sizeof(LanguageParams)) == 0 ||
			(BiosVersion >= 0x200 &&
				(std::fseek(fp.get(), *(s32*)(((u8*)nvmLayout) + offsetof(NVMLayout, regparams)), SEEK_SET) != 0 ||
					std::fread(RegParams, 12, 1, fp.get()) != 1 ||
					std::memcmp(RegParams, zero, sizeof(RegParams)) == 0)))
		{
			Console.Warning("Language Parameters missing, filling in defaults");

			FileSystem::FSeek64(fp.get(), 0, SEEK_SET);
			cdvdCreateNewNVM(fp.get());
		}
	}

	auto fpIlink = FileSystem::OpenManagedCFile(IlinkIdPath.c_str(), "rb");
	if (fpIlink && FileSystem::FSize64(fpIlink.get()) >= 8)
	{
		u8 ILinkID_Data[8] = {0x00, 0xAC, 0xFF, 0xFF, 0xFF, 0xFF, 0xB9, 0x86};

		if (FileSystem::FSize64(fpIlink.get()) == 0x400)
		{
			// NVM dump given for ILINK ID
			// MAME ROMs will only have the NVM dump available so this is required for compatibility.
			std::fseek(fpIlink.get(), 0x1e0, SEEK_SET);
			std::fread(ILinkID_Data, 1, 8, fpIlink.get());
			Console.WriteLn("ILINK ID MAME type");
		}
		else if (FileSystem::FSize64(fpIlink.get()) == 8)
		{
			std::fread(ILinkID_Data, 1, 8, fpIlink.get());
			Console.WriteLn("ILINK ID OLD type");
		}

		NVMLayout* nvmLayout = getNvmLayout();
		std::fseek(fp.get(), *(s32*)(((u8*)nvmLayout) + offsetof(NVMLayout, ilinkId)), SEEK_SET);
		std::fwrite(ILinkID_Data, sizeof(ILinkID_Data), 1, fp.get());
	}

	std::fseek(fp.get(), offset, SEEK_SET);

	size_t ret;
	if (read)
		ret = std::fread(buffer, 1, bytes, fp.get());
	else
		ret = std::fwrite(buffer, 1, bytes, fp.get());

	if (ret != bytes)
		Console.Error("Failed to %s %s. Did only %zu/%zu bytes",
					  read ? "read from" : "write to", nvmfile.c_str(), ret, bytes);
}

static void cdvdReadNVM(u8* dst, int offset, int bytes)
{
	cdvdNVM(dst, offset, bytes, true);
}

static void cdvdWriteNVM(const u8* src, int offset, int bytes)
{
	cdvdNVM(const_cast<u8*>(src), offset, bytes, false);
}

void getNvmData(u8* buffer, s32 offset, s32 size, s32 fmtOffset)
{
	// find the correct bios version
	NVMLayout* nvmLayout = getNvmLayout();

	// get data from eeprom
	cdvdReadNVM(buffer, *(s32*)(((u8*)nvmLayout) + fmtOffset) + offset, size);
}

void setNvmData(const u8* buffer, s32 offset, s32 size, s32 fmtOffset)
{
	// find the correct bios version
	NVMLayout* nvmLayout = getNvmLayout();

	// set data in eeprom
	cdvdWriteNVM(buffer, *(s32*)(((u8*)nvmLayout) + fmtOffset) + offset, size);
}

static void cdvdReadConsoleID(u8* id)
{
	getNvmData(id, 0, 8, offsetof(NVMLayout, consoleId));
}
static void cdvdWriteConsoleID(const u8* id)
{
	setNvmData(id, 0, 8, offsetof(NVMLayout, consoleId));
}

static void cdvdReadILinkID(u8* id)
{
	getNvmData(id, 0, 8, offsetof(NVMLayout, ilinkId));
}
static void cdvdWriteILinkID(const u8* id)
{
	setNvmData(id, 0, 8, offsetof(NVMLayout, ilinkId));
}

static void cdvdReadModelNumber(u8* num, s32 part)
{
	getNvmData(num, part, 8, offsetof(NVMLayout, modelNum));
}
static void cdvdWriteModelNumber(const u8* num, s32 part)
{
	setNvmData(num, part, 8, offsetof(NVMLayout, modelNum));
}

static void cdvdReadRegionParams(u8* num)
{
	getNvmData(num, 0, 8, offsetof(NVMLayout, regparams));
}
static void cdvdWriteRegionParams(const u8* num)
{
	setNvmData(num, 0, 8, offsetof(NVMLayout, regparams));
}

static void cdvdReadMAC(u8* num)
{
	getNvmData(num, 0, 8, offsetof(NVMLayout, mac));
}
static void cdvdWriteMAC(const u8* num)
{
	setNvmData(num, 0, 8, offsetof(NVMLayout, mac));
}

void cdvdReadLanguageParams(u8* config)
{
	getNvmData(config, 0xF, 16, offsetof(NVMLayout, config1));
}

s32 cdvdReadConfig(u8* config)
{
	// make sure its in read mode
	if (cdvd.CReadWrite != 0)
	{
		config[0] = 0x80;
		memset(&config[1], 0x00, 15);
		return 1;
	}
	// check if block index is in bounds
	else if (cdvd.CBlockIndex >= cdvd.CNumBlocks)
		return 1;
	else if (
		((cdvd.COffset == 0) && (cdvd.CBlockIndex >= 4)) ||
		((cdvd.COffset == 1) && (cdvd.CBlockIndex >= 2)) ||
		((cdvd.COffset == 2) && (cdvd.CBlockIndex >= 7)))
	{
		memset(config, 0, 16);
		return 0;
	}

	// get config data
	switch (cdvd.COffset)
	{
		case 0:
			getNvmData(config, (cdvd.CBlockIndex++) * 16, 16, offsetof(NVMLayout, config0));
			break;
		case 2:
			getNvmData(config, (cdvd.CBlockIndex++) * 16, 16, offsetof(NVMLayout, config2));
			break;
		default:
			getNvmData(config, (cdvd.CBlockIndex++) * 16, 16, offsetof(NVMLayout, config1));
	}
	return 0;
}
s32 cdvdWriteConfig(const u8* config)
{
	// make sure its in write mode && the block index is in bounds
	if ((cdvd.CReadWrite != 1) || (cdvd.CBlockIndex >= cdvd.CNumBlocks))
		return 1;
	else if (
		((cdvd.COffset == 0) && (cdvd.CBlockIndex >= 4)) ||
		((cdvd.COffset == 1) && (cdvd.CBlockIndex >= 2)) ||
		((cdvd.COffset == 2) && (cdvd.CBlockIndex >= 7)))
		return 0;

	// get config data
	switch (cdvd.COffset)
	{
		case 0:
			setNvmData(config, (cdvd.CBlockIndex++) * 16, 16, offsetof(NVMLayout, config0));
			break;
		case 2:
			setNvmData(config, (cdvd.CBlockIndex++) * 16, 16, offsetof(NVMLayout, config2));
			break;
		default:
			setNvmData(config, (cdvd.CBlockIndex++) * 16, 16, offsetof(NVMLayout, config1));
	}
	return 0;
}

// Sets ElfCRC to the CRC of the game bound to the CDVD source.
static __fi ElfObject* loadElf(std::string filename, bool isPSXElf)
{
	if (StringUtil::StartsWith(filename, "host:"))
	{
		std::string host_filename(filename.substr(5));
		s64 host_size = FileSystem::GetPathFileSize(host_filename.c_str());
		return new ElfObject(std::move(host_filename), static_cast<u32>(std::max<s64>(host_size, 0)), isPSXElf);
	}

	// Mimic PS2 behavior!
	// Much trial-and-error with changing the ISOFS and BOOT2 contents of an image have shown that
	// the PS2 BIOS performs the peculiar task of *ignoring* the version info from the parsed BOOT2
	// filename *and* the ISOFS, when loading the game's ELF image.  What this means is:
	//
	//   1. a valid PS2 ELF can have any version (ISOFS), and the version need not match the one in SYSTEM.CNF.
	//   2. the version info on the file in the BOOT2 parameter of SYSTEM.CNF can be missing, 10 chars long,
	//      or anything else.  Its all ignored.
	//   3. Games loading their own files do *not* exhibit this behavior; likely due to using newer IOP modules
	//      or lower level filesystem APIs (fortunately that doesn't affect us).
	//
	// FIXME: Properly mimicing this behavior is troublesome since we need to add support for "ignoring"
	// version information when doing file searches.  I'll add this later.  For now, assuming a ;1 should
	// be sufficient (no known games have their ELF binary as anything but version ;1)
	const std::string::size_type semi_pos = filename.rfind(';');
	if (semi_pos != std::string::npos && std::string_view(filename).substr(semi_pos) != ";1")
	{
		Console.WriteLn(Color_Blue, "(LoadELF) Non-conforming version suffix (%s) detected and replaced.", filename.c_str());
		filename.erase(semi_pos);
		filename += ";1";
	}

	IsoFSCDVD isofs;
	IsoFile file(isofs, filename);
	return new ElfObject(std::move(filename), file, isPSXElf);
}

static __fi void _reloadElfInfo(std::string elfpath)
{
	// Now's a good time to reload the ELF info...
	if (elfpath == LastELF)
		return;

	std::unique_ptr<ElfObject> elfptr(loadElf(elfpath, false));
	elfptr->loadHeaders();
	ElfCRC = elfptr->getCRC();
	ElfEntry = elfptr->header.e_entry;
	ElfTextRange = elfptr->getTextRange();
	LastELF = std::move(elfpath);

	Console.WriteLn(Color_StrongBlue, "ELF (%s) Game CRC = 0x%08X, EntryPoint = 0x%08X", LastELF.c_str(), ElfCRC, ElfEntry);

	// Note: Do not load game database info here.  This code is generic and called from
	// BIOS key encryption as well as eeloadReplaceOSDSYS.  The first is actually still executing
	// BIOS code, and patches and cheats should not be applied yet.  (they are applied when
	// eeGameStarting is invoked, which is when the VM starts executing the actual game ELF
	// binary).
}

static std::string ExecutablePathToSerial(const std::string& path)
{
	// cdrom:\SCES_123.45;1
	std::string::size_type pos = path.rfind('\\');
	std::string serial;
	if (pos != std::string::npos)
	{
		serial = path.substr(pos + 1);
	}
	else
	{
		// cdrom:SCES_123.45;1
		pos = serial.rfind(':');
		if (pos != std::string::npos)
			serial = path.substr(pos + 1);
		else
			serial = path;
	}

	// strip off ; or version number
	pos = serial.rfind(';');
	if (pos != std::string::npos)
		serial.erase(pos);

	// check that it matches our expected format.
	// this maintains the old behavior of PCSX2.
	if (!StringUtil::WildcardMatch(serial.c_str(), "????_???.??*"))
		serial.clear();

	// SCES_123.45 -> SCES-12345
	for (std::string::size_type pos = 0; pos < serial.size();)
	{
		if (serial[pos] == '.')
		{
			serial.erase(pos, 1);
			continue;
		}

		if (serial[pos] == '_')
			serial[pos] = '-';
		else
			serial[pos] = static_cast<char>(std::toupper(serial[pos]));

		pos++;
	}

	return serial;
}

void cdvdReloadElfInfo(std::string elfoverride)
{
	// called from context of executing VM code (recompilers), so we need to trap exceptions
	// and route them through the VM's exception handler.  (needed for non-SEH platforms, such
	// as Linux/GCC)
	DevCon.WriteLn(Color_Green, "Reload ELF");
	try
	{
		if (!elfoverride.empty())
		{
			_reloadElfInfo(std::move(elfoverride));
			return;
		}

		std::string elfpath;
		u32 discType = GetPS2ElfName(elfpath);
		DiscSerial = ExecutablePathToSerial(elfpath);

		if (discType == 1)
		{
			// PCSX2 currently only recognizes *.elf executables in proper PS2 format.
			// To support different PSX titles in the console title and for savestates, this code bypasses all the detection,
			// simply using the exe name, stripped of problematic characters.
			return;
		}

		// Isn't a disc we recognize?
		if (discType == 0)
			return;

		// Recognized and PS2 (BOOT2).  Good job, user.
		_reloadElfInfo(std::move(elfpath));
	}
	catch ([[maybe_unused]] Exception::FileNotFound& e)
	{
#ifdef PCSX2_CORE
		Console.Error("Failed to load ELF info");
		LastELF.clear();
		DiscSerial.clear();
		ElfCRC = 0;
		ElfEntry = 0;
		ElfTextRange = {};
		return;
#else
		pxFail("Not in my back yard!");
		Cpu->ThrowException(e);
#endif
	}
}

void cdvdReadKey(u8 arg0, u16 arg1, u32 arg2, u8* key)
{
	s32 numbers = 0, letters = 0;
	u32 key_0_3;
	u8 key_4, key_14;
	u32 cdvdOffset;

	cdvdReloadElfInfo();

	// clear key values
	memset(key, 0, 16);

	if (!DiscSerial.empty())
	{
		DevCon.WriteLn(Color_Green, "DiscSerial = %s, arg0=0x%x, arg1=0x%x, arg2=%d", DiscSerial.c_str(), arg0, arg1, arg2);
		// convert the number characters to a real 32 bit number
		numbers = StringUtil::FromChars<s32>(std::string_view(DiscSerial).substr(5, 5)).value_or(0);

		// combine the lower 7 bits of each char
		// to make the 4 letters fit into a single u32
		letters = (s32)((DiscSerial[3] & 0x7F) << 0) |
				  (s32)((DiscSerial[2] & 0x7F) << 7) |
				  (s32)((DiscSerial[1] & 0x7F) << 14) |
				  (s32)((DiscSerial[0] & 0x7F) << 21);
	}

	// calculate magic numbers
	key_0_3 = ((numbers & 0x1FC00) >> 10) | ((0x01FFFFFF & letters) << 7); // numbers = 7F  letters = FFFFFF80
	key_4 = ((numbers & 0x0001F) << 3) | ((0x0E000000 & letters) >> 25);   // numbers = F8  letters = 07
	key_14 = ((numbers & 0x003E0) >> 2) | 0x04;                            // numbers = F8  extra   = 04  unused = 03

	// store key values
	key[0] = (key_0_3 & 0x000000FF) >> 0;
	key[1] = (key_0_3 & 0x0000FF00) >> 8;
	key[2] = (key_0_3 & 0x00FF0000) >> 16;
	key[3] = (key_0_3 & 0xFF000000) >> 24;
	key[4] = key_4;

	switch (arg2)
	{
		case 75:
			key[10] = 0x10; // DNAS_ID[0]
			key[11] = 0x11; // DNAS_ID[1]
			key[12] = 0x12; // DNAS_ID[2]
			key[13] = 0x13; // DNAS_ID[3]
			key[14] = key_14; // DNAS_ID[4]
			key[15] = 0x05;
			break;

			//      case 3075:
			//          key[15] = 0x01;
			//          break;

		case 4246:
			cdvdOffset = getCdvdOffset();
			if (cdvdOffset)
			{
				key[0] = (cdvdOffset >> 24) & 0xff;
				key[1] = (cdvdOffset >> 16) & 0xff;
				key[2] = (cdvdOffset >> 8) & 0xff;
				key[3] = (cdvdOffset >> 0) & 0xff;
			}
			else
			{
				Console.Warning("cdvdReadKey : Unknown cdvdOffset for %s", DiscSerial.c_str());
				// DVD Player Version 2.10 (Australia) [PBPX-95209]
				// 0x0001F2F707 = sector 0x0001F2F7  dec 0x07 / 127735
				key[0] = 0x07; // SUB_ID[0]
				key[1] = 0xF7; // SUB_ID[1] / LBA[2]
				key[2] = 0xF2; // SUB_ID[2] / LBA[1]
				key[3] = 0x01; // SUB_ID[3] / LBA[0]
			}
			key[4] = 0x00;
			key[15] = 0x01;
			break;

		default:
			key[15] = 0x01;
			break;
	}

	DevCon.WriteLn("CDVD.KEY = %02X,%02X,%02X,%02X,%02X,%02X,%02X",
		cdvd.Key[0], cdvd.Key[1], cdvd.Key[2], cdvd.Key[3], cdvd.Key[4], cdvd.Key[14], cdvd.Key[15]);
}

s32 cdvdGetToc(void* toc)
{
	s32 ret = CDVD->getTOC(toc);
	if (ret == -1)
		ret = 0x80;
	return ret;
}

s32 cdvdReadSubQ(s32 lsn, cdvdSubQ* subq)
{
	s32 ret = CDVD->readSubQ(lsn, subq);
	if (ret == -1)
		ret = 0x80;
	return ret;
}

static void cdvdDetectDisk()
{
	cdvd.Type = DoCDVDdetectDiskType();

	if (cdvd.Type != 0)
	{
		cdvdTD td;
		CDVD->getTD(0, &td);
		cdvd.MaxSector = td.lsn;
	}
}

static void cdvdUpdateStatus(cdvdStatus NewStatus)
{
	cdvd.Status = NewStatus;
	cdvd.StatusSticky |= NewStatus;
}

static void cdvdUpdateReady(u8 NewReadyStatus)
{
	// We don't really use the MECHA bit but Cold Fear will kick back to the BIOS if it's not set
	cdvd.Ready = NewReadyStatus | (CDVD_DRIVE_MECHA_INIT | CDVD_DRIVE_DEV9CON);
}

s32 cdvdCtrlTrayOpen()
{
	DevCon.WriteLn(Color_Green, "Open virtual disk tray");

	// If we switch using a source change we need to pretend it's a new disc
	if (CDVDsys_GetSourceType() == CDVD_SourceType::Disc)
	{
		cdvdNewDiskCB();
		return 0;
	}

	cdvdDetectDisk();

	DiscSwapTimerSeconds = cdvd.RTC.second; // remember the PS2 time when this happened
	cdvdUpdateStatus(CDVD_STATUS_TRAY_OPEN);
	cdvdUpdateReady(0);
	cdvd.Spinning = false;
	cdvdSetIrq(1 << Irq_Eject);

	if (cdvd.Type > 0 || CDVDsys_GetSourceType() == CDVD_SourceType::NoDisc)
	{
		cdvd.Tray.cdvdActionSeconds = 3;
		cdvd.Tray.trayState = CDVD_DISC_EJECT;
		DevCon.WriteLn(Color_Green, "Simulating ejected media");
	}

	return 0; // needs to be 0 for success according to homebrew test "CDVD"
}

s32 cdvdCtrlTrayClose()
{
	DevCon.WriteLn(Color_Green, "Close virtual disk tray");

	if (!g_GameStarted && g_SkipBiosHack)
	{
		DevCon.WriteLn(Color_Green, "Media already loaded (fast boot)");
		cdvdUpdateReady(CDVD_DRIVE_READY);
		cdvdUpdateStatus(CDVD_STATUS_PAUSE);
		cdvd.Tray.trayState = CDVD_DISC_ENGAGED;
		cdvd.Tray.cdvdActionSeconds = 0;
	}
	else
	{
		DevCon.WriteLn(Color_Green, "Detecting media");
		cdvdUpdateReady(CDVD_DRIVE_BUSY);
		cdvdUpdateStatus(CDVD_STATUS_SEEK);
		cdvd.Tray.trayState = CDVD_DISC_DETECTING;
		cdvd.Tray.cdvdActionSeconds = 3;
	}
	cdvdDetectDisk();

	return 0; // needs to be 0 for success according to homebrew test "CDVD"
}

// check whether disc is single or dual layer
// if its dual layer, check what the disctype is and what sector number
// layer1 starts at
//
// args:    gets value for dvd type (0=single layer, 1=ptp, 2=otp)
//          gets value for start lsn of layer1
// returns: 1 if on dual layer disc
//          0 if not on dual layer disc
static s32 cdvdReadDvdDualInfo(s32* dualType, u32* layer1Start)
{
	*dualType = 0;
	*layer1Start = 0;

	return CDVD->getDualInfo(dualType, layer1Start);
}

static bool cdvdIsDVD()
{
	if (cdvd.Type == CDVD_TYPE_DETCTDVDS || cdvd.Type == CDVD_TYPE_DETCTDVDD || cdvd.Type == CDVD_TYPE_PS2DVD || cdvd.Type == CDVD_TYPE_DVDV)
		return true;
	else
		return false;
}

static int cdvdTrayStateDetecting()
{
	if (cdvd.Tray.trayState == CDVD_DISC_DETECTING)
		return CDVD_TYPE_DETCT;

	if (cdvdIsDVD())
	{
		u32 layer1Start;
		s32 dualType;

		cdvdReadDvdDualInfo(&dualType, &layer1Start);

		if (dualType > 0)
			return CDVD_TYPE_DETCTDVDD;
		else
			return CDVD_TYPE_DETCTDVDS;
	}

	if (cdvd.Type != CDVD_TYPE_NODISC)
		return CDVD_TYPE_DETCTCD;
	else
		return CDVD_TYPE_DETCT; //Detecting any kind of disc existing
}
static u32 cdvdRotationalLatency(CDVD_MODE_TYPE mode)
{
	// CAV rotation is constant (minimum speed to maintain exact speed on outer dge
	if (cdvd.SpindlCtrl & CDVD_SPINDLE_CAV)
	{
		const float rotationPerSecond = static_cast<float>(((mode == MODE_CDROM) ? CD_MIN_ROTATION_X1 : DVD_MIN_ROTATION_X1) * cdvd.Speed) / 60.0f;
		const float msPerRotation = 1000.0f / rotationPerSecond;

		return ((PSXCLK / 1000) * msPerRotation);
	}
	else
	{
		int numSectors = 0;
		int offset = 0;

		//CLV adjusts its speed based on where it is on the disc, so we can take the max RPM and use the sector to work it out
		// Sector counts are taken from google for Single layer, Dual layer DVD's and for 700MB CD's
		switch (cdvd.Type)
		{
			case CDVD_TYPE_DETCTDVDS:
			case CDVD_TYPE_PS2DVD:
			case CDVD_TYPE_DETCTDVDD:
				numSectors = 2298496;

				u32 layer1Start;
				s32 dualType;
				// Layer 1 needs an offset as it goes back to the middle of the disc
				cdvdReadDvdDualInfo(&dualType, &layer1Start);
				if (cdvd.SeekToSector >= layer1Start)
					offset = layer1Start;
				break;
			default: // Pretty much every CD format
				numSectors = 360000;
				break;
		}
		const float sectorSpeed = (((float)(cdvd.SeekToSector - offset) / numSectors) * 0.60f) + 0.40f;

		const float rotationPerSecond = static_cast<float>(((mode == MODE_CDROM) ? CD_MAX_ROTATION_X1 : DVD_MAX_ROTATION_X1) * cdvd.Speed * sectorSpeed) / 60.0f;
		const float msPerRotation = 1000.0f / rotationPerSecond;
		//DevCon.Warning("Rotations per second %f, msPerRotation cycles per ms %f total cycles per ms %d cycles per rotation %d", rotationPerSecond, msPerRotation, (u32)(PSXCLK / 1000), (u32)((PSXCLK / 1000) * msPerRotation));
		return ((PSXCLK / 1000) * msPerRotation);
	}
}

static uint cdvdBlockReadTime(CDVD_MODE_TYPE mode)
{
	// CAV Read speed is roughly 41% in the centre full speed on outer edge. I imagine it's more logarithmic than this
	if (cdvd.SpindlCtrl & CDVD_SPINDLE_CAV)
	{
		int numSectors = 0;
		int offset = 0;

		// Sector counts are taken from google for Single layer, Dual layer DVD's and for 700MB CD's
		switch (cdvd.Type)
		{
			case CDVD_TYPE_DETCTDVDS:
			case CDVD_TYPE_PS2DVD:
			case CDVD_TYPE_DETCTDVDD:
				numSectors = 2298496;
				u32 layer1Start;
				s32 dualType;

				// Layer 1 needs an offset as it goes back to the middle of the disc
				cdvdReadDvdDualInfo(&dualType, &layer1Start);
				if (cdvd.SeekToSector >= layer1Start)
					offset = layer1Start;
				break;
			default: // Pretty much every CD format
				numSectors = 360000;
				break;
		}

		// 0.40f is the "base" inner track speed.
		const float sectorSpeed = ((static_cast<float>(cdvd.SeekToSector - offset) / static_cast<float>(numSectors)) * 0.60f) + 0.40f;
		float cycles = static_cast<float>(PSXCLK) / (static_cast<float>(((mode == MODE_CDROM) ? CD_SECTORS_PERSECOND : DVD_SECTORS_PERSECOND) * cdvd.Speed) * sectorSpeed);

		return static_cast<int>(cycles);
	}

	// CLV Read Speed is constant
	float cycles = static_cast<float>(PSXCLK) / static_cast<float>(((mode == MODE_CDROM) ? CD_SECTORS_PERSECOND : DVD_SECTORS_PERSECOND) * cdvd.Speed);

	return static_cast<int>(cycles);
}

void readKeyStore(int idx_set)
{
	uint16_t* ks = (uint16_t*)&g_keyStore;

	uint32_t keyOffset = 0;
	for (int i = 0; i < 18; ++i)
	{
		uint16_t keyIdx = g_MemoryCardKeyIndexes[18 * idx_set + i];
		if (keyIdx >= 0x200)
		{
			if (keyIdx == 0xFFFF)
			{
				ks[keyOffset++] = 0;
				ks[keyOffset++] = 0;
				ks[keyOffset++] = 0;
				ks[keyOffset++] = 0;
			}
			else
			{
				ks[keyOffset++] = g_cardKeyStore[4 * (uint8_t)keyIdx];
				ks[keyOffset++] = g_cardKeyStore[4 * (uint8_t)keyIdx + 1];
				ks[keyOffset++] = g_cardKeyStore[4 * (uint8_t)keyIdx + 2];
				ks[keyOffset++] = g_cardKeyStore[4 * (uint8_t)keyIdx + 3];
			}
		}
		else
		{
			ks_index = keyIdx;

			ks[keyOffset++] = ks_data;
			ks[keyOffset++] = ks_data;
			ks[keyOffset++] = ks_data;
			ks[keyOffset++] = ks_data;
		}
	}

	ks_index = g_KelfKeysIndex[idx_set];
	for (int j = 0; j < 19; ++j)
	{
		ks[keyOffset++] = ks_data;
		ks[keyOffset++] = ks_data;
		ks[keyOffset++] = ks_data;
		ks[keyOffset++] = ks_data;
	}

	ks_index = 192;
	ks[keyOffset++] = ks_data;
	ks[keyOffset++] = ks_data;
	ks[keyOffset++] = ks_data;
	ks[keyOffset++] = ks_data;
}

static void desEncrypt(void* key, void* data)
{
	DesContext dc;
	desInit(&dc, (uint8_t*)key, 8);
	desEncryptBlock(&dc, (uint8_t*)data, (uint8_t*)data);
}

static void desDecrypt(void* key, void* data)
{
	DesContext dc;
	desInit(&dc, (uint8_t*)key, 8);
	desDecryptBlock(&dc, (uint8_t*)data, (uint8_t*)data);
}

static void doubleDesEncrypt(void* key, void* data)
{
	desEncrypt(key, data);
	desDecrypt(&((uint8_t*)key)[8], data);
	desEncrypt(key, data);
}

static void doubleDesDecrypt(void* key, void* data)
{
	desDecrypt(key, data);
	desEncrypt(&((uint8_t*)key)[8], data);
	desDecrypt(key, data);
}

static void xor_bit(const void* a, const void* b, void* Result, size_t Length)
{
	size_t i;
	for (i = 0; i < Length; i++)
	{
		((uint8_t*)Result)[i] = ((uint8_t*)a)[i] ^ ((uint8_t*)b)[i];
	}
}

void readAndDecryptKeyStore(int idx_set)
{
	uint8_t* ks = (uint8_t*)&g_keyStore;

	readKeyStore(idx_set);
	for (int i = 0; i < 38; ++i)
		doubleDesDecrypt(g_KeyStoreKey, &ks[i * 8]);

	uint8_t consoleID[8];
	cdvdReadConsoleID(consoleID);

	uint8_t iLinkID[8];
	cdvdReadILinkID(iLinkID);

	uint8_t icvps2Seed[8];
	xor_bit(iLinkID, consoleID, icvps2Seed, 8);

	uint8_t icvps2LowSeed[8];
	xor_bit(icvps2Seed, g_keyStore.icvps2LowIV, icvps2LowSeed, 8);

	uint8_t icvps2HiSeed[8];
	xor_bit(icvps2Seed, g_keyStore.icvps2HiIV, icvps2HiSeed, 8);

	doubleDesEncrypt(g_keyStore.icvps2LowKey, icvps2LowSeed);
	doubleDesEncrypt(g_keyStore.icvps2HiKey, icvps2HiSeed);

	memcpy(cdvd.icvps2Key, icvps2LowSeed, 8);
	memcpy(&cdvd.icvps2Key[8], icvps2HiSeed, 8);
}

void cdvdReset()
{
	memzero(cdvd);

	cdvd.Type = CDVD_TYPE_NODISC;
	cdvd.Spinning = false;

	cdvd.sDataIn = 0x40;
	cdvdUpdateReady(CDVD_DRIVE_READY);
	cdvdUpdateStatus(CDVD_STATUS_PAUSE);
	cdvd.Speed = 4;
	cdvd.BlockSize = 2064;
	cdvd.Action = cdvdAction_None;
	cdvd.ReadTime = cdvdBlockReadTime(MODE_DVDROM);

	// If we are recording, always use the same RTC setting
	// for games that use the RTC to seed their RNG -- this is very important to be the same everytime!
#ifndef DISABLE_RECORDING
	if (g_InputRecording.IsActive())
	{
		Console.WriteLn("Input Recording Active - Using Constant RTC of 04-03-2020 (DD-MM-YYYY)");
		// Why not just 0 everything? Some games apparently require the date to be valid in terms of when
		// the PS2 / Game actually came out. (MGS3).  So set it to a value well beyond any PS2 game's release date.
		cdvd.RTC.second = 0;
		cdvd.RTC.minute = 0;
		cdvd.RTC.hour = 0;
		cdvd.RTC.day = 4;
		cdvd.RTC.month = 3;
		cdvd.RTC.year = 20;
	}
	else
#endif
	{
		// CDVD internally uses GMT+9.  If you think the time's wrong, you're wrong.
		// Set up your time zone and winter/summer in the BIOS.  No PS2 BIOS I know of features automatic DST.
		const std::time_t utc_time = std::time(nullptr);
		const std::time_t gmt9_time = (utc_time + (60 * 60 * 9));
		struct tm curtime = {};
#ifdef _MSC_VER
		gmtime_s(&curtime, &gmt9_time);
#else
		gmtime_r(&gmt9_time, &curtime);
#endif
		cdvd.RTC.second = (u8)curtime.tm_sec;
		cdvd.RTC.minute = (u8)curtime.tm_min;
		cdvd.RTC.hour = (u8)curtime.tm_hour;
		cdvd.RTC.day = (u8)curtime.tm_mday;
		cdvd.RTC.month = (u8)curtime.tm_mon + 1; // WX returns Jan as "0"
		cdvd.RTC.year = (u8)(curtime.tm_year - 100); // offset from 2000
	}

	g_GameStarted = false;
	g_GameLoading = false;
	g_SkipBiosHack = EmuConfig.UseBOOT2Injection;

	cdvdCtrlTrayClose();

	{
		char filename[1024];
		snprintf(filename, sizeof(filename), "%s/%s", EmuFolders::Bios.c_str(), "eks.bin");
		FILE* f = fopen(filename, "rb");
		if (f)
		{
			fread(g_EncryptedKeyStore, 1, sizeof(g_EncryptedKeyStore), f);
			fclose(f);
		}
	}

	{
		char filename[1024];
		snprintf(filename, sizeof(filename), "%s/%s", EmuFolders::Bios.c_str(), "cks.bin");
		FILE* f = fopen(filename, "rb");
		if (f)
		{
			fread(g_cardKeyStore, 1, sizeof(g_cardKeyStore), f);
			fclose(f);
		}
	}

	{
		char filename[1024];
		snprintf(filename, sizeof(filename), "%s/%s", EmuFolders::Bios.c_str(), "kek.bin");
		FILE* f = fopen(filename, "rb");
		if (f)
		{
			fread(g_KeyStoreKey, 1, sizeof(g_KeyStoreKey), f);
			fclose(f);
		}
	}

	readAndDecryptKeyStore(1); // 0: dev, 1: retail, 2: proto?, 3: arcade
	cdvd.mecha_state = MECHA_STATE_READY;
	cdvdGetMechaVer(&temp_mechaver[0]);
}

struct Freeze_v10Compat
{
	u8 Action;
	u32 SeekToSector;
	u32 ReadTime;
	bool Spinning;
};

void SaveStateBase::cdvdFreeze()
{
	FreezeTag("cdvd");
	Freeze(cdvd);

	if (IsLoading())
	{
		// Make sure the Cdvd source has the expected track loaded into the buffer.
		// If cdvd.Readed is cleared it means we need to load the SeekToSector (ie, a
		// seek is in progress!)

		if (cdvd.Reading)
			cdvd.RErr = DoCDVDreadTrack(cdvd.Readed ? cdvd.Sector : cdvd.SeekToSector, cdvd.ReadMode);
	}
}

void cdvdNewDiskCB()
{
	DoCDVDresetDiskTypeCache();
	cdvdDetectDisk();

	// If not ejected but we've swapped source pretend it got ejected
	if ((g_GameStarted || !g_SkipBiosHack) && cdvd.Tray.trayState != CDVD_DISC_EJECT)
	{
		DevCon.WriteLn(Color_Green, "Ejecting media");
		cdvdUpdateStatus(CDVD_STATUS_TRAY_OPEN);
		cdvdUpdateReady(CDVD_DRIVE_BUSY);
		cdvd.Tray.trayState = CDVD_DISC_EJECT;
		cdvd.Spinning = false;
		cdvdSetIrq(1 << Irq_Eject);
		// If it really got ejected, the DVD Reader will report Type 0, so no need to simulate ejection
		if (cdvd.Type > 0)
			cdvd.Tray.cdvdActionSeconds = 3;
	}
	else if (cdvd.Type > 0)
	{
		DevCon.WriteLn(Color_Green, "Seeking new media");
		cdvdUpdateReady(CDVD_DRIVE_BUSY);
		cdvdUpdateStatus(CDVD_STATUS_SEEK);
		cdvd.Spinning = true;
		cdvd.Tray.trayState = CDVD_DISC_DETECTING;
		cdvd.Tray.cdvdActionSeconds = 3;
	}
}

static void mechaDecryptBytes(u32 madr, int size)
{
	int shiftAmount = (cdvd.decSet >> 4) & 7;
	int doXor = (cdvd.decSet) & 1;
	int doShift = (cdvd.decSet) & 2;

	u8* curval = iopPhysMem(madr);
	for (int i = 0; i < size; ++i, ++curval)
	{
		if (doXor)
			*curval ^= cdvd.Key[4];
		if (doShift)
			*curval = (*curval >> shiftAmount) | (*curval << (8 - shiftAmount));
	}
}

int cdvdReadSector()
{
	s32 bcr;

	CDVD_LOG("SECTOR %d (BCR %x;%x)", cdvd.Sector, HW_DMA3_BCR_H16, HW_DMA3_BCR_L16);

	bcr = (HW_DMA3_BCR_H16 * HW_DMA3_BCR_L16) * 4;
	if (bcr < cdvd.BlockSize || !(HW_DMA3_CHCR & 0x01000000))
	{
		CDVD_LOG("READBLOCK:  bcr < cdvd.BlockSize; %x < %x", bcr, cdvd.BlockSize);
		if (HW_DMA3_CHCR & 0x01000000)
		{
			HW_DMA3_CHCR &= ~0x01000000;
			psxDmaInterrupt(3);
		}
		return -1;
	}

	//if( (HW_DMA3_CHCR & 0x01000000) == 0 ) {
	//	// DMA3 problem?
	//	Console.Warning( "CDVD READ - DMA3 transfer off (try again)\n" );
	//}

	// DMAs use physical addresses (air)
	u8* mdest = iopPhysMem(HW_DMA3_MADR);

	// if raw dvd sector 'fill in the blanks'
	if (cdvd.BlockSize == 2064)
	{
		// get info on dvd type and layer1 start
		u32 layer1Start;
		s32 dualType;
		s32 layerNum;
		u32 lsn = cdvd.Sector;

		cdvdReadDvdDualInfo(&dualType, &layer1Start);

		if ((dualType == 1) && (lsn >= layer1Start))
		{
			// dual layer ptp disc
			layerNum = 1;
			lsn = lsn - layer1Start + 0x30000;
		}
		else if ((dualType == 2) && (lsn >= layer1Start))
		{
			// dual layer otp disc
			layerNum = 1;
			lsn = ~(layer1Start + 0x30000 - 1);
		}
		else
		{
			// Assuming the other dualType is 0,
			// single layer disc, or on first layer of dual layer disc.
			layerNum = 0;
			lsn += 0x30000;
		}

		mdest[0] = 0x20 | layerNum;
		mdest[1] = (u8)(lsn >> 16);
		mdest[2] = (u8)(lsn >> 8);
		mdest[3] = (u8)(lsn);

		// sector IED (not calculated at present)
		mdest[4] = 0;
		mdest[5] = 0;

		// sector CPR_MAI (not calculated at present)
		mdest[6] = 0;
		mdest[7] = 0;
		mdest[8] = 0;
		mdest[9] = 0;
		mdest[10] = 0;
		mdest[11] = 0;

		// normal 2048 bytes of sector data
		memcpy(&mdest[12], cdr.Transfer, 2048);

		// 4 bytes of edc (not calculated at present)
		mdest[2060] = 0;
		mdest[2061] = 0;
		mdest[2062] = 0;
		mdest[2063] = 0;
	}
	else
	{
		memcpy(mdest, cdr.Transfer, cdvd.BlockSize);
	}

	// decrypt sector's bytes
	if (cdvd.decSet)
		mechaDecryptBytes(HW_DMA3_MADR, cdvd.BlockSize);

	// Added a clear after memory write .. never seemed to be necessary before but *should*
	// be more correct. (air)
	psxCpu->Clear(HW_DMA3_MADR, cdvd.BlockSize / 4);

	//	Console.WriteLn("sector %x;%x;%x", PSXMu8(madr+0), PSXMu8(madr+1), PSXMu8(madr+2));

	HW_DMA3_BCR_H16 -= (cdvd.BlockSize / (HW_DMA3_BCR_L16 * 4));
	HW_DMA3_MADR += cdvd.BlockSize;

	if (!HW_DMA3_BCR_H16)
	{
		if (HW_DMA3_CHCR & 0x01000000)
		{
			HW_DMA3_CHCR &= ~0x01000000;
			psxDmaInterrupt(3);
		}
	}

	return 0;
}

// inlined due to being referenced in only one place.
__fi void cdvdActionInterrupt()
{
	switch (cdvd.Action)
	{
		case cdvdAction_Seek:
			cdvd.Spinning = true;
			cdvdUpdateReady(CDVD_DRIVE_READY);
			cdvd.Sector = cdvd.SeekToSector;
			cdvdUpdateStatus(CDVD_STATUS_PAUSE);
			cdvd.nextSectorsBuffered = 0;
			CDVDSECTORREADY_INT(cdvd.ReadTime);
			break;

		case cdvdAction_Standby:
			DevCon.Warning("CDVD Standby Call");
			cdvd.Spinning = true; //check (rama)
			cdvdUpdateReady(CDVD_DRIVE_READY);
			cdvd.Sector = cdvd.SeekToSector;
			cdvdUpdateStatus(CDVD_STATUS_PAUSE);
			cdvd.nextSectorsBuffered = 0;
			CDVDSECTORREADY_INT(cdvd.ReadTime);
			break;

		case cdvdAction_Stop:
			cdvd.Spinning = false;
			cdvdUpdateReady(CDVD_DRIVE_READY);
			cdvd.Sector = 0;
			cdvdUpdateStatus(CDVD_STATUS_STOP);
			break;

		case cdvdAction_Error:
			cdvdUpdateReady(CDVD_DRIVE_READY | CDVD_DRIVE_ERROR);
			cdvdUpdateStatus(CDVD_STATUS_PAUSE);
			break;
	}

	if(cdvd.Action != cdvdAction_Seek)
		cdvd.Action = cdvdAction_None;
	cdvdSetIrq();
}

__fi void cdvdSectorReady()
{
	if (cdvd.nextSectorsBuffered < 16)
	{
		cdvd.nextSectorsBuffered++;
		CDVD_LOG("Buffering sector");
	}

	if (cdvd.nextSectorsBuffered < 16)
		CDVDSECTORREADY_INT(cdvd.ReadTime);
	else
		cdvdUpdateStatus(CDVD_STATUS_PAUSE);
}

// inlined due to being referenced in only one place.
__fi void cdvdReadInterrupt()
{
	//Console.WriteLn("cdvdReadInterrupt %x %x %x %x %x", cpuRegs.interrupt, cdvd.Readed, cdvd.Reading, cdvd.nSectors, (HW_DMA3_BCR_H16 * HW_DMA3_BCR_L16) *4);

	cdvdUpdateReady(CDVD_DRIVE_BUSY);
	cdvdUpdateStatus(CDVD_STATUS_READ);
	cdvd.WaitingDMA = false;

	if (!cdvd.Readed)
	{
		// Seeking finished.  Process the track we requested before, and
		// then schedule another CDVD read int for when the block read finishes.

		// NOTE: The first CD track was read when the seek was initiated, so no need
		// to call CDVDReadTrack here.

		cdvd.Spinning = true;
		cdvd.RetryCntP = 0;
		cdvd.Reading = 1;
		cdvd.Readed = 1;
		cdvd.Sector = cdvd.SeekToSector;
		CDVD_LOG("Cdvd Seek Complete at iopcycle=%8.8x.", psxRegs.cycle);
	}

	if (cdvd.AbortRequested)
	{
		// Code in the CDVD controller suggest there is an alignment thing with DVD's but this seems to just break stuff (Auto Modellista).
		// Needs more investigation
		//if (!cdvdIsDVD() || !(cdvd.Sector & 0xF))
		{
			Console.Warning("Read Abort");
			cdvd.Error = 0x1; // Abort Error
			cdvdUpdateReady(CDVD_DRIVE_READY | CDVD_DRIVE_ERROR);
			cdvdUpdateStatus(CDVD_STATUS_PAUSE);
			cdvd.WaitingDMA = false;
			cdvdSetIrq();
			return;
		}
	}

	if (cdvd.Sector >= cdvd.MaxSector)
	{
		DevCon.Warning("Read past end of disc Sector %d Max Sector %d", cdvd.Sector, cdvd.MaxSector);
		cdvd.Error = 0x32; // Outermost track reached during playback
		cdvdUpdateReady(CDVD_DRIVE_READY | CDVD_DRIVE_ERROR);
		cdvdUpdateStatus(CDVD_STATUS_PAUSE);
		cdvd.WaitingDMA = false;
		cdvdSetIrq();
		return;
	}

	if (cdvd.Reading)
	{
		if (cdvd.RErr == 0)
		{
			while ((cdvd.RErr = DoCDVDgetBuffer(cdr.Transfer)), cdvd.RErr == -2)
			{
				// not finished yet ... block on the read until it finishes.
				Threading::Sleep(0);
				Threading::SpinWait();
			}
		}

		if (cdvd.RErr == -1)
		{
			cdvd.RetryCntP++;

			if (cdvd.RetryCntP <= cdvd.RetryCnt)
			{
				CDVD_LOG("CDVD read err, retrying... (attempt %d of %d)", cdvd.RetryCntP, cdvd.RetryCnt);
				cdvd.RErr = DoCDVDreadTrack(cdvd.Sector, cdvd.ReadMode);
				CDVDREAD_INT(cdvd.ReadTime);
			}
			else
				Console.Error("CDVD READ ERROR, sector = 0x%08x", cdvd.Sector);

			return;
		}

		cdvd.Reading = false;

		// Any other value besides 0 should be considered invalid here
		pxAssert(cdvd.RErr == 0);
	}

	if (cdvd.nSectors > 0 && cdvd.nextSectorsBuffered)
	{
		if (cdvdReadSector() == -1)
		{
			// This means that the BCR/DMA hasn't finished yet, and rather than fire off the
			// sector-finished notice too early (which might overwrite game data) we delay a
			// bit and try to read the sector again later.
			// An arbitrary delay of some number of cycles probably makes more sense here,
			// but for now it's based on the cdvd.ReadTime value. -- air
			pxAssert((int)cdvd.ReadTime > 0);
			//CDVDREAD_INT(cdvd.ReadTime); // Bring it back after the DMA has ended to avoid a nasty loop
			/*if (!(cdvd.Ready & CDVD_DRIVE_DATARDY))
			{
				cdvd.PwOff |= (1 << Irq_DataReady);
				psxHu32(0x1070) |= 0x4;
				cdvd.Ready |= CDVD_DRIVE_DATARDY;
			}*/
			cdvdUpdateStatus(CDVD_STATUS_PAUSE);
			cdvd.WaitingDMA = true;
			return;
		}

		cdvd.nextSectorsBuffered--;
		CDVDSECTORREADY_INT(cdvd.ReadTime);

		cdvd.Sector++;
		cdvd.SeekToSector++;

		if (--cdvd.nSectors <= 0)
		{
			// Setting the data ready flag fixes a black screen loading issue in
			// Street Fighter Ex3 (NTSC-J version).
			cdvdSetIrq();
			cdvdUpdateReady(CDVD_DRIVE_READY);

			if (cdvd.nextSectorsBuffered < 16)
				cdvdUpdateStatus(CDVD_STATUS_READ);
			else
				cdvdUpdateStatus(CDVD_STATUS_PAUSE);
			//DevCon.Warning("Scheduling interrupt in %d cycles", cdvd.ReadTime - ((cdvd.BlockSize / 4) * 12));
			// Timing issues on command end
			// Star Ocean (1.1 Japan) expects the DMA to end and interrupt at least 128 or more cycles before the CDVD command ends.
			// However the time required seems to increase slowly, so delaying the end of the command is not the solution.
			//cdvd.Status = CDVD_STATUS_PAUSE; // Needed here but could be smth else than Pause (rama)
			// All done! :D
			return;
		}
	}
	else
	{
		if (cdvd.nSectors <= 0)
		{
			cdvdSetIrq();
			//psxHu32(0x1070) |= 0x4;
			iopIntcIrq(2);
			cdvdUpdateReady(CDVD_DRIVE_READY);

			cdvdUpdateStatus(CDVD_STATUS_PAUSE);
			return;
		}
		CDVDREAD_INT((cdvd.BlockSize / 4) * 12);
		return;
	}

	cdvd.RetryCntP = 0;
	cdvd.Reading = 1;
	cdvd.RErr = DoCDVDreadTrack(cdvd.Sector, cdvd.ReadMode);
	if (cdvd.nextSectorsBuffered)
		CDVDREAD_INT((cdvd.BlockSize / 4) * 12);
	else
		CDVDREAD_INT((psxRegs.cycle - psxRegs.sCycle[IopEvt_CdvdSectorReady]) + ((cdvd.BlockSize / 4) * 12));
}

// Returns the number of IOP cycles until the event completes.
static uint cdvdStartSeek(uint newsector, CDVD_MODE_TYPE mode)
{
	cdvd.SeekToSector = newsector;

	uint delta = abs((s32)(cdvd.SeekToSector - cdvd.Sector));
	uint seektime;
	bool isSeeking = cdvd.nCommand == N_CD_SEEK;

	cdvdUpdateReady(CDVD_DRIVE_BUSY);
	cdvd.Reading = 1;
	cdvd.Readed = 0;
	// Okay so let's explain this, since people keep messing with it in the past and just poking it.
	// So when the drive is spinning, bit 0x2 is set on the Status, and bit 0x8 is set when the drive is not reading.
	// So In the case where it's seeking to data it will be Spinning (0x2) not reading (0x8) and Seeking (0x10, but because seeking is also spinning 0x2 is also set))
	// Update - Apparently all that was rubbish and some games don't like it. WRC was the one in this scenario which hated SEEK |ZPAUSE, so just putting it back to pause for now.
	// We should really run some tests for this behaviour.

	cdvdUpdateStatus(CDVD_STATUS_SEEK);

	if (!cdvd.Spinning)
	{
		CDVD_LOG("CdSpinUp > Simulating CdRom Spinup Time, and seek to sector %d", cdvd.SeekToSector);
		seektime = PSXCLK / 3; // 333ms delay
		cdvd.Spinning = true;
		cdvd.nextSectorsBuffered = 0;
		CDVDSECTORREADY_INT(seektime + cdvd.ReadTime);
	}
	else if ((tbl_ContigiousSeekDelta[mode] == 0) || (delta >= tbl_ContigiousSeekDelta[mode]))
	{
		// Select either Full or Fast seek depending on delta:
		psxRegs.interrupt &= ~(1 << IopEvt_CdvdSectorReady);
		cdvd.nextSectorsBuffered = 0;
		if (delta >= tbl_FastSeekDelta[mode])
		{
			// Full Seek
			CDVD_LOG("CdSeek Begin > to sector %d, from %d - delta=%d [FULL]", cdvd.SeekToSector, cdvd.Sector, delta);
			seektime = Cdvd_FullSeek_Cycles;
		}
		else
		{
			CDVD_LOG("CdSeek Begin > to sector %d, from %d - delta=%d [FAST]", cdvd.SeekToSector, cdvd.Sector, delta);
			seektime = Cdvd_FastSeek_Cycles;
		}
		isSeeking = true;
	}
	else
	{
		CDVD_LOG("CdSeek Begin > Contiguous block without seek - delta=%d sectors", delta);

		// if delta > 0 it will read a new sector so the readInterrupt will account for this.
		seektime = 0;
		isSeeking = false;

		if (delta == 0)
		{
			//cdvd.Status = CDVD_STATUS_PAUSE;
			cdvdUpdateStatus(CDVD_STATUS_READ);
			cdvd.Readed = 1; // Note: 1, not 0, as implied by the next comment. Need to look into this. --arcum42
			cdvd.Reading = 1; // We don't need to wait for it to read a sector as it's already queued up, or we adjust for it here.
			cdvd.RetryCntP = 0;

			// setting Readed to 0 skips the seek logic, which means the next call to
			// cdvdReadInterrupt will load a block.  So make sure it's properly scheduled
			// based on sector read speeds:

			//seektime = cdvd.ReadTime;

			if (!cdvd.nextSectorsBuffered)//Buffering time hasn't completed yet so cancel it and simulate the remaining time
			{
				if (psxRegs.interrupt & (1 << IopEvt_CdvdSectorReady))
				{
					//DevCon.Warning("coming back from ready sector early reducing %d cycles by %d cycles", seektime, psxRegs.cycle - psxRegs.sCycle[IopEvt_CdvdSectorReady]);
					seektime = (psxRegs.cycle - psxRegs.sCycle[IopEvt_CdvdSectorReady]) + ((cdvd.BlockSize / 4) * 12);
				}
				else
				{
					delta = 1; // Forces it to use the rotational delay since we have no sectors buffered and it isn't buffering any.
				}
			}
			else
				return (cdvd.BlockSize / 4) * 12;
		}
		else
		{
			psxRegs.interrupt &= ~(1 << IopEvt_CdvdSectorReady);
			cdvd.nextSectorsBuffered = 0;
		}
	}

	// Only do this on reads, the seek kind of accounts for this and then it reads the sectors after
	if ((delta || cdvd.Action == cdvdAction_Seek) && !isSeeking)
	{
		const u32 rotationalLatency = cdvdRotationalLatency((CDVD_MODE_TYPE)cdvdIsDVD());
		//DevCon.Warning("%s rotational latency at sector %d is %d cycles", (cdvd.SpindlCtrl & CDVD_SPINDLE_CAV) ? "CAV" : "CLV", cdvd.SeekToSector, rotationalLatency);
		seektime += rotationalLatency + cdvd.ReadTime;
		CDVDSECTORREADY_INT(seektime);
		seektime += (cdvd.BlockSize / 4) * 12;
	}
	else
		CDVDSECTORREADY_INT(seektime);

	// Clear the action on the following command, so we can rotate after seek.
	if (cdvd.nCommand != N_CD_SEEK)
		cdvd.Action = cdvdAction_None;

	return seektime;
}

void cdvdUpdateTrayState()
{
	if (cdvd.Tray.cdvdActionSeconds > 0)
	{
		if (--cdvd.Tray.cdvdActionSeconds == 0)
		{
			switch (cdvd.Tray.trayState)
			{
				case CDVD_DISC_EJECT:
					cdvdCtrlTrayClose();
					break;
				case CDVD_DISC_DETECTING:
					DevCon.WriteLn(Color_Green, "Seeking new disc");
					cdvd.Tray.trayState = CDVD_DISC_SEEKING;
					cdvd.Tray.cdvdActionSeconds = 2;
					cdvd.Spinning = true;
					break;
				case CDVD_DISC_SEEKING:
				case CDVD_DISC_ENGAGED:
					cdvd.Tray.trayState = CDVD_DISC_ENGAGED;
					cdvdUpdateReady(CDVD_DRIVE_READY);
					if (CDVDsys_GetSourceType() != CDVD_SourceType::NoDisc)
					{
						DevCon.WriteLn(Color_Green, "Media ready to read");
						cdvdUpdateStatus(CDVD_STATUS_PAUSE);
					}
					else
					{
						cdvd.Spinning = false;
						cdvdUpdateStatus(CDVD_STATUS_STOP);
					}
					break;
			}
		}
	}
}

void cdvdVsync()
{
	cdvd.RTCcount++;
	if (cdvd.RTCcount < GetVerticalFrequency())
		return;
	cdvd.RTCcount = 0;

	cdvdUpdateTrayState();

	cdvd.RTC.second++;
	if (cdvd.RTC.second < 60)
		return;
	cdvd.RTC.second = 0;

	cdvd.RTC.minute++;
	if (cdvd.RTC.minute < 60)
		return;
	cdvd.RTC.minute = 0;

	cdvd.RTC.hour++;
	if (cdvd.RTC.hour < 24)
		return;
	cdvd.RTC.hour = 0;

	cdvd.RTC.day++;
	if (cdvd.RTC.day <= (cdvd.RTC.month == 2 && cdvd.RTC.year % 4 == 0 ? 29 : monthmap[cdvd.RTC.month - 1]))
		return;
	cdvd.RTC.day = 1;

	cdvd.RTC.month++;
	if (cdvd.RTC.month <= 12)
		return;
	cdvd.RTC.month = 1;

	cdvd.RTC.year++;
	if (cdvd.RTC.year < 100)
		return;
	cdvd.RTC.year = 0;
}

static __fi u8 cdvdRead18(void) // SDATAOUT
{
	u8 ret = 0;

	if (((cdvd.sDataIn & 0x40) == 0) && (cdvd.SCMDResultP < cdvd.SCMDResultC))
	{
		cdvd.SCMDResultP++;
		if (cdvd.SCMDResultP >= cdvd.SCMDResultC)
			cdvd.sDataIn |= 0x40;
		ret = cdvd.SCMDResult[cdvd.SCMDResultP - 1];
	}
	CDVD_LOG("cdvdRead18(SDataOut) %x (ResultC=%d, ResultP=%d)", ret, cdvd.SCMDResultC, cdvd.SCMDResultP);

	return ret;
}

u8 cdvdRead(u8 key)
{
	switch (key)
	{
		case 0x04: // NCOMMAND
			CDVD_LOG("cdvdRead04(NCMD) %x", cdvd.nCommand);
			return cdvd.nCommand;

		case 0x05: // N-READY
			CDVD_LOG("cdvdRead05(NReady) %x", cdvd.Ready);
			return cdvd.Ready;

		case 0x06: // ERROR
		{
			CDVD_LOG("cdvdRead06(Error) %x", cdvd.Error);
			u8 ret = cdvd.Error;
			cdvd.Error = 0;
			return ret;
		}
		case 0x07: // BREAK
			CDVD_LOG("cdvdRead07(Break) %x", 0);
			return 0;

		case 0x08: // INTR_STAT
			CDVD_LOG("cdvdRead08(IntrReason) %x", cdvd.IntrStat);
			return cdvd.IntrStat;

		case 0x0A: // STATUS
			CDVD_LOG("cdvdRead0A(Status) %x", cdvd.Status);
			return cdvd.Status;

		case 0x0B: // STATUS STICKY
		{
			CDVD_LOG("cdvdRead0B(Status Sticky): %x", cdvd.StatusSticky);
			return cdvd.StatusSticky;
		}
		case 0x0C: // CRT MINUTE
			CDVD_LOG("cdvdRead0C(Min) %x", itob((u8)(cdvd.Sector / (60 * 75))));
			return itob((u8)(cdvd.Sector / (60 * 75)));

		case 0x0D: // CRT SECOND
			CDVD_LOG("cdvdRead0D(Sec) %x", itob((u8)((cdvd.Sector / 75) % 60) + 2));
			return itob((u8)((cdvd.Sector / 75) % 60) + 2);

		case 0x0E: // CRT FRAME
			CDVD_LOG("cdvdRead0E(Frame) %x", itob((u8)(cdvd.Sector % 75)));
			return itob((u8)(cdvd.Sector % 75));

		case 0x0F: // TYPE
			if (cdvd.Tray.trayState == CDVD_DISC_ENGAGED)
			{
				CDVD_LOG("cdvdRead0F(Disc Type) Engaged %x", cdvd.Type);
				return cdvd.Type;
			}
			else
			{
				CDVD_LOG("cdvdRead0F(Disc Type) Detecting %x", (cdvd.Tray.trayState <= CDVD_DISC_SEEKING) ? cdvdTrayStateDetecting() : 0);
				return (cdvd.Tray.trayState <= CDVD_DISC_SEEKING) ? cdvdTrayStateDetecting() : 0; // Detecting Disc / No Disc
			}

		case 0x13: // SPEED
		{
			u8 speedCtrl = cdvd.SpindlCtrl & 0x3F;

			if (speedCtrl == 0)
				speedCtrl = cdvdIsDVD() ? 3 : 5;

			if (cdvdIsDVD())
				speedCtrl += 0xF;
			else
				speedCtrl--;

			if (cdvd.Tray.trayState != CDVD_DISC_ENGAGED || cdvd.Spinning == false)
				speedCtrl = 0;

			CDVD_LOG("cdvdRead13(Speed) %x", speedCtrl);
			return speedCtrl;
		}


		case 0x15: // RSV
			CDVD_LOG("cdvdRead15(RSV)");
			return 0x0; //  PSX DESR related, but confirmed to be 0 on normal PS2

		case 0x16: // SCOMMAND
			CDVD_LOG("cdvdRead16(SCMD) %x", cdvd.sCommand);
			return cdvd.sCommand;

		case 0x17: // SREADY
			CDVD_LOG("cdvdRead17(SReady) %x", cdvd.sDataIn);
			return cdvd.sDataIn;

		case 0x18:
			return cdvdRead18();

		case 0x20:
		case 0x21:
		case 0x22:
		case 0x23:
		case 0x24:
		{
			int temp = key - 0x20;

			CDVD_LOG("cdvdRead%d(Key%d) %x", key, temp, cdvd.Key[temp]);
			return cdvd.Key[temp];
		}
		case 0x28:
		case 0x29:
		case 0x2A:
		case 0x2B:
		case 0x2C:
		{
			int temp = key - 0x23;

			CDVD_LOG("cdvdRead%d(Key%d) %x", key, temp, cdvd.Key[temp]);
			return cdvd.Key[temp];
		}

		case 0x30:
		case 0x31:
		case 0x32:
		case 0x33:
		case 0x34:
		{
			int temp = key - 0x26;

			CDVD_LOG("cdvdRead%d(Key%d) %x", key, temp, cdvd.Key[temp]);
			return cdvd.Key[temp];
		}

		case 0x38: // valid parts of key data (first and last are valid)
			CDVD_LOG("cdvdRead38(KeysValid) %x", cdvd.Key[15]);

			return cdvd.Key[15];

		case 0x39: // KEY-XOR
			CDVD_LOG("cdvdRead39(KeyXor) %x", cdvd.KeyXor);

			return cdvd.KeyXor;

		case 0x3A: // DEC_SET
			CDVD_LOG("cdvdRead3A(DecSet) %x", cdvd.decSet);

			return cdvd.decSet;

		default:
			// note: notify the console since this is a potentially serious emulation problem:
			// return -1 (all bits set) instead of 0, improves chances of the software being happy
			PSXHW_LOG("*Unknown 8bit read at address 0x1f4020%x", key);
			Console.Error("IOP Unknown 8bit read from addr 0x1f4020%x", key);
			return -1;
	}
}

static bool cdvdReadErrorHandler()
{
	if (cdvd.nSectors <= 0)
	{
		DevCon.Warning("Bad Sector Count Error");
		cdvd.Error = 0x21; // Number of read sectors abnormal
		return false;
	}

	if (cdvd.SeekToSector >= cdvd.MaxSector)
	{
		DevCon.Warning("Error reading past end of disc");
		// Probably should be 0x20 (bad LSN) but apparently Silent Hill 2 Black Ribbon has a fade at the end of the first trailer
		// And the only way you can throw an error and it still does that is to use 0x30 (Read error), anything else it skips the fade.
		// This'll do for now but needs investigation
		cdvd.Error = 0x30; // Problem occurred during read
		return false;
	}

	return true;
}

static bool cdvdCommandErrorHandler()
{
	if (cdvd.nCommand > N_CD_NOP) // Command needs a disc, so check the tray is closed
	{
		if ((cdvd.Status & CDVD_STATUS_TRAY_OPEN) || (cdvd.Type == CDVD_TYPE_NODISC))
		{
			cdvd.Error = (cdvd.Type == CDVD_TYPE_NODISC) ? 0x12 : 0x11; // No Disc Tray is open
			cdvd.Ready |= CDVD_DRIVE_ERROR;
			cdvdSetIrq();
			return false;
		}
	}

	if (cdvd.NCMDParamC != cdvdParamLength[cdvd.nCommand] && cdvdParamLength[cdvd.nCommand] != 255)
	{
		DevCon.Warning("CDVD: Error in command parameter length, expecting %d got %d", cdvdParamLength[cdvd.nCommand], cdvd.NCMDParamC);
		cdvd.Error = 0x22; // Invalid parameter for command
		cdvd.Ready |= CDVD_DRIVE_ERROR;
		cdvdSetIrq();
		return false;
	}

	if (cdvd.nCommand > N_CD_CHG_SPDL_CTRL)
	{
		DevCon.Warning("CDVD: Error invalid NCMD");
		cdvd.Error = 0x10; // Unsupported Command
		cdvd.Ready |= CDVD_DRIVE_ERROR;
		cdvdSetIrq();
		return false;
	}

	return true;
}

static void cdvdWrite04(u8 rt)
{ // NCOMMAND
	CDVD_LOG("cdvdWrite04: NCMD %s (%x) (ParamP = %x)", nCmdName[rt], rt, cdvd.NCMDParamP);

	if (!(cdvd.Ready & CDVD_DRIVE_READY))
	{
		DevCon.Warning("CDVD: Error drive not ready on command issue");
		cdvd.Error = 0x13; // Not Ready
		cdvd.Ready |= CDVD_DRIVE_ERROR;
		cdvdSetIrq();
		cdvd.NCMDParamP = 0;
		cdvd.NCMDParamC = 0;
		return;
	}

	cdvd.nCommand = rt;
	cdvd.AbortRequested = false;

	if (!cdvdCommandErrorHandler())
	{
		cdvd.NCMDParamP = 0;
		cdvd.NCMDParamC = 0;
		return;
	}

	switch (rt)
	{
		case N_CD_NOP: // CdNop_
			cdvdUpdateReady(CDVD_DRIVE_READY);
			cdvdSetIrq();
			break;
		case N_CD_RESET: // CdSync
			Console.WriteLn("CDVD: Reset NCommand");
			cdvdUpdateReady(CDVD_DRIVE_READY);
			cdvd.SCMDParamP = 0;
			cdvd.SCMDParamC = 0;
			cdvdUpdateStatus(CDVD_STATUS_STOP);
			cdvd.Spinning = false;
			memzero(cdvd.SCMDResult);
			cdvdSetIrq();
			break;

		case N_CD_STANDBY: // CdStandby

			// Seek to sector zero.  The cdvdStartSeek function will simulate
			// spinup times if needed.
			cdvdUpdateReady(CDVD_DRIVE_BUSY);
			DevCon.Warning("CdStandby : %d", rt);
			cdvd.ReadTime = cdvdBlockReadTime((CDVD_MODE_TYPE)cdvdIsDVD());
			CDVD_INT(cdvdStartSeek(0, MODE_DVDROM));
			// Might not seek, but makes sense since it does move to the inner most track
			// It's only temporary until the interrupt anyway when it sets itself ready
			cdvdUpdateStatus(CDVD_STATUS_SEEK);
			cdvd.Action = cdvdAction_Standby;
			break;

		case N_CD_STOP: // CdStop
			DevCon.Warning("CdStop : %d", rt);
			cdvdUpdateReady(CDVD_DRIVE_BUSY);
			cdvd.nextSectorsBuffered = 0;
			psxRegs.interrupt &= ~(1 << IopEvt_CdvdSectorReady);
			cdvdUpdateStatus(CDVD_STATUS_SPIN);
			CDVD_INT(PSXCLK / 6); // 166ms delay?
			cdvd.Action = cdvdAction_Stop;
			break;

		case N_CD_PAUSE: // CdPause
			// A few games rely on PAUSE setting the Status correctly.
			// However we should probably stop any read in progress too, just to be safe
			psxRegs.interrupt &= ~(1 << IopEvt_Cdvd);
			cdvdUpdateReady(CDVD_DRIVE_READY);
			cdvdSetIrq();
			//After Pausing needs to buffer the next sector
			cdvdUpdateStatus(CDVD_STATUS_PAUSE);
			cdvd.nextSectorsBuffered = 0;
			CDVDSECTORREADY_INT(cdvd.ReadTime);
			break;

		case N_CD_SEEK: // CdSeek
			cdvdUpdateReady(CDVD_DRIVE_BUSY);
			cdvd.ReadTime = cdvdBlockReadTime((CDVD_MODE_TYPE)cdvdIsDVD());
			CDVD_INT(cdvdStartSeek(*(uint*)(cdvd.NCMDParam + 0), (CDVD_MODE_TYPE)cdvdIsDVD()));
			cdvdUpdateStatus(CDVD_STATUS_SEEK);
			cdvd.Action = cdvdAction_Seek;
			break;

		case N_CD_READ: // CdRead
		{
			// Assign the seek to sector based on cdvd.Param[0]-[3], and the number of  sectors based on cdvd.Param[4]-[7].
			cdvd.SeekToSector = *(u32*)(cdvd.NCMDParam + 0);
			cdvd.nSectors = *(u32*)(cdvd.NCMDParam + 4);
			cdvd.RetryCnt = (cdvd.NCMDParam[8] == 0) ? 0x100 : cdvd.NCMDParam[8];
			u32 oldSpindleCtrl = cdvd.SpindlCtrl;

			if (cdvd.NCMDParam[9] & 0x3F)
				cdvd.SpindlCtrl = cdvd.NCMDParam[9];
			else
				cdvd.SpindlCtrl = (cdvd.NCMDParam[9] & 0x80) | (cdvdIsDVD() ? 3 : 5); // Max speed for DVD/CD

			if (cdvd.NCMDParam[9] & CDVD_SPINDLE_NOMINAL)
				DevCon.Warning("CDVD: CD Read using Nominal switch from CAV to CLV, unhandled");

			bool ParamError = false;
			const int oldSpeed = cdvd.Speed;

			switch (cdvd.SpindlCtrl & CDVD_SPINDLE_SPEED)
			{
				case 1: // x1
					cdvd.Speed = 1;
					break;
				case 2: // x2
					cdvd.Speed = 2;
					break;
				case 3: // x4
					cdvd.Speed = 4;
					break;
				case 4: // x12
					if (cdvdIsDVD())
					{
						DevCon.Warning("CDVD Read invalid DVD Speed %d", cdvd.SpindlCtrl & CDVD_SPINDLE_SPEED);
						ParamError = true;
					}
					else
						cdvd.Speed = 12;
					break;
				case 5: // x24
					if (cdvdIsDVD())
					{
						DevCon.Warning("CDVD Read invalid DVD Speed %d", cdvd.SpindlCtrl & CDVD_SPINDLE_SPEED);
						ParamError = true;
					}
					else
						cdvd.Speed = 24;
					break;
				default:
					Console.Error("Unknown CDVD Read Speed SpindleCtrl=%x", cdvd.SpindlCtrl);
					ParamError = true;
					break;
			}

			if ((cdvd.SpindlCtrl & CDVD_SPINDLE_CAV) != (oldSpindleCtrl & CDVD_SPINDLE_CAV) || oldSpeed != cdvd.Speed)
			{
				CDVD_LOG("CdRead > Speed change, adding delay");
				cdvd.Spinning = false;
			}

			if (cdvdIsDVD() && cdvd.NCMDParam[10] != 0)
			{
				ParamError = true;
			}
			else
			{
				switch (cdvd.NCMDParam[10])
				{
					case 2:
						cdvd.ReadMode = CDVD_MODE_2340;
						cdvd.BlockSize = 2340;
						break;
					case 1:
						cdvd.ReadMode = CDVD_MODE_2328;
						cdvd.BlockSize = 2328;
						break;
					case 0:
						cdvd.ReadMode = CDVD_MODE_2048;
						cdvd.BlockSize = 2048;
						break;
					default:
						ParamError = true;
						break;
				}
			}

			if (ParamError)
			{
				DevCon.Warning("CDVD: CD Read Bad Parameter Error");
				cdvd.SpindlCtrl = oldSpindleCtrl;
				cdvd.Error = 0x22; // Invalid Parameter
				cdvd.Action = cdvdAction_Error;
				cdvdUpdateStatus(CDVD_STATUS_SEEK);
				cdvdUpdateReady(CDVD_DRIVE_BUSY);
				CDVD_INT(cdvd.BlockSize * 12);
				break;
			}

			if (!cdvdReadErrorHandler())
			{
				cdvd.Action = cdvdAction_Error;
				cdvdUpdateStatus(CDVD_STATUS_SEEK);
				cdvdUpdateReady(CDVD_DRIVE_BUSY);
				CDVD_INT(cdvdRotationalLatency((CDVD_MODE_TYPE)cdvdIsDVD()));
				break;
			}

			CDVD_LOG("CDRead > startSector=%d, seekTo=%d nSectors=%d, RetryCnt=%x, Speed=%dx(%s), ReadMode=%x(%x) SpindleCtrl=%x",
				cdvd.Sector, cdvd.SeekToSector, cdvd.nSectors, cdvd.RetryCnt, cdvd.Speed, (cdvd.SpindlCtrl & CDVD_SPINDLE_CAV) ? "CAV" : "CLV", cdvd.ReadMode, cdvd.NCMDParam[10], cdvd.SpindlCtrl);

			if (EmuConfig.CdvdVerboseReads)
				Console.WriteLn(Color_Gray, "CDRead: Reading Sector %07d (%03d Blocks of Size %d) at Speed=%dx(%s) Spindle=%x",
					cdvd.SeekToSector, cdvd.nSectors, cdvd.BlockSize, cdvd.Speed, (cdvd.SpindlCtrl & CDVD_SPINDLE_CAV) ? "CAV" : "CLV", cdvd.SpindlCtrl);

			cdvd.ReadTime = cdvdBlockReadTime((CDVD_MODE_TYPE)cdvdIsDVD());
			CDVDREAD_INT(cdvdStartSeek(cdvd.SeekToSector, (CDVD_MODE_TYPE)cdvdIsDVD()));

			// Read-ahead by telling CDVD about the track now.
			// This helps improve performance on actual from-cd emulation
			// (ie, not using the hard drive)
			cdvd.RErr = DoCDVDreadTrack(cdvd.SeekToSector, cdvd.ReadMode);

			// Set the reading block flag.  If a seek is pending then Readed will
			// take priority in the handler anyway.  If the read is contiguous then
			// this'll skip the seek delay.
			cdvd.Reading = 1;
			break;
		}
		case N_CD_READ_CDDA: // CdReadCDDA
		case N_CD_READ_XCDDA: // CdReadXCDDA
		{
			if (cdvdIsDVD())
			{
				DevCon.Warning("CDVD: DVD Read when CD Error");
				cdvd.Error = 0x14; // Invalid for current disc type
				cdvdUpdateReady(CDVD_DRIVE_READY | CDVD_DRIVE_ERROR);
				cdvdSetIrq();
				return;
			}
			// Assign the seek to sector based on cdvd.Param[0]-[3], and the number of  sectors based on cdvd.Param[4]-[7].
			cdvd.SeekToSector = *(u32*)(cdvd.NCMDParam + 0);
			cdvd.nSectors = *(u32*)(cdvd.NCMDParam + 4);
			cdvd.RetryCnt = (cdvd.NCMDParam[8] == 0) ? 0x100 : cdvd.NCMDParam[8];

			u32 oldSpindleCtrl = cdvd.SpindlCtrl;

			if (cdvd.NCMDParam[9] & 0x3F)
				cdvd.SpindlCtrl = cdvd.NCMDParam[9];
			else
				cdvd.SpindlCtrl = (cdvd.NCMDParam[9] & 0x80) | 5; // Max speed for CD

			if (cdvd.NCMDParam[9] & CDVD_SPINDLE_NOMINAL)
				DevCon.Warning("CDVD: CDDA Read using Nominal switch from CAV to CLV, unhandled");

			bool ParamError = false;
			const int oldSpeed = cdvd.Speed;

			switch (cdvd.SpindlCtrl & CDVD_SPINDLE_SPEED)
			{
				case 1: // x1
					cdvd.Speed = 1;
					break;
				case 2: // x2
					cdvd.Speed = 2;
					break;
				case 3: // x4
					cdvd.Speed = 4;
					break;
				case 4: // x12
					cdvd.Speed = 12;
					break;
				case 5: // x24
					cdvd.Speed = 24;
					break;
				default:
					Console.Error("Unknown CDVD Read Speed SpindleCtrl=%x", cdvd.SpindlCtrl);
					ParamError = true;
					break;
			}

			if ((cdvd.SpindlCtrl & CDVD_SPINDLE_CAV) != (oldSpindleCtrl & CDVD_SPINDLE_CAV) || oldSpeed != cdvd.Speed)
			{
				CDVD_LOG("CdRead > Speed change, adding delay");
				cdvd.Spinning = false;
			}

			switch (cdvd.NCMDParam[10])
			{
				case 1:
					cdvd.ReadMode = CDVD_MODE_2368;
					cdvd.BlockSize = 2368;
					break;
				case 0:
					cdvd.ReadMode = CDVD_MODE_2352;
					cdvd.BlockSize = 2352;
					break;
				default:
					ParamError = true;
					break;
			}

			if (ParamError)
			{
				DevCon.Warning("CDVD: CDDA Read Bad Parameter Error");
				cdvd.SpindlCtrl = oldSpindleCtrl;
				cdvd.Error = 0x22; // Invalid Parameter
				cdvd.Action = cdvdAction_Error;
				cdvdUpdateStatus(CDVD_STATUS_SEEK);
				cdvdUpdateReady(CDVD_DRIVE_BUSY);
				CDVD_INT(cdvd.BlockSize * 12);
				break;
			}

			CDVD_LOG("CDRead > startSector=%d, seekTo=%d, nSectors=%d, RetryCnt=%x, Speed=%dx(%s), ReadMode=%x(%x) SpindleCtrl=%x",
				cdvd.Sector, cdvd.SeekToSector, cdvd.nSectors, cdvd.RetryCnt, cdvd.Speed, (cdvd.SpindlCtrl & CDVD_SPINDLE_CAV) ? "CAV" : "CLV", cdvd.ReadMode, cdvd.NCMDParam[10], cdvd.SpindlCtrl);

			if (EmuConfig.CdvdVerboseReads)
				Console.WriteLn(Color_Gray, "CdAudioRead: Reading Sector %07d (%03d Blocks of Size %d) at Speed=%dx(%s) Spindle=%x",
					cdvd.Sector, cdvd.nSectors, cdvd.BlockSize, cdvd.Speed, (cdvd.SpindlCtrl & CDVD_SPINDLE_CAV) ? "CAV" : "CLV", cdvd.SpindlCtrl);

			cdvd.ReadTime = cdvdBlockReadTime(MODE_CDROM);
			CDVDREAD_INT(cdvdStartSeek(cdvd.SeekToSector, MODE_CDROM));

			// Read-ahead by telling CDVD about the track now.
			// This helps improve performance on actual from-cd emulation
			// (ie, not using the hard drive)
			cdvd.RErr = DoCDVDreadTrack(cdvd.SeekToSector, cdvd.ReadMode);

			// Set the reading block flag.  If a seek is pending then Readed will
			// take priority in the handler anyway.  If the read is contiguous then
			// this'll skip the seek delay.
			cdvd.Reading = 1;
			break;
		}
		case N_DVD_READ: // DvdRead
		{
			if (!cdvdIsDVD())
			{
				DevCon.Warning("CDVD: DVD Read when CD Error");
				cdvd.Error = 0x14; // Invalid for current disc type
				cdvdUpdateReady(CDVD_DRIVE_READY | CDVD_DRIVE_ERROR);
				cdvdSetIrq();
				return;
			}
			// Assign the seek to sector based on cdvd.Param[0]-[3], and the number of  sectors based on cdvd.Param[4]-[7].
			cdvd.SeekToSector = *(u32*)(cdvd.NCMDParam + 0);
			cdvd.nSectors = *(u32*)(cdvd.NCMDParam + 4);

			u32 oldSpindleCtrl = cdvd.SpindlCtrl;

			if (cdvd.NCMDParam[8] == 0)
				cdvd.RetryCnt = 0x100;
			else
				cdvd.RetryCnt = cdvd.NCMDParam[8];

			if (cdvd.NCMDParam[9] & 0x3F)
				cdvd.SpindlCtrl = cdvd.NCMDParam[9];
			else
				cdvd.SpindlCtrl = (cdvd.NCMDParam[9] & 0x80) | 3; // Max speed for DVD

			if (cdvd.NCMDParam[9] & CDVD_SPINDLE_NOMINAL)
				DevCon.Warning("CDVD: DVD Read using Nominal switch from CAV to CLV, unhandled");

			bool ParamError = false;
			const int oldSpeed = cdvd.Speed;

			switch (cdvd.SpindlCtrl & CDVD_SPINDLE_SPEED)
			{
				case 1: // x1
					cdvd.Speed = 1;
					break;
				case 2: // x2
					cdvd.Speed = 2;
					break;
				case 3: // x4
					cdvd.Speed = 4;
					break;
				default:
					Console.Error("Unknown CDVD Read Speed SpindleCtrl=%x", cdvd.SpindlCtrl);
					ParamError = true;
					break;
			}

			if ((cdvd.SpindlCtrl & CDVD_SPINDLE_CAV) != (oldSpindleCtrl & CDVD_SPINDLE_CAV) || oldSpeed != cdvd.Speed)
			{
				CDVD_LOG("DvdRead > Speed change, adding delay");
				cdvd.Spinning = false;
			}

			if (cdvd.NCMDParam[10] != 0)
				ParamError = true;

			cdvd.ReadMode = CDVD_MODE_2048;
			cdvd.BlockSize = 2064;

			if (ParamError)
			{
				DevCon.Warning("CDVD: DVD Read Bad Parameter Error");
				cdvd.SpindlCtrl = oldSpindleCtrl;
				cdvd.Error = 0x22; // Invalid Parameter
				cdvd.Action = cdvdAction_Error;
				cdvdUpdateStatus(CDVD_STATUS_SEEK);
				cdvdUpdateReady(CDVD_DRIVE_BUSY);
				CDVD_INT(cdvd.BlockSize * 12);
				break;
			}

			if (!cdvdReadErrorHandler())
			{
				cdvd.Action = cdvdAction_Error;
				cdvdUpdateStatus(CDVD_STATUS_SEEK);
				cdvdUpdateReady(CDVD_DRIVE_BUSY);
				CDVD_INT(cdvdRotationalLatency((CDVD_MODE_TYPE)cdvdIsDVD()));
				break;
			}

			CDVD_LOG("DvdRead > startSector=%d, seekTo=%d nSectors=%d, RetryCnt=%x, Speed=%dx(%s), ReadMode=%x(%x) SpindleCtrl=%x",
				cdvd.Sector, cdvd.SeekToSector, cdvd.nSectors, cdvd.RetryCnt, cdvd.Speed, (cdvd.SpindlCtrl & CDVD_SPINDLE_CAV) ? "CAV" : "CLV", cdvd.ReadMode, cdvd.NCMDParam[10], cdvd.SpindlCtrl);

			if (EmuConfig.CdvdVerboseReads)
				Console.WriteLn(Color_Gray, "DvdRead: Reading Sector %07d (%03d Blocks of Size %d) at Speed=%dx(%s) SpindleCtrl=%x",
					cdvd.SeekToSector, cdvd.nSectors, cdvd.BlockSize, cdvd.Speed, (cdvd.SpindlCtrl & CDVD_SPINDLE_CAV) ? "CAV" : "CLV", cdvd.SpindlCtrl);

			cdvd.ReadTime = cdvdBlockReadTime(MODE_DVDROM);
			CDVDREAD_INT(cdvdStartSeek(cdvd.SeekToSector, MODE_DVDROM));

			// Read-ahead by telling CDVD about the track now.
			// This helps improve performance on actual from-cd emulation
			// (ie, not using the hard drive)
			cdvd.RErr = DoCDVDreadTrack(cdvd.SeekToSector, cdvd.ReadMode);

			// Set the reading block flag.  If a seek is pending then Readed will
			// take priority in the handler anyway.  If the read is contiguous then
			// this'll skip the seek delay.
			cdvd.Reading = 1;
			break;
		}
		case N_CD_GET_TOC: // CdGetToc & cdvdman_call19
			//Param[0] is 0 for CdGetToc and any value for cdvdman_call19
			//the code below handles only CdGetToc!
			//if(cdvd.Param[0]==0x01)
			//{
			DevCon.WriteLn("CDGetToc Param[0]=%d, Param[1]=%d", cdvd.NCMDParam[0], cdvd.NCMDParam[1]);
			//}
			cdvdGetToc(iopPhysMem(HW_DMA3_MADR));
			cdvdSetIrq();
			HW_DMA3_CHCR &= ~0x01000000;
			psxDmaInterrupt(3);
			cdvdUpdateReady(CDVD_DRIVE_READY);
			//After reading the TOC it needs to go back to buffer the next sector
			cdvdUpdateStatus(CDVD_STATUS_PAUSE);
			cdvd.nextSectorsBuffered = 0;
			CDVDSECTORREADY_INT(cdvd.ReadTime);
			break;

		case N_CD_READ_KEY: // CdReadKey
		{
			u8 arg0 = cdvd.NCMDParam[0];
			u16 arg1 = cdvd.NCMDParam[1] | (cdvd.NCMDParam[2] << 8);
			u32 arg2 = cdvd.NCMDParam[3] | (cdvd.NCMDParam[4] << 8) | (cdvd.NCMDParam[5] << 16) | (cdvd.NCMDParam[6] << 24);
			DevCon.WriteLn("cdvdReadKey(%d, %d, %d)", arg0, arg1, arg2);
			cdvdReadKey(arg0, arg1, arg2, cdvd.Key);
			cdvd.KeyXor = 0x00;
			cdvdSetIrq();
			//After reading the key it needs to go back to buffer the next sector
			cdvdUpdateStatus(CDVD_STATUS_PAUSE);
			cdvdUpdateReady(CDVD_DRIVE_READY);
			cdvd.nextSectorsBuffered = 0;
			CDVDSECTORREADY_INT(cdvd.ReadTime);
		}
		break;

		case N_CD_CHG_SPDL_CTRL: // CdChgSpdlCtrl
			Console.WriteLn("sceCdChgSpdlCtrl(%d)", cdvd.NCMDParam[0]);
			cdvdSetIrq();
			break;

		default: // Should be unreachable, handled in the error handler earlier
			Console.Warning("NCMD Unknown %x", rt);
			cdvdSetIrq();
			break;
	}
	cdvd.NCMDParamP = 0;
	cdvd.NCMDParamC = 0;
}

static __fi void cdvdWrite05(u8 rt)
{ // NDATAIN
	CDVD_LOG("cdvdWrite05(NDataIn) %x", rt);

	if (cdvd.NCMDParamP >= 16)
	{
		DevCon.Warning("CDVD: NCMD Overflow");
		cdvd.NCMDParamP = 0;
		cdvd.NCMDParamC = 0;
	}

	cdvd.NCMDParam[cdvd.NCMDParamP++] = rt;
	cdvd.NCMDParamC++;
}

static __fi void cdvdWrite06(u8 rt)
{ // HOWTO
	CDVD_LOG("cdvdWrite06(HowTo) %x", rt);
	cdvd.HowTo = rt;
}

static __fi void cdvdWrite07(u8 rt) // BREAK
{
	CDVD_LOG("cdvdWrite07(Break) %x", rt);

	// If we're already in a Ready state or already Breaking, then do nothing:
	if (!(cdvd.Ready & CDVD_DRIVE_BUSY) || cdvd.AbortRequested)
		return;

	DbgCon.WriteLn("*PCSX2*: CDVD BREAK %x", rt);

	cdvd.AbortRequested = true;
}

static __fi void cdvdWrite08(u8 rt)
{ // INTR_STAT
	CDVD_LOG("cdvdWrite08(IntrReason) = ACK(%x)", rt);
	cdvd.IntrStat &= ~rt;
}

static __fi void cdvdWrite0A(u8 rt)
{ // STATUS
	CDVD_LOG("cdvdWrite0A(Status) %x", rt);
}

static __fi void cdvdWrite0F(u8 rt)
{ // TYPE
	CDVD_LOG("cdvdWrite0F(Type) %x", rt);
	DevCon.WriteLn("*PCSX2*: CDVD TYPE %x", rt);
}

static __fi void cdvdWrite14(u8 rt)
{
	// Rama Or WISI guessed that "2" literally meant 2x but we can get 0x02 or 0xfe for "Standard" or "Fast" it appears. It is unsure what those values are meant to be
	// Tests with ref suggest this register is write only? - Weirdbeard
	if (rt == 0xFE)
		Console.Warning("*PCSX2*: Unimplemented PS1 mode DISC SPEED = FAST");
	else
		Console.Warning("*PCSX2*: Unimplemented PS1 mode DISC SPEED = STANDARD");
}

static __fi void fail_pol_cal()
{
	Console.Error("[MG] ERROR - Make sure the file is already decrypted!!!");
	cdvd.SCMDResult[0] = 0x80;
}

MECHA_RESULT generateCardChallenge()
{
	uint8_t cardIVSeed[8];
	xor_bit(cdvd.memcard_iv, cdvd.memcard_seed, cardIVSeed, 8);

	xor_bit(g_keyStore.CardIV[cdvd.cardKeyIndex], cardIVSeed, cdvd.memcard_key, 8);
	xor_bit(g_keyStore.CardIV2[cdvd.cardKeyIndex], cardIVSeed, &cdvd.memcard_key[8], 8);

	uint8_t key1[16];
	memcpy(key1, g_keyStore.CardKeyLow[cdvd.cardKeyIndex], 8);
	memcpy(&key1[8], g_keyStore.CardKeyHi[cdvd.cardKeyIndex], 8);

	uint8_t key2[16];
	memcpy(key2, g_keyStore.CardKeyLow[cdvd.cardKeyIndex], 8);
	memcpy(&key2[8], g_keyStore.CardKeyHi[cdvd.cardKeyIndex], 8);

	doubleDesEncrypt(key1, cdvd.memcard_key);
	doubleDesEncrypt(key2, &cdvd.memcard_key[8]);

	for (int i = 0; i < 8; i++)
		cdvd.memcard_random[i] = rand();

	xor_bit(g_keyStore.ChallengeIV, cdvd.memcard_random, cdvd.memcard_challenge1, 8);
	doubleDesEncrypt(cdvd.memcard_key, cdvd.memcard_challenge1);

	xor_bit(cdvd.memcard_nonce, cdvd.memcard_challenge1, cdvd.memcard_challenge2, 8);
	doubleDesEncrypt(cdvd.memcard_key, cdvd.memcard_challenge2);

	xor_bit(cdvd.memcard_iv, cdvd.memcard_challenge2, cdvd.memcard_challenge3, 8);
	doubleDesEncrypt(cdvd.memcard_key, cdvd.memcard_challenge3);

	return MECHA_RESULT_CARD_CHALLANGE_GENERATED;
}

MECHA_RESULT verifyCardChallenge()
{
	uint8_t rp1[8];
	memcpy(rp1, cdvd.memcard_reponse1, 8);
	doubleDesDecrypt(cdvd.memcard_key, rp1);
	xor_bit(rp1, g_keyStore.ChallengeIV, rp1, 8);
	if (memcmp(cdvd.memcard_nonce, rp1, 8) != 0)
	{
		Console.Error("Invalid response1");
		return MECHA_RESULT_FAILED;
	}

	uint8_t rp2[8];
	memcpy(rp2, cdvd.memcard_reponse2, 8);
	doubleDesDecrypt(cdvd.memcard_key, rp2);
	xor_bit(rp2, cdvd.memcard_reponse1, rp2, 8);
	if (memcmp(cdvd.memcard_random, rp2, 8) != 0)
	{
		Console.Error("Invalid response2");
		return MECHA_RESULT_FAILED;
	}

	uint8_t rp3[8];
	memcpy(rp3, cdvd.memcard_reponse3, 8);
	doubleDesDecrypt(cdvd.memcard_key, rp3);
	xor_bit(cdvd.memcard_reponse2, rp3, cdvd.CardKey[cdvd.cardKeySlot], 8);

	return MECHA_RESULT_CARD_VERIFIED;
}

static MECHA_RESULT DecryptKelfHeader()
{
	KELFHeader* header = (KELFHeader*)cdvd.data_buffer;
	uint32_t headerSize = sizeof(KELFHeader) + sizeof(ConsoleBan) * header->BanCount;

	if (header->Flags & 1)
		headerSize += cdvd.data_buffer[headerSize] + 1;

	uint8_t HeaderSignature[8];
	memset(HeaderSignature, 0, sizeof(HeaderSignature));
	for (unsigned int i = 0; i < (headerSize & 0xFFFFFFF8); i += 8)
	{
		xor_bit(&cdvd.data_buffer[i], HeaderSignature, HeaderSignature, 8);
		desEncrypt(g_keyStore.SignatureMasterKey, HeaderSignature);
	}
	desDecrypt(g_keyStore.SignatureHashKey, HeaderSignature);
	desEncrypt(g_keyStore.SignatureMasterKey, HeaderSignature);

	if (memcmp(HeaderSignature, &cdvd.data_buffer[headerSize], 8) != 0)
	{
		Console.Error("Invalid HeaderSignature");
		cdvd.mecha_errorcode = 0x84;
		return MECHA_RESULT_FAILED;
	}

	if (header->HeaderSize != cdvd.DataSize)
	{
		Console.Error("Invalid HeaderSize");
		cdvd.mecha_errorcode = 0x81;
		return MECHA_RESULT_FAILED;
	}

	// SystemType, ApplicationType, Flags check is skipped

	if (cdvd.mode == 3 && !(header->Flags & 4) && !(header->Flags & 8))
	{
		cdvd.mecha_errorcode = 0x82;
		return MECHA_RESULT_FAILED;
	}

	uint8_t consoleID[8];
	cdvdReadConsoleID(consoleID);

	uint8_t iLinkID[8];
	cdvdReadILinkID(iLinkID);

	ConsoleBan* bans = (ConsoleBan*)&cdvd.data_buffer[sizeof(KELFHeader)];
	for (int i = 0; i < header->BanCount; ++i)
	{
		if (memcmp(bans[i].iLinkID, iLinkID, 8) == 0)
		{
			if (memcmp(bans[i].consoleID, consoleID, 8) == 0)
			{
				cdvd.mecha_errorcode = 0x85;
				return MECHA_RESULT_FAILED;
			}
		}
	}

	uint32_t offset = headerSize + sizeof(HeaderSignature);

	// Region check is skipped

	// Nonce ban is skipped

	uint8_t Kbit[16];
	if (cdvd.mode == 1 || cdvd.mode == 3)
	{
		memcpy(Kbit, &cdvd.data_buffer[offset], 16);
		offset += 16;
		memcpy(cdvd.Kc, &cdvd.data_buffer[offset], 16);
		offset += 16;

		desDecrypt(cdvd.CardKey[cdvd.cardKeySlot], Kbit);
		desDecrypt(cdvd.CardKey[cdvd.cardKeySlot], &Kbit[8]);
		desDecrypt(cdvd.CardKey[cdvd.cardKeySlot], cdvd.Kc);
		desDecrypt(cdvd.CardKey[cdvd.cardKeySlot], &cdvd.Kc[8]);
	}
	else
	{
		uint8_t Nonce[8];
		xor_bit(cdvd.data_buffer, &cdvd.data_buffer[8], Nonce, 8);

		uint8_t KEK[16];
		xor_bit(g_keyStore.KbitIv, Nonce, KEK, 8);
		doubleDesEncrypt(g_keyStore.KbitMasterKey, KEK);
		xor_bit(g_keyStore.KcIv, Nonce, &KEK[8], 8);
		doubleDesEncrypt(g_keyStore.KcMasterKey, &KEK[8]);

		memcpy(Kbit, &cdvd.data_buffer[offset], 16);
		offset += 16;
		memcpy(cdvd.Kc, &cdvd.data_buffer[offset], 16);
		offset += 16;

		doubleDesDecrypt(KEK, Kbit);
		doubleDesDecrypt(KEK, &Kbit[8]);
		doubleDesDecrypt(KEK, cdvd.Kc);
		doubleDesDecrypt(KEK, &cdvd.Kc[8]);
	}

	cdvd.bitTablePtr = (BitTable*)&cdvd.data_buffer[offset];

	uint8_t BitTableEvenCiphertext[8];
	memcpy(BitTableEvenCiphertext, cdvd.bitTablePtr, 8);

	doubleDesDecrypt(Kbit, cdvd.bitTablePtr);
	xor_bit(g_keyStore.ContentTableIV, cdvd.bitTablePtr, cdvd.bitTablePtr, 8);
	cdvd.lastBitTable = 0;

	int signedBitBlocks = 0;
	for (int i = 0; cdvd.bitTablePtr->BlockCount > i; ++i)
	{
		BitBlock* currentBitBlock = &cdvd.bitTablePtr->Blocks[i];

		uint8_t BitTableOddCiphertext[8];
		memcpy(BitTableOddCiphertext, currentBitBlock, 8);
		doubleDesDecrypt(Kbit, currentBitBlock);
		xor_bit(BitTableEvenCiphertext, currentBitBlock, currentBitBlock, 8);

		memcpy(BitTableEvenCiphertext, currentBitBlock->Signature, 8);
		doubleDesDecrypt(Kbit, currentBitBlock->Signature);
		xor_bit(BitTableOddCiphertext, currentBitBlock->Signature, currentBitBlock->Signature, 8);

		if (currentBitBlock->Flags & BIT_BLOCK_SIGNED || currentBitBlock->Flags & BIT_BLOCK_ENCRYPTED)
		{
			if (cdvd.lastBitTable >= 64)
			{
				Console.Error("Too much bit block!");
				cdvd.mecha_errorcode = 0x81;
				return MECHA_RESULT_FAILED;
			}

			cdvd.bitBlocks[cdvd.lastBitTable].Flags = currentBitBlock->Flags;
			cdvd.bitBlocks[cdvd.lastBitTable].Size = currentBitBlock->Size;
			memcpy(cdvd.bitBlocks[cdvd.lastBitTable].Signature, currentBitBlock->Signature, 8);

			++cdvd.lastBitTable;
			if (currentBitBlock->Flags & BIT_BLOCK_SIGNED)
				++signedBitBlocks;
		}
	}

	if (!signedBitBlocks)
	{
		Console.Error("No signed bit block!");
		cdvd.mecha_errorcode = 0x81;
		return MECHA_RESULT_FAILED;
	}

	uint8_t BitTableSignature[8];
	memcpy(BitTableSignature, Kbit, 8);
	if (memcmp(Kbit, &Kbit[8], 8) != 0)
		xor_bit(&Kbit[8], BitTableSignature, BitTableSignature, 8);

	xor_bit(cdvd.Kc, BitTableSignature, BitTableSignature, 8);
	if (memcmp(cdvd.Kc, &cdvd.Kc[8], 8) != 0)
		xor_bit(&cdvd.Kc[8], BitTableSignature, BitTableSignature, 8);

	for (int i = 0; i < cdvd.bitTablePtr->BlockCount * 2 + 1; i++)
		xor_bit(&((uint8_t*)cdvd.bitTablePtr)[i * 8], BitTableSignature, BitTableSignature, 8);

	uint8_t SignatureMasterHashKey[16];
	memcpy(SignatureMasterHashKey, g_keyStore.SignatureMasterKey, 8);
	memcpy(&SignatureMasterHashKey[8], g_keyStore.SignatureHashKey, 8);

	doubleDesEncrypt(SignatureMasterHashKey, BitTableSignature);

	if (memcmp(&cdvd.data_buffer[offset + 8 + cdvd.bitTablePtr->BlockCount * 16], BitTableSignature, 8) != 0)
	{
		Console.Error("Invalid BitTableSignature!");
		cdvd.mecha_errorcode = 0x84;
		return MECHA_RESULT_FAILED;
	}

	cdvd.bit_length = 16 * cdvd.bitTablePtr->BlockCount + 8;

	uint8_t RootSignature[8];
	memcpy(RootSignature, HeaderSignature, 8);
	desEncrypt(g_keyStore.RootSigMasterKey, RootSignature);
	xor_bit(BitTableSignature, RootSignature, RootSignature, 8);
	desEncrypt(g_keyStore.RootSigMasterKey, RootSignature);
	for (int i = 0; i < cdvd.lastBitTable; ++i)
	{
		if (cdvd.bitBlocks[i].Flags & BIT_BLOCK_SIGNED)
		{
			xor_bit(cdvd.bitBlocks[i].Signature, RootSignature, RootSignature, 8);
			desEncrypt(g_keyStore.RootSigMasterKey, RootSignature);
		}
	}

	uint8_t RootSignatureSource[8];
	memcpy(RootSignatureSource, RootSignature, 8);

	if ((cdvd.mode == 1 || cdvd.mode == 3) && header->Flags & 2)
		doubleDesDecrypt(cdvd.icvps2Key, RootSignature);
	else
		doubleDesDecrypt(g_keyStore.RootSigHashKey, RootSignature);

	if (memcmp(&cdvd.data_buffer[offset + 8 + cdvd.bitTablePtr->BlockCount * 16 + 8], RootSignature, 8) != 0)
	{
		if ((cdvd.mode == 1 || cdvd.mode == 3) && header->Flags & 2)
			cdvd.mecha_errorcode = 0x83;
		else
			cdvd.mecha_errorcode = 0x84;
		return MECHA_RESULT_FAILED;
	}

	if (cdvd.mode == 2 && header->Flags & 2)
	{
		memcpy(cdvd.pub_icvps2, RootSignatureSource, 8);
		doubleDesDecrypt(cdvd.icvps2Key, cdvd.pub_icvps2);
	}

	if (cdvd.mode == 2)
	{
		memcpy(cdvd.pub_Kbit, Kbit, 16);
		memcpy(cdvd.pub_Kc, cdvd.Kc, 16);

		desEncrypt(cdvd.CardKey[cdvd.cardKeySlot], cdvd.pub_Kbit);
		desEncrypt(cdvd.CardKey[cdvd.cardKeySlot], &cdvd.pub_Kbit[8]);
		desEncrypt(cdvd.CardKey[cdvd.cardKeySlot], cdvd.pub_Kc);
		desEncrypt(cdvd.CardKey[cdvd.cardKeySlot], &cdvd.pub_Kc[8]);
	}
	else if (cdvd.mode == 3)
	{
		memcpy(cdvd.pub_Kbit, Kbit, 16);
		memcpy(cdvd.pub_Kc, cdvd.Kc, 16);

		desEncrypt(cdvd.CardKey[cdvd.mode3KeyIndex], cdvd.pub_Kbit);
		desEncrypt(cdvd.CardKey[cdvd.mode3KeyIndex], &cdvd.pub_Kbit[8]);
		desEncrypt(cdvd.CardKey[cdvd.mode3KeyIndex], cdvd.pub_Kc);
		desEncrypt(cdvd.CardKey[cdvd.mode3KeyIndex], &cdvd.pub_Kc[8]);
	}

	memcpy(&cdvd.verifiedKelfHeader, cdvd.data_buffer, sizeof(KELFHeader));
	cdvd.DoneBlocks = 0;
	cdvd.currentBlockIdx = 0;
	if (cdvd.mode == 2 || cdvd.mode == 3)
	{
		while (!(cdvd.bitBlocks[cdvd.currentBlockIdx].Flags & BIT_BLOCK_SIGNED))
			++cdvd.currentBlockIdx;
	}

	return MECHA_RESULT_KELF_HEADER_VERIFED;
}

static MECHA_RESULT DecryptKelfContent()
{
	uint8_t* buffer_ptr = cdvd.data_buffer;
	if (!cdvd.DoneBlocks)
	{
		memcpy(cdvd.ContentLastCiphertext, g_keyStore.ContentIV, 8);
		memset(cdvd.SignatureLastCiphertext, 0, 8);
	}

	int v0 = 0;
	if (cdvd.bitBlocks[cdvd.currentBlockIdx].Flags & BIT_BLOCK_ENCRYPTED)
	{
		while (v0 < cdvd.DataSize)
		{
			if (v0 < cdvd.data_buffer_offset)
			{
				int cryptoType = (cdvd.verifiedKelfHeader.Flags >> 8) & 0xF;
				if (cryptoType == 1) // ECB
				{
					int keyCount = (cdvd.verifiedKelfHeader.Flags >> 4) & 0xF;
					if (keyCount == 1) // Singe
					{
						desDecrypt(cdvd.Kc, buffer_ptr);
					}
					else if (keyCount == 2) // Double
					{
						doubleDesDecrypt(cdvd.Kc, buffer_ptr);
					}
				}
				else if (cryptoType == 2) // CBC
				{
					uint8_t temp[8];
					int keyCount = (cdvd.verifiedKelfHeader.Flags >> 4) & 0xF;
					if (keyCount == 1) // Singe
					{
						memcpy(temp, buffer_ptr, 8);
						desDecrypt(cdvd.Kc, temp);
					}
					else if (keyCount == 2) // Double
					{
						memcpy(temp, buffer_ptr, 8);
						doubleDesDecrypt(cdvd.Kc, temp);
					}

					xor_bit(cdvd.ContentLastCiphertext, temp, temp, 8);

					memcpy(cdvd.ContentLastCiphertext, buffer_ptr, 8);

					memcpy(buffer_ptr, temp, 8);
				}
				else
				{
					cdvd.mecha_errorcode = 0x81;
					return MECHA_RESULT_FAILED; // FIX
				}
				if (cdvd.bitBlocks[cdvd.currentBlockIdx].Flags & BIT_BLOCK_SIGNED)
				{
					xor_bit(buffer_ptr, cdvd.SignatureLastCiphertext, cdvd.SignatureLastCiphertext, 8);
				}
				v0 += 8;
				buffer_ptr += 8;
			}
		}
	}
	else
	{
		while (v0 < cdvd.DataSize)
		{
			if (v0 < cdvd.data_buffer_offset)
			{
				xor_bit(buffer_ptr, cdvd.SignatureLastCiphertext, cdvd.SignatureLastCiphertext, 8);
				desEncrypt(g_keyStore.SignatureMasterKey, cdvd.SignatureLastCiphertext);
				v0 += 8;
				buffer_ptr += 8;
			}
		}
	}
	cdvd.DoneBlocks += v0;

	if (cdvd.bitBlocks[cdvd.currentBlockIdx].Size <= cdvd.DoneBlocks)
	{
		cdvd.DoneBlocks = 0;
		if (cdvd.bitBlocks[cdvd.currentBlockIdx].Flags & BIT_BLOCK_ENCRYPTED)
		{
			uint8_t SignatureMasterHashKey[16];
			memcpy(SignatureMasterHashKey, g_keyStore.SignatureMasterKey, 8);
			memcpy(&SignatureMasterHashKey[8], g_keyStore.SignatureHashKey, 8);

			doubleDesEncrypt(SignatureMasterHashKey, cdvd.SignatureLastCiphertext);
		}
		else
		{
			desDecrypt(g_keyStore.SignatureHashKey, cdvd.SignatureLastCiphertext);
			desEncrypt(g_keyStore.SignatureMasterKey, cdvd.SignatureLastCiphertext);
		}

		if (cdvd.bitBlocks[cdvd.currentBlockIdx].Flags & BIT_BLOCK_SIGNED)
		{
			if (memcmp(cdvd.bitBlocks[cdvd.currentBlockIdx].Signature, cdvd.SignatureLastCiphertext, 8) != 0)
			{
				Console.Error("Signature error!");
				cdvd.mecha_errorcode = 0x84;
				return MECHA_RESULT_FAILED; // FIX
			}
		}

		if (cdvd.mode == 2 || cdvd.mode == 3)
		{
			do
				++cdvd.currentBlockIdx;
			while (!(cdvd.bitBlocks[cdvd.currentBlockIdx].Flags & BIT_BLOCK_SIGNED) && cdvd.currentBlockIdx < cdvd.lastBitTable);
		}
		else
		{
			++cdvd.currentBlockIdx;
		}
	}

	return MECHA_RESULT_KELF_CONTENT_DECRYPTED;
}

static void executeMechaHandler()
{
	switch (cdvd.mecha_state)
	{
		case MECHA_STATE_CARD_NONCE_SET:
			cdvd.mecha_result = generateCardChallenge();
			break;

		case MECHA_STATE_CARD_RESPONSE3_RECEIVED:
			//memset(cdvd.CardKey[cdvd.cardKeySlot], 0xAA, 8);
			//cdvd.mecha_result = MECHA_RESULT_CARD_VERIFIED;
			cdvd.mecha_result = verifyCardChallenge();
			break;

		case MECHA_STATE_KELF_HEADER_RECEIVED:
			cdvd.mecha_result = DecryptKelfHeader();
			break;

		case MECHA_STATE_DATA_IN_LENGTH_SET:
		case MECHA_STATE_KELF_CONTENT_RECEIVED:
			cdvd.mecha_result = DecryptKelfContent();
			break;
	}
}

static void cdvdWrite16(u8 rt) // SCOMMAND
{
	{
		//	cdvdTN	diskInfo;
		//	cdvdTD	trackInfo;
		//	int i, lbn, type, min, sec, frm, address;
		int address;
		u8 tmp;

		CDVD_LOG("cdvdWrite16: SCMD %s (%x) (ParamP = %x)", sCmdName[rt], rt, cdvd.SCMDParamP);

		cdvd.sCommand = rt;
		memzero(cdvd.SCMDResult);

		switch (rt)
		{
				//		case 0x01: // GetDiscType - from cdvdman (0:1)
				//			SetResultSize(1);
				//			cdvd.Result[0] = 0;
				//			break;

			case 0x02: // CdReadSubQ  (0:11)
				SetSCMDResultSize(11);
				cdvd.SCMDResult[0] = cdvdReadSubQ(cdvd.Sector, (cdvdSubQ*)&cdvd.SCMDResult[1]);
				break;

			case 0x03: // Mecacon-command
				switch (cdvd.SCMDParam[0])
				{
					case 0x00: // get mecha version (1:4)
						SetSCMDResultSize(4);
						memcpy(cdvd.SCMDResult, &temp_mechaver[0], 4);
						break;
					case 0x30:
						SetSCMDResultSize(2);
						cdvd.SCMDResult[0] = cdvd.Status;
						cdvd.SCMDResult[1] = (cdvd.Status & 0x1) ? 8 : 0;
						//Console.Warning("Tray check param[1]=%02X", cdvd.Param[1]);
						break;
					case 0x44: // write console ID (9:1)
						SetSCMDResultSize(1);
						cdvdWriteConsoleID(&cdvd.SCMDParam[1]);
						break;

					case 0x45: // read console ID (1:9)
						SetSCMDResultSize(9);
						cdvdReadConsoleID(&cdvd.SCMDResult[1]);
						break;

					case 0xFD: // _sceCdReadRenewalDate (1:6) BCD
						switch (temp_mechaver[1])
						{
							case 0x01:
							case 0x02:
							case 0x03:
								// Mechacon version prior to v5 doesnt support SCMD 03:FD command
								SetSCMDResultSize(1);
								cdvd.SCMDResult[0] = 0x80;
								break;

							case 0x05:
								if (temp_mechaver[2] < 0x10)
								{
									SetSCMDResultSize(6);
									memcpy(cdvd.SCMDResult, &MRenewalDate[temp_mechaver[2] >> 1], 6);
									// 5.6 Mexico differs from other regions 5.6
									if (((temp_mechaver[2] >> 1) == 3) && (temp_mechaver[0] == 0x07))
										memcpy(cdvd.SCMDResult, &MRenewalDate[15], 6);
								}
								else
								{
									SetSCMDResultSize(1);
									cdvd.SCMDResult[0] = 0x80;
								}
								break;

							case 0x06:
								if (temp_mechaver[2] < 0x0E)
								{
									SetSCMDResultSize(6);
									memcpy(cdvd.SCMDResult, &MRenewalDate[(temp_mechaver[2] >> 1) + 8], 6);
								}
								else
								{
									SetSCMDResultSize(1);
									cdvd.SCMDResult[0] = 0x80;
								}
								break;

							default:
								SetSCMDResultSize(1);
								cdvd.SCMDResult[0] = 0x80;
								break;
						}
						break;

					case 0xEF: // read console temperature (1:3)
						// Mechacon version prior to v5.4 doesnt support SCMD 03:EF command
						if ((temp_mechaver[1] < 5) || ((temp_mechaver[1] == 5) && (temp_mechaver[2] < 4)))
						{
							SetSCMDResultSize(1);
							cdvd.SCMDResult[0] = 0x80;
						}
						else
						{
							// This returns a fixed value of 30.5 C
							SetSCMDResultSize(3);
							cdvd.SCMDResult[0] = 0; // returns 0 on success
							cdvd.SCMDResult[1] = 0x0F; // last 8 bits for integer
							cdvd.SCMDResult[2] = 0x05; // leftmost bit for integer, other 7 bits for decimal place
						}
						break;

					default:
						SetSCMDResultSize(1);
						cdvd.SCMDResult[0] = 0x80;
						Console.Warning("*Unknown Mecacon Command param[0]=%02X", cdvd.SCMDParam[0]);
						break;
				}
				break;

			case 0x05: // CdTrayReqState (0:1) - resets the tray open detection
				//Console.Warning("CdTrayReqState. cdvd.Status = %d", cdvd.Status);
				// This function sets the Sticky tray flag to the same value as Status for detecting change
				cdvd.StatusSticky = cdvd.Status & CDVD_STATUS_TRAY_OPEN;

				SetSCMDResultSize(1);
				cdvd.SCMDResult[0] = 0; // Could be a bit to say it's busy, but actual function is unknown, it expects 0 to continue.
				break;

			case 0x06: // CdTrayCtrl  (1:1)
				SetSCMDResultSize(1);
				//Console.Warning( "CdTrayCtrl, param = %d", cdvd.Param[0]);
				if (cdvd.SCMDParam[0] == 0)
					cdvd.SCMDResult[0] = cdvdCtrlTrayOpen();
				else
					cdvd.SCMDResult[0] = cdvdCtrlTrayClose();
				break;

			case 0x08: // CdReadRTC (0:8)
				SetSCMDResultSize(8);
				cdvd.SCMDResult[0] = 0;
				cdvd.SCMDResult[1] = itob(cdvd.RTC.second); //Seconds
				cdvd.SCMDResult[2] = itob(cdvd.RTC.minute); //Minutes
				cdvd.SCMDResult[3] = itob(cdvd.RTC.hour);   //Hours
				cdvd.SCMDResult[4] = 0;                     //Nothing
				cdvd.SCMDResult[5] = itob(cdvd.RTC.day);    //Day
				cdvd.SCMDResult[6] = itob(cdvd.RTC.month);  //Month
				cdvd.SCMDResult[7] = itob(cdvd.RTC.year);   //Year
				/*Console.WriteLn("RTC Read Sec %x Min %x Hr %x Day %x Month %x Year %x", cdvd.Result[1], cdvd.Result[2],
				  cdvd.Result[3], cdvd.Result[5], cdvd.Result[6], cdvd.Result[7]);
				  Console.WriteLn("RTC Read Real Sec %d Min %d Hr %d Day %d Month %d Year %d", cdvd.RTC.second, cdvd.RTC.minute,
				  cdvd.RTC.hour, cdvd.RTC.day, cdvd.RTC.month, cdvd.RTC.year);*/
				break;

			case 0x09: // sceCdWriteRTC (7:1)
				SetSCMDResultSize(1);
				cdvd.SCMDResult[0] = 0;
				cdvd.RTC.pad = 0;

				cdvd.RTC.second = btoi(cdvd.SCMDParam[cdvd.SCMDParamP - 7]);
				cdvd.RTC.minute = btoi(cdvd.SCMDParam[cdvd.SCMDParamP - 6]) % 60;
				cdvd.RTC.hour = btoi(cdvd.SCMDParam[cdvd.SCMDParamP - 5]) % 24;
				cdvd.RTC.day = btoi(cdvd.SCMDParam[cdvd.SCMDParamP - 3]);
				cdvd.RTC.month = btoi(cdvd.SCMDParam[cdvd.SCMDParamP - 2] & 0x7f);
				cdvd.RTC.year = btoi(cdvd.SCMDParam[cdvd.SCMDParamP - 1]);
				/*Console.WriteLn("RTC write incomming Sec %x Min %x Hr %x Day %x Month %x Year %x", cdvd.Param[cdvd.ParamP-7], cdvd.Param[cdvd.ParamP-6],
				  cdvd.Param[cdvd.ParamP-5], cdvd.Param[cdvd.ParamP-3], cdvd.Param[cdvd.ParamP-2], cdvd.Param[cdvd.ParamP-1]);
				  Console.WriteLn("RTC Write Sec %d Min %d Hr %d Day %d Month %d Year %d", cdvd.RTC.second, cdvd.RTC.minute,
				  cdvd.RTC.hour, cdvd.RTC.day, cdvd.RTC.month, cdvd.RTC.year);*/
				//memcpy((u8*)&cdvd.RTC, cdvd.Param, 7);
				break;

			case 0x0A: // sceCdReadNVM (2:3)
				address = (cdvd.SCMDParam[0] << 8) | cdvd.SCMDParam[1];

				if (address < 512)
				{
					SetSCMDResultSize(3);
					cdvdReadNVM(&cdvd.SCMDResult[1], address * 2, 2);
					// swap bytes around
					tmp = cdvd.SCMDResult[1];
					cdvd.SCMDResult[1] = cdvd.SCMDResult[2];
					cdvd.SCMDResult[2] = tmp;
				}
				else
				{
					SetSCMDResultSize(1);
					cdvd.SCMDResult[0] = 0xff;
				}
				break;

			case 0x0B: // sceCdWriteNVM (4:1)
				SetSCMDResultSize(1);
				address = (cdvd.SCMDParam[0] << 8) | cdvd.SCMDParam[1];

				if (address < 512)
				{
					// swap bytes around
					tmp = cdvd.SCMDParam[2];
					cdvd.SCMDParam[2] = cdvd.SCMDParam[3];
					cdvd.SCMDParam[3] = tmp;
					cdvdWriteNVM(&cdvd.SCMDParam[2], address * 2, 2);
				}
				else
				{
					cdvd.SCMDResult[0] = 0xff;
				}
				break;

				//		case 0x0C: // sceCdSetHDMode (1:1)
				//			break;


			case 0x0F: // sceCdPowerOff (0:1)- Call74 from Xcdvdman
				Console.WriteLn(Color_StrongBlack, "sceCdPowerOff called. Resetting VM.");
#ifndef PCSX2_CORE
				GetCoreThread().Reset();
#else
				VMManager::Reset();
#endif
				break;

			case 0x12: // sceCdReadILinkId (0:9)
				SetSCMDResultSize(9);
				cdvdReadILinkID(&cdvd.SCMDResult[1]);
				if ((!cdvd.SCMDResult[3]) && (!cdvd.SCMDResult[4])) // nvm file is missing correct iLinkId, return hardcoded one
				{
					cdvd.SCMDResult[0] = 0x00;
					cdvd.SCMDResult[1] = 0x00;
					cdvd.SCMDResult[2] = 0xAC;
					cdvd.SCMDResult[3] = 0xFF;
					cdvd.SCMDResult[4] = 0xFF;
					cdvd.SCMDResult[5] = 0xFF;
					cdvd.SCMDResult[6] = 0xFF;
					cdvd.SCMDResult[7] = 0xB9;
					cdvd.SCMDResult[8] = 0x86;
				}
				break;

			case 0x13: // sceCdWriteILinkID (8:1)
				SetSCMDResultSize(1);
				cdvdWriteILinkID(&cdvd.SCMDParam[1]);
				break;

			case 0x14: // CdCtrlAudioDigitalOut (1:1)
				//parameter can be 2, 0, ...
				SetSCMDResultSize(1);
				cdvd.SCMDResult[0] = 0; //8 is a flag; not used
				break;

			case 0x15: // sceCdForbidDVDP (0:1)
				//Console.WriteLn("sceCdForbidDVDP");
				SetSCMDResultSize(1);
				cdvd.SCMDResult[0] = 5;
				break;

			case 0x16: // AutoAdjustCtrl - from cdvdman (1:1)
				SetSCMDResultSize(1);
				cdvd.SCMDResult[0] = 0;
				break;

			case 0x17: // CdReadModelNumber (1:9) - from xcdvdman
				SetSCMDResultSize(9);
				cdvdReadModelNumber(&cdvd.SCMDResult[1], cdvd.SCMDParam[0]);
				break;

			case 0x18: // CdWriteModelNumber (9:1) - from xcdvdman
				SetSCMDResultSize(1);
				cdvdWriteModelNumber(&cdvd.SCMDParam[1], cdvd.SCMDParam[0]);
				break;

				//		case 0x19: // sceCdForbidRead (0:1) - from xcdvdman
				//			break;

			case 0x1A: // sceCdBootCertify (4:1)//(4:16 in psx?)
				SetSCMDResultSize(1); //on input there are 4 bytes: 1;?10;J;C for 18000; 1;60;E;C for 39002 from ROMVER
				cdvd.SCMDResult[0] = 1; //i guess that means okay
				break;

			case 0x1B: // sceCdCancelPOffRdy (0:1) - Call73 from Xcdvdman (1:1)
				SetSCMDResultSize(1);
				cdvd.SCMDResult[0] = 0;
				break;

			case 0x1C: // sceCdBlueLEDCtl (1:1) - Call72 from Xcdvdman
				SetSCMDResultSize(1);
				cdvd.SCMDResult[0] = 0;
				break;

				//		case 0x1D: // cdvdman_call116 (0:5) - In V10 Bios
				//			break;

			case 0x1E: // sceRemote2Read (0:5) - // 00 14 AA BB CC -> remote key code
				SetSCMDResultSize(5);
				cdvd.SCMDResult[0] = 0x00;
				cdvd.SCMDResult[1] = 0x14;
				cdvd.SCMDResult[2] = 0x00;
				cdvd.SCMDResult[3] = 0x00;
				cdvd.SCMDResult[4] = 0x00;
				break;

				//		case 0x1F: // sceRemote2_7 (2:1) - cdvdman_call117
				//			break;

			case 0x20: // sceRemote2_6 (0:3)	// 00 01 00
				SetSCMDResultSize(3);
				cdvd.SCMDResult[0] = 0x00;
				cdvd.SCMDResult[1] = 0x01;
				cdvd.SCMDResult[2] = 0x00;
				break;

				//		case 0x21: // sceCdWriteWakeUpTime (8:1)
				//			break;

			case 0x22: // sceCdReadWakeUpTime (0:10)
				SetSCMDResultSize(10);
				cdvd.SCMDResult[0] = 0;
				cdvd.SCMDResult[1] = 0;
				cdvd.SCMDResult[2] = 0;
				cdvd.SCMDResult[3] = 0;
				cdvd.SCMDResult[4] = 0;
				cdvd.SCMDResult[5] = 0;
				cdvd.SCMDResult[6] = 0;
				cdvd.SCMDResult[7] = 0;
				cdvd.SCMDResult[8] = 0;
				cdvd.SCMDResult[9] = 0;
				break;

			case 0x24: // sceCdRCBypassCtrl (1:1) - In V10 Bios
				// FIXME: because PRId<0x23, the bit 0 of sio2 don't get updated 0xBF808284
				SetSCMDResultSize(1);
				cdvd.SCMDResult[0] = 0;
				break;

				//		case 0x25: // cdvdman_call120 (1:1) - In V10 Bios
				//			break;

				//		case 0x26: // cdvdman_call128 (0,3) - In V10 Bios
				//			break;

			case 0x27: // GetPS1BootParam (0:13) - called only by China region PS2 models

				// Return Disc Serial which is passed to PS1DRV and later used to find matching config.
				SetSCMDResultSize(13);
				cdvd.SCMDResult[0] = 0;
				cdvd.SCMDResult[1] = DiscSerial[0];
				cdvd.SCMDResult[2] = DiscSerial[1];
				cdvd.SCMDResult[3] = DiscSerial[2];
				cdvd.SCMDResult[4] = DiscSerial[3];
				cdvd.SCMDResult[5] = DiscSerial[4];
				cdvd.SCMDResult[6] = DiscSerial[5];
				cdvd.SCMDResult[7] = DiscSerial[6];
				cdvd.SCMDResult[8] = DiscSerial[7];
				cdvd.SCMDResult[9] = DiscSerial[9]; // Skipping dot here is required.
				cdvd.SCMDResult[10] = DiscSerial[10];
				cdvd.SCMDResult[11] = DiscSerial[11];
				cdvd.SCMDResult[12] = DiscSerial[12];
				break;

				//		case 0x28: // cdvdman_call150 (1:1) - In V10 Bios
				//			break;

			case 0x29: //sceCdNoticeGameStart (1:1)
				SetSCMDResultSize(1);
				cdvd.SCMDResult[0] = 0;
				break;

				//		case 0x2C: //sceCdXBSPowerCtl (2:2)
				//			break;

				//		case 0x2D: //sceCdXLEDCtl (2:2)
				//			break;
				// mechacon >= 5.6
				//		case 0x2E: //sceCdBuzzerCtl (0:1)
				//			break;
				// mechacon >= 5.6
				//		case 0x2F: //cdvdman_call167 (16:1)
				//			break;

				//		case 0x30: //cdvdman_call169 (1:9)
				//			break;

			case 0x31: //sceCdSetMediumRemoval (1:1)
				SetSCMDResultSize(1);
				cdvd.SCMDResult[0] = 0;
				break;

			case 0x32: //sceCdGetMediumRemoval (0:2)
				SetSCMDResultSize(2);
				cdvd.SCMDResult[0] = 0;
				//cdvd.Result[0] = 0; // fixme: I'm pretty sure that the same variable shouldn't be set twice here. Perhaps cdvd.Result[1]?
				break;

				//		case 0x33: //sceCdXDVRPReset (1:1)
				//			break;

			case 0x36: //cdvdman_call189 [__sceCdReadRegionParams - made up name] (0:15) i think it is 16, not 15
				// Mechacon version prior to v6.0 doesnt support SCMD 36 command
				// except of 5.6 Mexico that has SCMD 36 command partially implemented
				if (temp_mechaver[1] < 6)
				{
					if (!((temp_mechaver[0] == 0x07) && (temp_mechaver[1] == 5) && (temp_mechaver[2] == 6)))
					{
						SetSCMDResultSize(1);
						cdvd.SCMDResult[0] = 0x80;
					}
					else
					{
						SetSCMDResultSize(3);
						DevCon.WriteLn("REGION PARAMS = %s", mg_zones[temp_mechaver[0] & 7]);
						cdvd.SCMDResult[1] = 1 << temp_mechaver[0]; //encryption zone; see offset 0x1C in encrypted headers
						//////////////////////////////////////////
						cdvd.SCMDResult[2] = 0; //??
					}
				}
				else
				{
					SetSCMDResultSize(15);

					cdvdReadRegionParams(&cdvd.SCMDResult[3]); //size==8
					DevCon.WriteLn("REGION PARAMS = %s %s", mg_zones[temp_mechaver[0] & 7], &cdvd.SCMDResult[3]);
					cdvd.SCMDResult[1] = 1 << temp_mechaver[0]; //encryption zone; see offset 0x1C in encrypted headers
					//////////////////////////////////////////
					cdvd.SCMDResult[2] = 0; //??
					//			cdvd.Result[3] == ROMVER[4] == *0xBFC7FF04     J A E H C
					//			cdvd.Result[4] == OSDVER[4] == CAP			   Jjpn, Aeng, Eeng, Heng, Reng, Csch, Kkor, Htch, Aspa
					//			cdvd.Result[5] == OSDVER[5] == small
					//			cdvd.Result[6] == OSDVER[6] == small
					//			cdvd.Result[7] == OSDVER[7] == small
					//			cdvd.Result[8] == VERSTR[0x22] == *0xBFC7FF52  J A E
					//			cdvd.Result[9] == DVDID						   J U O E A R C M
					//			cdvd.Result[10]== 0;					//??
					cdvd.SCMDResult[11] = 0; //??
					cdvd.SCMDResult[12] = 0; //??
					//////////////////////////////////////////
					cdvd.SCMDResult[13] = 0; //0xFF - 77001
					cdvd.SCMDResult[14] = 0; //??
				}
				break;

			case 0x37: //called from EECONF [sceCdReadMAC - made up name] (0:9)
				SetSCMDResultSize(9);
				cdvdReadMAC(&cdvd.SCMDResult[1]);
				break;

			case 0x38: //used to fix the MAC back after accidentally trashed it :D [sceCdWriteMAC - made up name] (8:1)
				SetSCMDResultSize(1);
				cdvdWriteMAC(&cdvd.SCMDParam[0]);
				break;

			case 0x3E: //[__sceCdWriteRegionParams - made up name] (15:1) [Florin: hum, i was expecting 14:1]
				SetSCMDResultSize(1);
				// Mechacon version prior to v6.6 doesnt support SCMD 3E command
				if ((temp_mechaver[1] < 6) || ((temp_mechaver[1] == 6) && (temp_mechaver[2] < 6)))
					cdvd.SCMDResult[0] = 0x80;
				else
					cdvdWriteRegionParams(&cdvd.SCMDParam[2]);
				break;

			case 0x40: // CdOpenConfig (3:1)
				SetSCMDResultSize(1);
				cdvd.CReadWrite = cdvd.SCMDParam[0];
				cdvd.COffset = cdvd.SCMDParam[1];
				cdvd.CNumBlocks = cdvd.SCMDParam[2];
				cdvd.CBlockIndex = 0;
				cdvd.SCMDResult[0] = 0;
				break;

			case 0x41: // CdReadConfig (0:16)
				SetSCMDResultSize(16);
				cdvdReadConfig(&cdvd.SCMDResult[0]);
				break;

			case 0x42: // CdWriteConfig (16:1)
				SetSCMDResultSize(1);
				cdvdWriteConfig(&cdvd.SCMDParam[0]);
				break;

			case 0x43: // CdCloseConfig (0:1)
				SetSCMDResultSize(1);
				cdvd.CReadWrite = 0;
				cdvd.COffset = 0;
				cdvd.CNumBlocks = 0;
				cdvd.CBlockIndex = 0;
				cdvd.SCMDResult[0] = 0;
				break;

			case 0x80: // secrman: __mechacon_auth_0x80
				SetSCMDResultSize(1); //in:1
				cdvd.SCMDResult[0] = 0x80;
				if (cdvd.mecha_state)
				{
					if (cdvd.SCMDParamC == 1)
					{
						cdvd.mecha_state = MECHA_STATE_READY;
						if (cdvd.SCMDParam[0] < 0x10)
							cdvd.SCMDResult[0] = 0;
					}
				}
				break;

			case 0x81: // secrman: __mechacon_auth_0x81
				SetSCMDResultSize(1); //in:1
				cdvd.SCMDResult[0] = 0x80;
				if (cdvd.mecha_state)
				{
					if (cdvd.SCMDParamC == 1)
					{
						cdvd.mecha_state = MECHA_STATE_READY;
						uint8_t cardKeySlot = cdvd.SCMDParam[0] & 0x3F;
						uint8_t cardKeyIndex = (cdvd.SCMDParam[0] >> 6) & 3;
						if (cardKeySlot < 0x10 && cardKeyIndex != 3)
						{
							cdvd.cardKeySlot = cardKeySlot;
							cdvd.cardKeyIndex = cardKeyIndex;
							cdvd.mecha_state = MECHA_STATE_KEY_INDEXES_SET;
							cdvd.SCMDResult[0] = 0;
						}
					}
				}
				break;

			case 0x82: // secrman: __mechacon_auth_0x82
				SetSCMDResultSize(1); //in:16
				if (cdvd.mecha_state == MECHA_STATE_KEY_INDEXES_SET && cdvd.SCMDParamC == 16)
				{
					memcpy(cdvd.memcard_iv, cdvd.SCMDParam, 8);
					memcpy(cdvd.memcard_seed, &cdvd.SCMDParam[8], 8);
					cdvd.mecha_state = MECHA_STATE_CARD_IV_SEED_SET;
					cdvd.SCMDResult[0] = 0;
				}
				else
				{
					cdvd.mecha_state = MECHA_STATE_READY;
					cdvd.SCMDResult[0] = 0x80;
				}
				break;

			case 0x83: // secrman: __mechacon_auth_0x83
				SetSCMDResultSize(1); //in:8
				if (cdvd.mecha_state == MECHA_STATE_CARD_IV_SEED_SET && cdvd.SCMDParamC == 8)
				{
					memcpy(cdvd.memcard_nonce, cdvd.SCMDParam, 8);
					cdvd.mecha_state = MECHA_STATE_CARD_NONCE_SET;
					executeMechaHandler();
					cdvd.SCMDResult[0] = 0;
				}
				else
				{
					cdvd.mecha_state = MECHA_STATE_READY;
					cdvd.SCMDResult[0] = 0x80;
				}
				break;

			case 0x84: // secrman: __mechacon_auth_0x84
				if (cdvd.mecha_state == MECHA_STATE_CARD_CHALLANGE_GENERATED && cdvd.SCMDParamC == 0)
				{
					SetSCMDResultSize(1 + 8 + 4);
					cdvd.SCMDResult[0] = 0;
					memcpy(&cdvd.SCMDResult[1], cdvd.memcard_challenge1, 8);
					memcpy(&cdvd.SCMDResult[9], cdvd.memcard_challenge2, 4);
					cdvd.mecha_state = MECHA_STATE_CARD_CHALLENGE12_SENT;
				}
				else
				{
					SetSCMDResultSize(1);
					cdvd.SCMDResult[0] = 0x80;
					cdvd.mecha_state = MECHA_STATE_READY;
				}
				break;

			case 0x85: // secrman: __mechacon_auth_0x85
				if (cdvd.mecha_state == MECHA_STATE_CARD_CHALLENGE12_SENT && cdvd.SCMDParamC == 0)
				{
					SetSCMDResultSize(1 + 4 + 8);
					cdvd.SCMDResult[0] = 0;
					memcpy(&cdvd.SCMDResult[1], &cdvd.memcard_challenge2[4], 4);
					memcpy(&cdvd.SCMDResult[5], cdvd.memcard_challenge3, 8);
					cdvd.mecha_state = MECHA_STATE_CARD_CHALLENGE23_SENT;
				}
				else
				{
					SetSCMDResultSize(1);
					cdvd.SCMDResult[0] = 0x80;
				}
				break;

			case 0x86: // secrman: __mechacon_auth_0x86
				SetSCMDResultSize(1); //in:16
				if (cdvd.mecha_state == MECHA_STATE_CARD_CHALLENGE23_SENT && cdvd.SCMDParamC == 16)
				{
					memcpy(cdvd.memcard_reponse1, cdvd.SCMDParam, 8);
					memcpy(cdvd.memcard_reponse2, &cdvd.SCMDParam[8], 8);
					cdvd.mecha_state = MECHA_STATE_CARD_RESPONSE12_RECEIVED;
					cdvd.SCMDResult[0] = 0;
				}
				else
				{
					cdvd.SCMDResult[0] = 0x80;
					cdvd.mecha_state = MECHA_STATE_READY;
				}
				break;

			case 0x87: // secrman: __mechacon_auth_0x87
				SetSCMDResultSize(1); //in:8
				if (cdvd.mecha_state == MECHA_STATE_CARD_RESPONSE12_RECEIVED && cdvd.SCMDParamC == 8)
				{
					memcpy(cdvd.memcard_reponse3, cdvd.SCMDParam, 8);
					cdvd.mecha_state = MECHA_STATE_CARD_RESPONSE3_RECEIVED;
					executeMechaHandler();
					cdvd.SCMDResult[0] = 0;
				}
				else
				{
					cdvd.mecha_state = MECHA_STATE_READY;
					cdvd.SCMDResult[0] = 0x80;
				}
				break;

			case 0x88: // secrman: __mechacon_auth_0x88
				SetSCMDResultSize(1); //in:0
				if (cdvd.mecha_state == MECHA_STATE_CARD_VERIFIED && cdvd.SCMDParamC == 0)
				{
					cdvd.SCMDResult[0] = 0;
				}
				else
				{
					cdvd.SCMDResult[0] = 0x80;
				}
				break;

			case 0x8C:
				SetSCMDResultSize(1); //in:0
				executeMechaHandler();
				cdvd.mecha_result = MECHA_RESULT_0;
				cdvd.mecha_state = MECHA_STATE_READY;
				cdvd.SCMDResult[0] = 0;
				break;

			case 0x8D: // sceMgWriteData
				SetSCMDResultSize(1);
				cdvd.SCMDResult[0] = 0x80;

				if (cdvd.SCMDParamC &&
					(cdvd.mecha_state == MECHA_STATE_KELF_HEADER_PARAMS_SET ||
						cdvd.mecha_state == MECHA_STATE_DATA_IN_LENGTH_SET ||
						cdvd.mecha_state == MECHA_STATE_CRYPTO_DATA_IN_SIZE_SET) &&
					cdvd.data_buffer_offset + cdvd.SCMDParamC <= 0x800)
				{
					memcpy(&cdvd.data_buffer[cdvd.data_buffer_offset], cdvd.SCMDParam, cdvd.SCMDParamC);
					cdvd.data_buffer_offset += cdvd.SCMDParamC;
					switch (cdvd.mecha_state)
					{
						case MECHA_STATE_KELF_HEADER_PARAMS_SET:
							if (cdvd.DataSize <= cdvd.data_buffer_offset)
							{
								cdvd.mecha_state = MECHA_STATE_KELF_HEADER_RECEIVED;
								executeMechaHandler();
							}
							break;
						case MECHA_STATE_DATA_IN_LENGTH_SET:
							if (cdvd.DataSize <= cdvd.data_buffer_offset)
							{
								cdvd.mecha_state = MECHA_STATE_KELF_CONTENT_RECEIVED;
								executeMechaHandler();
							}
							break;
						case MECHA_STATE_CRYPTO_DATA_IN_SIZE_SET:
							if (cdvd.DataSize <= cdvd.data_buffer_offset)
							{
								cdvd.mecha_state = MECHA_STATE_CRYPTO_DATA_RECVED;
								executeMechaHandler();
							}
							break;
					}
					cdvd.SCMDResult[0] = 0;
				}
				break;

			case 0x8E: // sceMgReadData
				SetSCMDResultSize(1);
				if (cdvd.SCMDParamC == 0 &&
					(cdvd.mecha_state == MECHA_STATE_BIT_LENGTH_SENT ||
						cdvd.mecha_state == MECHA_STATE_DATA_OUT_LENGTH_SET ||
						cdvd.mecha_state == MECHA_STATE_CRYPTO_DATA_OUT_SIZE_SET))
				{
					uint16_t len = cdvd.DataSize - cdvd.data_out_offset;
					if (len > 0x10)
						len = 0x10;
					SetSCMDResultSize(len);
					for (int i = 0; i < len; i++)
					{
						cdvd.SCMDResult[i] = *cdvd.data_out_ptr++;
					}
					cdvd.data_out_offset += len;

					if (cdvd.DataSize <= cdvd.data_out_offset)
					{
						switch (cdvd.mecha_state)
						{
							case MECHA_STATE_BIT_LENGTH_SENT:
								cdvd.mecha_state = MECHA_STATE_KELF_CONTENT_DECRYPT_IN_PROGRESS;
								break;
							case MECHA_STATE_DATA_OUT_LENGTH_SET:
								if (cdvd.currentBlockIdx >= cdvd.lastBitTable)
									cdvd.mecha_state = MECHA_STATE_READY;
								else
									cdvd.mecha_state = MECHA_STATE_KELF_CONTENT_DECRYPT_IN_PROGRESS;
								break;
							case MECHA_STATE_CRYPTO_DATA_OUT_SIZE_SET:
								cdvd.mecha_state = MECHA_STATE_CRYPTO_KEYGEN_DONE;
								break;
						}
					}
				}
				else
				{
					SetSCMDResultSize(0);
					cdvd.mecha_state = MECHA_STATE_READY;
				}
				break;

			case 0x8F: // secrman: __mechacon_auth_0x8F
				SetSCMDResultSize(1); //in:0
				cdvd.SCMDResult[0] = 0x80;
				if (cdvd.SCMDParamC)
					break;
				switch (cdvd.mecha_state)
				{
					case MECHA_STATE_CARD_NONCE_SET:
					case MECHA_STATE_CARD_CHALLANGE_GENERATED:
						if (cdvd.mecha_result == MECHA_RESULT_CARD_CHALLANGE_GENERATED)
						{
							cdvd.mecha_state = MECHA_STATE_CARD_CHALLANGE_GENERATED;
							cdvd.SCMDResult[0] = 0;
						}
						break;
					case MECHA_STATE_CARD_RESPONSE3_RECEIVED:
					case MECHA_STATE_CARD_VERIFIED:
						if (cdvd.mecha_result == MECHA_RESULT_CARD_VERIFIED)
						{
							cdvd.mecha_state = MECHA_STATE_CARD_VERIFIED;
							cdvd.SCMDResult[0] = 0;
						}
						break;
					case MECHA_STATE_KELF_HEADER_RECEIVED:
					case MECHA_STATE_KELF_HEADER_VERIFED:
						if (cdvd.mecha_result == MECHA_RESULT_KELF_HEADER_VERIFED)
						{
							cdvd.mecha_state = MECHA_STATE_KELF_HEADER_VERIFED;
							cdvd.SCMDResult[0] = 0;
						}
						else if (cdvd.mecha_result == MECHA_RESULT_FAILED)
						{
							cdvd.mecha_state = MECHA_STATE_READY;
							cdvd.SCMDResult[0] = cdvd.mecha_errorcode;
						}
						break;

					case MECHA_STATE_DATA_IN_LENGTH_SET:
					case MECHA_STATE_UNK17:
					case MECHA_STATE_KELF_CONTENT_RECEIVED:
						if (cdvd.mecha_result == MECHA_RESULT_KELF_CONTENT_DECRYPTED)
						{
							if (cdvd.mode == 2 || cdvd.mode == 3)
							{
								if (cdvd.currentBlockIdx >= cdvd.lastBitTable)
									cdvd.mecha_state = MECHA_STATE_KELF_CONTENT_DECRYPT_DONE;
								else
									cdvd.mecha_state = MECHA_STATE_KELF_CONTENT_DECRYPT_IN_PROGRESS;
							}
							else
							{
								cdvd.mecha_state = MECHA_STATE_UNK17;
							}
							cdvd.SCMDResult[0] = 0;
						}
						else if (cdvd.mecha_result == MECHA_RESULT_FAILED)
						{
							cdvd.mecha_state = MECHA_STATE_READY;
							cdvd.SCMDResult[0] = cdvd.mecha_errorcode;
						}
						break;
				}
				break;

			case 0x90: // sceMgWriteHeaderStart
				SetSCMDResultSize(1); //in:5
				cdvd.SCMDResult[0] = 0x80;
				if (cdvd.mecha_state && cdvd.SCMDParamC == 5)
				{
					cdvd.mode = cdvd.SCMDParam[0];
					cdvd.DataSize = *(uint16_t*)&cdvd.SCMDParam[1];
					cdvd.cardKeySlot = cdvd.SCMDParam[3];
					cdvd.mode3KeyIndex = cdvd.SCMDParam[4];
					cdvd.data_buffer_offset = 0;
					cdvd.mecha_state = MECHA_STATE_READY;

					if (cdvd.mode <= 3 && cdvd.DataSize <= 0x800)
					{
						if (cdvd.mode == 0 || (cdvd.cardKeySlot <= 0x10 && ((cdvd.mode == 1 || cdvd.mode == 2) || cdvd.mode3KeyIndex < 0x10)))
						{
							cdvd.mecha_state = MECHA_STATE_KELF_HEADER_PARAMS_SET;
							cdvd.SCMDResult[0] = 0;
						}
					}
				}
				else
				{
					cdvd.mecha_state = MECHA_STATE_READY;
				}
				break;

			case 0x91: // sceMgReadBITLength
			{
				if (cdvd.mecha_state == MECHA_STATE_KELF_HEADER_VERIFED && cdvd.SCMDParamC == 0)
				{
					SetSCMDResultSize(3); //in:0
					cdvd.SCMDResult[0] = 0;
					*(uint16_t*)&cdvd.SCMDResult[1] = cdvd.bit_length;
					cdvd.DataSize = cdvd.bit_length;
					cdvd.data_out_offset = 0;
					cdvd.data_out_ptr = (uint8_t*)cdvd.bitTablePtr;
					cdvd.mecha_state = MECHA_STATE_BIT_LENGTH_SENT;
				}
				else
				{
					SetSCMDResultSize(1); //in:0
					cdvd.SCMDResult[0] = 0x80;
					cdvd.mecha_state = MECHA_STATE_READY;
				}
				break;
			}

			case 0x92: // sceMgWriteDatainLength
				SetSCMDResultSize(1); //in:2
				cdvd.SCMDResult[0] = 0x80;
				if (cdvd.mecha_state == MECHA_STATE_KELF_CONTENT_DECRYPT_IN_PROGRESS && cdvd.SCMDParamC == 2)
				{
					cdvd.DataSize = *(uint16_t*)cdvd.SCMDParam;
					uint16_t len = cdvd.bitBlocks[cdvd.currentBlockIdx].Size - cdvd.DoneBlocks;
					if (len > 0x800)
						len = 0x800;
					if (cdvd.DataSize == len)
					{
						cdvd.data_buffer_offset = 0;
						cdvd.mecha_state = MECHA_STATE_DATA_IN_LENGTH_SET;
						cdvd.SCMDResult[0] = 0;
					}
				}
				else
				{
					cdvd.mecha_state = MECHA_STATE_READY;
				}
				break;

			case 0x93: // sceMgWriteDataoutLength
				SetSCMDResultSize(1); //in:2
				cdvd.SCMDResult[0] = 0x80;
				if (cdvd.mecha_state == MECHA_STATE_UNK17 && cdvd.SCMDParamC == 2)
				{
					if (*(uint16_t*)cdvd.SCMDParam == cdvd.DataSize)
					{
						cdvd.data_out_offset = 0;
						cdvd.data_out_ptr = cdvd.data_buffer;
						cdvd.mecha_state = MECHA_STATE_DATA_OUT_LENGTH_SET;
						cdvd.SCMDResult[0] = 0;
					}
				}
				break;

			case 0x94: // sceMgReadKbit - read first half of BIT key
				if (cdvd.mecha_state == MECHA_STATE_KELF_CONTENT_DECRYPT_DONE && cdvd.SCMDParamC == 0)
				{
					SetSCMDResultSize(1 + 8);
					memcpy(&cdvd.SCMDResult[1], cdvd.pub_Kbit, 8);
					cdvd.mecha_state = MECHA_STATE_KBIT1_SENT;
				}
				else
				{
					SetSCMDResultSize(1);
					cdvd.SCMDResult[0] = 0x80;
					cdvd.mecha_state = MECHA_STATE_READY;
				}
				break;

			case 0x95: // sceMgReadKbit2 - read second half of BIT key
				if (cdvd.mecha_state == MECHA_STATE_KBIT1_SENT && cdvd.SCMDParamC == 0)
				{
					SetSCMDResultSize(1 + 8);
					memcpy(&cdvd.SCMDResult[1], &cdvd.pub_Kbit[8], 8);
					cdvd.mecha_state = MECHA_STATE_KBIT2_SENT;
				}
				else
				{
					SetSCMDResultSize(1);
					cdvd.SCMDResult[0] = 0x80;
					cdvd.mecha_state = MECHA_STATE_READY;
				}
				break;

			case 0x96: // sceMgReadKcon - read first half of content key
				if (cdvd.mecha_state == MECHA_STATE_KBIT2_SENT && cdvd.SCMDParamC == 0)
				{
					SetSCMDResultSize(1 + 8);
					memcpy(&cdvd.SCMDResult[1], cdvd.pub_Kc, 8);
					cdvd.mecha_state = MECHA_STATE_KC1_SENT;
				}
				else
				{
					SetSCMDResultSize(1);
					cdvd.SCMDResult[0] = 0x80;
					cdvd.mecha_state = MECHA_STATE_READY;
				}
				break;

			case 0x97: // sceMgReadKcon2 - read second half of content key
				if (cdvd.mecha_state == MECHA_STATE_KC1_SENT && cdvd.SCMDParamC == 0)
				{
					SetSCMDResultSize(1 + 8);
					memcpy(&cdvd.SCMDResult[1], &cdvd.pub_Kc[8], 8);
					if (cdvd.mode == 2 && cdvd.verifiedKelfHeader.Flags & 2)
						cdvd.mecha_state = MECHA_STATE_KC2_SENT;
					else
						cdvd.mecha_state = MECHA_STATE_READY;
				}
				else
				{
					SetSCMDResultSize(1);
					cdvd.SCMDResult[0] = 0x80;
					cdvd.mecha_state = MECHA_STATE_READY;
				}
				break;

			case 0x98: // sceMgReadKcon2 - read second half of content key
				if (cdvd.mecha_state == MECHA_STATE_KC2_SENT && cdvd.SCMDParamC == 0)
				{
					SetSCMDResultSize(1 + 8);
					memcpy(&cdvd.SCMDResult[1], cdvd.pub_icvps2, 8);
					cdvd.mecha_state = MECHA_STATE_READY;
				}
				else
				{
					SetSCMDResultSize(1);
					cdvd.SCMDResult[0] = 0x80;
					cdvd.mecha_state = MECHA_STATE_READY;
				}
				break;

			default:
				// fake a 'correct' command
				SetSCMDResultSize(1); //in:0
				cdvd.SCMDResult[0] = 0; // 0 complete ; 1 busy ; 0x80 error
				Console.WriteLn("SCMD Unknown %x", rt);
				break;
		} // end switch

		//Console.WriteLn("SCMD - 0x%x\n", rt);
		cdvd.SCMDParamP = 0;
		cdvd.SCMDParamC = 0;
	}
}

static __fi void cdvdWrite17(u8 rt)
{ // SDATAIN
	CDVD_LOG("cdvdWrite17(SDataIn) %x", rt);

	if (cdvd.SCMDParamP >= 16)
	{
		DevCon.Warning("CDVD: SCMD Overflow");
		cdvd.SCMDParamP = 0;
		cdvd.SCMDParamC = 0;
	}

	cdvd.SCMDParam[cdvd.SCMDParamP++] = rt;
	cdvd.SCMDParamC++;
}

static __fi void cdvdWrite18(u8 rt)
{ // SDATAOUT
	CDVD_LOG("cdvdWrite18(SDataOut) %x", rt);
	Console.WriteLn("*PCSX2* SDATAOUT");
}

static __fi void cdvdWrite3A(u8 rt)
{ // DEC-SET
	CDVD_LOG("cdvdWrite3A(DecSet) %x", rt);
	cdvd.decSet = rt;
}

void cdvdWrite(u8 key, u8 rt)
{
	switch (key)
	{
		case 0x04:
			cdvdWrite04(rt);
			break;
		case 0x05:
			cdvdWrite05(rt);
			break;
		case 0x06:
			cdvdWrite06(rt);
			break;
		case 0x07:
			cdvdWrite07(rt);
			break;
		case 0x08:
			cdvdWrite08(rt);
			break;
		case 0x0A:
			cdvdWrite0A(rt);
			break;
		case 0x0F:
			cdvdWrite0F(rt);
			break;
		case 0x14:
			cdvdWrite14(rt);
			break;
		case 0x16:
			cdvdWrite16(rt);
			break;
		case 0x17:
			cdvdWrite17(rt);
			break;
		case 0x18:
			cdvdWrite18(rt);
			break;
		case 0x3A:
			cdvdWrite3A(rt);
			break;
		default:
			Console.Warning("IOP Unknown 8bit write to addr 0x1f4020%x = 0x%x", key, rt);
			break;
	}
}
