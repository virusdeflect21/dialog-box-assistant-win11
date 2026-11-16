// Dialog Box Assistant - Windows 11 port. Shared code (settings store, recent lists, menus).
#pragma once
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <string>
#include <vector>

#define DBA_VERSION_W L"1.6.0"
#define DBA_MAIN_CLASS L"DialogBoxAssistantMainWnd"
#define COMMON_ITEMS_NAME L"COMMON ITEMS"

#define WM_DBA_TRAY      (WM_APP + 1)
#define WM_DBA_OPTIONS   (WM_APP + 2)
#define WM_DBA_RELOAD    (WM_APP + 3)

struct Item { std::wstring path; bool locked = false; };

struct AppData {
    std::vector<Item> docs, folders;
    int maxDocs = 10, maxFolders = 10;
};

struct Settings {
    bool rememberSize = true, rememberView = true, rememberFolder = true;
    bool showOptions = true, showTray = true, showRecent = true;
    bool showLocked = true, showCommon = true;
    UINT hkMod = MOD_CONTROL | MOD_ALT;   // modifiers as used by RegisterHotKey
    UINT hkVk = 'D';
    std::vector<std::wstring> excluded;   // program file names (e.g. notepad.exe) the assistant must leave alone
};
bool IsExcluded(const std::wstring& exeName);

Settings LoadSettings();
void SaveSettings(const Settings&);

AppData LoadApp(const std::wstring& app);
void SaveApp(const std::wstring& app, const AppData&);
void DeleteApp(const std::wstring& app);
std::vector<std::wstring> ListApps();       // does not include COMMON ITEMS
void AddRecent(const std::wstring& app, bool folder, const std::wstring& path);
DWORD GetAppInt(const std::wstring& app, const wchar_t* name, DWORD def);
void SetAppInt(const std::wstring& app, const wchar_t* name, DWORD v);
std::wstring GetAppStr(const std::wstring& app, const wchar_t* name);
void SetAppStr(const std::wstring& app, const wchar_t* name, const std::wstring& v);

// Common folders edited from Explorer
void CommonAdd(const std::wstring& folder);
void CommonRemove(const std::wstring& folder);
void CommonClear();

std::wstring CurrentExeName();               // base file name of the host process
std::wstring NormalizePath(const std::wstring&);
bool PathEq(const std::wstring&, const std::wstring&);

// Dark mode
bool IsDarkMode();                           // "Choose your default app mode" = dark
struct MenuThemeScope {                      // makes popup menus created in this scope follow the app mode
    int prev = -1;
    MenuThemeScope();
    ~MenuThemeScope();
};
#define DK_BG      RGB(32, 32, 32)
#define DK_CTRL    RGB(45, 45, 45)
#define DK_TEXT    RGB(240, 240, 240)
#define DK_EDGE    RGB(90, 90, 90)
#define DK_SEL     RGB(0, 90, 158)

// Menus (shared by file dialog buttons, hotkey menu and tray menu)
#define MENU_ID_OPTIONS 999
#define MENU_ID_FIRST   1000
// Builds the Recent Documents / Recent Folders menu. payload[id - MENU_ID_FIRST] is the path.
HMENU BuildRecentMenu(const std::wstring& app, bool folders, const Settings& s,
                      std::vector<std::wstring>& payload, bool withOptions);
HMENU BuildCommonFoldersMenu(std::vector<std::wstring>& payload);
bool MenuMeasure(HWND owner, MEASUREITEMSTRUCT* mi);
bool MenuDraw(DRAWITEMSTRUCT* di);
