// In-process hook: decorates standard Open / Save dialogs with "Recent documents" and
// "Recent folders" buttons, remembers dialog size / view style / last folder, records used files.
#include "common.h"
#include <algorithm>
#include <commctrl.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <uxtheme.h>
#include <vssym32.h>
#include <shellapi.h>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "shlwapi.lib")

#ifndef _WIN64
// x86 stdcall decoration: export the undecorated name
#pragma comment(linker, "/EXPORT:DBAHookProc=_DBAHookProc@12")
#endif

#define ID_BTN_DOC 0x0DBA
#define ID_BTN_FOL 0x0DBB
#define TIMER_ID   ((UINT_PTR)0x0DBA5EED)
#define SUBCLASS_ID ((UINT_PTR)0x0DBA0001)
#define WM_GETISHELLBROWSER_ (WM_USER + 7)

static HMODULE g_mod;
static std::wstring g_self;

struct Dlg {
    HWND hwnd = nullptr, btnDoc = nullptr, btnFol = nullptr, workerW = nullptr, rebar = nullptr;
    bool ready = false, applied = false, saved = false, timerOn = false;
    int tries = 0, tries2 = 0;
    std::wstring app;
    Settings s;
    HICON icoDoc = nullptr, icoFol = nullptr;
    int hotId = 0;
    int origRight = 0, lastSetRight = -1;
    bool pending = false;
    std::vector<std::wstring> pendingPaths;
};

// ---------------------------------------------------------------- helpers
static HWND FindDescendant(HWND parent, const wchar_t* cls) {
    struct Ctx { const wchar_t* cls; HWND found; } c = { cls, nullptr };
    EnumChildWindows(parent, [](HWND h, LPARAM l) -> BOOL {
        Ctx* c = (Ctx*)l;
        wchar_t buf[128];
        if (GetClassNameW(h, buf, 128) && _wcsicmp(buf, c->cls) == 0) { c->found = h; return FALSE; }
        return TRUE;
    }, (LPARAM)&c);
    return c.found;
}

static IShellBrowser* GetShellBrowser(HWND dlg) {
    DWORD_PTR r = 0;
    if (!SendMessageTimeoutW(dlg, WM_GETISHELLBROWSER_, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 500, &r)) return nullptr;
    return (IShellBrowser*)r;
}

static std::wstring CurrentFolder(HWND dlg) {
    std::wstring out;
    IShellBrowser* sb = GetShellBrowser(dlg);
    if (!sb) return out;
    IShellView* sv = nullptr;
    if (SUCCEEDED(sb->QueryActiveShellView(&sv)) && sv) {
        IFolderView* fv = nullptr;
        if (SUCCEEDED(sv->QueryInterface(IID_PPV_ARGS(&fv))) && fv) {
            IPersistFolder2* pf = nullptr;
            if (SUCCEEDED(fv->GetFolder(IID_PPV_ARGS(&pf))) && pf) {
                PIDLIST_ABSOLUTE pidl = nullptr;
                if (SUCCEEDED(pf->GetCurFolder(&pidl)) && pidl) {
                    wchar_t path[MAX_PATH * 2];
                    if (SHGetPathFromIDListEx(pidl, path, MAX_PATH * 2, GPFIDL_DEFAULT)) out = path;
                    CoTaskMemFree(pidl);
                }
                pf->Release();
            }
            fv->Release();
        }
        sv->Release();
    }
    return out;
}

static bool NavigateTo(HWND dlg, const std::wstring& path) {
    IShellBrowser* sb = GetShellBrowser(dlg);
    if (!sb) return false;
    PIDLIST_ABSOLUTE pidl = nullptr;
    if (FAILED(SHParseDisplayName(path.c_str(), nullptr, &pidl, 0, nullptr)) || !pidl) return false;
    HRESULT hr = sb->BrowseObject(pidl, SBSP_ABSOLUTE | SBSP_SAMEBROWSER);
    CoTaskMemFree(pidl);
    return SUCCEEDED(hr);
}

static HWND FileNameEdit(HWND dlg) {
    HWND c = GetDlgItem(dlg, 1148);
    if (!c) return nullptr;
    wchar_t cls[64] = {};
    GetClassNameW(c, cls, 64);
    if (_wcsicmp(cls, L"ComboBoxEx32") == 0) {
        HWND e = (HWND)SendMessageW(c, CBEM_GETEDITCONTROL, 0, 0);
        if (e) return e;
    }
    HWND e = FindWindowExW(c, nullptr, L"Edit", nullptr);
    return e ? e : c;
}

static UINT DpiOf(HWND h) { UINT d = GetDpiForWindow(h); return d ? d : 96; }

// Resolves what the user typed in "File name" to full paths.
static void CollectPending(Dlg* d) {
    d->pending = true;
    d->pendingPaths.clear();
    HWND ed = FileNameEdit(d->hwnd);
    if (!ed) return;
    int len = GetWindowTextLengthW(ed);
    if (len <= 0 || len > 32000) return;
    std::wstring text(len + 1, L'\0');
    GetWindowTextW(ed, &text[0], len + 1);
    text.resize(wcslen(text.c_str()));
    std::vector<std::wstring> toks;
    if (text.find(L'"') != std::wstring::npos) {
        size_t i = 0;
        while (i < text.size()) {
            size_t a = text.find(L'"', i);
            if (a == std::wstring::npos) break;
            size_t b = text.find(L'"', a + 1);
            if (b == std::wstring::npos) break;
            toks.push_back(text.substr(a + 1, b - a - 1));
            i = b + 1;
        }
    } else {
        toks.push_back(text);
    }
    std::wstring folder = CurrentFolder(d->hwnd);
    for (auto t : toks) {
        while (!t.empty() && t.back() == L' ') t.pop_back();
        if (t.empty()) continue;
        wchar_t exp[MAX_PATH * 2];
        if (ExpandEnvironmentStringsW(t.c_str(), exp, MAX_PATH * 2)) t = exp;
        std::wstring full;
        if (PathIsRelativeW(t.c_str())) {
            if (folder.empty()) continue;
            full = folder + L"\\" + t;
        } else full = t;
        wchar_t canon[MAX_PATH * 2];
        if (!PathCanonicalizeW(canon, full.c_str())) continue;
        full = canon;
        // only real file system paths (drive or UNC)
        bool ok = (full.size() > 2 && full[1] == L':') || (full.size() > 2 && full[0] == L'\\' && full[1] == L'\\');
        if (ok) d->pendingPaths.push_back(full);
    }
}

static void CommitPending(Dlg* d) {
    if (!d->pending) return;
    d->pending = false;
    std::wstring lastFolder;
    for (auto& p : d->pendingPaths) {
        DWORD attr = GetFileAttributesW(p.c_str());
        bool exists = attr != INVALID_FILE_ATTRIBUTES;
        if (exists && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
            AddRecent(d->app, true, p);
            lastFolder = p;
            continue;
        }
        bool hasExt = PathFindExtensionW(p.c_str())[0] != 0;
        std::wstring parent = p;
        PathRemoveFileSpecW(&parent[0]);
        parent.resize(wcslen(parent.c_str()));
        if (exists || hasExt) AddRecent(d->app, false, p);
        if (!parent.empty() && PathIsDirectoryW(parent.c_str())) {
            AddRecent(d->app, true, parent);
            lastFolder = parent;
        }
    }
    if (!lastFolder.empty()) SetAppStr(d->app, L"LastFolder", lastFolder);
    d->pendingPaths.clear();
}

static void SaveState(Dlg* d) {
    if (d->saved || !d->ready) return;
    d->saved = true;
    if (d->s.rememberSize && !IsIconic(d->hwnd) && !IsZoomed(d->hwnd) && (GetWindowLongW(d->hwnd, GWL_STYLE) & WS_THICKFRAME)) {
        RECT r; GetWindowRect(d->hwnd, &r);
        UINT dpi = DpiOf(d->hwnd);
        SetAppInt(d->app, L"DlgW", MulDiv(r.right - r.left, 96, dpi));
        SetAppInt(d->app, L"DlgH", MulDiv(r.bottom - r.top, 96, dpi));
    }
    if (d->s.rememberView) {
        IShellBrowser* sb = GetShellBrowser(d->hwnd);
        IShellView* sv = nullptr;
        if (sb && SUCCEEDED(sb->QueryActiveShellView(&sv)) && sv) {
            IFolderView2* fv = nullptr;
            if (SUCCEEDED(sv->QueryInterface(IID_PPV_ARGS(&fv))) && fv) {
                FOLDERVIEWMODE mode; int icon = 0;
                if (SUCCEEDED(fv->GetViewModeAndIconSize(&mode, &icon))) {
                    SetAppInt(d->app, L"ViewMode", (DWORD)mode);
                    SetAppInt(d->app, L"IconSize", (DWORD)icon);
                }
                fv->Release();
            }
            sv->Release();
        }
    }
}

// ---------------------------------------------------------------- layout and buttons
static void Layout(Dlg* d) {
    if (!d->ready || !d->workerW) return;
    UINT dpi = DpiOf(d->hwnd);
    int bw = MulDiv(30, dpi, 96), bh = MulDiv(26, dpi, 96), gap = MulDiv(4, dpi, 96);
    int nb = d->s.showRecent ? 2 : 1;
    RECT wr; GetWindowRect(d->workerW, &wr);
    MapWindowPoints(nullptr, d->hwnd, (POINT*)&wr, 2);
    int right = wr.right;
    if (right != d->lastSetRight) d->origRight = right;   // dialog laid the bar out again
    int total = nb * (bw + gap);
    int newRight = d->origRight - total;
    int newW = newRight - wr.left;
    if (newW > 50) {
        SetWindowPos(d->workerW, nullptr, 0, 0, newW, wr.bottom - wr.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        if (d->rebar) {
            RECT rr; GetClientRect(d->workerW, &rr);
            SetWindowPos(d->rebar, nullptr, 0, 0, rr.right, rr.bottom, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
        d->lastSetRight = newRight;
    }
    int y = wr.top + ((wr.bottom - wr.top) - bh) / 2;
    int x = newRight + gap;
    UINT fl = SWP_NOZORDER | SWP_NOACTIVATE;
    if (d->s.showRecent) {
        SetWindowPos(d->btnDoc, nullptr, x, y, bw, bh, fl | SWP_SHOWWINDOW);
        x += bw + gap;
    } else if (d->btnDoc) ShowWindow(d->btnDoc, SW_HIDE);
    SetWindowPos(d->btnFol, nullptr, x, y, bw, bh, fl | SWP_SHOWWINDOW);
}

static COLORREF Shade(COLORREF c, int delta) {
    auto cl = [](int v) { return std::max(0, std::min(255, v)); };
    return RGB(cl(GetRValue(c) + delta), cl(GetGValue(c) + delta), cl(GetBValue(c) + delta));
}

static void DrawButton(Dlg* d, DRAWITEMSTRUCT* di) {
    bool folder = di->CtlID == ID_BTN_FOL;
    HDC dc = di->hDC;
    RECT r = di->rcItem;
    bool pressed = (di->itemState & ODS_SELECTED) != 0;
    bool hot = (d->hotId == (int)di->CtlID);
    // background color of the dialog's toolbar row (works for light and dark dialogs)
    COLORREF bg = GetSysColor(COLOR_BTNFACE);
    if (d->rebar) {
        HDC rdc = GetDC(d->rebar);
        if (rdc) {
            COLORREF px = GetPixel(rdc, 1, 1);
            if (px != CLR_INVALID) bg = px;
            ReleaseDC(d->rebar, rdc);
        }
    }
    int lum = (GetRValue(bg) * 299 + GetGValue(bg) * 587 + GetBValue(bg) * 114) / 1000;
    bool dark = lum < 128;
    HBRUSH bb = CreateSolidBrush(bg);
    FillRect(dc, &r, bb);
    DeleteObject(bb);
    if (hot || pressed || (di->itemState & ODS_FOCUS)) {
        COLORREF fill = dark ? Shade(bg, pressed ? 14 : 28) : Shade(bg, pressed ? -30 : -16);
        COLORREF edge = dark ? Shade(bg, 60) : Shade(bg, -60);
        HBRUSH fb = CreateSolidBrush(fill);
        HPEN pen = CreatePen(PS_SOLID, 1, edge);
        HGDIOBJ ob = SelectObject(dc, fb), op = SelectObject(dc, pen);
        int rad = MulDiv(6, DpiOf(d->hwnd), 96);
        RoundRect(dc, r.left, r.top, r.right, r.bottom, rad, rad);
        SelectObject(dc, ob); SelectObject(dc, op);
        DeleteObject(fb); DeleteObject(pen);
    }
    HICON ico = folder ? d->icoFol : d->icoDoc;
    UINT dpi = DpiOf(d->hwnd);
    int isz = GetSystemMetricsForDpi(SM_CXSMICON, dpi);
    int aw = MulDiv(7, dpi, 96), ah = MulDiv(4, dpi, 96), sp = MulDiv(3, dpi, 96);
    int cw = isz + sp + aw;
    int x = r.left + ((r.right - r.left) - cw) / 2 + (pressed ? 1 : 0);
    int y = r.top + ((r.bottom - r.top) - isz) / 2 + (pressed ? 1 : 0);
    if (ico) DrawIconEx(dc, x, y, ico, isz, isz, 0, nullptr, DI_NORMAL);
    int ax = x + isz + sp, ay = r.top + ((r.bottom - r.top) - ah) / 2 + (pressed ? 1 : 0);
    POINT pts[3] = { { ax, ay }, { ax + aw, ay }, { ax + aw / 2, ay + ah } };
    COLORREF ac = dark ? RGB(230, 230, 230) : RGB(40, 40, 40);
    HBRUSH br = CreateSolidBrush(ac);
    HPEN pen = CreatePen(PS_SOLID, 1, ac);
    HGDIOBJ ob = SelectObject(dc, br), op = SelectObject(dc, pen);
    Polygon(dc, pts, 3);
    SelectObject(dc, ob); SelectObject(dc, op);
    DeleteObject(br); DeleteObject(pen);
}

static LRESULT CALLBACK BtnProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR ref) {
    Dlg* d = (Dlg*)ref;
    int cid = GetDlgCtrlID(h);
    switch (m) {
    case WM_MOUSEMOVE:
        if (d->hotId != cid) {
            d->hotId = cid;
            TRACKMOUSEEVENT t = { sizeof(t), TME_LEAVE, h, 0 };
            TrackMouseEvent(&t);
            InvalidateRect(h, nullptr, FALSE);
        }
        break;
    case WM_MOUSELEAVE:
        if (d->hotId == cid) { d->hotId = 0; InvalidateRect(h, nullptr, FALSE); }
        break;
    case WM_ERASEBKGND:
        return 1;
    case WM_NCDESTROY:
        RemoveWindowSubclass(h, BtnProc, id);
        break;
    }
    return DefSubclassProc(h, m, w, l);
}

static bool Detect(Dlg* d) {
    HWND h = d->hwnd;
    if (!GetDlgItem(h, IDOK) || !GetDlgItem(h, IDCANCEL) || !GetDlgItem(h, 1148)) return false;
    if (!FindDescendant(h, L"SHELLDLL_DefView")) return false;
    HWND w = nullptr;
    for (HWND c = FindWindowExW(h, nullptr, L"WorkerW", nullptr); c; c = FindWindowExW(h, c, L"WorkerW", nullptr)) {
        if (IsWindowVisible(c) && FindWindowExW(c, nullptr, L"ReBarWindow32", nullptr)) { w = c; break; }
    }
    if (!w) return false;
    d->workerW = w;
    d->rebar = FindWindowExW(w, nullptr, L"ReBarWindow32", nullptr);
    return true;
}

static void CreateButtons(Dlg* d) {
    HINSTANCE hi = g_mod;
    DWORD st = WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW;
    d->btnDoc = CreateWindowExW(0, L"BUTTON", L"Recent documents", st, 0, 0, 10, 10, d->hwnd, (HMENU)(UINT_PTR)ID_BTN_DOC, hi, nullptr);
    d->btnFol = CreateWindowExW(0, L"BUTTON", L"Recent folders", st, 0, 0, 10, 10, d->hwnd, (HMENU)(UINT_PTR)ID_BTN_FOL, hi, nullptr);
    HFONT f = (HFONT)SendMessageW(d->hwnd, WM_GETFONT, 0, 0);
    if (f) { SendMessageW(d->btnDoc, WM_SETFONT, (WPARAM)f, 0); SendMessageW(d->btnFol, WM_SETFONT, (WPARAM)f, 0); }
    SetWindowSubclass(d->btnDoc, BtnProc, 1, (DWORD_PTR)d);
    SetWindowSubclass(d->btnFol, BtnProc, 1, (DWORD_PTR)d);
    SHSTOCKICONINFO si = { sizeof(si) };
    if (SUCCEEDED(SHGetStockIconInfo(SIID_DOCNOASSOC, SHGSI_ICON | SHGSI_SMALLICON, &si))) d->icoDoc = si.hIcon;
    si = { sizeof(si) };
    if (SUCCEEDED(SHGetStockIconInfo(SIID_FOLDER, SHGSI_ICON | SHGSI_SMALLICON, &si))) d->icoFol = si.hIcon;
}

static void ApplySize(Dlg* d) {
    if (!d->s.rememberSize) return;
    if (!(GetWindowLongW(d->hwnd, GWL_STYLE) & WS_THICKFRAME)) return;
    DWORD w = GetAppInt(d->app, L"DlgW", 0), h = GetAppInt(d->app, L"DlgH", 0);
    if (!w || !h) return;
    UINT dpi = DpiOf(d->hwnd);
    int W = MulDiv(w, dpi, 96), H = MulDiv(h, dpi, 96);
    HMONITOR mon = MonitorFromWindow(d->hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(mon, &mi);
    RECT cur; GetWindowRect(d->hwnd, &cur);
    int maxW = mi.rcWork.right - mi.rcWork.left, maxH = mi.rcWork.bottom - mi.rcWork.top;
    W = std::min<int>(std::max<int>(W, MulDiv(480, dpi, 96)), maxW);
    H = std::min<int>(std::max<int>(H, MulDiv(320, dpi, 96)), maxH);
    int x = cur.left, y = cur.top;
    if (x + W > mi.rcWork.right) x = mi.rcWork.right - W;
    if (y + H > mi.rcWork.bottom) y = mi.rcWork.bottom - H;
    if (x < mi.rcWork.left) x = mi.rcWork.left;
    if (y < mi.rcWork.top) y = mi.rcWork.top;
    SetWindowPos(d->hwnd, nullptr, x, y, W, H, SWP_NOZORDER | SWP_NOACTIVATE);
}

// returns true when finished (shell view available)
static bool ApplyFolderAndView(Dlg* d) {
    IShellBrowser* sb = GetShellBrowser(d->hwnd);
    if (!sb) return true;                         // unsupported dialog: nothing to do
    IShellView* sv = nullptr;
    if (FAILED(sb->QueryActiveShellView(&sv)) || !sv) return false;
    sv->Release();
    if (d->s.rememberFolder) {
        std::wstring last = GetAppStr(d->app, L"LastFolder");
        if (!last.empty() && PathIsDirectoryW(last.c_str())) {
            std::wstring cur = CurrentFolder(d->hwnd);
            if (!PathEq(cur, last)) NavigateTo(d->hwnd, last);
        }
    }
    if (d->s.rememberView) {
        DWORD mode = GetAppInt(d->app, L"ViewMode", 0xFFFFFFFF);
        DWORD icon = GetAppInt(d->app, L"IconSize", 0);
        if (mode != 0xFFFFFFFF) {
            IShellView* sv2 = nullptr;
            if (SUCCEEDED(sb->QueryActiveShellView(&sv2)) && sv2) {
                IFolderView2* fv = nullptr;
                if (SUCCEEDED(sv2->QueryInterface(IID_PPV_ARGS(&fv))) && fv) {
                    fv->SetViewModeAndIconSize((FOLDERVIEWMODE)mode, (int)icon);
                    fv->Release();
                }
                sv2->Release();
            }
        }
    }
    return true;
}

static void OpenOptions(HWND dlg) {
    HWND main = FindWindowW(DBA_MAIN_CLASS, nullptr);
    if (main) {
        DWORD pid = 0; GetWindowThreadProcessId(main, &pid);
        AllowSetForegroundWindow(pid);
        PostMessageW(main, WM_DBA_OPTIONS, 0, 0);
    }
    (void)dlg;
}

static void ShowRecentMenu(Dlg* d, bool folders) {
    Settings s = LoadSettings();
    std::vector<std::wstring> payload;
    HMENU m = BuildRecentMenu(d->app, folders, s, payload, true);
    HWND btn = folders ? d->btnFol : d->btnDoc;
    RECT r; GetWindowRect(btn, &r);
    SetForegroundWindow(d->hwnd);
    int cmd;
    {
        MenuThemeScope theme;   // dark popup menu when apps use dark mode
        cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTALIGN | TPM_TOPALIGN, r.right, r.bottom, 0, d->hwnd, nullptr);
    }
    DestroyMenu(m);
    if (cmd == MENU_ID_OPTIONS) { OpenOptions(d->hwnd); return; }
    if (cmd < MENU_ID_FIRST || (size_t)(cmd - MENU_ID_FIRST) >= payload.size()) return;
    const std::wstring path = payload[cmd - MENU_ID_FIRST];
    if (folders) {
        if (!PathIsDirectoryW(path.c_str())) {
            MessageBoxW(d->hwnd, (L"The folder \"" + path + L"\" is not available.").c_str(), L"Dialog Box Assistant", MB_ICONWARNING | MB_OK);
            return;
        }
        if (!NavigateTo(d->hwnd, path)) {
            HWND ed = FileNameEdit(d->hwnd);
            if (ed) {
                SetWindowTextW(ed, path.c_str());
                SendMessageW(d->hwnd, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), (LPARAM)GetDlgItem(d->hwnd, IDOK));
            }
        }
    } else {
        HWND ed = FileNameEdit(d->hwnd);
        if (ed) {
            SetWindowTextW(ed, path.c_str());
            SendMessageW(d->hwnd, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), (LPARAM)GetDlgItem(d->hwnd, IDOK));
        }
    }
}

static void Cleanup(Dlg* d) {
    if (d->timerOn) KillTimer(d->hwnd, TIMER_ID);
    if (d->icoDoc) DestroyIcon(d->icoDoc);
    if (d->icoFol) DestroyIcon(d->icoFol);
}

// returns true when the dialog is not a file dialog and the subclass should be detached
static bool OnTimer(Dlg* d) {
    if (!d->ready) {
        if (Detect(d)) {
            d->ready = true;
            d->app = CurrentExeName();
            d->s = LoadSettings();
            CreateButtons(d);
            ApplySize(d);
            Layout(d);
            SetTimer(d->hwnd, TIMER_ID, 100, nullptr);
        } else if (++d->tries > 60) {
            return true;
        }
        return false;
    }
    if (!d->applied) {
        if (ApplyFolderAndView(d) || ++d->tries2 > 30) {
            d->applied = true;
            KillTimer(d->hwnd, TIMER_ID);
            d->timerOn = false;
            Layout(d);
        }
    }
    return false;
}

static LRESULT CALLBACK SubProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR ref) {
    Dlg* d = (Dlg*)ref;
    switch (m) {
    case WM_TIMER:
        if (w == TIMER_ID) {
            if (OnTimer(d)) {
                Cleanup(d);
                RemoveWindowSubclass(h, SubProc, id);
                delete d;
            }
            return 0;
        }
        break;
    case WM_COMMAND: {
        WORD cid = LOWORD(w);
        if (d->ready) {
            if ((cid == ID_BTN_DOC || cid == ID_BTN_FOL) && HIWORD(w) == BN_CLICKED) {
                ShowRecentMenu(d, cid == ID_BTN_FOL);
                return 0;
            }
            if (cid == IDOK && HIWORD(w) == BN_CLICKED) {
                CollectPending(d);
                SaveState(d); d->saved = false;     // keep a snapshot, dialog may stay open
            } else if (cid == IDCANCEL) {
                d->pending = false; d->pendingPaths.clear();
            }
        }
        break;
    }
    case WM_DRAWITEM: {
        DRAWITEMSTRUCT* di = (DRAWITEMSTRUCT*)l;
        if (di->CtlType == ODT_BUTTON && (di->CtlID == ID_BTN_DOC || di->CtlID == ID_BTN_FOL)) {
            DrawButton(d, di);
            return TRUE;
        }
        if (MenuDraw(di)) return TRUE;
        break;
    }
    case WM_MEASUREITEM:
        if (MenuMeasure(h, (MEASUREITEMSTRUCT*)l)) return TRUE;
        break;
    case WM_SIZE:
    case WM_DPICHANGED:
    case WM_THEMECHANGED:
    case WM_SETTINGCHANGE: {
        LRESULT r = DefSubclassProc(h, m, w, l);
        if (d->ready) {
            InvalidateRect(d->btnDoc, nullptr, TRUE);
            InvalidateRect(d->btnFol, nullptr, TRUE);
            // the dialog re-lays out its own controls asynchronously on some messages; do it again after
            Layout(d);
            PostMessageW(h, WM_APP + 77, 0, 0);
        }
        return r;
    }
    case WM_APP + 77:
        if (d->ready) { Layout(d); return 0; }
        break;
    case WM_DESTROY:
        if (d->ready) {
            SaveState(d);
            CommitPending(d);
        }
        break;
    case WM_NCDESTROY: {
        Cleanup(d);
        RemoveWindowSubclass(h, SubProc, id);
        delete d;
        return DefSubclassProc(h, m, w, l);
    }
    }
    return DefSubclassProc(h, m, w, l);
}

static void TryAttach(HWND h) {
    wchar_t cls[32];
    if (!GetClassNameW(h, cls, 32) || wcscmp(cls, L"#32770") != 0) return;
    LONG st = GetWindowLongW(h, GWL_STYLE);
    if (st & WS_CHILD) return;
    DWORD_PTR dummy;
    if (GetWindowSubclass(h, SubProc, SUBCLASS_ID, &dummy)) return;
    if (IsExcluded(g_self)) return;       // Exclusion List from the options window
    // The subclass and timer callbacks live in this DLL. When DBA exits, Windows unmaps the hook DLL from
    // every process; pin it so live subclassed windows never call into unmapped code (host crash).
    HMODULE pinned = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCWSTR)&TryAttach, &pinned);
    Dlg* d = new Dlg();
    d->hwnd = h;
    if (!SetWindowSubclass(h, SubProc, SUBCLASS_ID, (DWORD_PTR)d)) { delete d; return; }
    if (SetTimer(h, TIMER_ID, 50, nullptr)) d->timerOn = true;
}

extern "C" __declspec(dllexport) LRESULT CALLBACK DBAHookProc(int code, WPARAM w, LPARAM l) {
    if (code == HC_ACTION) {
        const CWPRETSTRUCT* m = (const CWPRETSTRUCT*)l;
        if (m->message == WM_SHOWWINDOW && m->wParam && !g_self.empty()) TryAttach(m->hwnd);
    }
    return CallNextHookEx(nullptr, code, w, l);
}

// Never touch the shell, system UI hosts or services: injecting into these (and pinning the DLL there)
// can blank the screen / taskbar. Only ordinary user applications get decorated.
static bool IsProtectedProcess(const std::wstring& n) {
    static const wchar_t* deny[] = {
        L"explorer.exe", L"dwm.exe", L"winlogon.exe", L"csrss.exe", L"smss.exe", L"wininit.exe",
        L"services.exe", L"lsass.exe", L"logonui.exe", L"fontdrvhost.exe", L"svchost.exe",
        L"sihost.exe", L"shellhost.exe", L"startmenuexperiencehost.exe", L"shellexperiencehost.exe",
        L"searchhost.exe", L"searchapp.exe", L"searchui.exe", L"textinputhost.exe", L"ctfmon.exe",
        L"runtimebroker.exe", L"applicationframehost.exe", L"lockapp.exe", L"systemsettings.exe",
        L"taskhostw.exe", L"widgets.exe", L"widgetservice.exe", L"microsoftstartfeedprovider.exe",
        L"consent.exe", L"credentialuibroker.exe", L"securityhealthsystray.exe", L"useroobebroker.exe",
        L"backgroundtaskhost.exe", L"smartscreen.exe", L"wudfhost.exe", L"spoolsv.exe", L"audiodg.exe",
        L"nvcontainer.exe", L"msedgewebview2.exe", L"dllhost.exe", L"conhost.exe", L"openconsole.exe",
        L"taskmgr.exe", L"mmc.exe", L"werfault.exe", L"onedrive.sync.service.exe",
    };
    for (auto d : deny) if (_wcsicmp(n.c_str(), d) == 0) return true;
    wchar_t exe[MAX_PATH * 2], win[MAX_PATH];
    if (GetModuleFileNameW(nullptr, exe, MAX_PATH * 2) && GetWindowsDirectoryW(win, MAX_PATH)) {
        std::wstring sys = std::wstring(win) + L"\\SystemApps\\";
        if (_wcsnicmp(exe, sys.c_str(), sys.size()) == 0) return true;
        sys = std::wstring(win) + L"\\ImmersiveControlPanel\\";
        if (_wcsnicmp(exe, sys.c_str(), sys.size()) == 0) return true;
    }
    return false;
}

BOOL APIENTRY DllMain(HMODULE mod, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_mod = mod;
        DisableThreadLibraryCalls(mod);
        std::wstring n = CurrentExeName();
        if (_wcsicmp(n.c_str(), L"DBA.exe") != 0 && _wcsicmp(n.c_str(), L"DBA32.exe") != 0 && !IsProtectedProcess(n)) g_self = n;
    }
    return TRUE;
}
