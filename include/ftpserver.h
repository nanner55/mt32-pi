//
// ftpserver.h
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

#ifndef _ftpserver_h
#define _ftpserver_h

#include <string>
#include <map>
#include <filesystem>

#ifndef MT32_PI_VERSION
#include "ftpstandalone.h"
#else
#include <circle/net/in.h>
#include <circle/net/netsubsystem.h>
#include <circle/net/socket.h>
#endif

#define CMD(op)  \
		op(ABOR) \
		op(CWD)  \
		op(DELE) \
		op(LIST) \
		op(MKD)  \
		op(PASS) \
		op(PASV) \
		op(PORT) \
		op(PWD)  \
		op(QUIT) \
		op(RETR) \
		op(RMD)  \
		op(RNFR) \
		op(RNTO) \
		op(SIZE) \
		op(STOR) \
		op(TYPE) \
		op(USER) \
		         \
		op(NOOP) \
		         \
		op(INV)

#define MAKE_ENUM(e) e,
enum class eCommand
{
	CMD(MAKE_ENUM)
};

// Connection state
struct sState
{
	CSocket *mClientSocket = nullptr;
};

// CFTPServer is a basic, single-threaded FTP server that services one
// passive connection.  Its intended use is for access to
// the SD and (1) USB devices for updating the kernel, soundfonts,
// roms, and other files without having to remove the media.
class CFTPServer
{
public:
	CFTPServer(CNetSubSystem *pNetSubSystem);
	~CFTPServer() = default;

	int  Initialize(u16 portNum = 21);
	void Run();
	void Close();

	static constexpr char Name[] = "ftpserver";

private:
	CSocket mSocket;

	CNetSubSystem *mNetSubSystem;
};

class CClientThread
{
public:
	CClientThread(CNetSubSystem *pNetSubSystem, sState *state);
	~CClientThread();

	int Run();

private:
	struct sCommand
	{
		eCommand Cmd;
		std::string CmdStr;
		std::string Args;
		std::string Resp;
	};

	int DecodeMessage(sCommand &command, std::string message);
	std::filesystem::path GeneratePath(std::string targetPath, std::filesystem::path *newVolume = nullptr, std::filesystem::path *newPath = nullptr);
	
#define MAKE_FUNC(funcName) void Handle##funcName(sCommand &command);
	CMD(MAKE_FUNC);
	
	// state from main thread
	sState *mState;

	// current path
	std::filesystem::path mVolume;
	std::filesystem::path mPath;

	eCommand mMode;
	// PASV
	CSocket *mPasvSocket;
	int mPasv1PortNum;
	int mPasv2PortNum;
	// PORT

	bool mRenamePending;
	std::filesystem::path mRenamePath;
	
	CNetSubSystem *mNetSubSystem;
};

#endif
