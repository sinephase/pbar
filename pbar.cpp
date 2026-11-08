// pbar.cpp - live-preview vertical taskbar for Windows 10/11 (a sibling of vbar)
//
// Every open window is a live DWM preview in a column that wraps around endlessly. The focused window
// sits in the middle at full size and the others step down in size the further they are from it.
// Hovering makes the preview under the mouse the big one, popping out a little past the bar's edge;
// the mouse wheel spins the column. A mini terminal (PowerShell, pwsh or cmd) with selectable,
// copyable output sits at the top. Pins, clock, calendar, start button, menus and the ini work like vbar's.
// Right-click the clock > Settings for a window with every option and what it does.
//
// Build (MinGW-w64):
//   g++ -O2 -municode -mwindows -static pbar.cpp -o pbar.exe -ldwmapi -lgdi32 -luser32 -lshell32 -lole32 -luuid -lmsimg32 -ladvapi32 -lgdiplus -lcomdlg32
// Build (MSVC, x64 Native Tools prompt):
//   cl /O2 /EHsc /DUNICODE /D_UNICODE pbar.cpp /link /SUBSYSTEM:WINDOWS
//
// First run writes pbar.ini next to the exe. Change things in Settings, or edit the ini and right-click the
// clock > Reload. "pbar.exe --settings" opens Settings (in the running pbar, if there is one).

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <string>
#include <vector>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <cmath>
#include <map>
#include <memory>
#include <cstdint>
#include <array>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <objidl.h>
namespace Gdiplus { using std::min; using std::max; }   // GDI+'s headers expect min/max (NOMINMAX removes them)
#include <gdiplus.h>

#ifdef _MSC_VER
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "msimg32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "comdlg32.lib")
#endif

#ifndef MOD_NOREPEAT
#define MOD_NOREPEAT 0x4000
#endif

#define PBAR_VERSION L"0.9"
#define PBAR_CONFIGVER 4

#define WM_APPBAR    (WM_APP + 1)
#define WM_STARTPINS (WM_APP + 2)
#define WM_TERMDATA  (WM_APP + 3)
#define WM_TERMEXIT  (WM_APP + 4)
#define WM_OPENSETTINGS (WM_APP + 5)
#define TIMER_CLOCK 1
#define TIMER_SCAN 2
#define TIMER_HOVER 3
#define TIMER_METER 4
#define TIMER_BLINK 5
#define TIMER_CAL 6
#define TIMER_ANIM 7
#define TIMER_RECENTER 8
#define TIMER_CARET 9
#define TIMER_TERMEXIT 10
#define HOTKEY_TERM 1
#define IDM_RELOAD 100
#define IDM_EDIT 101
#define IDM_EXIT 102
#define IDM_SIDE_LEFT 110
#define IDM_SIDE_RIGHT 111
#define IDM_CLOCK24 112
#define IDM_AUTOSTART 113
#define IDM_SETTINGS 114
#define IDM_MON_PRIMARY 119
#define IDM_MON_BASE 120
#define IDM_T_SWITCH   200
#define IDM_T_RESTORE  201
#define IDM_T_MINIMIZE 202
#define IDM_T_MAXIMIZE 203
#define IDM_T_CLOSE    204
#define IDM_T_LOCATION 205
#define IDM_T_KILL     206
#define IDM_T_NEW      207
#define IDM_P_OPEN     300
#define IDM_P_ADMIN    301
#define IDM_P_LOCATION 302
#define IDM_TERM_COPY      400
#define IDM_TERM_CLEAR     401
#define IDM_TERM_NOTEPAD   402
#define IDM_TERM_INTERRUPT 403
#define IDM_TERM_RESTART   404
#define IDM_TERM_COPYSEL   405
#define IDM_TERM_SELALL    406
#define IDM_TERM_COPYDIR   407
#define IDM_TERM_SH_PS     410
#define IDM_TERM_SH_PWSH   411
#define IDM_TERM_SH_CMD    412

enum Kind { K_PIN = 0, K_TASK = 1, K_CLOCK = 2, K_START = 3, K_TERM = 4, K_NONE = -1 };

// A 2-bit icon: every pixel is transparent or one of three grey shades, stored ready to AlphaBlend.
struct Icon2 { HBITMAP bmp = nullptr; int size = 0; ~Icon2() { if (bmp) DeleteObject(bmp); } };
typedef std::shared_ptr<Icon2> IconPtr;

struct Pin  { std::wstring name, path, args, dir, aumid; };
struct Task {
    HWND hwnd = nullptr;
    std::wstring title;
    bool flash = false, iconic = false, visible = false;
    DWORD pid = 0;
    std::wstring exe;
    float level = 0;
    IconPtr icon;
    int iconTries = 0;
    bool iconFinal = false;
    DWORD iconNext = 0;
    // live preview (a DWM thumbnail drawn into the bar by the compositor)
    HTHUMBNAIL thumb = nullptr;
    bool thumbShown = false;
    RECT lastDst{0, 0, 0, 0}, lastSrc{0, 0, 0, 0};   // what DWM was last told, to skip redundant updates
    BYTE lastOpacity = 0;
    DWORD thumbRetry = 0;          // after a failed registration, when to try again
    SIZE src{0, 0};                // DWM's size for the source window
    RECT crop{0, 0, 0, 0};         // the part of the source shown, in source coordinates
    int winW = 0, winH = 0;        // window size the insets were measured at
    RECT inset{0, 0, 0, 0};        // invisible resize borders (or the frame, with clientonly) cropped away
    bool insetValid = false;
    float aspect = 0.5625f;        // height / width of what's shown
    // column layout in client coords: where the card is heading (t*) and where it is now (c*)
    float tTop = 0, tH = 0, tW = 0, tX = 0, cTop = 0, cH = 0, cW = 0, cX = 0;
    bool placed = false, inLayout = false;
};
struct Row  { RECT r; int kind; int idx; int clipTop; int clipBottom; };
struct Item { int kind; int idx; std::vector<std::wstring> lines; int h; };

struct Config {
    int width = 150, gap = 6, spacing = 2, pad = 4, vpad = 2, lineGap = 0, maxLines = 3;
    int highlight = 4, fontSize = 12, fontWeight = 400, pinsMax = 35, hoverDelay = 0, borderMode = 1;
    std::wstring font = L"Consolas", fontFile, pinDir;
    bool aa = false, clock = true, hideTaskbar = false, ellipsis = true, startPins = true, italic = false;
    bool bold = false, hoverRestore = false, hoverFocus = false, meter = true, rightSide = false;
    int monitor = 0;   // 0 = primary, otherwise Windows display number
    int clockFontSize = 0, clockLines = 2, clockBold = -1, startHeight = 0;   // 0 / -1 = follow the main font
    int startLines = 2;  // start button height in text lines, while startHeight is 0
    bool startButton = true, clock24 = false;
    int weekStart = 0;   // 0 = Sunday, 1 = Monday
    int calTimeout = 5;  // seconds the calendar stays up once the mouse leaves it
    COLORREF startColor = RGB(216, 216, 216);
    bool startColorSet = false;
    int meterH = 2, meterFall = 4, meterDb = 48, meterFps = 30;
    COLORREF bg = RGB(0, 0, 0), face = RGB(64, 64, 64), text = RGB(216, 216, 216),
             hi = RGB(255, 255, 255), dim = RGB(136, 136, 136), flash = RGB(110, 110, 110),
             outline = RGB(0, 0, 0), hoverText = RGB(255, 255, 255), activeText = RGB(255, 255, 255),
             activeFace = RGB(64, 64, 64), pinFace = RGB(64, 64, 64), clockFace = RGB(64, 64, 64),
             activeBorder = RGB(255, 255, 255), meterColor = RGB(47, 125, 255);
    COLORREF flashColor = RGB(47, 125, 255);
    bool flashColorSet = false;
    int hoverBright = 28, flashRate = 500, flashCount = 8;
    bool icons = true, iconDither = true;
    int iconSize = 0, iconPad = 3, scrollW = 2;
    COLORREF iconShade[3] = {RGB(0, 0, 0), RGB(140, 140, 140), RGB(232, 232, 232)};
    COLORREF scrollColor = RGB(47, 125, 255);
    bool scrollColorSet = false;
    bool activeFaceSet = false, pinFaceSet = false, clockFaceSet = false;
    std::vector<std::wstring> strip;
    std::vector<Pin> pins;
    // live previews
    int previewMargin = 2, step = 75, minScale = 30, thumbPad = 3, titleMin = 80;
    int maxAspect = 100, minAspect = 30, minOpacity = 110, animSpeed = 35, recenterDelay = 400;
    int popOut = 10;          // px the hovered preview sticks out past the bar's inner edge
    bool clientOnly = false;
    // mini terminal
    bool terminal = true;
    std::wstring shell = L"powershell", termDir, termKey = L"Ctrl+Alt+T";
    int termLines = 18, termFontSize = 0, termScrollback = 2000;
    COLORREF termBg = RGB(12, 12, 12), termText = RGB(204, 204, 204), termCmd = RGB(242, 242, 242),
             termErr = RGB(231, 72, 86), termDim = RGB(118, 118, 118), termSel = RGB(38, 79, 120);
    int configVer = 0;
    // smartwatch clock
    bool watch = true;                    // clockstyle=watch (false = the classic text clock)
    int watchHeight = 100, watchFps = 20;
    std::wstring watchFont = L"Bahnschrift SemiBold";
    COLORREF watchAccent = RGB(47, 125, 255), watchDay = RGB(255, 159, 10);
    COLORREF watchRing[3] = {RGB(250, 17, 79), RGB(146, 232, 42), RGB(30, 234, 239)};   // seconds, minutes, hours
};

static Config g_cfg;
static HWND g_hwnd = nullptr;
static HFONT g_font = nullptr, g_clockFont = nullptr, g_calBigFont = nullptr, g_termFont = nullptr;
static HWND g_calHwnd = nullptr;
static int g_calYear = 0, g_calMonth = 0, g_calHover = 0;
static DWORD g_calClosedAt = 0, g_calLastHover = 0;
static WORD g_calLastSecond = 0xFFFF;
static bool g_blinkOn = true;
static int g_blinkTicks = 0;
static std::wstring g_exeDir, g_iniPath, g_loadedFontFile;
static std::vector<Task> g_tasks;
static std::vector<Pin> g_startPins;
static std::vector<Row> g_rows;
static int g_scrollPins = 0, g_lineH = 14, g_termLineH = 14;
static int g_pinsTop = 0, g_pinsBottom = 0;
static int g_hoverKind = K_NONE, g_hoverIdx = -1;
static bool g_tracking = false;
static UINT g_shellMsg = 0, g_taskbarCreatedMsg = 0;
static HWND g_lastFg = nullptr;
static int g_lastMinute = -1;
static LONG g_startPinsGen = 0;
static std::map<std::wstring, IconPtr> g_pinIcons;
static HDC g_iconDC = nullptr;
static int g_iconS = 0, g_indent = 0;
static bool g_fullscreenApp = false, g_zBottom = false;
static HWND g_fullscreenHwnd = nullptr;

// preview column state
static HWND g_lastTaskFg = nullptr;      // the last task window that had focus
static HWND g_pvAnchor = nullptr;        // the window shown biggest
static HWND g_pvBase = nullptr;          // the window the column's order wraps around (sits mid-column when idle)
static bool g_pvHover = false;           // true while the mouse (or the wheel) is steering the column
static int g_pvAnchorY = 0;              // centre of the anchor card while hovering
static int g_W = 0, g_pvTop = 0, g_pvBottom = 0;   // bar width (the reserved strip) and the column's extent
static int g_winW = 0;                              // window width: the bar plus the pop-out strip
static int g_wheelAcc = 0;

// ---------- helpers ----------

static std::string WToUtf8(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

static std::wstring Utf8ToW(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

static std::wstring DecodeText(const std::string& raw) {
    if (raw.size() >= 2 && (unsigned char)raw[0] == 0xFF && (unsigned char)raw[1] == 0xFE) {
        std::wstring w((raw.size() - 2) / 2, 0);
        memcpy(&w[0], raw.data() + 2, w.size() * 2);
        return w;
    }
    if (raw.size() >= 3 && (unsigned char)raw[0] == 0xEF && (unsigned char)raw[1] == 0xBB && (unsigned char)raw[2] == 0xBF)
        return Utf8ToW(raw.substr(3));
    return Utf8ToW(raw);
}

static std::string ReadFileBytes(const std::wstring& path) {
    std::ifstream f(path.c_str(), std::ios::binary);
    if (!f) return "";
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static std::wstring Trim(const std::wstring& s) {
    size_t a = s.find_first_not_of(L" \t\r\n");
    if (a == std::wstring::npos) return L"";
    size_t b = s.find_last_not_of(L" \t\r\n");
    return s.substr(a, b - a + 1);
}

static std::wstring Lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), ::towlower);
    return s;
}

static std::vector<std::wstring> Split(const std::wstring& s, wchar_t d) {
    std::vector<std::wstring> out;
    size_t start = 0;
    for (;;) {
        size_t p = s.find(d, start);
        out.push_back(Trim(s.substr(start, p == std::wstring::npos ? std::wstring::npos : p - start)));
        if (p == std::wstring::npos) break;
        start = p + 1;
    }
    return out;
}

static COLORREF ParseColor(const std::wstring& v, COLORREF def) {
    std::wstring s = v;
    if (!s.empty() && s[0] == L'#') s = s.substr(1);
    if (s.size() != 6) return def;
    unsigned long x = wcstoul(s.c_str(), nullptr, 16);
    return RGB((x >> 16) & 255, (x >> 8) & 255, x & 255);
}

static std::wstring ExpandEnv(const std::wstring& s) {
    if (s.empty()) return s;
    wchar_t buf[2048];
    DWORD n = ExpandEnvironmentStringsW(s.c_str(), buf, 2048);
    return (n && n <= 2048) ? std::wstring(buf) : s;
}

static std::wstring Resolve(const std::wstring& p) {
    std::wstring e = ExpandEnv(p);
    if (e.size() > 1 && (e[1] == L':' || (e[0] == L'\\' && e[1] == L'\\'))) return e;
    return g_exeDir + L"\\" + e;
}

static std::wstring FileStem(const std::wstring& path) {
    size_t s = path.find_last_of(L"\\/");
    std::wstring fn = s == std::wstring::npos ? path : path.substr(s + 1);
    size_t dot = fn.find_last_of(L'.');
    return dot == std::wstring::npos ? fn : fn.substr(0, dot);
}

static bool IsDir(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static void Fill(HDC dc, const RECT& r, COLORREF c) {
    SetDCBrushColor(dc, c);
    FillRect(dc, &r, (HBRUSH)GetStockObject(DC_BRUSH));
}

static std::wstring ProcessPath(HWND h) {
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return L"";
    wchar_t buf[MAX_PATH * 2];
    DWORD n = MAX_PATH * 2;
    std::wstring out;
    if (QueryFullProcessImageNameW(p, 0, buf, &n)) out.assign(buf, n);
    CloseHandle(p);
    return out;
}

static void OpenLocation(const std::wstring& path) {
    if (path.empty()) return;
    std::wstring args = L"/select,\"" + path + L"\"";
    ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
}

static void CopyToClipboard(const std::wstring& s) {
    if (!OpenClipboard(g_hwnd)) return;
    EmptyClipboard();
    size_t bytes = (s.size() + 1) * sizeof(wchar_t);
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (g) {
        void* p = GlobalLock(g);
        if (p) {
            memcpy(p, s.c_str(), bytes);
            GlobalUnlock(g);
            if (!SetClipboardData(CF_UNICODETEXT, g)) GlobalFree(g);
        } else {
            GlobalFree(g);
        }
    }
    CloseClipboard();
}

// ---------- pins ----------

static int PinCount() { return (int)(g_cfg.pins.size() + g_startPins.size()); }
static const Pin& PinAt(int i) {
    return i < (int)g_cfg.pins.size() ? g_cfg.pins[i] : g_startPins[i - g_cfg.pins.size()];
}

static std::vector<Pin> ListShortcutDir(const std::wstring& dir) {
    std::vector<Pin> found;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return found;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::wstring fn = fd.cFileName;
        size_t dot = fn.find_last_of(L'.');
        if (dot == std::wstring::npos) continue;
        std::wstring ext = Lower(fn.substr(dot));
        if (ext != L".lnk" && ext != L".url" && ext != L".exe") continue;
        Pin p;
        p.name = fn.substr(0, dot);
        p.path = dir + L"\\" + fn;
        found.push_back(p);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(found.begin(), found.end(), [](const Pin& a, const Pin& b) {
        return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
    });
    return found;
}

// Minimal JSON string reader: s[i] == '"' on entry, returns value, leaves i after the closing quote.
static std::wstring JsonString(const std::wstring& s, size_t& i) {
    std::wstring out;
    ++i;
    while (i < s.size() && s[i] != L'"') {
        wchar_t c = s[i++];
        if (c == L'\\' && i < s.size()) {
            wchar_t e = s[i++];
            switch (e) {
            case L'n': out += L'\n'; break;
            case L't': out += L'\t'; break;
            case L'r': out += L'\r'; break;
            case L'b': out += L'\b'; break;
            case L'f': out += L'\f'; break;
            case L'u':
                if (i + 4 <= s.size()) { out += (wchar_t)wcstoul(s.substr(i, 4).c_str(), nullptr, 16); i += 4; }
                break;
            default: out += e; break;     // \\  \"  \/
            }
        } else {
            out += c;
        }
    }
    if (i < s.size()) ++i;
    return out;
}

static std::wstring AppDisplayName(const std::wstring& aumid) {
    std::wstring name;
    std::wstring parse = L"shell:AppsFolder\\" + aumid;
    IShellItem* item = nullptr;
    if (SUCCEEDED(SHCreateItemFromParsingName(parse.c_str(), nullptr, IID_IShellItem, (void**)&item)) && item) {
        PWSTR n = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &n)) && n) {
            name = n;
            CoTaskMemFree(n);
        }
        item->Release();
    }
    if (name.empty()) {                       // fall back to something readable from the id
        name = aumid;
        size_t bang = name.find(L'!');
        if (bang != std::wstring::npos) name = name.substr(0, bang);
        size_t us = name.find(L'_');
        if (us != std::wstring::npos) name = name.substr(0, us);
        size_t dot = name.find_last_of(L'.');
        if (dot != std::wstring::npos && dot + 1 < name.size()) name = name.substr(dot + 1);
    }
    return name;
}

// Asks Windows for its own Start layout (Export-StartLayout) and turns the pinned list into Pins.
// Falls back to the taskbar's pinned shortcuts folder if that yields nothing.
static std::vector<Pin> LoadStartPins() {
    std::vector<Pin> pins;

    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring json = std::wstring(tmp) + L"pbar_startlayout.json";
    DeleteFileW(json.c_str());

    std::wstring quoted;
    for (wchar_t c : json) { quoted += c; if (c == L'\'') quoted += L'\''; }
    std::wstring cmd = L"powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command "
                       L"\"Export-StartLayout -Path '" + quoted + L"'\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                       nullptr, nullptr, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 20000);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }

    std::wstring s = DecodeText(ReadFileBytes(json));
    size_t i = 0;
    while (i < s.size()) {
        if (s[i] != L'"') { ++i; continue; }
        std::wstring key = JsonString(s, i);
        size_t j = i;
        while (j < s.size() && iswspace(s[j])) ++j;
        if (j >= s.size() || s[j] != L':') continue;
        ++j;
        while (j < s.size() && iswspace(s[j])) ++j;
        if (j >= s.size() || s[j] != L'"') { i = j; continue; }
        i = j;
        std::wstring val = JsonString(s, i);
        if (val.empty()) continue;
        Pin p;
        if (key == L"desktopAppLink") {
            p.path = ExpandEnv(val);
            p.name = FileStem(p.path);
        } else if (key == L"packagedAppId" || key == L"desktopAppId") {
            p.aumid = val;
            p.name = AppDisplayName(val);
        } else {
            continue;
        }
        if (!p.name.empty()) pins.push_back(p);
    }
    DeleteFileW(json.c_str());

    if (pins.empty())
        pins = ListShortcutDir(ExpandEnv(L"%APPDATA%\\Microsoft\\Internet Explorer\\Quick Launch\\User Pinned\\TaskBar"));
    return pins;
}

struct StartPinsResult { LONG gen; std::vector<Pin> pins; };

static DWORD WINAPI StartPinsThread(LPVOID param) {
    LONG gen = (LONG)(LONG_PTR)param;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    StartPinsResult* res = new StartPinsResult{gen, LoadStartPins()};
    CoUninitialize();
    if (!PostMessageW(g_hwnd, WM_STARTPINS, 0, (LPARAM)res)) delete res;
    return 0;
}

static void RequestStartPins() {
    LONG gen = InterlockedIncrement(&g_startPinsGen);
    if (!g_cfg.startPins) { g_startPins.clear(); return; }
    HANDLE t = CreateThread(nullptr, 0, StartPinsThread, (LPVOID)(LONG_PTR)gen, 0, nullptr);
    if (t) CloseHandle(t);
}

// ---------- config ----------

static const char* kDefaultIni =
"; pbar.ini  - right-click the clock > Settings, or edit this and right-click the clock > Reload\n"
"; All sizes are physical pixels. Comments must be on their own line.\n"
"\n"
"; bar width, 60 to 150\n"
"width=150\n"
"; black space between buttons and between previews\n"
"spacing=2\n"
"; black gap between the terminal, the previews, the pinned section and the clock\n"
"gap=6\n"
"; text padding inside a button: pad = left/right, vpad = top/bottom\n"
"pad=4\n"
"vpad=2\n"
"; extra pixels between wrapped lines\n"
"linegap=0\n"
"; wrapped lines per pinned button before cutting off (1 = no wrapping)\n"
"maxlines=3\n"
"; 1 = end a cut-off line with an ellipsis\n"
"ellipsis=1\n"
"; white corner highlight length in px (0 = full top/left edges)\n"
"highlight=4\n"
"\n"
"font=Consolas\n"
"fontsize=12\n"
"; 1 = bold (same as fontweight=700)\n"
"bold=0\n"
"; finer control: 100-900 (400 = normal, 700 = bold, 900 = black)\n"
"fontweight=400\n"
"italic=0\n"
"; optional font file next to pbar.exe, loaded privately (e.g. TerminusTTF-4.49.3.ttf, then font=Terminus (TTF))\n"
"fontfile=\n"
"; 0 = crisp non-antialiased, 1 = ClearType\n"
"aa=0\n"
"\n"
"; ---- live previews ----\n"
"; The focused window sits mid-column at full size; each step away is smaller. Hovering makes the\n"
"; preview under the mouse the big one, and the wheel spins the column round (it wraps forever).\n"
"; space either side of the biggest preview\n"
"previewmargin=2\n"
"; each step away from the big preview is this % of the size of the one before it\n"
"step=75\n"
"; smallest a preview gets, % of the biggest\n"
"minscale=30\n"
"; space between a card's edge and its live picture\n"
"thumbpad=3\n"
"; cards narrower than this (px) drop their title line (0 = never show titles)\n"
"titlemin=80\n"
"; tallest and flattest picture box, % of its width (portrait windows get letterboxed)\n"
"maxaspect=100\n"
"minaspect=30\n"
"; 1 = show only each window's contents, without its title bar and frame\n"
"clientonly=0\n"
"; opacity of minimized windows' previews, 0-255\n"
"minopacity=110\n"
"; animation: % of the remaining distance covered per frame (100 = jump straight there)\n"
"animspeed=35\n"
"; ms after the mouse leaves before the focused window slides back to the middle\n"
"recenterdelay=400\n"
"; how far (px) the hovered preview sticks out past the bar's inner edge (0 = stays inside)\n"
"popout=10\n"
"; 1 = hovering a preview also brings that window to the front (like vbar)\n"
"hoverfocus=0\n"
"; ms to wait before raising (0 = instant)\n"
"hoverdelay=0\n"
"; 1 = hovering also restores minimized windows\n"
"hoverrestore=0\n"
"\n"
"; ---- mini terminal at the top ----\n"
"terminal=1\n"
"; powershell (Windows PowerShell), pwsh (PowerShell 7) or cmd. Also in the terminal's right-click menu.\n"
"shell=powershell\n"
"; output lines shown (the wheel scrolls back through the rest)\n"
"termlines=18\n"
"; text size in px (0 = same as fontsize)\n"
"termfontsize=0\n"
"; folder it starts in (blank = your user folder)\n"
"termdir=\n"
"; global hotkey that jumps to the terminal, and back to your window if pressed again (blank = none)\n"
"termkey=Ctrl+Alt+T\n"
"; lines of output kept\n"
"termscrollback=2000\n"
"termbg=#0C0C0C\n"
"termtext=#CCCCCC\n"
"; your commands\n"
"termcmd=#F2F2F2\n"
"; error output\n"
"termerr=#E74856\n"
"; folder line and notes\n"
"termdim=#767676\n"
"; behind selected text (drag to select, double-click a word; Ctrl+C or right-click > Copy)\n"
"termsel=#264F78\n"
"\n"
"clock=1\n"
"; watch = smartwatch face (big hour/minute digits, day and date, seconds/minutes/hours rings)\n"
"; classic = the old one-line text clock\n"
"clockstyle=watch\n"
"; watch height in px\n"
"watchheight=100\n"
"; watch font (falls back to Segoe UI if it isn't installed)\n"
"watchfont=Bahnschrift SemiBold\n"
"; colours: the minutes digits, the day name, and the rings (seconds, minutes, hours)\n"
"watchaccent=#2F7DFF\n"
"watchday=#FF9F0A\n"
"watchrings=#FA114F,#92E82A,#1EEAEF\n"
"; frames per second for the sweeping seconds ring (0 = tick once a second)\n"
"watchfps=20\n"
"; classic clock text size in px (0 = same as fontsize); it wraps onto up to clocklines lines if it doesn't fit\n"
"clockfontsize=0\n"
"clocklines=2\n"
"; 0 = 12-hour (5:11 PM), 1 = 24-hour (17:11). Also in the right-click menu.\n"
"clock24=0\n"
"; click the clock for a calendar; first day of the week: 0 = Sunday, 1 = Monday\n"
"weekstart=0\n"
"; the calendar stays open while the mouse is over it (or the clock) and closes this many seconds after it leaves\n"
"caltimeout=5\n"
"; clockbold=1   (leave out to follow bold)\n"
"\n"
"; start button at the very bottom: click = Start menu, right-click = Win+X menu\n"
"startbutton=1\n"
"; height in text lines, the same as a button with that many lines (1 to 4)\n"
"startlines=2\n"
"; or an exact height in px, which wins over startlines (0 = use startlines)\n"
"startheight=0\n"
"; colour of the shade blocks (leave out to follow text)\n"
";startcolor=#D8D8D8\n"
"\n"
"; ---- icons: each app's real icon reduced to 2 bits (transparent + 3 grey shades) ----\n"
"icons=1\n"
"; icon size in px (0 = same as the text line height)\n"
"iconsize=0\n"
"; space between icon and text\n"
"iconpad=3\n"
"; 1 = ordered dither between shades (retro look), 0 = hard steps\n"
"icondither=1\n"
"; the three shades, darkest to lightest\n"
"iconshades=#000000,#8C8C8C,#E8E8E8\n"
"\n"
"; ---- scroll indicator on the pinned section and the terminal when they overflow ----\n"
"; width in px (0 = off); colour defaults to metercolor\n"
"scrollw=2\n"
";scrollcolor=#2F7DFF\n"
"\n"
"; ---- audio meter: a bar along the bottom of any preview whose app is playing sound ----\n"
"meter=1\n"
"; bar height in px (sits in the bottom padding, so keep thumbpad >= meterh + 1)\n"
"meterh=2\n"
"metercolor=#2F7DFF\n"
"; dB range shown across the full width (0 = linear amplitude instead)\n"
"meterdb=48\n"
"; how fast the bar falls back, in % of full width per frame\n"
"meterfall=4\n"
"; updates per second while sound is playing\n"
"meterfps=30\n"
"; 1 = hide the Windows taskbar while pbar runs (also turn on taskbar auto-hide so it frees the screen space)\n"
"hidetaskbar=0\n"
"; which screen: primary, or the display number Windows shows in Settings > Display > Identify\n"
"monitor=primary\n"
"; left or right edge of that screen\n"
"side=left\n"
"\n"
"; ---- colours (#RRGGBB) ----\n"
"; bar background (also the space between buttons)\n"
"bg=#000000\n"
"; 1px border around every button\n"
"outline=#000000\n"
"; border colour for previews picked out by bordermode\n"
"activeborder=#FFFFFF\n"
"; focused = only the window in front | visible = every window not fully covered or minimized | off\n"
"bordermode=focused\n"
"; corner highlight\n"
"hi=#FFFFFF\n"
"; button face; activeface / pinface / clockface override it per kind (leave out to follow face)\n"
"face=#404040\n"
";activeface=#404040\n"
";pinface=#404040\n"
";clockface=#404040\n"
"; text: normal, hovered, focused window, minimized window\n"
"text=#D8D8D8\n"
"hovertext=#FFFFFF\n"
"; how much a hovered button's face brightens (added to each RGB channel; 0 = off)\n"
"hoverbright=28\n"
"activetext=#FFFFFF\n"
"dim=#888888\n"
"; a window asking for attention blinks this colour (leave out to match metercolor)\n"
";flash=#2F7DFF\n"
"; blink speed in ms, and how many blinks before it stays lit until you focus the window (0 = stay lit)\n"
"flashrate=500\n"
"flashcount=8\n"
"\n"
"; suffixes trimmed from window titles\n"
"strip=- Google Chrome\n"
"strip=- Mozilla Firefox\n"
"strip=- Microsoft Edge\n"
"strip=- Brave\n"
"strip=- Visual Studio Code\n"
"strip=- Notepad\n"
"strip=- File Explorer\n"
"\n"
"; ---- pinned section (above the clock) ----\n"
"; 1 = include your Windows Start menu pins (read via Export-StartLayout; falls back to taskbar pins)\n"
"startpins=1\n"
"; most of the bar's height the pinned section may take, in percent (it scrolls past that)\n"
"pinsmax=35\n"
"; your own pins, listed first: pin=Name|path|args|working dir   (env vars like %USERPROFILE% work)\n"
"; pin=Terminal|wt.exe\n"
"; optional folder of shortcuts (.lnk/.url/.exe), listed alphabetically after the pin= lines\n"
"pindir=\n"
"\n"
"; ini format version (pbar updates older files itself)\n"
"configver=4\n";

static void SetIniValue(const std::wstring& key, const std::wstring& value);

static void AppendIniText(const std::wstring& block) {
    std::wstring text = DecodeText(ReadFileBytes(g_iniPath));
    if (!text.empty() && text.back() != L'\n') text += L"\r\n";
    text += block;
    std::ofstream o(g_iniPath.c_str(), std::ios::binary);
    o << WToUtf8(text);
}

// Rewrites the first copy of a line of text in the ini (used to correct a comment an older pbar wrote).
static void ReplaceIniText(const std::wstring& from, const std::wstring& to) {
    std::wstring text = DecodeText(ReadFileBytes(g_iniPath));
    size_t at = text.find(from);
    if (at == std::wstring::npos) return;
    text.replace(at, from.size(), to);
    std::ofstream o(g_iniPath.c_str(), std::ios::binary);
    o << WToUtf8(text);
}

static void LoadConfig() {
    g_cfg = Config();
    std::string raw = ReadFileBytes(g_iniPath);
    if (raw.empty()) {
        std::ofstream o(g_iniPath.c_str(), std::ios::binary);
        o << kDefaultIni;
        o.close();
        raw = kDefaultIni;
    }
    std::wstring text = DecodeText(raw);

    bool sawPopOut = false, sawTermSel = false, sawWatch = false, sawStartLines = false;
    std::wstringstream ws(text);
    std::wstring line;
    while (std::getline(ws, line)) {
        line = Trim(line);
        if (line.empty() || line[0] == L';' || line[0] == L'#') continue;
        size_t eq = line.find(L'=');
        if (eq == std::wstring::npos) continue;
        std::wstring k = Lower(Trim(line.substr(0, eq))), v = Trim(line.substr(eq + 1));
        int n = _wtoi(v.c_str());
        if      (k == L"width")       g_cfg.width = std::max(60, std::min(150, n));
        else if (k == L"gap")         g_cfg.gap = std::max(0, n);
        else if (k == L"spacing")     g_cfg.spacing = std::max(0, n);
        else if (k == L"pad")         g_cfg.pad = std::max(0, n);
        else if (k == L"vpad")        g_cfg.vpad = std::max(0, n);
        else if (k == L"linegap")     g_cfg.lineGap = std::max(0, n);
        else if (k == L"maxlines")    g_cfg.maxLines = std::max(1, n);
        else if (k == L"ellipsis")    g_cfg.ellipsis = n != 0;
        else if (k == L"highlight")   g_cfg.highlight = std::max(0, n);
        else if (k == L"font")        { if (!v.empty()) g_cfg.font = v; }
        else if (k == L"fontsize")    g_cfg.fontSize = std::max(5, n);
        else if (k == L"fontweight")  g_cfg.fontWeight = std::max(100, std::min(900, n));
        else if (k == L"italic")      g_cfg.italic = n != 0;
        else if (k == L"bold")        g_cfg.bold = n != 0;
        else if (k == L"hoverfocus")  g_cfg.hoverFocus = n != 0;
        else if (k == L"hoverdelay")  g_cfg.hoverDelay = std::max(0, n);
        else if (k == L"activeborder") g_cfg.activeBorder = ParseColor(v, g_cfg.activeBorder);
        else if (k == L"bordermode") {
            std::wstring m = Lower(v);
            g_cfg.borderMode = m == L"off" ? 0 : m == L"visible" ? 2 : 1;
        }
        else if (k == L"hoverrestore") g_cfg.hoverRestore = n != 0;
        else if (k == L"meter")       g_cfg.meter = n != 0;
        else if (k == L"meterh")      g_cfg.meterH = std::max(1, n);
        else if (k == L"meterfall")   g_cfg.meterFall = std::max(1, std::min(100, n));
        else if (k == L"meterdb")     g_cfg.meterDb = std::max(0, n);
        else if (k == L"meterfps")    g_cfg.meterFps = std::max(5, std::min(120, n));
        else if (k == L"metercolor")  g_cfg.meterColor = ParseColor(v, g_cfg.meterColor);
        else if (k == L"outline")     g_cfg.outline = ParseColor(v, g_cfg.outline);
        else if (k == L"hovertext")   g_cfg.hoverText = ParseColor(v, g_cfg.hoverText);
        else if (k == L"activetext")  g_cfg.activeText = ParseColor(v, g_cfg.activeText);
        else if (k == L"activeface")  { g_cfg.activeFace = ParseColor(v, g_cfg.activeFace); g_cfg.activeFaceSet = true; }
        else if (k == L"pinface")     { g_cfg.pinFace = ParseColor(v, g_cfg.pinFace); g_cfg.pinFaceSet = true; }
        else if (k == L"clockface")   { g_cfg.clockFace = ParseColor(v, g_cfg.clockFace); g_cfg.clockFaceSet = true; }
        else if (k == L"fontfile")    g_cfg.fontFile = v;
        else if (k == L"aa")          g_cfg.aa = n != 0;
        else if (k == L"clock")       g_cfg.clock = n != 0;
        else if (k == L"clockfontsize") g_cfg.clockFontSize = std::max(0, n);
        else if (k == L"clocklines")  g_cfg.clockLines = std::max(1, n);
        else if (k == L"clockbold")   g_cfg.clockBold = n != 0;
        else if (k == L"clock24")     g_cfg.clock24 = n != 0;
        else if (k == L"caltimeout")  g_cfg.calTimeout = std::max(1, n);
        else if (k == L"weekstart")   g_cfg.weekStart = (n == 1) ? 1 : 0;
        else if (k == L"startbutton") g_cfg.startButton = n != 0;
        else if (k == L"startheight") g_cfg.startHeight = std::max(0, n);
        else if (k == L"startlines")  { g_cfg.startLines = std::max(1, std::min(4, n)); sawStartLines = true; }
        else if (k == L"startcolor")  { g_cfg.startColor = ParseColor(v, g_cfg.startColor); g_cfg.startColorSet = true; }
        else if (k == L"hidetaskbar") g_cfg.hideTaskbar = n != 0;
        else if (k == L"side")        g_cfg.rightSide = Lower(v) == L"right";
        else if (k == L"monitor")     g_cfg.monitor = std::max(0, n);   // "primary" parses as 0
        else if (k == L"startpins")   g_cfg.startPins = n != 0;
        else if (k == L"pinsmax")     g_cfg.pinsMax = std::max(10, std::min(100, n));
        else if (k == L"bg")          g_cfg.bg = ParseColor(v, g_cfg.bg);
        else if (k == L"face")        g_cfg.face = ParseColor(v, g_cfg.face);
        else if (k == L"text")        g_cfg.text = ParseColor(v, g_cfg.text);
        else if (k == L"hi")          g_cfg.hi = ParseColor(v, g_cfg.hi);
        else if (k == L"dim")         g_cfg.dim = ParseColor(v, g_cfg.dim);
        else if (k == L"flash")       { g_cfg.flashColor = ParseColor(v, g_cfg.flashColor); g_cfg.flashColorSet = true; }
        else if (k == L"flashrate")   g_cfg.flashRate = std::max(100, n);
        else if (k == L"flashcount")  g_cfg.flashCount = std::max(0, n);
        else if (k == L"icons")       g_cfg.icons = n != 0;
        else if (k == L"iconsize")    g_cfg.iconSize = std::max(0, n);
        else if (k == L"iconpad")     g_cfg.iconPad = std::max(0, n);
        else if (k == L"icondither")  g_cfg.iconDither = n != 0;
        else if (k == L"iconshades") {
            auto parts = Split(v, L',');
            for (size_t j = 0; j < parts.size() && j < 3; ++j) g_cfg.iconShade[j] = ParseColor(parts[j], g_cfg.iconShade[j]);
        }
        else if (k == L"scrollw")     g_cfg.scrollW = std::max(0, n);
        else if (k == L"scrollcolor") { g_cfg.scrollColor = ParseColor(v, g_cfg.scrollColor); g_cfg.scrollColorSet = true; }
        else if (k == L"hoverbright") g_cfg.hoverBright = std::max(-255, std::min(255, n));
        else if (k == L"strip")       { if (!v.empty()) g_cfg.strip.push_back(v); }
        else if (k == L"pindir")      g_cfg.pinDir = v;
        else if (k == L"pin") {
            auto parts = Split(v, L'|');
            Pin p;
            p.name = parts.size() > 0 ? parts[0] : L"";
            p.path = parts.size() > 1 ? parts[1] : L"";
            p.args = parts.size() > 2 ? parts[2] : L"";
            p.dir  = parts.size() > 3 ? parts[3] : L"";
            if (!p.name.empty() && !p.path.empty()) g_cfg.pins.push_back(p);
        }
        // previews
        else if (k == L"previewmargin") g_cfg.previewMargin = std::max(0, std::min(40, n));
        else if (k == L"step")        g_cfg.step = std::max(10, std::min(100, n));
        else if (k == L"minscale")    g_cfg.minScale = std::max(5, std::min(100, n));
        else if (k == L"thumbpad")    g_cfg.thumbPad = std::max(0, std::min(20, n));
        else if (k == L"titlemin")    g_cfg.titleMin = std::max(0, n);
        else if (k == L"maxaspect")   g_cfg.maxAspect = std::max(5, std::min(400, n));
        else if (k == L"minaspect")   g_cfg.minAspect = std::max(5, std::min(400, n));
        else if (k == L"clientonly")  g_cfg.clientOnly = n != 0;
        else if (k == L"minopacity")  g_cfg.minOpacity = std::max(0, std::min(255, n));
        else if (k == L"animspeed")   g_cfg.animSpeed = std::max(1, std::min(100, n));
        else if (k == L"recenterdelay") g_cfg.recenterDelay = std::max(0, n);
        // terminal
        else if (k == L"terminal")    g_cfg.terminal = n != 0;
        else if (k == L"shell")       { if (!v.empty()) g_cfg.shell = v; }
        else if (k == L"termlines")   g_cfg.termLines = std::max(1, std::min(200, n));
        else if (k == L"termfontsize") g_cfg.termFontSize = std::max(0, n);
        else if (k == L"termdir")     g_cfg.termDir = v;
        else if (k == L"termkey")     g_cfg.termKey = v;
        else if (k == L"termscrollback") g_cfg.termScrollback = std::max(50, std::min(100000, n));
        else if (k == L"termbg")      g_cfg.termBg = ParseColor(v, g_cfg.termBg);
        else if (k == L"termtext")    g_cfg.termText = ParseColor(v, g_cfg.termText);
        else if (k == L"termcmd")     g_cfg.termCmd = ParseColor(v, g_cfg.termCmd);
        else if (k == L"termerr")     g_cfg.termErr = ParseColor(v, g_cfg.termErr);
        else if (k == L"termdim")     g_cfg.termDim = ParseColor(v, g_cfg.termDim);
        else if (k == L"termsel")     { g_cfg.termSel = ParseColor(v, g_cfg.termSel); sawTermSel = true; }
        else if (k == L"popout")      { g_cfg.popOut = std::max(0, std::min(100, n)); sawPopOut = true; }
        else if (k == L"configver")   g_cfg.configVer = n;
        // smartwatch clock
        else if (k == L"clockstyle")  { g_cfg.watch = Lower(v) != L"classic"; sawWatch = true; }
        else if (k == L"watchheight") { g_cfg.watchHeight = std::max(40, std::min(400, n)); sawWatch = true; }
        else if (k == L"watchfont")   { if (!v.empty()) g_cfg.watchFont = v; sawWatch = true; }
        else if (k == L"watchaccent") { g_cfg.watchAccent = ParseColor(v, g_cfg.watchAccent); sawWatch = true; }
        else if (k == L"watchday")    { g_cfg.watchDay = ParseColor(v, g_cfg.watchDay); sawWatch = true; }
        else if (k == L"watchfps")    { g_cfg.watchFps = std::max(0, std::min(60, n)); sawWatch = true; }
        else if (k == L"watchrings") {
            auto parts = Split(v, L',');
            for (size_t j = 0; j < parts.size() && j < 3; ++j) g_cfg.watchRing[j] = ParseColor(parts[j], g_cfg.watchRing[j]);
            sawWatch = true;
        }
    }

    // An ini from an older pbar: add the new settings, and move values still at an old default to the
    // new one (termlines 8 -> 18). Anything you changed yourself is left alone.
    if (g_cfg.configVer < PBAR_CONFIGVER) {
        if (g_cfg.configVer < 2) {
            if (g_cfg.termLines == 8) { SetIniValue(L"termlines", L"18"); g_cfg.termLines = 18; }
            std::wstring add;
            if (!sawPopOut)
                add += L"; how far (px) the hovered preview sticks out past the bar's inner edge (0 = stays inside)\r\n"
                       L"popout=10\r\n";
            if (!sawTermSel)
                add += L"; colour behind selected terminal text (drag to select, double-click a word; Ctrl+C or right-click > Copy)\r\n"
                       L"termsel=#264F78\r\n";
            if (!add.empty()) AppendIniText(L"\r\n; ---- added by a pbar update ----\r\n" + add);
        }
        if (g_cfg.configVer < 3 && !sawWatch)
            AppendIniText(L"\r\n; ---- added by a pbar update: smartwatch clock ----\r\n"
                          L"; watch = smartwatch face (big hour/minute digits, day and date, seconds/minutes/hours rings)\r\n"
                          L"; classic = the old one-line text clock\r\n"
                          L"clockstyle=watch\r\n"
                          L"; watch height in px\r\n"
                          L"watchheight=100\r\n"
                          L"; watch font (falls back to Segoe UI if it isn't installed)\r\n"
                          L"watchfont=Bahnschrift SemiBold\r\n"
                          L"; colours: the minutes digits, the day name, and the rings (seconds, minutes, hours)\r\n"
                          L"watchaccent=#2F7DFF\r\n"
                          L"watchday=#FF9F0A\r\n"
                          L"watchrings=#FA114F,#92E82A,#1EEAEF\r\n"
                          L"; frames per second for the sweeping seconds ring (0 = tick once a second)\r\n"
                          L"watchfps=20\r\n");
        if (g_cfg.configVer < 4 && !sawStartLines) {
            ReplaceIniText(L"; height in px (0 = same as a one-line button)",
                           L"; height in px, which wins over startlines (0 = use startlines)");
            AppendIniText(L"\r\n; ---- added by a pbar update: start button height ----\r\n"
                          L"; start button height in text lines, the same as a button with that many lines (1 to 4)\r\n"
                          L"startlines=2\r\n");
        }
        SetIniValue(L"configver", std::to_wstring(PBAR_CONFIGVER));
        g_cfg.configVer = PBAR_CONFIGVER;
    }

    if (g_cfg.minAspect > g_cfg.maxAspect) std::swap(g_cfg.minAspect, g_cfg.maxAspect);
    if (!g_cfg.startColorSet) g_cfg.startColor = g_cfg.text;
    if (!g_cfg.flashColorSet) g_cfg.flashColor = g_cfg.meterColor;
    if (!g_cfg.scrollColorSet) g_cfg.scrollColor = g_cfg.meterColor;
    if (!g_cfg.activeFaceSet) g_cfg.activeFace = g_cfg.face;
    if (!g_cfg.pinFaceSet)    g_cfg.pinFace = g_cfg.face;
    if (!g_cfg.clockFaceSet)  g_cfg.clockFace = g_cfg.face;

    if (!g_cfg.pinDir.empty()) {
        auto found = ListShortcutDir(Resolve(g_cfg.pinDir));
        g_cfg.pins.insert(g_cfg.pins.end(), found.begin(), found.end());
    }
}

static HFONT MakeFont(int px, int weight) {
    return CreateFontW(-px, 0, 0, 0, weight, g_cfg.italic, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       g_cfg.aa ? CLEARTYPE_QUALITY : NONANTIALIASED_QUALITY,
                       FIXED_PITCH | FF_MODERN, g_cfg.font.c_str());
}

static void CreateBarFont() {
    for (HFONT* f : {&g_font, &g_clockFont, &g_calBigFont, &g_termFont})
        if (*f) { DeleteObject(*f); *f = nullptr; }
    if (!g_loadedFontFile.empty()) {
        RemoveFontResourceExW(g_loadedFontFile.c_str(), FR_PRIVATE, nullptr);
        g_loadedFontFile.clear();
    }
    if (!g_cfg.fontFile.empty()) {
        std::wstring p = Resolve(g_cfg.fontFile);
        if (AddFontResourceExW(p.c_str(), FR_PRIVATE, nullptr) > 0) g_loadedFontFile = p;
    }
    int weight = g_cfg.bold ? std::max(g_cfg.fontWeight, (int)FW_BOLD) : g_cfg.fontWeight;
    g_font = MakeFont(g_cfg.fontSize, weight);
    bool cb = g_cfg.clockBold < 0 ? g_cfg.bold : g_cfg.clockBold != 0;
    int cweight = cb ? std::max(g_cfg.fontWeight, (int)FW_BOLD) : g_cfg.fontWeight;
    g_clockFont = MakeFont(g_cfg.clockFontSize ? g_cfg.clockFontSize : g_cfg.fontSize, cweight);
    g_calBigFont = MakeFont(g_cfg.fontSize * 2, cweight);
    g_termFont = MakeFont(g_cfg.termFontSize ? g_cfg.termFontSize : g_cfg.fontSize, weight);
}

// ---------- taskbar / appbar ----------

static void ShowSystemTaskbars(bool show) {
    int cmd = show ? SW_SHOW : SW_HIDE;
    if (HWND t = FindWindowW(L"Shell_TrayWnd", nullptr)) ShowWindow(t, cmd);
    HWND s = nullptr;
    while ((s = FindWindowExW(nullptr, s, L"Shell_SecondaryTrayWnd", nullptr)) != nullptr) ShowWindow(s, cmd);
}

struct Mon { HMONITOR h; RECT rc; int num; bool primary; };

static BOOL CALLBACK MonProc(HMONITOR hm, HDC, LPRECT, LPARAM lp) {
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(hm, &mi)) return TRUE;
    std::wstring dev = mi.szDevice;                       // e.g. \\.\DISPLAY2
    size_t d = dev.find_last_not_of(L"0123456789");
    int num = (d != std::wstring::npos && d + 1 < dev.size()) ? _wtoi(dev.c_str() + d + 1) : 0;
    ((std::vector<Mon>*)lp)->push_back(Mon{hm, mi.rcMonitor, num, (mi.dwFlags & MONITORINFOF_PRIMARY) != 0});
    return TRUE;
}

static std::vector<Mon> Monitors() {
    std::vector<Mon> v;
    EnumDisplayMonitors(nullptr, nullptr, MonProc, (LPARAM)&v);
    std::sort(v.begin(), v.end(), [](const Mon& a, const Mon& b) { return a.num < b.num; });
    return v;
}

// The configured screen, or the primary one if it isn't connected.
static HMONITOR PickMonitor() {
    if (g_cfg.monitor > 0)
        for (auto& m : Monitors()) if (m.num == g_cfg.monitor) return m.h;
    return MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
}

// True while the window in front covers the bar's whole screen (a fullscreen game or video).
static bool ForegroundCovers() {
    HWND fg = GetForegroundWindow();
    if (!fg || fg == g_hwnd || fg == g_calHwnd) return false;
    wchar_t cls[64] = {0};
    GetClassNameW(fg, cls, 64);
    if (!wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW") ||
        !wcscmp(cls, L"Shell_TrayWnd") || !wcscmp(cls, L"Shell_SecondaryTrayWnd")) return false;
    RECT wr;
    if (!GetWindowRect(fg, &wr)) return false;
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST), &mi);
    return wr.left <= mi.rcMonitor.left && wr.top <= mi.rcMonitor.top &&
           wr.right >= mi.rcMonitor.right && wr.bottom >= mi.rcMonitor.bottom;
}

// Topmost normally; dropped to the bottom while a fullscreen app owns the bar's screen, so pbar never
// knocks a game out of exclusive fullscreen. Only touches the z-order when that state changes.
static void ApplyZOrder(bool force) {
    HWND fg = GetForegroundWindow();
    bool bottom = ForegroundCovers() || (g_fullscreenApp && fg && fg == g_fullscreenHwnd);
    if (!force && bottom == g_zBottom) return;
    g_zBottom = bottom;
    SetWindowPos(g_hwnd, bottom ? HWND_BOTTOM : HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

// The window is popout px wider than the strip the bar reserves, on its inner side, so the hovered
// preview can stick out past the bar. A window region keeps that extra strip invisible and
// click-through except where a card actually sticks out into it.
static int BarOX() { return g_cfg.rightSide ? g_cfg.popOut : 0; }   // where the bar starts inside the window

static RECT BarScreenRect() {
    RECT r;
    GetWindowRect(g_hwnd, &r);
    r.left += BarOX();
    r.right = r.left + g_cfg.width;
    return r;
}

static std::vector<RECT> g_rgnExtra;
static bool g_rgnSet = false;

// Region = the bar plus any card currently sticking out of it. Returns true if it changed.
static bool UpdateBarRegion(bool force = false) {
    if (!g_hwnd) return false;
    RECT wr;
    GetWindowRect(g_hwnd, &wr);
    int ox = BarOX(), W = g_cfg.width;
    std::vector<RECT> extra;
    if (g_cfg.popOut > 0 && g_pvBottom > g_pvTop)
        for (auto& t : g_tasks) {
            if (!t.inLayout || !t.placed) continue;
            int x0 = (int)std::lround(t.cX), x1 = x0 + (int)std::lround(t.cW);
            if (x0 >= 0 && x1 <= W) continue;                        // inside the bar
            int y0 = std::max(g_pvTop, (int)std::lround(t.cTop));
            int y1 = std::min(g_pvBottom, (int)std::lround(t.cTop + t.cH));
            if (y1 > y0) extra.push_back(RECT{x0 + ox, y0, x1 + ox, y1});
        }
    bool same = g_rgnSet && !force && extra.size() == g_rgnExtra.size() &&
                std::equal(extra.begin(), extra.end(), g_rgnExtra.begin(),
                           [](const RECT& a, const RECT& b) { return EqualRect(&a, &b) != 0; });
    if (same) return false;
    HRGN rgn = CreateRectRgn(ox, 0, ox + W, wr.bottom - wr.top);
    for (auto& r : extra) {
        HRGN e = CreateRectRgnIndirect(&r);
        CombineRgn(rgn, rgn, e, RGN_OR);
        DeleteObject(e);
    }
    if (!SetWindowRgn(g_hwnd, rgn, TRUE)) DeleteObject(rgn);   // on success the system owns rgn
    g_rgnExtra = extra;
    g_rgnSet = true;
    return true;
}

static void AppBarSetPos() {
    HMONITOR mon = PickMonitor();
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(mon, &mi);

    APPBARDATA abd{};
    abd.cbSize = sizeof(abd);
    abd.hWnd = g_hwnd;
    abd.uEdge = g_cfg.rightSide ? ABE_RIGHT : ABE_LEFT;
    abd.rc = mi.rcMonitor;
    if (g_cfg.rightSide) abd.rc.left = abd.rc.right - g_cfg.width;
    else abd.rc.right = abd.rc.left + g_cfg.width;
    SHAppBarMessage(ABM_QUERYPOS, &abd);                 // the shell may move our edge in (other appbars)
    if (g_cfg.rightSide) abd.rc.left = abd.rc.right - g_cfg.width;
    else abd.rc.right = abd.rc.left + g_cfg.width;
    SHAppBarMessage(ABM_SETPOS, &abd);

    // Position only: z-order is ApplyZOrder's job, so a display change mid-game can't pull the bar on top.
    // The window covers the reserved strip plus the pop-out strip on the inner side.
    SetWindowPos(g_hwnd, nullptr, abd.rc.left - BarOX(), abd.rc.top,
                 (abd.rc.right - abd.rc.left) + g_cfg.popOut, abd.rc.bottom - abd.rc.top, SWP_NOACTIVATE | SWP_NOZORDER);
    UpdateBarRegion(true);
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

static void AppBarRegister() {
    APPBARDATA abd{};
    abd.cbSize = sizeof(abd);
    abd.hWnd = g_hwnd;
    abd.uCallbackMessage = WM_APPBAR;
    SHAppBarMessage(ABM_NEW, &abd);
    AppBarSetPos();
    ApplyZOrder(true);
}

static void AppBarRemove() {
    APPBARDATA abd{};
    abd.cbSize = sizeof(abd);
    abd.hWnd = g_hwnd;
    SHAppBarMessage(ABM_REMOVE, &abd);
}

// ---------- window list ----------

static bool IsTaskWindow(HWND h) {
    if (h == g_hwnd || !IsWindowVisible(h)) return false;
    LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
    bool appWin = (ex & WS_EX_APPWINDOW) != 0;
    if (!appWin) {
        if (GetWindow(h, GW_OWNER)) return false;
        if (ex & WS_EX_TOOLWINDOW) return false;
        if (ex & WS_EX_NOACTIVATE) return false;
    }
    int cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) return false;
    if (GetWindowTextLengthW(h) == 0) return false;
    wchar_t cls[64];
    GetClassNameW(h, cls, 64);
    if (!wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW") ||
        !wcscmp(cls, L"Shell_TrayWnd") || !wcscmp(cls, L"Shell_SecondaryTrayWnd")) return false;
    return true;
}

static std::wstring CleanTitle(HWND h) {
    int len = GetWindowTextLengthW(h);
    std::wstring t(len + 1, 0);
    GetWindowTextW(h, &t[0], len + 1);
    t.resize(wcslen(t.c_str()));
    t.erase(std::remove(t.begin(), t.end(), L'\x200B'), t.end());   // Edge pads its name with zero-width spaces
    for (auto& s : g_cfg.strip) {
        if (t.size() > s.size() && t.compare(t.size() - s.size(), s.size(), s) == 0) {
            t.resize(t.size() - s.size());
            t = Trim(t);
            break;
        }
    }
    return t;
}

static BOOL CALLBACK EnumProc(HWND h, LPARAM lp) {
    if (IsTaskWindow(h)) ((std::vector<HWND>*)lp)->push_back(h);
    return TRUE;
}

static int TaskIndex(HWND h) {
    if (!h) return -1;
    for (size_t i = 0; i < g_tasks.size(); ++i) if (g_tasks[i].hwnd == h) return (int)i;
    return -1;
}

// The window to treat as focused: the real foreground window, or while you're typing in pbar's
// terminal (or using its menus), the window you were in before.
static HWND FocusedTask() {
    HWND fg = GetForegroundWindow();
    if (fg && (fg == g_hwnd || fg == g_calHwnd)) return g_lastTaskFg;
    return fg;
}

static void ThumbHide(Task& t) {
    if (t.thumb && t.thumbShown) {
        DWM_THUMBNAIL_PROPERTIES p{};
        p.dwFlags = DWM_TNP_VISIBLE;
        p.fVisible = FALSE;
        DwmUpdateThumbnailProperties(t.thumb, &p);
    }
    t.thumbShown = false;
}

static void ThumbRelease(Task& t) {
    if (t.thumb) DwmUnregisterThumbnail(t.thumb);
    t.thumb = nullptr;
    t.thumbShown = false;
}

// Measures what a preview shows: the window minus its invisible resize borders (or just its client
// area), as a crop rectangle in DWM's source coordinates, plus that area's height/width ratio.
// Minimized windows keep the measurements from when they were last on screen.
// Returns true if the shape changed enough to re-layout the column.
static bool UpdateGeometry(Task& t) {
    if (!t.iconic) {
        RECT wr;
        if (GetWindowRect(t.hwnd, &wr) && wr.right > wr.left && wr.bottom > wr.top) {
            RECT in{0, 0, 0, 0};
            if (g_cfg.clientOnly) {
                RECT cr;
                POINT o{0, 0};
                if (GetClientRect(t.hwnd, &cr) && ClientToScreen(t.hwnd, &o) && cr.right > 0 && cr.bottom > 0)
                    in = RECT{o.x - wr.left, o.y - wr.top, wr.right - (o.x + cr.right), wr.bottom - (o.y + cr.bottom)};
            } else {
                RECT fr;
                if (SUCCEEDED(DwmGetWindowAttribute(t.hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &fr, sizeof(fr))))
                    in = RECT{fr.left - wr.left, fr.top - wr.top, wr.right - fr.right, wr.bottom - fr.bottom};
            }
            in.left = std::max(0L, (long)in.left);
            in.top = std::max(0L, (long)in.top);
            in.right = std::max(0L, (long)in.right);
            in.bottom = std::max(0L, (long)in.bottom);
            t.winW = wr.right - wr.left;
            t.winH = wr.bottom - wr.top;
            t.inset = in;
            t.insetValid = true;
        }
    } else if (!t.insetValid) {
        WINDOWPLACEMENT wp{};
        wp.length = sizeof(wp);
        if (GetWindowPlacement(t.hwnd, &wp)) {
            t.winW = wp.rcNormalPosition.right - wp.rcNormalPosition.left;
            t.winH = wp.rcNormalPosition.bottom - wp.rcNormalPosition.top;
            t.inset = RECT{0, 0, 0, 0};
            t.insetValid = t.winW > 0 && t.winH > 0;
        }
    }
    if (t.thumb) {
        SIZE s{0, 0};
        if (SUCCEEDED(DwmQueryThumbnailSourceSize(t.thumb, &s))) t.src = s;
    }

    RECT oldCrop = t.crop;
    float shownW = 16, shownH = 9;
    bool srcOk = t.src.cx > 0 && t.src.cy > 0 && !(t.iconic && t.src.cy < 48);
    if (srcOk && t.winW > 0 && t.winH > 0) {
        float sx = t.src.cx / (float)t.winW, sy = t.src.cy / (float)t.winH;   // DPI-virtualized windows differ
        RECT c{(LONG)std::lround(t.inset.left * sx), (LONG)std::lround(t.inset.top * sy),
               t.src.cx - (LONG)std::lround(t.inset.right * sx), t.src.cy - (LONG)std::lround(t.inset.bottom * sy)};
        if (c.right - c.left < 8 || c.bottom - c.top < 8) c = RECT{0, 0, t.src.cx, t.src.cy};
        t.crop = c;
        shownW = (float)(c.right - c.left);
        shownH = (float)(c.bottom - c.top);
    } else {
        t.crop = RECT{0, 0, t.src.cx, t.src.cy};
        if (t.winW > 0 && t.winH > 0) {
            shownW = (float)std::max(1, t.winW - (int)(t.inset.left + t.inset.right));
            shownH = (float)std::max(1, t.winH - (int)(t.inset.top + t.inset.bottom));
        }
    }
    float a = shownH / std::max(1.0f, shownW);
    bool changed = std::fabs(a - t.aspect) > t.aspect * 0.02f || !EqualRect(&t.crop, &oldCrop);
    t.aspect = a;
    return changed;
}

// Rebuilds the task list keeping existing order; returns true if anything visible changed.
static bool RefreshTasks() {
    std::vector<HWND> found;
    EnumWindows(EnumProc, (LPARAM)&found);
    HWND fg = GetForegroundWindow();

    std::vector<Task> next;
    for (auto& t : g_tasks) {
        if (std::find(found.begin(), found.end(), t.hwnd) != found.end()) next.push_back(t);
        else ThumbRelease(t);
    }
    for (HWND h : found) {
        bool have = std::any_of(next.begin(), next.end(), [h](const Task& t) { return t.hwnd == h; });
        if (!have) {
            Task t;
            t.hwnd = h;
            GetWindowThreadProcessId(h, &t.pid);
            t.exe = ProcessPath(h);
            t.iconic = IsIconic(h) != 0;
            UpdateGeometry(t);
            next.push_back(t);
        }
    }

    // Which windows show any pixels: walk top-to-bottom z-order, subtracting the windows above.
    std::vector<HWND> shown;
    if (g_cfg.borderMode == 2) {
        RECT screen{GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN), 0, 0};
        screen.right = screen.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
        screen.bottom = screen.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
        std::vector<RECT> above;
        for (HWND h : found) {                       // EnumWindows returns top-most first
            if (IsIconic(h)) continue;
            RECT wr;
            if (FAILED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, &wr, sizeof(wr)))) GetWindowRect(h, &wr);
            RECT vis;
            if (!IntersectRect(&vis, &wr, &screen)) continue;
            HRGN rg = CreateRectRgnIndirect(&vis);
            int res = SIMPLEREGION;
            for (auto& a : above) {
                HRGN ar = CreateRectRgnIndirect(&a);
                res = CombineRgn(rg, rg, ar, RGN_DIFF);
                DeleteObject(ar);
                if (res == NULLREGION) break;
            }
            DeleteObject(rg);
            if (res != NULLREGION) shown.push_back(h);
            above.push_back(wr);
        }
    }

    for (auto& t : next) {
        t.title = CleanTitle(t.hwnd);
        t.iconic = IsIconic(t.hwnd) != 0;
        t.visible = std::find(shown.begin(), shown.end(), t.hwnd) != shown.end();
        if (t.hwnd == fg) t.flash = false;
    }

    bool changed = next.size() != g_tasks.size() || fg != g_lastFg;
    for (size_t i = 0; !changed && i < next.size(); ++i)
        changed = next[i].hwnd != g_tasks[i].hwnd || next[i].title != g_tasks[i].title ||
                  next[i].iconic != g_tasks[i].iconic || next[i].flash != g_tasks[i].flash ||
                  next[i].visible != g_tasks[i].visible;
    g_tasks.swap(next);
    g_lastFg = fg;

    if (TaskIndex(fg) >= 0) g_lastTaskFg = fg;
    else if (TaskIndex(g_lastTaskFg) < 0) g_lastTaskFg = nullptr;
    if (g_hoverKind == K_TASK && g_hoverIdx >= (int)g_tasks.size()) { g_hoverKind = K_NONE; g_hoverIdx = -1; }
    return changed;
}

// ---------- text wrapping ----------

static bool IsBreakAfter(wchar_t c) { return c == L'-' || c == L'/' || c == L'\\' || c == L'_' || c == L'|'; }

// Wraps at spaces (or after - / \ _ |), hard-breaks words longer than a line, caps at maxLines.
// The first narrowLines lines get narrowW (they sit beside the icon); the rest get the full maxW.
static std::vector<std::wstring> Wrap(HDC dc, const std::wstring& s, int maxW, int maxLines,
                                      int narrowW = 0, int narrowLines = 0) {
    std::vector<std::wstring> out;
    size_t pos = 0, n = s.size();
    while (pos < n && (int)out.size() < maxLines) {
        while (pos < n && s[pos] == L' ') ++pos;
        if (pos >= n) break;
        int fit = 0;
        SIZE sz;
        int lineW = (int)out.size() < narrowLines ? narrowW : maxW;
        GetTextExtentExPointW(dc, s.c_str() + pos, (int)(n - pos), lineW, &fit, nullptr, &sz);
        if (fit < 1) fit = 1;
        if ((size_t)fit >= n - pos) { out.push_back(Trim(s.substr(pos))); break; }

        if ((int)out.size() == maxLines - 1) {          // last allowed line: cut it
            std::wstring l = s.substr(pos, fit);
            if (g_cfg.ellipsis && l.size() > 1) { l.pop_back(); l += L'\x2026'; }
            out.push_back(l);
            break;
        }

        size_t end = pos + fit, next = end;
        if (s[end] == L' ') {
            next = end + 1;
        } else {
            for (size_t k = end; k > pos + 1; --k) {
                if (s[k - 1] == L' ')          { end = k - 1; next = k; break; }
                if (IsBreakAfter(s[k - 1]))    { end = k;     next = k; break; }
            }
        }
        std::wstring l = s.substr(pos, end - pos);
        while (!l.empty() && l.back() == L' ') l.pop_back();
        out.push_back(l);
        pos = next;
    }
    if (out.empty()) out.push_back(L"");
    return out;
}

// s, or "…" plus as much of its end as fits in width (for folder paths).
static std::wstring FitLeft(HDC dc, const std::wstring& s, int width) {
    SIZE z;
    GetTextExtentPoint32W(dc, s.c_str(), (int)s.size(), &z);
    if (z.cx <= width) return s;
    for (size_t k = 1; k < s.size(); ++k) {
        std::wstring t = L"\x2026" + s.substr(k);
        GetTextExtentPoint32W(dc, t.c_str(), (int)t.size(), &z);
        if (z.cx <= width) return t;
    }
    return L"\x2026";
}

// ---------- icons ----------

static BITMAPINFO Dib32(int w, int h) {
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;                          // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    return bi;
}

// px: s*s straight-alpha BGRA. Contrast-stretches the icon's own brightness range, then maps it
// to 3 shades (optionally Bayer-dithered); pixels under 50% alpha become transparent.
static IconPtr MakeIcon2(const std::vector<uint32_t>& px, int s) {
    if (s <= 0 || (int)px.size() != s * s) return nullptr;
    std::vector<int> lum(s * s, -1);
    int lo = 255, hi = 0;
    for (int i = 0; i < s * s; ++i) {
        uint32_t p = px[i];
        if ((int)(p >> 24) < 128) continue;
        int b = p & 255, g = (p >> 8) & 255, r = (p >> 16) & 255;
        int l = (r * 299 + g * 587 + b * 114) / 1000;
        lum[i] = l;
        lo = std::min(lo, l);
        hi = std::max(hi, l);
    }
    if (hi < lo) return nullptr;                         // nothing opaque
    uint32_t shade[3];
    for (int k = 0; k < 3; ++k) {
        COLORREF c = g_cfg.iconShade[k];
        shade[k] = 0xFF000000u | ((uint32_t)GetRValue(c) << 16) | ((uint32_t)GetGValue(c) << 8) | GetBValue(c);
    }
    static const int bayer[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
    BITMAPINFO bi = Dib32(s, s);
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp || !bits) return nullptr;
    uint32_t* out = (uint32_t*)bits;
    bool flat = hi - lo < 48;                            // one-colour glyph: show it in the light shade
    for (int y = 0; y < s; ++y)
        for (int x = 0; x < s; ++x) {
            int i = y * s + x;
            if (lum[i] < 0) { out[i] = 0; continue; }
            float t = flat ? 1.0f : (lum[i] - lo) / (float)(hi - lo);
            float d = g_cfg.iconDither ? (bayer[y & 3][x & 3] + 0.5f) / 16.0f : 0.5f;
            int q = std::max(0, std::min(2, (int)(t * 2.0f + d)));
            out[i] = shade[q];                           // opaque, so premultiplied == straight
        }
    GdiFlush();
    IconPtr ip = std::make_shared<Icon2>();
    ip->bmp = bmp;
    ip->size = s;
    return ip;
}

// Renders an HICON onto black and onto white; the difference recovers alpha for any icon format.
static std::vector<uint32_t> RasterHIcon(HICON hi, int s) {
    std::vector<uint32_t> out;
    if (!hi || s <= 0) return out;
    BITMAPINFO bi = Dib32(s, s);
    void *pb = nullptr, *pw = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bb = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &pb, nullptr, 0);
    HBITMAP bw = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &pw, nullptr, 0);
    if (bb && bw && pb && pw) {
        memset(pb, 0x00, (size_t)s * s * 4);
        memset(pw, 0xFF, (size_t)s * s * 4);
        HGDIOBJ old = SelectObject(dc, bb);
        DrawIconEx(dc, 0, 0, hi, s, s, 0, nullptr, DI_NORMAL);
        SelectObject(dc, bw);
        DrawIconEx(dc, 0, 0, hi, s, s, 0, nullptr, DI_NORMAL);
        SelectObject(dc, old);
        GdiFlush();
        const uint32_t* B = (const uint32_t*)pb;
        const uint32_t* Wt = (const uint32_t*)pw;
        out.resize((size_t)s * s);
        for (int i = 0; i < s * s; ++i) {
            int a = 0;
            for (int ch = 0; ch < 3; ++ch)
                a += 255 - ((int)((Wt[i] >> (8 * ch)) & 255) - (int)((B[i] >> (8 * ch)) & 255));
            a = std::max(0, std::min(255, a / 3));
            uint32_t px = (uint32_t)a << 24;
            for (int ch = 0; ch < 3; ++ch) {
                int vb = (B[i] >> (8 * ch)) & 255;
                int c = a ? std::min(255, vb * 255 / a) : 0;
                px |= (uint32_t)c << (8 * ch);
            }
            out[i] = px;
        }
    }
    if (bb) DeleteObject(bb);
    if (bw) DeleteObject(bw);
    DeleteDC(dc);
    return out;
}

// Store apps: ask the shell for the app's icon image.
static std::vector<uint32_t> RasterShellItem(const std::wstring& parse, int s) {
    std::vector<uint32_t> out;
    IShellItemImageFactory* f = nullptr;
    if (FAILED(SHCreateItemFromParsingName(parse.c_str(), nullptr, IID_IShellItemImageFactory, (void**)&f)) || !f)
        return out;
    HBITMAP hb = nullptr;
    SIZE want{s, s};
    if (SUCCEEDED(f->GetImage(want, SIIGBF_ICONONLY, &hb)) && hb) {
        BITMAP bm{};
        GetObjectW(hb, sizeof(bm), &bm);
        int w = bm.bmWidth, h = bm.bmHeight;
        if (w > 0 && h > 0) {
            std::vector<uint32_t> src((size_t)w * h);
            BITMAPINFO bi = Dib32(w, h);
            HDC sdc = GetDC(nullptr);
            int got = GetDIBits(sdc, hb, 0, h, src.data(), &bi, DIB_RGB_COLORS);
            ReleaseDC(nullptr, sdc);
            if (got > 0) {
                bool anyAlpha = false, straight = false;
                for (uint32_t p : src) {
                    int a = p >> 24;
                    if (a) anyAlpha = true;
                    if ((int)(p & 255) > a || (int)((p >> 8) & 255) > a || (int)((p >> 16) & 255) > a) straight = true;
                }
                out.resize((size_t)s * s);
                for (int y = 0; y < s; ++y)
                    for (int x = 0; x < s; ++x) {
                        uint32_t p = src[(size_t)(y * h / s) * w + (x * w / s)];
                        int a = anyAlpha ? (int)(p >> 24) : 255;
                        uint32_t px = (uint32_t)a << 24;
                        for (int ch = 0; ch < 3; ++ch) {
                            int c = (p >> (8 * ch)) & 255;
                            if (anyAlpha && !straight && a > 0) c = std::min(255, c * 255 / a);
                            px |= (uint32_t)c << (8 * ch);
                        }
                        out[(size_t)y * s + x] = px;
                    }
            }
        }
        DeleteObject(hb);
    }
    f->Release();
    return out;
}

static IconPtr IconFromFile(const std::wstring& path, int s) {
    if (path.empty()) return nullptr;
    SHFILEINFOW sfi{};
    UINT flags = SHGFI_ICON | (s > 16 ? SHGFI_LARGEICON : SHGFI_SMALLICON);
    if (!SHGetFileInfoW(path.c_str(), 0, &sfi, sizeof(sfi), flags) || !sfi.hIcon) return nullptr;
    IconPtr ip = MakeIcon2(RasterHIcon(sfi.hIcon, s), s);
    DestroyIcon(sfi.hIcon);
    return ip;
}

// Bare names like "explorer.exe" are looked up on PATH.
static std::wstring PinFilePath(const Pin& p) {
    std::wstring e = ExpandEnv(p.path);
    if (e.find(L'\\') != std::wstring::npos || e.find(L'/') != std::wstring::npos) return e;
    if (e.find(L':') != std::wstring::npos) return L"";           // a URI like ms-settings:
    wchar_t buf[MAX_PATH];
    if (SearchPathW(nullptr, e.c_str(), nullptr, MAX_PATH, buf, nullptr)) return buf;
    return Resolve(e);
}

static IconPtr PinIcon(const Pin& p, int s) {
    std::wstring key = p.aumid.empty() ? L"path:" + p.path : L"app:" + p.aumid;
    auto it = g_pinIcons.find(key);
    if (it != g_pinIcons.end()) return it->second;       // also caches "no icon"
    IconPtr ip = p.aumid.empty() ? IconFromFile(PinFilePath(p), s)
                                 : MakeIcon2(RasterShellItem(L"shell:AppsFolder\\" + p.aumid, s), s);
    g_pinIcons[key] = ip;
    return ip;
}

static HICON WindowIconHandle(HWND h, bool big) {
    const WPARAM order[3] = {big ? (WPARAM)ICON_BIG : (WPARAM)ICON_SMALL2, big ? (WPARAM)ICON_SMALL2 : (WPARAM)ICON_BIG,
                             (WPARAM)ICON_SMALL};
    for (WPARAM w : order) {
        DWORD_PTR res = 0;
        if (SendMessageTimeoutW(h, WM_GETICON, w, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 40, &res) && res) return (HICON)res;
    }
    ULONG_PTR c = GetClassLongPtrW(h, big ? GCLP_HICON : GCLP_HICONSM);
    if (!c) c = GetClassLongPtrW(h, big ? GCLP_HICONSM : GCLP_HICON);
    return (HICON)c;
}

// The window's own icon; falls back to its exe's icon and retries for a few seconds,
// since many apps set their window icon only after the window appears.
static void EnsureTaskIcon(Task& t, int s) {
    if (t.iconFinal) return;
    DWORD now = GetTickCount();
    if (t.iconTries > 0 && (LONG)(now - t.iconNext) < 0) return;
    HICON hi = WindowIconHandle(t.hwnd, s > 16);
    IconPtr ip = hi ? MakeIcon2(RasterHIcon(hi, s), s) : nullptr;    // window icons are not ours to destroy
    if (ip) {
        t.icon = ip;
        t.iconFinal = true;
        return;
    }
    if (!t.icon) t.icon = IconFromFile(t.exe, s);
    if (++t.iconTries >= 6) t.iconFinal = true;
    t.iconNext = now + 1000;
}

static void ResetIcons() {
    g_pinIcons.clear();
    for (auto& t : g_tasks) { t.icon.reset(); t.iconTries = 0; t.iconFinal = false; t.iconNext = 0; }
}

// ---------- drawing ----------

// Text lines that overlap the icon vertically are indented past it; lines below wrap under it.
static int IndentLines() {
    if (!g_cfg.icons || g_iconS <= 0) return 0;
    int iconTop = g_iconS <= g_lineH ? (g_lineH - g_iconS) / 2 : 0;
    int iconBottom = iconTop + g_iconS + g_cfg.lineGap;      // keep at least linegap below the icon
    int k = 0;
    while (k * (g_lineH + g_cfg.lineGap) < iconBottom) ++k;
    return k;
}

static int ButtonHeight(int lines) {
    return 2 + 2 * g_cfg.vpad + lines * g_lineH + (lines - 1) * g_cfg.lineGap;
}

static void BlendIcon(HDC dc, const Icon2* icon, int x, int y, int size, BYTE alpha) {
    if (!icon || !icon->bmp) return;
    if (!g_iconDC) g_iconDC = CreateCompatibleDC(nullptr);
    HGDIOBJ old = SelectObject(g_iconDC, icon->bmp);
    BLENDFUNCTION bf{AC_SRC_OVER, 0, alpha, AC_SRC_ALPHA};
    AlphaBlend(dc, x, y, size, size, g_iconDC, 0, 0, icon->size, icon->size, bf);
    SelectObject(g_iconDC, old);
}

static void DrawButton(HDC dc, RECT r, bool pressed, COLORREF border, COLORREF face, COLORREF txt,
                       const std::vector<std::wstring>& lines, const Icon2* icon = nullptr,
                       int indent = 0, BYTE iconAlpha = 255) {
    Fill(dc, r, border);                                // 1px outline
    RECT in{r.left + 1, r.top + 1, r.right - 1, r.bottom - 1};
    if (in.right <= in.left || in.bottom <= in.top) return;
    Fill(dc, in, face);

    int w = in.right - in.left, h = in.bottom - in.top;
    int lx = g_cfg.highlight ? std::min(g_cfg.highlight, w) : w;
    int ly = g_cfg.highlight ? std::min(g_cfg.highlight, h) : h;
    if (!pressed) {                                     // raised: light from top-left
        Fill(dc, RECT{in.left, in.top, in.left + lx, in.top + 1}, g_cfg.hi);
        Fill(dc, RECT{in.left, in.top, in.left + 1, in.top + ly}, g_cfg.hi);
    } else {                                            // sunken: light lands bottom-right
        Fill(dc, RECT{in.right - lx, in.bottom - 1, in.right, in.bottom}, g_cfg.hi);
        Fill(dc, RECT{in.right - 1, in.bottom - ly, in.right, in.bottom}, g_cfg.hi);
    }

    int shift = pressed ? 1 : 0;
    RECT clip{in.left + 1, in.top + 1, in.right - 1, in.bottom - 1};
    SetTextColor(dc, txt);
    int x = in.left + g_cfg.pad + shift;
    int y = in.top + g_cfg.vpad + shift;
    if (icon && icon->bmp) {
        int is = icon->size;
        int iy = is <= g_lineH ? y + (g_lineH - is) / 2 : y;     // centred on the first text line
        BlendIcon(dc, icon, x, iy, is, iconAlpha);
    }
    int beside = indent ? IndentLines() : 0;
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::wstring& l = lines[i];
        int lx2 = x + ((int)i < beside ? indent : 0);
        ExtTextOutW(dc, lx2, y, ETO_CLIPPED, &clip, l.c_str(), (UINT)l.size(), nullptr);
        y += g_lineH + g_cfg.lineGap;
    }
}

static std::wstring ClockText() {
    static const wchar_t* D[] = {L"Sun", L"Mon", L"Tue", L"Wed", L"Thu", L"Fri", L"Sat"};
    static const wchar_t* M[] = {L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun",
                                 L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec"};
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t buf[64];
    if (g_cfg.clock24)
        wsprintfW(buf, L"%02d:%02d %s %d %s", st.wHour, st.wMinute, D[st.wDayOfWeek], st.wDay, M[st.wMonth - 1]);
    else
        wsprintfW(buf, L"%d:%02d %s %s %d %s", (st.wHour + 11) % 12 + 1, st.wMinute, st.wHour < 12 ? L"AM" : L"PM",
                  D[st.wDayOfWeek], st.wDay, M[st.wMonth - 1]);
    return buf;
}

static int ContentHeight(const std::vector<Item>& v) {
    if (v.empty()) return 0;
    int h = 0;
    for (auto& it : v) h += it.h;
    return h + g_cfg.spacing * ((int)v.size() - 1);
}

static COLORREF Brighten(COLORREF c, int d) {
    auto ch = [d](int v) { return std::max(0, std::min(255, v + d)); };
    return RGB(ch(GetRValue(c)), ch(GetGValue(c)), ch(GetBValue(c)));
}

static COLORREF HoverFace(COLORREF face, bool hov) { return hov ? Brighten(face, g_cfg.hoverBright) : face; }

// The pinned section. Buttons span x0..x1; sbX >= 0 draws a scroll thumb at that x when content overflows.
static void DrawPins(HDC dc, int W, int x0, int x1, const std::vector<Item>& items, int top, int bottom,
                     int scroll, int content, int sbX) {
    if (bottom <= top) return;
    HRGN clip = CreateRectRgn(BarOX(), top, BarOX() + W, bottom);   // clip regions are in device coords
    SelectClipRgn(dc, clip);
    int y = top - scroll;
    for (auto& it : items) {
        RECT r{x0, y, x1, y + it.h};
        if (r.bottom > top && r.top < bottom) {
            bool hov = g_hoverKind == it.kind && g_hoverIdx == it.idx;
            IconPtr ic = g_cfg.icons && it.idx < PinCount() ? PinIcon(PinAt(it.idx), g_iconS) : nullptr;
            DrawButton(dc, r, false, g_cfg.outline, HoverFace(g_cfg.pinFace, hov), hov ? g_cfg.hoverText : g_cfg.text,
                       it.lines, ic.get(), g_indent);
        }
        g_rows.push_back(Row{r, it.kind, it.idx, top, bottom});
        y += it.h + g_cfg.spacing;
    }
    SelectClipRgn(dc, nullptr);
    DeleteObject(clip);

    int area = bottom - top;
    if (sbX >= 0 && g_cfg.scrollW > 0 && content > area) {
        int th = std::max(std::min(area, 8), (int)((long long)area * area / content));
        int ty = top + (int)((long long)(area - th) * scroll / std::max(1, content - area));
        Fill(dc, RECT{sbX, ty, sbX + g_cfg.scrollW, ty + th}, g_cfg.scrollColor);
    }
}

// ░▒▓█ drawn as real pixel dither patterns, stretched across the button, so it's crisp in any font.
static void DrawStartButton(HDC dc, RECT r, COLORREF border, COLORREF face, COLORREF ink) {
    DrawButton(dc, r, false, border, face, ink, {});
    RECT a{r.left + 1 + g_cfg.pad, r.top + 1 + g_cfg.vpad, r.right - 1 - g_cfg.pad, r.bottom - 1 - g_cfg.vpad};
    int w = a.right - a.left;
    if (w < 4 || a.bottom <= a.top) return;
    for (int c = 0; c < 4; ++c) {
        int x0 = a.left + w * c / 4, x1 = a.left + w * (c + 1) / 4;
        if (c == 3) { Fill(dc, RECT{x0, a.top, x1, a.bottom}, ink); continue; }
        for (int y = a.top; y < a.bottom; ++y)
            for (int x = x0; x < x1; ++x) {
                bool on = c == 0 ? (x % 2 == 0 && y % 2 == 0)       // ░ 25%
                        : c == 1 ? ((x + y) % 2 == 0)                // ▒ 50%
                        :          !(x % 2 == 1 && y % 2 == 1);      // ▓ 75%
                if (on) SetPixelV(dc, x, y, ink);
            }
    }
}

// ---------- live previews ----------
// The column is a ring: every window appears once, ordered from the base window outward (the order
// wraps, so the column never ends). The anchor card is the biggest; each step away from it in that
// order is smaller. Idle: anchor = base = focused window, centred. Hovering re-anchors on the card
// under the mouse where it already sits, without reordering; the wheel rotates anchor and base together.

static int CaptionH(int w) { return g_cfg.titleMin > 0 && w >= g_cfg.titleMin ? g_lineH + g_cfg.vpad : 0; }

// Card height at width w: outline, optional title line, padding and a picture box shaped like the window.
static int CardHeight(const Task& t, int w) {
    int boxW = std::max(4, w - 2 - 2 * g_cfg.thumbPad);
    float a = std::max(g_cfg.minAspect / 100.0f, std::min(g_cfg.maxAspect / 100.0f, t.aspect));
    int boxH = std::max(4, (int)std::lround(boxW * a));
    return 2 + CaptionH(w) + 2 * g_cfg.thumbPad + boxH;
}

static float StepScale(int d) {
    float s = 1.0f, step = g_cfg.step / 100.0f, mn = g_cfg.minScale / 100.0f;
    for (int i = 0; i < d && s > mn; ++i) s *= step;
    return std::max(mn, s);
}

// Sets every task's target geometry (tTop/tH/tW/tX, bar coordinates) from the anchor, base and column
// bounds. Cards are centred, except a hovered anchor: it keeps its outer edge and sticks out popout px
// past the bar's inner edge.
static void PvComputeTargets() {
    int N = (int)g_tasks.size();
    for (auto& t : g_tasks) t.inLayout = false;
    if (N == 0 || g_pvBottom - g_pvTop < 8 || g_W <= 0) return;
    int ai = TaskIndex(g_pvAnchor);
    if (ai < 0) {
        ai = std::max(0, TaskIndex(g_lastTaskFg));
        g_pvAnchor = g_tasks[ai].hwnd;
    }
    int bi = TaskIndex(g_pvBase);
    if (bi < 0) {
        bi = ai;
        g_pvBase = g_pvAnchor;
    }

    std::vector<int> off(N), w(N), h(N);
    for (int i = 0; i < N; ++i) {
        int o = ((i - bi) % N + N) % N;
        if (o > N / 2) o -= N;                            // half above the base, half below
        off[i] = o;
    }
    int maxW = std::max(8, g_W - 2 * g_cfg.previewMargin);
    bool pop = g_pvHover && g_cfg.popOut > 0;
    for (int i = 0; i < N; ++i) {
        w[i] = std::max(8, (int)std::lround(maxW * StepScale(std::abs(off[i] - off[ai]))));
        if (i == ai && pop) w[i] = maxW + g_cfg.previewMargin + g_cfg.popOut;
        h[i] = CardHeight(g_tasks[i], w[i]);
    }
    std::vector<int> ord(N);
    for (int i = 0; i < N; ++i) ord[i] = i;
    std::sort(ord.begin(), ord.end(), [&](int a, int b) { return off[a] < off[b]; });
    int pa = (int)(std::find(ord.begin(), ord.end(), ai) - ord.begin());

    int region = g_pvBottom - g_pvTop;
    int cy = g_pvHover ? g_pvAnchorY : g_pvTop + region / 2;
    int top = cy - h[ai] / 2;
    if (h[ai] >= region) top = g_pvTop;
    else top = std::max(g_pvTop, std::min(top, g_pvBottom - h[ai]));   // keep the big one fully in view

    auto set = [&](int i, int y) {
        Task& t = g_tasks[i];
        t.tTop = (float)y;
        t.tH = (float)h[i];
        t.tW = (float)w[i];
        if (i == ai && pop)                               // pinned to the outer margin, poking out inward
            t.tX = (float)(g_cfg.rightSide ? g_W - g_cfg.previewMargin - w[i] : g_cfg.previewMargin);
        else
            t.tX = (float)((g_W - w[i]) / 2);
        t.inLayout = true;
    };
    set(ai, top);
    int y = top + h[ai] + g_cfg.spacing;
    for (int k = pa + 1; k < N; ++k) { set(ord[k], y); y += h[ord[k]] + g_cfg.spacing; }
    y = top - g_cfg.spacing;
    for (int k = pa - 1; k >= 0; --k) { y -= h[ord[k]]; set(ord[k], y); y -= g_cfg.spacing; }
}

// One animation frame: every card moves part of the way to its target. Cards whose target jumped
// across the column (wrapping from one end to the other) snap instead of flying through the middle.
static bool PvStep() {
    float k = std::max(0.01f, std::min(1.0f, g_cfg.animSpeed / 100.0f));
    float jump = (g_pvBottom - g_pvTop) * 0.6f;
    bool moving = false;
    for (auto& t : g_tasks) {
        if (!t.inLayout) continue;
        if (!t.placed || k >= 1.0f || std::fabs(t.tTop - t.cTop) > jump) {
            t.cTop = t.tTop;
            t.cH = t.tH;
            t.cW = t.tW;
            t.cX = t.tX;
            t.placed = true;
            continue;
        }
        auto approach = [&](float& c, float tg) {
            float d = tg - c;
            if (std::fabs(d) < 0.5f) c = tg;
            else { c += d * k; moving = true; }
        };
        approach(t.cTop, t.tTop);
        approach(t.cH, t.tH);
        approach(t.cW, t.tW);
        approach(t.cX, t.tX);
    }
    return moving;
}

static void PvAnimate() {
    PvComputeTargets();
    if (g_hwnd) {
        SetTimer(g_hwnd, TIMER_ANIM, 15, nullptr);
        InvalidateRect(g_hwnd, nullptr, FALSE);
    }
}

static int PvHitTest(int x, int y) {
    if (y < g_pvTop || y >= g_pvBottom) return -1;
    for (size_t i = 0; i < g_tasks.size(); ++i) {
        const Task& t = g_tasks[i];
        if (!t.inLayout) continue;
        if (x >= t.tX && x < t.tX + t.tW && y >= t.tTop && y < t.tTop + t.tH) return (int)i;
    }
    return -1;
}

static void PvSetIdleAnchor() {
    HWND h = nullptr;
    if (TaskIndex(g_lastTaskFg) >= 0) h = g_lastTaskFg;
    else if (TaskIndex(g_pvAnchor) >= 0) h = g_pvAnchor;
    else if (!g_tasks.empty()) h = g_tasks[0].hwnd;
    g_pvAnchor = g_pvBase = h;
}

// After the window list, focus or window shapes changed. An idle column re-centres on the focused window.
static void PvSync() {
    if (!g_pvHover) PvSetIdleAnchor();
    PvAnimate();
}

static void PvEndHover() {
    KillTimer(g_hwnd, TIMER_RECENTER);
    if (!g_pvHover) return;
    g_pvHover = false;
    g_wheelAcc = 0;
    PvSync();
}

// The mouse left the column: slide back to the focused window after recenterdelay.
static void PvLeave() {
    if (!g_pvHover) return;
    if (g_cfg.recenterDelay <= 0) PvEndHover();
    else SetTimer(g_hwnd, TIMER_RECENTER, g_cfg.recenterDelay, nullptr);
}

// Mouse at (x, y) inside the column. The card under it becomes the big one, growing around its own
// centre so it stays under the mouse. Returns that task's index, or -1 over a gap.
static int PvHover(int x, int y) {
    KillTimer(g_hwnd, TIMER_RECENTER);
    int i = PvHitTest(x, y);
    if (i < 0) return -1;
    Task& t = g_tasks[i];
    if (!g_pvHover || g_pvAnchor != t.hwnd) {
        g_pvHover = true;
        g_pvAnchor = t.hwnd;
        g_pvAnchorY = (int)(t.tTop + t.tH / 2);
        PvAnimate();
        int j = PvHitTest(x, y);
        if (j >= 0) i = j;
    }
    return i;
}

// Each wheel notch rotates the ring one window: the next (or previous) window slides under the mouse
// and becomes the big one. It wraps, so it never runs out.
static void PvWheel(int delta, int y) {
    int N = (int)g_tasks.size();
    if (N < 2) return;
    g_wheelAcc += delta;
    int steps = g_wheelAcc / WHEEL_DELTA;
    if (!steps) return;
    g_wheelAcc -= steps * WHEEL_DELTA;
    int ai = TaskIndex(g_pvAnchor), bi = TaskIndex(g_pvBase);
    if (ai < 0) ai = 0;
    if (bi < 0) bi = ai;
    int move = -steps;                                    // wheel down = next window, up = previous
    ai = ((ai + move) % N + N) % N;
    bi = ((bi + move) % N + N) % N;
    g_pvAnchor = g_tasks[ai].hwnd;
    g_pvBase = g_tasks[bi].hwnd;
    g_pvHover = true;
    g_pvAnchorY = y;
    KillTimer(g_hwnd, TIMER_RECENTER);
    PvAnimate();
}

static bool ThumbEnsure(Task& t) {
    if (t.thumb) return true;
    if (t.thumbRetry && (LONG)(GetTickCount() - t.thumbRetry) < 0) return false;
    HTHUMBNAIL th = nullptr;
    if (FAILED(DwmRegisterThumbnail(g_hwnd, t.hwnd, &th)) || !th) {
        t.thumbRetry = GetTickCount() + 3000;
        return false;
    }
    t.thumb = th;
    t.thumbShown = false;
    t.thumbRetry = 0;
    UpdateGeometry(t);                                    // now DWM can tell us the source size
    return true;
}

// Places the live picture inside box, letterboxed to the window's shape and cut off at the column's
// top and bottom (DWM draws thumbnails over everything, so they must be clipped by hand).
// Returns false when DWM has nothing to show, so the caller draws a placeholder.
static bool ThumbShow(Task& t, RECT box, int clipTop, int clipBottom, BYTE opacity) {
    if (!ThumbEnsure(t)) return false;
    if (t.src.cx <= 0 || t.src.cy <= 0 || (t.iconic && t.src.cy < 48)) { ThumbHide(t); return false; }
    RECT c = t.crop;
    float cw = (float)(c.right - c.left), ch = (float)(c.bottom - c.top);
    float bw = (float)(box.right - box.left), bh = (float)(box.bottom - box.top);
    if (cw < 1 || ch < 1 || bw < 2 || bh < 2) { ThumbHide(t); return true; }
    float sc = std::min(bw / cw, bh / ch);
    float dw = cw * sc, dh = ch * sc;
    float dx = box.left + (bw - dw) / 2, dy = box.top + (bh - dh) / 2;
    float y0 = std::max(dy, (float)clipTop), y1 = std::min(dy + dh, (float)clipBottom);
    if (y1 - y0 < 1) { ThumbHide(t); return true; }

    DWM_THUMBNAIL_PROPERTIES p{};
    p.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_RECTSOURCE | DWM_TNP_VISIBLE | DWM_TNP_OPACITY |
                DWM_TNP_SOURCECLIENTAREAONLY;
    int ox = BarOX();                                     // DWM wants window coordinates
    p.rcDestination = RECT{(LONG)std::lround(dx) + ox, (LONG)std::lround(y0), (LONG)std::lround(dx + dw) + ox, (LONG)std::lround(y1)};
    p.rcSource = RECT{c.left, c.top + (LONG)std::lround((y0 - dy) / sc), c.right, c.top + (LONG)std::lround((y1 - dy) / sc)};
    if (p.rcSource.bottom <= p.rcSource.top) p.rcSource.bottom = p.rcSource.top + 1;
    p.fVisible = TRUE;
    p.opacity = opacity;
    p.fSourceClientAreaOnly = FALSE;
    if (t.thumbShown && EqualRect(&t.lastDst, &p.rcDestination) && EqualRect(&t.lastSrc, &p.rcSource) &&
        t.lastOpacity == opacity) return true;           // unchanged since the last paint
    if (FAILED(DwmUpdateThumbnailProperties(t.thumb, &p))) {
        ThumbRelease(t);
        t.thumbRetry = GetTickCount() + 3000;
        return false;
    }
    t.thumbShown = true;
    t.lastDst = p.rcDestination;
    t.lastSrc = p.rcSource;
    t.lastOpacity = opacity;
    return true;
}

// No live picture (DWM refused, or a minimized window it keeps no image of): the app's 2-bit icon,
// scaled up by whole pixels so it stays crisp.
static void DrawPlaceholder(HDC dc, RECT box, const Task& t) {
    const Icon2* ic = t.icon.get();
    if (!ic || !ic->bmp || ic->size <= 0) return;
    int bw = box.right - box.left, bh = box.bottom - box.top;
    int k = std::min(3, std::min(bw, bh) / ic->size);
    if (k < 1) return;
    int ds = ic->size * k;
    BlendIcon(dc, ic, box.left + (bw - ds) / 2, box.top + (bh - ds) / 2, ds, t.iconic ? 120 : 255);
}

static void DrawPreviews(HDC dc) {
    int top = g_pvTop, bottom = g_pvBottom, W = g_W;
    if (bottom - top < 8) { for (auto& t : g_tasks) ThumbHide(t); return; }
    if (g_tasks.empty()) {
        SetTextColor(dc, g_cfg.dim);
        RECT r{0, top, W, bottom};
        DrawTextW(dc, L"no windows", -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        return;
    }
    HRGN clip = CreateRectRgn(0, top, g_winW, bottom);  // device coords: the whole window width, pop-out included
    SelectClipRgn(dc, clip);
    HWND fg = FocusedTask();
    // The anchor is drawn last so the big (possibly popped-out) card sits over its neighbours mid-animation.
    std::vector<size_t> order;
    size_t anchorIdx = (size_t)-1;
    for (size_t i = 0; i < g_tasks.size(); ++i) {
        if (g_tasks[i].hwnd == g_pvAnchor) anchorIdx = i;
        else order.push_back(i);
    }
    if (anchorIdx != (size_t)-1) order.push_back(anchorIdx);
    for (size_t i : order) {
        Task& t = g_tasks[i];
        if (!t.inLayout || !t.placed) { ThumbHide(t); continue; }
        int w = (int)std::lround(t.cW), h = (int)std::lround(t.cH), y = (int)std::lround(t.cTop);
        int x = (int)std::lround(t.cX);
        RECT r{x, y, x + w, y + h};
        if (r.bottom <= top || r.top >= bottom || w < 6 || h < 6) { ThumbHide(t); continue; }

        bool hov = g_hoverKind == K_TASK && g_hoverIdx == (int)i;
        bool active = t.hwnd == fg && !t.iconic;
        bool blink = t.flash && g_blinkOn;
        COLORREF face = HoverFace(blink ? g_cfg.flashColor : (active ? g_cfg.activeFace : g_cfg.face), hov);
        COLORREF txt = (active || blink) ? g_cfg.activeText : hov ? g_cfg.hoverText : (t.iconic ? g_cfg.dim : g_cfg.text);
        bool lit = (g_cfg.borderMode == 1 && active) || (g_cfg.borderMode == 2 && (active || t.visible));
        DrawButton(dc, r, active, lit ? g_cfg.activeBorder : g_cfg.outline, face, txt, {});

        RECT in{r.left + 1, r.top + 1, r.right - 1, r.bottom - 1};
        int capH = CaptionH(w);
        if (in.bottom - in.top - capH - 2 * g_cfg.thumbPad < 4) capH = 0;
        if (capH) {                                      // title line: 2-bit icon + name, cut with an ellipsis
            int sh = active ? 1 : 0;
            int cx = in.left + g_cfg.pad + sh, cy = in.top + g_cfg.vpad + sh;
            if (g_cfg.icons && t.icon && t.icon->bmp) {
                int is = t.icon->size;
                BlendIcon(dc, t.icon.get(), cx, is <= g_lineH ? cy + (g_lineH - is) / 2 : cy, is, t.iconic ? 120 : 255);
                cx += is + g_cfg.iconPad;
            }
            RECT tr{cx, cy, in.right - 2, cy + g_lineH};
            if (tr.right > tr.left) {
                SetTextColor(dc, txt);
                DrawTextW(dc, t.title.c_str(), -1, &tr, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
            }
        }
        RECT box{in.left + g_cfg.thumbPad, in.top + capH + g_cfg.thumbPad, in.right - g_cfg.thumbPad, in.bottom - g_cfg.thumbPad};
        if (box.right - box.left >= 2 && box.bottom - box.top >= 2) {
            Fill(dc, box, g_cfg.bg);
            BYTE op = t.iconic ? (BYTE)g_cfg.minOpacity : 255;
            if (!ThumbShow(t, box, top, bottom, op)) DrawPlaceholder(dc, box, t);
        } else {
            ThumbHide(t);
        }
        if (g_cfg.meter && t.level > 0.001f) {
            int mx0 = r.left + 1, mx1 = r.right - 1;
            int mw = (int)((mx1 - mx0) * std::min(1.0f, t.level) + 0.5f);
            if (mw > 0) Fill(dc, RECT{mx0, r.bottom - 1 - g_cfg.meterH, mx0 + mw, r.bottom - 1}, g_cfg.meterColor);
        }
    }
    SelectClipRgn(dc, nullptr);
    DeleteObject(clip);
}

// ---------- mini terminal ----------
// A real shell (Windows PowerShell, pwsh or cmd) runs hidden with its input and output on pipes.
// Every command is followed by a marker line that prints the exit code and the current folder, so
// pbar knows when the command finished and where the shell is. Output wraps to the bar's width.

enum { TL_OUT = 0, TL_ERR = 1, TL_CMD = 2, TL_INFO = 3 };
struct TermLine { std::wstring text; int kind; bool cont = false; };   // cont: a wrapped piece of the line above

struct Term {
    HANDLE proc = nullptr, in = nullptr;
    DWORD pid = 0;
    bool started = false, running = false, booting = false;
    std::wstring cwd, marker, kind;
    std::vector<TermLine> lines;
    std::string raw[2];           // undecoded bytes of each stream's unfinished line
    std::wstring partial[2];      // that unfinished line, decoded, shown live
    std::wstring input, draft;
    int caret = 0, inputScroll = 0, histPos = -1, scroll = 0;
    std::vector<std::wstring> history;
    DWORD lastInterrupt = 0;
};
static Term g_term;
static CRITICAL_SECTION g_termCs;
static LONG g_termGen = 0;               // bumped on every (re)start; stale reader output is dropped
static std::string g_termPending[2];     // bytes the reader threads have collected (guarded by g_termCs)
static bool g_termNotified = false;
static LONG g_termExitGen = 0;
static bool g_active = false, g_caretOn = true;
static RECT g_termRect{0, 0, 0, 0};
static std::vector<TermLine> g_termVis;  // output wrapped to the panel's width
static bool g_termVisDirty = true;
static int g_termVisW = -1;

// Selection: positions are (wrapped line in g_termVis, character). Drag to select, double-click for a
// word; Ctrl+C or the right-click menu copies it, joining wrapped pieces back into their original lines.
struct TermPos { int line = 0, col = 0; };
struct TermLayout { int left = 0, outTop = 0, rows = 0, lh = 14, first = 0; };
static TermLayout g_tl;                  // where the output text sat in the last paint (bar coords)
static bool g_sel = false, g_selDrag = false;
static TermPos g_selA, g_selB;           // where the drag started, where it is now

static BOOL WINAPI IgnoreCtrl(DWORD) { return TRUE; }   // pbar itself never dies of the Ctrl+C it sends

static std::wstring ShellKind() {
    std::wstring k = Lower(Trim(g_cfg.shell));
    if (k == L"cmd" || k == L"cmd.exe") return L"cmd";
    if (k == L"pwsh" || k == L"pwsh.exe") return L"pwsh";
    return L"powershell";
}

static std::wstring ShellLabel(const std::wstring& k) {
    return k == L"cmd" ? L"cmd" : k == L"pwsh" ? L"pwsh" : L"powershell";
}

static std::wstring TermStartDir() {
    if (!g_term.cwd.empty() && IsDir(g_term.cwd)) return g_term.cwd;
    if (!g_cfg.termDir.empty()) {
        std::wstring d = Resolve(g_cfg.termDir);
        if (IsDir(d)) return d;
    }
    std::wstring home = ExpandEnv(L"%USERPROFILE%");
    if (IsDir(home)) return home;
    return g_exeDir;
}

// cmd gets the marker on the same line as the command ("cmd&call echo ..."): it can drop lines that arrive
// in the same pipe read, and %^CD% only expands once call runs, after the command. PowerShell reads its
// input properly, so there the marker is a line of its own and $? gives a real per-command result.
static std::wstring MarkerCommand() {
    if (g_term.kind == L"cmd") return L"call echo " + g_term.marker + L"0*%^CD%";
    return L"Write-Output ('" + g_term.marker + L"' + [int](-not $?) + '*' + $PWD.Path)";
}

static void TermInvalidate() {
    if (!g_hwnd) return;
    RECT r = g_termRect;
    OffsetRect(&r, BarOX(), 0);                           // bar coords -> window coords
    InvalidateRect(g_hwnd, &r, FALSE);
}

static void TermAddLine(const std::wstring& s, int kind) {
    g_term.lines.push_back(TermLine{s, kind});
    size_t cap = (size_t)g_cfg.termScrollback;
    if (g_term.lines.size() > cap + 200) {
        g_term.lines.erase(g_term.lines.begin(), g_term.lines.end() - cap);
        g_sel = false;                                    // positions shifted under it
    }
    g_termVisDirty = true;
}

static void TermClearOutput() {
    g_term.lines.clear();
    g_term.scroll = 0;
    g_sel = false;
    g_termVisDirty = true;
}

static bool PosLess(const TermPos& a, const TermPos& b) { return a.line < b.line || (a.line == b.line && a.col < b.col); }

static void SelRange(TermPos& s, TermPos& e) {
    s = g_selA;
    e = g_selB;
    if (PosLess(e, s)) std::swap(s, e);
}

// Selected text, with wrapped pieces joined back into their original lines.
static std::wstring TermSelectionText() {
    if (!g_sel || g_termVis.empty()) return L"";
    TermPos s, e;
    SelRange(s, e);
    int last = (int)g_termVis.size() - 1;
    s.line = std::max(0, std::min(s.line, last));
    e.line = std::max(0, std::min(e.line, last));
    std::wstring out;
    for (int L = s.line; L <= e.line; ++L) {
        const TermLine& v = g_termVis[L];
        int len = (int)v.text.size();
        int a = L == s.line ? std::min(s.col, len) : 0;
        int b = L == e.line ? std::min(e.col, len) : len;
        if (L > s.line && !v.cont) out += L"\r\n";
        if (b > a) out += v.text.substr(a, b - a);
    }
    return out;
}

static void TermCopySelection() {
    std::wstring s = TermSelectionText();
    if (!s.empty()) CopyToClipboard(s);
    g_sel = false;
    TermInvalidate();
}

static void TermSelectAll() {
    if (g_termVis.empty()) return;
    g_selA = TermPos{0, 0};
    g_selB = TermPos{(int)g_termVis.size() - 1, (int)g_termVis.back().text.size()};
    g_sel = true;
    TermInvalidate();
}

// The character boundary nearest to (x, y), bar coordinates; rows above or below the shown output
// map to lines scrolled out of view.
static TermPos TermPosAt(int x, int y) {
    int n = (int)g_termVis.size();
    if (n == 0 || g_tl.lh <= 0) return TermPos{};
    int dy = y - g_tl.outTop;
    int row = dy >= 0 ? dy / g_tl.lh : -1 - (-dy - 1) / g_tl.lh;
    int line = g_tl.first + row;
    if (line < 0) return TermPos{0, 0};
    if (line >= n) return TermPos{n - 1, (int)g_termVis[n - 1].text.size()};
    const std::wstring& s = g_termVis[line].text;
    int rel = x - g_tl.left;
    if (rel <= 0 || s.empty()) return TermPos{line, 0};
    std::vector<int> dx(s.size());
    SIZE sz;
    HDC dc = GetDC(g_hwnd);
    HGDIOBJ old = SelectObject(dc, g_termFont);
    GetTextExtentExPointW(dc, s.c_str(), (int)s.size(), 0, nullptr, dx.data(), &sz);
    SelectObject(dc, old);
    ReleaseDC(g_hwnd, dc);
    int col = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        int start = i ? dx[i - 1] : 0;
        if (rel > start + (dx[i] - start) / 2) col = (int)i + 1;
        else break;
    }
    return TermPos{line, col};
}

// Double-click: the run of non-spaces (a word, a path, a number) under the mouse, or the run of spaces.
static void TermSelectWord(const TermPos& p) {
    if (p.line < 0 || p.line >= (int)g_termVis.size()) return;
    const std::wstring& s = g_termVis[p.line].text;
    int n = (int)s.size();
    if (n == 0) return;
    int c = std::min(p.col, n - 1);
    if (s[c] == L' ' && c > 0 && s[c - 1] != L' ') --c;  // just past the end of a word
    bool word = s[c] != L' ';
    int a = c, b = c;
    while (a > 0 && (s[a - 1] != L' ') == word) --a;
    while (b < n && (s[b] != L' ') == word) ++b;
    g_selA = TermPos{p.line, a};
    g_selB = TermPos{p.line, b};
    g_sel = b > a;
    TermInvalidate();
}

static bool InTermOutput(int x, int y) {
    POINT pt{x, y};
    return g_cfg.terminal && PtInRect(&g_termRect, pt) && y >= g_tl.outTop && y < g_tl.outTop + g_tl.rows * g_tl.lh;
}

// Mouse drag in progress: extend the selection, scrolling when the mouse goes above or below the text.
static void TermDragTo(int x, int y) {
    int area = g_tl.rows * g_tl.lh;
    int total = (int)g_termVis.size();
    int maxScroll = std::max(0, total - g_tl.rows);
    if (y < g_tl.outTop) g_term.scroll = std::min(maxScroll, g_term.scroll + 1);
    else if (y >= g_tl.outTop + area) g_term.scroll = std::max(0, g_term.scroll - 1);
    g_tl.first = std::max(0, total - g_tl.rows - g_term.scroll);
    int cy = std::max(g_tl.outTop, std::min(y, g_tl.outTop + area - 1));
    g_selB = TermPosAt(x, cy);
    g_sel = g_selA.line != g_selB.line || g_selA.col != g_selB.col;
    TermInvalidate();
}

static void TermWrite(const std::wstring& text) {
    if (!g_term.in) return;
    std::string u = WToUtf8(text);
    DWORD done = 0;
    WriteFile(g_term.in, u.data(), (DWORD)u.size(), &done, nullptr);
}

// One line of shell output to display text: honours carriage returns (keeps what was written last),
// tries UTF-8 and falls back to the OEM code page, drops ANSI escapes and control codes, expands tabs.
static std::wstring TermDecode(std::string b) {
    while (!b.empty() && b.back() == '\r') b.pop_back();
    size_t cr = b.find_last_of('\r');
    if (cr != std::string::npos) b = b.substr(cr + 1);
    if (b.size() >= 3 && (unsigned char)b[0] == 0xEF && (unsigned char)b[1] == 0xBB && (unsigned char)b[2] == 0xBF)
        b = b.substr(3);
    std::wstring w;
    if (!b.empty()) {
        UINT cp = CP_UTF8;
        DWORD flags = MB_ERR_INVALID_CHARS;
        int n = MultiByteToWideChar(cp, flags, b.data(), (int)b.size(), nullptr, 0);
        if (n <= 0) {
            cp = CP_OEMCP;
            flags = 0;
            n = MultiByteToWideChar(cp, flags, b.data(), (int)b.size(), nullptr, 0);
        }
        if (n > 0) {
            w.resize(n);
            MultiByteToWideChar(cp, flags, b.data(), (int)b.size(), &w[0], n);
        }
    }
    std::wstring out;
    for (size_t i = 0; i < w.size(); ++i) {
        wchar_t c = w[i];
        if (c == 0x1B) {                                    // ESC [ ... final byte, or ESC ] ... BEL / ST
            if (i + 1 < w.size() && w[i + 1] == L'[') {
                i += 2;
                while (i < w.size() && !(w[i] >= 0x40 && w[i] <= 0x7E)) ++i;
            } else if (i + 1 < w.size() && w[i + 1] == L']') {
                i += 2;
                while (i < w.size() && w[i] != 7 && !(w[i] == 0x1B && i + 1 < w.size() && w[i + 1] == L'\\')) ++i;
                if (i < w.size() && w[i] == 0x1B) ++i;
            } else {
                ++i;
            }
            continue;
        }
        if (c == L'\t') { out.append(8 - out.size() % 8, L' '); continue; }
        if (c < 32 || c == 0x7F || c == 0xFEFF) continue;
        out += c;
    }
    return out;
}

// Safety net in case PowerShell prints its "PS C:\...> " prompt into the pipe: drop it from line starts.
static std::wstring StripPrompt(std::wstring s) {
    if (g_term.kind == L"cmd") return s;
    for (int guard = 0; guard < 8 && s.compare(0, 3, L"PS ") == 0; ++guard) {
        size_t gt = s.find(L"> ");
        if (gt == std::wstring::npos) {
            if (!s.empty() && s.back() == L'>') s.clear();     // a bare prompt
            break;
        }
        s = s.substr(gt + 2);
    }
    return s;
}

static void TermCommit(int stream, const std::wstring& rawText) {
    std::wstring text = StripPrompt(rawText);
    if (stream == 0 && !g_term.marker.empty()) {
        size_t m = text.find(g_term.marker);
        std::wstring rest = m == std::wstring::npos ? L"" : text.substr(m + g_term.marker.size());
        size_t star = rest.find(L'*');
        bool valid = star != std::wstring::npos && star > 0 && star < 12 &&
                     rest.find_first_not_of(L"-0123456789") == star;   // "<code>*<folder>", not an echo of the text
        if (valid) {                                      // a command finished
            std::wstring before = text.substr(0, m);
            if (!g_term.booting && !before.empty()) TermAddLine(before, TL_OUT);
            int code = _wtoi(rest.substr(0, star).c_str());
            std::wstring dir = Trim(rest.substr(star + 1));
            if (!dir.empty()) g_term.cwd = dir;
            if (!g_term.booting && code != 0) TermAddLine(L"[failed]", TL_INFO);
            g_term.booting = false;
            g_term.running = false;
            return;
        }
    }
    if (g_term.booting) return;                           // the shell's start-up chatter
    if (text.empty() && !rawText.empty()) return;         // nothing but a prompt
    TermAddLine(text, stream == 1 ? TL_ERR : TL_OUT);
}

static void TermFeed(int stream, const std::string& bytes) {
    std::string& buf = g_term.raw[stream];
    buf += bytes;
    size_t start = 0;
    for (;;) {
        size_t nl = buf.find('\n', start);
        if (nl == std::string::npos) break;
        TermCommit(stream, TermDecode(buf.substr(start, nl - start)));
        start = nl + 1;
    }
    buf.erase(0, start);
    if (buf.size() > 65536) { TermCommit(stream, TermDecode(buf)); buf.clear(); }   // a runaway line
    g_term.partial[stream] = StripPrompt(TermDecode(buf));
    g_termVisDirty = true;
}

static void TermDrain() {
    std::string got[2];
    EnterCriticalSection(&g_termCs);
    got[0].swap(g_termPending[0]);
    got[1].swap(g_termPending[1]);
    g_termNotified = false;
    LeaveCriticalSection(&g_termCs);
    for (int s = 0; s < 2; ++s)
        if (!got[s].empty()) TermFeed(s, got[s]);
    TermInvalidate();
}

struct TermPipe { HANDLE h; int stream; LONG gen; };
struct TermWait { HANDLE proc; LONG gen; };

static DWORD WINAPI TermReader(LPVOID p) {
    TermPipe* a = (TermPipe*)p;
    char buf[4096];
    DWORD n = 0;
    while (ReadFile(a->h, buf, sizeof(buf), &n, nullptr) && n > 0) {
        bool post = false;
        EnterCriticalSection(&g_termCs);
        if (a->gen == g_termGen) {
            g_termPending[a->stream].append(buf, n);
            post = !g_termNotified;
            g_termNotified = true;
        }
        LeaveCriticalSection(&g_termCs);
        if (post && !PostMessageW(g_hwnd, WM_TERMDATA, 0, 0)) {
            EnterCriticalSection(&g_termCs);
            g_termNotified = false;
            LeaveCriticalSection(&g_termCs);
        }
    }
    CloseHandle(a->h);
    delete a;
    return 0;
}

static DWORD WINAPI TermWaiter(LPVOID p) {
    TermWait* a = (TermWait*)p;
    WaitForSingleObject(a->proc, INFINITE);
    CloseHandle(a->proc);
    PostMessageW(g_hwnd, WM_TERMEXIT, (WPARAM)a->gen, 0);
    delete a;
    return 0;
}

static bool TermStart() {
    if (g_term.started) return true;
    g_term.kind = ShellKind();
    wchar_t mk[48];
    wsprintfW(mk, L"__pbar_%08lx%04lx__", (unsigned long)GetTickCount(), (unsigned long)(GetCurrentProcessId() & 0xFFFF));
    g_term.marker = mk;

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE inR = nullptr, inW = nullptr, outR = nullptr, outW = nullptr, errR = nullptr, errW = nullptr;
    bool pipes = CreatePipe(&inR, &inW, &sa, 1 << 16) && CreatePipe(&outR, &outW, &sa, 1 << 16) &&
                 CreatePipe(&errR, &errW, &sa, 1 << 16);
    if (!pipes) {
        for (HANDLE h : {inR, inW, outR, outW, errR, errW}) if (h) CloseHandle(h);
        TermAddLine(L"[couldn't create pipes]", TL_INFO);
        return false;
    }
    SetHandleInformation(inW, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(errR, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput = inR;
    si.hStdOutput = outW;
    si.hStdError = errW;
    PROCESS_INFORMATION pi{};
    std::wstring cl = g_term.kind == L"cmd" ? L"cmd.exe /D /Q"
                    : (g_term.kind == L"pwsh" ? L"pwsh.exe" : L"powershell.exe") +
                      std::wstring(L" -NoLogo -NoProfile -NoExit -Command -");
    std::wstring dir = TermStartDir();
    BOOL ok = CreateProcessW(nullptr, &cl[0], nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                             dir.empty() ? nullptr : dir.c_str(), &si, &pi);
    CloseHandle(inR);
    CloseHandle(outW);
    CloseHandle(errW);
    if (!ok) {
        CloseHandle(inW);
        CloseHandle(outR);
        CloseHandle(errR);
        TermAddLine(L"[couldn't start " + ShellLabel(g_term.kind) + L"]", TL_INFO);
        TermInvalidate();
        return false;
    }
    CloseHandle(pi.hThread);

    EnterCriticalSection(&g_termCs);
    LONG gen = ++g_termGen;
    g_termPending[0].clear();
    g_termPending[1].clear();
    g_termNotified = false;
    LeaveCriticalSection(&g_termCs);

    g_term.proc = pi.hProcess;
    g_term.pid = pi.dwProcessId;
    g_term.in = inW;
    g_term.started = true;
    g_term.booting = true;
    g_term.running = true;
    for (int s = 0; s < 2; ++s) { g_term.raw[s].clear(); g_term.partial[s].clear(); }
    if (g_term.cwd.empty()) g_term.cwd = dir;

    HANDLE t1 = CreateThread(nullptr, 0, TermReader, new TermPipe{outR, 0, gen}, 0, nullptr);
    if (t1) CloseHandle(t1);
    HANDLE t2 = CreateThread(nullptr, 0, TermReader, new TermPipe{errR, 1, gen}, 0, nullptr);
    if (t2) CloseHandle(t2);
    HANDLE dup = nullptr;
    if (DuplicateHandle(GetCurrentProcess(), pi.hProcess, GetCurrentProcess(), &dup, SYNCHRONIZE, FALSE, 0)) {
        HANDLE t3 = CreateThread(nullptr, 0, TermWaiter, new TermWait{dup, gen}, 0, nullptr);
        if (t3) CloseHandle(t3);
        else CloseHandle(dup);
    }

    // UTF-8 output where the shell allows it, then the first marker (ends the "booting" phase).
    if (g_term.kind == L"cmd") {
        TermWrite(L"chcp 65001>nul&" + MarkerCommand() + L"\r\n");
    } else {
        TermWrite(L"$ProgressPreference='SilentlyContinue'; "
                  L"[Console]::OutputEncoding = New-Object System.Text.UTF8Encoding $false\r\n");
        TermWrite(MarkerCommand() + L"\r\n");
    }
    TermInvalidate();
    return true;
}

// Kills the shell and everything still attached to its hidden console (a running ping -t, say),
// but not GUI apps it launched.
static void TermKillTree() {
    if (g_term.pid) {
        FreeConsole();
        if (AttachConsole(g_term.pid)) {
            DWORD ids[256];
            DWORD n = GetConsoleProcessList(ids, 256);
            FreeConsole();
            DWORD me = GetCurrentProcessId();
            for (DWORD i = 0; i < n && i < 256; ++i) {
                if (ids[i] == me) continue;
                if (HANDLE p = OpenProcess(PROCESS_TERMINATE, FALSE, ids[i])) { TerminateProcess(p, 1); CloseHandle(p); }
            }
        }
    }
    if (g_term.proc) TerminateProcess(g_term.proc, 1);
}

static void TermReset() {
    if (g_term.in) { CloseHandle(g_term.in); g_term.in = nullptr; }
    if (g_term.proc) { CloseHandle(g_term.proc); g_term.proc = nullptr; }
    g_term.pid = 0;
    g_term.started = g_term.running = g_term.booting = false;
    g_termVisDirty = true;
}

// Stops the shell on purpose (restart, shell switch, exit): its leftover output is discarded.
static void TermStop() {
    if (!g_term.started) return;
    EnterCriticalSection(&g_termCs);
    ++g_termGen;
    g_termPending[0].clear();
    g_termPending[1].clear();
    LeaveCriticalSection(&g_termCs);
    TermKillTree();
    for (int s = 0; s < 2; ++s) { g_term.raw[s].clear(); g_term.partial[s].clear(); }
    TermReset();
}

// The shell exited by itself (typed exit, or it crashed): flush what it said last.
static void TermOnExit() {
    for (int s = 0; s < 2; ++s) {
        if (!g_term.raw[s].empty()) TermCommit(s, TermDecode(g_term.raw[s]));
        g_term.raw[s].clear();
        g_term.partial[s].clear();
    }
    TermReset();
    TermAddLine(L"[shell exited - Enter starts a new one]", TL_INFO);
    TermInvalidate();
}

// Ctrl+C goes to everything on the shell's console, like in a real terminal. A second Ctrl+C within
// two seconds, while the command is still going, kills it and restarts the shell.
static void TermInterrupt() {
    if (!g_term.started) return;
    DWORD now = GetTickCount();
    if (g_term.running && g_term.lastInterrupt && now - g_term.lastInterrupt < 2000) {
        g_term.lastInterrupt = 0;
        TermStop();
        TermAddLine(L"[stopped - shell restarted]", TL_INFO);
        TermStart();
        return;
    }
    g_term.lastInterrupt = now;
    FreeConsole();
    if (AttachConsole(g_term.pid)) {
        GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0);
        FreeConsole();
    }
    TermAddLine(L"^C", TL_INFO);
    TermInvalidate();
}

static void TermSubmit() {
    std::wstring cmd = g_term.input;
    g_term.input.clear();
    g_term.caret = g_term.inputScroll = 0;
    g_term.histPos = -1;
    g_term.scroll = 0;
    g_sel = false;
    std::wstring t = Trim(cmd);
    if (!t.empty() && (g_term.history.empty() || g_term.history.back() != cmd)) {
        g_term.history.push_back(cmd);
        if (g_term.history.size() > 100) g_term.history.erase(g_term.history.begin());
    }
    std::wstring low = Lower(t);
    if (!g_term.running && (low == L"cls" || low == L"clear")) {
        TermClearOutput();
        return;
    }
    bool busy = g_term.started && g_term.running;
    if (busy && !g_term.partial[0].empty()) {
        // answering a prompt ("Name? "): show the answer on the prompt's line, like a terminal does
        TermAddLine(g_term.partial[0] + cmd, TL_CMD);
        g_term.raw[0].clear();
        g_term.partial[0].clear();
    } else {
        TermAddLine(L"> " + cmd, TL_CMD);
    }
    if (!TermStart()) return;
    if (g_term.kind == L"cmd") {
        // While something runs, the line is input for it (set /p, pause, a Y/N prompt): send it as is.
        if (busy) TermWrite(cmd + L"\r\n");
        else TermWrite((t.empty() ? L"" : cmd + L"&") + MarkerCommand() + L"\r\n");
    } else {
        // PowerShell: a marker after every line, so even input typed for a running command
        // ends with the shell reporting back once it's free again.
        TermWrite(cmd + L"\r\n" + MarkerCommand() + L"\r\n");
    }
    g_term.running = true;
}

static void TermPaste() {
    if (!OpenClipboard(g_hwnd)) return;
    std::wstring t;
    if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
        if (const wchar_t* p = (const wchar_t*)GlobalLock(h)) {
            t = p;
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    std::wstring clean;
    for (wchar_t c : t) {
        if (c == L'\r') continue;
        clean += (c == L'\n' || c == L'\t') ? L' ' : c;
    }
    while (!clean.empty() && clean.back() == L' ') clean.pop_back();
    g_term.input.insert(g_term.caret, clean);
    g_term.caret += (int)clean.size();
}

static std::wstring TermAllText() {
    std::wstring s;
    for (auto& l : g_term.lines) { s += l.text; s += L"\r\n"; }
    return s;
}

static void TermOpenInNotepad() {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring path = std::wstring(tmp) + L"pbar-output.txt";
    {
        std::ofstream o(path.c_str(), std::ios::binary);
        o << "\xEF\xBB\xBF" << WToUtf8(TermAllText());
    }
    std::wstring args = L"\"" + path + L"\"";
    ShellExecuteW(nullptr, L"open", L"notepad.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
}

static void ForceForeground(HWND h);

// Esc on an empty line, or the hotkey again: back to the window you were in.
static void TermLeave() {
    if (g_lastTaskFg && IsWindow(g_lastTaskFg)) ForceForeground(g_lastTaskFg);
}

static int WordLeft(const std::wstring& s, int c) {
    while (c > 0 && s[c - 1] == L' ') --c;
    while (c > 0 && s[c - 1] != L' ') --c;
    return c;
}

static int WordRight(const std::wstring& s, int c) {
    int n = (int)s.size();
    while (c < n && s[c] != L' ') ++c;
    while (c < n && s[c] == L' ') ++c;
    return c;
}

static void TermCaretReset() {
    g_caretOn = true;
    if (g_active) SetTimer(g_hwnd, TIMER_CARET, GetCaretBlinkTime(), nullptr);
}

// WM_KEYDOWN while the bar has keyboard focus. Returns true if handled.
static bool TermKey(WPARAM vk) {
    bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0;
    std::wstring& s = g_term.input;
    int& c = g_term.caret;
    switch (vk) {
    case VK_RETURN: TermSubmit(); break;
    case VK_BACK:
        if (c > 0) { int from = ctrl ? WordLeft(s, c) : c - 1; s.erase(from, c - from); c = from; }
        break;
    case VK_DELETE: if (c < (int)s.size()) s.erase(c, 1); break;
    case VK_LEFT: c = ctrl ? WordLeft(s, c) : std::max(0, c - 1); break;
    case VK_RIGHT: c = ctrl ? WordRight(s, c) : std::min((int)s.size(), c + 1); break;
    case VK_HOME: c = 0; break;
    case VK_END: c = (int)s.size(); break;
    case VK_UP:
        if (g_term.history.empty()) break;
        if (g_term.histPos < 0) { g_term.draft = s; g_term.histPos = (int)g_term.history.size(); }
        if (g_term.histPos > 0) { s = g_term.history[--g_term.histPos]; c = (int)s.size(); }
        break;
    case VK_DOWN:
        if (g_term.histPos < 0) break;
        if (++g_term.histPos >= (int)g_term.history.size()) { g_term.histPos = -1; s = g_term.draft; }
        else s = g_term.history[g_term.histPos];
        c = (int)s.size();
        break;
    case VK_PRIOR: g_term.scroll += std::max(1, g_cfg.termLines - 1); break;
    case VK_NEXT: g_term.scroll = std::max(0, g_term.scroll - std::max(1, g_cfg.termLines - 1)); break;
    case VK_ESCAPE:
        if (g_sel) g_sel = false;                          // first Esc drops a selection
        else if (!s.empty()) { s.clear(); c = 0; }
        else TermLeave();
        break;
    case VK_INSERT:
        if (ctrl) { if (g_sel) TermCopySelection(); break; }
        if (!shift) return false;
        TermPaste();
        break;
    default:
        if (ctrl && vk == 'C') {
            // Like Windows Terminal: with text selected Ctrl+C copies it; otherwise it interrupts.
            if (g_sel) TermCopySelection();
            else if (shift) break;
            else if (g_term.started && g_term.running) TermInterrupt();
            else if (!s.empty()) { TermAddLine(L"> " + s + L"^C", TL_CMD); s.clear(); c = 0; }
            break;
        }
        if (ctrl && vk == 'V') { TermPaste(); break; }
        if (ctrl && vk == 'L') { TermClearOutput(); break; }
        return false;
    }
    if (g_term.histPos >= 0 && vk != VK_UP && vk != VK_DOWN) g_term.histPos = -1;
    TermCaretReset();
    TermInvalidate();
    return true;
}

static void TermChar(wchar_t ch) {
    if (ch < 32 || ch == 127) return;                    // Enter, Backspace, Esc, Ctrl+letters: WM_KEYDOWN's job
    g_term.input.insert(g_term.input.begin() + g_term.caret, ch);
    ++g_term.caret;
    g_term.histPos = -1;
    g_sel = false;                                        // typing drops a selection, as in a terminal
    TermCaretReset();
    TermInvalidate();
}

static int TermHeight() {
    // folder line + output lines + input line, a separator, padding and outline
    return 2 + 2 * g_cfg.vpad + (g_cfg.termLines + 2) * g_termLineH + 2;
}

static void TermBuildVis(HDC dc, int width) {
    if (!g_termVisDirty && width == g_termVisW) return;
    if (width != g_termVisW) g_sel = false;              // rewrapped: old positions mean nothing now
    g_termVis.clear();
    auto add = [&](const std::wstring& s, int kind) {
        if (s.empty()) { g_termVis.push_back(TermLine{L"", kind}); return; }
        size_t pos = 0;
        while (pos < s.size()) {
            int fit = 0;
            SIZE sz;
            GetTextExtentExPointW(dc, s.c_str() + pos, (int)(s.size() - pos), width, &fit, nullptr, &sz);
            if (fit < 1) fit = 1;
            g_termVis.push_back(TermLine{s.substr(pos, fit), kind, pos > 0});
            pos += fit;
        }
    };
    for (auto& l : g_term.lines) add(l.text, l.kind);
    if (!g_term.booting)
        for (int s = 0; s < 2; ++s)
            if (!g_term.partial[s].empty() && g_term.partial[s].find(L"__pbar_") == std::wstring::npos)
                add(g_term.partial[s], s == 1 ? TL_ERR : TL_OUT);
    g_termVisDirty = false;
    g_termVisW = width;
}

static void DrawTerminal(HDC dc, RECT r) {
    bool focus = g_active;
    bool busy = g_term.started && g_term.running;
    DrawButton(dc, r, true, focus ? g_cfg.activeBorder : g_cfg.face, g_cfg.termBg, g_cfg.termText, {});
    HGDIOBJ oldFont = SelectObject(dc, g_termFont);
    RECT in{r.left + 1 + g_cfg.pad, r.top + 1 + g_cfg.vpad, r.right - 1 - g_cfg.pad, r.bottom - 1 - g_cfg.vpad};
    if (in.right - in.left < 8 || in.bottom - in.top < g_termLineH) { SelectObject(dc, oldFont); return; }
    int lh = g_termLineH;
    int rows = std::max(0, std::min(g_cfg.termLines, (int)(in.bottom - in.top - 2) / lh - 2));
    int sbw = g_cfg.scrollW ? g_cfg.scrollW + 1 : 0;
    int width = std::max(8, (int)(in.right - in.left) - sbw);

    // folder line: where the shell is, cut from the left; blue while a command runs
    std::wstring head = FitLeft(dc, g_term.cwd.empty() ? TermStartDir() : g_term.cwd, in.right - in.left);
    SetTextColor(dc, busy ? g_cfg.meterColor : g_cfg.termDim);
    RECT hr{in.left, in.top, in.right, in.top + lh};
    ExtTextOutW(dc, in.left, in.top, ETO_CLIPPED, &hr, head.c_str(), (UINT)head.size(), nullptr);

    // output, newest at the bottom of what's shown; the wheel / PgUp scroll back
    TermBuildVis(dc, width);
    int outTop = in.top + lh;
    int total = (int)g_termVis.size();
    int maxScroll = std::max(0, total - rows);
    g_term.scroll = std::max(0, std::min(g_term.scroll, maxScroll));
    int first = std::max(0, total - rows - g_term.scroll);
    g_tl = TermLayout{(int)in.left, outTop, rows, lh, first};
    RECT oclip{in.left, outTop, in.left + width, outTop + rows * lh};
    TermPos ss, se;
    if (g_sel) SelRange(ss, se);
    for (int k = 0; k < rows && first + k < total; ++k) {
        int L = first + k, y = outTop + k * lh;
        const TermLine& v = g_termVis[L];
        if (g_sel && L >= ss.line && L <= se.line) {      // selection highlight behind the text
            int len = (int)v.text.size();
            int a = L == ss.line ? std::min(ss.col, len) : 0;
            int b = L == se.line ? std::min(se.col, len) : len;
            SIZE za{0, 0}, zb{0, 0};
            GetTextExtentPoint32W(dc, v.text.c_str(), a, &za);
            GetTextExtentPoint32W(dc, v.text.c_str(), b, &zb);
            int x0 = in.left + za.cx, x1 = in.left + zb.cx;
            if (L != se.line) x1 = in.left + width;      // the line break is selected too
            if (x1 > x0) Fill(dc, RECT{x0, y, x1, y + lh}, g_cfg.termSel);
        }
        SetTextColor(dc, v.kind == TL_ERR ? g_cfg.termErr : v.kind == TL_CMD ? g_cfg.termCmd
                       : v.kind == TL_INFO ? g_cfg.termDim : g_cfg.termText);
        ExtTextOutW(dc, in.left, y, ETO_CLIPPED, &oclip, v.text.c_str(), (UINT)v.text.size(), nullptr);
    }
    if (g_cfg.scrollW > 0 && total > rows && rows > 0) {
        int area = rows * lh;
        int th = std::max(std::min(area, 6), area * rows / total);
        int ty = outTop + (area - th) * (maxScroll - g_term.scroll) / std::max(1, maxScroll);
        Fill(dc, RECT{in.right - g_cfg.scrollW, ty, in.right, ty + th}, g_cfg.scrollColor);
    }

    // separator, then the input line
    int sepY = outTop + rows * lh;
    Fill(dc, RECT{in.left, sepY, in.right, sepY + 1}, g_cfg.face);
    int iy = sepY + 2;
    RECT ir{in.left, iy, in.right, iy + lh};
    SetTextColor(dc, busy ? g_cfg.meterColor : g_cfg.termCmd);
    ExtTextOutW(dc, in.left, iy, ETO_CLIPPED, &ir, L">", 1, nullptr);
    SIZE psz;
    GetTextExtentPoint32W(dc, L"> ", 2, &psz);
    int tx = in.left + psz.cx, avail = std::max(4, (int)in.right - tx - 2);
    std::wstring& s = g_term.input;
    int& sc = g_term.inputScroll;
    sc = std::max(0, std::min(sc, (int)s.size()));
    if (g_term.caret < sc) sc = g_term.caret;
    for (;;) {                                            // scroll the line so the caret stays in view
        SIZE z;
        GetTextExtentPoint32W(dc, s.c_str() + sc, g_term.caret - sc, &z);
        if (z.cx <= avail || sc >= g_term.caret) break;
        ++sc;
    }
    RECT tr{tx, iy, in.right, iy + lh};
    if (s.empty() && !focus) {                            // hint: which shell you'll get
        std::wstring hint = ShellLabel(g_term.started ? g_term.kind : ShellKind());
        SetTextColor(dc, g_cfg.termDim);
        ExtTextOutW(dc, tx, iy, ETO_CLIPPED, &tr, hint.c_str(), (UINT)hint.size(), nullptr);
    } else {
        SetTextColor(dc, g_cfg.termCmd);
        ExtTextOutW(dc, tx, iy, ETO_CLIPPED, &tr, s.c_str() + sc, (UINT)(s.size() - sc), nullptr);
    }
    if (focus && g_caretOn) {
        SIZE z;
        GetTextExtentPoint32W(dc, s.c_str() + sc, g_term.caret - sc, &z);
        int cx = std::min((int)in.right - 2, tx + (int)z.cx);
        Fill(dc, RECT{cx, iy, cx + 2, iy + lh}, g_cfg.termCmd);
    }
    SelectObject(dc, oldFont);
}

// ---------- smartwatch clock ----------
// A watch face in the clock slot: a graphite case, a black "OLED" screen, the hour stacked over the
// minutes in big digits, the day and date, and three activity-style rings for seconds (sweeping),
// minutes and hours. Shapes are drawn with GDI+ so the curves are antialiased. It lives in a small
// child window so the seconds ring can sweep without repainting the rest of the bar; mouse input
// passes straight through to the bar (HTTRANSPARENT), so click, hover and right-click behave like the old clock.

#define TIMER_WATCH 11

static HWND g_watchHwnd = nullptr;
static ULONG_PTR g_gdipToken = 0;
static HFONT g_watchBig = nullptr, g_watchSmall = nullptr;
static int g_watchFontW = 0, g_watchFontH = 0;
static RECT g_watchRect{0, 0, 0, 0};              // where it sits, window coords

static const wchar_t* const kWatchDays[] = {L"SUN", L"MON", L"TUE", L"WED", L"THU", L"FRI", L"SAT"};
static const wchar_t* const kWatchMonths[] = {L"JAN", L"FEB", L"MAR", L"APR", L"MAY", L"JUN",
                                              L"JUL", L"AUG", L"SEP", L"OCT", L"NOV", L"DEC"};

// Where a font's ink really lands: rows above and below the baseline, and its widest character.
struct InkBox {
    int above = 0, below = 0;
    int h() const { return above + below; }
};
static InkBox g_bigInk, g_smallInk;

// The watch's layout for its current size, worked out once when the fonts are fitted.
struct WatchLayout {
    float bx = 0, by = 0, bw = 0, bh = 0, br = 0;       // case
    float sx = 0, sy = 0, sw = 0, sh = 0, sr = 0;       // screen
    float pad = 0, gap = 0, spacing = 0;                // side padding, hour-minutes gap, text-ring gap
    float colL = 0, digitsW = 0, digitsTop = 0;         // the digits' column and the top of the hour's ink
    float col2L = 0, col2R = 0, cx = 0, cy = 0, rOuter = 0;   // date, rings and AM/PM column
    bool side = false, rings = false;
};
static WatchLayout g_wl;

static Gdiplus::Color GpColor(COLORREF c, BYTE a = 255) {
    return Gdiplus::Color(a, GetRValue(c), GetGValue(c), GetBValue(c));
}

static void AddRoundRect(Gdiplus::GraphicsPath& p, float x, float y, float w, float h, float r) {
    float d = std::min(2 * r, std::min(w, h));
    p.AddArc(x, y, d, d, 180, 90);
    p.AddArc(x + w - d, y, d, d, 270, 90);
    p.AddArc(x + w - d, y + h - d, d, d, 0, 90);
    p.AddArc(x, y + h - d, d, d, 90, 90);
    p.CloseFigure();
}

static int CALLBACK FontFoundProc(const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM p) {
    *(bool*)p = true;
    return 0;
}

static bool FontInstalled(const std::wstring& face) {
    if (face.empty() || face.size() >= LF_FACESIZE) return false;
    LOGFONTW lf{};
    lf.lfCharSet = DEFAULT_CHARSET;
    wcscpy(lf.lfFaceName, face.c_str());
    bool found = false;
    HDC dc = GetDC(nullptr);
    EnumFontFamiliesExW(dc, &lf, FontFoundProc, (LPARAM)&found, 0);
    ReleaseDC(nullptr, dc);
    return found;
}

// The configured face if it's installed (Bahnschrift ships with Windows 10 and 11), else a sensible fallback.
static std::wstring WatchFace() {
    for (const std::wstring& f : {g_cfg.watchFont, std::wstring(L"Bahnschrift SemiBold"), std::wstring(L"Bahnschrift"),
                                  std::wstring(L"Segoe UI Semibold"), std::wstring(L"Segoe UI")})
        if (FontInstalled(f)) return f;
    return L"Arial";
}

static HFONT MakeWatchFont(const std::wstring& face, int em) {
    return CreateFontW(-em, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
                       CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, face.c_str());
}

// Draws chars once and scans the pixels for ink, so sizing never depends on a font's reported metrics.
// (Windows doesn't fill in a font's cap height, and estimating it instead made the digits about a third
// too big: they overlapped and ran off the top of the screen.)
static InkBox MeasureInk(HFONT f, const wchar_t* chars) {
    InkBox r;
    HDC dc = CreateCompatibleDC(nullptr);
    HGDIOBJ oldF = SelectObject(dc, f);
    TEXTMETRICW tm;
    GetTextMetricsW(dc, &tm);
    int n = (int)wcslen(chars);
    SIZE sz;
    GetTextExtentPoint32W(dc, chars, n, &sz);
    int m = tm.tmHeight / 2 + 4;                          // room for ink that strays past the font's own metrics
    int W = sz.cx + 2 * m, H = tm.tmHeight + 2 * m;
    BITMAPINFO bi = Dib32(W, H);
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bmp && bits) {
        HGDIOBJ oldB = SelectObject(dc, bmp);
        memset(bits, 0, (size_t)W * H * 4);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        SetTextAlign(dc, TA_LEFT | TA_BASELINE);
        int base = m + tm.tmAscent;
        ExtTextOutW(dc, m, base, 0, nullptr, chars, (UINT)n, nullptr);
        GdiFlush();
        const uint32_t* px = (const uint32_t*)bits;
        int top = -1, bottom = -1;
        for (int y = 0; y < H; ++y) {
            const uint32_t* row = px + (size_t)y * W;
            for (int x = 0; x < W; ++x) {
                uint32_t c = row[x];
                if ((c & 0xFF) > 40 || ((c >> 8) & 0xFF) > 40 || ((c >> 16) & 0xFF) > 40) {   // faint fringes don't count
                    if (top < 0) top = y;
                    bottom = y;
                    break;
                }
            }
        }
        if (top >= 0) {
            r.above = base - top;
            r.below = std::max(0, bottom + 1 - base);
        }
        SelectObject(dc, oldB);
    }
    if (bmp) DeleteObject(bmp);
    if (r.above <= 0) r.above = std::max(1, (int)tm.tmAscent);   // nothing rendered: assume the worst
    SelectObject(dc, oldF);
    DeleteDC(dc);
    return r;
}

static int MaxExtent(HFONT f, const std::vector<std::wstring>& strs) {
    HDC dc = CreateCompatibleDC(nullptr);
    HGDIOBJ old = SelectObject(dc, f);
    int w = 0;
    for (auto& s : strs) {
        SIZE z;
        GetTextExtentPoint32W(dc, s.c_str(), (int)s.size(), &z);
        w = std::max(w, (int)z.cx);
    }
    SelectObject(dc, old);
    DeleteDC(dc);
    return w;
}

// The biggest font whose ink for inkChars is at most maxH tall and whose widest string in widths is at
// most maxW wide: a scaled first guess, then measured and stepped until it's the largest that fits.
static HFONT FitWatchFont(const std::wstring& face, const wchar_t* inkChars, const std::vector<std::wstring>& widths,
                          float maxH, float maxW, InkBox& ink) {
    auto fits = [&](int em, InkBox& out) {
        HFONT f = MakeWatchFont(face, em);
        out = MeasureInk(f, inkChars);
        bool ok = out.h() <= maxH && MaxExtent(f, widths) <= maxW;
        DeleteObject(f);
        return ok;
    };
    HFONT probe = MakeWatchFont(face, 100);
    InkBox p = MeasureInk(probe, inkChars);
    int pw = std::max(1, MaxExtent(probe, widths));
    DeleteObject(probe);
    int em = std::max(6, (int)(100.0f * std::min(maxH / std::max(1, p.h()), maxW / pw)));
    InkBox b;
    int guard = 0;
    while (em > 6 && !fits(em, b) && ++guard < 60) --em;          // too big: shrink until it fits
    InkBox up;
    for (int k = 0; k < 3 && fits(em + 1, up); ++k) { ++em; b = up; }   // rounding left room: grow
    if (em <= 6) fits(em, b);
    ink = b;
    return MakeWatchFont(face, em);
}

static void WatchResetFonts() {
    if (g_watchBig) DeleteObject(g_watchBig);
    if (g_watchSmall) DeleteObject(g_watchSmall);
    g_watchBig = g_watchSmall = nullptr;
    g_watchFontW = g_watchFontH = 0;
}

// How far a rounded corner of radius r cuts in from the side at depth d from the top (or bottom) edge.
static float CornerInset(float r, float d) {
    if (d >= r) return 0;
    float k = r - d;
    return r - std::sqrt(std::max(0.0f, r * r - k * k));
}

// Fits the fonts to a w x h watch and works out where everything goes. The hour and minutes take two
// equal lines that fill the screen's height inside its margins; beside them, when the bar is wide
// enough, a column holds the day and date, the rings and AM/PM. The column's text is sized to fit it
// (clear of the screen's rounded corners) and the rings take whatever height is left between.
static void WatchEnsureFonts(int w, int h) {
    if (g_watchBig && g_watchFontW == w && g_watchFontH == h) return;
    WatchResetFonts();
    std::wstring face = WatchFace();
    WatchLayout L;
    L.bx = 2.5f; L.by = 1.5f; L.bw = w - 5.0f; L.bh = h - 3.0f;
    L.br = std::min(L.bh * 0.28f, 28.0f);
    L.sx = L.bx + 4; L.sy = L.by + 4; L.sw = L.bw - 8; L.sh = L.bh - 8;
    L.sr = std::max(3.0f, L.br - 4);
    L.pad = std::max(4.0f, L.sw * 0.05f);
    L.gap = std::max(2.0f, L.sh * 0.06f);
    L.spacing = std::max(2.0f, L.sh * 0.025f);
    L.side = L.sw >= 96;
    float margin = std::max(3.0f, L.sh * 0.09f);           // above the hour and below the minutes
    float edge = std::max(3.0f, L.sh * 0.06f);             // above the date and below AM/PM

    // Digits: the side column always keeps at least 40% of the width (0.46 + 2.8 x 5% padding leaves it).
    std::vector<std::wstring> pairs;
    for (wchar_t d = L'0'; d <= L'9'; ++d) pairs.push_back(std::wstring(2, d));
    float digitMaxH = std::max(4.0f, (L.sh - 2 * margin - L.gap) / 2);
    float digitMaxW = L.side ? L.sw * 0.46f : L.sw - 2 * L.pad;
    g_watchBig = FitWatchFont(face, L"0123456789", pairs, digitMaxH, digitMaxW, g_bigInk);
    L.digitsW = (float)MaxExtent(g_watchBig, pairs);
    L.digitsTop = L.sy + (L.sh - (2.0f * g_bigInk.h() + L.gap)) / 2;
    L.colL = L.side ? L.sx + L.pad : L.sx + (L.sw - L.digitsW) / 2;
    L.col2L = L.colL + L.digitsW + L.pad * 0.8f;
    L.col2R = L.sx + L.sw - L.pad;

    if (L.side) {
        std::vector<std::wstring> words = {L"AM", L"PM"};
        for (auto m : kWatchMonths) words.push_back(m);
        for (auto d : kWatchDays)
            for (int n = 1; n <= 31; ++n) words.push_back(std::wstring(d) + L" " + std::to_wstring(n));
        float col2W = L.col2R - L.col2L;
        L.cx = (L.col2L + L.col2R) / 2;
        L.cy = L.sy + L.sh / 2;
        // The date's top row (and AM/PM's bottom row) can reach the rounded corners on the right.
        float right = L.sx + L.sw - std::max(L.pad, CornerInset(L.sr, edge) + 1.5f);
        float smallMaxW = std::min(col2W, 2 * (right - L.cx));
        float smallMaxH = std::min(std::max(7.0f, L.sh * 0.13f), (L.sh - 2 * edge - 2 * L.spacing) / 2);
        g_watchSmall = FitWatchFont(face, L"ABCDEFGHIJLMNOPRSTUVWY0123456789", words, std::max(4.0f, smallMaxH),
                                    smallMaxW, g_smallInk);
        L.rOuter = std::max(0.0f, std::min(col2W / 2, L.sh / 2 - edge - L.spacing - g_smallInk.h()));
        L.rings = L.rOuter > 8;
    }
    g_wl = L;
    g_watchFontW = w;
    g_watchFontH = h;
}

// Draws the watch at the size WatchEnsureFonts last fitted (the watch window's client size).
static void DrawWatch(HDC dc) {
    using namespace Gdiplus;
    const WatchLayout& L = g_wl;
    SYSTEMTIME st;
    GetLocalTime(&st);
    bool hov = g_hoverKind == K_CLOCK;

    {
        Graphics g(dc);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(PixelOffsetModeHalf);

        // case: graphite, lit from above; the rim picks up the accent colour on hover
        GraphicsPath body;
        AddRoundRect(body, L.bx, L.by, L.bw, L.bh, L.br);
        LinearGradientBrush caseFill(PointF(0, L.by - 1), PointF(0, L.by + L.bh + 1), Color(255, 78, 78, 84), Color(255, 20, 20, 23));
        g.FillPath(&caseFill, &body);
        Pen rim(hov ? GpColor(g_cfg.watchAccent) : Color(255, 132, 132, 140), 1.0f);
        g.DrawPath(&rim, &body);

        GraphicsPath screen;
        AddRoundRect(screen, L.sx, L.sy, L.sw, L.sh, L.sr);
        SolidBrush black(Color(255, 0, 0, 0));
        g.FillPath(&black, &screen);

        // rings, outside in: seconds (sweeping), minutes, hours; each over a dim track
        if (L.side && L.rings) {
            float secs = st.wSecond + st.wMilliseconds / 1000.0f;
            float mins = st.wMinute + secs / 60.0f;
            float hrs = g_cfg.clock24 ? (st.wHour + mins / 60.0f) / 24.0f : ((st.wHour % 12) + mins / 60.0f) / 12.0f;
            float frac[3] = {secs / 60.0f, mins / 60.0f, hrs};
            float stroke = std::max(2.5f, L.rOuter * 0.21f);
            float step = stroke + std::max(1.0f, stroke * 0.3f);
            for (int k = 0; k < 3; ++k) {
                float r = L.rOuter - stroke / 2 - k * step;
                if (r < stroke * 0.75f) break;
                RectF rr(L.cx - r, L.cy - r, 2 * r, 2 * r);
                Pen track(GpColor(g_cfg.watchRing[k], 56), stroke);
                g.DrawEllipse(&track, rr);
                if (frac[k] > 0.003f) {
                    Pen arc(GpColor(g_cfg.watchRing[k]), stroke);
                    arc.SetStartCap(LineCapRound);
                    arc.SetEndCap(LineCapRound);
                    g.DrawArc(&arc, rr, -90.0f, std::min(359.9f, frac[k] * 360.0f));
                }
            }
        }

        // faint glass sheen over the top of the screen
        g.SetClip(RectF(L.sx, L.sy, L.sw, L.sh * 0.42f));
        LinearGradientBrush sheen(PointF(0, L.sy - 1), PointF(0, L.sy + L.sh * 0.42f + 1), Color(26, 255, 255, 255), Color(0, 255, 255, 255));
        g.FillPath(&sheen, &screen);
        g.ResetClip();
    }

    // Text (GDI, antialiased) on top, placed by its measured ink and clipped to the screen so nothing
    // can ever spill onto the case.
    int sr2 = (int)std::lround(2 * L.sr);
    HRGN clip = CreateRoundRectRgn((int)std::floor(L.sx), (int)std::floor(L.sy), (int)std::ceil(L.sx + L.sw) + 1,
                                   (int)std::ceil(L.sy + L.sh) + 1, sr2, sr2);
    SelectClipRgn(dc, clip);
    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ oldFont = SelectObject(dc, g_watchBig);

    // hour over minutes, centred in their column
    int hour = g_cfg.clock24 ? st.wHour : (st.wHour + 11) % 12 + 1;
    wchar_t hh[8], mm[8];
    wsprintfW(hh, L"%02d", hour);
    wsprintfW(mm, L"%02d", st.wMinute);
    int dx = (int)std::lround(L.colL + L.digitsW / 2);
    SetTextAlign(dc, TA_CENTER | TA_BASELINE);
    SetTextColor(dc, RGB(255, 255, 255));
    ExtTextOutW(dc, dx, (int)std::lround(L.digitsTop + g_bigInk.above), 0, nullptr, hh, (UINT)wcslen(hh), nullptr);
    SetTextColor(dc, g_cfg.watchAccent);
    ExtTextOutW(dc, dx, (int)std::lround(L.digitsTop + g_bigInk.h() + L.gap + g_bigInk.above), 0, nullptr, mm,
                (UINT)wcslen(mm), nullptr);

    if (L.side && g_watchSmall) {
        SelectObject(dc, g_watchSmall);
        // day name (day colour) and date (white), centred together above the rings
        std::wstring day = kWatchDays[st.wDayOfWeek % 7], date = L" " + std::to_wstring(st.wDay);
        SIZE a, b;
        GetTextExtentPoint32W(dc, day.c_str(), (int)day.size(), &a);
        GetTextExtentPoint32W(dc, date.c_str(), (int)date.size(), &b);
        int x0 = (int)std::lround(L.cx - (a.cx + b.cx) / 2.0f);
        int base = (int)std::lround(L.cy - L.rOuter - L.spacing - g_smallInk.below);
        SetTextAlign(dc, TA_LEFT | TA_BASELINE);
        SetTextColor(dc, g_cfg.watchDay);
        ExtTextOutW(dc, x0, base, 0, nullptr, day.c_str(), (UINT)day.size(), nullptr);
        SetTextColor(dc, RGB(255, 255, 255));
        ExtTextOutW(dc, x0 + a.cx, base, 0, nullptr, date.c_str(), (UINT)date.size(), nullptr);
        // AM/PM under the rings (the month, in 24-hour mode)
        std::wstring low = g_cfg.clock24 ? kWatchMonths[(st.wMonth + 11) % 12] : (st.wHour < 12 ? L"AM" : L"PM");
        SetTextAlign(dc, TA_CENTER | TA_BASELINE);
        SetTextColor(dc, RGB(150, 150, 160));
        ExtTextOutW(dc, (int)std::lround(L.cx), (int)std::lround(L.cy + L.rOuter + L.spacing + g_smallInk.above), 0,
                    nullptr, low.c_str(), (UINT)low.size(), nullptr);
    }
    SetTextAlign(dc, TA_LEFT | TA_TOP);
    SelectObject(dc, oldFont);
    SelectClipRgn(dc, nullptr);
    DeleteObject(clip);
}

static void WatchPaint(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    RECT cr;
    GetClientRect(hwnd, &cr);
    if (cr.right >= 40 && cr.bottom >= 30) {
        HDC dc = CreateCompatibleDC(hdc);
        HBITMAP bmp = CreateCompatibleBitmap(hdc, cr.right, cr.bottom);
        HGDIOBJ oldBmp = SelectObject(dc, bmp);
        Fill(dc, cr, g_cfg.bg);
        WatchEnsureFonts(cr.right, cr.bottom);
        DrawWatch(dc);
        BitBlt(hdc, 0, 0, cr.right, cr.bottom, dc, 0, 0, SRCCOPY);
        SelectObject(dc, oldBmp);
        DeleteObject(bmp);
        DeleteDC(dc);
    }
    EndPaint(hwnd, &ps);
}

// Redraw rate: watchfps for a smooth sweep, or once a second on the second (watchfps=0).
static void WatchArm() {
    if (!g_watchHwnd || !IsWindowVisible(g_watchHwnd)) return;
    UINT ms;
    if (g_cfg.watchFps > 0) {
        ms = 1000 / g_cfg.watchFps;
    } else {
        SYSTEMTIME st;
        GetLocalTime(&st);
        ms = 1000 - st.wMilliseconds + 2;
    }
    SetTimer(g_watchHwnd, TIMER_WATCH, ms, nullptr);
}

static LRESULT CALLBACK WatchProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_NCHITTEST: return HTTRANSPARENT;           // the bar underneath handles the mouse
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: WatchPaint(h); return 0;
    case WM_TIMER:
        if (wp == TIMER_WATCH) {
            if (g_cfg.watchFps <= 0) WatchArm();         // stay aligned to the second
            if (!g_zBottom) InvalidateRect(h, nullptr, FALSE);   // nothing to redraw behind a fullscreen game
        }
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// Shows the watch over r (bar coordinates), or hides it when r is null.
static void WatchPlace(const RECT* r) {
    if (!g_watchHwnd) return;
    if (!r) {
        if (IsWindowVisible(g_watchHwnd)) {
            KillTimer(g_watchHwnd, TIMER_WATCH);
            ShowWindow(g_watchHwnd, SW_HIDE);
        }
        return;
    }
    RECT wr = *r;
    OffsetRect(&wr, BarOX(), 0);
    if (EqualRect(&wr, &g_watchRect) && IsWindowVisible(g_watchHwnd)) return;
    g_watchRect = wr;
    SetWindowPos(g_watchHwnd, HWND_TOP, wr.left, wr.top, wr.right - wr.left, wr.bottom - wr.top,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    WatchArm();
}

// ---------- paint ----------

static void Paint(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    RECT cr;
    GetClientRect(hwnd, &cr);
    // The window is the bar plus the pop-out strip. Everything below draws in bar coordinates
    // (0..W); the viewport origin shifts it into place when the strip is on the left.
    int winW = cr.right, H = cr.bottom;
    int W = std::max(20, winW - g_cfg.popOut);
    g_winW = winW;

    HDC mdc = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, winW, H);
    HGDIOBJ oldBmp = SelectObject(mdc, bmp);
    HGDIOBJ oldFont = SelectObject(mdc, g_font);
    SetBkMode(mdc, TRANSPARENT);
    Fill(mdc, cr, g_cfg.bg);
    SetViewportOrgEx(mdc, BarOX(), 0, nullptr);

    TEXTMETRICW tm;
    GetTextMetricsW(mdc, &tm);
    g_lineH = tm.tmHeight;
    SelectObject(mdc, g_termFont);
    GetTextMetricsW(mdc, &tm);
    g_termLineH = tm.tmHeight;
    SelectObject(mdc, g_font);
    int textW = std::max(1, W - 2 - 2 * g_cfg.pad - 1);   // -1 leaves room for the pressed shift

    // Icons: size follows the text line unless set; text is indented past the icon on every line.
    int iconS = g_cfg.iconSize ? g_cfg.iconSize : g_lineH;
    if (g_cfg.icons && iconS != g_iconS) ResetIcons();
    g_iconS = iconS;
    g_indent = g_cfg.icons ? iconS + g_cfg.iconPad : 0;
    int minH = g_cfg.icons ? 2 + 2 * g_cfg.vpad + iconS : 0;
    if (g_cfg.icons) for (auto& t : g_tasks) EnsureTaskIcon(t, iconS);

    auto buildPins = [&](int bw) {
        std::vector<Item> v;
        int fullW = std::max(1, bw - 2 - 2 * g_cfg.pad - 1);
        int tw = std::max(1, fullW - g_indent);
        int beside = g_indent ? IndentLines() : 0;
        int n = PinCount();
        for (int i = 0; i < n; ++i) {
            auto lines = Wrap(mdc, PinAt(i).name, fullW, g_cfg.maxLines, tw, beside);
            v.push_back(Item{K_PIN, i, lines, std::max(ButtonHeight((int)lines.size()), minH)});
        }
        return v;
    };
    int sbW = g_cfg.scrollW, sbGap = sbW ? 1 : 0;
    int narrowW = std::max(4, W - sbW - sbGap);

    // Clock: the smartwatch face (its own child window), or the classic text button with its own
    // font size that wraps if it doesn't fit the bar width.
    bool watch = g_cfg.clock && g_cfg.watch;
    std::vector<std::wstring> clockLines;
    int clockLineH = g_lineH;
    if (g_cfg.clock && !watch) {
        SelectObject(mdc, g_clockFont);
        TEXTMETRICW ctm;
        GetTextMetricsW(mdc, &ctm);
        clockLineH = ctm.tmHeight;
        clockLines = Wrap(mdc, ClockText(), textW, g_cfg.clockLines);
        SelectObject(mdc, g_font);
    }

    // Layout, top to bottom:
    // [terminal] gap [live previews ... wrap forever] gap [pinned, anchored low] gap [clock] spacing [start]
    int startH = g_cfg.startButton ? (g_cfg.startHeight ? g_cfg.startHeight : ButtonHeight(g_cfg.startLines)) : 0;
    int startTop = H - startH;
    int clockH = !g_cfg.clock ? 0
               : watch ? std::min(g_cfg.watchHeight, std::max(40, H / 3))
               : 2 + 2 * g_cfg.vpad + (int)clockLines.size() * clockLineH + ((int)clockLines.size() - 1) * g_cfg.lineGap;
    int clockTop = (g_cfg.startButton ? startTop - g_cfg.spacing : H) - clockH;
    int bottomLimit = g_cfg.clock ? clockTop - g_cfg.gap
                    : g_cfg.startButton ? startTop - g_cfg.gap : H;

    int termH = g_cfg.terminal ? std::min(TermHeight(), std::max(0, bottomLimit / 2)) : 0;
    g_termRect = RECT{0, 0, W, termH};
    int contentTop = termH ? termH + g_cfg.gap : 0;

    // A pinned section that overflows is rewrapped a little narrower to make room for its scroll indicator.
    int pinsMaxArea = std::max(0, bottomLimit - contentTop) * g_cfg.pinsMax / 100;
    std::vector<Item> pins = buildPins(W);
    bool pinsScroll = sbW > 0 && ContentHeight(pins) > pinsMaxArea;
    if (pinsScroll) pins = buildPins(narrowW);
    int pinsContent = ContentHeight(pins);
    int pinsArea = pins.empty() ? 0 : std::min(pinsContent, pinsMaxArea);
    g_pinsBottom = bottomLimit;
    g_pinsTop = bottomLimit - pinsArea;
    g_scrollPins = std::max(0, std::min(g_scrollPins, pinsContent - pinsArea));

    g_W = W;
    g_pvTop = contentTop;
    g_pvBottom = std::max(contentTop, pins.empty() ? bottomLimit : g_pinsTop - g_cfg.gap);
    PvComputeTargets();
    bool needAnim = false;
    for (auto& t : g_tasks) {
        if (!t.inLayout) continue;
        if (!t.placed) { t.cTop = t.tTop; t.cH = t.tH; t.cW = t.tW; t.cX = t.tX; t.placed = true; }
        else if (std::fabs(t.cTop - t.tTop) >= 0.5f || std::fabs(t.cH - t.tH) >= 0.5f ||
                 std::fabs(t.cW - t.tW) >= 0.5f || std::fabs(t.cX - t.tX) >= 0.5f)
            needAnim = true;
    }
    if (needAnim) SetTimer(hwnd, TIMER_ANIM, 15, nullptr);

    // Indicator sits on the inner edge (away from the screen edge the bar is docked to).
    int nx0 = g_cfg.rightSide ? sbW + sbGap : 0, nx1 = g_cfg.rightSide ? W : W - sbW - sbGap;
    int sbX = g_cfg.rightSide ? 0 : W - sbW;

    g_rows.clear();
    if (termH) DrawTerminal(mdc, g_termRect);
    DrawPreviews(mdc);
    DrawPins(mdc, W, pinsScroll ? nx0 : 0, pinsScroll ? nx1 : W, pins, g_pinsTop, g_pinsBottom,
             g_scrollPins, pinsContent, pinsScroll ? sbX : -1);

    if (g_cfg.clock) {
        RECT r{0, clockTop, W, clockTop + clockH};
        if (watch) {
            WatchPlace(&r);                               // it draws itself; the row still drives click and hover
        } else {
            WatchPlace(nullptr);
            bool hov = g_hoverKind == K_CLOCK;
            SelectObject(mdc, g_clockFont);
            int saved = g_lineH;
            g_lineH = clockLineH;
            DrawButton(mdc, r, false, g_cfg.outline, HoverFace(g_cfg.clockFace, hov), hov ? g_cfg.hoverText : g_cfg.text, clockLines);
            g_lineH = saved;
            SelectObject(mdc, g_font);
        }
        g_rows.push_back(Row{r, K_CLOCK, 0, r.top, r.bottom});
    } else {
        WatchPlace(nullptr);
    }
    if (g_cfg.startButton) {
        RECT r{0, startTop, W, H};
        bool hov = g_hoverKind == K_START;
        DrawStartButton(mdc, r, g_cfg.outline, HoverFace(g_cfg.clockFace, hov), hov ? g_cfg.hoverText : g_cfg.startColor);
        g_rows.push_back(Row{r, K_START, 0, startTop, H});
    }

    SetViewportOrgEx(mdc, 0, 0, nullptr);
    BitBlt(hdc, 0, 0, winW, H, mdc, 0, 0, SRCCOPY);
    SelectObject(mdc, oldFont);
    SelectObject(mdc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mdc);
    EndPaint(hwnd, &ps);
    // A card started or stopped sticking out: reshape the window and paint once more so the
    // newly shown part isn't left blank (the second pass finds the region unchanged).
    if (UpdateBarRegion()) InvalidateRect(hwnd, nullptr, FALSE);
}

static const Row* HitTest(int x, int y) {
    for (auto& r : g_rows) {
        if (y < r.clipTop || y >= r.clipBottom) continue;
        if (x >= r.r.left && x < r.r.right && y >= r.r.top && y < r.r.bottom) return &r;
    }
    return nullptr;
}

static bool InTerminal(int x, int y) {
    POINT pt{x, y};
    return g_cfg.terminal && PtInRect(&g_termRect, pt);
}

// ---------- audio meters ----------
// Polls every active output device's sessions (WASAPI) and maps each session's peak to the
// previews of the same process, or of the same exe (browsers play audio from a child process).

static const GUID kCLSID_MMDeviceEnumerator  = {0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
static const GUID kIID_IMMDeviceEnumerator   = {0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
static const GUID kIID_IAudioSessionManager2 = {0x77AA99A0, 0x1BD6, 0x484F, {0x8B, 0xC7, 0x2C, 0x65, 0x4C, 0x9A, 0x9B, 0x6F}};
static const GUID kIID_IAudioSessionControl2 = {0xBFB7FF88, 0x7239, 0x4FC9, {0x8F, 0xA2, 0x07, 0xC9, 0x50, 0xBE, 0x9C, 0x6D}};
static const GUID kIID_IAudioMeterInformation = {0xC02216F6, 0x8C67, 0x4B5B, {0x9D, 0x00, 0xD0, 0x08, 0xE7, 0x3E, 0x00, 0x64}};

// IAudioMeterInformation, declared locally because some MinGW headers only forward-declare it.
struct IPbarMeter : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetPeakValue(float* peak) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetMeteringChannelCount(UINT* count) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetChannelsPeakValues(UINT32 count, float* peaks) = 0;
    virtual HRESULT STDMETHODCALLTYPE QueryHardwareSupport(DWORD* mask) = 0;
};

struct AudioSess { DWORD pid; std::wstring exe; IPbarMeter* meter; };
static IMMDeviceEnumerator* g_mmEnum = nullptr;
static std::vector<AudioSess> g_sessions;
static DWORD g_sessionsAt = 0;

static std::wstring PidPath(DWORD pid) {
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return L"";
    wchar_t buf[MAX_PATH * 2];
    DWORD n = MAX_PATH * 2;
    std::wstring out;
    if (QueryFullProcessImageNameW(p, 0, buf, &n)) out.assign(buf, n);
    CloseHandle(p);
    return out;
}

static void AudioRelease() {
    for (auto& a : g_sessions) if (a.meter) a.meter->Release();
    g_sessions.clear();
}

static void AudioRefreshSessions() {
    AudioRelease();
    g_sessionsAt = GetTickCount();
    if (!g_mmEnum && FAILED(CoCreateInstance(kCLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL,
                                             kIID_IMMDeviceEnumerator, (void**)&g_mmEnum))) {
        g_mmEnum = nullptr;
        return;
    }
    IMMDeviceCollection* coll = nullptr;
    if (FAILED(g_mmEnum->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &coll)) || !coll) return;
    UINT nDev = 0;
    coll->GetCount(&nDev);
    for (UINT d = 0; d < nDev; ++d) {
        IMMDevice* dev = nullptr;
        if (FAILED(coll->Item(d, &dev)) || !dev) continue;
        IAudioSessionManager2* mgr = nullptr;
        if (SUCCEEDED(dev->Activate(kIID_IAudioSessionManager2, CLSCTX_ALL, nullptr, (void**)&mgr)) && mgr) {
            IAudioSessionEnumerator* en = nullptr;
            if (SUCCEEDED(mgr->GetSessionEnumerator(&en)) && en) {
                int count = 0;
                en->GetCount(&count);
                for (int j = 0; j < count; ++j) {
                    IAudioSessionControl* ctl = nullptr;
                    if (FAILED(en->GetSession(j, &ctl)) || !ctl) continue;
                    IAudioSessionControl2* ctl2 = nullptr;
                    DWORD pid = 0;
                    if (SUCCEEDED(ctl->QueryInterface(kIID_IAudioSessionControl2, (void**)&ctl2)) && ctl2) {
                        if (ctl2->GetProcessId(&pid) != S_OK) pid = 0;   // skip system sounds / multi-process
                        ctl2->Release();
                    }
                    IPbarMeter* m = nullptr;
                    if (pid && SUCCEEDED(ctl->QueryInterface(kIID_IAudioMeterInformation, (void**)&m)) && m)
                        g_sessions.push_back(AudioSess{pid, PidPath(pid), m});
                    ctl->Release();
                }
                en->Release();
            }
            mgr->Release();
        }
        dev->Release();
    }
    coll->Release();
}

static float ToDisplay(float peak) {
    if (peak <= 0.0f) return 0.0f;
    if (g_cfg.meterDb <= 0) return std::min(1.0f, peak);
    float db = 20.0f * std::log10(peak);
    return std::max(0.0f, std::min(1.0f, 1.0f + db / (float)g_cfg.meterDb));
}

// Returns true if any preview's bar changed enough to repaint.
static bool AudioTick() {
    if (g_sessions.empty() || GetTickCount() - g_sessionsAt > 1000) AudioRefreshSessions();

    std::vector<std::pair<AudioSess*, float>> peaks;
    for (auto& a : g_sessions) {
        float p = 0;
        if (SUCCEEDED(a.meter->GetPeakValue(&p)) && p > 0) peaks.push_back({&a, p});
    }

    const float fall = g_cfg.meterFall / 100.0f;
    bool changed = false;
    for (auto& t : g_tasks) {
        float target = 0;
        for (auto& pk : peaks) {
            const AudioSess& a = *pk.first;
            bool direct = a.pid == t.pid;
            bool sameExe = !direct && !t.exe.empty() && _wcsicmp(a.exe.c_str(), t.exe.c_str()) == 0 &&
                           std::none_of(g_tasks.begin(), g_tasks.end(), [&](const Task& o) { return o.pid == a.pid; });
            if (direct || sameExe) target = std::max(target, ToDisplay(pk.second));
        }
        float next = std::max(target, t.level - fall);
        if (next < 0.001f) next = 0;
        if (std::fabs(next - t.level) > 0.002f) changed = true;
        t.level = next;
    }
    return changed;
}

// Blink timer runs only while some window is asking for attention.
static void UpdateBlinkTimer(bool restart) {
    bool any = std::any_of(g_tasks.begin(), g_tasks.end(), [](const Task& t) { return t.flash; });
    if (!any) { KillTimer(g_hwnd, TIMER_BLINK); g_blinkOn = true; g_blinkTicks = 0; return; }
    if (restart) {
        g_blinkOn = true;
        g_blinkTicks = 0;
        SetTimer(g_hwnd, TIMER_BLINK, g_cfg.flashRate, nullptr);
    }
}

static void StartMeterTimer() {
    KillTimer(g_hwnd, TIMER_METER);
    if (g_cfg.meter) SetTimer(g_hwnd, TIMER_METER, 1000 / g_cfg.meterFps, nullptr);
    else for (auto& t : g_tasks) t.level = 0;
}

// ---------- actions ----------

// Brings h to the front without SwitchToThisWindow's slow animated path.
static void ForceForeground(HWND h) {
    if (SetForegroundWindow(h)) return;
    HWND fg = GetForegroundWindow();
    DWORD me = GetCurrentThreadId();
    DWORD fgThread = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
    bool attached = fgThread && fgThread != me && AttachThreadInput(me, fgThread, TRUE);
    BringWindowToTop(h);
    SetForegroundWindow(h);
    if (attached) AttachThreadInput(me, fgThread, FALSE);
}

static void ActivateOrMinimize(HWND h) {
    if (!IsWindow(h)) return;
    if (IsIconic(h)) {
        ShowWindowAsync(h, SW_RESTORE);
        ForceForeground(h);
    } else if (GetForegroundWindow() == h) {          // (from the terminal, clicking your last window goes back to it)
        ShowWindowAsync(h, SW_MINIMIZE);
    } else {
        ForceForeground(h);
    }
}

// Press keys down in order, release in reverse (e.g. {VK_LWIN, 'X'} = Win+X).
static void PressKeys(std::initializer_list<WORD> keys) {
    std::vector<INPUT> in;
    for (WORD k : keys) { INPUT i{}; i.type = INPUT_KEYBOARD; i.ki.wVk = k; in.push_back(i); }
    for (auto it = keys.end(); it != keys.begin();) {
        --it;
        INPUT i{}; i.type = INPUT_KEYBOARD; i.ki.wVk = *it; i.ki.dwFlags = KEYEVENTF_KEYUP; in.push_back(i);
    }
    SendInput((UINT)in.size(), in.data(), sizeof(INPUT));
}

static void LaunchPin(const Pin& p) {
    if (!p.aumid.empty()) {
        std::wstring target = L"shell:AppsFolder\\" + p.aumid;
        ShellExecuteW(nullptr, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return;
    }
    std::wstring path = ExpandEnv(p.path), args = ExpandEnv(p.args), dir = ExpandEnv(p.dir);
    ShellExecuteW(nullptr, L"open", path.c_str(), args.empty() ? nullptr : args.c_str(),
                  dir.empty() ? nullptr : dir.c_str(), SW_SHOWNORMAL);
}

static bool ParseHotkey(const std::wstring& spec, UINT& mods, UINT& vk) {
    mods = 0;
    vk = 0;
    for (auto& part : Split(spec, L'+')) {
        std::wstring p = Lower(part);
        if (p.empty()) continue;
        if (p == L"ctrl" || p == L"control") mods |= MOD_CONTROL;
        else if (p == L"alt") mods |= MOD_ALT;
        else if (p == L"shift") mods |= MOD_SHIFT;
        else if (p == L"win" || p == L"windows") mods |= MOD_WIN;
        else if (p.size() == 1) {
            wchar_t ch = towupper(p[0]);
            if ((ch >= L'A' && ch <= L'Z') || (ch >= L'0' && ch <= L'9')) vk = ch;
            else if (ch == L'`' || ch == L'~') vk = VK_OEM_3;
            else {
                SHORT k = VkKeyScanW(p[0]);
                if (k != -1) vk = LOBYTE(k);
            }
        }
        else if (p[0] == L'f' && p.size() <= 3 && iswdigit(p[1])) {
            int f = _wtoi(p.c_str() + 1);
            if (f >= 1 && f <= 24) vk = VK_F1 + f - 1;
        }
        else if (p == L"space") vk = VK_SPACE;
        else if (p == L"enter" || p == L"return") vk = VK_RETURN;
        else if (p == L"tab") vk = VK_TAB;
        else if (p == L"esc" || p == L"escape") vk = VK_ESCAPE;
        else return false;
    }
    return vk != 0;
}

static void RegisterTermHotkey() {
    UnregisterHotKey(g_hwnd, HOTKEY_TERM);
    UINT mods = 0, vk = 0;
    if (!g_cfg.terminal || Trim(g_cfg.termKey).empty()) return;
    if (!ParseHotkey(g_cfg.termKey, mods, vk)) {
        TermAddLine(L"[termkey " + g_cfg.termKey + L" not understood]", TL_INFO);
        return;
    }
    if (!RegisterHotKey(g_hwnd, HOTKEY_TERM, mods | MOD_NOREPEAT, vk))
        TermAddLine(L"[" + g_cfg.termKey + L" is taken by another app]", TL_INFO);
}

static void SettingsSync();

static void Reload() {
    LoadConfig();
    ResetIcons();
    CreateBarFont();
    ShowSystemTaskbars(!g_cfg.hideTaskbar);
    AppBarSetPos();
    ApplyZOrder(true);
    RefreshTasks();
    for (auto& t : g_tasks) UpdateGeometry(t);
    RequestStartPins();
    StartMeterTimer();
    if (!g_cfg.terminal || (g_term.started && g_term.kind != ShellKind())) TermStop();
    RegisterTermHotkey();
    g_termVisDirty = true;
    WatchResetFonts();                                    // the watch font or size may have changed
    WatchArm();
    if (g_watchHwnd) InvalidateRect(g_watchHwnd, nullptr, FALSE);
    PvSync();
    InvalidateRect(g_hwnd, nullptr, FALSE);
    SettingsSync();                                       // an open settings window shows what's now in the ini
}

// Sets key=value in pbar.ini in place (first uncommented match), or appends it.
static void SetIniValue(const std::wstring& key, const std::wstring& value) {
    std::wstring text = DecodeText(ReadFileBytes(g_iniPath));
    std::wstring out, line;
    bool done = false;
    size_t start = 0;
    while (start <= text.size()) {
        size_t nl = text.find(L'\n', start);
        line = text.substr(start, nl == std::wstring::npos ? std::wstring::npos : nl - start);
        std::wstring t = Trim(line);
        size_t eq = t.find(L'=');
        if (!done && !t.empty() && t[0] != L';' && t[0] != L'#' && eq != std::wstring::npos &&
            _wcsicmp(Trim(t.substr(0, eq)).c_str(), key.c_str()) == 0) {
            bool cr = !line.empty() && line.back() == L'\r';
            line = key + L"=" + value + (cr ? L"\r" : L"");
            done = true;
        }
        out += line;
        if (nl == std::wstring::npos) break;
        out += L'\n';
        start = nl + 1;
    }
    if (!done) {
        if (!out.empty() && out.back() != L'\n') out += L"\r\n";
        out += key + L"=" + value + L"\r\n";
    }
    std::ofstream o(g_iniPath.c_str(), std::ios::binary);
    o << WToUtf8(out);
}

// ---------- start with Windows ----------
// A per-user Run entry (HKCU, no admin needed) pointing at this exe's current location.

static const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

static std::wstring ExePath() {
    wchar_t exe[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH * 2);
    return std::wstring(exe, n);
}

// True only if the entry exists and points at this exe (a stale path from a moved exe counts as off).
static bool AutostartEnabled() {
    wchar_t val[MAX_PATH * 2 + 8];
    DWORD size = sizeof(val), type = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, kRunKey, L"pbar", RRF_RT_REG_SZ, &type, val, &size) != ERROR_SUCCESS)
        return false;
    std::wstring v = val;
    v.erase(std::remove(v.begin(), v.end(), L'"'), v.end());
    return _wcsicmp(Trim(v).c_str(), ExePath().c_str()) == 0;
}

static void SetAutostart(bool on) {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS)
        return;
    if (on) {
        std::wstring cmd = L"\"" + ExePath() + L"\"";
        RegSetValueExW(k, L"pbar", 0, REG_SZ, (const BYTE*)cmd.c_str(), (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(k, L"pbar");
    }
    RegCloseKey(k);
}

// ---------- settings window ----------
// Every pbar.ini setting on a handful of pages, each with a plain description, so nobody has to edit the
// ini by hand. Changes wait for Apply (or OK); then they're written into pbar.ini in place, keeping your
// comments and order, and loaded exactly like Reload. The ini stays the one place settings live, so it
// and this window never disagree.

enum class SwT { Page, Head, Bool, Int, Choice, Color, AutoColor, Text, Font, File, Folder, Hotkey, Lines, Pins,
                 Autostart, Monitor };

struct SwOpt {
    SwT type;
    const wchar_t* key;     // ini key; nullptr for pages and headings ("*autostart" lives in the registry)
    const wchar_t* label;
    const wchar_t* desc;
    int lo, hi, step;       // Int: range and the -/+ step. Color: which colour of a comma-separated list (-1 = all)
    const wchar_t* extra;   // Int: unit. Choice: "value=Label|...". AutoColor: the key it follows while unset.
};

#define SW_PAGE(l, d)                  {SwT::Page, nullptr, l, d, 0, 0, 0, nullptr}
#define SW_HEAD(l)                     {SwT::Head, nullptr, l, nullptr, 0, 0, 0, nullptr}
#define SW_BOOL(k, l, d)               {SwT::Bool, k, l, d, 0, 0, 0, nullptr}
#define SW_INT(k, l, lo, hi, u, d)     {SwT::Int, k, l, d, lo, hi, 1, u}
#define SW_INTS(k, l, lo, hi, s, u, d) {SwT::Int, k, l, d, lo, hi, s, u}
#define SW_CHOICE(k, l, c, d)          {SwT::Choice, k, l, d, 0, 0, 0, c}
#define SW_COLOR(k, l, d)              {SwT::Color, k, l, d, -1, 0, 0, nullptr}
#define SW_COLORN(k, n, l, d)          {SwT::Color, k, l, d, n, 0, 0, nullptr}
#define SW_AUTOCOLOR(k, f, l, d)       {SwT::AutoColor, k, l, d, 0, 0, 0, f}
#define SW_OF(t, k, l, d)              {SwT::t, k, l, d, 0, 0, 0, nullptr}

static const SwOpt kSwOpts[] = {
    SW_PAGE(L"Bar", L"Where the bar sits and how much room it takes."),
    SW_CHOICE(L"side", L"Screen edge", L"left=Left|right=Right",
              L"Which side of the screen the bar docks to. Windows keeps that strip for it, so maximized windows "
              L"stop at the bar instead of going underneath."),
    SW_OF(Monitor, L"monitor", L"Display",
          L"The screen the bar lives on. The numbers match the ones Windows shows under Settings > System > "
          L"Display > Identify. Primary follows whichever screen is set as your main display."),
    SW_INT(L"width", L"Width", 60, 150, L"px",
           L"How wide the bar is. The terminal, previews and watch all resize to fit; below about 110 px the "
           L"watch shows just the time."),
    SW_BOOL(L"hidetaskbar", L"Hide the Windows taskbar",
            L"Hides the normal taskbar while pbar runs and brings it back when pbar closes. Also turn on auto-hide "
            L"in the taskbar's own settings, or the space it keeps for itself stays empty."),
    SW_OF(Autostart, L"*autostart", L"Start with Windows",
          L"Starts pbar when you sign in. If you move pbar.exe, turn this on again so it points to the new place."),
    SW_HEAD(L"Spacing"),
    SW_INT(L"gap", L"Section gap", 0, 40, L"px",
           L"Empty space between the bar's sections: the terminal, the previews, the pinned apps and the clock."),
    SW_INT(L"spacing", L"Item spacing", 0, 20, L"px",
           L"Empty space between neighbouring previews, and between pinned buttons."),
    SW_INT(L"pad", L"Side padding", 0, 20, L"px", L"Space between a button's left and right edges and its text."),
    SW_INT(L"vpad", L"Top and bottom padding", 0, 20, L"px",
           L"Space above and below the text in a button. Bigger values make every button taller and easier to hit."),
    SW_INT(L"highlight", L"Bevel glint", 0, 40, L"px",
           L"Length of the white lines along each button's top and left edges, measured from the corner. 0 runs "
           L"them the full length for a classic raised look."),
    SW_INT(L"hoverbright", L"Hover glow", -255, 255, L"",
           L"How much a button brightens under the mouse. This amount is added to the red, green and blue of its "
           L"face colour; 0 turns it off and negative numbers darken instead."),

    SW_PAGE(L"Text", L"The font used for buttons, preview titles, the classic clock and the terminal."),
    SW_OF(Font, L"font", L"Font", L"Any installed font works. A fixed-width one like Consolas suits the retro look."),
    SW_INT(L"fontsize", L"Size", 5, 72, L"px",
           L"Height of the text. Buttons grow to fit it, and the terminal and classic clock use it too unless they "
           L"have a size of their own."),
    SW_INTS(L"fontweight", L"Weight", 100, 900, 100, L"",
            L"How thick the strokes are: 400 is normal, 700 bold, 900 black. If the font doesn't come in that "
            L"weight, Windows imitates it."),
    SW_BOOL(L"bold", L"Bold", L"Quick switch for bold text. When on, the weight is at least 700."),
    SW_BOOL(L"italic", L"Italic", L"Slanted text."),
    SW_CHOICE(L"aa", L"Edges", L"0=Crisp|1=Smooth",
              L"Crisp draws hard pixel edges, which keeps small text sharp and suits the bevelled look. Smooth uses "
              L"ClearType, like the rest of Windows."),
    SW_OF(File, L"fontfile", L"Font file",
          L"A .ttf or .otf file to load just for pbar, without installing it. Then type the font's name under "
          L"Font; opening the file in Windows shows the name at the top."),
    SW_HEAD(L"Wrapping"),
    SW_INT(L"maxlines", L"Lines per pinned app", 1, 10, L"",
           L"How many lines a long pinned app name can wrap onto before it's cut off. 1 keeps every name on a "
           L"single line."),
    SW_INT(L"linegap", L"Line gap", 0, 20, L"px", L"Extra space between the lines of wrapped text."),
    SW_BOOL(L"ellipsis", L"Mark cut-off text", L"Ends text that doesn't fit with … so you can tell it was shortened."),
    SW_OF(Lines, L"strip", L"Title endings to remove",
          L"Text trimmed off the end of window titles to save space, one per line. With “- Google Chrome” "
          L"in the list, “Inbox - Google Chrome” shows as just “Inbox”."),

    SW_PAGE(L"Previews",
            L"The live column of windows. The focused window sits in the middle at full size and the others get "
            L"smaller the further away they are. Hovering a preview makes it the big one, and the mouse wheel "
            L"spins the column."),
    SW_INT(L"previewmargin", L"Margin", 0, 40, L"px", L"Space on each side of the biggest preview."),
    SW_INT(L"step", L"Size step", 10, 100, L"%",
           L"Each preview is this percentage of the size of its neighbour nearer the big one. Lower values shrink "
           L"them faster; 100 makes them all the same size."),
    SW_INT(L"minscale", L"Smallest size", 5, 100, L"%", L"Previews never shrink below this percentage of the biggest one."),
    SW_INT(L"thumbpad", L"Picture padding", 0, 20, L"px",
           L"Space between a preview's edge and the window picture inside it. The audio meter sits in the bottom "
           L"padding, so keep this at least 1 more than the meter's height."),
    SW_INTS(L"titlemin", L"Title line from", 0, 400, 5, L"px",
            L"Previews at least this wide show the app's icon and name above the picture; narrower ones show only "
            L"the picture. 0 hides the title line on every preview."),
    SW_INTS(L"maxaspect", L"Tallest picture", 5, 400, 5, L"%",
            L"The most a picture's height can be, as a percentage of its width. At 100, tall windows are shown no "
            L"taller than square, with bars at the sides."),
    SW_INTS(L"minaspect", L"Flattest picture", 5, 400, 5, L"%",
            L"The least a picture's height can be, as a percentage of its width, so very wide windows don't turn "
            L"into thin strips."),
    SW_BOOL(L"clientonly", L"Contents only",
            L"Crops off each window's title bar and frame, so the preview shows only what's inside."),
    SW_INTS(L"minopacity", L"Minimized opacity", 0, 255, 5, L"",
            L"How solid the previews of minimized windows are, from 0 (invisible) to 255 (fully solid)."),
    SW_INT(L"popout", L"Hover pop-out", 0, 100, L"px",
           L"How far the hovered preview grows past the bar's inner edge, over whatever is next to the bar. 0 "
           L"keeps it inside the bar."),
    SW_HEAD(L"Motion"),
    SW_INT(L"animspeed", L"Animation speed", 1, 100, L"%",
           L"How much of the remaining distance the previews cover each frame. Higher is snappier; 100 jumps "
           L"straight there with no animation."),
    SW_INTS(L"recenterdelay", L"Recenter delay", 0, 5000, 50, L"ms",
            L"After the mouse leaves the column, how long to wait before the focused window slides back to the middle."),
    SW_HEAD(L"Hovering"),
    SW_BOOL(L"hoverfocus", L"Bring to front on hover",
            L"Hovering a preview also brings its window to the front, without clicking."),
    SW_INTS(L"hoverdelay", L"Hover delay", 0, 5000, 50, L"ms",
            L"When bringing windows to the front on hover, how long the mouse has to rest on a preview first. 0 is instant."),
    SW_BOOL(L"hoverrestore", L"Restore minimized on hover",
            L"When bringing windows to the front on hover, minimized windows are restored too."),
    SW_HEAD(L"Highlighted outline"),
    SW_CHOICE(L"bordermode", L"Outline on", L"focused=Focused|visible=Visible|off=None",
              L"Which previews get a bright outline: only the window in front, every window you can at least partly "
              L"see on screen, or none."),
    SW_COLOR(L"activeborder", L"Outline colour", L"Colour of that outline."),

    SW_PAGE(L"Terminal",
            L"The mini terminal at the top of the bar. Click it and type a command; the mouse wheel scrolls back "
            L"through the output."),
    SW_BOOL(L"terminal", L"Show the terminal", L"Turn off to give the whole bar to previews."),
    SW_CHOICE(L"shell", L"Shell", L"powershell=PowerShell|pwsh=PowerShell 7|cmd=cmd",
              L"What runs your commands: Windows PowerShell, PowerShell 7 (installed separately) or the classic "
              L"Command Prompt. Changing this restarts the shell."),
    SW_INT(L"termlines", L"Height", 1, 200, L"lines",
           L"How many lines of output show at once. It never takes more than half the space above the clock."),
    SW_INT(L"termfontsize", L"Text size", 0, 72, L"px", L"0 uses the main text size from the Text page."),
    SW_OF(Folder, L"termdir", L"Start folder", L"The folder the shell starts in. Leave blank for your user folder."),
    SW_OF(Hotkey, L"termkey", L"Hotkey",
          L"Works from any app: jumps to the terminal, and back to where you were when pressed again. Click the "
          L"box and press the keys you want; Backspace clears it."),
    SW_INTS(L"termscrollback", L"Scrollback", 50, 100000, 500, L"lines",
            L"How many lines of output are kept to scroll back through."),
    SW_HEAD(L"Colours"),
    SW_COLOR(L"termbg", L"Background", L"Behind the terminal's text."),
    SW_COLOR(L"termtext", L"Output", L"Normal output from commands."),
    SW_COLOR(L"termcmd", L"Your commands", L"What you typed, as it's echoed in the output."),
    SW_COLOR(L"termerr", L"Errors", L"Error messages."),
    SW_COLOR(L"termdim", L"Folder and notes", L"The current folder above the prompt, and pbar's own notes."),
    SW_COLOR(L"termsel", L"Selection",
             L"Behind selected text. Drag to select or double-click a word, then press Ctrl+C or right-click and "
             L"choose Copy."),

    SW_PAGE(L"Clock", L"The clock near the bottom of the bar. Click it for a calendar; right-click it for pbar's menu."),
    SW_BOOL(L"clock", L"Show the clock", L"Turn off to hide the clock, and the calendar with it."),
    SW_CHOICE(L"clockstyle", L"Style", L"watch=Watch|classic=Classic",
              L"Watch is a smartwatch face with big digits, the date and three progress rings. Classic is a plain "
              L"text button."),
    SW_CHOICE(L"clock24", L"Format", L"0=12-hour|1=24-hour",
              L"In 24-hour mode the watch shows the month under its rings instead of AM or PM."),
    SW_HEAD(L"Watch"),
    SW_INT(L"watchheight", L"Height", 40, 400, L"px",
           L"How tall the watch is. It never takes more than a third of the screen's height, and everything on it "
           L"scales to fit."),
    SW_OF(Font, L"watchfont", L"Font",
          L"Typeface for the watch's digits and date. If it isn't installed, the watch uses Bahnschrift, then Segoe UI."),
    SW_INT(L"watchfps", L"Frame rate", 0, 60, L"fps",
           L"How smoothly the seconds ring sweeps round. 0 moves it once a second and uses the least power."),
    SW_COLOR(L"watchaccent", L"Minutes colour", L"The minute digits, and the watch's rim when the mouse is over it."),
    SW_COLOR(L"watchday", L"Day colour", L"The day of the week."),
    SW_COLORN(L"watchrings", 0, L"Seconds ring", L"Outer ring: fills once a minute."),
    SW_COLORN(L"watchrings", 1, L"Minutes ring", L"Middle ring: fills once an hour."),
    SW_COLORN(L"watchrings", 2, L"Hours ring", L"Inner ring: fills every 12 hours, or every 24 in 24-hour mode."),
    SW_HEAD(L"Classic clock"),
    SW_INT(L"clockfontsize", L"Text size", 0, 72, L"px", L"0 uses the main text size from the Text page."),
    SW_INT(L"clocklines", L"Lines", 1, 4, L"",
           L"If the time doesn't fit across the bar, it wraps onto up to this many lines."),
    SW_CHOICE(L"clockbold", L"Weight", L"=Like the text|0=Normal|1=Bold",
              L"Bold or normal clock text, separately from the rest of the bar. Like the text follows Bold on the "
              L"Text page."),
    SW_HEAD(L"Calendar"),
    SW_CHOICE(L"weekstart", L"Week starts on", L"0=Sunday|1=Monday", L"The first column of the calendar."),
    SW_INT(L"caltimeout", L"Close after", 1, 60, L"s",
           L"The calendar stays open while the mouse is over it or the clock, and closes this long after the mouse leaves."),

    SW_PAGE(L"Start & pins", L"The start button at the very bottom, and the pinned apps above the clock."),
    SW_BOOL(L"startbutton", L"Start button",
            L"The shaded bar at the very bottom. Click it for the Start menu; right-click it for the Win+X menu."),
    SW_INT(L"startlines", L"Height", 1, 4, L"lines", L"How tall the start button is, in lines of text."),
    SW_INT(L"startheight", L"Exact height", 0, 200, L"px",
           L"A height in pixels instead, which wins over the line count. 0 uses the line count."),
    SW_AUTOCOLOR(L"startcolor", L"text", L"Shade colour",
                 L"Colour of the button's four dithered shade blocks. Default uses the text colour."),
    SW_HEAD(L"Pinned apps"),
    SW_OF(Pins, L"pin", L"Your pins",
          L"Programs, shortcuts, documents or folders you want one click away, listed first. Variables like "
          L"%USERPROFILE% work in every field."),
    SW_BOOL(L"startpins", L"Include Start menu pins",
            L"Adds the apps pinned to your Windows Start menu after your own, or the taskbar's pins if the Start "
            L"menu's can't be read."),
    SW_OF(Folder, L"pindir", L"Shortcut folder",
          L"Every shortcut or program in this folder becomes a pin too, listed alphabetically after your own. "
          L"Leave blank for none."),
    SW_INT(L"pinsmax", L"Most of the bar", 10, 100, L"%",
           L"The largest share of the bar's height the pinned apps can take. Past that, the section scrolls."),

    SW_PAGE(L"Colours", L"Colours for the whole bar. Click a swatch to pick one, or type a hex code such as #2F7DFF."),
    SW_HEAD(L"Buttons"),
    SW_COLOR(L"bg", L"Background", L"The bar itself, and the gaps between buttons and previews."),
    SW_COLOR(L"face", L"Face", L"Fill colour of buttons and preview cards."),
    SW_COLOR(L"outline", L"Outline", L"The 1-pixel border around every button and preview."),
    SW_COLOR(L"hi", L"Glint", L"The bevel lines on each button's top and left edges."),
    SW_AUTOCOLOR(L"activeface", L"face", L"Focused window",
                 L"Face of the preview for the window in front, and of today in the calendar. Default uses Face."),
    SW_AUTOCOLOR(L"pinface", L"face", L"Pinned apps", L"Face of the pinned app buttons. Default uses Face."),
    SW_AUTOCOLOR(L"clockface", L"face", L"Clock and start",
                 L"Face of the classic clock, the start button and the calendar's heading. Default uses Face."),
    SW_HEAD(L"Text"),
    SW_COLOR(L"text", L"Text", L"Normal text."),
    SW_COLOR(L"hovertext", L"Under the mouse", L"Text on whatever the mouse is over."),
    SW_COLOR(L"activetext", L"Focused window", L"Title of the window in front, and of any window asking for attention."),
    SW_COLOR(L"dim", L"Faded", L"Titles of minimized windows, and days outside the current month in the calendar."),
    SW_HEAD(L"Asking for attention"),
    SW_AUTOCOLOR(L"flash", L"metercolor", L"Blink colour",
                 L"When a window asks for attention, its preview blinks this colour. Default uses the audio meter colour."),
    SW_INTS(L"flashrate", L"Blink speed", 100, 5000, 50, L"ms", L"How long each blink lasts."),
    SW_INT(L"flashcount", L"Blinks", 0, 100, L"",
           L"How many times it blinks before staying lit until you switch to that window. 0 stays lit from the start."),
    SW_HEAD(L"Scroll indicators"),
    SW_INT(L"scrollw", L"Width", 0, 10, L"px",
           L"The thin bar showing where you are when the pinned apps or the terminal hold more than fits. 0 hides it."),
    SW_AUTOCOLOR(L"scrollcolor", L"metercolor", L"Colour", L"Default uses the audio meter colour."),

    SW_PAGE(L"Icons",
            L"Each app's real icon, reduced to three shades plus see-through so it matches the bar. Changing the "
            L"shades tints every icon."),
    SW_BOOL(L"icons", L"Show icons",
            L"Icons on pinned buttons and preview title lines, and in place of the picture when a window has no "
            L"live preview."),
    SW_INT(L"iconsize", L"Size", 0, 64, L"px", L"0 matches the height of a line of text."),
    SW_INT(L"iconpad", L"Space after icon", 0, 20, L"px", L"Gap between an icon and the text beside it."),
    SW_BOOL(L"icondither", L"Dithering",
            L"Blends between the shades with a fine checker pattern for a retro look. Off uses hard steps."),
    SW_COLORN(L"iconshades", 0, L"Dark shade", L"The darkest of the three."),
    SW_COLORN(L"iconshades", 1, L"Middle shade", L""),
    SW_COLORN(L"iconshades", 2, L"Light shade", L"The lightest of the three."),

    SW_PAGE(L"Audio meter", L"A level bar along the bottom of any preview whose app is playing sound."),
    SW_BOOL(L"meter", L"Show the meter", L"Turn off to stop checking for sound altogether."),
    SW_INT(L"meterh", L"Height", 1, 20, L"px",
           L"Thickness of the bar. It sits in the preview's bottom padding, so keep Picture padding on the "
           L"Previews page at least 1 more than this."),
    SW_COLOR(L"metercolor", L"Colour", L"Also the default colour for blinking and for the scroll indicators."),
    SW_INTS(L"meterdb", L"Range", 0, 120, 6, L"dB",
            L"How much of the loudness range spans the bar's width. Larger values show quiet sounds more; 0 shows "
            L"raw volume instead."),
    SW_INT(L"meterfall", L"Fall speed", 1, 100, L"%",
           L"How fast the bar drops back after a peak, as a percentage of its full width per frame."),
    SW_INT(L"meterfps", L"Updates", 5, 120, L"fps", L"How many times a second the bar updates while sound plays."),
};
static const int kSwCount = (int)(sizeof(kSwOpts) / sizeof(kSwOpts[0]));

static const COLORREF kSwBg = RGB(0, 0, 0), kSwSide = RGB(12, 12, 12), kSwLine = RGB(48, 48, 48),
                      kSwFace = RGB(64, 64, 64), kSwSel = RGB(38, 98, 200), kSwText = RGB(216, 216, 216),
                      kSwBright = RGB(255, 255, 255), kSwDesc = RGB(150, 150, 150), kSwDim = RGB(105, 105, 105),
                      kSwField = RGB(24, 24, 24), kSwEdge = RGB(78, 78, 78), kSwAccent = RGB(47, 125, 255);

enum SwRole { SR_SEG = 1, SR_MINUS, SR_PLUS, SR_NUM, SR_HEX, SR_SWATCH, SR_TEXT, SR_PICK, SR_BROWSE, SR_HOTKEY,
              SR_LINES, SR_PINLIST, SR_PINADD, SR_PINDEL, SR_PINUP, SR_PINDOWN, SR_PINFIELD, SR_PINBROWSE };

#define SWID_OK     20
#define SWID_CANCEL 21
#define SWID_APPLY  22
#define SWID_RESET  23
#define SWID_INI    24
#define SWID_PAGE0  100
#define SWID_CTL0   1000
#define TIMER_SWREPEAT 30

struct SwCtl {
    HWND h = nullptr;
    int opt = -1, role = 0, idx = 0;
    std::wstring val;            // SR_SEG: the value this segment picks
    RECT rc{};                   // content coordinates; for fields, the frame drawn around the edit
};
struct SwNote { RECT r; std::wstring text; bool dim; };
struct SwRow {
    int opt = -1, first = 0, count = 0;      // its controls are g_swCtls[first .. first + count)
    RECT label{}, desc{};
    std::vector<SwNote> notes;               // units, ranges and field names, drawn by the panel
};
struct SwModel { std::map<std::wstring, std::wstring> orig, cur, def; };   // by ini key: as loaded, as edited, default
using SwPin = std::array<std::wstring, 4>;                                  // name, program, arguments, start in

static HWND g_swHwnd = nullptr, g_swPanel = nullptr, g_swSbar = nullptr, g_swHot = nullptr;
static std::vector<int> g_swPages;                      // where each page starts in kSwOpts
static int g_swPage = 0, g_swDpi = 96, g_swScroll = 0, g_swContentH = 0, g_swFontH = 16, g_swWheel = 0;
static int g_swPinSel = -1, g_swRepeat = 0, g_swDragY = -1, g_swDragScroll = 0;
static HFONT g_swFont = nullptr, g_swBold = nullptr, g_swTitle = nullptr, g_swHead = nullptr;
static HBRUSH g_swFieldBrush = nullptr;
static HICON g_swIconBig = nullptr, g_swIconSmall = nullptr;
static std::vector<SwCtl> g_swCtls;
static std::vector<SwRow> g_swRows;
static SwModel g_sm;
static bool g_swQuiet = false;                          // set while we change a field's text ourselves
static bool g_swApplied = false;
static RECT g_swStatusR{};
static WNDPROC g_swBtnBase = nullptr, g_swEditBase = nullptr;
static COLORREF g_swCustom[16];

static int SwS(int v) { return MulDiv(v, g_swDpi, 96); }
static int SwPageEnd(int p) { return p + 1 < (int)g_swPages.size() ? g_swPages[p + 1] : kSwCount; }

// ----- values -----

struct SwChoice { std::wstring val, label; };

static std::vector<SwChoice> SwChoices(const SwOpt& o) {
    std::vector<SwChoice> v;
    switch (o.type) {
    case SwT::Bool: case SwT::Autostart: return {{L"0", L"Off"}, {L"1", L"On"}};
    case SwT::AutoColor: return {{L"", L"Default"}, {L"*", L"Custom"}};
    case SwT::Monitor:
        v.push_back({L"primary", L"Primary"});
        for (auto& m : Monitors()) v.push_back({std::to_wstring(m.num), std::to_wstring(m.num)});
        return v;
    case SwT::Choice:
        for (auto& part : Split(o.extra, L'|')) {
            size_t eq = part.find(L'=');
            if (eq != std::wstring::npos) v.push_back({part.substr(0, eq), part.substr(eq + 1)});
        }
        return v;
    default: return v;
    }
}

static std::wstring SwHex(COLORREF c) {
    wchar_t b[8];
    wsprintfW(b, L"#%02X%02X%02X", GetRValue(c), GetGValue(c), GetBValue(c));
    return b;
}

static bool SwParseHex(std::wstring s, COLORREF& c) {
    s = Trim(s);
    if (!s.empty() && s[0] == L'#') s.erase(0, 1);
    if (s.size() != 6 || s.find_first_not_of(L"0123456789abcdefABCDEF") != std::wstring::npos) return false;
    c = ParseColor(s, 0);
    return true;
}

static bool SwParseInt(std::wstring s, int& n) {
    s = Trim(s);
    size_t i = (!s.empty() && (s[0] == L'-' || s[0] == L'+')) ? 1 : 0;
    if (i >= s.size() || s.size() > 9 || s.find_first_not_of(L"0123456789", i) != std::wstring::npos) return false;
    n = _wtoi(s.c_str());
    return true;
}

// A value as pbar would understand it, written the one way this window shows it; anything it can't read
// becomes the fallback.
static std::wstring SwNorm(const SwOpt& o, const std::wstring& raw, const std::wstring& fallback) {
    std::wstring v = Trim(raw);
    int n = 0;
    COLORREF c = 0;
    switch (o.type) {
    case SwT::Bool: case SwT::Autostart: return SwParseInt(v, n) && n != 0 ? L"1" : L"0";
    case SwT::Int: return SwParseInt(v, n) ? std::to_wstring(std::max(o.lo, std::min(o.hi, n))) : fallback;
    case SwT::Monitor: return SwParseInt(v, n) && n > 0 ? std::to_wstring(n) : L"primary";
    case SwT::Choice: {
        auto ch = SwChoices(o);
        for (auto& x : ch) if (_wcsicmp(x.val.c_str(), v.c_str()) == 0) return x.val;
        if (SwParseInt(v, n))                                   // on/off settings: any number counts, as in pbar
            for (auto& x : ch) if (x.val == (n != 0 ? L"1" : L"0")) return x.val;
        return fallback;
    }
    case SwT::Color: {
        if (o.lo < 0) return SwParseHex(v, c) ? SwHex(c) : fallback;
        auto parts = Split(v, L','), fb = Split(fallback, L',');
        std::wstring out;
        for (size_t i = 0; i < 3; ++i) {
            std::wstring f = i < fb.size() && !fb[i].empty() ? fb[i] : L"#000000";
            out += (i ? L"," : L"") + ((i < parts.size() && SwParseHex(parts[i], c)) ? SwHex(c) : f);
        }
        return out;
    }
    case SwT::AutoColor: return v.empty() ? L"" : SwParseHex(v, c) ? SwHex(c) : fallback;
    case SwT::Lines: {
        std::wstring out;
        for (auto& l : Split(v, L'\n')) if (!l.empty()) out += (out.empty() ? L"" : L"\n") + l;
        return out;
    }
    case SwT::Pins: {
        std::wstring out;
        for (auto& l : Split(v, L'\n')) {
            auto p = Split(l, L'|');
            p.resize(4);
            if (p[1].empty()) continue;                         // nothing to open
            if (p[0].empty()) p[0] = FileStem(p[1]);
            size_t keep = 4;
            while (keep > 2 && p[keep - 1].empty()) --keep;
            std::wstring line;
            for (size_t i = 0; i < keep; ++i) line += (i ? L"|" : L"") + p[i];
            out += (out.empty() ? L"" : L"\n") + line;
        }
        return out;
    }
    default: return v;
    }
}

// The settings in some ini text, by lowercase key: the last one wins, as in LoadConfig; strip= and pin= collect.
static std::map<std::wstring, std::wstring> SwIniMap(const std::wstring& text) {
    std::map<std::wstring, std::wstring> m;
    std::wstringstream ws(text);
    std::wstring line;
    while (std::getline(ws, line)) {
        line = Trim(line);
        if (line.empty() || line[0] == L';' || line[0] == L'#') continue;
        size_t eq = line.find(L'=');
        if (eq == std::wstring::npos) continue;
        std::wstring k = Lower(Trim(line.substr(0, eq))), v = Trim(line.substr(eq + 1));
        if (k == L"strip" || k == L"pin") {
            if (!v.empty()) m[k] += (m[k].empty() ? L"" : L"\n") + v;
        } else {
            m[k] = v;
        }
    }
    return m;
}

// What pbar.ini says now (a setting it leaves out shows its default, which is what pbar uses).
static SwModel SwReadModel() {
    SwModel m;
    auto user = SwIniMap(DecodeText(ReadFileBytes(g_iniPath)));
    auto defs = SwIniMap(DecodeText(std::string(kDefaultIni)));
    for (const SwOpt& o : kSwOpts) {
        if (!o.key || m.orig.count(o.key)) continue;
        std::wstring k = o.key;
        bool list = o.type == SwT::Lines || o.type == SwT::Pins;
        std::wstring d = defs.count(k) ? defs[k] : L"";
        std::wstring def = SwNorm(o, d, o.type == SwT::Int ? std::to_wstring(o.lo) : L"");
        std::wstring raw = o.type == SwT::Autostart ? (AutostartEnabled() ? L"1" : L"0")
                         : user.count(k) ? user[k] : list ? L"" : d;
        std::wstring v = SwNorm(o, raw, def);
        m.orig[k] = m.cur[k] = v;
        m.def[k] = o.type == SwT::Autostart ? v : def;
    }
    return m;
}

static int SwOptOf(const std::wstring& key) {
    for (int i = 0; i < kSwCount; ++i) if (kSwOpts[i].key && key == kSwOpts[i].key) return i;
    return -1;
}

static std::wstring SwGet(int opt) {
    const SwOpt& o = kSwOpts[opt];
    const std::wstring& v = g_sm.cur[o.key];
    if (o.type == SwT::Color && o.lo >= 0) {
        auto p = Split(v, L',');
        return o.lo < (int)p.size() ? p[o.lo] : L"#000000";
    }
    return v;
}

static void SwChanged();

static void SwPut(int opt, const std::wstring& val) {
    const SwOpt& o = kSwOpts[opt];
    std::wstring& v = g_sm.cur[o.key];
    if (o.type == SwT::Color && o.lo >= 0) {
        auto p = Split(v, L',');
        while (p.size() < 3) p.push_back(L"#000000");
        p[o.lo] = val;
        v = p[0] + L"," + p[1] + L"," + p[2];
    } else {
        v = val;
    }
    g_swApplied = false;
    SwChanged();
}

// The colour a swatch shows: its own, or for an unset "Default" colour, the one it follows.
static COLORREF SwColorOf(int opt) {
    const SwOpt& o = kSwOpts[opt];
    std::wstring v = SwGet(opt);
    if (o.type == SwT::AutoColor && v.empty()) v = g_sm.cur[o.extra];
    COLORREF c = 0;
    SwParseHex(v, c);
    return c;
}

static int SwDirty() {
    int n = 0;
    for (auto& kv : g_sm.cur) if (kv.second != g_sm.orig[kv.first]) ++n;
    return n;
}

static std::vector<SwPin> SwPins() {
    std::vector<SwPin> v;
    const std::wstring& s = g_sm.cur[L"pin"];
    if (s.empty()) return v;
    for (auto& line : Split(s, L'\n')) {
        SwPin p;
        auto parts = Split(line, L'|');
        for (size_t i = 0; i < 4 && i < parts.size(); ++i) p[i] = parts[i];
        v.push_back(p);
    }
    return v;
}

static void SwPinsPut(const std::vector<SwPin>& v) {
    std::wstring s;
    for (auto& p : v) {
        size_t n = 4;
        while (n > 2 && p[n - 1].empty()) --n;
        std::wstring line;
        for (size_t i = 0; i < n; ++i) line += (i ? L"|" : L"") + p[i];
        s += (s.empty() ? L"" : L"\n") + line;
    }
    SwPut(SwOptOf(L"pin"), s);
}

// ----- writing pbar.ini -----

// pbar.ini as lines, so a batch of settings is written in one go without disturbing comments or order.
struct SwIni {
    std::vector<std::wstring> lines;
    bool crlf = true;

    void Load() {
        std::wstring t = DecodeText(ReadFileBytes(g_iniPath));
        crlf = t.empty() || t.find(L"\r\n") != std::wstring::npos;
        size_t s = 0;
        while (s < t.size()) {
            size_t nl = t.find(L'\n', s);
            std::wstring l = t.substr(s, nl == std::wstring::npos ? std::wstring::npos : nl - s);
            if (!l.empty() && l.back() == L'\r') l.pop_back();
            lines.push_back(l);
            if (nl == std::wstring::npos) break;
            s = nl + 1;
        }
    }

    void Save() const {
        std::wstring out;
        for (auto& l : lines) out += l + (crlf ? L"\r\n" : L"\n");
        std::ofstream f(g_iniPath.c_str(), std::ios::binary);
        f << WToUtf8(out);
    }

    // The setting on a line (lowercase), or "" for a comment or blank; off = switched off with ';'.
    static std::wstring KeyOf(const std::wstring& line, bool& off) {
        std::wstring t = Trim(line);
        off = !t.empty() && t[0] == L';';
        if (off) t = Trim(t.substr(1));
        size_t eq = t.find(L'=');
        if (t.empty() || t[0] == L';' || t[0] == L'#' || eq == std::wstring::npos) return L"";
        std::wstring k = Lower(Trim(t.substr(0, eq)));
        return k.find_first_of(L" \t,:") == std::wstring::npos ? k : L"";   // a sentence with '=' in it isn't a key
    }

    std::vector<size_t> Find(const std::wstring& key, bool off) const {
        std::vector<size_t> at;
        bool o = false;
        for (size_t i = 0; i < lines.size(); ++i) if (KeyOf(lines[i], o) == key && o == off) at.push_back(i);
        return at;
    }

    // key=value lines go where the key's lines are now; with none, in place of a switched-off example of it
    // (";startcolor=#D8D8D8"), or after a commented one for a list ("; pin=Terminal|wt.exe"), else at the end.
    void Set(const std::wstring& key, const std::vector<std::wstring>& values) {
        std::vector<std::wstring> add;
        for (auto& v : values) add.push_back(key + L"=" + v);
        auto on = Find(key, false);
        size_t at = lines.size();
        if (!on.empty()) {
            at = on[0];
            for (size_t j = on.size(); j-- > 0;) lines.erase(lines.begin() + on[j]);
        } else {
            auto off = Find(key, true);
            if (!off.empty()) {
                at = off.back();
                if (key == L"pin" || key == L"strip") ++at;
                else lines.erase(lines.begin() + at);
            }
        }
        lines.insert(lines.begin() + at, add.begin(), add.end());
    }

    void Unset(const std::wstring& key) {
        for (size_t i : Find(key, false)) lines[i] = L";" + lines[i];
    }
};

// ----- layout and drawing -----

static bool SwIsField(int role) {
    return role == SR_NUM || role == SR_HEX || role == SR_TEXT || role == SR_HOTKEY || role == SR_LINES ||
           role == SR_PINFIELD || role == SR_PINLIST;
}

static int SwFind(int role, int idx = -1) {
    for (size_t i = 0; i < g_swCtls.size(); ++i)
        if (g_swCtls[i].role == role && (idx < 0 || g_swCtls[i].idx == idx)) return (int)i;
    return -1;
}

static int SwViewH() {
    RECT r;
    GetClientRect(g_swPanel, &r);
    return r.bottom;
}

// Where a control's window goes now: its rect, scrolled; for a field, the edit inside the frame drawn round it.
static RECT SwWinRect(const SwCtl& c) {
    RECT r = c.rc;
    OffsetRect(&r, 0, -g_swScroll);
    if (c.role == SR_LINES || c.role == SR_PINLIST) {
        InflateRect(&r, -SwS(5), -SwS(5));
    } else if (SwIsField(c.role)) {
        int eh = g_swFontH + 2;
        r.left += SwS(8);
        r.right -= SwS(8);
        r.top += (c.rc.bottom - c.rc.top - eh) / 2;
        r.bottom = r.top + eh;
    }
    return r;
}

static void SwPlace() {
    if (g_swCtls.empty()) return;
    HDWP dw = BeginDeferWindowPos((int)g_swCtls.size());
    for (auto& c : g_swCtls) {
        RECT r = SwWinRect(c);
        if (dw) dw = DeferWindowPos(dw, c.h, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top,
                                    SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (dw) EndDeferWindowPos(dw);
}

static void SwScrollTo(int y) {
    y = std::max(0, std::min(y, std::max(0, g_swContentH - SwViewH())));
    if (y == g_swScroll) return;
    int dy = g_swScroll - y;
    g_swScroll = y;
    ScrollWindowEx(g_swPanel, 0, dy, nullptr, nullptr, nullptr, nullptr, SW_SCROLLCHILDREN | SW_INVALIDATE);
    SwPlace();                                            // and any control that started out of sight
    UpdateWindow(g_swPanel);
    InvalidateRect(g_swSbar, nullptr, FALSE);
}

static void SwEnsureVisible(int i) {
    if (i < 0 || i >= (int)g_swCtls.size()) return;
    RECT r = g_swCtls[i].rc;
    int view = SwViewH(), m = SwS(28);
    if (r.top - m < g_swScroll) SwScrollTo(r.top - m);
    else if (r.bottom + m > g_swScroll + view) SwScrollTo(r.bottom + m - view);
}

// The bar's own button look: black outline, flat face, a short white glint at the top-left corner (raised)
// or the bottom-right (pressed).
static void SwBevel(HDC dc, RECT r, bool down, COLORREF face) {
    Fill(dc, r, RGB(0, 0, 0));
    RECT in{r.left + 1, r.top + 1, r.right - 1, r.bottom - 1};
    if (in.right <= in.left || in.bottom <= in.top) return;
    Fill(dc, in, face);
    int l = SwS(4), lx = std::min(l, (int)(in.right - in.left)), ly = std::min(l, (int)(in.bottom - in.top));
    if (!down) {
        Fill(dc, RECT{in.left, in.top, in.left + lx, in.top + 1}, kSwBright);
        Fill(dc, RECT{in.left, in.top, in.left + 1, in.top + ly}, kSwBright);
    } else {
        Fill(dc, RECT{in.right - lx, in.bottom - 1, in.right, in.bottom}, kSwBright);
        Fill(dc, RECT{in.right - 1, in.bottom - ly, in.right, in.bottom}, kSwBright);
    }
}

// A text field's frame: sunken like a pressed button, outlined in the accent colour while it has the focus.
static void SwField(HDC dc, RECT r, bool focus) {
    Fill(dc, r, focus ? kSwAccent : kSwEdge);
    RECT in{r.left + 1, r.top + 1, r.right - 1, r.bottom - 1};
    if (in.right <= in.left || in.bottom <= in.top) return;
    Fill(dc, in, kSwField);
}

static bool SwSegOn(const SwCtl& c) {
    std::wstring v = SwGet(c.opt);
    if (kSwOpts[c.opt].type == SwT::AutoColor) return c.val.empty() ? v.empty() : !v.empty();
    return v == c.val;
}

static void SwDrawPin(const DRAWITEMSTRUCT* d) {
    if (d->itemID == (UINT)-1) return;
    auto pins = SwPins();
    HDC dc = d->hDC;
    RECT r = d->rcItem;
    bool sel = (d->itemState & ODS_SELECTED) != 0;
    Fill(dc, r, sel ? kSwSel : kSwField);
    if (d->itemID >= pins.size()) return;
    const SwPin& p = pins[d->itemID];
    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ of = SelectObject(dc, g_swFont);
    std::wstring name = p[0].empty() ? FileStem(p[1]) : p[0];
    RECT t{r.left + SwS(8), r.top, r.right - SwS(6), r.bottom};
    SetTextColor(dc, kSwBright);
    DrawTextW(dc, name.c_str(), -1, &t, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    SIZE z{};
    GetTextExtentPoint32W(dc, name.c_str(), (int)name.size(), &z);
    t.left += z.cx + SwS(14);
    if (t.left < t.right) {
        SetTextColor(dc, sel ? RGB(205, 220, 245) : kSwDesc);
        std::wstring path = p[1];
        DrawTextW(dc, &path[0], -1, &t, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_PATH_ELLIPSIS);
    }
    SelectObject(dc, of);
}

static void SwDrawItem(const DRAWITEMSTRUCT* d) {
    if (d->CtlType == ODT_LISTBOX) { SwDrawPin(d); return; }
    HDC dc = d->hDC;
    RECT r = d->rcItem;
    int id = (int)d->CtlID;
    wchar_t buf[128];
    GetWindowTextW(d->hwndItem, buf, 128);
    std::wstring text = buf;
    bool sel = false, swatch = false, page = false, follows = false;
    int role = 0;
    COLORREF sw = 0;
    if (GetParent(d->hwndItem) == g_swHwnd && id >= SWID_PAGE0 && id < SWID_PAGE0 + (int)g_swPages.size()) {
        page = true;
        sel = id - SWID_PAGE0 == g_swPage;
        text = kSwOpts[g_swPages[id - SWID_PAGE0]].label;   // the window text doubles '&' for the dialog manager
    } else if (GetParent(d->hwndItem) == g_swPanel && id >= SWID_CTL0 && id - SWID_CTL0 < (int)g_swCtls.size()) {
        const SwCtl& c = g_swCtls[id - SWID_CTL0];
        role = c.role;
        if (c.role == SR_SEG) sel = SwSegOn(c);
        if (c.role == SR_SWATCH) {
            swatch = true;
            sw = SwColorOf(c.opt);
            follows = kSwOpts[c.opt].type == SwT::AutoColor && SwGet(c.opt).empty();
        }
    }
    bool dis = (d->itemState & ODS_DISABLED) != 0;
    bool down = (d->itemState & ODS_SELECTED) || sel;
    bool hot = d->hwndItem == g_swHot && !dis;
    COLORREF face = sel ? kSwSel : kSwFace;
    if (hot) face = Brighten(face, 26);
    SwBevel(dc, r, down, face);
    int sh = down ? 1 : 0;
    if (swatch) {
        RECT s{r.left + SwS(6) + sh, r.top + SwS(6) + sh, r.right - SwS(6) + sh, r.bottom - SwS(6) + sh};
        Fill(dc, s, RGB(0, 0, 0));
        InflateRect(&s, -1, -1);
        Fill(dc, s, sw);
        if (follows) {                                    // a default colour: hatched, to show it's borrowed
            int lum = (GetRValue(sw) * 30 + GetGValue(sw) * 59 + GetBValue(sw) * 11) / 100;
            COLORREF dot = lum > 110 ? RGB(0, 0, 0) : RGB(150, 150, 150);
            for (int y = s.top; y < s.bottom; ++y)
                for (int x = s.left; x < s.right; ++x)
                    if ((x + y) % 4 == 0) SetPixelV(dc, x, y, dot);
        }
    } else if (role == SR_MINUS || role == SR_PLUS) {        // drawn, so they stay crisp at any size
        COLORREF col = dis ? kSwDim : hot ? kSwBright : kSwText;
        int cx = (r.left + r.right) / 2 + sh, cy = (r.top + r.bottom) / 2 + sh, a = SwS(5), t = std::max(2, SwS(2));
        Fill(dc, RECT{cx - a, cy - t / 2, cx + a, cy - t / 2 + t}, col);
        if (role == SR_PLUS) Fill(dc, RECT{cx - t / 2, cy - a, cx - t / 2 + t, cy + a}, col);
    } else {
        HGDIOBJ of = SelectObject(dc, g_swFont);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, dis ? kSwDim : (sel || hot) ? kSwBright : kSwText);
        RECT t{r.left + (page ? SwS(14) : SwS(6)) + sh, r.top + sh, r.right - SwS(6) + sh, r.bottom + sh};
        DrawTextW(dc, text.c_str(), -1, &t,
                  (page ? DT_LEFT : DT_CENTER) | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        SelectObject(dc, of);
    }
    if ((d->itemState & ODS_FOCUS) && !(d->itemState & ODS_NOFOCUSRECT)) {
        RECT f = r;
        InflateRect(&f, -SwS(3), -SwS(3));
        SetTextColor(dc, RGB(255, 255, 255));
        SetBkColor(dc, RGB(0, 0, 0));
        DrawFocusRect(dc, &f);
    }
}

// Places one setting's controls in a w-wide column at (x, y); returns the height they take.
static int SwLayoutWidget(HDC dc, SwRow& r, int x, int y, int w, int ctlH) {
    const SwOpt& o = kSwOpts[r.opt];
    int gap = SwS(4);
    auto C = [&](int k) -> SwCtl& { return g_swCtls[r.first + k]; };
    auto tw = [&](const std::wstring& s) {
        HGDIOBJ of = SelectObject(dc, g_swFont);
        SIZE z{};
        GetTextExtentPoint32W(dc, s.c_str(), (int)s.size(), &z);
        SelectObject(dc, of);
        return (int)z.cx;
    };
    auto label = [&](int k) {
        wchar_t b[128];
        GetWindowTextW(C(k).h, b, 128);
        return std::wstring(b);
    };
    auto segs = [&](int from, int n, int x0, int maxW) {   // a row of segment buttons; returns its right edge
        std::vector<int> ws;
        int total = 0;
        for (int k = 0; k < n; ++k) {
            int sw = std::max(SwS(58), tw(label(from + k)) + SwS(28));
            ws.push_back(sw);
            total += sw + (k ? gap : 0);
        }
        double f = total > maxW ? (double)maxW / total : 1.0;
        int cx = x0;
        for (int k = 0; k < n; ++k) {
            int sw = (int)(ws[k] * f);
            C(from + k).rc = RECT{cx, y, cx + sw, y + ctlH};
            cx += sw + gap;
        }
        return cx - gap;
    };
    switch (o.type) {
    case SwT::Bool: case SwT::Choice: case SwT::Monitor: case SwT::Autostart:
        segs(0, r.count, x, w);
        return ctlH;
    case SwT::Int: {
        int b = SwS(30), ew = SwS(80);
        C(0).rc = RECT{x, y, x + b, y + ctlH};
        C(1).rc = RECT{x + b + gap, y, x + b + gap + ew, y + ctlH};
        int px = x + b + gap + ew + gap;
        C(2).rc = RECT{px, y, px + b, y + ctlH};
        int nx = px + b + SwS(10);
        std::wstring unit = o.extra ? o.extra : L"";
        if (!unit.empty()) {
            r.notes.push_back({RECT{nx, y, nx + tw(unit) + 2, y + ctlH}, unit, false});
            nx += tw(unit) + SwS(12);
        }
        r.notes.push_back({RECT{nx, y, x + w, y + ctlH}, std::to_wstring(o.lo) + L" – " + std::to_wstring(o.hi), true});
        return ctlH;
    }
    case SwT::Color: case SwT::AutoColor: {
        int swW = SwS(46), hexW = SwS(90), cx = x, k = 0;
        if (o.type == SwT::AutoColor) {
            cx = segs(0, 2, x, w - (SwS(12) + swW + SwS(8) + hexW)) + SwS(12);
            k = 2;
        }
        C(k).rc = RECT{cx, y, cx + swW, y + ctlH};
        cx += swW + SwS(8);
        C(k + 1).rc = RECT{cx, y, cx + hexW, y + ctlH};
        return ctlH;
    }
    case SwT::Text: C(0).rc = RECT{x, y, x + w, y + ctlH}; return ctlH;
    case SwT::Hotkey: C(0).rc = RECT{x, y, x + std::min(w, SwS(220)), y + ctlH}; return ctlH;
    case SwT::Font: case SwT::File: case SwT::Folder: {
        int bw = tw(label(1)) + SwS(30);
        C(0).rc = RECT{x, y, x + w - bw - SwS(6), y + ctlH};
        C(1).rc = RECT{x + w - bw, y, x + w, y + ctlH};
        return ctlH;
    }
    case SwT::Lines: {
        int h = SwS(12) + 8 * g_swFontH;
        C(0).rc = RECT{x, y, x + w, y + h};
        return h;
    }
    case SwT::Pins: {
        int bw = SwS(118), lh = SwS(26) * 6 + SwS(10);
        C(0).rc = RECT{x, y, x + w - bw - SwS(10), y + lh};
        int by = y;
        for (int k = 1; k <= 4; ++k) {
            C(k).rc = RECT{x + w - bw, by, x + w, by + ctlH};
            by += ctlH + SwS(6);
        }
        static const wchar_t* names[] = {L"Name", L"Program", L"Arguments", L"Start in"};
        int fy = y + lh + SwS(14), lab = SwS(96), k = 5, browseW = tw(L"Browse…") + SwS(30);
        for (int f = 0; f < 4; ++f) {
            r.notes.push_back({RECT{x, fy, x + lab, fy + ctlH}, names[f], false});
            bool br = f == 1 || f == 3;
            C(k++).rc = RECT{x + lab, fy, x + w - (br ? browseW + SwS(6) : 0), fy + ctlH};
            if (br) C(k++).rc = RECT{x + w - browseW, fy, x + w, fy + ctlH};
            fy += ctlH + SwS(8);
        }
        return fy - SwS(8) - y;
    }
    default: return 0;
    }
}

// Lays out the page: title and intro, then each setting with its name and description on the left and
// its controls on the right (stacked underneath when the window is narrow). Lists take the full width.
static void SwLayout() {
    if (!g_swPanel) return;
    RECT pc;
    GetClientRect(g_swPanel, &pc);
    HDC dc = GetDC(g_swPanel);
    int padL = SwS(30), padR = SwS(26), inner = std::max(SwS(200), (int)pc.right - padL - padR);
    int wc = SwS(310), cg = SwS(28), ctlH = SwS(30);
    bool side = inner - wc - cg >= SwS(250);
    int textW = side ? inner - wc - cg : inner, wx = padL + textW + cg;
    auto textH = [&](HFONT f, const wchar_t* s, int w) -> int {
        if (!s || !*s) return 0;
        RECT r{0, 0, w, 0};
        HGDIOBJ o = SelectObject(dc, f);
        DrawTextW(dc, s, -1, &r, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
        SelectObject(dc, o);
        return r.bottom;
    };
    int y = SwS(26);
    for (auto& r : g_swRows) {
        const SwOpt& o = kSwOpts[r.opt];
        r.notes.clear();
        if (o.type == SwT::Page) {
            int th = textH(g_swTitle, o.label, inner);
            r.label = RECT{padL, y, padL + inner, y + th};
            y += th + SwS(6);
            int dh = textH(g_swFont, o.desc, inner);
            r.desc = RECT{padL, y, padL + inner, y + dh};
            y += dh + SwS(4);
            continue;
        }
        if (o.type == SwT::Head) {
            y += SwS(34);
            int hh = textH(g_swHead, o.label, inner);
            r.label = RECT{padL, y, padL + inner, y + hh};
            y += hh + SwS(8);
            continue;
        }
        y += SwS(22);
        bool full = o.type == SwT::Lines || o.type == SwT::Pins, beside = side && !full;
        int lw = beside ? textW : inner;
        int lh = textH(g_swBold, o.label, lw), dh = textH(g_swFont, o.desc, lw);
        int top = beside ? y + std::max(0, (ctlH - lh) / 2) : y;
        r.label = RECT{padL, top, padL + lw, top + lh};
        r.desc = RECT{padL, top + lh + SwS(4), padL + lw, top + lh + SwS(4) + dh};
        if (beside) {
            int wh = SwLayoutWidget(dc, r, wx, y, wc, ctlH);
            y = std::max((int)r.desc.bottom, y + wh);
        } else {
            int wy = (dh ? r.desc.bottom : r.label.bottom) + SwS(10);
            y = wy + SwLayoutWidget(dc, r, padL, wy, full ? inner : std::min(inner, wc), ctlH);
        }
    }
    g_swContentH = y + SwS(34);
    ReleaseDC(g_swPanel, dc);
    g_swScroll = std::max(0, std::min(g_swScroll, g_swContentH - (int)pc.bottom));
    SwPlace();
    InvalidateRect(g_swPanel, nullptr, FALSE);
    InvalidateRect(g_swSbar, nullptr, FALSE);
}

static void SwPaintPanel(HWND h) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(h, &ps);
    RECT pc;
    GetClientRect(h, &pc);
    int W = std::max(1, (int)pc.right), H = std::max(1, (int)pc.bottom);
    HDC dc = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, W, H);
    HGDIOBJ ob = SelectObject(dc, bmp);
    Fill(dc, pc, kSwBg);
    SetBkMode(dc, TRANSPARENT);
    int oy = -g_swScroll;
    auto text = [&](HFONT f, COLORREF col, const std::wstring& s, RECT r, UINT fmt) {
        OffsetRect(&r, 0, oy);
        if (s.empty() || r.bottom < 0 || r.top > H) return;
        HGDIOBJ of = SelectObject(dc, f);
        SetTextColor(dc, col);
        DrawTextW(dc, s.c_str(), -1, &r, fmt | DT_NOPREFIX);
        SelectObject(dc, of);
    };
    for (auto& r : g_swRows) {
        const SwOpt& o = kSwOpts[r.opt];
        if (o.type == SwT::Page) {
            text(g_swTitle, kSwBright, o.label, r.label, DT_LEFT | DT_TOP | DT_SINGLELINE);
            text(g_swFont, kSwDesc, o.desc, r.desc, DT_LEFT | DT_TOP | DT_WORDBREAK);
            continue;
        }
        if (o.type == SwT::Head) {
            std::wstring up = o.label;
            CharUpperBuffW(&up[0], (DWORD)up.size());
            SetTextCharacterExtra(dc, SwS(1));
            text(g_swHead, kSwAccent, up, r.label, DT_LEFT | DT_TOP | DT_SINGLELINE);
            SetTextCharacterExtra(dc, 0);
            int ly = r.label.bottom + SwS(6) + oy;
            Fill(dc, RECT{r.label.left, ly, r.label.right, ly + 1}, kSwLine);
            continue;
        }
        text(g_swBold, kSwText, o.label, r.label, DT_LEFT | DT_TOP | DT_WORDBREAK);
        if (o.desc) text(g_swFont, kSwDesc, o.desc, r.desc, DT_LEFT | DT_TOP | DT_WORDBREAK);
        for (auto& n : r.notes) text(g_swFont, n.dim ? kSwDim : kSwDesc, n.text, n.r, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }
    HWND focus = GetFocus();
    for (auto& c : g_swCtls) {
        if (!SwIsField(c.role)) continue;
        RECT r = c.rc;
        OffsetRect(&r, 0, oy);
        if (r.bottom < 0 || r.top > H) continue;
        SwField(dc, r, c.h == focus);
        if (c.role == SR_PINLIST && SwPins().empty()) {
            HGDIOBJ of = SelectObject(dc, g_swFont);
            SetTextColor(dc, kSwDim);
            DrawTextW(dc, L"No pins of your own yet. Add\u2026 picks a program or shortcut.", -1, &r,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
            SelectObject(dc, of);
        }
    }
    BitBlt(hdc, ps.rcPaint.left, ps.rcPaint.top, ps.rcPaint.right - ps.rcPaint.left, ps.rcPaint.bottom - ps.rcPaint.top,
           dc, ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);
    SelectObject(dc, ob);
    DeleteObject(bmp);
    DeleteDC(dc);
    EndPaint(h, &ps);
}

// ----- building a page -----

static LRESULT CALLBACK SwBtnProc(HWND h, UINT msg, WPARAM wp, LPARAM lp);
static LRESULT CALLBACK SwNumProc(HWND h, UINT msg, WPARAM wp, LPARAM lp);
static LRESULT CALLBACK SwHotkeyProc(HWND h, UINT msg, WPARAM wp, LPARAM lp);

typedef HRESULT(WINAPI* SwSetWindowThemeFn)(HWND, LPCWSTR, LPCWSTR);

// Dark scroll bars on the lists, where Windows offers them.
static void SwDarkScrollbars(HWND h) {
    static SwSetWindowThemeFn fn = nullptr;
    static bool looked = false;
    if (!looked) {
        looked = true;
        if (HMODULE m = LoadLibraryW(L"uxtheme.dll")) fn = (SwSetWindowThemeFn)(void*)GetProcAddress(m, "SetWindowTheme");
    }
    if (fn) fn(h, L"DarkMode_Explorer", nullptr);
}

static HWND SwMake(int opt, int role, int idx, const std::wstring& val, const std::wstring& text) {
    SwCtl c;
    c.opt = opt;
    c.role = role;
    c.idx = idx;
    c.val = val;
    int id = SWID_CTL0 + (int)g_swCtls.size();
    const wchar_t* cls = L"BUTTON";
    DWORD style = WS_CHILD | WS_VISIBLE | WS_TABSTOP;
    if (role == SR_PINLIST) {
        cls = L"LISTBOX";
        style |= LBS_NOTIFY | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT | WS_VSCROLL;
    } else if (role == SR_LINES) {
        cls = L"EDIT";
        style |= ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN;
    } else if (SwIsField(role)) {
        cls = L"EDIT";
        style |= ES_AUTOHSCROLL | (role == SR_NUM ? ES_CENTER : 0) | (role == SR_HEX ? ES_UPPERCASE : 0);
    } else {
        style |= BS_OWNERDRAW | BS_NOTIFY;
    }
    c.h = CreateWindowExW(0, cls, role == SR_PINLIST ? L"" : text.c_str(), style, 0, 0, 10, 10, g_swPanel,
                          (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(c.h, WM_SETFONT, (WPARAM)g_swFont, FALSE);
    if (!SwIsField(role)) {
        WNDPROC old = (WNDPROC)SetWindowLongPtrW(c.h, GWLP_WNDPROC, (LONG_PTR)SwBtnProc);
        if (!g_swBtnBase) g_swBtnBase = old;
    } else if (role == SR_NUM || role == SR_HOTKEY) {
        WNDPROC old = (WNDPROC)SetWindowLongPtrW(c.h, GWLP_WNDPROC, (LONG_PTR)(role == SR_NUM ? SwNumProc : SwHotkeyProc));
        if (!g_swEditBase) g_swEditBase = old;
    }
    if (role == SR_PINLIST) SwDarkScrollbars(c.h);
    g_swCtls.push_back(c);
    return c.h;
}

static void SwStatusUpdate() {
    if (!g_swHwnd) return;
    EnableWindow(GetDlgItem(g_swHwnd, SWID_APPLY), SwDirty() > 0);
    InvalidateRect(g_swHwnd, &g_swStatusR, FALSE);
}

// After any change: segments and swatches show the new state, and a Default colour's box shows the
// colour it's borrowing.
static void SwChanged() {
    for (auto& c : g_swCtls) {
        if (c.role == SR_SEG || c.role == SR_SWATCH) InvalidateRect(c.h, nullptr, FALSE);
        if (c.role == SR_HEX && kSwOpts[c.opt].type == SwT::AutoColor) {
            bool custom = !SwGet(c.opt).empty();
            if ((IsWindowEnabled(c.h) != FALSE) != custom) EnableWindow(c.h, custom);
            if (GetFocus() != c.h) {
                std::wstring t = SwHex(SwColorOf(c.opt));
                wchar_t b[16];
                GetWindowTextW(c.h, b, 16);
                if (t != b) {
                    g_swQuiet = true;
                    SetWindowTextW(c.h, t.c_str());
                    g_swQuiet = false;
                }
            }
        }
    }
    SwStatusUpdate();
}

// The pin list and the fields for the selected pin.
static void SwPinsShow() {
    int li = SwFind(SR_PINLIST);
    if (li < 0) return;
    HWND lb = g_swCtls[li].h;
    auto pins = SwPins();
    int n = (int)pins.size();
    g_swPinSel = n ? std::max(0, std::min(g_swPinSel, n - 1)) : -1;
    SendMessageW(lb, WM_SETREDRAW, FALSE, 0);
    SendMessageW(lb, LB_RESETCONTENT, 0, 0);
    for (auto& p : pins) SendMessageW(lb, LB_ADDSTRING, 0, (LPARAM)p[0].c_str());
    SendMessageW(lb, LB_SETCURSEL, (WPARAM)g_swPinSel, 0);
    SendMessageW(lb, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(lb, nullptr, TRUE);
    ShowWindow(lb, n ? SW_SHOWNA : SW_HIDE);              // with none, the panel shows a hint in its place
    RECT fr = g_swCtls[li].rc;
    OffsetRect(&fr, 0, -g_swScroll);
    InvalidateRect(g_swPanel, &fr, FALSE);
    bool has = g_swPinSel >= 0;
    g_swQuiet = true;
    for (int f = 0; f < 4; ++f) {
        int fi = SwFind(SR_PINFIELD, f);
        if (fi < 0) continue;
        SetWindowTextW(g_swCtls[fi].h, has ? pins[g_swPinSel][f].c_str() : L"");
        EnableWindow(g_swCtls[fi].h, has);
    }
    g_swQuiet = false;
    for (int f : {1, 3}) {
        int bi = SwFind(SR_PINBROWSE, f);
        if (bi >= 0) EnableWindow(g_swCtls[bi].h, has);
    }
    auto en = [&](int role, bool on) {
        int i = SwFind(role);
        if (i >= 0) EnableWindow(g_swCtls[i].h, on);
    };
    en(SR_PINDEL, has);
    en(SR_PINUP, has && g_swPinSel > 0);
    en(SR_PINDOWN, has && g_swPinSel < n - 1);
}

static void SwBuild(bool keepScroll) {
    if (!g_swPanel) return;
    int focusId = 0;
    HWND f = GetFocus();
    if (keepScroll && f && GetParent(f) == g_swPanel) focusId = GetDlgCtrlID(f);
    SendMessageW(g_swPanel, WM_SETREDRAW, FALSE, 0);
    g_swHot = nullptr;
    for (auto& c : g_swCtls) DestroyWindow(c.h);
    g_swCtls.clear();
    g_swRows.clear();
    if (!keepScroll) g_swScroll = 0;
    for (int i = g_swPages[g_swPage]; i < SwPageEnd(g_swPage); ++i) {
        const SwOpt& o = kSwOpts[i];
        SwRow r;
        r.opt = i;
        r.first = (int)g_swCtls.size();
        std::wstring v = o.key ? SwGet(i) : L"";
        switch (o.type) {
        case SwT::Bool: case SwT::Choice: case SwT::Monitor: case SwT::Autostart: {
            auto ch = SwChoices(o);
            bool found = false;
            for (auto& x : ch) found = found || x.val == v;
            if (!found && o.type == SwT::Monitor) ch.push_back({v, v});   // set, but not plugged in right now
            for (size_t k = 0; k < ch.size(); ++k) SwMake(i, SR_SEG, (int)k, ch[k].val, ch[k].label);
            break;
        }
        case SwT::Int:
            SwMake(i, SR_MINUS, 0, L"", L"−");
            SwMake(i, SR_NUM, 0, L"", v);
            SwMake(i, SR_PLUS, 0, L"", L"+");
            break;
        case SwT::Color: case SwT::AutoColor:
            if (o.type == SwT::AutoColor)
                for (auto& x : SwChoices(o)) SwMake(i, SR_SEG, 0, x.val, x.label);
            SwMake(i, SR_SWATCH, 0, L"", L"");
            SwMake(i, SR_HEX, 0, L"", SwHex(SwColorOf(i)));
            if (o.type == SwT::AutoColor && v.empty()) EnableWindow(g_swCtls.back().h, FALSE);
            break;
        case SwT::Text: SwMake(i, SR_TEXT, 0, L"", v); break;
        case SwT::Font:
            SwMake(i, SR_TEXT, 0, L"", v);
            SwMake(i, SR_PICK, 0, L"", L"Pick…");
            break;
        case SwT::File: case SwT::Folder:
            SwMake(i, SR_TEXT, 0, L"", v);
            SwMake(i, SR_BROWSE, 0, L"", L"Browse…");
            break;
        case SwT::Hotkey: SwMake(i, SR_HOTKEY, 0, L"", v); break;
        case SwT::Lines: {
            std::wstring t;
            for (auto& l : Split(v, L'\n')) if (!l.empty()) t += l + L"\r\n";
            SwMake(i, SR_LINES, 0, L"", t);
            break;
        }
        case SwT::Pins:
            SwMake(i, SR_PINLIST, 0, L"", L"");
            SwMake(i, SR_PINADD, 0, L"", L"Add…");
            SwMake(i, SR_PINDEL, 0, L"", L"Remove");
            SwMake(i, SR_PINUP, 0, L"", L"Move up");
            SwMake(i, SR_PINDOWN, 0, L"", L"Move down");
            for (int fld = 0; fld < 4; ++fld) {
                SwMake(i, SR_PINFIELD, fld, L"", L"");
                if (fld == 1 || fld == 3) SwMake(i, SR_PINBROWSE, fld, L"", L"Browse…");
            }
            break;
        default: break;
        }
        r.count = (int)g_swCtls.size() - r.first;
        g_swRows.push_back(r);
    }
    SwPinsShow();
    SwLayout();
    SendMessageW(g_swPanel, WM_SETREDRAW, TRUE, 0);
    RedrawWindow(g_swPanel, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME);
    if (focusId)
        if (HWND nf = GetDlgItem(g_swPanel, focusId)) SetFocus(nf);
    for (int p = 0; p < (int)g_swPages.size(); ++p)
        if (HWND b = GetDlgItem(g_swHwnd, SWID_PAGE0 + p)) InvalidateRect(b, nullptr, FALSE);
    InvalidateRect(g_swSbar, nullptr, FALSE);
    SwStatusUpdate();
}

// ----- editing -----

// Tidies a field you typed in: a number is kept in its range, a colour rewritten as #RRGGBB.
static void SwCommitCtl(int i) {
    if (i < 0 || i >= (int)g_swCtls.size()) return;
    const SwCtl& c = g_swCtls[i];
    if (c.role != SR_NUM && c.role != SR_HEX) return;
    wchar_t buf[64];
    GetWindowTextW(c.h, buf, 64);
    std::wstring t;
    if (c.role == SR_NUM) {
        const SwOpt& o = kSwOpts[c.opt];
        int n;
        t = SwParseInt(buf, n) ? std::to_wstring(std::max(o.lo, std::min(o.hi, n)))
                               : SwNorm(o, SwGet(c.opt), g_sm.orig[o.key]);
        if (t != SwGet(c.opt)) SwPut(c.opt, t);
    } else {
        t = SwHex(SwColorOf(c.opt));
    }
    if (t != buf) {
        g_swQuiet = true;
        SetWindowTextW(c.h, t.c_str());
        g_swQuiet = false;
    }
}

static void SwCommit() {
    HWND f = GetFocus();
    for (size_t i = 0; i < g_swCtls.size(); ++i) if (g_swCtls[i].h == f) SwCommitCtl((int)i);
}

// -/+ (or the arrow keys in the box): one step, snapped to the step size, kept in range.
static void SwStep(int i, int dir) {
    if (i < 0 || i >= (int)g_swCtls.size()) return;
    int opt = g_swCtls[i].opt;
    const SwOpt& o = kSwOpts[opt];
    int n;
    if (!SwParseInt(SwGet(opt), n)) n = o.lo;
    int s = std::max(1, o.step);
    if (s == 1) n += dir;
    else n = dir > 0 ? (n / s + 1) * s : ((n + s - 1) / s - 1) * s;
    n = std::max(o.lo, std::min(o.hi, n));
    std::wstring v = std::to_wstring(n);
    SwPut(opt, v);
    for (auto& c : g_swCtls)
        if (c.opt == opt && c.role == SR_NUM) {
            g_swQuiet = true;
            SetWindowTextW(c.h, v.c_str());
            g_swQuiet = false;
        }
}

static void SwPickColor(int opt) {
    static bool seeded = false;
    if (!seeded) {                                       // the picker's custom colours start as pbar's defaults
        const COLORREF d[16] = {RGB(0, 0, 0), RGB(12, 12, 12), RGB(64, 64, 64), RGB(118, 118, 118),
                                RGB(136, 136, 136), RGB(204, 204, 204), RGB(216, 216, 216), RGB(255, 255, 255),
                                RGB(47, 125, 255), RGB(38, 79, 120), RGB(255, 159, 10), RGB(250, 17, 79),
                                RGB(146, 232, 42), RGB(30, 234, 239), RGB(231, 72, 86), RGB(242, 242, 242)};
        memcpy(g_swCustom, d, sizeof(d));
        seeded = true;
    }
    CHOOSECOLORW cc{};
    cc.lStructSize = sizeof(cc);
    cc.hwndOwner = g_swHwnd;
    cc.rgbResult = SwColorOf(opt);
    cc.lpCustColors = g_swCustom;
    cc.Flags = CC_FULLOPEN | CC_RGBINIT | CC_ANYCOLOR;
    if (!ChooseColorW(&cc) || !g_swHwnd) return;
    std::wstring v = SwHex(cc.rgbResult);
    SwPut(opt, v);
    for (auto& c : g_swCtls)
        if (c.opt == opt && c.role == SR_HEX) {
            EnableWindow(c.h, TRUE);
            g_swQuiet = true;
            SetWindowTextW(c.h, v.c_str());
            g_swQuiet = false;
        }
}

static bool SwPickFont(std::wstring& face) {
    LOGFONTW lf{};
    lf.lfHeight = -SwS(16);
    lf.lfCharSet = DEFAULT_CHARSET;
    wcsncpy(lf.lfFaceName, face.c_str(), LF_FACESIZE - 1);
    CHOOSEFONTW cf{};
    cf.lStructSize = sizeof(cf);
    cf.hwndOwner = g_swHwnd;
    cf.lpLogFont = &lf;
    cf.Flags = CF_SCREENFONTS | CF_INITTOLOGFONTSTRUCT | CF_NOSIZESEL | CF_NOSTYLESEL | CF_NOSCRIPTSEL | CF_NOVERTFONTS;
    if (!ChooseFontW(&cf)) return false;
    face = lf.lfFaceName;
    return true;
}

static const GUID kCLSID_FileOpenDialog = {0xDC1C5A9C, 0xE88A, 0x4DDE, {0xA5, 0xA1, 0x60, 0xF8, 0x2A, 0x20, 0xAE, 0xF7}};
static const GUID kIID_IFileOpenDialog = {0xD57C7288, 0xD4AD, 0x4768, {0xBE, 0x02, 0x9D, 0x96, 0x95, 0x32, 0xD9, 0x60}};
static const GUID kIID_IShellItem = {0x43826D1E, 0xE718, 0x42EE, {0xBC, 0x55, 0xA1, 0xE2, 0x61, 0xC3, 0x7B, 0xFE}};

// The Windows file (or folder) picker, opening where the current value points. links = return a chosen
// shortcut itself rather than what it points to.
static bool SwPickPath(bool folder, bool links, const wchar_t* filterName, const wchar_t* filterSpec,
                       const std::wstring& current, std::wstring& out) {
    IFileOpenDialog* d = nullptr;
    if (FAILED(CoCreateInstance(kCLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, kIID_IFileOpenDialog, (void**)&d)) || !d)
        return false;
    FILEOPENDIALOGOPTIONS o = 0;
    d->GetOptions(&o);
    o |= FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR | (folder ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST);
    if (links) o |= FOS_NODEREFERENCELINKS;
    d->SetOptions(o);
    if (!folder && filterSpec) {
        COMDLG_FILTERSPEC fs[2] = {{filterName, filterSpec}, {L"All files", L"*.*"}};
        d->SetFileTypes(2, fs);
    }
    std::wstring dir = Trim(current).empty() ? L"" : Resolve(Trim(current));
    if (!dir.empty() && !IsDir(dir)) dir = dir.substr(0, dir.find_last_of(L"\\/") == std::wstring::npos ? 0 : dir.find_last_of(L"\\/"));
    if (!dir.empty() && IsDir(dir)) {
        IShellItem* si = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(dir.c_str(), nullptr, kIID_IShellItem, (void**)&si)) && si) {
            d->SetFolder(si);
            si->Release();
        }
    }
    bool ok = false;
    if (SUCCEEDED(d->Show(g_swHwnd))) {
        IShellItem* it = nullptr;
        if (SUCCEEDED(d->GetResult(&it)) && it) {
            PWSTR p = nullptr;
            if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) {
                out = p;
                CoTaskMemFree(p);
                ok = true;
            }
            it->Release();
        }
    }
    d->Release();
    return ok && g_swHwnd;
}

// A path inside pbar's own folder is stored relative to it, so the folder still works if you move it.
static std::wstring SwRelative(const std::wstring& p) {
    std::wstring base = g_exeDir + L"\\";
    if (p.size() > base.size() && _wcsnicmp(p.c_str(), base.c_str(), base.size()) == 0) return p.substr(base.size());
    return p;
}

static void SwPinCmd(int role, int idx) {
    auto pins = SwPins();
    int s = g_swPinSel, n = (int)pins.size();
    static const wchar_t* kProgs = L"*.exe;*.lnk;*.url;*.bat;*.cmd;*.msc";
    switch (role) {
    case SR_PINADD: {
        std::wstring path;
        if (!SwPickPath(false, true, L"Programs and shortcuts", kProgs, L"", path)) return;
        pins.push_back(SwPin{FileStem(path), path, L"", L""});
        s = (int)pins.size() - 1;
        break;
    }
    case SR_PINDEL:
        if (s < 0 || s >= n) return;
        pins.erase(pins.begin() + s);
        s = std::min(s, (int)pins.size() - 1);
        break;
    case SR_PINUP:
        if (s <= 0 || s >= n) return;
        std::swap(pins[s], pins[s - 1]);
        --s;
        break;
    case SR_PINDOWN:
        if (s < 0 || s >= n - 1) return;
        std::swap(pins[s], pins[s + 1]);
        ++s;
        break;
    case SR_PINBROWSE: {
        if (s < 0 || s >= n) return;
        std::wstring path;
        if (!SwPickPath(idx == 3, idx == 1, L"Programs and shortcuts", idx == 1 ? kProgs : nullptr, pins[s][idx], path))
            return;
        pins[s][idx] = path;
        if (idx == 1 && pins[s][0].empty()) pins[s][0] = FileStem(path);
        break;
    }
    default: return;
    }
    g_swPinSel = s;
    SwPinsPut(pins);
    SwPinsShow();
    if (role == SR_PINADD) {
        int fi = SwFind(SR_PINFIELD, 0);
        if (fi >= 0) {
            SetFocus(g_swCtls[fi].h);
            SendMessageW(g_swCtls[fi].h, EM_SETSEL, 0, -1);
        }
    }
}

static void SwCommand(int id, int code) {
    int i = id - SWID_CTL0;
    if (i < 0 || i >= (int)g_swCtls.size()) return;
    SwCtl c = g_swCtls[i];                                 // a copy: a picker dialog can outlive the page
    const SwOpt& o = kSwOpts[c.opt];
    bool click = code == BN_CLICKED || code == BN_DOUBLECLICKED;
    auto text = [&]() {
        int n = GetWindowTextLengthW(c.h);
        std::wstring t(n + 1, L'\0');
        GetWindowTextW(c.h, &t[0], n + 1);
        t.resize(wcslen(t.c_str()));
        return t;
    };
    bool field = SwIsField(c.role) && c.role != SR_PINLIST;
    switch (c.role) {
    case SR_SEG:
        if (!click) break;
        if (o.type == SwT::AutoColor) {
            if (c.val.empty()) SwPut(c.opt, L"");
            else if (SwGet(c.opt).empty()) SwPut(c.opt, SwHex(SwColorOf(c.opt)));   // start from what it looks like now
        } else {
            SwPut(c.opt, c.val);
        }
        break;
    case SR_MINUS: case SR_PLUS:
        if (click) SwStep(i, c.role == SR_PLUS ? 1 : -1);   // keyboard; the mouse goes through SwBtnProc
        break;
    case SR_NUM:
        if (code == EN_CHANGE && !g_swQuiet) {
            int n;
            if (SwParseInt(text(), n)) SwPut(c.opt, std::to_wstring(n));
        } else if (code == EN_KILLFOCUS) {
            SwCommitCtl(i);
        }
        break;
    case SR_HEX:
        if (code == EN_CHANGE && !g_swQuiet) {
            COLORREF col;
            if (SwParseHex(text(), col)) SwPut(c.opt, SwHex(col));
        } else if (code == EN_KILLFOCUS) {
            SwCommitCtl(i);
        }
        break;
    case SR_SWATCH:
        if (click) SwPickColor(c.opt);
        break;
    case SR_TEXT: case SR_HOTKEY:
        if (code == EN_CHANGE && !g_swQuiet) SwPut(c.opt, Trim(text()));
        if (c.role == SR_HOTKEY && code == EN_SETFOCUS) UnregisterHotKey(g_hwnd, HOTKEY_TERM);   // let the keys reach the box
        if (c.role == SR_HOTKEY && code == EN_KILLFOCUS) RegisterTermHotkey();
        break;
    case SR_LINES:
        if (code == EN_CHANGE && !g_swQuiet) SwPut(c.opt, SwNorm(o, text(), L""));
        break;
    case SR_PICK:
        if (click) {
            std::wstring face = SwGet(c.opt);
            if (SwPickFont(face) && i - 1 >= 0 && i - 1 < (int)g_swCtls.size()) SetWindowTextW(g_swCtls[i - 1].h, face.c_str());
        }
        break;
    case SR_BROWSE:
        if (click) {
            bool folder = o.type == SwT::Folder;
            std::wstring path;
            if (SwPickPath(folder, false, L"Fonts", folder ? nullptr : L"*.ttf;*.otf;*.fon;*.fnt", SwGet(c.opt), path) &&
                i - 1 >= 0 && i - 1 < (int)g_swCtls.size())
                SetWindowTextW(g_swCtls[i - 1].h, SwRelative(path).c_str());
        }
        break;
    case SR_PINLIST:
        if (code == LBN_SELCHANGE) {
            g_swPinSel = (int)SendMessageW(c.h, LB_GETCURSEL, 0, 0);
            SwPinsShow();
        }
        break;
    case SR_PINADD: case SR_PINDEL: case SR_PINUP: case SR_PINDOWN: case SR_PINBROWSE:
        if (click) SwPinCmd(c.role, c.idx);
        break;
    case SR_PINFIELD:
        if (code == EN_CHANGE && !g_swQuiet) {
            auto pins = SwPins();
            if (g_swPinSel >= 0 && g_swPinSel < (int)pins.size()) {
                std::wstring t = text();
                std::replace(t.begin(), t.end(), L'|', L'/');   // '|' separates the fields in pbar.ini
                pins[g_swPinSel][c.idx] = Trim(t);
                SwPinsPut(pins);
                int li = SwFind(SR_PINLIST);
                if (li >= 0) InvalidateRect(g_swCtls[li].h, nullptr, FALSE);
            }
        }
        break;
    }
    // focus moved: the field's frame changes colour, and by keyboard the control scrolls into view
    bool gotFocus = field ? code == EN_SETFOCUS : c.role == SR_PINLIST ? code == LBN_SETFOCUS : code == BN_SETFOCUS;
    bool lostFocus = field ? code == EN_KILLFOCUS : c.role == SR_PINLIST && code == LBN_KILLFOCUS;
    if ((gotFocus || lostFocus) && SwIsField(c.role)) {
        RECT r = c.rc;
        OffsetRect(&r, 0, -g_swScroll);
        InflateRect(&r, 1, 1);
        InvalidateRect(g_swPanel, &r, FALSE);
    }
    if (gotFocus && GetKeyState(VK_LBUTTON) >= 0) SwEnsureVisible(i);
}

// ----- control behaviour -----

// Owner-drawn buttons: light up under the mouse; -/+ repeat while held.
static LRESULT CALLBACK SwBtnProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_MOUSEMOVE:
        if (g_swHot != h) {
            if (g_swHot) InvalidateRect(g_swHot, nullptr, FALSE);
            g_swHot = h;
            InvalidateRect(h, nullptr, FALSE);
            TRACKMOUSEEVENT t{sizeof(t), TME_LEAVE, h, 0};
            TrackMouseEvent(&t);
        }
        break;
    case WM_MOUSELEAVE:
        if (g_swHot == h) {
            g_swHot = nullptr;
            InvalidateRect(h, nullptr, FALSE);
        }
        break;
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: {
        int i = GetDlgCtrlID(h) - SWID_CTL0;
        if (GetParent(h) == g_swPanel && i >= 0 && i < (int)g_swCtls.size() &&
            (g_swCtls[i].role == SR_MINUS || g_swCtls[i].role == SR_PLUS)) {
            SetCapture(h);
            SendMessageW(h, BM_SETSTATE, TRUE, 0);
            g_swRepeat = GetDlgCtrlID(h);
            SwStep(i, g_swCtls[i].role == SR_PLUS ? 1 : -1);
            SetTimer(g_swPanel, TIMER_SWREPEAT, 400, nullptr);
            return 0;
        }
        break;
    }
    case WM_LBUTTONUP: case WM_CAPTURECHANGED:
        if (g_swRepeat && g_swRepeat == GetDlgCtrlID(h) && GetParent(h) == g_swPanel) {
            g_swRepeat = 0;
            KillTimer(g_swPanel, TIMER_SWREPEAT);
            SendMessageW(h, BM_SETSTATE, FALSE, 0);
            if (msg == WM_LBUTTONUP) ReleaseCapture();
            return 0;
        }
        break;
    }
    return CallWindowProcW(g_swBtnBase, h, msg, wp, lp);
}

// Number boxes: digits only (and a minus), and the arrow keys step like -/+.
static LRESULT CALLBACK SwNumProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_KEYDOWN && (wp == VK_UP || wp == VK_DOWN)) {
        int i = GetDlgCtrlID(h) - SWID_CTL0;
        SwCommitCtl(i);
        SwStep(i, wp == VK_UP ? 1 : -1);
        return 0;
    }
    if (msg == WM_CHAR && wp >= 32 && !iswdigit((wint_t)wp) && wp != L'-') return 0;
    return CallWindowProcW(g_swEditBase, h, msg, wp, lp);
}

static std::wstring SwKeyName(UINT vk) {
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) return std::wstring(1, (wchar_t)vk);
    if (vk >= VK_F1 && vk <= VK_F24) return L"F" + std::to_wstring(vk - VK_F1 + 1);
    switch (vk) {
    case VK_SPACE: return L"Space";
    case VK_RETURN: return L"Enter";
    case VK_TAB: return L"Tab";
    case VK_ESCAPE: return L"Esc";
    case VK_OEM_3: return L"`";
    }
    UINT ch = MapVirtualKeyW(vk, MAPVK_VK_TO_CHAR) & 0x7FFF;
    return ch > 32 && ch < 127 ? std::wstring(1, (wchar_t)towlower((wint_t)ch)) : L"";
}

// The hotkey box records the combination you press, in the form termkey= takes ("Ctrl+Alt+T").
static LRESULT CALLBACK SwHotkeyProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    bool ctrl = GetKeyState(VK_CONTROL) < 0, alt = GetKeyState(VK_MENU) < 0, shift = GetKeyState(VK_SHIFT) < 0,
         win = GetKeyState(VK_LWIN) < 0 || GetKeyState(VK_RWIN) < 0;
    switch (msg) {
    case WM_GETDLGCODE: {
        const MSG* m = (const MSG*)lp;
        if (m && m->message == WM_KEYDOWN && m->wParam == VK_TAB && !ctrl && !alt && !win) return 0;   // Tab moves on
        return DLGC_WANTALLKEYS | DLGC_WANTCHARS;
    }
    case WM_KEYDOWN: case WM_SYSKEYDOWN: {
        UINT vk = (UINT)wp;
        if (vk == VK_CONTROL || vk == VK_MENU || vk == VK_SHIFT || vk == VK_LWIN || vk == VK_RWIN) return 0;
        if ((vk == VK_BACK || vk == VK_DELETE) && !ctrl && !alt && !win) {
            SetWindowTextW(h, L"");
            return 0;
        }
        std::wstring key = SwKeyName(vk);
        bool fkey = vk >= VK_F1 && vk <= VK_F24;
        if (key.empty() || (!ctrl && !alt && !win && !fkey)) return 0;   // a lone key would steal it from every app
        std::wstring s = std::wstring(ctrl ? L"Ctrl+" : L"") + (alt ? L"Alt+" : L"") + (shift ? L"Shift+" : L"") +
                         (win ? L"Win+" : L"") + key;
        SetWindowTextW(h, s.c_str());
        SendMessageW(h, EM_SETSEL, s.size(), s.size());
        return 0;
    }
    case WM_CHAR: case WM_SYSCHAR: case WM_KEYUP: case WM_SYSKEYUP: case WM_PASTE: case WM_CUT: case WM_CONTEXTMENU:
        return 0;
    }
    return CallWindowProcW(g_swEditBase, h, msg, wp, lp);
}

// ----- the windows -----

static bool SwThumb(RECT& t, int& travel) {
    RECT sc;
    GetClientRect(g_swSbar, &sc);
    int view = SwViewH();
    if (g_swContentH <= view || sc.bottom < SwS(24)) return false;
    int track = sc.bottom - SwS(8);
    int th = std::max(SwS(36), (int)((long long)track * view / g_swContentH));
    travel = std::max(1, track - th);
    int ty = SwS(4) + (int)((long long)travel * g_swScroll / (g_swContentH - view));
    t = RECT{SwS(2), ty, sc.right - SwS(2), ty + th};
    return true;
}

static LRESULT CALLBACK SwSbarProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT cr;
        GetClientRect(h, &cr);
        Fill(dc, cr, kSwBg);
        RECT t;
        int travel;
        if (SwThumb(t, travel)) SwBevel(dc, t, false, g_swDragY >= 0 ? RGB(110, 110, 110) : RGB(80, 80, 80));
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_LBUTTONDOWN: {
        RECT t;
        int travel;
        POINT p{(short)LOWORD(lp), (short)HIWORD(lp)};
        if (!SwThumb(t, travel)) return 0;
        if (PtInRect(&t, p)) {
            g_swDragY = p.y;
            g_swDragScroll = g_swScroll;
            SetCapture(h);
            InvalidateRect(h, nullptr, FALSE);
        } else {
            SwScrollTo(g_swScroll + (p.y < t.top ? -1 : 1) * (SwViewH() - SwS(40)));
        }
        return 0;
    }
    case WM_MOUSEMOVE:
        if (g_swDragY >= 0) {
            RECT t;
            int travel;
            if (SwThumb(t, travel))
                SwScrollTo(g_swDragScroll + (int)((long long)((short)HIWORD(lp) - g_swDragY) * (g_swContentH - SwViewH()) / travel));
        }
        return 0;
    case WM_LBUTTONUP: case WM_CAPTURECHANGED:
        if (g_swDragY >= 0) {
            g_swDragY = -1;
            if (msg == WM_LBUTTONUP) ReleaseCapture();
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    case WM_MOUSEWHEEL: return SendMessageW(g_swPanel, msg, wp, lp);
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static LRESULT CALLBACK SwPanelProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: SwPaintPanel(h); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_SIZE: SwLayout(); return 0;
    case WM_MOUSEWHEEL: {
        g_swWheel += GET_WHEEL_DELTA_WPARAM(wp);
        int notches = g_swWheel / WHEEL_DELTA;
        if (notches) {
            g_swWheel -= notches * WHEEL_DELTA;
            SwScrollTo(g_swScroll - notches * SwS(66));
        }
        return 0;
    }
    case WM_LBUTTONDOWN: {                               // a click on a field's frame, beside its text, focuses it
        POINT p{(short)LOWORD(lp), (short)HIWORD(lp) + g_swScroll};
        for (auto& c : g_swCtls)
            if (SwIsField(c.role) && PtInRect(&c.rc, p) && IsWindowEnabled(c.h) && IsWindowVisible(c.h)) {
                SetFocus(c.h);
                break;
            }
        return 0;
    }
    case WM_MEASUREITEM: {
        MEASUREITEMSTRUCT* m = (MEASUREITEMSTRUCT*)lp;
        if (m->CtlType == ODT_LISTBOX) m->itemHeight = SwS(26);
        return TRUE;
    }
    case WM_DRAWITEM: SwDrawItem((const DRAWITEMSTRUCT*)lp); return TRUE;
    case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX:
        SetTextColor((HDC)wp, kSwBright);
        SetBkColor((HDC)wp, kSwField);
        return (LRESULT)g_swFieldBrush;
    case WM_CTLCOLORSTATIC:                               // disabled fields
        SetTextColor((HDC)wp, kSwDim);
        SetBkColor((HDC)wp, kSwField);
        return (LRESULT)g_swFieldBrush;
    case WM_TIMER:
        if (wp == TIMER_SWREPEAT && g_swRepeat && g_swRepeat - SWID_CTL0 < (int)g_swCtls.size()) {
            POINT p;
            GetCursorPos(&p);
            RECT r;
            GetWindowRect(GetDlgItem(h, g_swRepeat), &r);
            if (PtInRect(&r, p)) SwStep(g_swRepeat - SWID_CTL0, g_swCtls[g_swRepeat - SWID_CTL0].role == SR_PLUS ? 1 : -1);
            SetTimer(h, TIMER_SWREPEAT, 60, nullptr);
        }
        return 0;
    case WM_COMMAND: SwCommand(LOWORD(wp), HIWORD(wp)); return 0;
    case WM_CLOSE:                                       // Esc in the title-endings box: close the window, not the panel
        PostMessageW(g_swHwnd, WM_CLOSE, 0, 0);
        return 0;
    case WM_NEXTDLGCTL:                                  // Tab from the title-endings box
        if (HWND next = GetNextDlgTabItem(g_swHwnd, GetFocus(), wp != 0)) SetFocus(next);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static void SwMakeFonts() {
    for (HFONT* f : {&g_swFont, &g_swBold, &g_swTitle, &g_swHead})
        if (*f) { DeleteObject(*f); *f = nullptr; }
    std::wstring face = L"Segoe UI";
    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof(ncm);
    if (SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, g_swDpi) && ncm.lfMessageFont.lfFaceName[0])
        face = ncm.lfMessageFont.lfFaceName;
    auto mk = [&](int px, int weight) {
        return CreateFontW(-SwS(px), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, face.c_str());
    };
    g_swFont = mk(14, FW_NORMAL);
    g_swBold = mk(15, FW_SEMIBOLD);
    g_swTitle = mk(26, FW_SEMIBOLD);
    g_swHead = mk(12, FW_BOLD);
    HDC dc = GetDC(nullptr);
    HGDIOBJ o = SelectObject(dc, g_swFont);
    TEXTMETRICW tm;
    GetTextMetricsW(dc, &tm);
    g_swFontH = tm.tmHeight;
    SelectObject(dc, o);
    ReleaseDC(nullptr, dc);
}

// The start button's four shade blocks, as the window's icon.
static HICON SwMakeIcon(int s) {
    BITMAPINFO bi = Dib32(s, s);
    void* bits = nullptr;
    HBITMAP color = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!color || !bits) return nullptr;
    uint32_t* px = (uint32_t*)bits;
    int m = std::max(1, s / 10), w = s - 2 * m;
    for (int y = 0; y < s; ++y)
        for (int x = 0; x < s; ++x) {
            uint32_t c = 0;
            if (x >= m && x < s - m && y >= m && y < s - m) {
                int block = (x - m) * 4 / w;
                bool on = block == 0 ? (x % 2 == 0 && y % 2 == 0) : block == 1 ? ((x + y) % 2 == 0)
                        : block == 2 ? !(x % 2 == 1 && y % 2 == 1) : true;
                bool edge = x == m || x == s - m - 1 || y == m || y == s - m - 1;
                c = edge ? 0xFF000000u : on ? 0xFFD8D8D8u : 0xFF404040u;
            }
            px[(size_t)y * s + x] = c;
        }
    std::vector<BYTE> zeros((size_t)((s + 15) / 16) * 2 * s, 0);
    HBITMAP mask = CreateBitmap(s, s, 1, 1, zeros.data());
    ICONINFO ii{TRUE, 0, 0, mask, color};
    HICON ic = CreateIconIndirect(&ii);
    DeleteObject(mask);
    DeleteObject(color);
    return ic;
}

static void SwPlaceChrome() {
    if (!g_swHwnd || !g_swPanel) return;
    RECT cr;
    GetClientRect(g_swHwnd, &cr);
    int leftW = SwS(180), botH = SwS(64), sbW = SwS(12), contentB = std::max(SwS(40), (int)cr.bottom - botH);
    int bh = SwS(34), by = SwS(20);
    for (int p = 0; p < (int)g_swPages.size(); ++p) {
        if (HWND b = GetDlgItem(g_swHwnd, SWID_PAGE0 + p)) MoveWindow(b, SwS(14), by, leftW - SwS(28), bh, TRUE);
        by += bh + SwS(6);
    }
    MoveWindow(g_swPanel, leftW + 1, 0, std::max(10, (int)cr.right - leftW - 1 - sbW), contentB, TRUE);
    MoveWindow(g_swSbar, cr.right - sbW, 0, sbW, contentB, TRUE);
    HDC dc = GetDC(g_swHwnd);
    HGDIOBJ of = SelectObject(dc, g_swFont);
    auto bw = [&](int id) {
        wchar_t t[64];
        GetWindowTextW(GetDlgItem(g_swHwnd, id), t, 64);
        SIZE z{};
        GetTextExtentPoint32W(dc, t, (int)wcslen(t), &z);
        return std::max(SwS(92), (int)z.cx + SwS(36));
    };
    int y = contentB + (botH - bh) / 2, x = SwS(14);
    for (int id : {SWID_RESET, SWID_INI}) {
        int w = bw(id);
        MoveWindow(GetDlgItem(g_swHwnd, id), x, y, w, bh, TRUE);
        x += w + SwS(8);
    }
    int rx = cr.right - SwS(16);
    for (int id : {SWID_APPLY, SWID_CANCEL, SWID_OK}) {
        int w = bw(id);
        rx -= w;
        MoveWindow(GetDlgItem(g_swHwnd, id), rx, y, w, bh, TRUE);
        rx -= SwS(8);
    }
    g_swStatusR = RECT{x + SwS(10), contentB + 1, rx - SwS(10), cr.bottom};
    SelectObject(dc, of);
    ReleaseDC(g_swHwnd, dc);
    InvalidateRect(g_swHwnd, nullptr, FALSE);
}

static void SwPaintChrome(HWND h) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(h, &ps);
    RECT cr;
    GetClientRect(h, &cr);
    int leftW = SwS(180), botH = SwS(64), contentB = std::max(SwS(40), (int)cr.bottom - botH);
    Fill(dc, RECT{0, 0, leftW, contentB}, kSwSide);
    Fill(dc, RECT{leftW, 0, leftW + 1, contentB}, kSwLine);
    Fill(dc, RECT{0, contentB, cr.right, contentB + 1}, kSwLine);
    Fill(dc, RECT{0, contentB + 1, cr.right, cr.bottom}, kSwSide);
    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ of = SelectObject(dc, g_swFont);
    RECT vr{SwS(16), contentB - SwS(40), leftW - SwS(8), contentB - SwS(14)};
    SetTextColor(dc, kSwDim);
    DrawTextW(dc, L"pbar " PBAR_VERSION, -1, &vr, DT_LEFT | DT_BOTTOM | DT_SINGLELINE | DT_NOPREFIX);
    int n = SwDirty();
    std::wstring st = n ? std::to_wstring(n) + (n == 1 ? L" change not applied yet" : L" changes not applied yet")
                        : g_swApplied ? L"Applied" : L"";
    SetTextColor(dc, n ? kSwAccent : kSwDim);
    RECT sr = g_swStatusR;
    DrawTextW(dc, st.c_str(), -1, &sr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    SelectObject(dc, of);
    EndPaint(h, &ps);
}

static void SwApply() {
    SwCommit();
    SwIni ini;
    ini.Load();
    bool wrote = false;
    for (auto& kv : g_sm.cur) {
        const std::wstring& k = kv.first;
        int oi = SwOptOf(k);
        if (oi < 0) continue;
        const SwOpt& o = kSwOpts[oi];
        std::wstring v = SwNorm(o, kv.second, g_sm.orig[k]);
        if (v == g_sm.orig[k]) continue;
        if (o.type == SwT::Autostart) {
            SetAutostart(v == L"1");
            continue;
        }
        if (o.type == SwT::Lines || o.type == SwT::Pins) ini.Set(k, v.empty() ? std::vector<std::wstring>() : Split(v, L'\n'));
        else if (v.empty() && (o.type == SwT::AutoColor || o.type == SwT::Choice)) ini.Unset(k);
        else ini.Set(k, {v});
        wrote = true;
    }
    if (wrote) ini.Save();
    g_sm = SwReadModel();                                 // what's in the file now is the new starting point
    g_swApplied = true;
    if (wrote) Reload();
    SwBuild(true);
}

static void SwResetPage() {
    SwCommit();
    for (int i = g_swPages[g_swPage]; i < SwPageEnd(g_swPage); ++i) {
        const SwOpt& o = kSwOpts[i];
        if (o.key && o.type != SwT::Autostart && o.type != SwT::Pins) g_sm.cur[o.key] = g_sm.def[o.key];   // your pins stay
    }
    g_swApplied = false;
    SwBuild(true);
}

// Something else changed pbar.ini (the right-click menu, or Reload after a hand edit): show the new values,
// but keep anything changed here and not applied yet.
static void SettingsSync() {
    if (!g_swHwnd) return;
    SwModel fresh = SwReadModel();
    bool changed = false;
    for (auto& kv : fresh.orig) {
        std::wstring& o = g_sm.orig[kv.first];
        std::wstring& c = g_sm.cur[kv.first];
        if (o == kv.second) continue;
        if (c == o) {
            c = kv.second;
            changed = true;
        }
        o = kv.second;
    }
    if (changed) SwBuild(true);
    else SwStatusUpdate();
}

static LRESULT CALLBACK SwProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        g_swHwnd = h;
        g_swDpi = GetDpiForWindow(h);
        if (!g_swDpi) g_swDpi = 96;
        SwMakeFonts();
        if (!g_swFieldBrush) g_swFieldBrush = CreateSolidBrush(kSwField);
        BOOL dark = TRUE;                                 // dark title bar (attribute 20; 19 on older Windows 10)
        if (FAILED(DwmSetWindowAttribute(h, (DWORD)20, &dark, sizeof(dark)))) DwmSetWindowAttribute(h, (DWORD)19, &dark, sizeof(dark));
        HINSTANCE hi = GetModuleHandleW(nullptr);
        auto button = [&](int id, const std::wstring& text) {
            HWND b = CreateWindowExW(0, L"BUTTON", text.c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW | BS_NOTIFY,
                                     0, 0, 10, 10, h, (HMENU)(INT_PTR)id, hi, nullptr);
            SendMessageW(b, WM_SETFONT, (WPARAM)g_swFont, FALSE);
            WNDPROC old = (WNDPROC)SetWindowLongPtrW(b, GWLP_WNDPROC, (LONG_PTR)SwBtnProc);
            if (!g_swBtnBase) g_swBtnBase = old;
        };
        for (int p = 0; p < (int)g_swPages.size(); ++p) {
            std::wstring t;
            for (const wchar_t* s = kSwOpts[g_swPages[p]].label; *s; ++s) t += *s == L'&' ? L"&&" : std::wstring(1, *s);
            button(SWID_PAGE0 + p, t);
        }
        g_swPanel = CreateWindowExW(WS_EX_CONTROLPARENT, L"PBarSetPanel", L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
                                    0, 0, 10, 10, h, nullptr, hi, nullptr);
        g_swSbar = CreateWindowExW(0, L"PBarSetScroll", L"", WS_CHILD | WS_VISIBLE, 0, 0, 10, 10, h, nullptr, hi, nullptr);
        button(SWID_RESET, L"Reset page");
        button(SWID_INI, L"Open pbar.ini");
        button(SWID_OK, L"OK");
        button(SWID_CANCEL, L"Cancel");
        button(SWID_APPLY, L"Apply");
        return 0;
    }
    case WM_SIZE: SwPlaceChrome(); return 0;
    case WM_GETMINMAXINFO: {
        MINMAXINFO* mm = (MINMAXINFO*)lp;
        mm->ptMinTrackSize.x = SwS(640);
        mm->ptMinTrackSize.y = SwS(500);
        return 0;
    }
    case WM_DPICHANGED: {
        g_swDpi = HIWORD(wp);
        SwMakeFonts();
        for (HWND c = GetWindow(h, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) SendMessageW(c, WM_SETFONT, (WPARAM)g_swFont, FALSE);
        const RECT* r = (const RECT*)lp;
        SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        SwPlaceChrome();
        SwBuild(true);
        return 0;
    }
    case WM_PAINT: SwPaintChrome(h); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_DRAWITEM: SwDrawItem((const DRAWITEMSTRUCT*)lp); return TRUE;
    case WM_MOUSEWHEEL: return SendMessageW(g_swPanel, msg, wp, lp);
    case WM_ACTIVATE:
        if (LOWORD(wp) != WA_INACTIVE && g_swPanel) SettingsSync();
        break;
    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id == IDOK) SwCommit();                       // Enter in a field: tidy it, don't close
        else if (id == IDCANCEL) PostMessageW(h, WM_CLOSE, 0, 0);
        else if (id == SWID_OK) { SwApply(); DestroyWindow(h); }
        else if (id == SWID_CANCEL) DestroyWindow(h);
        else if (id == SWID_APPLY) SwApply();
        else if (id == SWID_RESET) SwResetPage();
        else if (id == SWID_INI) ShellExecuteW(nullptr, L"open", g_iniPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        else if (id >= SWID_PAGE0 && id < SWID_PAGE0 + (int)g_swPages.size() && id - SWID_PAGE0 != g_swPage) {
            SwCommit();
            g_swPage = id - SWID_PAGE0;
            SwBuild(false);
        }
        return 0;
    }
    case WM_CLOSE:
        SwCommit();
        if (SwDirty() > 0) {
            int r = MessageBoxW(h, L"Apply the changes you made before closing?", L"pbar settings",
                                MB_YESNOCANCEL | MB_ICONQUESTION);
            if (r == IDCANCEL) return 0;
            if (r == IDYES) SwApply();
        }
        DestroyWindow(h);
        return 0;
    case WM_NCDESTROY:
        KillTimer(g_swPanel, TIMER_SWREPEAT);
        g_swCtls.clear();
        g_swRows.clear();
        for (HFONT* f : {&g_swFont, &g_swBold, &g_swTitle, &g_swHead})
            if (*f) { DeleteObject(*f); *f = nullptr; }
        g_swHwnd = g_swPanel = g_swSbar = g_swHot = nullptr;
        g_swRepeat = 0;
        g_swDragY = -1;
        RegisterTermHotkey();                             // in case the hotkey box had it switched off
        break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// Keyboard for the settings window: Tab moves between controls and Esc closes, except in the hotkey box,
// which takes every key combination itself.
static bool SettingsPreTranslate(MSG* m) {
    if (!g_swHwnd || (m->hwnd != g_swHwnd && !IsChild(g_swHwnd, m->hwnd))) return false;
    if (m->message >= WM_KEYFIRST && m->message <= WM_KEYLAST && GetParent(m->hwnd) == g_swPanel) {
        int i = GetDlgCtrlID(m->hwnd) - SWID_CTL0;
        bool plainTab = m->message == WM_KEYDOWN && m->wParam == VK_TAB && GetKeyState(VK_CONTROL) >= 0 &&
                        GetKeyState(VK_MENU) >= 0 && GetKeyState(VK_LWIN) >= 0 && GetKeyState(VK_RWIN) >= 0;
        if (i >= 0 && i < (int)g_swCtls.size() && g_swCtls[i].role == SR_HOTKEY && !plainTab) return false;
    }
    return IsDialogMessageW(g_swHwnd, m) != FALSE;
}

// The settings page that holds an ini key.
static int SwPageOf(const wchar_t* key) {
    int page = -1;
    for (int i = 0; i < kSwCount; ++i) {
        if (kSwOpts[i].type == SwT::Page) ++page;
        if (kSwOpts[i].key && !wcscmp(kSwOpts[i].key, key)) return page;
    }
    return -1;
}

// Opens the settings window (or brings it forward), on a given page or the one used last.
static void OpenSettings(int page = -1) {
    if (g_swPages.empty())
        for (int i = 0; i < kSwCount; ++i) if (kSwOpts[i].type == SwT::Page) g_swPages.push_back(i);
    if (g_swHwnd) {
        if (IsIconic(g_swHwnd)) ShowWindow(g_swHwnd, SW_RESTORE);
        if (page >= 0 && page < (int)g_swPages.size() && page != g_swPage) {
            SwCommit();
            g_swPage = page;
            SwBuild(false);
        }
        ForceForeground(g_swHwnd);
        return;
    }
    if (page >= 0 && page < (int)g_swPages.size()) g_swPage = page;
    HINSTANCE hi = GetModuleHandleW(nullptr);
    static bool registered = false;
    if (!registered) {
        g_swIconBig = SwMakeIcon(GetSystemMetrics(SM_CXICON));
        g_swIconSmall = SwMakeIcon(GetSystemMetrics(SM_CXSMICON));
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.hInstance = hi;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpfnWndProc = SwProc;
        wc.lpszClassName = L"PBarSettings";
        wc.hIcon = g_swIconBig;
        wc.hIconSm = g_swIconSmall;
        RegisterClassExW(&wc);
        wc.hIcon = wc.hIconSm = nullptr;
        wc.lpfnWndProc = SwPanelProc;
        wc.lpszClassName = L"PBarSetPanel";
        RegisterClassExW(&wc);
        wc.lpfnWndProc = SwSbarProc;
        wc.lpszClassName = L"PBarSetScroll";
        RegisterClassExW(&wc);
        registered = true;
    }
    g_sm = SwReadModel();
    g_swApplied = false;
    g_swScroll = 0;
    g_swPinSel = -1;
    // on the bar's screen, sized for that screen's scaling and centred in the space the bar leaves
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTOPRIMARY), &mi);
    DWORD style = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, ex = WS_EX_CONTROLPARENT;
    HWND h = CreateWindowExW(ex, L"PBarSettings", L"pbar settings", style, mi.rcWork.left + 40, mi.rcWork.top + 40,
                             640, 480, nullptr, nullptr, hi, nullptr);
    if (!h) return;
    RECT r{0, 0, SwS(980), SwS(740)};
    AdjustWindowRectExForDpi(&r, style, FALSE, ex, g_swDpi);
    int ww = std::min((int)(r.right - r.left), (int)(mi.rcWork.right - mi.rcWork.left) * 94 / 100);
    int wh = std::min((int)(r.bottom - r.top), (int)(mi.rcWork.bottom - mi.rcWork.top) * 94 / 100);
    SetWindowPos(h, nullptr, mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - ww) / 2,
                 mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - wh) / 2, ww, wh, SWP_NOZORDER | SWP_NOACTIVATE);
    SwBuild(false);
    ShowWindow(h, SW_SHOW);
    ForceForeground(h);
}

static int TrackMenu(HMENU m, POINT screenPt) {
    KillTimer(g_hwnd, TIMER_HOVER);
    SetForegroundWindow(g_hwnd);                  // required so the menu closes when you click away
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, screenPt.x, screenPt.y, 0, g_hwnd, nullptr);
    PostMessageW(g_hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);
    return cmd;
}

static void ShowMenu(POINT screenPt) {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, IDM_SETTINGS, L"Settings\u2026");
    SetMenuDefaultItem(m, IDM_SETTINGS, FALSE);
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);

    HMENU screens = CreatePopupMenu();
    AppendMenuW(screens, MF_STRING | (g_cfg.monitor == 0 ? MF_CHECKED : 0), IDM_MON_PRIMARY, L"Primary");
    auto mons = Monitors();
    for (size_t i = 0; i < mons.size() && i < 16; ++i) {
        wchar_t label[96];
        wsprintfW(label, L"Display %d  (%dx%d)%s", mons[i].num,
                  mons[i].rc.right - mons[i].rc.left, mons[i].rc.bottom - mons[i].rc.top,
                  mons[i].primary ? L"  primary" : L"");
        AppendMenuW(screens, MF_STRING | (g_cfg.monitor == mons[i].num ? MF_CHECKED : 0),
                    IDM_MON_BASE + i, label);
    }
    AppendMenuW(screens, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(screens, MF_STRING | (!g_cfg.rightSide ? MF_CHECKED : 0), IDM_SIDE_LEFT, L"Left edge");
    AppendMenuW(screens, MF_STRING | (g_cfg.rightSide ? MF_CHECKED : 0), IDM_SIDE_RIGHT, L"Right edge");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)screens, L"Position");
    AppendMenuW(m, MF_STRING | (g_cfg.clock24 ? MF_CHECKED : 0), IDM_CLOCK24, L"24-hour clock");
    bool autostart = AutostartEnabled();
    AppendMenuW(m, MF_STRING | (autostart ? MF_CHECKED : 0), IDM_AUTOSTART, L"Start with Windows");

    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_RELOAD, L"Reload config + Start pins");
    AppendMenuW(m, MF_STRING, IDM_EDIT, L"Edit pbar.ini");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_EXIT, L"Exit pbar");
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"pbar " PBAR_VERSION);
    int cmd = TrackMenu(m, screenPt);
    if (cmd == IDM_SETTINGS) {
        OpenSettings();
    } else if (cmd == IDM_AUTOSTART) {
        SetAutostart(!autostart);
        SettingsSync();
    } else if (cmd == IDM_CLOCK24) {
        SetIniValue(L"clock24", g_cfg.clock24 ? L"0" : L"1");
        Reload();
    } else if (cmd == IDM_SIDE_LEFT || cmd == IDM_SIDE_RIGHT) {
        SetIniValue(L"side", cmd == IDM_SIDE_RIGHT ? L"right" : L"left");
        Reload();
    } else if (cmd == IDM_MON_PRIMARY) {
        SetIniValue(L"monitor", L"primary");
        Reload();
    } else if (cmd >= IDM_MON_BASE && cmd < IDM_MON_BASE + 16 && cmd - IDM_MON_BASE < (int)mons.size()) {
        SetIniValue(L"monitor", std::to_wstring(mons[cmd - IDM_MON_BASE].num));
        Reload();
    }
    else if (cmd == IDM_RELOAD) Reload();
    else if (cmd == IDM_EDIT) ShellExecuteW(nullptr, L"open", g_iniPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    else if (cmd == IDM_EXIT) DestroyWindow(g_hwnd);
}

static void Activate(HWND h) {
    if (!IsWindow(h)) return;
    if (IsIconic(h)) ShowWindowAsync(h, SW_RESTORE);
    ForceForeground(h);
}

// Raise the hovered window right away. Skips work when it's already in front.
static void HoverRaise(int idx) {
    if (idx < 0 || idx >= (int)g_tasks.size()) return;
    HWND h = g_tasks[idx].hwnd;
    if (!IsWindow(h) || GetForegroundWindow() == h) return;
    if (IsIconic(h) && !g_cfg.hoverRestore) return;
    Activate(h);
}

static void ShowTaskMenu(HWND h, POINT screenPt) {
    if (!IsWindow(h)) return;
    bool iconic = IsIconic(h) != 0, zoomed = IsZoomed(h) != 0;
    std::wstring exe = ProcessPath(h);
    UINT noExe = exe.empty() ? MF_GRAYED : 0;

    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, IDM_T_SWITCH, L"Switch to");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | ((iconic || zoomed) ? 0 : MF_GRAYED), IDM_T_RESTORE, L"Restore");
    AppendMenuW(m, MF_STRING | (iconic ? MF_GRAYED : 0), IDM_T_MINIMIZE, L"Minimize");
    AppendMenuW(m, MF_STRING | (zoomed ? MF_GRAYED : 0), IDM_T_MAXIMIZE, L"Maximize");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | noExe, IDM_T_NEW, L"New instance");
    AppendMenuW(m, MF_STRING | noExe, IDM_T_LOCATION, L"Open file location");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_T_CLOSE, L"Close window");
    AppendMenuW(m, MF_STRING, IDM_T_KILL, L"End process");
    SetMenuDefaultItem(m, IDM_T_SWITCH, FALSE);

    switch (TrackMenu(m, screenPt)) {
    case IDM_T_SWITCH:   Activate(h); break;
    case IDM_T_RESTORE:  ShowWindowAsync(h, SW_RESTORE); Activate(h); break;
    case IDM_T_MINIMIZE: ShowWindowAsync(h, SW_MINIMIZE); break;
    case IDM_T_MAXIMIZE: ShowWindowAsync(h, SW_MAXIMIZE); Activate(h); break;
    case IDM_T_NEW:      ShellExecuteW(nullptr, L"open", exe.c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
    case IDM_T_LOCATION: OpenLocation(exe); break;
    case IDM_T_CLOSE:    PostMessageW(h, WM_CLOSE, 0, 0); break;
    case IDM_T_KILL: {
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        if (HANDLE p = OpenProcess(PROCESS_TERMINATE, FALSE, pid)) { TerminateProcess(p, 1); CloseHandle(p); }
        break;
    }
    }
    if (RefreshTasks()) PvSync();
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

static void ShowPinMenu(int idx, POINT screenPt) {
    if (idx < 0 || idx >= PinCount()) return;
    Pin p = PinAt(idx);                           // copy: the list can change while the menu is open
    UINT noPath = p.aumid.empty() ? 0 : MF_GRAYED;

    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, IDM_P_OPEN, L"Open");
    AppendMenuW(m, MF_STRING | noPath, IDM_P_ADMIN, L"Run as administrator");
    AppendMenuW(m, MF_STRING | noPath, IDM_P_LOCATION, L"Open file location");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_SETTINGS, L"Pin settings\u2026");
    SetMenuDefaultItem(m, IDM_P_OPEN, FALSE);

    switch (TrackMenu(m, screenPt)) {
    case IDM_P_OPEN: LaunchPin(p); break;
    case IDM_P_ADMIN: {
        std::wstring path = ExpandEnv(p.path), args = ExpandEnv(p.args), dir = ExpandEnv(p.dir);
        ShellExecuteW(nullptr, L"runas", path.c_str(), args.empty() ? nullptr : args.c_str(),
                      dir.empty() ? nullptr : dir.c_str(), SW_SHOWNORMAL);
        break;
    }
    case IDM_P_LOCATION: OpenLocation(Resolve(p.path)); break;
    case IDM_SETTINGS: OpenSettings(SwPageOf(L"pin")); break;
    }
}

static void ShowTermMenu(POINT screenPt) {
    bool busy = g_term.started && g_term.running;
    std::wstring k = g_term.started ? g_term.kind : ShellKind();
    UINT noOut = g_term.lines.empty() ? MF_GRAYED : 0;
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | (g_sel ? 0 : MF_GRAYED), IDM_TERM_COPYSEL, L"Copy\tCtrl+C");
    AppendMenuW(m, MF_STRING | noOut, IDM_TERM_SELALL, L"Select all");
    AppendMenuW(m, MF_STRING | noOut, IDM_TERM_COPY, L"Copy all output");
    AppendMenuW(m, MF_STRING, IDM_TERM_COPYDIR, L"Copy folder path");
    AppendMenuW(m, MF_STRING | noOut, IDM_TERM_NOTEPAD, L"Open output in Notepad");
    AppendMenuW(m, MF_STRING, IDM_TERM_CLEAR, L"Clear\tCtrl+L");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (busy ? 0 : MF_GRAYED), IDM_TERM_INTERRUPT, L"Interrupt\tCtrl+C");
    AppendMenuW(m, MF_STRING | (g_term.started ? 0 : MF_GRAYED), IDM_TERM_RESTART, L"Restart shell");
    HMENU sh = CreatePopupMenu();
    AppendMenuW(sh, MF_STRING | (k == L"powershell" ? MF_CHECKED : 0), IDM_TERM_SH_PS, L"Windows PowerShell");
    AppendMenuW(sh, MF_STRING | (k == L"pwsh" ? MF_CHECKED : 0), IDM_TERM_SH_PWSH, L"PowerShell 7 (pwsh)");
    AppendMenuW(sh, MF_STRING | (k == L"cmd" ? MF_CHECKED : 0), IDM_TERM_SH_CMD, L"Command Prompt (cmd)");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)sh, L"Shell");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_SETTINGS, L"Terminal settings\u2026");

    int cmd = TrackMenu(m, screenPt);
    switch (cmd) {
    case IDM_TERM_COPYSEL: TermCopySelection(); break;
    case IDM_TERM_SELALL: TermSelectAll(); break;
    case IDM_TERM_COPYDIR: CopyToClipboard(g_term.cwd.empty() ? TermStartDir() : g_term.cwd); break;
    case IDM_TERM_INTERRUPT: TermInterrupt(); break;
    case IDM_TERM_RESTART:
        TermStop();
        TermAddLine(L"[shell restarted]", TL_INFO);
        TermStart();
        break;
    case IDM_TERM_SH_PS: case IDM_TERM_SH_PWSH: case IDM_TERM_SH_CMD: {
        std::wstring v = cmd == IDM_TERM_SH_CMD ? L"cmd" : cmd == IDM_TERM_SH_PWSH ? L"pwsh" : L"powershell";
        SetIniValue(L"shell", v);
        g_cfg.shell = v;
        SettingsSync();
        if (g_term.started && g_term.kind != v) {
            TermStop();
            TermAddLine(L"[switched to " + v + L"]", TL_INFO);
            TermStart();
        }
        break;
    }
    case IDM_TERM_COPY: CopyToClipboard(TermAllText()); break;
    case IDM_TERM_NOTEPAD: TermOpenInNotepad(); break;
    case IDM_TERM_CLEAR: TermClearOutput(); break;
    case IDM_SETTINGS: OpenSettings(SwPageOf(L"shell")); break;
    }
    TermInvalidate();
}

// ---------- calendar pop-out ----------
// Clicking the clock opens this beside the bar: live time, full date, and a month grid
// in the same bevel style. It never takes focus. It stays open while the mouse is over it
// or the clock, and closes caltimeout seconds after the mouse leaves (re-hovering resets that).
// Wheel changes month, clicking the month name returns to today, clicking the clock closes it.

enum { CAL_PREV = 1, CAL_NEXT = 2, CAL_TITLE = 3, CAL_DAY = 100 };
struct CalHit { RECT r; int id; };
static std::vector<CalHit> g_calHits;

static int DaysIn(int y, int m) {
    static const int d[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    return m == 2 && leap ? 29 : d[m - 1];
}

static int Weekday(int y, int m, int d) {             // 0 = Sunday (Sakamoto)
    static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (m < 3) y -= 1;
    return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}

struct CalMetrics { int m, gap, cellW, cellH, bigH, wdH, w, h; };

static CalMetrics CalMeasure(HDC dc) {
    CalMetrics c{};
    c.m = 3;
    c.gap = std::max(1, g_cfg.spacing);
    HGDIOBJ old = SelectObject(dc, g_font);
    TEXTMETRICW tm;
    GetTextMetricsW(dc, &tm);
    SIZE a{}, b{};
    GetTextExtentPoint32W(dc, L"88", 2, &a);
    GetTextExtentPoint32W(dc, L"Mo", 2, &b);
    c.cellW = std::max(a.cx, b.cx) + 2 * g_cfg.pad + 4;
    c.cellH = tm.tmHeight + 2 * g_cfg.vpad + 2;
    c.wdH = tm.tmHeight + 2;
    SelectObject(dc, g_calBigFont);
    GetTextMetricsW(dc, &tm);
    c.bigH = tm.tmHeight + 2 * g_cfg.vpad + 2;
    SelectObject(dc, old);
    c.w = 2 * c.m + 7 * c.cellW + 6 * c.gap;
    c.h = 2 * c.m + c.bigH + c.gap + c.cellH + c.gap + c.cellH + c.gap + c.wdH + c.gap + 6 * c.cellH + 5 * c.gap;
    return c;
}

static void CalCell(HDC dc, RECT r, bool pressed, COLORREF border, COLORREF face, COLORREF txt, const wchar_t* s,
                    UINT align = DT_CENTER) {
    DrawButton(dc, r, pressed, border, face, txt, {});
    int sh = pressed ? 1 : 0;
    RECT tr{r.left + 1 + g_cfg.pad + sh, r.top + 1 + sh, r.right - 1 - g_cfg.pad + sh, r.bottom - 1 + sh};
    SetTextColor(dc, txt);
    DrawTextW(dc, s, -1, &tr, align | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
}

static void CalPaint(HWND h) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(h, &ps);
    RECT cr;
    GetClientRect(h, &cr);
    HDC dc = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, cr.right, cr.bottom);
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    HGDIOBJ oldFont = SelectObject(dc, g_font);
    SetBkMode(dc, TRANSPARENT);
    Fill(dc, cr, g_cfg.bg);
    Fill(dc, RECT{0, 0, cr.right, 1}, g_cfg.hi);              // panel bevel: lit top-left edge
    Fill(dc, RECT{0, 0, 1, cr.bottom}, g_cfg.hi);
    g_calHits.clear();

    CalMetrics c = CalMeasure(dc);
    SYSTEMTIME st;
    GetLocalTime(&st);
    static const wchar_t* DL[] = {L"Sunday", L"Monday", L"Tuesday", L"Wednesday", L"Thursday", L"Friday", L"Saturday"};
    static const wchar_t* ML[] = {L"January", L"February", L"March", L"April", L"May", L"June", L"July",
                                  L"August", L"September", L"October", L"November", L"December"};
    static const wchar_t* WD[] = {L"Su", L"Mo", L"Tu", L"We", L"Th", L"Fr", L"Sa"};
    int x0 = c.m, x1 = cr.right - c.m, y = c.m;
    wchar_t buf[96];

    // live time
    if (g_cfg.clock24) wsprintfW(buf, L"%02d:%02d:%02d", st.wHour, st.wMinute, st.wSecond);
    else wsprintfW(buf, L"%d:%02d:%02d %s", (st.wHour + 11) % 12 + 1, st.wMinute, st.wSecond, st.wHour < 12 ? L"AM" : L"PM");
    SelectObject(dc, g_calBigFont);
    CalCell(dc, RECT{x0, y, x1, y + c.bigH}, false, g_cfg.outline, g_cfg.clockFace, g_cfg.hoverText, buf);
    SelectObject(dc, g_font);
    y += c.bigH + c.gap;

    // full date
    wsprintfW(buf, L"%s, %s %d, %d", DL[st.wDayOfWeek], ML[st.wMonth - 1], st.wDay, st.wYear);
    CalCell(dc, RECT{x0, y, x1, y + c.cellH}, false, g_cfg.outline, g_cfg.clockFace, g_cfg.text, buf);
    y += c.cellH + c.gap;

    // month navigation
    RECT rp{x0, y, x0 + c.cellW, y + c.cellH}, rn{x1 - c.cellW, y, x1, y + c.cellH};
    RECT rt{rp.right + c.gap, y, rn.left - c.gap, y + c.cellH};
    CalCell(dc, rp, false, g_cfg.outline, HoverFace(g_cfg.face, g_calHover == CAL_PREV),
            g_calHover == CAL_PREV ? g_cfg.hoverText : g_cfg.text, L"<");
    CalCell(dc, rn, false, g_cfg.outline, HoverFace(g_cfg.face, g_calHover == CAL_NEXT),
            g_calHover == CAL_NEXT ? g_cfg.hoverText : g_cfg.text, L">");
    wsprintfW(buf, L"%s %d", ML[g_calMonth - 1], g_calYear);
    CalCell(dc, rt, false, g_cfg.outline, HoverFace(g_cfg.face, g_calHover == CAL_TITLE),
            g_calHover == CAL_TITLE ? g_cfg.hoverText : g_cfg.text, buf);
    g_calHits.push_back({rp, CAL_PREV});
    g_calHits.push_back({rn, CAL_NEXT});
    g_calHits.push_back({rt, CAL_TITLE});
    y += c.cellH + c.gap;

    // weekday names
    SetTextColor(dc, g_cfg.dim);
    for (int col = 0; col < 7; ++col) {
        int cx = x0 + col * (c.cellW + c.gap);
        RECT r{cx, y, cx + c.cellW, y + c.wdH};
        DrawTextW(dc, WD[(col + g_cfg.weekStart) % 7], -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    y += c.wdH + c.gap;

    // 6-week grid
    int first = Weekday(g_calYear, g_calMonth, 1);
    int offset = (first - g_cfg.weekStart + 7) % 7;
    int pm = g_calMonth == 1 ? 12 : g_calMonth - 1, py = g_calMonth == 1 ? g_calYear - 1 : g_calYear;
    int nm = g_calMonth == 12 ? 1 : g_calMonth + 1, ny = g_calMonth == 12 ? g_calYear + 1 : g_calYear;
    int dim = DaysIn(g_calYear, g_calMonth), dimPrev = DaysIn(py, pm);
    for (int i = 0; i < 42; ++i) {
        int d = i - offset + 1, cy = g_calYear, cm = g_calMonth;
        bool inMonth = true;
        if (d < 1) { d += dimPrev; cy = py; cm = pm; inMonth = false; }
        else if (d > dim) { d -= dim; cy = ny; cm = nm; inMonth = false; }
        bool today = cy == st.wYear && cm == st.wMonth && d == st.wDay;
        bool hov = g_calHover == CAL_DAY + i;
        int cx = x0 + (i % 7) * (c.cellW + c.gap), ry = y + (i / 7) * (c.cellH + c.gap);
        RECT r{cx, ry, cx + c.cellW, ry + c.cellH};
        wsprintfW(buf, L"%d", d);
        COLORREF txt = today ? g_cfg.activeText : hov ? g_cfg.hoverText : inMonth ? g_cfg.text : g_cfg.dim;
        CalCell(dc, r, today, today ? g_cfg.activeBorder : g_cfg.outline,
                HoverFace(today ? g_cfg.activeFace : g_cfg.face, hov), txt, buf);
        g_calHits.push_back({r, CAL_DAY + i});
    }

    BitBlt(hdc, 0, 0, cr.right, cr.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldFont);
    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(dc);
    EndPaint(h, &ps);
}

static void CalHide() {
    if (!g_calHwnd || !IsWindowVisible(g_calHwnd)) return;
    KillTimer(g_calHwnd, TIMER_CAL);
    ShowWindow(g_calHwnd, SW_HIDE);
    g_calClosedAt = GetTickCount();
}

static void CalShiftMonth(int delta) {
    int idx = g_calYear * 12 + (g_calMonth - 1) + delta;
    g_calYear = idx / 12;
    g_calMonth = idx % 12 + 1;
    InvalidateRect(g_calHwnd, nullptr, FALSE);
}

static void CalToday() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    g_calYear = st.wYear;
    g_calMonth = st.wMonth;
}

static LRESULT CALLBACK CalProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: CalPaint(h); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_TIMER:
        if (wp == TIMER_CAL) {
            POINT pt;
            GetCursorPos(&pt);
            RECT cal;
            GetWindowRect(h, &cal);
            RECT bar = BarScreenRect();
            bool over = PtInRect(&cal, pt) != 0;
            for (auto& rw : g_rows)
                if (rw.kind == K_CLOCK) {
                    RECT cr = rw.r;
                    OffsetRect(&cr, bar.left, bar.top);
                    if (PtInRect(&cr, pt)) over = true;
                }
            DWORD now = GetTickCount();
            if (over) g_calLastHover = now;
            else if (now - g_calLastHover >= (DWORD)g_cfg.calTimeout * 1000) { CalHide(); return 0; }
            SYSTEMTIME st;
            GetLocalTime(&st);
            if (st.wSecond != g_calLastSecond) { g_calLastSecond = st.wSecond; InvalidateRect(h, nullptr, FALSE); }
        }
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) CalHide();
        else if (wp == VK_LEFT || wp == VK_PRIOR) CalShiftMonth(-1);
        else if (wp == VK_RIGHT || wp == VK_NEXT) CalShiftMonth(1);
        else if (wp == VK_HOME) { CalToday(); InvalidateRect(h, nullptr, FALSE); }
        return 0;
    case WM_MOUSEWHEEL:
        CalShiftMonth(GET_WHEEL_DELTA_WPARAM(wp) > 0 ? -1 : 1);
        return 0;
    case WM_MOUSEMOVE: {
        TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, h, 0};
        TrackMouseEvent(&tme);
        POINT pt{(short)LOWORD(lp), (short)HIWORD(lp)};
        int hit = 0;
        for (auto& c : g_calHits) if (PtInRect(&c.r, pt)) { hit = c.id; break; }
        if (hit != g_calHover) { g_calHover = hit; InvalidateRect(h, nullptr, FALSE); }
        return 0;
    }
    case WM_MOUSELEAVE:
        if (g_calHover) { g_calHover = 0; InvalidateRect(h, nullptr, FALSE); }
        return 0;
    case WM_LBUTTONUP: {
        POINT pt{(short)LOWORD(lp), (short)HIWORD(lp)};
        for (auto& c : g_calHits) {
            if (!PtInRect(&c.r, pt)) continue;
            if (c.id == CAL_PREV) CalShiftMonth(-1);
            else if (c.id == CAL_NEXT) CalShiftMonth(1);
            else if (c.id == CAL_TITLE) { CalToday(); InvalidateRect(h, nullptr, FALSE); }
            break;
        }
        return 0;
    }
    case WM_RBUTTONUP:
        CalHide();
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static void CalToggle() {
    if (g_calHwnd && IsWindowVisible(g_calHwnd)) { CalHide(); return; }
    if (!g_calHwnd) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = CalProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = L"PBarCal";
        RegisterClassExW(&wc);
        g_calHwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, L"PBarCal", L"pbar calendar", WS_POPUP,
                                    0, 0, 10, 10, g_hwnd, nullptr, wc.hInstance, nullptr);
        if (!g_calHwnd) return;
    }
    CalToday();
    g_calHover = 0;

    HDC sdc = GetDC(nullptr);
    CalMetrics c = CalMeasure(sdc);
    ReleaseDC(nullptr, sdc);

    // Beside the bar on its inner side, bottom-aligned with the clock, kept on the bar's monitor.
    RECT bar = BarScreenRect();
    int clockBottom = bar.bottom;
    for (auto& rw : g_rows) if (rw.kind == K_CLOCK) clockBottom = bar.top + rw.r.bottom;
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTOPRIMARY), &mi);
    int x = g_cfg.rightSide ? bar.left - c.w - 2 : bar.right + 2;
    int y = std::max((int)mi.rcMonitor.top, std::min(clockBottom, (int)mi.rcMonitor.bottom) - c.h);
    SetWindowPos(g_calHwnd, HWND_TOPMOST, x, y, c.w, c.h, SWP_SHOWWINDOW | SWP_NOACTIVATE);
    g_calLastHover = GetTickCount();
    SetTimer(g_calHwnd, TIMER_CAL, 200, nullptr);
    InvalidateRect(g_calHwnd, nullptr, FALSE);
}

// ---------- window proc ----------

static void SetHover(int k, int i) {
    if (k == g_hoverKind && i == g_hoverIdx) return;
    bool watchChanged = (k == K_CLOCK) != (g_hoverKind == K_CLOCK);
    g_hoverKind = k;
    g_hoverIdx = i;
    KillTimer(g_hwnd, TIMER_HOVER);
    InvalidateRect(g_hwnd, nullptr, FALSE);
    if (watchChanged && g_watchHwnd) InvalidateRect(g_watchHwnd, nullptr, FALSE);   // its rim lights up on hover
    if (k == K_TASK && g_cfg.hoverFocus) {
        if (g_cfg.hoverDelay == 0) HoverRaise(i);
        else SetTimer(g_hwnd, TIMER_HOVER, g_cfg.hoverDelay, nullptr);
    }
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == g_shellMsg && g_shellMsg) {
        HWND h = (HWND)lp;
        bool newFlash = false;
        if (wp == HSHELL_FLASH) {
            for (auto& t : g_tasks) if (t.hwnd == h && !t.flash) { t.flash = true; newFlash = true; }
        } else if ((wp & 0x7FFF) == HSHELL_WINDOWACTIVATED) {
            for (auto& t : g_tasks) if (t.hwnd == h) t.flash = false;
        }
        bool changed = RefreshTasks();
        UpdateBlinkTimer(newFlash);
        ApplyZOrder(false);
        if (changed) PvSync();
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    if (msg == g_taskbarCreatedMsg && g_taskbarCreatedMsg) {     // explorer restarted
        AppBarRemove();
        AppBarRegister();
        ShowSystemTaskbars(!g_cfg.hideTaskbar);
        return 0;
    }

    switch (msg) {
    case WM_STARTPINS: {
        StartPinsResult* res = (StartPinsResult*)lp;
        if (res->gen == g_startPinsGen && g_cfg.startPins) {
            g_startPins.clear();
            for (auto& p : res->pins) {
                bool dup = std::any_of(g_cfg.pins.begin(), g_cfg.pins.end(), [&](const Pin& c) {
                    return _wcsicmp(c.name.c_str(), p.name.c_str()) == 0;
                });
                if (!dup) g_startPins.push_back(p);
            }
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        delete res;
        return 0;
    }

    case WM_TERMDATA:
        TermDrain();
        return 0;
    case WM_TERMEXIT:
        // give the reader threads a moment to deliver the shell's last words before tidying up
        g_termExitGen = (LONG)wp;
        SetTimer(hwnd, TIMER_TERMEXIT, 150, nullptr);
        return 0;

    case WM_OPENSETTINGS:
        OpenSettings();
        return 0;
    case WM_APPBAR:
        if (wp == ABN_POSCHANGED) AppBarSetPos();
        else if (wp == ABN_FULLSCREENAPP) {
            g_fullscreenApp = lp != 0;
            g_fullscreenHwnd = lp ? GetForegroundWindow() : nullptr;
            ApplyZOrder(false);
        }
        return 0;

    case WM_MOUSEACTIVATE: {
        // Only the terminal takes focus; clicking anything else leaves your window active.
        POINT pt;
        GetCursorPos(&pt);
        ScreenToClient(hwnd, &pt);
        return InTerminal(pt.x - BarOX(), pt.y) ? MA_ACTIVATE : MA_NOACTIVATE;
    }
    case WM_ACTIVATE:
        g_active = LOWORD(wp) != WA_INACTIVE;
        if (g_active) TermCaretReset();
        else KillTimer(hwnd, TIMER_CARET);
        g_caretOn = true;
        InvalidateRect(hwnd, nullptr, FALSE);
        break;                                            // DefWindowProc gives us keyboard focus
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);
            SetCursor(LoadCursorW(nullptr, InTerminal(pt.x - BarOX(), pt.y) ? IDC_IBEAM : IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_HOTKEY:
        if (wp == HOTKEY_TERM && g_cfg.terminal) {
            if (GetForegroundWindow() == hwnd) TermLeave();
            else {
                ForceForeground(hwnd);
                SetFocus(hwnd);
                TermStart();
            }
        }
        return 0;
    case WM_KEYDOWN:
        if (g_cfg.terminal && TermKey(wp)) return 0;
        break;
    case WM_CHAR:
        if (g_cfg.terminal) TermChar((wchar_t)wp);
        return 0;
    case WM_CLOSE:
        return 0;                                         // Alt+F4 while typing shouldn't kill the bar; use Exit

    case WM_TIMER:
        if (wp == TIMER_SCAN) {
            bool changed = RefreshTasks();
            for (auto& t : g_tasks) if (UpdateGeometry(t)) changed = true;
            if (changed) PvSync();
            UpdateBlinkTimer(false);
            ApplyZOrder(false);
            if (g_cfg.hideTaskbar) {
                HWND t = FindWindowW(L"Shell_TrayWnd", nullptr);
                if (t && IsWindowVisible(t)) ShowSystemTaskbars(false);
            }
        } else if (wp == TIMER_ANIM) {
            if (!PvStep()) KillTimer(hwnd, TIMER_ANIM);
            UpdateBarRegion();                            // reshape before the paint that draws the new frame
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (wp == TIMER_RECENTER) {
            PvEndHover();
        } else if (wp == TIMER_CARET) {
            g_caretOn = !g_caretOn;
            TermInvalidate();
        } else if (wp == TIMER_TERMEXIT) {
            KillTimer(hwnd, TIMER_TERMEXIT);
            if (g_termExitGen == g_termGen && g_term.started) {
                TermDrain();
                TermOnExit();
            }
        } else if (wp == TIMER_HOVER) {
            KillTimer(hwnd, TIMER_HOVER);
            if (g_hoverKind == K_TASK) HoverRaise(g_hoverIdx);
        } else if (wp == TIMER_BLINK) {
            UpdateBlinkTimer(false);
            if (std::any_of(g_tasks.begin(), g_tasks.end(), [](const Task& t) { return t.flash; })) {
                ++g_blinkTicks;
                if (g_cfg.flashCount > 0 && g_blinkTicks >= g_cfg.flashCount * 2) {
                    g_blinkOn = true;                    // done blinking: stay lit until focused
                    KillTimer(hwnd, TIMER_BLINK);
                } else {
                    g_blinkOn = g_cfg.flashCount == 0 ? true : !g_blinkOn;
                }
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        } else if (wp == TIMER_METER) {
            if (AudioTick()) InvalidateRect(hwnd, nullptr, FALSE);
        } else if (wp == TIMER_CLOCK && g_cfg.clock) {
            SYSTEMTIME st;
            GetLocalTime(&st);
            if (st.wMinute != g_lastMinute) {
                g_lastMinute = st.wMinute;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        }
        return 0;

    case WM_MOUSEMOVE: {
        if (!g_tracking) {
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&tme);
            g_tracking = true;
        }
        int x = (short)LOWORD(lp) - BarOX(), y = (short)HIWORD(lp);   // bar coordinates
        if (g_selDrag) { TermDragTo(x, y); return 0; }
        int k = K_NONE, i = -1;
        if (y >= g_pvTop && y < g_pvBottom) {
            i = PvHover(x, y);
            if (i >= 0) k = K_TASK;
        } else {
            PvLeave();
            if (InTerminal(x, y)) k = K_TERM;
            else if (const Row* r = HitTest(x, y)) { k = r->kind; i = r->idx; }
        }
        SetHover(k, i);
        return 0;
    }
    case WM_MOUSELEAVE:
        KillTimer(hwnd, TIMER_HOVER);
        g_tracking = false;
        if (g_hoverKind == K_CLOCK && g_watchHwnd) InvalidateRect(g_watchHwnd, nullptr, FALSE);
        g_hoverKind = K_NONE;
        g_hoverIdx = -1;
        PvLeave();
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;

    case WM_MOUSEWHEEL: {
        POINT pt{(short)LOWORD(lp), (short)HIWORD(lp)};       // screen coords
        ScreenToClient(hwnd, &pt);
        pt.x -= BarOX();
        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        if (InTerminal(pt.x, pt.y)) {
            g_term.scroll = std::max(0, g_term.scroll + delta * 3 / WHEEL_DELTA);
            TermInvalidate();
        } else if (pt.y >= g_pvTop && pt.y < g_pvBottom) {
            PvWheel(delta, pt.y);
            int i = PvHitTest(pt.x, pt.y);
            SetHover(i >= 0 ? K_TASK : K_NONE, i);
        } else if (pt.y >= g_pinsTop && pt.y < g_pinsBottom) {
            g_scrollPins -= delta / WHEEL_DELTA * (g_lineH + g_cfg.vpad) * 3;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: {
        int x = (short)LOWORD(lp) - BarOX(), y = (short)HIWORD(lp);
        if (InTerminal(x, y)) {
            if (GetForegroundWindow() != hwnd) ForceForeground(hwnd);
            SetFocus(hwnd);
            TermStart();
            if (InTermOutput(x, y)) {
                TermPos p = TermPosAt(x, y);
                if (msg == WM_LBUTTONDBLCLK) {
                    TermSelectWord(p);                    // double-click: the word under the mouse
                } else {
                    if (!((wp & MK_SHIFT) && g_sel)) g_selA = p;   // shift-click extends a selection
                    g_selB = p;
                    g_sel = (wp & MK_SHIFT) && (g_selA.line != p.line || g_selA.col != p.col);
                    g_selDrag = true;
                    SetCapture(hwnd);
                }
            }
            TermCaretReset();
            TermInvalidate();
        }
        return 0;
    }
    case WM_CAPTURECHANGED:
        if ((HWND)lp != hwnd) g_selDrag = false;
        return 0;
    case WM_LBUTTONUP: {
        KillTimer(hwnd, TIMER_HOVER);                 // a click wins over a pending hover-raise
        int x = (short)LOWORD(lp) - BarOX(), y = (short)HIWORD(lp);
        if (g_selDrag) {                              // end of a text selection drag
            TermDragTo(x, y);
            g_selDrag = false;
            ReleaseCapture();
            return 0;
        }
        if (InTerminal(x, y)) return 0;
        if (y >= g_pvTop && y < g_pvBottom) {
            int i = PvHitTest(x, y);
            if (i >= 0) ActivateOrMinimize(g_tasks[i].hwnd);
        } else if (const Row* r = HitTest(x, y)) {
            if (r->kind == K_START) { PressKeys({VK_LWIN}); return 0; }
            if (r->kind == K_CLOCK) { CalToggle(); return 0; }
            if (r->kind == K_PIN && r->idx < PinCount()) LaunchPin(PinAt(r->idx));
        }
        if (RefreshTasks()) PvSync();
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_MBUTTONUP: {
        int x = (short)LOWORD(lp) - BarOX(), y = (short)HIWORD(lp);
        if (y >= g_pvTop && y < g_pvBottom) {
            int i = PvHitTest(x, y);
            if (i >= 0) PostMessageW(g_tasks[i].hwnd, WM_CLOSE, 0, 0);
        }
        return 0;
    }
    case WM_RBUTTONUP: {
        POINT pt{(short)LOWORD(lp), (short)HIWORD(lp)};       // window coords, for placing the menu
        int bx = pt.x - BarOX(), by = pt.y;                   // bar coords, for hit-testing
        int kind = K_NONE, idx = -1;
        if (InTerminal(bx, by)) kind = K_TERM;
        else if (by >= g_pvTop && by < g_pvBottom) {
            idx = PvHitTest(bx, by);
            if (idx >= 0) kind = K_TASK;
        } else if (const Row* r = HitTest(bx, by)) {
            kind = r->kind;
            idx = r->idx;
        }
        if (kind == K_START) { PressKeys({VK_LWIN, 'X'}); return 0; }
        ClientToScreen(hwnd, &pt);
        if (kind == K_TERM) ShowTermMenu(pt);
        else if (kind == K_PIN && idx < PinCount()) ShowPinMenu(idx, pt);
        else if (kind == K_TASK && idx < (int)g_tasks.size()) ShowTaskMenu(g_tasks[idx].hwnd, pt);
        else ShowMenu(pt);
        return 0;
    }

    case WM_DISPLAYCHANGE:
        AppBarSetPos();
        return 0;

    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        Paint(hwnd);
        return 0;

    case WM_DESTROY:
        for (UINT_PTR id : {TIMER_SCAN, TIMER_CLOCK, TIMER_HOVER, TIMER_METER, TIMER_BLINK,
                            TIMER_ANIM, TIMER_RECENTER, TIMER_CARET, TIMER_TERMEXIT})
            KillTimer(hwnd, id);
        UnregisterHotKey(hwnd, HOTKEY_TERM);
        TermStop();
        for (auto& t : g_tasks) ThumbRelease(t);
        AudioRelease();
        if (g_mmEnum) { g_mmEnum->Release(); g_mmEnum = nullptr; }
        DeregisterShellHookWindow(hwnd);
        AppBarRemove();
        ShowSystemTaskbars(true);
        if (!g_loadedFontFile.empty()) RemoveFontResourceExW(g_loadedFontFile.c_str(), FR_PRIVATE, nullptr);
        for (HFONT f : {g_font, g_clockFont, g_calBigFont, g_termFont}) if (f) DeleteObject(f);
        WatchResetFonts();
        if (g_calHwnd) DestroyWindow(g_calHwnd);
        g_pinIcons.clear();
        g_tasks.clear();
        if (g_iconDC) DeleteDC(g_iconDC);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------- entry ----------

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    InitializeCriticalSection(&g_termCs);
    SetConsoleCtrlHandler(IgnoreCtrl, TRUE);

    bool wantSettings = false;
    int argc = 0;
    if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
        for (int i = 1; i < argc; ++i)
            if (!_wcsicmp(argv[i], L"--settings") || !_wcsicmp(argv[i], L"/settings")) wantSettings = true;
        LocalFree(argv);
    }

    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"pbar_single_instance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        // already running: "pbar.exe --settings" asks that one to open its settings
        if (wantSettings)
            if (HWND other = FindWindowW(L"PBarWnd", nullptr)) {
                DWORD pid = 0;
                GetWindowThreadProcessId(other, &pid);
                AllowSetForegroundWindow(pid);
                PostMessageW(other, WM_OPENSETTINGS, 0, 0);
            }
        return 0;
    }

    Gdiplus::GdiplusStartupInput gdipIn;
    Gdiplus::GdiplusStartup(&g_gdipToken, &gdipIn, nullptr);

    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    g_exeDir = exe;
    g_exeDir = g_exeDir.substr(0, g_exeDir.find_last_of(L"\\/"));
    g_iniPath = g_exeDir + L"\\pbar.ini";

    LoadConfig();

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"PBarWnd";
    wc.style = CS_DBLCLKS;                                // double-click selects a word in the terminal
    RegisterClassExW(&wc);

    // No WS_EX_NOACTIVATE: the terminal needs keyboard focus. WM_MOUSEACTIVATE keeps every other click
    // from activating the bar, and WS_EX_TOOLWINDOW keeps it out of Alt+Tab.
    g_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"PBarWnd", L"pbar",
                             WS_POPUP | WS_CLIPCHILDREN, 0, 0, g_cfg.width, 100, nullptr, nullptr, hInst, nullptr);
    if (!g_hwnd) return 1;

    // The smartwatch clock: a child window the bar positions over the clock slot (hidden for clockstyle=classic).
    WNDCLASSEXW ww{};
    ww.cbSize = sizeof(ww);
    ww.lpfnWndProc = WatchProc;
    ww.hInstance = hInst;
    ww.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    ww.lpszClassName = L"PBarWatch";
    RegisterClassExW(&ww);
    g_watchHwnd = CreateWindowExW(0, L"PBarWatch", L"", WS_CHILD, 0, 0, 0, 0, g_hwnd, nullptr, hInst, nullptr);

    CreateBarFont();
    g_shellMsg = RegisterWindowMessageW(L"SHELLHOOK");
    g_taskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");
    RegisterShellHookWindow(g_hwnd);
    AppBarRegister();
    if (g_cfg.hideTaskbar) ShowSystemTaskbars(false);
    RefreshTasks();
    PvSetIdleAnchor();
    RequestStartPins();
    RegisterTermHotkey();

    SetTimer(g_hwnd, TIMER_SCAN, 500, nullptr);
    SetTimer(g_hwnd, TIMER_CLOCK, 1000, nullptr);
    StartMeterTimer();
    ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
    UpdateWindow(g_hwnd);
    if (wantSettings) OpenSettings();

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (SettingsPreTranslate(&m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    if (mutex) { ReleaseMutex(mutex); CloseHandle(mutex); }
    DeleteCriticalSection(&g_termCs);
    if (g_gdipToken) Gdiplus::GdiplusShutdown(g_gdipToken);
    CoUninitialize();
    return 0;
}
