﻿
/*
Copyright (c) 2009-present Maximus5
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:
1. Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.
3. The name of the authors may not be used to endorse or promote products
   derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE AUTHOR ''AS IS'' AND ANY EXPRESS OR
IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include "../common/defines.h"
#include "../common/MAssert.h"
#include "../common/MStrDup.h"
#include "../common/shlobj.h"
#include "../common/WConsole.h"
#include "../common/WObjects.h"
#include "../common/WUser.h"

#include "WorkerBase.h"

#include "ConProcess.h"
#include "ConsoleArgs.h"
#include "ConsoleMain.h"
#include "ConsoleState.h"
#include "Debugger.h"
#include "DumpOnException.h"
#include "ExitCodes.h"
#include "ExportedFunctions.h"
#include "StdCon.h"
#include "../common/SetEnvVar.h"

/* Console Handles */
MConHandle ghConOut(L"CONOUT$");

WorkerBase* gpWorker = nullptr;

namespace
{
bool IsWin10Build9879()
{
	if (!IsWin10())
		return false;
	_ASSERTE(gpState != NULL);
	return gpState->osVerInfo_.dwBuildNumber == 9879;
}
}

WorkerBase::~WorkerBase()
{
	DoneCreateDumpOnException();
}

WorkerBase::WorkerBase()
	: kernel32(L"kernel32.dll")
	, processes_(std::make_shared<ConProcess>(kernel32))
{
	SetupCreateDumpOnException();

	kernel32.GetProcAddress("GetConsoleKeyboardLayoutNameW", pfnGetConsoleKeyboardLayoutName);
	kernel32.GetProcAddress("GetConsoleDisplayMode", pfnGetConsoleDisplayMode);

	gpLocalSecurity = LocalSecurity();

	// This could be nullptr when process was started as detached
	gpState->realConWnd_ = GetConEmuHWND(2);
	gbVisibleOnStartup = IsWindowVisible(gpState->realConWnd_);
	gnSelfPID = GetCurrentProcessId();
	gdwMainThreadId = GetCurrentThreadId();

	#ifdef _DEBUG
	if (gpState->realConWnd_)
	{
		// This event could be used in debug version of Far Manager
		wchar_t szEvtName[64] = L"";
		swprintf_c(szEvtName, L"FARconEXEC:%08X", LODWORD(gpState->realConWnd_));
		ghFarInExecuteEvent = CreateEvent(0, TRUE, FALSE, szEvtName);
	}
	#endif
}

void WorkerBase::Done(const int /*exitCode*/, const bool /*reportShutdown*/)
{
	dbgInfo.reset();
}

int WorkerBase::ProcessCommandLineArgs()
{
	LogFunction(L"ParseCommandLine{in-progress-base}");

	// ReSharper disable once CppInitializedValueIsAlwaysRewritten
	int iRc = 0;

	if (gpConsoleArgs->isLogging_.exists)
	{
		CreateLogSizeFile(0);
	}
	
	if (gpConsoleArgs->needCdToProfileDir_)
	{
		CdToProfileDir();
	}

	if (gpState->attachMode_ & am_Auto)
	{
		if ((iRc = ParamAutoAttach()) != 0)
			return iRc;
	}

	if (gpConsoleArgs->attachGuiAppWnd_.exists)
	{
		if ((iRc = ParamAttachGuiApp()) != 0)
			return iRc;
	}

	if (gpConsoleArgs->rootPid_.exists)
	{
		if ((iRc = ParamAlienAttachProcess()) != 0)
			return iRc;
	}

	if (gpConsoleArgs->consoleColorIndexes_.exists)
	{
		if ((iRc = ParamColorIndexes()) != 0)
			return iRc;
	}

	if (gpConsoleArgs->conemuPid_.exists)
	{
		if ((iRc = ParamConEmuGuiPid()) != 0)
			return iRc;
	}

	if (gpConsoleArgs->requestNewGuiWnd_.exists)
	{
		if ((iRc = ParamConEmuGuiWnd()))
			return iRc;
	}

	if (gpConsoleArgs->debugPidList_.exists)
	{
		if ((iRc = ParamDebugPid()) != 0)
			return iRc;
	}

	if (gpConsoleArgs->debugExe_.exists || gpConsoleArgs->debugTree_.exists)
	{
		if ((iRc = ParamDebugExeOrTree()) != 0)
			return iRc;
	}

	if (gpConsoleArgs->debugDump_.exists)
	{
		if ((iRc = ParamDebugDump()) != 0)
			return iRc;
	}

	return 0;
}

int WorkerBase::ParamDebugDump()
{
	if (gpConsoleArgs->debugMiniDump_.GetBool())
		SetDebugDumpType(DumpProcessType::MiniDump);
	else if (gpConsoleArgs->debugFullDump_.GetBool())
		SetDebugDumpType(DumpProcessType::FullDump);
	else if (gpConsoleArgs->debugAutoMini_.exists)
		SetDebugAutoDump(gpConsoleArgs->debugAutoMini_.GetStr());
	else // if just "/DUMP"
		SetDebugDumpType(DumpProcessType::AskUser);

	return 0;
}

int WorkerBase::ParamDebugExeOrTree()
{
	const bool debugTree = gpConsoleArgs->debugTree_.GetBool();
	const auto dbgRc = SetDebuggingExe(gpConsoleArgs->command_, debugTree);
	if (dbgRc != 0)
		return dbgRc;
	return 0;
}

int WorkerBase::ParamDebugPid()
{
	const auto dbgRc = SetDebuggingPid(gpConsoleArgs->debugPidList_.GetStr());
	if (dbgRc != 0)
		return dbgRc;
	return 0;
}

int WorkerBase::ParamConEmuGuiWnd() const
{
	_ASSERTE(gpState->runMode_ == RunMode::AutoAttach
		|| gpState->runMode_ == RunMode::Server || gpState->runMode_ == RunMode::AltServer);

	if (gpConsoleArgs->requestNewGuiWnd_.GetBool())
	{
		gpState->hGuiWnd = nullptr;
		_ASSERTE(gpState->conemuPid_ == 0);
		gpState->conemuPid_ = 0;
	}
	else
	{
		wchar_t szLog[120];
		const bool isWnd = gpState->hGuiWnd ? IsWindow(gpState->hGuiWnd) : FALSE;
		const DWORD nErr = gpState->hGuiWnd ? GetLastError() : 0;

		swprintf_c(
			szLog, L"GUI HWND=0x%08X, %s, ErrCode=%u",
			LODWORD(gpState->hGuiWnd), isWnd ? L"Valid" : L"Invalid", nErr);
		LogString(szLog);

		if (!isWnd)
		{
			LogString(L"CERR_CARGUMENT: Invalid GUI HWND was specified in /GHWND arg");
			_printf("Invalid GUI HWND specified: /GHWND");
			_printf("\n" "Command line:\n");
			_wprintf(gpConsoleArgs->fullCmdLine_);
			_printf("\n");
			_ASSERTE(FALSE && "Invalid window was specified in /GHWND arg");
			return CERR_CARGUMENT;
		}

		DWORD nPid = 0;
		GetWindowThreadProcessId(gpState->hGuiWnd, &nPid);
		_ASSERTE(gpState->conemuPid_ == 0 || gpState->conemuPid_ == nPid);
		gpState->conemuPid_ = nPid;
	}

	return 0;
}

int WorkerBase::ParamConEmuGuiPid() const
{
	_ASSERTE(gpState->runMode_ == RunMode::Server || gpState->runMode_ == RunMode::AltServer);

	gpState->conemuPid_ = LODWORD(gpConsoleArgs->conemuPid_.GetInt());

	if (gpState->conemuPid_ == 0)
	{
		LogString(L"CERR_CARGUMENT: Invalid GUI PID specified");
		_printf("Invalid GUI PID specified:\n");
		_wprintf(gpConsoleArgs->fullCmdLine_);
		_printf("\n");
		_ASSERTE(FALSE);
		return CERR_CARGUMENT;
	}

	return 0;
}

// ReSharper disable once CppMemberFunctionMayBeStatic
int WorkerBase::ParamColorIndexes() const
{
	const DWORD nColors = LODWORD(gpConsoleArgs->consoleColorIndexes_.GetInt());

	if (nColors)
	{
		const DWORD nTextIdx = (nColors & 0xFF);
		const DWORD nBackIdx = ((nColors >> 8) & 0xFF);
		const DWORD nPopTextIdx = ((nColors >> 16) & 0xFF);
		const DWORD nPopBackIdx = ((nColors >> 24) & 0xFF);

		if ((nTextIdx <= 15) && (nBackIdx <= 15) && (nTextIdx != nBackIdx))
			gnDefTextColors = MAKECONCOLOR(nTextIdx, nBackIdx);

		if ((nPopTextIdx <= 15) && (nPopBackIdx <= 15) && (nPopTextIdx != nPopBackIdx))
			gnDefPopupColors = MAKECONCOLOR(nPopTextIdx, nPopBackIdx);

		// ReSharper disable once CppLocalVariableMayBeConst
		HANDLE hConOut = ghConOut;
		CONSOLE_SCREEN_BUFFER_INFO csbi5 = {};
		GetConsoleScreenBufferInfo(hConOut, &csbi5);

		if (gnDefTextColors || gnDefPopupColors)
		{
			BOOL bPassed = FALSE;

			if (gnDefPopupColors && IsWin6())
			{
				MY_CONSOLE_SCREEN_BUFFER_INFOEX csbi = { sizeof(csbi) };  // NOLINT(clang-diagnostic-missing-field-initializers)
				if (apiGetConsoleScreenBufferInfoEx(hConOut, &csbi))
				{
					// Microsoft bug? When console is started elevated - it does NOT show
					// required attributes, BUT GetConsoleScreenBufferInfoEx returns them.
					if (!(gpState->attachMode_ & am_Admin)
						&& (!gnDefTextColors || ((csbi.wAttributes = gnDefTextColors)))
						&& (!gnDefPopupColors || ((csbi.wPopupAttributes = gnDefPopupColors))))
					{
						bPassed = TRUE; // nothing to change, console matches
					}
					else
					{
						if (gnDefTextColors)
							csbi.wAttributes = gnDefTextColors;
						if (gnDefPopupColors)
							csbi.wPopupAttributes = gnDefPopupColors;

						_ASSERTE(FALSE && "Continue to SetConsoleScreenBufferInfoEx");

						// Vista/Win7. _SetConsoleScreenBufferInfoEx unexpectedly SHOWS console window
						//if (gnOsVer == 0x0601)
						//{
						//	RECT rcGui = {};
						//	if (gpState->hGuiWnd)
						//		GetWindowRect(gpState->hGuiWnd, &rcGui);
						//	//SetWindowPos(gpState->realConWnd, HWND_BOTTOM, rcGui.left+3, rcGui.top+3, 0,0, SWP_NOSIZE|SWP_SHOWWINDOW|SWP_NOZORDER);
						//	SetWindowPos(gpState->realConWnd, NULL, -30000, -30000, 0,0, SWP_NOSIZE|SWP_SHOWWINDOW|SWP_NOZORDER);
						//	apiShowWindow(gpState->realConWnd, SW_SHOWMINNOACTIVE);
						//	#ifdef _DEBUG
						//	apiShowWindow(gpState->realConWnd, SW_SHOWNORMAL);
						//	apiShowWindow(gpState->realConWnd, SW_HIDE);
						//	#endif
						//}

						bPassed = apiSetConsoleScreenBufferInfoEx(hConOut, &csbi);

						// Fix problems of Windows 7
						if (!gbVisibleOnStartup)
						{
							apiShowWindow(gpState->realConWnd_, SW_HIDE);
						}
					}
				}
			}


			if (!bPassed && gnDefTextColors)
			{
				SetConsoleTextAttribute(hConOut, gnDefTextColors);
				RefillConsoleAttributes(csbi5, csbi5.wAttributes, gnDefTextColors);
			}
		}
	}

	return 0;
}

int WorkerBase::ParamAlienAttachProcess()
{
	SetRootProcessId(LODWORD(gpConsoleArgs->rootPid_.GetInt()));

	if ((RootProcessId() == 0) && gpConsoleArgs->creatingHiddenConsole_)
	{
		SetRootProcessId(Processes().WaitForRootConsoleProcess(30000));
	}

	if (gpConsoleArgs->alternativeAttach_ && RootProcessId())
	{
		// if process was started "with console window"
		if (gpState->realConWnd_)
		{
			DEBUGTEST(SafeCloseHandle(ghFarInExecuteEvent));
		}

		const BOOL bAttach = StdCon::AttachParentConsole(RootProcessId());
		if (!bAttach)
		{
			gbInShutdown = TRUE;
			gpState->alwaysConfirmExit_ = FALSE;
			LogString(L"CERR_CARGUMENT: (gbAlternativeAttach && RootProcessId())");
			return CERR_CARGUMENT;
		}

		// Need to be set, because of new console === new handler
		SetConsoleCtrlHandler(HandlerRoutine, true);

		_ASSERTE(ghFarInExecuteEvent == nullptr);
		_ASSERTE(gpState->realConWnd_ != nullptr);
	}
	else if (RootProcessId() == 0)
	{
		LogString("CERR_CARGUMENT: Attach to GUI was requested, but invalid PID specified");
		_printf("Attach to GUI was requested, but invalid PID specified:\n");
		_wprintf(gpConsoleArgs->fullCmdLine_);
		_printf("\n");
		_ASSERTE(FALSE && "Attach to GUI was requested, but invalid PID specified");
		return CERR_CARGUMENT;
	}

	return 0;
}

int WorkerBase::ParamAttachGuiApp()
{
	if (!gpConsoleArgs->attachGuiAppWnd_.IsValid())
	{
		LogString(L"CERR_CARGUMENT: Invalid Child HWND was specified in /GuiAttach arg");
		_printf("Invalid Child HWND specified: /GuiAttach");
		_printf("\n" "Command line:\n");
		_wprintf(gpConsoleArgs->fullCmdLine_);
		_printf("\n");
		_ASSERTE(FALSE && "Invalid window was specified in /GuiAttach arg");
		return CERR_CARGUMENT;
	}

	const HWND2 hAppWnd{ LODWORD(gpConsoleArgs->attachGuiAppWnd_.value) };
	if (IsWindow(hAppWnd))
		SetRootProcessGui(hAppWnd);

	return 0;
}

int WorkerBase::ParamAutoAttach() const
{
	//ConEmu autorun (c) Maximus5
	//Starting "%ConEmuPath%" in "Attach" mode (NewWnd=%FORCE_NEW_WND%)

	if (!gpConsoleArgs->IsAutoAttachAllowed())
	{
		if (gpState->realConWnd_ && IsWindowVisible(gpState->realConWnd_))
		{
			_printf("AutoAttach was requested, but skipped\n");
		}
		gpState->DisableAutoConfirmExit();
		//_ASSERTE(FALSE && "AutoAttach was called while Update process is in progress?");
		return CERR_AUTOATTACH_NOT_ALLOWED;
	}

	return 0;
}

bool WorkerBase::IsCmdK() const
{
	return false;
}

void WorkerBase::SetCmdK(bool useCmdK)
{
	_ASSERTE(useCmdK == false);
	// nothing to do in base!
}

void WorkerBase::SetRootProcessId(const DWORD pid)
{
	_ASSERTE(pid != 0);
	this->rootProcess.processId = pid;
}

void WorkerBase::SetRootProcessHandle(HANDLE processHandle)
{
	this->rootProcess.processHandle = processHandle;
}

void WorkerBase::SetRootThreadId(const DWORD tid)
{
	this->rootProcess.threadId = tid;
}

void WorkerBase::SetRootThreadHandle(HANDLE threadHandle)
{
	this->rootProcess.threadHandle = threadHandle;
}

void WorkerBase::SetRootStartTime(const DWORD ticks)
{
	this->rootProcess.startTime = ticks;
}

void WorkerBase::SetParentFarPid(DWORD pid)
{
	this->farManagerInfo.dwParentFarPid = pid;
}

void WorkerBase::SetRootProcessGui(HWND hwnd)
{
	this->rootProcess.childGuiWnd = hwnd;
}

DWORD WorkerBase::RootProcessId() const
{
	return this->rootProcess.processId;
}

DWORD WorkerBase::RootThreadId() const
{
	return this->rootProcess.threadId;
}

DWORD WorkerBase::RootProcessStartTime() const
{
	return this->rootProcess.startTime;
}

DWORD WorkerBase::ParentFarPid() const
{
	return this->farManagerInfo.dwParentFarPid;
}

HANDLE WorkerBase::RootProcessHandle() const
{
	return this->rootProcess.processHandle;
}

HWND WorkerBase::RootProcessGui() const
{
	return this->rootProcess.childGuiWnd;
}

void WorkerBase::CloseRootProcessHandles()
{
	SafeCloseHandle(this->rootProcess.processHandle);
	SafeCloseHandle(this->rootProcess.threadHandle);
}

ConProcess& WorkerBase::Processes()
{
	if (!processes_)
	{
		_ASSERTE(FALSE && "processes_ should be set already");
		processes_.reset(new ConProcess(kernel32));
	}

	return *processes_;
}

const CONSOLE_SCREEN_BUFFER_INFO& WorkerBase::GetSbi() const
{
	return this->consoleInfo.sbi;
}

void WorkerBase::EnableProcessMonitor(bool enable)
{
	// nothing to do in base
}

bool WorkerBase::IsDebuggerActive() const
{
	if (!dbgInfo)
		return false;
	return dbgInfo->bDebuggerActive;
}

bool WorkerBase::IsDebugProcess() const
{
	if (!dbgInfo)
		return false;
	return dbgInfo->bDebugProcess;
}

bool WorkerBase::IsDebugProcessTree() const
{
	if (!dbgInfo)
		return false;
	return dbgInfo->bDebugProcessTree;
}

bool WorkerBase::IsDebugCmdLineSet() const
{
	if (!dbgInfo)
		return false;
	return !dbgInfo->szDebuggingCmdLine.IsEmpty();
}

int WorkerBase::SetDebuggingPid(const wchar_t* const pidList)
{
	if (!dbgInfo)
		dbgInfo.reset(new DebuggerInfo);

	gpState->noCreateProcess_ = TRUE;
	dbgInfo->bDebugProcess = TRUE;
	dbgInfo->bDebugProcessTree = FALSE;

	wchar_t* pszEnd = nullptr;
	SetRootProcessId(wcstoul(pidList, &pszEnd, 10));

	if (RootProcessId() == 0)
	{
		// ReSharper disable twice StringLiteralTypo
		LogString(L"CERR_CARGUMENT: Debug of process was requested, but invalid PID specified");
		_printf("Debug of process was requested, but invalid PID specified:\n");
		_wprintf(GetCommandLineW());
		_printf("\n");
		_ASSERTE(FALSE && "Invalid PID specified for debugging");
		return CERR_CARGUMENT;
	}

	// "Comma" is a mark that debug/dump was requested for a bunch of processes
	if (pszEnd && (*pszEnd == L','))
	{
		dbgInfo->bDebugMultiProcess = TRUE;
		dbgInfo->pDebugAttachProcesses = new MArray<DWORD>;
		while (pszEnd && (*pszEnd == L',') && *(pszEnd + 1))
		{
			DWORD nPID = wcstoul(pszEnd + 1, &pszEnd, 10);
			if (nPID != 0)
			{
				dbgInfo->pDebugAttachProcesses->push_back(nPID);
			}
		}
	}

	return 0;
}

int WorkerBase::SetDebuggingExe(const wchar_t* const commandLine, const bool debugTree)
{
	_ASSERTE(gpWorker->IsDebugProcess() == false); // should not be set yet

	if (!dbgInfo)
		dbgInfo.reset(new DebuggerInfo);

	gpState->noCreateProcess_ = TRUE;
	dbgInfo->bDebugProcess = true;
	dbgInfo->bDebugProcessTree = debugTree;

	dbgInfo->szDebuggingCmdLine = SkipNonPrintable(commandLine);

	if (dbgInfo->szDebuggingCmdLine.IsEmpty())
	{
		// ReSharper disable twice StringLiteralTypo
		LogString(L"CERR_CARGUMENT: Debug of process was requested, but command was not found");
		_printf("Debug of process was requested, but command was not found\n");
		_ASSERTE(FALSE && "Invalid command line for debugger was passed");
		return CERR_CARGUMENT;
	}

	return 0;
}

void WorkerBase::SetDebugDumpType(DumpProcessType dumpType)
{
	if (!dbgInfo)
		dbgInfo.reset(new DebuggerInfo);

	dbgInfo->debugDumpProcess = dumpType;
}

void WorkerBase::SetDebugAutoDump(const wchar_t* interval)
{
	if (!dbgInfo)
		dbgInfo.reset(new DebuggerInfo);

	dbgInfo->debugDumpProcess = DumpProcessType::None;
	dbgInfo->bAutoDump = true;
	dbgInfo->nAutoInterval = 1000;

	if (interval && *interval && isDigit(interval[0]))
	{
		wchar_t* pszEnd = nullptr;
		DWORD nVal = wcstol(interval, &pszEnd, 10);
		if (nVal)
		{
			if (pszEnd && *pszEnd)
			{
				if (lstrcmpni(pszEnd, L"ms", 2) == 0)
				{
					// Already milliseconds
				}
				else if (lstrcmpni(pszEnd, L"s", 1) == 0)
				{
					nVal *= 60; // seconds
				}
				else if (lstrcmpni(pszEnd, L"m", 2) == 0)
				{
					nVal *= 60 * 60; // minutes
				}
			}
			dbgInfo->nAutoInterval = nVal;
		}
	}
}

DebuggerInfo& WorkerBase::DbgInfo()
{
	if (!dbgInfo)
	{
		_ASSERTE(FALSE && "dbgInfo should be set already");
		dbgInfo.reset(new DebuggerInfo);
	}

	return *dbgInfo;
}

bool WorkerBase::IsKeyboardLayoutChanged(DWORD& pdwLayout, LPDWORD pdwErrCode /*= NULL*/)
{
	bool bChanged = false;

	if (!gpWorker)
	{
		_ASSERTE(gpWorker!=nullptr);
		return false;
	}

	static bool bGetConsoleKeyboardLayoutNameImplemented = true;
	if (bGetConsoleKeyboardLayoutNameImplemented && pfnGetConsoleKeyboardLayoutName)
	{
		wchar_t szCurKeybLayout[32] = L"";

		//#ifdef _DEBUG
		//wchar_t szDbgKeybLayout[KL_NAMELENGTH/*==9*/];
		//BOOL lbGetRC = GetKeyboardLayoutName(szDbgKeybLayout); // -- не дает эффекта, поскольку "на процесс", а не на консоль
		//#endif

		// The expected result of GetConsoleKeyboardLayoutName is like "00000419"
		const BOOL bConApiRc = pfnGetConsoleKeyboardLayoutName(szCurKeybLayout) && szCurKeybLayout[0];

		const DWORD nErr = bConApiRc ? 0 : GetLastError();
		if (pdwErrCode)
			*pdwErrCode = nErr;

		/*
		if (!bConApiRc && (nErr == ERROR_GEN_FAILURE))
		{
			_ASSERTE(FALSE && "ConsKeybLayout failed");
			MModule kernel(GetModuleHandle(L"kernel32.dll"));
			BOOL (WINAPI* getLayoutName)(LPWSTR,int);
			if (kernel.GetProcAddress("GetConsoleKeyboardLayoutNameW", getLayoutName))
			{
				bConApiRc = getLayoutName(szCurKeybLayout, countof(szCurKeybLayout));
				nErr = bConApiRc ? 0 : GetLastError();
			}
		}
		*/

		if (!bConApiRc)
		{
			// If GetConsoleKeyboardLayoutName is not implemented in Windows, ERROR_MR_MID_NOT_FOUND or E_NOTIMPL will be returned.
			// (there is no matching DOS/Win32 error code for NTSTATUS code returned)
			// When this happens, we don't want to continue to call the function.
			if (nErr == ERROR_MR_MID_NOT_FOUND || LOWORD(nErr) == LOWORD(E_NOTIMPL))
			{
				bGetConsoleKeyboardLayoutNameImplemented = false;
			}

			if (this->szKeybLayout[0])
			{
				// Log only first error per session
				wcscpy_c(szCurKeybLayout, this->szKeybLayout);
			}
			else
			{
				wchar_t szErr[80];
				swprintf_c(szErr, L"ConsKeybLayout failed with code=%u forcing to GetKeyboardLayoutName or 0409", nErr);
				_ASSERTE(!bGetConsoleKeyboardLayoutNameImplemented && "ConsKeybLayout failed");
				LogString(szErr);
				if (!GetKeyboardLayoutName(szCurKeybLayout) || (szCurKeybLayout[0] == 0))
				{
					wcscpy_c(szCurKeybLayout, L"00000419");
				}
			}
		}

		if (szCurKeybLayout[0])
		{
			if (lstrcmpW(szCurKeybLayout, this->szKeybLayout))
			{
				#ifdef _DEBUG
				wchar_t szDbg[128];
				swprintf_c(szDbg,
				          L"ConEmuC: InputLayoutChanged (GetConsoleKeyboardLayoutName returns) '%s'\n",
				          szCurKeybLayout);
				OutputDebugString(szDbg);
				#endif

				if (gpLogSize)
				{
					char szInfo[128]; wchar_t szWide[128];
					swprintf_c(szWide, L"ConsKeybLayout changed from '%s' to '%s'", this->szKeybLayout, szCurKeybLayout);
					WideCharToMultiByte(CP_ACP,0,szWide,-1,szInfo,128,0,0);
					LogFunction(szInfo);
				}

				// was changed
				wcscpy_c(this->szKeybLayout, szCurKeybLayout);
				bChanged = true;
			}
		}
	}
	else if (pdwErrCode)
	{
		*pdwErrCode = static_cast<DWORD>(-1);
	}

	// The result, if possible
	{
		wchar_t *pszEnd = nullptr; //szCurKeybLayout+8;
		//WARNING("BUGBUG: 16 цифр не вернет"); -- тут именно 8 цифр. Это LayoutNAME, а не string(HKL)
		// LayoutName: "00000409", "00010409", ...
		// HKL differs, so we pass DWORD
		// HKL in x64 looks like: "0x0000000000020409", "0xFFFFFFFFF0010409"
		pdwLayout = wcstoul(this->szKeybLayout, &pszEnd, 16);
	}

	return bChanged;
}

const MModule& WorkerBase::KernelModule() const
{
	return kernel32;
}

//WARNING("BUGBUG: x64 US-Dvorak"); - done
void WorkerBase::CheckKeyboardLayout()
{
	if (!gpWorker)
	{
		_ASSERTE(gpWorker != nullptr);
		return;
	}

	if (pfnGetConsoleKeyboardLayoutName == nullptr)
	{
		return;
	}

	DWORD dwLayout = 0;

	//WARNING("BUGBUG: 16 цифр не вернет"); -- тут именно 8 цифр. Это LayoutNAME, а не string(HKL)
	// LayoutName: "00000409", "00010409", ...
	// А HKL от него отличается, так что передаем DWORD
	// HKL в x64 выглядит как: "0x0000000000020409", "0xFFFFFFFFF0010409"

	if (IsKeyboardLayoutChanged(dwLayout))
	{
		// Сменился, Отошлем в GUI
		CESERVER_REQ* pIn = ExecuteNewCmd(CECMD_LANGCHANGE,sizeof(CESERVER_REQ_HDR)+sizeof(DWORD)); //-V119

		if (pIn)
		{
			//memmove(pIn->Data, &dwLayout, 4);
			pIn->dwData[0] = dwLayout;

			CESERVER_REQ* pOut = ExecuteGuiCmd(gpState->realConWnd_, pIn, gpState->realConWnd_);

			ExecuteFreeResult(pOut);
			ExecuteFreeResult(pIn);
		}
	}
}

bool WorkerBase::CheckHwFullScreen()
{
	bool bFullScreenHw = false;

	// #Server this should be wrap in the method. and there is a note of a bug in Windows 10 b9879
	if (wasFullscreenMode_ && pfnGetConsoleDisplayMode)
	{
		DWORD nModeFlags = 0;
		pfnGetConsoleDisplayMode(&nModeFlags);
		// The bug of Windows 10 b9879
		if (IsWin10Build9879())
		{
			nModeFlags = 0;
		}

		gpState->consoleDisplayMode_ = nModeFlags;
		if (nModeFlags & CONSOLE_FULLSCREEN_HARDWARE)
		{
			bFullScreenHw = true;
		}
		else
		{
			wasFullscreenMode_ = false;
		}
	}

	return bFullScreenHw;
}

bool WorkerBase::EnterHwFullScreen(PCOORD pNewSize /*= nullptr*/)
{
	if (!pfnSetConsoleDisplayMode)
	{
		if (!kernel32.GetProcAddress("SetConsoleDisplayMode", pfnSetConsoleDisplayMode))
		{
			SetLastError(ERROR_INVALID_FUNCTION);
			return false;
		}
	}

	COORD crNewSize = {};
	const bool result = pfnSetConsoleDisplayMode(ghConOut, 1/*CONSOLE_FULLSCREEN_MODE*/, &crNewSize);

	if (result)
	{
		wasFullscreenMode_ = true;
		if (pNewSize)
			*pNewSize = crNewSize;
	}

	return result;
}

void WorkerBase::CdToProfileDir()
{
	BOOL bRc = FALSE;
	wchar_t szPath[MAX_PATH] = L"";
	HRESULT hr = SHGetFolderPath(NULL, CSIDL_PROFILE, NULL, 0, szPath);
	if (FAILED(hr))
		GetEnvironmentVariable(L"USERPROFILE", szPath, countof(szPath));
	if (szPath[0])
		bRc = SetCurrentDirectory(szPath);
	// Write action to log file
	if (gpLogSize)
	{
		wchar_t* pszMsg = lstrmerge(bRc ? L"Work dir changed to %USERPROFILE%: " : L"CD failed to %USERPROFILE%: ", szPath);
		LogFunction(pszMsg);
		SafeFree(pszMsg);
	}
}
