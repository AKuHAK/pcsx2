// SPDX-FileCopyrightText: 2002-2023 PCSX2 Dev Team
// SPDX-License-Identifier: LGPL-3.0+

#include "ATA.h"
#include "DEV9/DEV9.h"

void ATA::WriteUInt16(u8* data, int* index, u16 value)
{
	*(u16*)&data[*index] = value;
	*index += sizeof(value);
}

void ATA::WriteUInt32(u8* data, int* index, u32 value)
{
	*(u32*)&data[*index] = value;
	*index += sizeof(value);
}

void ATA::WriteUInt64(u8* data, int* index, u64 value)
{
	*(u64*)&data[*index] = value;
	*index += sizeof(value);
}

//No null char
void ATA::WritePaddedString(u8* data, int* index, const std::string& value, u32 len)
{
	memset(&data[*index], (u8)' ', len);
	memcpy(&data[*index], value.c_str(), value.length() < len ? value.length() : len);
	*index += len;
}

void ATA::CreateHDDinfo(u64 sizeSectors)
{
	//PS2 is limited to 32bit size HDD (2TB), however,
	//we don't yet support 48bit, so limit to 28bit size
	if (!(lba48Supported))
	{
		constexpr u32 maxSize = (1 << 28) - 1; // 128Gb
		sizeSectors = std::min<u32>(sizeSectors, maxSize);
	}
	else
	{
		constexpr u64 maxSize = (1ULL << 48) - 1; // 2Tb
		sizeSectors = std::min(sizeSectors, maxSize);
	}

	constexpr u16 sectorSize = 512;
	DevCon.WriteLn("DEV9: HddSize : %i", sizeSectors * sectorSize / (1024 * 1024));
	const u64 nbSectors = sizeSectors;
	DevCon.WriteLn("DEV9: nbSectors : %i", nbSectors);

	memset(&identifyData, 0, sizeof(identifyData));

	//General configuration bit-significant information:
	/* ATA-7
	 * bit 0: Reserved
	 * bit 1: Retired
	 * bit 2: Response incomplete
	 * bit 3-5: Retired
	 * bit 6: Obsolete
	 * bit 7: Removable media
	 * bit 8-14: Retired
	 * bit 15: 0 = ATA device
	 */
	int index = 0;
	WriteUInt16(identifyData, &index, 0x0040); //word 0
	//Obsolete
	WriteUInt16(identifyData, &index, 0x3fff); //word 1
	//Specific configuration, c837: Device does not require SET FEATURES subcommand to spin-up after power-up and IDENTIFY DEVICE data is complete
	WriteUInt16(identifyData, &index, 0xc837); //word 2
	//Obsolete
	WriteUInt16(identifyData, &index, 0x0010); //word 3
	//Retired
	index += 2 * 2; //word 4-5
	//Obsolete
	WriteUInt16(identifyData, &index, 0x003f); //word 6
	//Reserved for assignment by the CompactFlash™ Association
	index += 2 * 2; //word 7-8
	//Retired
	index += 1 * 2; //word 9
	//Serial number (20 ASCII characters)
	WritePaddedString(identifyData, &index, "CPXS-2ED9VA-ATH-DD", 20); //word 10-19
	//Retired
	WriteUInt32(identifyData, &index, 0x10000300); //word 20-21
	//Obsolete
	WriteUInt16(identifyData, &index, 0x0039); //word 22
	//Firmware revision (8 ASCII characters)
	WritePaddedString(identifyData, &index, "IFMR01 0", 8); //word 23-26
	//Model number (40 ASCII characters)
	WritePaddedString(identifyData, &index, "CPXS-2ED9VA-ATH-DD", 40); //word 27-46
	//READ/WRITE MULI max sectors (16 sectors)
	WriteUInt16(identifyData, &index, 16 & (0x80 << 8)); //word 47
	//Reserved
	WriteUInt16(identifyData, &index, 0); //word 48
	//Capabilities
	/*
	 * bits 7-0: Retired
	 * bit 8: DMA supported, 1
	 * bit 9: LBA supported, 1
	 * bit 10:IORDY may be disabled, 1
	 * bit 11:IORDY supported, 1
	 * bit 12:Reserved, 0
	 * bit 13:Standby timer values as specified in this standard are supported, 1
	 */
	WriteUInt16(identifyData, &index, ((1 << 13) | (1 << 11) | (1 << 10) | (1 << 9) | (1 << 8))); //word 49
	//Capabilities (bit 14: 1, bit 0-Shall be set to one to indicate a device specific Standby timer value minimum)
	WriteUInt16(identifyData, &index, 1 << 14); //word 50
	//Obsolete
	WriteUInt16(identifyData, &index, 0x0200); //word 51
	//Obsolete
	index += 1 * 2; //word 52
	//
	/*
	 * bit 0: Obsolete
	 * bit 1: Fields in 64:70 are valid (pio3,4 and MWDMA info)
	 * bit 2: Fields in 88 are valid    (UDMA modes)
	 */
	WriteUInt16(identifyData, &index, (1 | (1 << 1) | (1 << 2))); //word 53
	//Obsolete, Word (58:54)
	WriteUInt32(identifyData, &index, 0x00103fff);
	WriteUInt32(identifyData, &index, 0xfc10003f);
	WriteUInt16(identifyData, &index, 0x00fb);
	//PIO READ/WRITE Multiple setting
	/*
	 * bit 7-0: Current setting for number of logical sectors that shall be transferred per DRQ
	 *			data block on READ/WRITE Multiple commands
	 * bit 8: Multiple sector setting is valid
	 */
	WriteUInt16(identifyData, &index, (16 | (1 << 8))); //word 59, 16 from word 47
	//Total number of user addressable logical sectors
	WriteUInt32(identifyData, &index, static_cast<u32>(nbSectors)); //word 60-61
	// Obsolete
	index += 1 * 2; //word 62
	//MDMA Modes
	/*
	 * bits 0-7: Multiword modes supported (0,1,2)
	 * bits 8-15: Transfer mode active
	 */
	DevCon.WriteLn("mdmaMode: " + std::to_string(mdmaMode));
	DevCon.WriteLn("index: " + std::to_string(index / 2));

	if (mdmaMode >= 0)
		WriteUInt16(identifyData, &index, static_cast<u16>(0x07 | (1 << (mdmaMode + 8)))); //word 63, 0x0007
	else
		WriteUInt16(identifyData, &index, 0x07); //word 63
	//Bits 0-1-PIO modes supported (3,4)
	WriteUInt16(identifyData, &index, 0x03); //word 64 (pio3,4 supported)
	//Minimum Multiword DMA transfer cycle time per word, 120ns
	WriteUInt16(identifyData, &index, 0x78); //word 65
	//Manufacturer’s recommended Multiword DMA transfer cycle time, 120ns
	WriteUInt16(identifyData, &index, 0x78); //word 66
	//Minimum PIO transfer cycle time without flow control, 120ns
	WriteUInt16(identifyData, &index, 0x78); //word 67
	//Minimum PIO transfer cycle time with IORDY flow control, 120ns
	WriteUInt16(identifyData, &index, 0x78); //word 68
	//Reserved
	//69-74
	//Queue depth (4bit, Maximum queue depth - 1), not supported
	index = 75 * 2;
	WriteUInt16(identifyData, &index, 0); //word 75
	//Reserved for sata
	//76-79
	index = 80 * 2;
	//Major revision number (supports ATA/ATAPI-6, ATA/ATAPI-5, ATA/ATAPI-4)
	WriteUInt16(identifyData, &index, ((1 << 6) | (1 << 5) | (1 << 4) | 0xe)); //word 80
	//Minor revision number, 0x18 - ATA/ATAPI-6 T13 1410D revision 0
	WriteUInt16(identifyData, &index, 0x18); //word 81, 0x0018
	//Supported Feature Sets (82)
	/*
	 * bit 0: Smart, 1
	 * bit 1: Security Mode, 1
	 * bit 2: Removable media feature set, 0
	 * bit 3: Power management, 1
	 * bit 4: Packet (the CD features), 0
	 * bit 5: Write cache, 1
	 * bit 6: Look-ahead, 1
	 * bit 7: Release interrupt, 0
	 * bit 8: SERVICE interrupt, 0
	 * bit 9: DEVICE RESET interrupt, 1
	 * bit 10: Host Protected Area, 1
	 * bit 11: (Obsolete), 1
	 * bit 12: WRITE BUFFER command, 1
	 * bit 13: READ BUFFER command, 1
	 * bit 14: NOP, 1
	 * bit 15: (Obsolete), 0
	 */
	WriteUInt16(identifyData, &index, ((1 << 14) | (1 << 13) | (1 << 12) | (1 << 11) | (1 << 10) | (1 << 6) | (1 << 5) | (1 << 3) | (1 << 1) | (fetSmartEnabled << 0))); //word 82
	//Supported Feature Sets (83)
	/*
	 * bit 0: DOWNLOAD MICROCODE, 1
	 * bit 1: READ/WRITE DMA QUEUED, 0
	 * bit 2: CFA (Card reader), 0
	 * bit 3: Advanced Power Management, 1
	 * bit 4: Removable Media Status Notifications, 0
	 * bit 5: Power-Up Standby, 0
	 * bit 6: SET FEATURES required to spin up after power-up, 0
	 * bit 7: ??, 0
	 * bit 8: SET MAX security extension, 1
	 * bit 9: Automatic Acoustic Management, 1
	 * bit 10: 48bit LBA, 0
	 * bit 11: Device Configuration Overlay, 1
	 * bit 12: FLUSH CACHE, 0
	 * bit 13: FLUSH CACHE EXT, 0
	 * bit 14: 1
	 */
	WriteUInt16(identifyData, &index, ((1 << 14) | (1 << 11) | (lba48Supported << 10) | (1 << 9) | (1 << 8) | (1 << 3) | 1)); //word 83, 0x4b09
	//Supported Feature Sets (84)
	/*
	 * bit 0: Smart error logging, 1
	 * bit 1: smart self-test, 1
	 * bit 2: Media serial number
	 * bit 3: Media Card Pass Though
	 * bit 4: Streaming feature set
	 * bit 5: General Purpose Logging
	 * bit 6: WRITE DMA FUA EXT & WRITE MULTIPLE FUA EXT
	 * bit 7: WRITE DMA QUEUED FUA EXT
	 * bit 8: 64bit World Wide Name
	 * bit 9: URG bit supported for WRITE STREAM DMA EXT amd WRITE STREAM EXT
	 * bit 10: URG bit supported for READ STREAM DMA EXT amd READ STREAM EXT
	 * bit 13: IDLE IMMEDIATE with UNLOAD FEATURE
	 * bit 14: 1
	 */
	WriteUInt16(identifyData, &index, static_cast<u16>((1 << 14) | (1 << 1) | 1)); //word 84, 0x4003

	//Command set/feature enabled/supported (See word 82)
	WriteUInt16(identifyData, &index, ((1 << 14) | (1 << 13) | (1 << 12) | (1 << 11) | (fetHostProtectedAreaEnabled << 10) | (fetLookAheadEnabled << 6) | (fetWriteCacheEnabled << 5) | (1 << 3) | (fetSecurityEnabled << 1) | (fetSmartEnabled << 0))); //word 85

	//Command set/feature enabled/supported (See word 83)
	// clang-format ofvbnf
	WriteUInt16(identifyData, &index, static_cast<u16>((1 << 11) | //Fixed
													   (lba48Supported << 10) | (1 << 9) | (1 << 0))); //Fixed      //word 86, todo 0x0a01
	//Command set/feature enabled/supported (See word 84)
	WriteUInt16(identifyData, &index, static_cast<u16>((1 << 14) | (1 << 1) | 1));
	// WriteUInt16(identifyData, &index, static_cast<u16>(
	//	(1) |								//Fixed
	//	((1) << 1)));						//Fixed      //word 87, todo 0x4003
	// clang-format ofgfn
	//UDMA modes
	/*
	 * bits 0-7: ultraword modes supported (0,1,2,4,5,6)
	 * bits 8-15: Transfer mode active
	 */
	DevCon.WriteLn("udmaMode: " + std::to_string(udmaMode));
	DevCon.WriteLn("index: " + std::to_string(index / 2));

	if (udmaMode >= 0)
		WriteUInt16(identifyData, &index, static_cast<u16>(0x3f | (1 << (udmaMode + 8)))); //word 88, UDMA 5 supported
	else
		WriteUInt16(identifyData, &index, 0x3f); //word 88
	//Time required for security erase unit completion
	//89, zeroed
	//Time required for Enhanced security erase completion
	//90, zeroed
	//Current advanced power management value
	//91, zeroed
	//Master Password Identifier
	//92
	index = 92 * 2;
	WriteUInt16(identifyData, &index, 0xfffe); //word 92,
	//Hardware reset result. The contents of bits (12:0) of this word shall change only during the execution of a hardware reset.
	/*
	 * bit 0: SMART1
	 * bit 1-2: How Dev0 determined Dev number (01 = Jumper)
	 * bit 3: Dev 0 Passes Diag, 1
	 * bit 4: Dev 0 Detected assertion of PDIAG, 0
	 * bit 5: Dev 0 Detected assertion of DSAP, 0
	 * bit 6: Dev 0 Responds when Dev1 is selected, 0
	 * bit 7: Reserved, 0
	 * bit 8: 0
	 * bit 9-10: How Dev1 determined Dev number, 0
	 * bit 11: Dev1 asserted, 0
	 * bit 12: Reserved, 0
	 * bit 13: Dev detected CBLID above Vih, 1
	 * bit 14: 1
	 */
	WriteUInt16(identifyData, &index, 0x600b); //word 93, 0x600b
	//Vendor’s recommended acoustic management value.
	WriteUInt16(identifyData, &index, 0xc0fe); //word 94, 0xc0fe

	//Stream Minimum Request Size
	//95
	//Streaming Transfer Time - DMA
	//96
	//Streaming Access Latency - DMA and PIO
	//97
	//Streaming Performance Granularity
	//98-99
	//Total Number of User Addressable Sectors for the 48-bit Address feature set.
	if (lba48Supported)
	{
		index = 100 * 2;
		WriteUInt64(identifyData, &index, nbSectors); //word 100-103
		index = 100 * 2;
		WriteUInt16(identifyData, &index, 0); //truncate to 48bits (word 100 = 0)
	}
	//Streaming Transfer Time - PIO
	//104
	//Reserved
	//105
	//Physical sector size / Logical Sector Size
	/*
	 * bit 0-3: 2^X logical sectors per physical sector
	 * bit 12: Logical sector longer than 512 bytes
	 * bit 13: multiple logical sectors per physical sector
	 * bit 14: 1
	 */
	// index = 106 * 2;
	// WriteUInt16(identifyData, &index, static_cast<u16>((1 << 14) | 0)); // word 106, zero
	//Inter-seek delay for ISO-7779acoustic testing in microseconds
	//107
	//WNN
	//108-111
	//Reserved
	//112-115
	//Reserved
	//116
	//Words per Logical Sector
	//117-118
	//Reserved
	//119-126
	//Removable Media Status Notification feature support
	//127
	//Security status
	/*
	 * bit 0: Security supported
	 * bit 1: Security enabled
	 * bit 2: Security locked
	 * bit 3: Security frozen
	 * bit 4: Security count expired
	 * bit 5: Enhanced erase supported
	 * bit 6-7: reserved
	 * bit 8: is Maximum Security
	 */
	//Vendor Specific
	//129-159
	//CFA power mode 1
	//160
	//Reserved for CFA
	//161-175
	//Current media serial number (60 ASCII characters)
	//176-205
	//Reserved
	//206-254
	//Integrity word
	//15:8 Checksum, 7:0 Signature
	CreateHDDinfoCsum();
}
void ATA::CreateHDDinfoCsum()
{
	u8 counter = 0;

	for (int i = 0; i < (512 - 1); i++)
		counter += identifyData[i];

	counter += 0xA5;

	identifyData[510] = 0xA5;
	identifyData[511] = static_cast<u8>(255 - counter + 1);
	counter = 0;

	for (int i = 0; i < (512); i++)
		counter += identifyData[i];

	DevCon.WriteLn("DEV9: %i", counter);
}
