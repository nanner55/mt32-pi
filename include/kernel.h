//
// kernel.h
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

#ifndef _kernel_h
#define _kernel_h

#include <circle_stdlib_app.h>
#include <circle/cputhrottle.h>
#include <circle/gpiomanager.h>
#include <circle/i2cmaster.h>
#include <circle/sched/scheduler.h>
#include <circle/spimaster.h>
#include <circle/timer.h>

#include <circle/bcm54213.h>
#include <circle/net/netsubsystem.h>
#include <wlan/bcm4343.h>
#include <wlan/hostap/wpa_supplicant/wpasupplicant.h>

#include "config.h"
#include "mt32pi.h"
#include "zoneallocator.h"

#include "ftpserver.h"

class CKernel : public CStdlibApp
{
public:
	CKernel(void);

	virtual bool Initialize(void) override;
	TShutdownMode Run(void) override;

protected:
	CCPUThrottle m_CPUThrottle;
	CSerialDevice m_Serial;
#ifdef HDMI_CONSOLE
	CScreenDevice m_Screen;
#endif
	CTimer m_Timer;
	CLogger m_Logger;
	CScheduler m_Scheduler;
	CUSBHCIDevice m_USBHCI;
#if RASPPI > 3
	CBcm54213Device m_Bcm54213;
#endif
	CEMMCDevice m_EMMC;
	FATFS m_FileSystem;
	CI2CMaster m_I2CMaster;
	CSPIMaster m_SPIMaster;
	CGPIOManager m_GPIOManager;
	CBcm4343Device m_WLAN;
	CNetSubSystem* m_pNet;
	CWPASupplicant m_WPASupplicant;

private:
	CZoneAllocator m_Allocator;
	CConfig m_Config;
	CMT32Pi m_MT32Pi;
};

#endif
