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
	snprintf(cmd, sizeof cmd, "\"%s\"%s", exe, args);

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
