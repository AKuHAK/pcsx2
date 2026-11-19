// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once
#include <vector>

#include "DHCP_Options.h"
#include "DEV9/PacketReader/IP/IP_Packet.h"

namespace PacketReader::IP::UDP::DHCP
{
	class DHCP_Packet : public Payload
	{
	public:
		u8 op = 0;
		u8 hardwareType = 0;
		u8 hardwareAddressLength = 0;
		u8 hops = 0;
		u32 transactionID = 0; //xid
		u16 seconds = 0;
		u16 flags = 0;
		IP_Address clientIP{};
		IP_Address yourIP{};
		IP_Address serverIP{};
		IP_Address gatewayIP{};
		u8 clientHardwareAddress[16]{}; //always 16 bytes, regardless of HardwareAddressLength
		//192 bytes of padding
		u32 magicCookie = 0;
		//Assumes ownership of ptrs assigned to it
		std::vector<BaseOption*> options;

		//used by GetLength & WriteBytes
		int maxLength = 576;

		DHCP_Packet() {}
		DHCP_Packet(const u8* buffer, int bufferSize);
		DHCP_Packet(const DHCP_Packet&);

		virtual int GetLength();
		virtual void WriteBytes(u8* buffer, int* offset);
		virtual DHCP_Packet* Clone() const;

		virtual ~DHCP_Packet();
	};
} // namespace PacketReader::IP::UDP::DHCP
