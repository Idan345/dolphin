// Copyright 2013 Dolphin Emulator Project
// Licensed under GPLv2
// Refer to the license.txt file included.

#pragma once

#include "UDPWiimote.h"

#include "Common.h"
#include "ControllerEmu.h"
#include "IniFile.h"
#include <string>

class UDPWrapper : public ControllerEmu::ControlGroup
{
public:
	UDPWiimote * inst;
	int index;
	bool updIR, updAccel, updButt, updNun, updNunAccel, udpEn; //upd from update and udp from... well... UDP
	std::string port;

	UDPWrapper(int index, const char* const _name);
	virtual void LoadConfig(IniFile::Section *sec, const std::string& defdev = "", const std::string& base = "");
	virtual void SaveConfig(IniFile::Section *sec, const std::string& defdev = "", const std::string& base = "");
	void Refresh();
	virtual ~UDPWrapper();
};
