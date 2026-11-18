// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "DEV9/ATA/ATA.h"
#include "DEV9/DEV9.h"

void ATA::DRQCmdPIODataToHost(u8* buff, int buffLen, int buffIndex, int size, bool sendIRQ)
{
	//Data in PIO ready to be sent
	pioPtr = 0;
	pioEnd = size >> 1;

	memcpy(pioBuffer, &buff[buffIndex], size < (buffLen - buffIndex) ? size : (buffLen - buffIndex));

	regStatus &= ~ATA_STAT_BUSY;
	regStatus |= ATA_STAT_DRQ;

	// Only set pendingInterrupt if nIEN is cleared
	if (regControlEnableIRQ && sendIRQ)
	{
		pendingInterrupt = true;
		_DEV9irq(ATA_INTR_INTRQ, 1);
	}
}
void ATA::PostCmdPIODataToHost()
{
	pioPtr = 0;
	pioEnd = 0;
	//AnyMoreData?
	if (pioDRQEndTransferFunc != nullptr)
	{
		regStatus |= ATA_STAT_BUSY;
		regStatus &= ~ATA_STAT_DRQ;
		//Call cmd to retrive more data
		(this->*pioDRQEndTransferFunc)();
	}
	else
		regStatus &= ~ATA_STAT_DRQ;
}

//FromHost
u16 ATA::ATAreadPIO()
{
	//DevCon.WriteLn("DEV9: *ATA_R_DATA 16bit read, pio_count %i,  pio_size %i", pioPtr, pioEnd);
	if (pioPtr < pioEnd)
	{
		const u16 ret = *(u16*)&pioBuffer[pioPtr * 2];
		//DevCon.WriteLn("DEV9: *ATA_R_DATA returned value is  %x", ret);
		pioPtr++;
		if (pioPtr >= pioEnd) //Fnished transfer (Changed from MegaDev9)
			PostCmdPIODataToHost();

		return ret;
	}
	return 0xFF;
}
void ATA::DRQCmdPIODataFromHost(bool sendIRQ)
{
	//Device is ready to accept one sector from the host
	pioPtr = 0;
	pioEnd = 256; //words

	regStatus &= ~ATA_STAT_BUSY;
	regStatus |= ATA_STAT_DRQ;

	// Only set pendingInterrupt if nIEN is cleared. The first block of a PIO
	// data-out command raises no interrupt: the host polls for DRQ instead, and
	// INTRQ only follows each block the device has taken.
	if (regControlEnableIRQ && sendIRQ)
	{
		pendingInterrupt = true;
		_DEV9irq(ATA_INTR_INTRQ, 1);
	}
}

void ATA::PostCmdPIODataFromHost()
{
	pioPtr = 0;
	pioEnd = 0;

	//A whole sector is in the PIO buffer, move it into the write buffer
	memcpy(&currentWrite[wrTransferred], pioBuffer, 512);
	wrTransferred += 512;

	regStatus &= ~ATA_STAT_DRQ;
	regStatus |= ATA_STAT_BUSY;

	if (wrTransferred >= nsector * 512)
	{
		//Whole transfer received, hand it to the disk. Ownership of
		//currentWrite passes to the queue, which frees it.
		WriteQueueEntry entry{0};
		entry.data = currentWrite;
		entry.length = currentWriteLength;
		entry.sector = currentWriteSectors;
		writeQueue.Enqueue(entry);
		currentWrite = nullptr;
		currentWriteLength = 0;
		currentWriteSectors = 0;
		wrTransferred = 0;
		nsectorLeft = 0;

		HDD_SetErrorAtTransferEnd();

		if (fetWriteCacheEnabled)
		{
			regStatus &= ~ATA_STAT_BUSY;
			pendingInterrupt = true;
			if (regControlEnableIRQ)
				_DEV9irq(ATA_INTR_INTRQ, 1);
		}
		else
			awaitFlush = true;

		Async(-1);
	}
	else
	{
		//More to come. Interrupt on each block boundary, mirroring the read path.
		DRQCmdPIODataFromHost(((wrTransferred / 512) % sectorsPerInterrupt) == 0);
	}
}

//FromHost, data-out
void ATA::ATAwritePIO(u16 value)
{
	if (pioPtr < pioEnd)
	{
		*(u16*)&pioBuffer[pioPtr * 2] = value;
		pioPtr++;
		if (pioPtr >= pioEnd) //Finished transfer of this sector
			PostCmdPIODataFromHost();
	}
}

void ATA::HDD_IdentifyDevice()
{
	if (!PreCmd())
		return;
	DevCon.WriteLn("DEV9: HddidentifyDevice");

	//IDE transfer start
	CreateHDDinfo(hddImageSize / 512);

	pioDRQEndTransferFunc = nullptr;
	DRQCmdPIODataToHost(identifyData, 256 * 2, 0, 256 * 2, true);
}

//Read Buffer

void ATA::HDD_ReadMultiple(bool isLBA48)
{
	sectorsPerInterrupt = curMultipleSectorsSetting;
	HDD_ReadPIO(isLBA48);
}

void ATA::HDD_ReadSectors(bool isLBA48)
{
	sectorsPerInterrupt = 1;
	HDD_ReadPIO(isLBA48);
}

void ATA::HDD_ReadPIO(bool isLBA48)
{
	//Log_Info("HDD_ReadPIO");
	if (!PreCmd())
		return;

	if (sectorsPerInterrupt == 0)
	{
		CmdNoDataAbort();
		return;
	}

	IDE_CmdLBA48Transform(isLBA48);

	regStatus &= ~ATA_STAT_SEEK;
	if (!HDD_CanSeek())
	{
		regStatus |= ATA_STAT_ERR;
		regStatusSeekLock = -1;
		regError |= ATA_ERR_ID;
		PostCmdNoData();
		return;
	}
	else
		regStatus |= ATA_STAT_SEEK;

	HDD_ReadSync(&ATA::HDD_ReadPIOS2);
}

void ATA::HDD_ReadPIOS2()
{
	//Log_Info("HDD_ReadPIO Stage 2");
	pioDRQEndTransferFunc = &ATA::HDD_ReadPIOEndBlock;
	DRQCmdPIODataToHost(readBuffer, readBufferLen, 0, 256 * 2, true);
}

void ATA::HDD_ReadPIOEndBlock()
{
	//Log_Info("HDD_ReadPIO End Block");
	rdTransferred += 512;
	if (rdTransferred >= nsector * 512)
	{
		//Log_Info("HDD_ReadPIO Done");
		HDD_SetErrorAtTransferEnd();
		regStatus &= ~ATA_STAT_BUSY;
		pioDRQEndTransferFunc = nullptr;
		rdTransferred = 0;
	}
	else
	{
		if ((rdTransferred / 512) % sectorsPerInterrupt == 0)
			DRQCmdPIODataToHost(readBuffer, readBufferLen, rdTransferred, 256 * 2, true);
		else
			DRQCmdPIODataToHost(readBuffer, readBufferLen, rdTransferred, 256 * 2, false);
	}
}

//Write Buffer

//Write Multiple

void ATA::HDD_WriteMultiple(bool isLBA48)
{
	sectorsPerInterrupt = curMultipleSectorsSetting;
	HDD_WritePIO(isLBA48);
}

//Write Sectors

void ATA::HDD_WriteSectors(bool isLBA48)
{
	sectorsPerInterrupt = 1;
	HDD_WritePIO(isLBA48);
}

void ATA::HDD_WritePIO(bool isLBA48)
{
	if (!PreCmd())
		return;
	DevCon.WriteLn(isLBA48 ? "DEV9: HDD_WritePIO48" : "DEV9: HDD_WritePIO");

	if (sectorsPerInterrupt == 0)
	{
		CmdNoDataAbort();
		return;
	}

	IDE_CmdLBA48Transform(isLBA48);

	regStatus &= ~ATA_STAT_SEEK;
	if (!HDD_CanSeek())
	{
		Console.Error("DEV9: ATA: Transfer to invalid LBA %lu", HDD_GetLBA());
		nsector = -1;
		regStatus |= ATA_STAT_ERR;
		regStatusSeekLock = -1;
		regError |= ATA_ERR_ID;
		PostCmdNoData();
		return;
	}
	else
		regStatus |= ATA_STAT_SEEK;

	if (!HDD_CanAssessOrSetError())
		return;

	nsectorLeft = nsector;
	currentWrite = new u8[nsector * 512];
	currentWriteLength = nsector * 512;
	currentWriteSectors = HDD_GetLBA();
	wrTransferred = 0;

	DRQCmdPIODataFromHost(false);
}

//Download Microcode (Used for FW updates)
