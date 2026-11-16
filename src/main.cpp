// Dialog Box Assistant - Windows 11 port: tray application, options window, hot key, hook management.
#include "common.h"
#include <commctrl.h>
#include <shellapi.h>
#include <commdlg.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <uxtheme.h>
#include <dwmapi.h>
#include <map>
#include <set>
#include <algorithm>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "dwmapi.lib")

#define APP_TITLE L"Dialog Box Assistant"
#define IDI_APP 1

static HINSTANCE g_inst;
static HWND g_main;
static Settings g_settings;
static HHOOK g_hook;
static HMODULE g_hookDll;
static HANDLE g_host32;
static UINT g_wmTaskbar;
static bool g_trayAdded;

// ================================================================= helpers
static std::wstring ExeDir() {
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    PathRemoveFileSpecW(p);
    return p;
}
static std::wstring ExePath() {
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    return p;
}

static HFONT UiFont(UINT dpi) {
    NONCLIENTMETRICSW ncm = { sizeof(ncm) };
    if (!SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, dpi))
        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    return CreateFontIndirectW(&ncm.lfMessageFont);
}

// ================================================================= hooks
static void StartHooks() {
    std::wstring dll = ExeDir() + L"\\DBAHook64.dll";
    g_hookDll = LoadLibraryW(dll.c_str());
    if (g_hookDll) {
        HOOKPROC proc = (HOOKPROC)GetProcAddress(g_hookDll, "DBAHookProc");
        if (proc) g_hook = SetWindowsHookExW(WH_CALLWNDPROCRET, proc, g_hookDll, 0);
    }
    // 32-bit helper for 32-bit processes
    std::wstring host = ExeDir() + L"\\DBA32.exe";
    if (GetFileAttributesW(host.c_str()) != INVALID_FILE_ATTRIBUTES) {
        wchar_t cmd[MAX_PATH * 2];
        wsprintfW(cmd, L"\"%s\" /parent %lu", host.c_str(), GetCurrentProcessId());
        STARTUPINFOW si = { sizeof(si) };
        PROCESS_INFORMATION pi = {};
        if (CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
            g_host32 = pi.hProcess;
            CloseHandle(pi.hThread);
        }
    }
}
static void StopHooks() {
    if (g_hook) { UnhookWindowsHookEx(g_hook); g_hook = nullptr; }
    if (g_host32) { TerminateProcess(g_host32, 0); CloseHandle(g_host32); g_host32 = nullptr; }
}

// ================================================================= tray
static void UpdateTray() {
    NOTIFYICONDATAW nid = { sizeof(nid) };
    nid.hWnd = g_main;
    nid.uID = 1;
    if (g_settings.showTray) {
        nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        nid.uCallbackMessage = WM_DBA_TRAY;
        nid.hIcon = (HICON)LoadImageW(g_inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                                       GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
        lstrcpyW(nid.szTip, APP_TITLE);
        if (!Shell_NotifyIconW(g_trayAdded ? NIM_MODIFY : NIM_ADD, &nid) && g_trayAdded)
            Shell_NotifyIconW(NIM_ADD, &nid);
        g_trayAdded = true;
        if (nid.hIcon) DestroyIcon(nid.hIcon);
    } else if (g_trayAdded) {
        Shell_NotifyIconW(NIM_DELETE, &nid);
        g_trayAdded = false;
    }
}

static void RegisterHotKeyFromSettings() {
    UnregisterHotKey(g_main, 1);
    if (g_settings.hkVk && !RegisterHotKey(g_main, 1, g_settings.hkMod | MOD_NOREPEAT, g_settings.hkVk))
        MessageBoxW(nullptr, L"Failed to register hotkey. It maybe in use by another program.\n\n"
                             L"The program will still operate, but current hotkey will not be available.\n"
                             L"If necessary, you may use the Options dialog to change the hotkey.",
                    APP_TITLE, MB_ICONWARNING | MB_OK);
}

static void OpenFolder(const std::wstring& path) {
    if (!PathIsDirectoryW(path.c_str())) {
        MessageBoxW(g_main, (L"The folder \"" + path + L"\" is not available.").c_str(), APP_TITLE, MB_ICONWARNING | MB_OK);
        return;
    }
    ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

static void ShowCommonFoldersMenu(POINT pt) {
    std::vector<std::wstring> payload;
    HMENU m = BuildCommonFoldersMenu(payload);
    SetForegroundWindow(g_main);
    int cmd;
    { MenuThemeScope theme; cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, g_main, nullptr); }
    PostMessageW(g_main, WM_NULL, 0, 0);
    DestroyMenu(m);
    if (cmd >= MENU_ID_FIRST && (size_t)(cmd - MENU_ID_FIRST) < payload.size()) OpenFolder(payload[cmd - MENU_ID_FIRST]);
}

// ================================================================= options window
static void ShowOptions();

namespace opt {

enum {
    ID_CHK_SIZE = 101, ID_CHK_VIEW, ID_CHK_FOLDER, ID_CHK_OPTIONS, ID_CHK_TRAY, ID_CHK_RECENT,
    ID_HOTKEY = 110, ID_TAB = 111,
    ID_APPS = 120, ID_RB_DOCS = 131, ID_RB_FOLDERS = 132, ID_ADD = 133, ID_REMOVE = 134, ID_ITEMS = 135,
    ID_LOCK = 136, ID_MAX = 137, ID_UPDOWN = 138, ID_APPLY = 139, ID_SHOWLOCKED = 140, ID_SHOWCOMMON = 141,
    ID_EXCL = 142,
};

struct State {
    HWND wnd = nullptr, tab = nullptr, pages[3] = {};
    HWND chk[6] = {}, hk = nullptr, edExcl = nullptr;
    HWND lbApps = nullptr, lbItems = nullptr, rbDocs = nullptr, rbFolders = nullptr, btnAdd = nullptr, btnRemove = nullptr,
         chkLock = nullptr, edMax = nullptr, udMax = nullptr, btnApply = nullptr, chkShowLocked = nullptr, chkShowCommon = nullptr,
         stRemember = nullptr, stLockIcon = nullptr, stMenu = nullptr, stRange = nullptr, groupInfo = nullptr, stAppList = nullptr;
    HFONT font = nullptr, fontBold = nullptr;
    UINT dpi = 96;
    HICON lockIcon = nullptr;
    HBRUSH pageBrush = nullptr, ctrlBrush = nullptr;
    bool dark = false;
    Settings s;
    std::map<std::wstring, AppData> data;
    std::set<std::wstring> dirty;
    std::vector<std::wstring> apps;     // index 0 = COMMON ITEMS
    int selApp = 0;
    bool showFolders = true;
    bool loading = false;
};
static State* S = nullptr;

static int Sc(int v) { return MulDiv(v, S->dpi, 96); }

static HWND Mk(HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id, DWORD ex = 0) {
    HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, Sc(x), Sc(y), Sc(w), Sc(h), parent, (HMENU)(UINT_PTR)id, g_inst, nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)S->font, TRUE);
    return c;
}

static AppData& Cur() { return S->data[S->apps[S->selApp]]; }
static std::vector<Item>& CurList() { return S->showFolders ? Cur().folders : Cur().docs; }
static bool IsCommon() { return S->selApp == 0; }

static void RefreshItems(int sel = -1) {
    SendMessageW(S->lbItems, WM_SETREDRAW, FALSE, 0);
    SendMessageW(S->lbItems, LB_RESETCONTENT, 0, 0);
    for (auto& it : CurList()) SendMessageW(S->lbItems, LB_ADDSTRING, 0, (LPARAM)it.path.c_str());
    int n = (int)CurList().size();
    if (sel >= n) sel = n - 1;
    if (sel >= 0) SendMessageW(S->lbItems, LB_SETCURSEL, sel, 0);
    SendMessageW(S->lbItems, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(S->lbItems, nullptr, TRUE);
}

static void UpdateControls() {
    bool common = IsCommon();
    int sel = (int)SendMessageW(S->lbItems, LB_GETCURSEL, 0, 0);
    EnableWindow(S->btnRemove, sel >= 0);
    // lock + remember are not available for COMMON ITEMS
    int show = common ? SW_HIDE : SW_SHOW;
    ShowWindow(S->chkLock, show); ShowWindow(S->stLockIcon, show); ShowWindow(S->stRemember, show);
    ShowWindow(S->edMax, show); ShowWindow(S->udMax, show); ShowWindow(S->stRange, show); ShowWindow(S->btnApply, show);
    EnableWindow(S->chkLock, sel >= 0);
    S->loading = true;
    if (sel >= 0 && !common) SendMessageW(S->chkLock, BM_SETCHECK, CurList()[sel].locked ? BST_CHECKED : BST_UNCHECKED, 0);
    else SendMessageW(S->chkLock, BM_SETCHECK, BST_UNCHECKED, 0);
    if (!common) {
        int mx = S->showFolders ? Cur().maxFolders : Cur().maxDocs;
        SendMessageW(S->udMax, UDM_SETPOS32, 0, mx);
        SetWindowTextW(S->stRange, S->showFolders ? L"folders (1-50)" : L"documents (1-50)");
    }
    S->loading = false;
    EnableWindow(S->btnApply, FALSE);
}

static void RefreshApps(int sel) {
    SendMessageW(S->lbApps, LB_RESETCONTENT, 0, 0);
    for (auto& a : S->apps) SendMessageW(S->lbApps, LB_ADDSTRING, 0, (LPARAM)a.c_str());
    S->selApp = sel;
    SendMessageW(S->lbApps, LB_SETCURSEL, sel, 0);
}

static int EditMax() {
    wchar_t b[16] = {};
    GetWindowTextW(S->edMax, b, 16);
    int v = _wtoi(b);
    return std::min(50, std::max(1, v));
}

static void ApplyMax() {
    if (IsCommon()) return;
    int v = EditMax();
    AppData& d = Cur();
    int& mx = S->showFolders ? d.maxFolders : d.maxDocs;
    mx = v;
    auto& l = CurList();
    int cnt = 0;
    for (size_t i = 0; i < l.size();) {
        if (!l[i].locked && ++cnt > v) l.erase(l.begin() + i); else i++;
    }
    S->dirty.insert(S->apps[S->selApp]);
    int sel = (int)SendMessageW(S->lbItems, LB_GETCURSEL, 0, 0);
    RefreshItems(sel);
    UpdateControls();
}

static void SwitchTab(int i) {
    for (int k = 0; k < 3; k++) if (k != i) ShowWindow(S->pages[k], SW_HIDE);
    ShowWindow(S->pages[i], SW_SHOW);
    RedrawWindow(S->wnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
}

static void LayoutPages() {
    RECT rc; GetClientRect(S->tab, &rc);
    TabCtrl_AdjustRect(S->tab, FALSE, &rc);
    MapWindowPoints(S->tab, S->wnd, (POINT*)&rc, 2);
    for (int k = 0; k < 3; k++)
        SetWindowPos(S->pages[k], HWND_TOP, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, 0);
}

static LRESULT CALLBACK PageProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (!S) return DefWindowProcW(h, m, w, l);
    switch (m) {
    case WM_ERASEBKGND: {
        RECT r; GetClientRect(h, &r);
        FillRect((HDC)w, &r, S->pageBrush);
        return 1;
    }
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLOREDIT:
        if (S->dark) {
            HDC dc = (HDC)w;
            SetTextColor(dc, DK_TEXT); SetBkColor(dc, DK_CTRL);
            return (LRESULT)S->ctrlBrush;
        }
        break;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN: {
        HDC dc = (HDC)w;
        SetBkColor(dc, S->dark ? DK_BG : GetSysColor(COLOR_WINDOW));
        if (S->dark) SetTextColor(dc, DK_TEXT);
        return (LRESULT)S->pageBrush;
    }
    case WM_COMMAND: case WM_NOTIFY: case WM_DRAWITEM: case WM_MEASUREITEM: case WM_VSCROLL:
        return SendMessageW(GetParent(h), m, w, l);
    }
    return DefWindowProcW(h, m, w, l);
}

static void BuildMainPage(HWND p) {
    const wchar_t* t[6] = { L"Remember dialog size", L"Remember listview style", L"Remember recently used folder",
                            L"Show \"Options\" in the menu", L"Show application icon in the tray", L"Show \"Recent documents\" button" };
    for (int i = 0; i < 6; i++)
        S->chk[i] = Mk(p, L"BUTTON", t[i], BS_AUTOCHECKBOX | WS_TABSTOP | (i == 0 ? WS_GROUP : 0), 30, 24 + i * 28, 270, 20, ID_CHK_SIZE + i);
    Mk(p, L"STATIC", L"Shortcut key to open \"Common folders\" menu:", 0, 30, 24 + 6 * 28 + 14, 270, 32, -1);
    S->hk = CreateWindowExW(WS_EX_CLIENTEDGE, HOTKEY_CLASSW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP, Sc(30), Sc(24 + 6 * 28 + 38 + 14), Sc(250), Sc(24), p, (HMENU)(UINT_PTR)ID_HOTKEY, g_inst, nullptr);
    Mk(p, L"STATIC", L"Exclusion list - programs to leave alone (one file name per line, e.g. notepad.exe):", 0, 316, 22, 282, 48, -1);
    S->edExcl = Mk(p, L"EDIT", L"", ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | WS_VSCROLL | WS_TABSTOP, 316, 74, 282, 180, ID_EXCL, WS_EX_CLIENTEDGE);
    SendMessageW(S->hk, WM_SETFONT, (WPARAM)S->font, TRUE);
    SendMessageW(S->hk, HKM_SETRULES, HKCOMB_NONE | HKCOMB_S, MAKELPARAM(HOTKEYF_CONTROL | HOTKEYF_ALT, 0));
}

static void BuildAppPage(HWND p) {
    S->stAppList = Mk(p, L"STATIC", L"Application list", 0, 12, 8, 150, 16, -1);
    S->lbApps = Mk(p, L"LISTBOX", L"", LBS_NOTIFY | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | WS_VSCROLL | WS_TABSTOP, 12, 28, 160, 252, ID_APPS, WS_EX_CLIENTEDGE);
    S->groupInfo = Mk(p, L"BUTTON", L"Application information", BS_GROUPBOX, 186, 6, 416, 276, -1);
    S->rbDocs = Mk(p, L"BUTTON", L"Documents", BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP, 200, 28, 96, 20, ID_RB_DOCS);
    S->rbFolders = Mk(p, L"BUTTON", L"Folders", BS_AUTORADIOBUTTON | WS_TABSTOP, 302, 28, 90, 20, ID_RB_FOLDERS);
    S->btnAdd = Mk(p, L"BUTTON", L"Add", BS_PUSHBUTTON | WS_TABSTOP, 430, 24, 78, 24, ID_ADD);
    S->btnRemove = Mk(p, L"BUTTON", L"Remove", BS_PUSHBUTTON | WS_TABSTOP, 516, 24, 78, 24, ID_REMOVE);
    S->lbItems = Mk(p, L"LISTBOX", L"", LBS_NOTIFY | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | WS_VSCROLL | WS_HSCROLL | WS_TABSTOP, 198, 54, 396, 150, ID_ITEMS, WS_EX_CLIENTEDGE);
    S->stLockIcon = Mk(p, L"STATIC", L"", SS_ICON | SS_CENTERIMAGE, 198, 214, 20, 20, -1);
    S->chkLock = Mk(p, L"BUTTON", L"Lock item in the list", BS_AUTOCHECKBOX | WS_TABSTOP, 224, 214, 190, 20, ID_LOCK);
    S->stRemember = Mk(p, L"STATIC", L"Remember:", 0, 198, 248, 62, 18, -1);
    S->edMax = Mk(p, L"EDIT", L"10", ES_NUMBER | ES_CENTER | WS_TABSTOP, 262, 244, 40, 24, ID_MAX, WS_EX_CLIENTEDGE);
    S->udMax = CreateWindowExW(0, UPDOWN_CLASSW, L"", WS_CHILD | WS_VISIBLE | UDS_SETBUDDYINT | UDS_ALIGNRIGHT | UDS_ARROWKEYS | UDS_NOTHOUSANDS,
                               0, 0, 0, 0, p, (HMENU)(UINT_PTR)ID_UPDOWN, g_inst, nullptr);
    SendMessageW(S->udMax, UDM_SETBUDDY, (WPARAM)S->edMax, 0);
    SendMessageW(S->udMax, UDM_SETRANGE32, 1, 50);
    S->stRange = Mk(p, L"STATIC", L"folders (1-50)", 0, 310, 248, 92, 18, -1);
    S->btnApply = Mk(p, L"BUTTON", L"Apply", BS_PUSHBUTTON | WS_TABSTOP, 190 + 0, 0, 0, 0, ID_APPLY);
    SetWindowPos(S->btnApply, nullptr, Sc(300), Sc(0), 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    // placed below the remember row
    MoveWindow(S->btnApply, Sc(420), Sc(214 + 0), Sc(0), Sc(0), FALSE);
    S->stMenu = Mk(p, L"STATIC", L"In the menu:", 0, 474, 214, 124, 16, -1);
    S->chkShowLocked = Mk(p, L"BUTTON", L"Show locked items", BS_AUTOCHECKBOX | WS_TABSTOP, 474, 234, 124, 20, ID_SHOWLOCKED);
    S->chkShowCommon = Mk(p, L"BUTTON", L"Show common items", BS_AUTOCHECKBOX | WS_TABSTOP, 474, 256, 124, 20, ID_SHOWCOMMON);
    // Apply button sits right of the remember row
    MoveWindow(S->btnApply, Sc(406), Sc(243), Sc(62), Sc(26), TRUE);
    S->lockIcon = nullptr;
    SHSTOCKICONINFO si = { sizeof(si) };
    if (SUCCEEDED(SHGetStockIconInfo(SIID_LOCK, SHGSI_ICON | SHGSI_SMALLICON, &si))) {
        S->lockIcon = si.hIcon;
        SendMessageW(S->stLockIcon, STM_SETICON, (WPARAM)S->lockIcon, 0);
    }
}

static void BuildAboutPage(HWND p) {
    HICON ic = (HICON)LoadImageW(g_inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, Sc(48), Sc(48), 0);
    HWND si = Mk(p, L"STATIC", L"", SS_ICON, 30, 26, 48, 48, -1);
    SendMessageW(si, STM_SETICON, (WPARAM)ic, 0);
    HWND t = Mk(p, L"STATIC", APP_TITLE, 0, 96, 28, 400, 22, -1);
    SendMessageW(t, WM_SETFONT, (WPARAM)S->fontBold, TRUE);
    Mk(p, L"STATIC", L"Version " DBA_VERSION_W L" (Windows 11)", 0, 96, 52, 400, 18, -1);
    Mk(p, L"STATIC",
       L"Dialog Box Assistant adds two buttons to the standard Open and Save As dialog boxes: "
       L"one for the list of recently used files, another for the recent folders. It also remembers the size "
       L"and the list style of the file dialogs and keeps a list of recently used folders and files for every program.\r\n\r\n"
       L"Press the shortcut key to open your favorite (\"Common\") folders from anywhere. You can add folders to the "
       L"list from the Windows Explorer context menu.\r\n\r\n"
       L"Based on Dialog Box Assistant 1.5.1, Copyright (C) 2002 Duality Software.",
       0, 30, 100, 560, 150, -1);
}

static void LoadIntoControls() {
    SendMessageW(S->chk[0], BM_SETCHECK, S->s.rememberSize, 0);
    SendMessageW(S->chk[1], BM_SETCHECK, S->s.rememberView, 0);
    SendMessageW(S->chk[2], BM_SETCHECK, S->s.rememberFolder, 0);
    SendMessageW(S->chk[3], BM_SETCHECK, S->s.showOptions, 0);
    SendMessageW(S->chk[4], BM_SETCHECK, S->s.showTray, 0);
    SendMessageW(S->chk[5], BM_SETCHECK, S->s.showRecent, 0);
    SendMessageW(S->chkShowLocked, BM_SETCHECK, S->s.showLocked, 0);
    SendMessageW(S->chkShowCommon, BM_SETCHECK, S->s.showCommon, 0);
    BYTE f = 0;
    if (S->s.hkMod & MOD_SHIFT) f |= HOTKEYF_SHIFT;
    if (S->s.hkMod & MOD_CONTROL) f |= HOTKEYF_CONTROL;
    if (S->s.hkMod & MOD_ALT) f |= HOTKEYF_ALT;
    SendMessageW(S->hk, HKM_SETHOTKEY, MAKEWORD(S->s.hkVk, f), 0);
    {
        std::wstring ex;
        for (auto& e : S->s.excluded) { if (!ex.empty()) ex += L"\r\n"; ex += e; }
        SetWindowTextW(S->edExcl, ex.c_str());
    }
    SendMessageW(S->rbFolders, BM_SETCHECK, BST_CHECKED, 0);
    S->showFolders = true;
}

static void Commit() {
    ApplyMax();
    Settings n = S->s;
    n.rememberSize = SendMessageW(S->chk[0], BM_GETCHECK, 0, 0) == BST_CHECKED;
    n.rememberView = SendMessageW(S->chk[1], BM_GETCHECK, 0, 0) == BST_CHECKED;
    n.rememberFolder = SendMessageW(S->chk[2], BM_GETCHECK, 0, 0) == BST_CHECKED;
    n.showOptions = SendMessageW(S->chk[3], BM_GETCHECK, 0, 0) == BST_CHECKED;
    n.showTray = SendMessageW(S->chk[4], BM_GETCHECK, 0, 0) == BST_CHECKED;
    n.showRecent = SendMessageW(S->chk[5], BM_GETCHECK, 0, 0) == BST_CHECKED;
    n.showLocked = SendMessageW(S->chkShowLocked, BM_GETCHECK, 0, 0) == BST_CHECKED;
    n.showCommon = SendMessageW(S->chkShowCommon, BM_GETCHECK, 0, 0) == BST_CHECKED;
    WORD hk = (WORD)SendMessageW(S->hk, HKM_GETHOTKEY, 0, 0);
    BYTE vk = LOBYTE(hk), f = HIBYTE(hk);
    n.hkVk = vk;
    n.hkMod = 0;
    if (f & HOTKEYF_SHIFT) n.hkMod |= MOD_SHIFT;
    if (f & HOTKEYF_CONTROL) n.hkMod |= MOD_CONTROL;
    if (f & HOTKEYF_ALT) n.hkMod |= MOD_ALT;
    if (!n.hkMod) n.hkVk = 0;
    n.excluded.clear();
    {
        int len = GetWindowTextLengthW(S->edExcl);
        std::wstring txt(len + 1, L'\0');
        GetWindowTextW(S->edExcl, &txt[0], len + 1);
        txt.resize(wcslen(txt.c_str()));
        size_t i = 0;
        while (i <= txt.size()) {
            size_t j = txt.find_first_of(L"\r\n", i);
            if (j == std::wstring::npos) j = txt.size();
            std::wstring line = txt.substr(i, j - i);
            while (!line.empty() && (line.back() == L' ' || line.back() == L'\t')) line.pop_back();
            size_t a = line.find_first_not_of(L" \t");
            if (a != std::wstring::npos) n.excluded.push_back(line.substr(a));
            i = j + 1;
        }
    }
    SaveSettings(n);
    for (auto& name : S->dirty) {
        AppData d = S->data[name];
        // keep entries that other programs added while this window was open
        SaveApp(name, d);
    }
    PostMessageW(g_main, WM_DBA_RELOAD, 0, 0);
}

static void AddItem() {
    std::wstring path;
    if (S->showFolders) {
        IFileOpenDialog* fd = nullptr;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&fd)))) return;
        DWORD fl = 0; fd->GetOptions(&fl);
        fd->SetOptions(fl | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        if (SUCCEEDED(fd->Show(S->wnd))) {
            IShellItem* si = nullptr;
            if (SUCCEEDED(fd->GetResult(&si))) {
                PWSTR pp = nullptr;
                if (SUCCEEDED(si->GetDisplayName(SIGDN_FILESYSPATH, &pp))) { path = pp; CoTaskMemFree(pp); }
                si->Release();
            }
        }
        fd->Release();
    } else {
        wchar_t buf[MAX_PATH * 2] = {};
        OPENFILENAMEW ofn = { sizeof(ofn) };
        ofn.hwndOwner = S->wnd;
        ofn.lpstrFile = buf; ofn.nMaxFile = MAX_PATH * 2;
        ofn.lpstrFilter = L"All files\0*.*\0";
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
        if (GetOpenFileNameW(&ofn)) path = buf;
    }
    if (path.empty()) return;
    path = NormalizePath(path);
    auto& l = CurList();
    for (auto& it : l) if (PathEq(it.path, path)) {
        MessageBoxW(S->wnd, S->showFolders ? L"This folder is in the list already." : L"This document is in the list already.",
                    APP_TITLE, MB_ICONINFORMATION | MB_OK);
        return;
    }
    Item it; it.path = path; it.locked = false;
    int pos = 0;
    if (IsCommon()) { l.push_back(it); pos = (int)l.size() - 1; }
    else {
        l.insert(l.begin(), it);
        int mx = S->showFolders ? Cur().maxFolders : Cur().maxDocs;
        int cnt = 0;
        for (size_t i = 0; i < l.size();) {
            if (!l[i].locked && ++cnt > mx) { if ((int)i == 0) pos = -1; l.erase(l.begin() + i); } else i++;
        }
    }
    S->dirty.insert(S->apps[S->selApp]);
    RefreshItems(pos);
    UpdateControls();
}

static void RemoveItem() {
    int sel = (int)SendMessageW(S->lbItems, LB_GETCURSEL, 0, 0);
    auto& l = CurList();
    if (sel < 0 || sel >= (int)l.size()) return;
    l.erase(l.begin() + sel);
    S->dirty.insert(S->apps[S->selApp]);
    RefreshItems(sel);
    UpdateControls();
}

static void ToggleLock() {
    if (S->loading || IsCommon()) return;
    int sel = (int)SendMessageW(S->lbItems, LB_GETCURSEL, 0, 0);
    auto& l = CurList();
    if (sel < 0 || sel >= (int)l.size()) return;
    l[sel].locked = SendMessageW(S->chkLock, BM_GETCHECK, 0, 0) == BST_CHECKED;
    S->dirty.insert(S->apps[S->selApp]);
    InvalidateRect(S->lbItems, nullptr, TRUE);
}

static void FillItemBg(DRAWITEMSTRUCT* di, bool sel) {
    if (!S->dark) { FillRect(di->hDC, &di->rcItem, GetSysColorBrush(sel ? COLOR_HIGHLIGHT : COLOR_WINDOW)); return; }
    HBRUSH b = CreateSolidBrush(sel ? DK_SEL : DK_CTRL);
    FillRect(di->hDC, &di->rcItem, b);
    DeleteObject(b);
}

// Dark tab control: the themed control cannot be recolored, so paint it ourselves.
static LRESULT CALLBACK DarkTabProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR) {
    switch (m) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc; GetClientRect(h, &rc);
        HBRUSH bg = CreateSolidBrush(DK_BG), tabBg = CreateSolidBrush(DK_CTRL), edge = CreateSolidBrush(DK_EDGE);
        FillRect(dc, &rc, bg);
        RECT body = rc; TabCtrl_AdjustRect(h, FALSE, &body);
        InflateRect(&body, 2, 2);
        FrameRect(dc, &body, edge);
        HGDIOBJ of = SelectObject(dc, (HFONT)SendMessageW(h, WM_GETFONT, 0, 0));
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, DK_TEXT);
        int n = TabCtrl_GetItemCount(h), cur = TabCtrl_GetCurSel(h);
        for (int i = 0; i < n; i++) {
            RECT r; TabCtrl_GetItemRect(h, i, &r);
            FillRect(dc, &r, i == cur ? bg : tabBg);
            FrameRect(dc, &r, edge);
            if (i == cur) { RECT g = { r.left + 1, r.bottom - 1, r.right - 1, r.bottom + 1 }; FillRect(dc, &g, bg); }
            wchar_t t[64] = {};
            TCITEMW ti = {}; ti.mask = TCIF_TEXT; ti.pszText = t; ti.cchTextMax = 64;
            TabCtrl_GetItem(h, i, &ti);
            DrawTextW(dc, t, -1, &r, DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);
        }
        SelectObject(dc, of);
        DeleteObject(bg); DeleteObject(tabBg); DeleteObject(edge);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_NCDESTROY: RemoveWindowSubclass(h, DarkTabProc, id); break;
    }
    return DefSubclassProc(h, m, w, l);
}

static void DrawAppItem(DRAWITEMSTRUCT* di) {
    if ((int)di->itemID < 0) return;
    bool sel = (di->itemState & ODS_SELECTED) != 0;
    FillItemBg(di, sel);
    wchar_t t[512];
    SendMessageW(di->hwndItem, LB_GETTEXT, di->itemID, (LPARAM)t);
    SetBkMode(di->hDC, TRANSPARENT);
    SetTextColor(di->hDC, sel ? (S->dark ? DK_TEXT : GetSysColor(COLOR_HIGHLIGHTTEXT))
                              : (di->itemID == 0 ? (S->dark ? RGB(120, 170, 255) : RGB(0, 0, 255))
                                                 : (S->dark ? DK_TEXT : GetSysColor(COLOR_WINDOWTEXT))));
    RECT r = di->rcItem; r.left += Sc(4);
    DrawTextW(di->hDC, t, -1, &r, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
    if (di->itemState & ODS_FOCUS) DrawFocusRect(di->hDC, &di->rcItem);
}

static void DrawListItem(DRAWITEMSTRUCT* di) {
    if ((int)di->itemID < 0) return;
    auto& l = CurList();
    if (di->itemID >= l.size()) return;
    bool sel = (di->itemState & ODS_SELECTED) != 0;
    FillItemBg(di, sel);
    int isz = GetSystemMetricsForDpi(SM_CXSMICON, S->dpi);
    if (l[di->itemID].locked && S->lockIcon)
        DrawIconEx(di->hDC, di->rcItem.left + Sc(2), di->rcItem.top + ((di->rcItem.bottom - di->rcItem.top) - isz) / 2, S->lockIcon, isz, isz, 0, nullptr, DI_NORMAL);
    SetBkMode(di->hDC, TRANSPARENT);
    SetTextColor(di->hDC, S->dark ? DK_TEXT : (sel ? GetSysColor(COLOR_HIGHLIGHTTEXT) : GetSysColor(COLOR_WINDOWTEXT)));
    RECT r = di->rcItem; r.left += Sc(2) + isz + Sc(6);
    DrawTextW(di->hDC, l[di->itemID].path.c_str(), -1, &r, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
    if (di->itemState & ODS_FOCUS) DrawFocusRect(di->hDC, &di->rcItem);
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_NOTIFY: {
        NMHDR* nh = (NMHDR*)l;
        if (nh->idFrom == ID_TAB && nh->code == TCN_SELCHANGE) { SwitchTab(TabCtrl_GetCurSel(S->tab)); return 0; }
        break;
    }
    case WM_DRAWITEM: {
        DRAWITEMSTRUCT* di = (DRAWITEMSTRUCT*)l;
        if (di->CtlID == ID_APPS) { DrawAppItem(di); return TRUE; }
        if (di->CtlID == ID_ITEMS) { DrawListItem(di); return TRUE; }
        break;
    }
    case WM_MEASUREITEM: {
        MEASUREITEMSTRUCT* mi = (MEASUREITEMSTRUCT*)l;
        if (mi->CtlID == ID_APPS || mi->CtlID == ID_ITEMS) {
            mi->itemHeight = std::max(GetSystemMetricsForDpi(SM_CYSMICON, S->dpi), 16) + Sc(4);
            return TRUE;
        }
        break;
    }
    case WM_COMMAND: {
        int id = LOWORD(w), code = HIWORD(w);
        switch (id) {
        case IDOK: Commit(); DestroyWindow(h); return 0;
        case IDCANCEL: DestroyWindow(h); return 0;
        case ID_APPS:
            if (code == LBN_SELCHANGE) {
                if (IsCommon() == false) ApplyMax();   // keep a typed value
                int s = (int)SendMessageW(S->lbApps, LB_GETCURSEL, 0, 0);
                if (s >= 0) { S->selApp = s; RefreshItems(0); UpdateControls(); }
            }
            return 0;
        case ID_RB_DOCS: case ID_RB_FOLDERS:
            if (code == BN_CLICKED) {
                if (!IsCommon()) ApplyMax();
                S->showFolders = (id == ID_RB_FOLDERS);
                RefreshItems(0); UpdateControls();
            }
            return 0;
        case ID_ITEMS:
            if (code == LBN_SELCHANGE) UpdateControls();
            return 0;
        case ID_ADD: AddItem(); return 0;
        case ID_REMOVE: RemoveItem(); return 0;
        case ID_LOCK: if (code == BN_CLICKED) ToggleLock(); return 0;
        case ID_APPLY: ApplyMax(); return 0;
        case ID_MAX:
            if (code == EN_CHANGE && !S->loading) {
                int mx = S->showFolders ? Cur().maxFolders : Cur().maxDocs;
                EnableWindow(S->btnApply, EditMax() != mx);
            }
            return 0;
        }
        break;
    }
    case WM_ERASEBKGND:
        if (S && S->dark) {
            RECT r; GetClientRect(h, &r);
            FillRect((HDC)w, &r, S->pageBrush);
            return 1;
        }
        break;
    case WM_TIMER:
        if (w == 0xDBA1) {
            KillTimer(h, 0xDBA1);
            if (IsIconic(h)) ShowWindow(h, SW_RESTORE);
            return 0;
        }
        break;
    case WM_CLOSE: DestroyWindow(h); return 0;
    case WM_DESTROY: {
        if (S) {
            if (S->ctrlBrush) DeleteObject(S->ctrlBrush);
            if (S->font) DeleteObject(S->font);
            if (S->fontBold) DeleteObject(S->fontBold);
            if (S->pageBrush) DeleteObject(S->pageBrush);
            if (S->lockIcon) DestroyIcon(S->lockIcon);
            delete S;
            S = nullptr;
        }
        return 0;
    }
    }
    return DefWindowProcW(h, m, w, l);
}

static HWND g_optWnd;

bool Show() {
    if (g_optWnd && IsWindow(g_optWnd)) {
        if (IsIconic(g_optWnd)) ShowWindow(g_optWnd, SW_RESTORE);
        SetForegroundWindow(g_optWnd);
        return true;
    }
    static bool reg = false;
    if (!reg) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc = WndProc; wc.hInstance = g_inst; wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1); wc.lpszClassName = L"DBAOptionsWnd";
        wc.hIcon = LoadIconW(g_inst, MAKEINTRESOURCEW(IDI_APP));
        RegisterClassW(&wc);
        WNDCLASSW pc = {};
        pc.lpfnWndProc = PageProc; pc.hInstance = g_inst; pc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        pc.lpszClassName = L"DBAOptionsPage";
        RegisterClassW(&pc);
        reg = true;
    }
    S = new State();
    S->dpi = GetDpiForSystem();
    S->s = LoadSettings();
    S->font = UiFont(S->dpi);
    LOGFONTW lf; GetObjectW(S->font, sizeof(lf), &lf); lf.lfWeight = FW_BOLD;
    S->fontBold = CreateFontIndirectW(&lf);
    S->dark = IsDarkMode();
    S->pageBrush = CreateSolidBrush(S->dark ? DK_BG : GetSysColor(COLOR_WINDOW));
    S->ctrlBrush = CreateSolidBrush(DK_CTRL);

    S->apps.push_back(COMMON_ITEMS_NAME);
    for (auto& a : ListApps()) S->apps.push_back(a);
    for (auto& a : S->apps) S->data[a] = LoadApp(a);

    int cw = 640, ch = 384;
    RECT wr = { 0, 0, Sc(cw), Sc(ch) };
    AdjustWindowRectExForDpi(&wr, WS_CAPTION | WS_SYSMENU, FALSE, 0, S->dpi);
    int W = wr.right - wr.left, H = wr.bottom - wr.top;
    POINT pt; GetCursorPos(&pt);
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY), &mi);
    int x = mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - W) / 2;
    int y = mi.rcWork.top + ((mi.rcWork.bottom - mi.rcWork.top) - H) / 2;
    S->wnd = CreateWindowExW(WS_EX_CONTROLPARENT, L"DBAOptionsWnd", APP_TITLE, WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN,
                             x, y, W, H, nullptr, nullptr, g_inst, nullptr);
    if (!S->wnd) { wchar_t b[64]; wsprintfW(b, L"CreateWindow failed: %lu", GetLastError()); MessageBoxW(nullptr, b, APP_TITLE, MB_ICONERROR); delete S; S = nullptr; return false; }
    g_optWnd = S->wnd;
    S->tab = CreateWindowExW(0, WC_TABCONTROLW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_CLIPSIBLINGS, Sc(8), Sc(8), Sc(cw - 16), Sc(ch - 56), S->wnd, (HMENU)(UINT_PTR)ID_TAB, g_inst, nullptr);
    SendMessageW(S->tab, WM_SETFONT, (WPARAM)S->font, TRUE);
    const wchar_t* names[3] = { L"Main Options", L"Application Options", L"About" };
    for (int i = 0; i < 3; i++) {
        TCITEMW ti = {}; ti.mask = TCIF_TEXT; ti.pszText = (LPWSTR)names[i];
        TabCtrl_InsertItem(S->tab, i, &ti);
    }
    for (int i = 0; i < 3; i++)
        S->pages[i] = CreateWindowExW(WS_EX_CONTROLPARENT, L"DBAOptionsPage", L"", WS_CHILD, 0, 0, 10, 10, S->wnd, nullptr, g_inst, nullptr);
    LayoutPages();
    BuildMainPage(S->pages[0]);
    BuildAppPage(S->pages[1]);
    BuildAboutPage(S->pages[2]);
    HWND ok = Mk(S->wnd, L"BUTTON", L"OK", BS_DEFPUSHBUTTON | WS_TABSTOP, cw - 8 - 80 - 8 - 80, ch - 36, 80, 26, IDOK);
    Mk(S->wnd, L"BUTTON", L"Cancel", BS_PUSHBUTTON | WS_TABSTOP, cw - 8 - 80, ch - 36, 80, 26, IDCANCEL);
    (void)ok;
    if (S->dark) {
        BOOL on = TRUE;
        DwmSetWindowAttribute(S->wnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &on, sizeof(on));
        auto theme = [](HWND c, LPARAM) -> BOOL {
            wchar_t cls[32] = {};
            GetClassNameW(c, cls, 32);
            // owner-drawn tab headers and plain statics need no visual style
            if (_wcsicmp(cls, L"Static") && _wcsicmp(cls, WC_TABCONTROLW))
                SetWindowTheme(c, (!_wcsicmp(cls, L"Edit") || !_wcsicmp(cls, HOTKEY_CLASSW)) ? L"DarkMode_CFD" : L"DarkMode_Explorer", nullptr);
            return TRUE;
        };
        EnumChildWindows(S->wnd, theme, 0);   // recurses into the page windows
        SetWindowSubclass(S->tab, DarkTabProc, 1, 0);
    }
    LoadIntoControls();
    RefreshApps(0);
    // start with the first real application when there is one
    if (S->apps.size() > 1) RefreshApps(1);
    RefreshItems(0);
    UpdateControls();
    SwitchTab(0);
    ShowWindow(S->wnd, SW_SHOWNORMAL);
    if (IsIconic(S->wnd)) ShowWindow(S->wnd, SW_RESTORE);   // first ShowWindow may obey a "minimized" launch flag
    SetWindowPos(S->wnd, HWND_TOP, x, y, W, H, SWP_SHOWWINDOW);
    SetForegroundWindow(S->wnd);
    if (IsIconic(S->wnd)) ShowWindow(S->wnd, SW_RESTORE);
    SetTimer(S->wnd, 0xDBA1, 150, nullptr);   // one-shot safety net, see WM_TIMER
    return true;
}

} // namespace opt

static void ShowOptions() { opt::Show(); }

// ================================================================= main window
static LRESULT CALLBACK MainProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == g_wmTaskbar && g_wmTaskbar) { g_trayAdded = false; UpdateTray(); return 0; }
    switch (m) {
    case WM_DBA_TRAY:
        switch (LOWORD(l)) {
        case WM_LBUTTONDBLCLK: ShowOptions(); break;
        case WM_RBUTTONUP: case WM_CONTEXTMENU: {
            POINT pt; GetCursorPos(&pt);
            std::vector<std::wstring> payload;
            HMENU menu = CreatePopupMenu();
            HMENU common = BuildCommonFoldersMenu(payload);
            AppendMenuW(menu, MF_POPUP, (UINT_PTR)common, L"Common Folders");
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, MENU_ID_OPTIONS, L"Options");
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, 998, L"Exit");
            SetMenuDefaultItem(menu, MENU_ID_OPTIONS, FALSE);
            SetForegroundWindow(h);
            int cmd;
            { MenuThemeScope theme; cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, h, nullptr); }
            PostMessageW(h, WM_NULL, 0, 0);
            DestroyMenu(menu);
            if (cmd == MENU_ID_OPTIONS) ShowOptions();
            else if (cmd == 998) DestroyWindow(h);
            else if (cmd >= MENU_ID_FIRST && (size_t)(cmd - MENU_ID_FIRST) < payload.size()) OpenFolder(payload[cmd - MENU_ID_FIRST]);
            break;
        }
        }
        return 0;
    case WM_HOTKEY: {
        POINT pt; GetCursorPos(&pt);
        ShowCommonFoldersMenu(pt);
        return 0;
    }
    case WM_DBA_OPTIONS: ShowOptions(); return 0;
    case WM_DBA_RELOAD:
        g_settings = LoadSettings();
        UpdateTray();
        RegisterHotKeyFromSettings();
        return 0;
    case WM_MEASUREITEM:
        if (MenuMeasure(h, (MEASUREITEMSTRUCT*)l)) return TRUE;
        break;
    case WM_DRAWITEM:
        if (MenuDraw((DRAWITEMSTRUCT*)l)) return TRUE;
        break;
    case WM_ENDSESSION:
        if (w) DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        UnregisterHotKey(h, 1);
        {
            NOTIFYICONDATAW nid = { sizeof(nid) };
            nid.hWnd = h; nid.uID = 1;
            Shell_NotifyIconW(NIM_DELETE, &nid);
        }
        if (opt::g_optWnd && IsWindow(opt::g_optWnd)) DestroyWindow(opt::g_optWnd);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

// ================================================================= install / command line
static void SetRegStr(HKEY root, const std::wstring& path, const wchar_t* name, const std::wstring& v) {
    HKEY k;
    if (RegCreateKeyExW(root, path.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) == ERROR_SUCCESS) {
        RegSetValueExW(k, name, 0, REG_SZ, (const BYTE*)v.c_str(), (DWORD)((v.size() + 1) * 2));
        RegCloseKey(k);
    }
}

static std::wstring StartMenuDir() {
    wchar_t p[MAX_PATH];
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_PROGRAMS, nullptr, SHGFP_TYPE_CURRENT, p))) return L"";
    return std::wstring(p) + L"\\Dialog Box Assistant";
}

static void MakeShortcut(const std::wstring& lnk, const std::wstring& args, const wchar_t* desc) {
    IShellLinkW* sl = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sl)))) return;
    std::wstring exe = ExePath();
    sl->SetPath(exe.c_str());
    sl->SetArguments(args.c_str());
    sl->SetDescription(desc);
    sl->SetWorkingDirectory(ExeDir().c_str());
    IPersistFile* pf = nullptr;
    if (SUCCEEDED(sl->QueryInterface(IID_PPV_ARGS(&pf)))) { pf->Save(lnk.c_str(), TRUE); pf->Release(); }
    sl->Release();
}

static void Install() {
    std::wstring exe = ExePath();
    std::wstring base = L"Software\\Classes\\Directory\\shell\\DialogBoxAssistant";
    SetRegStr(HKEY_CURRENT_USER, base, L"MUIVerb", APP_TITLE);
    SetRegStr(HKEY_CURRENT_USER, base, L"SubCommands", L"");
    SetRegStr(HKEY_CURRENT_USER, base, L"Icon", L"\"" + exe + L"\",0");
    struct V { const wchar_t* key; const wchar_t* label; const wchar_t* arg; } v[3] = {
        { L"1add", L"Add to Common Folders", L"/add" },
        { L"2remove", L"Remove from Common Folders", L"/remove" },
        { L"3clear", L"Clear Common Folders", L"/clear" } };
    for (auto& e : v) {
        std::wstring k = base + L"\\shell\\" + e.key;
        SetRegStr(HKEY_CURRENT_USER, k, L"MUIVerb", e.label);
        std::wstring cmd = L"\"" + exe + L"\" " + e.arg + (wcscmp(e.arg, L"/clear") ? L" \"%1\"" : L"");
        SetRegStr(HKEY_CURRENT_USER, k + L"\\command", nullptr, cmd);
    }
    SetRegStr(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", L"DialogBoxAssistant", L"\"" + exe + L"\" /silent");
    std::wstring dir = StartMenuDir();
    if (!dir.empty()) {
        CreateDirectoryW(dir.c_str(), nullptr);
        MakeShortcut(dir + L"\\Dialog Box Assistant.lnk", L"/silent", L"Start Dialog Box Assistant");
        MakeShortcut(dir + L"\\Options.lnk", L"/options", L"Dialog Box Assistant options");
    }
    // allow packaged (AppContainer) applications to load the hook libraries
    wchar_t cmd[MAX_PATH * 2];
    wsprintfW(cmd, L"icacls.exe \"%s\" /grant *S-1-15-2-1:(OI)(CI)(RX) /Q", ExeDir().c_str());
    STARTUPINFOW si = { sizeof(si) }; si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};
    if (CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 15000);
        CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    }
}

static void Uninstall() {
    RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\Classes\\Directory\\shell\\DialogBoxAssistant");
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_WRITE, &k) == ERROR_SUCCESS) {
        RegDeleteValueW(k, L"DialogBoxAssistant");
        RegCloseKey(k);
    }
    std::wstring dir = StartMenuDir();
    if (!dir.empty()) {
        DeleteFileW((dir + L"\\Dialog Box Assistant.lnk").c_str());
        DeleteFileW((dir + L"\\Options.lnk").c_str());
        RemoveDirectoryW(dir.c_str());
    }
    HWND w = FindWindowW(DBA_MAIN_CLASS, nullptr);
    if (w) PostMessageW(w, WM_CLOSE, 0, 0);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    g_inst = inst;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring cmd = argc > 1 ? argv[1] : L"";
    std::wstring arg = argc > 2 ? argv[2] : L"";
    if (argv) LocalFree(argv);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    if (_wcsicmp(cmd.c_str(), L"/add") == 0) { CommonAdd(arg); return 0; }
    if (_wcsicmp(cmd.c_str(), L"/remove") == 0) { CommonRemove(arg); return 0; }
    if (_wcsicmp(cmd.c_str(), L"/clear") == 0) { CommonClear(); return 0; }
    if (_wcsicmp(cmd.c_str(), L"/install") == 0) { Install(); return 0; }
    if (_wcsicmp(cmd.c_str(), L"/uninstall") == 0) { Uninstall(); return 0; }

    HWND existing = FindWindowW(DBA_MAIN_CLASS, nullptr);
    if (existing) {
        if (_wcsicmp(cmd.c_str(), L"/silent") != 0) {   // double-click / /options: show the window
            DWORD pid = 0; GetWindowThreadProcessId(existing, &pid);
            AllowSetForegroundWindow(pid);
            PostMessageW(existing, WM_DBA_OPTIONS, 0, 0);
        }
        return 0;
    }
    HANDLE mtx = CreateMutexW(nullptr, TRUE, L"Local\\DialogBoxAssistant_Main");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_TAB_CLASSES | ICC_HOTKEY_CLASS | ICC_UPDOWN_CLASS | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    WNDCLASSW wc = {};
    wc.lpfnWndProc = MainProc; wc.hInstance = inst; wc.lpszClassName = DBA_MAIN_CLASS;
    RegisterClassW(&wc);
    g_main = CreateWindowExW(0, DBA_MAIN_CLASS, APP_TITLE, WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, inst, nullptr);
    if (!g_main) return 1;
    // The first ShowWindow call of a process obeys the launcher's STARTUPINFO (can be "minimized/hidden").
    // Spend it on the hidden main window so the Options window always appears normally.
    ShowWindow(g_main, SW_HIDE);
    g_wmTaskbar = RegisterWindowMessageW(L"TaskbarCreated");
    ChangeWindowMessageFilterEx(g_main, g_wmTaskbar, MSGFLT_ALLOW, nullptr);
    ChangeWindowMessageFilterEx(g_main, WM_DBA_OPTIONS, MSGFLT_ALLOW, nullptr);

    g_settings = LoadSettings();
    UpdateTray();
    RegisterHotKeyFromSettings();
    StartHooks();
    if (_wcsicmp(cmd.c_str(), L"/silent") != 0) ShowOptions();   // normal launch opens the window; /silent = tray only

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (opt::g_optWnd && IsWindow(opt::g_optWnd) && IsDialogMessageW(opt::g_optWnd, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    StopHooks();
    if (mtx) CloseHandle(mtx);
    return 0;
}
