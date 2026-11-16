#include "common.h"
#include <shlwapi.h>
#include <algorithm>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

static const wchar_t* ROOT = L"Software\\DialogBoxAssistant";

// ---------------------------------------------------------------- registry helpers
static HKEY OpenKey(const std::wstring& sub, bool write) {
    HKEY k = nullptr;
    std::wstring p = ROOT;
    if (!sub.empty()) p += L"\\" + sub;
    if (write) {
        if (RegCreateKeyExW(HKEY_CURRENT_USER, p.c_str(), 0, nullptr, 0, KEY_READ | KEY_WRITE, nullptr, &k, nullptr) != ERROR_SUCCESS)
            return nullptr;
    } else {
        if (RegOpenKeyExW(HKEY_CURRENT_USER, p.c_str(), 0, KEY_READ, &k) != ERROR_SUCCESS)
            return nullptr;
    }
    return k;
}

static DWORD GetDw(HKEY k, const wchar_t* name, DWORD def) {
    if (!k) return def;
    DWORD v = 0, sz = sizeof(v), t = 0;
    if (RegQueryValueExW(k, name, nullptr, &t, (BYTE*)&v, &sz) == ERROR_SUCCESS && t == REG_DWORD) return v;
    return def;
}
static void SetDw(HKEY k, const wchar_t* name, DWORD v) {
    if (k) RegSetValueExW(k, name, 0, REG_DWORD, (const BYTE*)&v, sizeof(v));
}

static std::vector<Item> GetList(HKEY k, const wchar_t* name) {
    std::vector<Item> out;
    if (!k) return out;
    DWORD sz = 0, t = 0;
    if (RegQueryValueExW(k, name, nullptr, &t, nullptr, &sz) != ERROR_SUCCESS || t != REG_MULTI_SZ || sz < 4) return out;
    std::vector<wchar_t> buf(sz / 2 + 2, 0);
    if (RegQueryValueExW(k, name, nullptr, &t, (BYTE*)buf.data(), &sz) != ERROR_SUCCESS) return out;
    for (const wchar_t* p = buf.data(); *p; p += wcslen(p) + 1) {
        if (wcslen(p) < 3 || p[1] != L'|') continue;
        Item it; it.locked = (p[0] == L'L'); it.path = p + 2;
        out.push_back(it);
    }
    return out;
}
static void SetList(HKEY k, const wchar_t* name, const std::vector<Item>& v) {
    if (!k) return;
    std::wstring s;
    for (auto& it : v) { s += it.locked ? L"L|" : L"U|"; s += it.path; s.push_back(L'\0'); }
    s.push_back(L'\0');
    RegSetValueExW(k, name, 0, REG_MULTI_SZ, (const BYTE*)s.c_str(), (DWORD)((s.size() + 1) * sizeof(wchar_t)));
}

// ---------------------------------------------------------------- settings
Settings LoadSettings() {
    Settings s;
    HKEY k = OpenKey(L"", false);
    s.rememberSize = GetDw(k, L"RememberSize", 1) != 0;
    s.rememberView = GetDw(k, L"RememberView", 1) != 0;
    s.rememberFolder = GetDw(k, L"RememberFolder", 1) != 0;
    s.showOptions = GetDw(k, L"ShowOptions", 1) != 0;
    s.showTray = GetDw(k, L"ShowTray", 1) != 0;
    s.showRecent = GetDw(k, L"ShowRecent", 1) != 0;
    s.showLocked = GetDw(k, L"ShowLocked", 1) != 0;
    s.showCommon = GetDw(k, L"ShowCommon", 1) != 0;
    s.hkMod = GetDw(k, L"HotKeyMod", MOD_CONTROL | MOD_ALT);
    s.hkVk = GetDw(k, L"HotKeyVk", 'D');
    if (k) {
        DWORD sz = 0, t = 0;
        if (RegQueryValueExW(k, L"Excluded", nullptr, &t, nullptr, &sz) == ERROR_SUCCESS && t == REG_MULTI_SZ && sz >= 4) {
            std::vector<wchar_t> b(sz / 2 + 2, 0);
            if (RegQueryValueExW(k, L"Excluded", nullptr, &t, (BYTE*)b.data(), &sz) == ERROR_SUCCESS) {
                for (const wchar_t* p = b.data(); *p; p += wcslen(p) + 1) s.excluded.push_back(p);
            }
        }
        RegCloseKey(k);
    }
    return s;
}
bool IsExcluded(const std::wstring& exe) {
    Settings s = LoadSettings();
    for (auto& e : s.excluded) if (_wcsicmp(e.c_str(), exe.c_str()) == 0) return true;
    return false;
}
void SaveSettings(const Settings& s) {
    HKEY k = OpenKey(L"", true);
    SetDw(k, L"RememberSize", s.rememberSize);
    SetDw(k, L"RememberView", s.rememberView);
    SetDw(k, L"RememberFolder", s.rememberFolder);
    SetDw(k, L"ShowOptions", s.showOptions);
    SetDw(k, L"ShowTray", s.showTray);
    SetDw(k, L"ShowRecent", s.showRecent);
    SetDw(k, L"ShowLocked", s.showLocked);
    SetDw(k, L"ShowCommon", s.showCommon);
    SetDw(k, L"HotKeyMod", s.hkMod);
    SetDw(k, L"HotKeyVk", s.hkVk);
    if (k) {
        std::wstring m;
        for (auto& e : s.excluded) if (!e.empty()) { m += e; m += L'\0'; }
        m += L'\0';
        if (m.size() < 2) m = std::wstring(2, L'\0');
        RegSetValueExW(k, L"Excluded", 0, REG_MULTI_SZ, (const BYTE*)m.c_str(), (DWORD)((m.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(k);
    }
}

// ---------------------------------------------------------------- per-application data
static std::wstring AppKey(const std::wstring& app) { return L"Apps\\" + app; }

AppData LoadApp(const std::wstring& app) {
    AppData d;
    HKEY k = OpenKey(AppKey(app), false);
    d.docs = GetList(k, L"Docs");
    d.folders = GetList(k, L"Folders");
    d.maxDocs = (int)std::min<DWORD>(50, std::max<DWORD>(1, GetDw(k, L"MaxDocs", 10)));
    d.maxFolders = (int)std::min<DWORD>(50, std::max<DWORD>(1, GetDw(k, L"MaxFolders", 10)));
    if (k) RegCloseKey(k);
    return d;
}
void SaveApp(const std::wstring& app, const AppData& d) {
    HKEY k = OpenKey(AppKey(app), true);
    SetList(k, L"Docs", d.docs);
    SetList(k, L"Folders", d.folders);
    SetDw(k, L"MaxDocs", d.maxDocs);
    SetDw(k, L"MaxFolders", d.maxFolders);
    if (k) RegCloseKey(k);
}
void DeleteApp(const std::wstring& app) {
    std::wstring p = std::wstring(ROOT) + L"\\" + AppKey(app);
    RegDeleteTreeW(HKEY_CURRENT_USER, p.c_str());
}
std::vector<std::wstring> ListApps() {
    std::vector<std::wstring> out;
    HKEY k = OpenKey(L"Apps", false);
    if (!k) return out;
    wchar_t name[512];
    for (DWORD i = 0;; i++) {
        DWORD n = 512;
        if (RegEnumKeyExW(k, i, name, &n, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        if (_wcsicmp(name, COMMON_ITEMS_NAME) != 0) out.push_back(name);
    }
    RegCloseKey(k);
    std::sort(out.begin(), out.end(), [](const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; });
    return out;
}
DWORD GetAppInt(const std::wstring& app, const wchar_t* name, DWORD def) {
    HKEY k = OpenKey(AppKey(app), false);
    DWORD v = GetDw(k, name, def);
    if (k) RegCloseKey(k);
    return v;
}
void SetAppInt(const std::wstring& app, const wchar_t* name, DWORD v) {
    HKEY k = OpenKey(AppKey(app), true);
    SetDw(k, name, v);
    if (k) RegCloseKey(k);
}

std::wstring GetAppStr(const std::wstring& app, const wchar_t* name) {
    std::wstring out;
    HKEY k = OpenKey(AppKey(app), false);
    if (k) {
        DWORD sz = 0, t = 0;
        if (RegQueryValueExW(k, name, nullptr, &t, nullptr, &sz) == ERROR_SUCCESS && t == REG_SZ && sz >= 2) {
            std::vector<wchar_t> b(sz / 2 + 2, 0);
            if (RegQueryValueExW(k, name, nullptr, &t, (BYTE*)b.data(), &sz) == ERROR_SUCCESS) out = b.data();
        }
        RegCloseKey(k);
    }
    return out;
}
void SetAppStr(const std::wstring& app, const wchar_t* name, const std::wstring& v) {
    HKEY k = OpenKey(AppKey(app), true);
    if (k) {
        RegSetValueExW(k, name, 0, REG_SZ, (const BYTE*)v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(k);
    }
}

std::wstring NormalizePath(const std::wstring& in) {
    std::wstring p = in;
    while (p.size() > 3 && (p.back() == L'\\' || p.back() == L'/')) p.pop_back();
    return p;
}
bool PathEq(const std::wstring& a, const std::wstring& b) {
    return _wcsicmp(NormalizePath(a).c_str(), NormalizePath(b).c_str()) == 0;
}

static void Trim(std::vector<Item>& v, int maxUnlocked) {
    int cnt = 0;
    for (size_t i = 0; i < v.size();) {
        if (!v[i].locked && ++cnt > maxUnlocked) v.erase(v.begin() + i); else i++;
    }
}

void AddRecent(const std::wstring& app, bool folder, const std::wstring& pathIn) {
    std::wstring path = NormalizePath(pathIn);
    if (path.empty() || app.empty()) return;
    AppData d = LoadApp(app);
    auto& v = folder ? d.folders : d.docs;
    for (size_t i = 0; i < v.size(); i++) {
        if (PathEq(v[i].path, path)) {
            if (v[i].locked) return;           // locked items stay where they are
            v.erase(v.begin() + i);
            break;
        }
    }
    Item it; it.path = path; it.locked = false;
    v.insert(v.begin(), it);
    Trim(v, folder ? d.maxFolders : d.maxDocs);
    SaveApp(app, d);
}

void CommonAdd(const std::wstring& folder) {
    std::wstring p = NormalizePath(folder);
    if (p.empty()) return;
    AppData d = LoadApp(COMMON_ITEMS_NAME);
    for (auto& it : d.folders) if (PathEq(it.path, p)) return;
    Item it; it.path = p;
    d.folders.push_back(it);
    SaveApp(COMMON_ITEMS_NAME, d);
}
void CommonRemove(const std::wstring& folder) {
    AppData d = LoadApp(COMMON_ITEMS_NAME);
    for (size_t i = 0; i < d.folders.size(); i++)
        if (PathEq(d.folders[i].path, folder)) { d.folders.erase(d.folders.begin() + i); break; }
    SaveApp(COMMON_ITEMS_NAME, d);
}
void CommonClear() {
    AppData d = LoadApp(COMMON_ITEMS_NAME);
    d.folders.clear();
    SaveApp(COMMON_ITEMS_NAME, d);
}

std::wstring CurrentExeName() {
    wchar_t buf[MAX_PATH * 2] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH * 2);
    const wchar_t* n = PathFindFileNameW(buf);
    return n;
}

// ---------------------------------------------------------------- dark mode
#pragma comment(lib, "advapi32.lib")
bool IsDarkMode() {
    DWORD v = 1, sz = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &sz) != ERROR_SUCCESS) return false;
    return v == 0;
}

// uxtheme ordinals (undocumented, Windows 10 1903+): 135 SetPreferredAppMode, 136 FlushMenuThemes
typedef int(WINAPI* PFN_SetPreferredAppMode)(int);
typedef void(WINAPI* PFN_FlushMenuThemes)();
static PFN_SetPreferredAppMode g_setMode;
static PFN_FlushMenuThemes g_flush;
static void InitMenuTheme() {
    static bool done = false;
    if (done) return;
    done = true;
    HMODULE ux = LoadLibraryW(L"uxtheme.dll");
    if (!ux) return;
    g_setMode = (PFN_SetPreferredAppMode)GetProcAddress(ux, MAKEINTRESOURCEA(135));
    g_flush = (PFN_FlushMenuThemes)GetProcAddress(ux, MAKEINTRESOURCEA(136));
}
MenuThemeScope::MenuThemeScope() {
    InitMenuTheme();
    if (!g_setMode) return;
    prev = g_setMode(IsDarkMode() ? 2 /*ForceDark*/ : 3 /*ForceLight*/);
    if (g_flush) g_flush();
}
MenuThemeScope::~MenuThemeScope() {
    if (prev < 0 || !g_setMode) return;
    g_setMode(prev);
    if (g_flush) g_flush();
}

// ---------------------------------------------------------------- menus
static const wchar_t* kLockedHdr = L"Locked";
static const wchar_t* kCommonHdr = L"Common";

static std::wstring Display(const std::wstring& p) {
    if (p.size() <= 70) return p;
    wchar_t out[80];
    if (PathCompactPathExW(out, p.c_str(), 70, 0)) return out;
    return p;
}

static void AddItems(HMENU m, const std::vector<Item>& v, bool wantLocked, std::vector<std::wstring>& payload, bool all = false) {
    for (auto& it : v) {
        if (!all && it.locked != wantLocked) continue;
        UINT id = MENU_ID_FIRST + (UINT)payload.size();
        payload.push_back(it.path);
        std::wstring label = Display(it.path);
        // escape '&' so it is not treated as an accelerator
        std::wstring esc;
        for (wchar_t c : label) { if (c == L'&') esc += L'&'; esc += c; }
        AppendMenuW(m, MF_STRING, id, esc.c_str());
    }
}
static void AddHeader(HMENU m, const wchar_t* text, bool sep) {
    if (sep) AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_OWNERDRAW | MF_DISABLED, 0, (LPCWSTR)text);
}

HMENU BuildRecentMenu(const std::wstring& app, bool folders, const Settings& s,
                      std::vector<std::wstring>& payload, bool withOptions) {
    HMENU m = CreatePopupMenu();
    AppData d = LoadApp(app);
    AppData c = LoadApp(COMMON_ITEMS_NAME);
    const auto& mine = folders ? d.folders : d.docs;
    const auto& common = folders ? c.folders : c.docs;
    bool any = false;
    size_t before = payload.size();
    AddItems(m, mine, false, payload);
    any = payload.size() > before;
    if (s.showLocked) {
        size_t b = payload.size();
        bool hasLocked = false;
        for (auto& it : mine) if (it.locked) hasLocked = true;
        if (hasLocked) { AddHeader(m, kLockedHdr, any); AddItems(m, mine, true, payload); any = true; (void)b; }
    } else {
        // locked items hidden from the menu
    }
    if (s.showCommon && !common.empty()) {
        AddHeader(m, kCommonHdr, any);
        AddItems(m, common, false, payload, true);
        any = true;
    }
    if (!any) AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"< Empty >");
    if (withOptions && s.showOptions) {
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING, MENU_ID_OPTIONS, L"Options");
    }
    return m;
}

HMENU BuildCommonFoldersMenu(std::vector<std::wstring>& payload) {
    HMENU m = CreatePopupMenu();
    AppData c = LoadApp(COMMON_ITEMS_NAME);
    AddItems(m, c.folders, false, payload, true);
    if (c.folders.empty()) AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"< Empty >");
    return m;
}

static HFONT MenuFont(UINT dpi, bool bold) {
    NONCLIENTMETRICSW ncm = { sizeof(ncm) };
    typedef BOOL(WINAPI * PFN)(UINT, UINT, PVOID, UINT, UINT);
    static PFN fn = (PFN)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SystemParametersInfoForDpi");
    if (!(fn && fn(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, dpi)))
        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    if (bold) ncm.lfMenuFont.lfWeight = FW_BOLD;
    return CreateFontIndirectW(&ncm.lfMenuFont);
}

bool MenuMeasure(HWND owner, MEASUREITEMSTRUCT* mi) {
    if (mi->CtlType != ODT_MENU || !mi->itemData) return false;
    typedef UINT(WINAPI * PFNDPI)(HWND);
    static PFNDPI gd = (PFNDPI)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
    UINT dpi = gd ? gd(owner) : 96;
    if (!dpi) dpi = 96;
    HDC dc = GetDC(owner);
    HFONT f = MenuFont(dpi, true);
    HGDIOBJ old = SelectObject(dc, f);
    const wchar_t* t = (const wchar_t*)mi->itemData;
    SIZE sz; GetTextExtentPoint32W(dc, t, (int)wcslen(t), &sz);
    TEXTMETRICW tm; GetTextMetricsW(dc, &tm);
    SelectObject(dc, old); DeleteObject(f); ReleaseDC(owner, dc);
    mi->itemWidth = sz.cx + MulDiv(16, dpi, 96);
    mi->itemHeight = tm.tmHeight + MulDiv(6, dpi, 96);
    return true;
}

bool MenuDraw(DRAWITEMSTRUCT* di) {
    if (di->CtlType != ODT_MENU || !di->itemData) return false;
    typedef UINT(WINAPI * PFNDPI)(HWND);
    static PFNDPI gd = (PFNDPI)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
    HWND w = WindowFromDC(di->hDC);
    UINT dpi = (gd && w) ? gd(w) : 96;
    if (!dpi) dpi = 96;
    HFONT f = MenuFont(dpi, true);
    HGDIOBJ old = SelectObject(di->hDC, f);
    bool dark = IsDarkMode();
    if (dark) {
        HBRUSH b = CreateSolidBrush(DK_CTRL);
        FillRect(di->hDC, &di->rcItem, b);
        DeleteObject(b);
    } else FillRect(di->hDC, &di->rcItem, GetSysColorBrush(COLOR_MENU));
    SetBkMode(di->hDC, TRANSPARENT);
    SetTextColor(di->hDC, dark ? DK_TEXT : GetSysColor(COLOR_MENUTEXT));
    RECT r = di->rcItem; r.left += MulDiv(8, dpi, 96);
    const wchar_t* t = (const wchar_t*)di->itemData;
    DrawTextW(di->hDC, t, -1, &r, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX);
    HPEN pen = CreatePen(PS_SOLID, 1, dark ? DK_EDGE : GetSysColor(COLOR_BTNSHADOW));
    HGDIOBJ op = SelectObject(di->hDC, pen);
    MoveToEx(di->hDC, di->rcItem.left + 2, di->rcItem.bottom - 1, nullptr);
    LineTo(di->hDC, di->rcItem.right - 2, di->rcItem.bottom - 1);
    SelectObject(di->hDC, op); DeleteObject(pen);
    SelectObject(di->hDC, old); DeleteObject(f);
    return true;
}
