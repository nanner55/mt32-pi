//
// kernel.cpp
//
// mt32-pi - A baremetal MIDI synthesizer for Raspberry Pi
// Copyright (C) 2020-2021 Dale Whinham <daleyo@gmail.com>
//
// This file is part of mt32-pi.
//
// mt32-pi is free software: you can redistribute it and/or modify it under the
// terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later
// version.
//
// mt32-pi is distributed in the hope that it will be useful, but WITHOUT ANY
// WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
// FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
// details.
//
// You should have received a copy of the GNU General Public License along with
// mt32-pi. If not, see <http://www.gnu.org/licenses/>.
//

#include "kernel.h"

#ifndef MT32_PI_VERSION
#define MT32_PI_VERSION "<unknown>"
#endif

#define FIRMWARE_PATH "SD:/firmware/"
#define CONFIG_FILE "SD:/wpa_supplicant.conf"

CKernel::CKernel(void)
	: CStdlibApp("mt32-pi"),

	  m_Serial(&mInterrupt, true),
#ifdef HDMI_CONSOLE
	  m_Screen(mOptions.GetWidth(), mOptions.GetHeight()),
#endif

	  m_Timer(&mInterrupt),
	  m_Logger(mOptions.GetLogLevel(), &m_Timer),
	  m_USBHCI(&mInterrupt, &m_Timer, true),
	  m_EMMC(&mInterrupt, &m_Timer, &mActLED),
	  m_FileSystem{},

	  m_I2CMaster(1, true),
	  m_GPIOManager(&mInterrupt),
	  m_WLAN(FIRMWARE_PATH),
	  m_pNet(nullptr),
	  m_WPASupplicant(CONFIG_FILE),

	  m_MT32Pi(&m_I2CMaster, &m_SPIMaster, &mInterrupt, &m_GPIOManager, &m_Serial, &m_USBHCI)
{
}

bool CKernel::Initialize(void)
{	
	if (!CStdlibApp::Initialize())
		return false;

#ifdef HDMI_CONSOLE
	if (!m_Screen.Initialize())
		return false;
#endif

	CDevice* pLogTarget = mDeviceNameService.GetDevice(mOptions.GetLogDevice(), false);

	if (!pLogTarget)
		pLogTarget = &mNullDevice;

	// Init serial port early if used for logging
	bool bSerialMIDIEnabled = pLogTarget != &m_Serial;
	if (!bSerialMIDIEnabled && !m_Serial.Initialize(115200))
		return false;

	if (!m_Logger.Initialize(pLogTarget))
		return false;

	m_Logger.Write(GetKernelName(), LogError, "Logger init complete");

	if (!m_Timer.Initialize())
		return false;

	if (!m_EMMC.Initialize())
		return false;

	char const* partitionName = "SD:";

	if (f_mount(&m_FileSystem, partitionName, 1) != FR_OK)
	{
		m_Logger.Write(GetKernelName(), LogError, "Cannot mount partition: %s", partitionName);
		return false;
	}

	// Initialize newlib stdio with a reference to Circle's file system
	CGlueStdioInit(m_FileSystem);

	// Load configuration file
	if (!m_Config.Initialize("mt32-pi.cfg"))
		m_Logger.Write(GetKernelName(), LogWarning, "Unable to find or parse config file; using defaults");

	// Init serial port for GPIO MIDI if not being used for logging
	if (bSerialMIDIEnabled && !m_Serial.Initialize(m_Config.MIDIGPIOBaudRate))
		return false;

	// Init I2C; don't bother with Initialize() as it only sets the clock to 100/400KHz
	m_I2CMaster.SetClock(m_Config.SystemI2CBaudRate);

	// Init SPI
	if (!m_SPIMaster.Initialize())
		return false;

	// Init GPIO manager
	if (!m_GPIOManager.Initialize())
		return false;

	if (m_Config.NetEnable
#if defined(__aarch64__) && RASPPI <= 3
	   // 64b RP3 doesn't currently support ethernet so skip net init
       && m_Config.NetWifiEnable
#endif
	)
	{
		m_pNet = new CNetSubSystem(0, 0, 0, 0, DEFAULT_HOSTNAME, m_Config.NetWifiEnable ? NetDeviceTypeWLAN : NetDeviceTypeEthernet);
		bool openEnable = m_Config.NetWifiOpenSSID.length();

		if (m_Config.NetWifiEnable)
		{
			m_Logger.Write(GetKernelName(), LogNotice, "WLAN init begin");
			if (m_Config.NetWifiEnable && !m_WLAN.Initialize())
				return false;
			m_Logger.Write(GetKernelName(), LogNotice, "WLAN init end");

			if (m_Config.NetWifiEnable && openEnable)
			{
				m_Logger.Write(GetKernelName(), LogNotice, "WLAN open init begin");
				if (!m_WLAN.JoinOpenNet(m_Config.NetWifiOpenSSID.c_str()))
					return false;
				m_Logger.Write(GetKernelName(), LogNotice, "WLAN open init end");
			}
		}
		else
		{
			m_Logger.Write(GetKernelName(), LogNotice, "ETH init begin");
#if RASPPI > 3
			// Net also inits this for pi4.  Should this be removed?
			if (!m_Config.NetWifiEnable && !m_Bcm54213.Initialize())
				return false;
#else
#if !defined(__aarch64__) || !defined(LEAVE_QEMU_ON_HALT)
			// The USB driver is not supported under 64-bit QEMU, so
			// the initialization must be skipped in this case, or an
			// exit happens here under 64-bit QEMU.
			if (!m_USBHCI.Initialize())
				return false;
				
#endif
#endif
			m_Logger.Write(GetKernelName(), LogNotice, "ETH init end");
		}

		m_Logger.Write(GetKernelName(), LogNotice, "NET init begin");
		if (!m_pNet->Initialize(m_Config.NetWifiEnable ? FALSE : TRUE))
			return false;
		m_Logger.Write(GetKernelName(), LogNotice, "NET init end");

		if (m_Config.NetWifiEnable && !openEnable)
		{
			m_Logger.Write(GetKernelName(), LogNotice, "WPA init begin");
			if (!m_WPASupplicant.Initialize())
				return false;
			m_Logger.Write(GetKernelName(), LogNotice, "WPA init end");
		}

		m_Logger.Write(GetKernelName(), LogNotice, "NET wait begin");
		while (!m_pNet->IsRunning())
		{
			m_Scheduler.MsSleep(1000);
		}
		m_Logger.Write(GetKernelName(), LogNotice, "NET wait end");
		
		CString IPString;
		m_pNet->GetConfig()->GetIPAddress()->Format(&IPString);

		m_Logger.Write (GetKernelName(), LogNotice, "Net IP: %s", (const char *) IPString);
	}

	// Init custom memory allocator
	if (!m_Allocator.Initialize())
		return false;

	if (!m_MT32Pi.Initialize(bSerialMIDIEnabled, m_pNet))
		return false;

	return true;
}

CStdlibApp::TShutdownMode CKernel::Run(void)
{
	m_Logger.Write(GetKernelName(), LogNotice, "mt32-pi " MT32_PI_VERSION);
	m_Logger.Write(GetKernelName(), LogNotice, "Compile time: " __DATE__ " " __TIME__);

	m_MT32Pi.Run(0);

	return ShutdownReboot;
}
