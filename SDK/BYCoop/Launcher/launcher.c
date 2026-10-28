/* BYCoopLauncher: starts barnyard.exe suspended, loads BYModCore.dll into it, resumes.
   BYModCore then loads every DLL in Mods\ (BYCoop.dll). Arguments are passed through,
   e.g. BYCoopLauncher.exe -coop-udp 27100 -coop-host  */
#include <windows.h>
#include <stdio.h>
#include <string.h>

static int fail(const char* m) {
	char b[512];
	snprintf(b, sizeof b, "%s (error %lu)", m, GetLastError());
	MessageBoxA(NULL, b, "BYCoop Launcher", MB_ICONERROR);
	return 1;
}

int main(void) {
	SetProcessDPIAware(); /* real pixels for SM_CXSCREEN under display scaling */
	char dir[MAX_PATH], exe[MAX_PATH], dll[MAX_PATH], cmd[2048];
	GetModuleFileNameA(NULL, dir, MAX_PATH);
	*strrchr(dir, '\\') = 0;
	snprintf(exe, sizeof exe, "%s\\barnyard.exe", dir);
	snprintf(dll, sizeof dll, "%s\\BYModCore.dll", dir);
	if (GetFileAttributesA(exe) == INVALID_FILE_ATTRIBUTES) return fail("barnyard.exe not found next to the launcher");
	if (GetFileAttributesA(dll) == INVALID_FILE_ATTRIBUTES) return fail("BYModCore.dll not found next to the launcher");

	/* pass our own arguments through */
	const char* args = GetCommandLineA();
	if (*args == '"') { args = strchr(args + 1, '"'); args = args ? args + 1 : ""; }
	else { while (*args && *args != ' ') args++; }
	/* Default: fullscreen at the desktop resolution. "-windowed" gives a 1280x720 window
	   (used to run several copies side by side for testing). */
	int windowed = strstr(args, "-windowed") != NULL;
	DWORD w = windowed ? 1280 : (DWORD)GetSystemMetrics(SM_CXSCREEN);
	DWORD h = windowed ? 720 : (DWORD)GetSystemMetrics(SM_CYSCREEN);
	char extra[128] = "";
	if (!windowed && !strstr(args, "-fullscreen")) strcat(extra, " -fullscreen");
	if (!strstr(args, "-width")) snprintf(extra + strlen(extra), sizeof extra - strlen(extra), " -width %lu -height %lu", w, h);
	snprintf(cmd, sizeof cmd, "\"%s\"%s%s", exe, args, extra);

	/* Also store the mode in the game's own display settings (HKCU\\Software\\THQ\\Barnyard, read by AOptions). */
	{
		HKEY k;
		if (RegCreateKeyExA(HKEY_CURRENT_USER, "Software\\THQ\\Barnyard", 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL) == ERROR_SUCCESS) {
			DWORD win = windowed;
			RegSetValueExA(k, "Width", 0, REG_DWORD, (const BYTE*)&w, sizeof w);
			RegSetValueExA(k, "Height", 0, REG_DWORD, (const BYTE*)&h, sizeof h);
			RegSetValueExA(k, "RealForcedWindowed", 0, REG_DWORD, (const BYTE*)&win, sizeof win);
			RegCloseKey(k);
		}
	}

	STARTUPINFOA si = { sizeof si };
	PROCESS_INFORMATION pi;
	if (!CreateProcessA(exe, cmd, NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, dir, &si, &pi))
		return fail("could not start barnyard.exe");

	size_t n = strlen(dll) + 1;
	void* mem = VirtualAllocEx(pi.hProcess, NULL, n, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	if (!mem || !WriteProcessMemory(pi.hProcess, mem, dll, n, NULL)) {
		TerminateProcess(pi.hProcess, 1);
		return fail("could not write into the game process");
	}
	HANDLE th = CreateRemoteThread(pi.hProcess, NULL, 0,
		(LPTHREAD_START_ROUTINE)GetProcAddress(GetModuleHandleA("kernel32.dll"), "LoadLibraryA"), mem, 0, NULL);
	if (!th) { TerminateProcess(pi.hProcess, 1); return fail("could not inject BYModCore.dll"); }
	WaitForSingleObject(th, 15000);
	DWORD mod = 0;
	GetExitCodeThread(th, &mod);
	CloseHandle(th);
	VirtualFreeEx(pi.hProcess, mem, 0, MEM_RELEASE);
	if (!mod) { TerminateProcess(pi.hProcess, 1); return fail("BYModCore.dll failed to load (missing VC++ x86 runtime?)"); }

	ResumeThread(pi.hThread);
	CloseHandle(pi.hThread);
	CloseHandle(pi.hProcess);
	return 0;
}
