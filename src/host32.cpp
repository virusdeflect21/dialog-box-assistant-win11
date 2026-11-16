// 32-bit helper: installs the hook for 32-bit processes on a 64-bit system.
#pragma comment(lib, "user32.lib")
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>
#include <wchar.h>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR cmd, int) {
    DWORD ppid = 0;
    const wchar_t* p = wcsstr(cmd, L"/parent");
    if (p) ppid = (DWORD)wcstoul(p + 7, nullptr, 10);
    HANDLE parent = ppid ? OpenProcess(SYNCHRONIZE, FALSE, ppid) : nullptr;
    if (!parent) return 2;
    HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\DialogBoxAssistant_Host32");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    wchar_t* sl = wcsrchr(path, L'\\');
    if (sl) *(sl + 1) = 0;
    wcscat_s(path, L"DBAHook32.dll");
    HMODULE m = LoadLibraryW(path);
    if (!m) return 3;
    HOOKPROC proc = (HOOKPROC)GetProcAddress(m, "DBAHookProc");
    if (!proc) return 4;
    HHOOK h = SetWindowsHookExW(WH_CALLWNDPROCRET, proc, m, 0);
    if (!h) return 5;
    for (;;) {
        DWORD r = MsgWaitForMultipleObjects(1, &parent, FALSE, INFINITE, QS_ALLINPUT);
        if (r == WAIT_OBJECT_0) break;
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    UnhookWindowsHookEx(h);
    (void)single;
    return 0;
}
