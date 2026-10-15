#include "stdafx.h"
#include <windows.h>
#include <tchar.h>
#include <stdio.h>
#include <stdarg.h>
#include <shlobj.h>
#include <shellapi.h>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shlwapi.lib")

// 全局句柄
HANDLE g_hConsoleOut = INVALID_HANDLE_VALUE;
FILE* g_logFile = NULL;

// 双输出函数：控制台输出英文彻底避免乱码，日志输出中文方便排查
void Print(LPCTSTR szConsoleEn, LPCTSTR szLogZh, ...)
{
	TCHAR szBuf[4096] = { 0 };
	va_list args;

	// 输出英文到控制台
	if (g_hConsoleOut != INVALID_HANDLE_VALUE && szConsoleEn != NULL)
	{
		va_start(args, szLogZh);
		_vstprintf_s(szBuf, _countof(szBuf), szConsoleEn, args);
		va_end(args);
		DWORD dwWritten = 0;
		TCHAR szOutput[4100] = { 0 };
		_stprintf_s(szOutput, _countof(szOutput), _T("%s\r\n"), szBuf);
		WriteConsole(g_hConsoleOut, szOutput, (DWORD)_tcslen(szOutput), &dwWritten, NULL);
	}

	// 输出中文到日志文件
	if (g_logFile != NULL && szLogZh != NULL)
	{
		va_start(args, szLogZh);
		_vstprintf_s(szBuf, _countof(szBuf), szLogZh, args);
		va_end(args);
		_ftprintf(g_logFile, _T("%s\r\n"), szBuf);
		fflush(g_logFile);
	}
}

// 检测管理员权限
BOOL IsRunningAsAdmin()
{
	BOOL bIsAdmin = FALSE;
	HANDLE hToken = NULL;
	if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken))
	{
		TOKEN_ELEVATION Elevation;
		DWORD cbSize = sizeof(TOKEN_ELEVATION);
		if (GetTokenInformation(hToken, TokenElevation, &Elevation, sizeof(Elevation), &cbSize))
		{
			bIsAdmin = Elevation.TokenIsElevated;
		}
		CloseHandle(hToken);
	}
	return bIsAdmin;
}

// 停止MSI服务
BOOL StopMSIService()
{
	Print(_T("[1/7] Stopping Windows Installer service..."), _T("[1/7] 正在停止 Windows Installer 服务..."));
	SC_HANDLE hSCM = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);
	if (hSCM == NULL)
	{
		Print(_T("  [FAIL] Failed to open service manager, error: %d"), _T("  ❌ 打开服务管理器失败，错误码：%d"), GetLastError());
		return FALSE;
	}

	SC_HANDLE hService = OpenService(hSCM, _T("msiserver"), SERVICE_ALL_ACCESS);
	if (hService == NULL)
	{
		Print(_T("  [FAIL] Failed to open MSI service, error: %d"), _T("  ❌ 打开MSI服务失败，错误码：%d"), GetLastError());
		CloseServiceHandle(hSCM);
		return FALSE;
	}

	SERVICE_STATUS ss;
	if (!ControlService(hService, SERVICE_CONTROL_STOP, &ss))
	{
		DWORD err = GetLastError();
		if (err == ERROR_SERVICE_NOT_ACTIVE)
		{
			Print(_T("  [INFO] Service already stopped, no action needed"), _T("  ℹ️ 服务已经处于停止状态，无需停止"));
		}
		else
		{
			Print(_T("  [WARN] Stop service returned error (ignorable), code: %d"), _T("  ⚠️ 停止服务返回错误（可忽略），错误码：%d"), err);
		}
	}
	else
	{
		Sleep(2000);
		Print(_T("  [ OK ] Service stopped successfully"), _T("  ✅ 服务已停止"));
	}

	CloseServiceHandle(hService);
	CloseServiceHandle(hSCM);
	return TRUE;
}

// 启动MSI服务
BOOL StartMSIService()
{
	Print(_T("[6/7] Starting Windows Installer service..."), _T("[6/7] 正在启动 Windows Installer 服务..."));
	SC_HANDLE hSCM = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);
	if (hSCM == NULL)
	{
		Print(_T("  [FAIL] Failed to open service manager, error: %d"), _T("  ❌ 打开服务管理器失败，错误码：%d"), GetLastError());
		return FALSE;
	}

	SC_HANDLE hService = OpenService(hSCM, _T("msiserver"), SERVICE_ALL_ACCESS);
	if (hService == NULL)
	{
		Print(_T("  [FAIL] Failed to open MSI service, error: %d"), _T("  ❌ 打开MSI服务失败，错误码：%d"), GetLastError());
		CloseServiceHandle(hSCM);
		return FALSE;
	}

	if (!StartService(hService, 0, NULL))
	{
		DWORD err = GetLastError();
		if (err == ERROR_SERVICE_ALREADY_RUNNING)
		{
			Print(_T("  [INFO] Service already running"), _T("  ℹ️ 服务已经处于运行状态"));
		}
		else
		{
			Print(_T("  [FAIL] Failed to start service, error: %d"), _T("  ❌ 启动服务失败，错误码：%d"), err);
			CloseServiceHandle(hService);
			CloseServiceHandle(hSCM);
			return FALSE;
		}
	}
	else
	{
		Print(_T("  [ OK ] Service started successfully"), _T("  ✅ 服务已成功启动"));
	}

	CloseServiceHandle(hService);
	CloseServiceHandle(hSCM);
	return TRUE;
}

// 重新注册msiexec
BOOL ReRegisterMSIExec()
{
	Print(_T("[2/7] Re-registering msiexec core component..."), _T("[2/7] 正在重新注册 msiexec 核心组件..."));
	TCHAR szSysDir[MAX_PATH] = { 0 };
	PVOID pOldRedirection = NULL;

	typedef BOOL(WINAPI *LPFN_WOW64DISABLEWOW64FSREDIRECTION)(PVOID*);
	typedef BOOL(WINAPI *LPFN_WOW64REVERTWOW64FSREDIRECTION)(PVOID);
	LPFN_WOW64DISABLEWOW64FSREDIRECTION pfnWow64DisableWow64FsRedirection =
		(LPFN_WOW64DISABLEWOW64FSREDIRECTION)GetProcAddress(GetModuleHandle(_T("kernel32.dll")), "Wow64DisableWow64FsRedirection");
	if (pfnWow64DisableWow64FsRedirection)
	{
		pfnWow64DisableWow64FsRedirection(&pOldRedirection);
	}

	GetSystemDirectory(szSysDir, MAX_PATH);
	TCHAR szMsiExecPath[MAX_PATH] = { 0 };
	_stprintf_s(szMsiExecPath, _countof(szMsiExecPath), _T("\"%s\\msiexec.exe\""), szSysDir);

	SHELLEXECUTEINFO sei = { 0 };
	sei.cbSize = sizeof(SHELLEXECUTEINFO);
	sei.fMask = SEE_MASK_NOCLOSEPROCESS;
	sei.hwnd = NULL;
	sei.lpVerb = _T("open");
	sei.lpFile = szMsiExecPath;
	sei.lpParameters = _T("/unregister");
	sei.nShow = SW_HIDE;
	if (ShellExecuteEx(&sei))
	{
		WaitForSingleObject(sei.hProcess, 10000);
		CloseHandle(sei.hProcess);
		Print(_T("  [ OK ] msiexec /unregister completed"), _T("  ✅ msiexec /unregister 执行完成"));
	}
	else
	{
		Print(_T("  [WARN] msiexec /unregister failed, error: %d"), _T("  ⚠️ msiexec /unregister 执行失败，错误码：%d"), GetLastError());
	}

	ZeroMemory(&sei, sizeof(sei));
	sei.cbSize = sizeof(SHELLEXECUTEINFO);
	sei.fMask = SEE_MASK_NOCLOSEPROCESS;
	sei.hwnd = NULL;
	sei.lpVerb = _T("open");
	sei.lpFile = szMsiExecPath;
	sei.lpParameters = _T("/regserver");
	sei.nShow = SW_HIDE;
	if (ShellExecuteEx(&sei))
	{
		WaitForSingleObject(sei.hProcess, 10000);
		CloseHandle(sei.hProcess);
		Print(_T("  [ OK ] msiexec /regserver completed"), _T("  ✅ msiexec /regserver 执行完成"));
	}
	else
	{
		Print(_T("  [WARN] msiexec /regserver failed, error: %d"), _T("  ⚠️ msiexec /regserver 执行失败，错误码：%d"), GetLastError());
	}

	LPFN_WOW64REVERTWOW64FSREDIRECTION pfnWow64RevertWow64FsRedirection =
		(LPFN_WOW64REVERTWOW64FSREDIRECTION)GetProcAddress(GetModuleHandle(_T("kernel32.dll")), "Wow64RevertWow64FsRedirection");
	if (pfnWow64RevertWow64FsRedirection && pOldRedirection)
	{
		pfnWow64RevertWow64FsRedirection(pOldRedirection);
	}

	return TRUE;
}

// 清除UserChoice
BOOL DeleteAllUserChoice()
{
	Print(_T("[3/7] Cleaning UserChoice association cache..."), _T("[3/7] 正在清除所有用户的UserChoice关联缓存..."));
	LONG lRes = 0;
	HKEY hRootKey = NULL;

	TCHAR szCurrentUserKey[] = _T("Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\.msi");
	lRes = RegOpenKeyEx(HKEY_CURRENT_USER, szCurrentUserKey, 0, KEY_ALL_ACCESS | KEY_WOW64_64KEY, &hRootKey);
	if (lRes == ERROR_SUCCESS)
	{
		lRes = RegDeleteTree(hRootKey, _T("UserChoice"));
		if (lRes == ERROR_SUCCESS)
		{
			Print(_T("  [ OK ] Current user UserChoice deleted"), _T("  ✅ 当前用户UserChoice项已删除"));
		}
		else if (lRes == ERROR_FILE_NOT_FOUND)
		{
			Print(_T("  [INFO] Current user has no UserChoice, skip"), _T("  ℹ️ 当前用户不存在UserChoice项，无需删除"));
		}
		else
		{
			Print(_T("  [WARN] Failed to delete current user UserChoice, error: %d"), _T("  ⚠️ 删除当前用户UserChoice失败，错误码：%d"), lRes);
		}
		RegCloseKey(hRootKey);
	}

	lRes = RegOpenKeyEx(HKEY_USERS, _T(".DEFAULT"), 0, KEY_ALL_ACCESS | KEY_WOW64_64KEY, &hRootKey);
	if (lRes == ERROR_SUCCESS)
	{
		HKEY hSubKey = NULL;
		lRes = RegOpenKeyEx(hRootKey, szCurrentUserKey, 0, KEY_ALL_ACCESS | KEY_WOW64_64KEY, &hSubKey);
		if (lRes == ERROR_SUCCESS)
		{
			lRes = RegDeleteTree(hSubKey, _T("UserChoice"));
			if (lRes == ERROR_SUCCESS)
			{
				Print(_T("  [ OK ] Default user UserChoice deleted"), _T("  ✅ 默认用户UserChoice项已删除"));
			}
			RegCloseKey(hSubKey);
		}
		RegCloseKey(hRootKey);
	}

	Print(_T("  [ OK ] UserChoice cache cleaned"), _T("  ✅ UserChoice缓存清理完成"));
	return TRUE;
}

// 修复文件关联
BOOL RepairMSIFileAssociation()
{
	Print(_T("[4/7] Repairing MSI file association registry..."), _T("[4/7] 正在修复MSI文件关联注册表..."));
	LONG lRes = 0;
	HKEY hKey = NULL;
	DWORD dwDisposition = 0;

	lRes = RegCreateKeyEx(HKEY_CLASSES_ROOT, _T(".msi"), 0, NULL, 0, KEY_ALL_ACCESS | KEY_WOW64_64KEY, NULL, &hKey, &dwDisposition);
	if (lRes == ERROR_SUCCESS)
	{
		TCHAR szProgId[] = _T("Msi.Package");
		lRes = RegSetValueEx(hKey, NULL, 0, REG_SZ, (LPBYTE)szProgId, (DWORD)(_tcslen(szProgId) + 1) * sizeof(TCHAR));
		if (lRes == ERROR_SUCCESS)
		{
			Print(_T("  [ OK ] .msi default association set to Msi.Package"), _T("  ✅ .msi 扩展名默认关联已设置为 Msi.Package"));
		}
		RegCloseKey(hKey);
	}

	TCHAR szSysDir[MAX_PATH] = { 0 };
	TCHAR szWow64Dir[MAX_PATH] = { 0 };
	PVOID pOldRedirection = NULL;
	typedef BOOL(WINAPI *LPFN_WOW64DISABLEWOW64FSREDIRECTION)(PVOID*);
	typedef BOOL(WINAPI *LPFN_WOW64REVERTWOW64FSREDIRECTION)(PVOID);
	LPFN_WOW64DISABLEWOW64FSREDIRECTION pfnWow64DisableWow64FsRedirection =
		(LPFN_WOW64DISABLEWOW64FSREDIRECTION)GetProcAddress(GetModuleHandle(_T("kernel32.dll")), "Wow64DisableWow64FsRedirection");
	if (pfnWow64DisableWow64FsRedirection)
	{
		pfnWow64DisableWow64FsRedirection(&pOldRedirection);
	}
	GetSystemDirectory(szSysDir, MAX_PATH);
	if (pfnWow64DisableWow64FsRedirection && pOldRedirection)
	{
		LPFN_WOW64REVERTWOW64FSREDIRECTION pfnWow64RevertWow64FsRedirection =
			(LPFN_WOW64REVERTWOW64FSREDIRECTION)GetProcAddress(GetModuleHandle(_T("kernel32.dll")), "Wow64RevertWow64FsRedirection");
		pfnWow64RevertWow64FsRedirection(pOldRedirection);
	}
	UINT uWowRet = GetSystemWow64Directory(szWow64Dir, MAX_PATH);

	TCHAR szMsiExecSys[MAX_PATH] = { 0 };
	TCHAR szMsiExecWow[MAX_PATH] = { 0 };
	_stprintf_s(szMsiExecSys, _countof(szMsiExecSys), _T("\"%s\\msiexec.exe\""), szSysDir);
	if (uWowRet > 0 && _tcslen(szWow64Dir) > 0)
	{
		_stprintf_s(szMsiExecWow, _countof(szMsiExecWow), _T("\"%s\\msiexec.exe\""), szWow64Dir);
	}

	TCHAR szMsiPackageKey[] = _T("Msi.Package\\shell");
	lRes = RegCreateKeyEx(HKEY_CLASSES_ROOT, szMsiPackageKey, 0, NULL, 0, KEY_ALL_ACCESS | KEY_WOW64_64KEY, NULL, &hKey, &dwDisposition);
	if (lRes == ERROR_SUCCESS)
	{
		TCHAR szDefaultAction[] = _T("install");
		RegSetValueEx(hKey, NULL, 0, REG_SZ, (LPBYTE)szDefaultAction, (DWORD)(_tcslen(szDefaultAction) + 1) * sizeof(TCHAR));
		RegCloseKey(hKey);
		Print(_T("  [ OK ] Msi.Package default action set to install"), _T("  ✅ Msi.Package 外壳默认动作设置为安装"));
	}

	struct VerbCommand {
		LPCTSTR szVerb;
		LPCTSTR szCommand;
	} verbs[] = {
		{ _T("install\\command"), _T("%s /i \"%%1\" %%*") },
		{ _T("open\\command"), _T("%s /i \"%%1\" %%*") },
		{ _T("repair\\command"), _T("%s /f \"%%1\" %%*") },
		{ _T("uninstall\\command"), _T("%s /x \"%%1\" %%*") },
	};

	for (int i = 0; i < _countof(verbs); i++)
	{
		TCHAR szFullKey[MAX_PATH] = { 0 };
		_stprintf_s(szFullKey, _countof(szFullKey), _T("Msi.Package\\shell\\%s"), verbs[i].szVerb);

		lRes = RegCreateKeyEx(HKEY_CLASSES_ROOT, szFullKey, 0, NULL, 0, KEY_ALL_ACCESS | KEY_WOW64_64KEY, NULL, &hKey, &dwDisposition);
		if (lRes == ERROR_SUCCESS)
		{
			TCHAR szCmd[MAX_PATH] = { 0 };
			_stprintf_s(szCmd, _countof(szCmd), verbs[i].szCommand, szMsiExecSys);
			RegSetValueEx(hKey, NULL, 0, REG_EXPAND_SZ, (LPBYTE)szCmd, (DWORD)(_tcslen(szCmd) + 1) * sizeof(TCHAR));
			Print(_T("  [ OK ] Registered command: shell\\%s"), _T("  ✅ 注册命令成功：shell\\%s"), verbs[i].szVerb);
			RegCloseKey(hKey);
		}
	}

	if (uWowRet > 0 && _tcslen(szMsiExecWow) > 0)
	{
		for (int i = 0; i < _countof(verbs); i++)
		{
			TCHAR szFullKey[MAX_PATH] = { 0 };
			_stprintf_s(szFullKey, _countof(szFullKey), _T("Wow6432Node\\Msi.Package\\shell\\%s"), verbs[i].szVerb);
			lRes = RegCreateKeyEx(HKEY_CLASSES_ROOT, szFullKey, 0, NULL, 0, KEY_ALL_ACCESS | KEY_WOW64_64KEY, NULL, &hKey, &dwDisposition);
			if (lRes == ERROR_SUCCESS)
			{
				TCHAR szCmd[MAX_PATH] = { 0 };
				_stprintf_s(szCmd, _countof(szCmd), verbs[i].szCommand, szMsiExecWow);
				RegSetValueEx(hKey, NULL, 0, REG_EXPAND_SZ, (LPBYTE)szCmd, (DWORD)(_tcslen(szCmd) + 1) * sizeof(TCHAR));
				RegCloseKey(hKey);
			}
		}
		Print(_T("  [ OK ] 32bit compatibility association registered"), _T("  ✅ 32位程序兼容性关联已注册"));
	}

	TCHAR szIconKey[] = _T("Msi.Package\\DefaultIcon");
	lRes = RegCreateKeyEx(HKEY_CLASSES_ROOT, szIconKey, 0, NULL, 0, KEY_ALL_ACCESS | KEY_WOW64_64KEY, NULL, &hKey, &dwDisposition);
	if (lRes == ERROR_SUCCESS)
	{
		TCHAR szIcon[MAX_PATH] = { 0 };
		_stprintf_s(szIcon, _countof(szIcon), _T("%s,0"), szMsiExecSys);
		RegSetValueEx(hKey, NULL, 0, REG_SZ, (LPBYTE)szIcon, (DWORD)(_tcslen(szIcon) + 1) * sizeof(TCHAR));
		RegCloseKey(hKey);
		Print(_T("  [ OK ] MSI default icon restored"), _T("  ✅ MSI文件默认图标已恢复"));
	}

	Print(_T("  [ OK ] File association registry repair completed"), _T("  ✅ 文件关联注册表修复完成"));
	return TRUE;
}

// 修复服务配置
BOOL RepairMSIServiceConfig()
{
	Print(_T("[5/7] Repairing Windows Installer service config..."), _T("[5/7] 正在修复Windows Installer服务配置..."));
	SC_HANDLE hSCM = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);
	if (hSCM == NULL)
	{
		Print(_T("  [FAIL] Failed to open service manager, error: %d"), _T("  ❌ 打开服务管理器失败，错误码：%d"), GetLastError());
		return FALSE;
	}

	SC_HANDLE hService = OpenService(hSCM, _T("msiserver"), SERVICE_ALL_ACCESS);
	if (hService == NULL)
	{
		Print(_T("  [FAIL] Failed to open MSI service, error: %d"), _T("  ❌ 打开MSI服务失败，错误码：%d"), GetLastError());
		CloseServiceHandle(hSCM);
		return FALSE;
	}

	TCHAR szSysDir[MAX_PATH] = { 0 };
	PVOID pOldRedirection = NULL;
	typedef BOOL(WINAPI *LPFN_WOW64DISABLEWOW64FSREDIRECTION)(PVOID*);
	typedef BOOL(WINAPI *LPFN_WOW64REVERTWOW64FSREDIRECTION)(PVOID);
	LPFN_WOW64DISABLEWOW64FSREDIRECTION pfnWow64DisableWow64FsRedirection =
		(LPFN_WOW64DISABLEWOW64FSREDIRECTION)GetProcAddress(GetModuleHandle(_T("kernel32.dll")), "Wow64DisableWow64FsRedirection");
	if (pfnWow64DisableWow64FsRedirection)
	{
		pfnWow64DisableWow64FsRedirection(&pOldRedirection);
	}
	GetSystemDirectory(szSysDir, MAX_PATH);
	if (pfnWow64DisableWow64FsRedirection && pOldRedirection)
	{
		LPFN_WOW64REVERTWOW64FSREDIRECTION pfnWow64RevertWow64FsRedirection =
			(LPFN_WOW64REVERTWOW64FSREDIRECTION)GetProcAddress(GetModuleHandle(_T("kernel32.dll")), "Wow64RevertWow64FsRedirection");
		pfnWow64RevertWow64FsRedirection(pOldRedirection);
	}

	TCHAR szImagePath[MAX_PATH] = { 0 };
	_stprintf_s(szImagePath, _countof(szImagePath), _T("\"%s\\msiexec.exe\" /V"), szSysDir);

	ChangeServiceConfig(hService, SERVICE_NO_CHANGE, SERVICE_DEMAND_START, SERVICE_ERROR_NORMAL,
		szImagePath, NULL, NULL, NULL, NULL, NULL, _T("Windows Installer"));

	HKEY hKey = NULL;
	LONG lRes = RegCreateKeyEx(HKEY_LOCAL_MACHINE, _T("SYSTEM\\CurrentControlSet\\Services\\msiserver"),
		0, NULL, 0, KEY_ALL_ACCESS | KEY_WOW64_64KEY, NULL, &hKey, NULL);
	if (lRes == ERROR_SUCCESS)
	{
		RegSetValueEx(hKey, _T("ImagePath"), 0, REG_EXPAND_SZ, (LPBYTE)szImagePath, (DWORD)(_tcslen(szImagePath) + 1) * sizeof(TCHAR));
		RegCloseKey(hKey);
	}

	TCHAR szMsiDllPath[MAX_PATH] = { 0 };
	_stprintf_s(szMsiDllPath, _countof(szMsiDllPath), _T("%s\\msi.dll"), szSysDir);
	HMODULE hMsiDll = LoadLibrary(szMsiDllPath);
	if (hMsiDll)
	{
		typedef HRESULT(WINAPI* DllRegisterServerFunc)(void);
		DllRegisterServerFunc pfnDllRegisterServer = (DllRegisterServerFunc)GetProcAddress(hMsiDll, "DllRegisterServer");
		if (pfnDllRegisterServer)
		{
			pfnDllRegisterServer();
		}
		FreeLibrary(hMsiDll);
	}

	CloseServiceHandle(hService);
	CloseServiceHandle(hSCM);
	Print(_T("  [ OK ] Service config repair completed"), _T("  ✅ 服务配置修复完成"));
	return TRUE;
}

// 刷新外壳缓存
void FlushShellCache()
{
	Print(_T("[7/7] Flushing shell association cache..."), _T("[7/7] 正在刷新系统外壳关联缓存..."));
	SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, NULL, NULL);
	Print(_T("  [ OK ] Cache flushed, changes take effect immediately"), _T("  ✅ 缓存刷新完成，关联已立即生效"));
}

// 校验修复结果
BOOL CheckRepairResult()
{
	Print(_T("\r\n==================== Repair Result Check ===================="), _T("\r\n==================== 修复结果校验 ===================="));
	BOOL bAllOk = TRUE;
	HKEY hKey = NULL;
	TCHAR szValue[MAX_PATH] = { 0 };
	DWORD dwSize = MAX_PATH * sizeof(TCHAR);
	DWORD dwType = 0;
	LONG lRes;

	lRes = RegOpenKeyEx(HKEY_CLASSES_ROOT, _T(".msi"), 0, KEY_READ | KEY_WOW64_64KEY, &hKey);
	if (lRes == ERROR_SUCCESS)
	{
		dwSize = MAX_PATH * sizeof(TCHAR);
		ZeroMemory(szValue, sizeof(szValue));
		RegQueryValueEx(hKey, NULL, NULL, &dwType, (LPBYTE)szValue, &dwSize);
		if (_tcscmp(szValue, _T("Msi.Package")) == 0)
		{
			Print(_T("[PASS] .msi default association: correct (Msi.Package)"), _T("✅ .msi 默认关联：正确（Msi.Package）"));
		}
		else
		{
			Print(_T("[FAIL] .msi default association: wrong, current value: %s"), _T("❌ .msi 默认关联：错误，当前值为：%s"), szValue);
			bAllOk = FALSE;
		}
		RegCloseKey(hKey);
	}

	lRes = RegOpenKeyEx(HKEY_CLASSES_ROOT, _T("Msi.Package\\shell\\install\\command"), 0, KEY_READ | KEY_WOW64_64KEY, &hKey);
	if (lRes == ERROR_SUCCESS)
	{
		dwSize = MAX_PATH * sizeof(TCHAR);
		ZeroMemory(szValue, sizeof(szValue));
		RegQueryValueEx(hKey, NULL, NULL, &dwType, (LPBYTE)szValue, &dwSize);
		if (_tcsstr(szValue, _T("msiexec.exe")) != NULL && _tcsstr(szValue, _T("/i")) != NULL)
		{
			Print(_T("[PASS] Install command: correct"), _T("✅ install命令：正确，配置为：%s"), szValue);
		}
		else
		{
			Print(_T("[FAIL] Install command: wrong, current value: %s"), _T("❌ install命令：错误，当前值为：%s"), szValue);
			bAllOk = FALSE;
		}
		RegCloseKey(hKey);
	}

	lRes = RegOpenKeyEx(HKEY_CURRENT_USER, _T("Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\.msi\\UserChoice"),
		0, KEY_READ | KEY_WOW64_64KEY, &hKey);
	if (lRes == ERROR_SUCCESS)
	{
		Print(_T("[WARN] UserChoice still exists, may be locked by antivirus"), _T("⚠️ UserChoice项仍然存在，可能被安全软件锁定，请暂时退出安全软件后重新运行"));
		RegCloseKey(hKey);
		bAllOk = FALSE;
	}
	else
	{
		Print(_T("[PASS] UserChoice: deleted successfully"), _T("✅ UserChoice项：已成功删除"));
	}

	if (bAllOk)
	{
		Print(_T("\r\n[SUCCESS] All checks passed! MSI association repaired successfully."), _T("\r\n🎉 所有校验项全部通过！MSI打开方式已经修复成功。"));
		Print(_T("[INFO] It is recommended to restart computer before installing MSI packages."), _T("ℹ️ 建议重启电脑后再尝试运行MSI安装包。"));
	}
	else
	{
		Print(_T("\r\n[WARN] Some checks failed, basic functionality should still work."), _T("\r\n⚠️ 部分校验项未通过，请查看上方日志定位具体问题。"));
	}

	return bAllOk;
}

// 无视后缀安装MSI
BOOL InstallMSI(LPCTSTR szFilePath)
{
	Print(_T("\r\n==================== Force Install MSI ===================="), _T("\r\n======================================================"));
	Print(_T("                 Starting MSI Installation"), _T("               开始执行MSI强制安装"));
	Print(_T("========================================================"), _T("======================================================"));

	DWORD dwAttr = GetFileAttributes(szFilePath);
	if (dwAttr == INVALID_FILE_ATTRIBUTES)
	{
		Print(_T("[FAIL] File not found: %s"), _T("❌ 错误：文件不存在！路径：%s"), szFilePath);
		Print(_T("Error code: %d"), _T("错误码：%d"), GetLastError());
		return FALSE;
	}

	if (dwAttr & FILE_ATTRIBUTE_DIRECTORY)
	{
		Print(_T("[FAIL] Path is a directory, not a file"), _T("❌ 错误：选择的路径是文件夹，不是文件"));
		return FALSE;
	}

	Print(_T("Target file: %s"), _T("📦 待安装文件：%s"), szFilePath);
	LARGE_INTEGER llFileSize;
	HANDLE hFile = CreateFile(szFilePath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	if (hFile != INVALID_HANDLE_VALUE)
	{
		GetFileSizeEx(hFile, &llFileSize);
		Print(_T("File size: %d KB"), _T("文件大小：%d KB"), (int)(llFileSize.QuadPart / 1024));
		CloseHandle(hFile);
	}

	TCHAR szSysDir[MAX_PATH] = { 0 };
	PVOID pOldRedirection = NULL;
	typedef BOOL(WINAPI *LPFN_WOW64DISABLEWOW64FSREDIRECTION)(PVOID*);
	typedef BOOL(WINAPI *LPFN_WOW64REVERTWOW64FSREDIRECTION)(PVOID);
	LPFN_WOW64DISABLEWOW64FSREDIRECTION pfnWow64DisableWow64FsRedirection =
		(LPFN_WOW64DISABLEWOW64FSREDIRECTION)GetProcAddress(GetModuleHandle(_T("kernel32.dll")), "Wow64DisableWow64FsRedirection");
	if (pfnWow64DisableWow64FsRedirection)
	{
		pfnWow64DisableWow64FsRedirection(&pOldRedirection);
	}
	GetSystemDirectory(szSysDir, MAX_PATH);
	if (pfnWow64DisableWow64FsRedirection && pOldRedirection)
	{
		LPFN_WOW64REVERTWOW64FSREDIRECTION pfnWow64RevertWow64FsRedirection =
			(LPFN_WOW64REVERTWOW64FSREDIRECTION)GetProcAddress(GetModuleHandle(_T("kernel32.dll")), "Wow64RevertWow64FsRedirection");
		pfnWow64RevertWow64FsRedirection(pOldRedirection);
	}

	TCHAR szMsiExecPath[MAX_PATH] = { 0 };
	_stprintf_s(szMsiExecPath, _countof(szMsiExecPath), _T("%s\\msiexec.exe"), szSysDir);

	Print(_T("Launching msiexec with command:"), _T("🚀 正在调用msiexec执行安装，命令行："));
	Print(_T("  \"%s\" /i \"%s\""), _T("   \"%s\" /i \"%s\""), szMsiExecPath, szFilePath);

	SHELLEXECUTEINFO sei = { 0 };
	sei.cbSize = sizeof(SHELLEXECUTEINFO);
	sei.fMask = SEE_MASK_NOCLOSEPROCESS;
	sei.hwnd = NULL;
	sei.lpVerb = _T("open");
	sei.lpFile = szMsiExecPath;
	TCHAR szParams[MAX_PATH * 2] = { 0 };
	_stprintf_s(szParams, _countof(szParams), _T("/i \"%s\""), szFilePath);
	sei.lpParameters = szParams;
	sei.nShow = SW_SHOWNORMAL;

	if (!ShellExecuteEx(&sei))
	{
		DWORD err = GetLastError();
		// 如果普通启动失败，尝试runas提权启动
		sei.lpVerb = _T("runas");
		if (!ShellExecuteEx(&sei))
		{
			Print(_T("[FAIL] Failed to start installer, error: %d"), _T("❌ 启动安装程序失败，错误码：%d"), GetLastError());
			return FALSE;
		}
	}

	Print(_T("[ OK ] Installer launched successfully!"), _T("✅ 安装程序已成功启动"));
	Print(_T("[INFO] You can close this window, installer will continue running"), _T("ℹ️ 你可以关闭本窗口，安装程序会继续运行"));

	WaitForSingleObject(sei.hProcess, INFINITE);

	DWORD dwExitCode = 0;
	GetExitCodeProcess(sei.hProcess, &dwExitCode);
	CloseHandle(sei.hProcess);

	if (dwExitCode == 0)
	{
		Print(_T("[SUCCESS] Installer completed successfully (exit code: 0)"), _T("🎉 安装程序执行完成，返回码：0（成功）"));
	}
	else
	{
		Print(_T("[INFO] Installer exited with code: %d"), _T("⚠️ 安装程序结束，返回码：%d"), dwExitCode);
		Print(_T("Common codes: 1603=Fatal error/permission denied, 1602=User cancelled"), _T("常见返回码：1603=安装失败/权限不足，1602=用户取消安装"));
	}

	return TRUE;
}

// 显示菜单
void ShowMainMenu()
{
	Print(_T("\r\n==================== Main Menu ===================="), _T("\r\n======================================================"));
	Print(_T("1. Re-run MSI association repair"), _T("                    功能菜单"));
	Print(_T("2. Force install MSI file (ignore extension)"), _T("======================================================"));
	Print(_T("3. Exit"), _T("  1. 重新修复MSI文件关联"));
	Print(_T(""), _T("  2. 无视后缀强制安装MSI文件"));
	Print(_T("TIP: Drag & drop any file onto this EXE to install directly"), _T("  3. 退出程序"));
	Print(_T("\r\nEnter your choice (1/2/3): "), _T("💡 提示：你也可以直接把任意文件拖到本exe上自动安装"));
	Print(_T(""), _T("\r\n请输入选项（1/2/3）："));
}

int _tmain()
{
	// 获取控制台句柄
	g_hConsoleOut = GetStdHandle(STD_OUTPUT_HANDLE);
	SetConsoleTitle(_T("MSI Repair & Install Tool v3.1"));

	// 初始化日志文件
	TCHAR szExePath[MAX_PATH] = { 0 };
	GetModuleFileName(NULL, szExePath, MAX_PATH);
	TCHAR* pLastSlash = _tcsrchr(szExePath, _T('\\'));
	if (pLastSlash) *(pLastSlash + 1) = _T('\0');
	TCHAR szLogPath[MAX_PATH] = { 0 };
	_stprintf_s(szLogPath, _countof(szLogPath), _T("%sMSI修复日志.txt"), szExePath);
	_tfopen_s(&g_logFile, szLogPath, _T("w, ccs=UNICODE"));

	Print(_T("======================================================"), _T("======================================================"));
	Print(_T("          MSI Repair & Install Tool v3.1 Final"), _T("          MSI修复与安装工具 v3.1 最终无乱码版"));
	Print(_T("  Supports Windows 7/8/10/11, x86/x64 auto detect"), _T("  支持Windows 7/8/10/11，32/64位系统自动适配"));
	Print(_T("  Console: English (no garbled text)"), _T("  ✅ 控制台英文输出，彻底解决乱码"));
	Print(_T("  Log file: Chinese (for troubleshooting)"), _T("  ✅ 日志文件中文输出，方便排查问题"));
	Print(_T("======================================================"), _T("  ✅ 支持无视后缀强制安装MSI，拖拽即可安装"));
	Print(_T("Log file saved to: %s\r\n"), _T("======================================================"));
	Print(_T(""), _T("日志文件保存位置：%s\r\n"), szLogPath);

	// ========== 用官方API正确解析命令行参数，解决拖拽不生效问题 ==========
	int nArgs = 0;
	LPWSTR* pArgv = CommandLineToArgvW(GetCommandLineW(), &nArgs);
	if (pArgv != NULL && nArgs > 1)
	{
		// 有命令行参数，拖拽模式，直接安装
		Print(_T("Drag&drop mode detected, starting installer..."), _T("检测到拖放文件，自动进入安装模式..."));
		InstallMSI(pArgv[1]);
		LocalFree(pArgv);
		Print(_T("\r\nPress any key to exit..."), _T("\r\n按任意键退出..."));
		if (g_logFile) fclose(g_logFile);
		system("pause");
		return 0;
	}
	if (pArgv) LocalFree(pArgv);

	// ========== 正常双击模式，检查管理员权限 ==========
	if (!IsRunningAsAdmin())
	{
		Print(_T("[ERROR] Administrator privileges required!"), _T("❌ 错误：当前程序没有管理员权限！"));
		Print(_T("Please right-click this EXE and select 'Run as administrator'"), _T("请右键点击本程序，选择【以管理员身份运行】后再试。"));
		Print(_T("\r\nPress any key to exit..."), _T("\r\n按任意键退出..."));
		if (g_logFile) fclose(g_logFile);
		system("pause");
		return 1;
	}
	Print(_T("[ OK ] Administrator privileges verified"), _T("✅ 管理员权限检测通过"));

	// 默认执行修复
	Print(_T("\r\nRunning default repair process...\r\n"), _T("\r\n开始执行默认修复流程...\r\n"));
	StopMSIService();
	ReRegisterMSIExec();
	DeleteAllUserChoice();
	RepairMSIFileAssociation();
	RepairMSIServiceConfig();
	StartMSIService();
	FlushShellCache();
	CheckRepairResult();

	// 菜单循环
	while (TRUE)
	{
		ShowMainMenu();
		TCHAR szInput[256] = { 0 };
		_fgetts(szInput, _countof(szInput), stdin);
		int nLen = (int)_tcslen(szInput);
		while (nLen > 0 && (szInput[nLen - 1] == _T('\r') || szInput[nLen - 1] == _T('\n')))
		{
			szInput[nLen - 1] = _T('\0');
			nLen--;
		}

		if (_tcscmp(szInput, _T("1")) == 0)
		{
			Print(_T("\r\nRe-running repair process...\r\n"), _T("\r\n开始重新修复...\r\n"));
			StopMSIService();
			ReRegisterMSIExec();
			DeleteAllUserChoice();
			RepairMSIFileAssociation();
			RepairMSIServiceConfig();
			StartMSIService();
			FlushShellCache();
			CheckRepairResult();
		}
		else if (_tcscmp(szInput, _T("2")) == 0)
		{
			Print(_T("\r\nEnter full path to MSI file (you can drag file into this window):"), _T("\r\n请输入要安装的MSI文件完整路径（可以直接把文件拖到控制台窗口自动填入路径）："));
			TCHAR szFilePath[MAX_PATH] = { 0 };
			_fgetts(szFilePath, _countof(szFilePath), stdin);
			nLen = (int)_tcslen(szFilePath);
			while (nLen > 0 && (szFilePath[nLen - 1] == _T('\r') || szFilePath[nLen - 1] == _T('\n') || szFilePath[nLen - 1] == _T('"')))
			{
				szFilePath[nLen - 1] = _T('\0');
				nLen--;
			}
			if (szFilePath[0] == _T('"'))
			{
				memmove(szFilePath, szFilePath + 1, _tcslen(szFilePath) * sizeof(TCHAR));
			}
			if (_tcslen(szFilePath) > 0)
			{
				InstallMSI(szFilePath);
			}
			else
			{
				Print(_T("[ERROR] Path cannot be empty"), _T("❌ 路径不能为空"));
			}
		}
		else if (_tcscmp(szInput, _T("3")) == 0)
		{
			break;
		}
		else
		{
			Print(_T("[ERROR] Invalid input, please enter 1/2/3"), _T("❌ 输入无效，请输入1/2/3"));
		}
	}

	Print(_T("\r\nProgram exited. Thank you for using!"), _T("\r\n程序已退出，感谢使用！"));
	if (g_logFile) fclose(g_logFile);
	return 0;
}
