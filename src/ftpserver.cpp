//
// ftpserver.cpp
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

#ifndef MT32_PI_VERSION
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#else
#include <circle/logger.h>
#endif
#include <fcntl.h>
#include <pwd.h>
#include <assert.h>
#include <time.h>
#include <dirent.h>
#include <stdio.h>
#include <unistd.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <vector>
#include <sstream>
#include <cstring>

#include "ftpserver.h"

namespace fs = std::filesystem;

static const int BUFLEN = 1024;
static const int FILE_BUFLEN = 8192;

#define MAKE_ENUM_TO_STRING(e) { eCommand::e, #e },
static std::map<enum eCommand, std::string> CommandToString = {
	CMD(MAKE_ENUM_TO_STRING)
};
#define MAKE_STRING_TO_ENUM(e) { #e, eCommand::e },
static std::map<std::string, enum eCommand> StringToCommand = {
	CMD(MAKE_STRING_TO_ENUM)
};

#ifndef MT32_PI_VERSION
#define DEBUG_PRINT(name, lev, ...) do { fprintf(stderr, "[%30s:%-5d] ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } while(0);
#else
#ifdef HDMI_CONSOLE
static CLogger* pLogger = nullptr;
#define DEBUG_PRINT(name, lev, ...) do { char tBuf[BUFLEN]; snprintf(tBuf, BUFLEN, __VA_ARGS__); pLogger->Write(name, lev, tBuf); } while(0);
#else
#define DEBUG_PRINT(name, lev, ...) do { } while(0);
#endif
// HACK to avoid missing library functions which are not used
char *getcwd(char *buf, size_t size) { return 0; }
long pathconf(const char *path, int name) { return 0; }
#endif

CFTPServer::CFTPServer(CNetSubSystem *pNetSubSystem) :
	mSocket(pNetSubSystem, IPPROTO_TCP),
	mNetSubSystem(pNetSubSystem)
{
}

int CFTPServer::Initialize(u16 portNum)
{
	mSocket.Bind(portNum);

	srand(time(NULL));
	
	return 1;
}

void CFTPServer::Run()
{
#ifdef MT32_PI_VERSION
#ifdef HDMI_CONSOLE
	pLogger = CLogger::Get();
#endif
#endif
	DEBUG_PRINT(CFTPServer::Name, LogWarning, "Listening on socket");
	if (mSocket.Listen(1) < 0) return;

	DEBUG_PRINT(CFTPServer::Name, LogWarning, "Entering Accept loop");
	while (1)
	{
		CSocket *clientSocket;
		CIPAddress clientIp;
		u16 clientPort;
		DEBUG_PRINT(CFTPServer::Name, LogWarning, "Blocking on Accept");
		if (!(clientSocket = mSocket.Accept(&clientIp, &clientPort))) break;
		sState *state = new sState{ clientSocket };

		DEBUG_PRINT(CFTPServer::Name, LogWarning, "Spawing client");

		// could spawn a thread here with client state as argument
		CClientThread client(mNetSubSystem, state);
		
		client.Run();
	}
}

void CFTPServer::Close()
{
}

CClientThread::CClientThread(CNetSubSystem *pNetSubSystem, sState *state) :
	mState(state),
	mVolume(""),
	mPath("/"),
	mMode(eCommand::NOOP),
	mPasvSocket(nullptr),
	mPasv1PortNum(0),
	mPasv2PortNum(0),
	mRenamePending(false),
	mRenamePath(""),
	mNetSubSystem(pNetSubSystem)
{
}

CClientThread::~CClientThread()
{
	delete mState->mClientSocket;
	delete mState;
}

int CClientThread::Run()
{
	std::string welcomeString = "220 Welcome\n";
	DEBUG_PRINT(CFTPServer::Name, LogWarning, "Sending welcome, socket");
	mState->mClientSocket->Send(welcomeString.c_str(), welcomeString.length(), 0);
	
	int bytesRead;
	char buffer[BUFLEN];

	DEBUG_PRINT(CFTPServer::Name, LogWarning, "waiting on client command initial, socket");
	while ((bytesRead = mState->mClientSocket->Receive(buffer, BUFLEN, 0)) >= 0) {
		if (!bytesRead) continue;
		
		DEBUG_PRINT(CFTPServer::Name, LogWarning, "Read bytesRead: %d from clientSocket", bytesRead);
		
		if (bytesRead >= BUFLEN)
		{
			DEBUG_PRINT(CFTPServer::Name, LogWarning, "Read overflow(): %d/%d", bytesRead, BUFLEN);
			return -1;
		}
		
		// force end of string
		buffer[bytesRead] = '\0';
		std::string msg = buffer;
		// remove new lines
		msg.erase(std::remove_if(msg.begin(), msg.end(), [](const char c){ return c == '\n' || c == '\r'; }), msg.end());
		DEBUG_PRINT(CFTPServer::Name, LogWarning, "Req: %s", msg.c_str());		

		sCommand command;
		if (DecodeMessage(command, msg))
		{
			DEBUG_PRINT(CFTPServer::Name, LogWarning, "Bad DecodeMessage");
			return -1;
		}
		
		DEBUG_PRINT(CFTPServer::Name, LogWarning, "Dec: CMD: %s, ARGS: %s", CommandToString[command.Cmd].c_str(), command.Args.c_str());
		
		// handle command
#define MAKE_SWITCH(funcName) case eCommand::funcName: Handle##funcName(command); break;
		switch (command.Cmd) { CMD(MAKE_SWITCH); }

		// generate response
		DEBUG_PRINT(CFTPServer::Name, LogWarning, "Rsp: %s", command.Resp.c_str());
		mState->mClientSocket->Send(command.Resp.c_str(), command.Resp.length(), 0);
		
		DEBUG_PRINT(CFTPServer::Name, LogWarning, "waiting on client command loop");
	}

	return 0;
}

int CClientThread::DecodeMessage(sCommand &command, std::string msg)
{
	command.Cmd = eCommand::INV;
	
	// get pos/length of command end
	auto pos = msg.find_first_of(' ');
	auto cmd = msg.substr(0, pos);
	// if any remaining characters assign to argument 
	auto arg = (pos == std::string::npos) ? "" : msg.substr(pos);
	// remove extra white space
	arg.erase(arg.begin(), std::find_if(arg.begin(), arg.end(), [](const char c){ return !::isspace(c); }));

	auto it = StringToCommand.find(cmd.c_str());
	if (it != StringToCommand.end()) command.Cmd = it->second;

	command.CmdStr = cmd;
	command.Args = arg;

	return 0;
}

std::filesystem::path CClientThread::GeneratePath(std::string targetPath, std::filesystem::path *newVolume, std::filesystem::path *newPath)
{
	std::filesystem::path volume = mVolume;
	std::filesystem::path path = mPath;

	if (!targetPath.empty())
	{
#ifdef MT32_PI_VERSION
		// look for a named volume.
		int pos = targetPath[0] == '/' ? 1 : 0;
		if (!targetPath.compare(pos, 3, "SD:"))
		{
			volume = "SD:";
			targetPath.replace(pos, 3, 3, '/');
		}
		else if (!targetPath.compare(pos, 4, "USB:"))
		{
			volume = "USB:";
			targetPath.replace(pos, 4, 4, '/');
		}
#endif

		if (targetPath[0] == '/')
		{
			path = targetPath;
		}
		else
		{
			path = path / targetPath;
		}
	}
	
	if (newVolume) *newVolume = volume;
	if (newPath) *newPath = path;

	// The std::filesystem compiled for pi doesnt seem to like volume (root) names of the form "<name>:".
	// To work around this concatenate them as strings.	
	return volume.string() + path.string();
}

void CClientThread::HandleABOR(sCommand &command)
{
	// ok to call delete on nullptr
	delete mPasvSocket;
	mPasvSocket = nullptr;
	
    command.Resp = "226 ABOR: success\n";
}

void CClientThread::HandleCWD(sCommand &command)
{
	std::filesystem::path volume;
	std::filesystem::path path;
	// special case the volume and path as arguments since std::filesystem
	// doesn't seem to like roots of the form "<name>:"
	GeneratePath(command.Args, &volume, &path);
	
	DEBUG_PRINT(CFTPServer::Name, LogWarning, "CWD: canonical: volume=%s path=%s args=%s", volume.c_str(), path.c_str(), command.Args.c_str());
		
	auto p = path.string();
	std::vector<std::string> elems;
	for (auto it = p.begin(), it_end = std::find(it, p.end(), '/'); it != p.end(); it = it_end, it_end = std::find(it, p.end(), '/'))
	{
		if (it == it_end)
		{
			// consume '/'
			it_end++;
			DEBUG_PRINT(CFTPServer::Name, LogWarning, "CWD: consume /");
		}
		else
		{	
			std::string item(it, it_end);
			
			if (item == ".." && !elems.empty()) elems.pop_back();
			if (item != ".." && item != ".") {
				elems.push_back(item);
				DEBUG_PRINT(CFTPServer::Name, LogWarning, "CWD: pushing: %s", item.c_str());
			}
		}
	}

	std::string newp = elems.empty() ? "/" : "";
	for (auto &s: elems) newp += "/" + s;
	path = newp;
	
	auto dirPath = volume.string() + path.string();
#ifndef MT32_PI_VERSION	
	DIR *isDir = opendir(dirPath.c_str());
	if (isDir) closedir(isDir);
#else
	FATFS_DIR DIR;
	bool isDir = f_opendir(&DIR, dirPath.c_str()) == FR_OK;
	f_closedir(&DIR);
#endif
	
	if (isDir) {
		mVolume = volume;
		mPath = path;
		command.Resp = "250 CWD: success\n";
	}
	else {
		command.Resp = "550 CWD: failed\n";
	}
}

void CClientThread::HandleDELE(sCommand &command)
{
	auto path = GeneratePath(command.Args);

	if (remove(path))
	{
		command.Resp = "250 \"" + path.string() + "\" DELE: success\n";
	}
	else
	{
		command.Resp = "550 DELE: failed " + path.string() + "\n";
	}
}

void CClientThread::HandleLIST(sCommand &command)
{

    auto path = std::filesystem::path(mVolume.string() + mPath.string());
        
    if (command.Args.length() && command.Args[0] != '-')
    {
		path = GeneratePath(command.Args);
	}
		
	if (mMode == eCommand::PASV)
	{
		CIPAddress ip;
		u16 port;
		DEBUG_PRINT(CFTPServer::Name, LogWarning, "LIST: Pasv Accept()");
		auto socket = mPasvSocket->Accept(&ip, &port);
		command.Resp = "150 LIST: start\n";

		DEBUG_PRINT(CFTPServer::Name, LogWarning, "LIST: readdir start: %s", path.c_str());

#ifndef MT32_PI_VERSION	
		DIR *dir = opendir(path.c_str());
		struct dirent *de;

		while (dir && (de = readdir(dir)))
		{
			auto entryPath = path / de->d_name;
#else
		FATFS_DIR DIR;
		FILINFO FI;
		auto res = f_opendir(&DIR, path.c_str());

		while (res == FR_OK && f_readdir(&DIR, &FI) == FR_OK && FI.fname[0] != 0)
		{
			auto entryPath = path / FI.fname;
#endif

			DEBUG_PRINT(CFTPServer::Name, LogWarning, "LIST: entryPath: %s", entryPath.c_str());
			std::error_code ec;
			char timeBuffer[80];
			using namespace std::chrono;
#ifndef MT32_PI_VERSION
			auto ftime = fs::last_write_time(entryPath);
			auto sctp = time_point_cast<system_clock::duration>(ftime - decltype(ftime)::clock::now() + system_clock::now());
			std::time_t cftime = std::chrono::system_clock::to_time_t(sctp);
			strftime(timeBuffer, 80, "%b %d %H:%M", std::localtime(&cftime));
			
			long linkCount = fs::hard_link_count(entryPath, ec);
			long fileSize = fs::file_size(entryPath, ec);
			
			bool isDir = fs::is_directory(entryPath, ec) && ec.value() != -1;
#else
			strcpy(timeBuffer, "Jan 1 00:00");
			
			FATFS_DIR DIR;
			bool isDir = f_opendir(&DIR, entryPath.c_str()) == FR_OK;
			f_closedir(&DIR);
			
			FILE *fp;
			long linkCount = 1;
			long fileSize = 0;
			if ((fp = fopen(entryPath.c_str(), "r")))
			{
				fseek(fp, 0L, SEEK_END);
				fileSize = ftell(fp);
				fclose(fp);
			}
#endif
			char buffer[BUFLEN];
			auto perms = fs::status(entryPath, ec).permissions();
			auto len = snprintf(buffer, sizeof(buffer),
						"%c%c%c%c%c%c%c%c%c%c %5ld %s %s %8ld %s %s\r\n", 
						isDir ? 'd' : '-',
						((perms & fs::perms::owner_read) != fs::perms::none) ? 'r' : '-',
						((perms & fs::perms::owner_write) != fs::perms::none) ? 'w' : '-',
						((perms & fs::perms::owner_exec) != fs::perms::none) ? 'x' : '-',
						((perms & fs::perms::group_read) != fs::perms::none) ? 'r' : '-',
						((perms & fs::perms::group_write) != fs::perms::none) ? 'w' : '-',
						((perms & fs::perms::group_exec) != fs::perms::none) ? 'x' : '-',
						((perms & fs::perms::others_read) != fs::perms::none) ? 'r' : '-',
						((perms & fs::perms::others_write) != fs::perms::none) ? 'w' : '-',
						((perms & fs::perms::others_exec) != fs::perms::none) ? 'x' : '-',
						linkCount,
						"ftp", 
						"ftp",
						fileSize,
						timeBuffer,
						entryPath.filename().c_str());
			
			if (len >= 0) socket->Send(buffer, len, 0);
			DEBUG_PRINT(CFTPServer::Name, LogWarning, "LIST: string: %s", buffer);
			DEBUG_PRINT(CFTPServer::Name, LogWarning, "LIST: directory entry: %s", entryPath.c_str());
		}
#ifndef MT32_PI_VERSION	
		if (dir) closedir(dir);
#else
		f_closedir(&DIR);
#endif
		
		mState->mClientSocket->Send(command.Resp.c_str(), command.Resp.length(), 0);
		command.Resp = "226 LIST: success\n";
		delete socket;
	}
	else if(mMode == eCommand::PORT)
	{
		command.Resp = "502 LIST: PORT not implemented\n";
		mMode = eCommand::NOOP;
	}
	else
	{
		command.Resp = "425 LIST: missing PASV or PORT\n";
		mMode = eCommand::NOOP;
	}
}

void CClientThread::HandleMKD(sCommand &command)
{
	auto path = GeneratePath(command.Args);
	std::error_code ec;
	
	if (fs::create_directory(path, ec) && ec.value() != -1)
	{
		command.Resp = "257 \"" + path.string() + "\" directory created\n";
	}
	else
	{
		command.Resp = std::string("550 MKD: ") + (ec.value() != -1 && fs::exists(path, ec) && ec.value() != -1 ? "exists\n" : "failed\n");
	}
}

void CClientThread::HandlePASS(sCommand &command)
{
	command.Resp = "230 PASS: accepted\n";
}

void CClientThread::HandlePASV(sCommand &command)
{
	// ok to call delete on nullptr
	delete mPasvSocket;
	mPasvSocket = new CSocket(mNetSubSystem, IPPROTO_TCP);
	
	// generate port numbers
	mPasv1PortNum = 128 + (rand() % 64);;
	mPasv2PortNum = rand() % 256;
		
	int portNum = 256 * mPasv1PortNum + mPasv2PortNum;
	mPasvSocket->Bind(portNum);
	mPasvSocket->Listen(1);
	
	// create socket for passive port and change mode
	mMode = eCommand::PASV;

#ifndef MT32_PI_VERSION
	auto ip = mState->mClientSocket->GetMyIP();
#else
	auto ip = mNetSubSystem->GetConfig()->GetIPAddress()->Get();
#endif
	command.Resp = std::string("227 PASV: success (") + std::to_string(ip[0]) + "," + std::to_string(ip[1]) + "," + std::to_string(ip[2]) + "," + std::to_string(ip[3]) + "," + std::to_string(mPasv1PortNum) + "," + std::to_string(mPasv2PortNum) + ")\n";
}

void CClientThread::HandlePORT(sCommand &command)
{
	command.Resp = "500 PORT: unsupported\n";
}

void CClientThread::HandlePWD(sCommand &command)
{
	command.Resp = "257 \"" + mVolume.string() + mPath.string() + "\"\n";
}

void CClientThread::HandleQUIT(sCommand &command)
{
	// close the socket, but still keep a valid pointer so the Receive() doesn't dereference
	// a bad pointer
	delete mState->mClientSocket;
	mState->mClientSocket = new CSocket(mNetSubSystem, IPPROTO_TCP);

	command.Resp = "221 QUIT: success\n";
}

void CClientThread::HandleRETR(sCommand &command)
{
	auto path = GeneratePath(command.Args);

	if (mMode == eCommand::PASV)
	{
		std::ifstream fs(path.string(), std::ios::in | std::ios::binary);
		if (fs)
		{
			command.Resp = "150 RETR: establishing BINARY connection\n";
			mState->mClientSocket->Send(command.Resp.c_str(), command.Resp.length(), 0);
			
			CIPAddress ip;
			u16 port;
			auto socket = mPasvSocket->Accept(&ip, &port);
			char buffer[FILE_BUFLEN];

			while (fs.read(buffer, FILE_BUFLEN))
			{
				socket->Send(buffer, fs.gcount(), 0);
			}
			if (fs.gcount()) socket->Send(buffer, fs.gcount(), 0);
			fs.close();
						
			command.Resp = fs.eof() ? "226 RETR: success\n" : "550 RETR: failed\n";

			delete socket;
		}
		else
		{
			command.Resp = "550 RETR: failed to open file\n";
		}
	}
	else if (mMode == eCommand::PORT)
	{
        command.Resp = "502 RETR: PORT not implemented\n";
		mMode = eCommand::NOOP;
	}
	else
	{
		command.Resp = "425 RETR: missing PASV or PORT\n";
		mMode = eCommand::NOOP;
	}
	
}

void CClientThread::HandleRMD(sCommand &command)
{
	auto path = GeneratePath(command.Args);
	std::error_code ec;

#ifndef MT32_PI_VERSION	
	DIR *isDir = opendir(path.c_str());
	if (isDir) closedir(isDir);
#else
	FATFS_DIR DIR;
	bool isDir = f_opendir(&DIR, path.c_str()) == FR_OK;
	f_closedir(&DIR);
#endif

	if (isDir && remove(path))
	{
		command.Resp = "250 \"" + path.string() + "\"RMD: success\n";
	}
	else
	{
		command.Resp = "550 RMD: failed\n";
	}
}

void CClientThread::HandleRNFR(sCommand &command)
{
	mRenamePath = GeneratePath(command.Args);

	command.Resp = "350 \"" + mRenamePath.string() + "\"RNFR: pending\n";
	mRenamePending = true;
}

void CClientThread::HandleRNTO(sCommand &command)
{
	auto path = GeneratePath(command.Args);

	std::error_code ec;
	if (mRenamePending)
	{
		fs::rename(mRenamePath, path, ec);
		if (ec.value() != -1)
		{
			command.Resp = "250 \"" + path.string() + "\"RNFR: success\n";
		}
		else {
			command.Resp = "500 RNTO: failed\n";
		}
	}
	else
	{
		command.Resp = "500 RNTO: not pending\n";
	}
}

void CClientThread::HandleSIZE(sCommand &command)
{
	auto path = GeneratePath(command.Args);
	std::error_code ec;	
	auto size = fs::file_size(path, ec);

	if (ec.value() != -1)
	{
		command.Resp = "213 " + std::to_string(size) + "\n";
	}
	else
	{
		command.Resp = "550 SIZE: failed\n";
	}
}

void CClientThread::HandleSTOR(sCommand &command)
{
	auto path = GeneratePath(command.Args);

	if (mMode == eCommand::PASV)
	{
		std::ofstream fs(path.string(), std::ios::out | std::ios::binary);
		if (fs)
		{
			command.Resp = "150 STOR: establishing BINARY connection\n";
			mState->mClientSocket->Send(command.Resp.c_str(), command.Resp.length(), 0);
			
			CIPAddress ip;
			u16 port;
			auto socket = mPasvSocket->Accept(&ip, &port);
			char buffer[FILE_BUFLEN];
			int bytesRead = 0;
			UINT bytesWritten = 0;
			
			while (fs && ((bytesRead = socket->Receive(buffer, FILE_BUFLEN, 0)) > 0))
			{
				fs.write(buffer, bytesRead);
			}

			command.Resp = (bytesRead <= 0) ? "226 STOR: success\n" : "550 STOR: failed read=" + std::to_string(bytesRead) + " written=" + std::to_string(bytesWritten) + "\n";
			fs.close();

			delete socket;
		}
		else
		{
			command.Resp = "550 STOR: failed to open file\n";
		}
	}
	else if (mMode == eCommand::PORT)
	{
        command.Resp = "502 STOR: PORT not implemented\n";
		mMode = eCommand::NOOP;
	}
	else
	{
		command.Resp = "425 STOR: missing PASV or PORT\n";
		mMode = eCommand::NOOP;
	}
	
}

void CClientThread::HandleTYPE(sCommand &command)
{
	// TODO: add ascii support?
	if (command.Args == "I")
	{
		command.Resp = "200 TYPE: binary success\n";
	}
	else if (command.Args == "A")
	{
		command.Resp = "200 TYPE: ascii success\n";
	}
	else {
		command.Resp = "504 TYPE: failed\n";
	}
}

void CClientThread::HandleUSER(sCommand &command)
{
	// all user names accepted
	command.Resp = "331 USER: accepted.  PASS: enter password (all accepted)\n";
}

void CClientThread::HandleNOOP(sCommand &command)
{
	command.Resp = "200 NOOP: success\n";
}

void CClientThread::HandleINV(sCommand &command)
{
	command.Resp = "500 " + command.CmdStr + ": unsupported command\n";
}
