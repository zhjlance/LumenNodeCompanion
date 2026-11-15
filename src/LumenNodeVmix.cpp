#pragma warning(disable : 4996)

#ifndef NOMINMAX
#define NOMINMAX
#endif

// Enable modern Windows 10/11 Visual Styles (Common Controls v6)
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <dwmapi.h>
#include <shlobj.h>
#include <objbase.h>
#include <oleauto.h>
#include <UIAutomation.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shell32.lib")

#define IDI_ICON1 101
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

// ============================================================================
// 1. Data Structures & Global State
// ============================================================================
struct DiscoveredNode {
    std::string ip;
    int srt_port = 10000;
    int http_port = 8080;
    int latency_ms = 25;
};

static std::mutex g_mutex;
static std::vector<DiscoveredNode> g_nodes;
static std::string g_active_ip;
static int g_active_srt_port = 10000;
static int g_active_http_port = 8080;
static int g_active_latency_ms = 25;
static std::atomic<bool> g_running{ true };
static std::atomic<bool> g_auto_watch_vmix_dialog{ true };

// Win32 GUI Control IDs
#define IDC_COMBO_DEVICES   1001
#define IDC_EDIT_IP         1002
#define IDC_EDIT_SRT_PORT   1003
#define IDC_EDIT_HTTP_PORT  1004
#define IDC_EDIT_LATENCY    1005
#define IDC_CHK_AUTO_FILL   1006
#define IDC_BTN_FILL_ONLY   1007
#define IDC_BTN_FILL_AND_OK 1008
#define IDC_BTN_INSTALL_OBS 1009
static HWND g_hBtnInstallObs = nullptr;
static HWND g_hLblObsStatus = nullptr;
#define WM_NODE_DISCOVERED  (WM_USER + 101)
#define IDT_WATCH_VMIX      2001

// GUI Control Handles
static HWND g_hMainWnd = nullptr;
static HWND g_hGrpParams = nullptr;
static HWND g_hLblDevice = nullptr;
static HWND g_hComboDevices = nullptr;
static HWND g_hLblIp = nullptr;
static HWND g_hEditIp = nullptr;
static HWND g_hLblSrtPort = nullptr;
static HWND g_hEditSrtPort = nullptr;
static HWND g_hLblHttpPort = nullptr;
static HWND g_hEditHttpPort = nullptr;
static HWND g_hLblLatency = nullptr;
static HWND g_hEditLatency = nullptr;

static HWND g_hGrpActions = nullptr;
static HWND g_hChkAutoFill = nullptr;
static HWND g_hBtnFillOnly = nullptr;
static HWND g_hBtnFillAndOk = nullptr;
static HWND g_hNoteFillOnly = nullptr;
static HWND g_hNoteFillAndOk = nullptr;
static HWND g_hLblStatus = nullptr;

static HWND g_lastAutoFilledDialog = nullptr;

// GDI Resources & DPI State
static UINT g_currentDpi = 96;
static HFONT g_hFontNormal = nullptr;
static HFONT g_hFontGroup = nullptr;
static HFONT g_hFontSmall = nullptr;
static HBRUSH g_hBrushBg = nullptr;

// 新增 UI 卡片分组标题句柄、画刷与卡片矩形区域（配合下方函数使用）
static HWND g_hGrpObs = nullptr;
static HBRUSH g_hBrushCard = nullptr;
static HBRUSH g_hBrushEdit = nullptr;
static RECT g_rcCardNodes{};
static RECT g_rcCardVmix{};
static RECT g_rcCardObs{};
static RECT g_rcCardStatus{};

static void EnableDarkTitleBar(HWND hwnd) {
    if (!hwnd) return;
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark)); // DWMWA_USE_IMMERSIVE_DARK_MODE
    DwmSetWindowAttribute(hwnd, 19, &dark, sizeof(dark)); // fallback for older builds
}

static bool IsRunAsAdmin() {
    BOOL isAdmin = FALSE;
    PSID adminGroup = nullptr;
    SID_IDENTIFIER_AUTHORITY ntAuth = SECURITY_NT_AUTHORITY;
    if (AllocateAndInitializeSid(&ntAuth, 2, SECURITY_BUILTIN_DOMAIN_RID,
        DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &adminGroup)) {
        CheckTokenMembership(nullptr, adminGroup, &isAdmin);
        FreeSid(adminGroup);
    }
    return isAdmin == TRUE;
}

static void RelaunchAsAdmin() {
    wchar_t exePath[MAX_PATH] = { 0 };
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    SHELLEXECUTEINFOW sei = { sizeof(sei) };
    sei.lpVerb = L"runas";
    sei.lpFile = exePath;
    sei.nShow = SW_SHOWNORMAL;
    if (ShellExecuteExW(&sei)) {
        ExitProcess(0);
    }
}

static int ScaleDpi(int px) {
    return MulDiv(px, static_cast<int>(g_currentDpi), 96);
}

static void EnablePerMonitorDpiAwareness() {
    HMODULE hUser32 = GetModuleHandleW(L"user32.dll");
    if (hUser32) {
        typedef BOOL(WINAPI* SetProcessDpiAwarenessContextFn)(DPI_AWARENESS_CONTEXT);
        auto fn = reinterpret_cast<SetProcessDpiAwarenessContextFn>(
            GetProcAddress(hUser32, "SetProcessDpiAwarenessContext"));
        if (fn) {
            if (fn(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) return;
            fn(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE);
            return;
        }
        typedef BOOL(WINAPI* SetProcessDPIAwareFn)(void);
        auto fnLegacy = reinterpret_cast<SetProcessDPIAwareFn>(
            GetProcAddress(hUser32, "SetProcessDPIAware"));
        if (fnLegacy) fnLegacy();
    }
}

static UINT QueryWindowDpi(HWND hwnd) {
    HMODULE hUser32 = GetModuleHandleW(L"user32.dll");
    if (hUser32 && hwnd) {
        typedef UINT(WINAPI* GetDpiForWindowFn)(HWND);
        auto fn = reinterpret_cast<GetDpiForWindowFn>(GetProcAddress(hUser32, "GetDpiForWindow"));
        if (fn) {
            UINT dpi = fn(hwnd);
            if (dpi > 0) return dpi;
        }
    }
    HDC hdc = GetDC(hwnd);
    UINT dpi = 96;
    if (hdc) {
        dpi = static_cast<UINT>(GetDeviceCaps(hdc, LOGPIXELSX));
        ReleaseDC(hwnd, hdc);
    }
    return (dpi > 0) ? dpi : 96;
}
static void RebuildFontsForDpi() {
    if (g_hFontNormal) DeleteObject(g_hFontNormal);
    if (g_hFontGroup) DeleteObject(g_hFontGroup);
    if (g_hFontSmall) DeleteObject(g_hFontSmall);

    g_hFontNormal = CreateFontW(-ScaleDpi(13), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    g_hFontGroup = CreateFontW(-ScaleDpi(13), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    g_hFontSmall = CreateFontW(-ScaleDpi(11), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

    HWND normalCtrls[] = {
        g_hLblDevice, g_hComboDevices, g_hLblIp, g_hEditIp,
        g_hLblSrtPort, g_hEditSrtPort, g_hLblHttpPort, g_hEditHttpPort,
        g_hLblLatency, g_hEditLatency, g_hChkAutoFill, g_hLblStatus,
        g_hLblObsStatus
    };
    for (HWND h : normalCtrls) {
        if (h) SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(g_hFontNormal), TRUE);
    }

    HWND boldCtrls[] = {
        g_hGrpParams, g_hGrpActions, g_hGrpObs,
        g_hBtnFillOnly, g_hBtnFillAndOk, g_hBtnInstallObs
    };
    for (HWND h : boldCtrls) {
        if (h) SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(g_hFontGroup), TRUE);
    }

    HWND smallCtrls[] = { g_hNoteFillOnly, g_hNoteFillAndOk };
    for (HWND h : smallCtrls) {
        if (h) SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(g_hFontSmall), TRUE);
    }
}

static void LayoutControls(int clientW, int clientH) {
    (void)clientH;
    if (!g_hGrpParams) return;

    int margin = ScaleDpi(16);
    int innerPad = ScaleDpi(18);
    int cardW = std::max(ScaleDpi(420), clientW - margin * 2);
    int leftX = margin + innerPad;
    int rightEdge = margin + cardW - innerPad;
    int contentW = rightEdge - leftX;
    int rowH = ScaleDpi(24);

    // =========================================================================
    // 卡片 1: Lumen Nodes (设备发现与网络参数框)
    // =========================================================================
    int card1Top = ScaleDpi(14);
    int card1H = ScaleDpi(168);
    g_rcCardNodes = { margin, card1Top, margin + cardW, card1Top + card1H };

    MoveWindow(g_hGrpParams, leftX + ScaleDpi(8), card1Top + ScaleDpi(12), contentW - ScaleDpi(8), ScaleDpi(20), TRUE);

    int y1 = card1Top + ScaleDpi(46);
    int labelW = ScaleDpi(135);
    int comboX = leftX + labelW + ScaleDpi(8);
    MoveWindow(g_hLblDevice, leftX, y1 + ScaleDpi(3), labelW, rowH, TRUE);
    MoveWindow(g_hComboDevices, comboX, y1, rightEdge - comboX, ScaleDpi(200), TRUE);

    int colGap = ScaleDpi(20);
    int halfW = (contentW - colGap) / 2;
    int col1LblW = ScaleDpi(115);
    int col1EditX = leftX + col1LblW;
    int col1EditW = std::max(ScaleDpi(90), halfW - col1LblW);

    int col2LeftX = leftX + halfW + colGap;
    int col2LblW = ScaleDpi(115);
    int col2EditX = col2LeftX + col2LblW;
    int col2EditW = std::max(ScaleDpi(70), rightEdge - col2EditX);

    int y2 = y1 + ScaleDpi(38);
    MoveWindow(g_hLblIp, leftX, y2 + ScaleDpi(3), col1LblW, rowH, TRUE);
    MoveWindow(g_hEditIp, col1EditX, y2, col1EditW, rowH, TRUE);
    MoveWindow(g_hLblLatency, col2LeftX, y2 + ScaleDpi(3), col2LblW, rowH, TRUE);
    MoveWindow(g_hEditLatency, col2EditX, y2, col2EditW, rowH, TRUE);

    int y3 = y2 + ScaleDpi(36);
    MoveWindow(g_hLblSrtPort, leftX, y3 + ScaleDpi(3), col1LblW, rowH, TRUE);
    MoveWindow(g_hEditSrtPort, col1EditX, y3, col1EditW, rowH, TRUE);
    MoveWindow(g_hLblHttpPort, col2LeftX, y3 + ScaleDpi(3), col2LblW, rowH, TRUE);
    MoveWindow(g_hEditHttpPort, col2EditX, y3, col2EditW, rowH, TRUE);

    // =========================================================================
    // 卡片 2: vMix (vMix 专用导入与自动填充框)
    // =========================================================================
    int card2Top = g_rcCardNodes.bottom + ScaleDpi(14);
    int card2H = ScaleDpi(190);
    g_rcCardVmix = { margin, card2Top, margin + cardW, card2Top + card2H };

    MoveWindow(g_hGrpActions, leftX + ScaleDpi(8), card2Top + ScaleDpi(12), contentW - ScaleDpi(8), ScaleDpi(20), TRUE);

    int chkY = card2Top + ScaleDpi(44);
    MoveWindow(g_hChkAutoFill, leftX, chkY, contentW, ScaleDpi(24), TRUE);

    int btnY = chkY + ScaleDpi(32);
    int btnH = ScaleDpi(38);
    int btnGap = ScaleDpi(16);
    int btnW = (contentW - btnGap) / 2;
    int btn2X = leftX + btnW + btnGap;

    MoveWindow(g_hBtnFillOnly, leftX, btnY, btnW, btnH, TRUE);
    MoveWindow(g_hBtnFillAndOk, btn2X, btnY, btnW, btnH, TRUE);

    int noteY = btnY + btnH + ScaleDpi(8);
    int noteH = ScaleDpi(46);
    MoveWindow(g_hNoteFillOnly, leftX + ScaleDpi(2), noteY, btnW - ScaleDpi(4), noteH, TRUE);
    MoveWindow(g_hNoteFillAndOk, btn2X + ScaleDpi(2), noteY, btnW - ScaleDpi(4), noteH, TRUE);

    // =========================================================================
    // 卡片 3: OBS Studio (OBS 专用插件一键安装框，加高说明文字区防止第 3 行被裁切)
    // =========================================================================
    int card3Top = g_rcCardVmix.bottom + ScaleDpi(14);
    int card3H = ScaleDpi(118);
    g_rcCardObs = { margin, card3Top, margin + cardW, card3Top + card3H };

    if (g_hGrpObs) {
        MoveWindow(g_hGrpObs, leftX + ScaleDpi(8), card3Top + ScaleDpi(12), contentW - ScaleDpi(8), ScaleDpi(20), TRUE);
    }

    int obsBodyY = card3Top + ScaleDpi(48);
    int obsBtnW = ScaleDpi(190);
    int obsBtnH = ScaleDpi(40);
    MoveWindow(g_hBtnInstallObs, leftX, obsBodyY, obsBtnW, obsBtnH, TRUE);

    int obsDescX = leftX + obsBtnW + ScaleDpi(16);
    MoveWindow(g_hLblObsStatus, obsDescX, card3Top + ScaleDpi(44), rightEdge - obsDescX, ScaleDpi(62), TRUE);

    // =========================================================================
    // 底部状态栏框: Status Bar
    // =========================================================================
    int card4Top = g_rcCardObs.bottom + ScaleDpi(14);
    int card4H = ScaleDpi(40);
    g_rcCardStatus = { margin, card4Top, margin + cardW, card4Top + card4H };

    MoveWindow(g_hLblStatus, leftX + ScaleDpi(12), card4Top + ScaleDpi(10), contentW - ScaleDpi(16), ScaleDpi(22), TRUE);

    InvalidateRect(g_hMainWnd, nullptr, TRUE);
    UpdateWindow(g_hMainWnd);
}

static int ExtractJsonInt(const char* json, const char* key, int default_val) {
    if (!json || !key) return default_val;
    std::string pattern = std::string("\"") + key + "\":";
    const char* pos = strstr(json, pattern.c_str());
    if (!pos) return default_val;
    pos += pattern.size();
    while (*pos == ' ' || *pos == '\t') ++pos;
    int val = atoi(pos);
    return (val > 0) ? val : default_val;
}

static std::wstring ToWide(const std::string& str) {
    if (str.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, nullptr, 0);
    std::wstring wstr(len > 0 ? len - 1 : 0, L'\0');
    if (len > 1) {
        MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, &wstr[0], len);
    }
    return wstr;
}

static std::string GetEditString(HWND hEdit) {
    char buf[128] = { 0 };
    GetWindowTextA(hEdit, buf, sizeof(buf) - 1);
    return std::string(buf);
}

static bool InstallObsPlugin(std::wstring& out_error) {
    wchar_t exePath[MAX_PATH] = { 0 };
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring exeDir = exePath;
    size_t pos = exeDir.find_last_of(L"\\/");
    if (pos != std::wstring::npos) exeDir = exeDir.substr(0, pos);

    std::wstring candidates[] = {
        exeDir + L"\\lumen-node-obs.dll",
        exeDir + L"\\obs-lumen-source.dll",
        exeDir + L"\\..\\lumen-node-obs.dll",
        exeDir + L"\\..\\..\\lumen-node-obs.dll"
    };

    std::wstring srcDll;
    for (const auto& cand : candidates) {
        if (GetFileAttributesW(cand.c_str()) != INVALID_FILE_ATTRIBUTES) {
            srcDll = cand;
            break;
        }
    }

    if (srcDll.empty()) {
        out_error = L"OBS: lumen-node-obs.dll not found next to the program.\nLooked in: " + exeDir;
        return false;
    }

    // 1. 安装到 OBS Studio 标准 ProgramData 插件目录: %ProgramData%\obs-studio\plugins\lumen-node-obs\bin\64bit\lumen-node-obs.dll
    wchar_t programData[MAX_PATH] = { 0 };
    SHGetFolderPathW(nullptr, CSIDL_COMMON_APPDATA, nullptr, 0, programData);
    std::wstring baseDir = std::wstring(programData) + L"\\obs-studio";
    std::wstring pluginsDir = baseDir + L"\\plugins";
    std::wstring modDir = pluginsDir + L"\\lumen-node-obs";
    std::wstring binDir = modDir + L"\\bin";
    std::wstring bitDir = binDir + L"\\64bit";
    std::wstring destDll = bitDir + L"\\lumen-node-obs.dll";

    CreateDirectoryW(baseDir.c_str(), nullptr);
    CreateDirectoryW(pluginsDir.c_str(), nullptr);
    CreateDirectoryW(modDir.c_str(), nullptr);
    CreateDirectoryW(binDir.c_str(), nullptr);
    CreateDirectoryW(bitDir.c_str(), nullptr);

    // 清理旧版直接放在 modDir 根目录下的无效同名 DLL
    std::wstring legacyDll = modDir + L"\\lumen-node-obs.dll";
    if (GetFileAttributesW(legacyDll.c_str()) != INVALID_FILE_ATTRIBUTES) {
        SetFileAttributesW(legacyDll.c_str(), FILE_ATTRIBUTE_NORMAL);
        DeleteFileW(legacyDll.c_str());
    }

    if (GetFileAttributesW(destDll.c_str()) != INVALID_FILE_ATTRIBUTES) {
        SetFileAttributesW(destDll.c_str(), FILE_ATTRIBUTE_NORMAL);
    }

    bool copiedOk = (CopyFileW(srcDll.c_str(), destDll.c_str(), FALSE) != FALSE);

    // 2. 若系统默认安装路径 C:\Program Files\obs-studio\obs-plugins\64bit 存在，同步覆盖更新，防止加载旧版本缓存
    std::wstring progFilesObsDir = L"C:\\Program Files\\obs-studio\\obs-plugins\\64bit";
    if (GetFileAttributesW(progFilesObsDir.c_str()) != INVALID_FILE_ATTRIBUTES) {
        std::wstring pfDll1 = progFilesObsDir + L"\\lumen-node-obs.dll";
        std::wstring pfDll2 = progFilesObsDir + L"\\obs-lumen-source.dll";
        SetFileAttributesW(pfDll1.c_str(), FILE_ATTRIBUTE_NORMAL);
        CopyFileW(srcDll.c_str(), pfDll1.c_str(), FALSE);
        if (GetFileAttributesW(pfDll2.c_str()) != INVALID_FILE_ATTRIBUTES) {
            SetFileAttributesW(pfDll2.c_str(), FILE_ATTRIBUTE_NORMAL);
            CopyFileW(srcDll.c_str(), pfDll2.c_str(), FALSE);
        }
    }

    if (copiedOk) {
        return true;
    }

    DWORD err = GetLastError();
    if (err == ERROR_ACCESS_DENIED) {
        out_error = L"OBS: Access denied. Please run this program as Administrator.";
    }
    else if (err == ERROR_SHARING_VIOLATION) {
        out_error = L"OBS: DLL is in use. Please close OBS Studio completely and try again.";
    }
    else {
        out_error = L"OBS: Failed to copy DLL (error " + std::to_wstring(err) + L"). Close OBS and run as Administrator.";
    }
    return false;
}

static void SyncGuiToGlobalConfig() {
    if (!g_hMainWnd) return;
    std::string ip = GetEditString(g_hEditIp);
    int srt_port = atoi(GetEditString(g_hEditSrtPort).c_str());
    int http_port = atoi(GetEditString(g_hEditHttpPort).c_str());
    int latency = atoi(GetEditString(g_hEditLatency).c_str());

    std::lock_guard<std::mutex> lk(g_mutex);
    if (!ip.empty()) g_active_ip = ip;
    if (srt_port > 0 && srt_port <= 65535) g_active_srt_port = srt_port;
    if (http_port > 0 && http_port <= 65535) g_active_http_port = http_port;
    if (latency >= 5 && latency <= 1000) g_active_latency_ms = latency;
}

// ============================================================================
// 2. Reliable vMix "Input Select -> Stream / SRT" Inspection & Auto-Fill
// ============================================================================
struct FindVmixWndCtx {
    HWND hMainVmix = nullptr;
    HWND hInputSelect = nullptr;
};

static BOOL CALLBACK EnumVmixWindowsProc(HWND hwnd, LPARAM lParam) {
    if (!IsWindowVisible(hwnd)) return TRUE;
    wchar_t title[256] = { 0 };
    GetWindowTextW(hwnd, title, 255);
    if (wcslen(title) == 0) return TRUE;

    auto* ctx = reinterpret_cast<FindVmixWndCtx*>(lParam);
    if (wcsstr(title, L"Input Select") != nullptr || wcsstr(title, L"输入选择") != nullptr) {
        ctx->hInputSelect = hwnd;
    }
    else if (wcsncmp(title, L"vMix", 4) == 0 && wcsstr(title, L"Lumen") == nullptr) {
        ctx->hMainVmix = hwnd;
    }
    return TRUE;
}

static void ClickScreenPoint(int x, int y) {
    SetCursorPos(x, y);
    mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);
    Sleep(20);
    mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
}

static void ClickScreenRectCenter(RECT rc) {
    ClickScreenPoint((rc.left + rc.right) / 2, (rc.top + rc.bottom) / 2);
}

struct StreamTabControls {
    HWND hStreamTypeCombo = nullptr;
    RECT rcCombo{};
    std::vector<HWND> visibleEdits;
    HWND hOkButton = nullptr;
};

static BOOL CALLBACK EnumInputSelectChildrenProc(HWND hwnd, LPARAM lParam) {
    if (!IsWindowVisible(hwnd)) return TRUE;

    auto* info = reinterpret_cast<StreamTabControls*>(lParam);
    char clsName[128] = { 0 };
    GetClassNameA(hwnd, clsName, sizeof(clsName) - 1);
    _strupr(clsName);

    if (strstr(clsName, "COMBOBOX") != nullptr) {
        LRESULT idxCaller = SendMessageA(hwnd, CB_FINDSTRING, static_cast<WPARAM>(-1),
            reinterpret_cast<LPARAM>("SRT (Caller)"));
        LRESULT idxRtsp = SendMessageA(hwnd, CB_FINDSTRING, static_cast<WPARAM>(-1),
            reinterpret_cast<LPARAM>("RTSP over UDP"));
        if (idxCaller != CB_ERR || idxRtsp != CB_ERR) {
            info->hStreamTypeCombo = hwnd;
            GetWindowRect(hwnd, &info->rcCombo);
        }
    }
    else if (strstr(clsName, "EDIT") != nullptr) {
        info->visibleEdits.push_back(hwnd);
    }
    else if (strstr(clsName, "BUTTON") != nullptr) {
        wchar_t btnText[64] = { 0 };
        GetWindowTextW(hwnd, btnText, 63);
        if (wcscmp(btnText, L"OK") == 0 || wcscmp(btnText, L"确定") == 0) {
            info->hOkButton = hwnd;
        }
    }
    return TRUE;
}

static void SetWinFormsEditText(HWND hwndEdit, const std::string& text) {
    if (!hwndEdit) return;
    SendMessageA(hwndEdit, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(text.c_str()));
    HWND hParent = GetParent(hwndEdit);
    if (hParent) {
        SendMessageW(hParent, WM_COMMAND,
            MAKEWPARAM(GetDlgCtrlID(hwndEdit), EN_CHANGE),
            reinterpret_cast<LPARAM>(hwndEdit));
        SendMessageW(hParent, WM_COMMAND,
            MAKEWPARAM(GetDlgCtrlID(hwndEdit), EN_UPDATE),
            reinterpret_cast<LPARAM>(hwndEdit));
    }
}

static bool SelectSrtCallerInCombo(HWND hCombo) {
    if (!hCombo || !IsWindowVisible(hCombo)) return false;

    LRESULT idx = SendMessageA(hCombo, CB_FINDSTRING, static_cast<WPARAM>(-1),
        reinterpret_cast<LPARAM>("SRT (Caller)"));
    if (idx == CB_ERR) return false;

    LRESULT cur = SendMessageA(hCombo, CB_GETCURSEL, 0, 0);
    if (cur != idx) {
        SendMessageA(hCombo, CB_SETCURSEL, static_cast<WPARAM>(idx), 0);
        HWND hParent = GetParent(hCombo);
        if (hParent) {
            int ctrlId = GetDlgCtrlID(hCombo);
            SendMessageW(hParent, WM_COMMAND, MAKEWPARAM(ctrlId, CBN_SELCHANGE), reinterpret_cast<LPARAM>(hCombo));
            SendMessageW(hParent, WM_COMMAND, MAKEWPARAM(ctrlId, CBN_SELENDOK), reinterpret_cast<LPARAM>(hCombo));
        }
        Sleep(150);
    }
    return true;
}

static void ClickStreamSrtTabInDialog(IUIAutomation* uia, HWND hInputSelect) {
    SetForegroundWindow(hInputSelect);
    Sleep(60);

    bool clicked_via_uia = false;
    IUIAutomationElement* dlgRoot = nullptr;
    if (uia && SUCCEEDED(uia->ElementFromHandle(hInputSelect, &dlgRoot)) && dlgRoot) {
        IUIAutomationCondition* trueCond = nullptr;
        uia->CreateTrueCondition(&trueCond);
        IUIAutomationElementArray* allElems = nullptr;
        if (SUCCEEDED(dlgRoot->FindAll(TreeScope_Descendants, trueCond, &allElems)) && allElems) {
            int count = 0;
            allElems->get_Length(&count);
            for (int i = 0; i < count; ++i) {
                IUIAutomationElement* el = nullptr;
                allElems->GetElement(i, &el);
                if (!el) continue;

                BSTR name = nullptr;
                el->get_CurrentName(&name);
                if (name && (wcsstr(name, L"Stream / SRT") != nullptr || wcsstr(name, L"Stream/SRT") != nullptr)) {
                    RECT rc{};
                    if (SUCCEEDED(el->get_CurrentBoundingRectangle(&rc)) && (rc.right - rc.left) > 10) {
                        ClickScreenRectCenter(rc);
                        clicked_via_uia = true;
                    }
                    SysFreeString(name);
                    el->Release();
                    break;
                }
                if (name) SysFreeString(name);
                el->Release();
            }
            allElems->Release();
        }
        if (trueCond) trueCond->Release();
        dlgRoot->Release();
    }

    if (!clicked_via_uia) {
        RECT dlgRc{};
        GetWindowRect(hInputSelect, &dlgRc);
        ClickScreenPoint(dlgRc.left + 85, dlgRc.top + 288);
    }
}

static bool AutoFillVmixInputSelectWindow(bool force_open_and_switch_tab, bool auto_click_ok, std::wstring& out_status) {
    SyncGuiToGlobalConfig();

    std::string ip;
    int srt_port = 10000;
    int latency_ms = 25;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        ip = g_active_ip;
        srt_port = g_active_srt_port;
        latency_ms = g_active_latency_ms;
    }

    if (ip.empty()) {
        out_status = L"Status: No camera IP available. Start Lumen Node on your phone or enter an IP above.";
        return false;
    }

    FindVmixWndCtx wndCtx;
    EnumWindows(EnumVmixWindowsProc, reinterpret_cast<LPARAM>(&wndCtx));

    IUIAutomation* uia = nullptr;
    CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&uia));

    if (!wndCtx.hInputSelect && force_open_and_switch_tab) {
        if (!wndCtx.hMainVmix) {
            if (uia) uia->Release();
            out_status = L"Status: vMix is not running. Please launch vMix first.";
            return false;
        }

        SetForegroundWindow(wndCtx.hMainVmix);
        Sleep(80);

        bool clicked_add_input = false;
        IUIAutomationElement* rootMain = nullptr;
        if (uia && SUCCEEDED(uia->ElementFromHandle(wndCtx.hMainVmix, &rootMain)) && rootMain) {
            IUIAutomationCondition* trueCond = nullptr;
            uia->CreateTrueCondition(&trueCond);
            IUIAutomationElementArray* allMain = nullptr;
            if (SUCCEEDED(rootMain->FindAll(TreeScope_Descendants, trueCond, &allMain)) && allMain) {
                int len = 0;
                allMain->get_Length(&len);
                for (int i = 0; i < len; ++i) {
                    IUIAutomationElement* item = nullptr;
                    allMain->GetElement(i, &item);
                    if (!item) continue;
                    BSTR name = nullptr;
                    item->get_CurrentName(&name);
                    if (name && (wcscmp(name, L"Add Input") == 0 || wcscmp(name, L"添加输入") == 0)) {
                        RECT rc{};
                        if (SUCCEEDED(item->get_CurrentBoundingRectangle(&rc)) && rc.right > rc.left) {
                            ClickScreenRectCenter(rc);
                            clicked_add_input = true;
                        }
                        SysFreeString(name);
                        item->Release();
                        break;
                    }
                    if (name) SysFreeString(name);
                    item->Release();
                }
                allMain->Release();
            }
            if (trueCond) trueCond->Release();
            rootMain->Release();
        }

        if (!clicked_add_input) {
            RECT mainRc{};
            GetWindowRect(wndCtx.hMainVmix, &mainRc);
            ClickScreenPoint(mainRc.left + 30, mainRc.bottom - 35);
        }

        for (int wait_i = 0; wait_i < 15; ++wait_i) {
            Sleep(100);
            EnumWindows(EnumVmixWindowsProc, reinterpret_cast<LPARAM>(&wndCtx));
            if (wndCtx.hInputSelect) break;
        }
    }

    if (!wndCtx.hInputSelect) {
        if (uia) uia->Release();
        if (force_open_and_switch_tab) {
            out_status = L"Status: Please click [Add Input] -> [Stream / SRT] in vMix.";
        }
        return false;
    }

    StreamTabControls tabCtrl{};
    EnumChildWindows(wndCtx.hInputSelect, EnumInputSelectChildrenProc, reinterpret_cast<LPARAM>(&tabCtrl));

    if (!tabCtrl.hStreamTypeCombo) {
        if (!force_open_and_switch_tab) {
            if (uia) uia->Release();
            return false;
        }

        ClickStreamSrtTabInDialog(uia, wndCtx.hInputSelect);

        for (int retry = 0; retry < 12; ++retry) {
            Sleep(80);
            tabCtrl = StreamTabControls{};
            EnumChildWindows(wndCtx.hInputSelect, EnumInputSelectChildrenProc, reinterpret_cast<LPARAM>(&tabCtrl));
            if (tabCtrl.hStreamTypeCombo) break;
        }
    }

    if (!tabCtrl.hStreamTypeCombo) {
        if (uia) uia->Release();
        out_status = L"Status: Please click [Stream / SRT] on the left sidebar of vMix Input Select.";
        return false;
    }

    SelectSrtCallerInCombo(tabCtrl.hStreamTypeCombo);

    tabCtrl = StreamTabControls{};
    EnumChildWindows(wndCtx.hInputSelect, EnumInputSelectChildrenProc, reinterpret_cast<LPARAM>(&tabCtrl));

    if (!tabCtrl.hStreamTypeCombo) {
        if (uia) uia->Release();
        return false;
    }

    RECT dlgRect{};
    GetWindowRect(wndCtx.hInputSelect, &dlgRect);

    struct SortedEdit {
        HWND hwnd;
        RECT rc;
    };
    std::vector<SortedEdit> srtEdits;

    for (HWND hEdit : tabCtrl.visibleEdits) {
        RECT rc{};
        GetWindowRect(hEdit, &rc);
        if ((rc.right - rc.left) > 25 && (rc.bottom - rc.top) > 12 &&
            rc.left > (dlgRect.left + 150) &&
            rc.top >= (tabCtrl.rcCombo.bottom - 5)) {
            srtEdits.push_back({ hEdit, rc });
        }
    }

    std::sort(srtEdits.begin(), srtEdits.end(), [](const SortedEdit& a, const SortedEdit& b) {
        if (std::abs(static_cast<int>(a.rc.top - b.rc.top)) > 10) {
            return a.rc.top < b.rc.top;
        }
        return a.rc.left < b.rc.left;
        });

    if (srtEdits.size() < 2) {
        if (uia) uia->Release();
        out_status = L"Status: Waiting for SRT (Caller) fields (Hostname / Port) to appear...";
        return false;
    }

    std::string port_str = std::to_string(srt_port);
    std::string lat_str = std::to_string(latency_ms);

    SetWinFormsEditText(srtEdits[0].hwnd, ip);
    SetWinFormsEditText(srtEdits[1].hwnd, port_str);
    if (srtEdits.size() >= 3) {
        SetWinFormsEditText(srtEdits[2].hwnd, lat_str);
    }

    g_lastAutoFilledDialog = wndCtx.hInputSelect;

    if (auto_click_ok && tabCtrl.hOkButton) {
        Sleep(100);
        SendMessageW(tabCtrl.hOkButton, BM_CLICK, 0, 0);
        out_status = L"Status: Mounted SRT Caller (" + ToWide(ip) + L":" + ToWide(port_str) + L") directly into vMix!";
    }
    else {
        out_status = L"Status: Auto-filled SRT Caller (" + ToWide(ip) + L":" + ToWide(port_str) +
            L", Latency " + ToWide(lat_str) + L"ms) into vMix dialog for review.";
    }

    if (uia) uia->Release();
    return true;
}

// ============================================================================
// 3. Background Threads: UDP 19800 Discovery + vMix 8099 Tally Sync
// ============================================================================
static void SendTallyToPhone(const std::string& ip, int http_port, const char* tally) {
    if (ip.empty()) return;
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return;

    DWORD timeout = 300;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<u_short>(http_port));
    inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);

    if (connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
        char body[64];
        snprintf(body, sizeof(body), "{\"tally\":\"%s\"}", tally);
        char req[256];
        snprintf(req, sizeof(req),
            "POST /api/tally HTTP/1.1\r\nHost: %s:%d\r\nContent-Type: application/json\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n%s",
            ip.c_str(), http_port, strlen(body), body);
        send(s, req, static_cast<int>(strlen(req)), 0);
    }
    closesocket(s);
}

static void StartBackgroundThreads() {
    std::thread([]() {
        SOCKET udp_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        BOOL reuse = TRUE;
        setsockopt(udp_sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
        DWORD timeout_ms = 500;
        setsockopt(udp_sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));

        sockaddr_in bind_addr{};
        bind_addr.sin_family = AF_INET;
        bind_addr.sin_port = htons(19800);
        bind_addr.sin_addr.s_addr = INADDR_ANY;
        bind(udp_sock, reinterpret_cast<sockaddr*>(&bind_addr), sizeof(bind_addr));

        ip_mreq mreq{};
        inet_pton(AF_INET, "239.255.19.80", &mreq.imr_multiaddr);
        mreq.imr_interface.s_addr = INADDR_ANY;
        setsockopt(udp_sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, reinterpret_cast<const char*>(&mreq), sizeof(mreq));

        char buf[512];
        while (g_running.load()) {
            sockaddr_in sender{};
            int sender_len = sizeof(sender);
            int n = recvfrom(udp_sock, buf, sizeof(buf) - 1, 0, reinterpret_cast<sockaddr*>(&sender), &sender_len);
            if (n > 0) {
                buf[n] = '\0';
                if (strstr(buf, "LUMEN_NODE_V1") != nullptr) {
                    char ip_str[INET_ADDRSTRLEN] = { 0 };
                    inet_ntop(AF_INET, &sender.sin_addr, ip_str, sizeof(ip_str));

                    DiscoveredNode node;
                    node.ip = ip_str;
                    node.srt_port = ExtractJsonInt(buf, "srt_port", 10000);
                    node.http_port = ExtractJsonInt(buf, "http_port", 8080);
                    node.latency_ms = ExtractJsonInt(buf, "latency_ms", 25);
                    if (node.latency_ms < 20) node.latency_ms = 25;

                    bool is_new = false;
                    {
                        std::lock_guard<std::mutex> lk(g_mutex);
                        bool found = false;
                        for (auto& existing : g_nodes) {
                            if (existing.ip == node.ip) {
                                existing = node;
                                found = true;
                                break;
                            }
                        }
                        if (!found) {
                            g_nodes.insert(g_nodes.begin(), node);
                            is_new = true;
                        }
                        if (g_active_ip.empty()) {
                            g_active_ip = node.ip;
                            g_active_srt_port = node.srt_port;
                            g_active_http_port = node.http_port;
                            g_active_latency_ms = node.latency_ms;
                            is_new = true;
                        }
                    }

                    if (is_new && g_hMainWnd) {
                        PostMessageW(g_hMainWnd, WM_NODE_DISCOVERED, 0, 0);
                    }
                }
            }
        }
        closesocket(udp_sock);
        }).detach();

        std::thread([]() {
            while (g_running.load()) {
                std::string ip;
                int http_port = 8080;
                {
                    std::lock_guard<std::mutex> lk(g_mutex);
                    ip = g_active_ip;
                    http_port = g_active_http_port;
                }
                if (ip.empty()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                    continue;
                }

                SOCKET tcp_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
                sockaddr_in vmix_addr{};
                vmix_addr.sin_family = AF_INET;
                vmix_addr.sin_port = htons(8099);
                inet_pton(AF_INET, "127.0.0.1", &vmix_addr.sin_addr);

                if (connect(tcp_sock, reinterpret_cast<sockaddr*>(&vmix_addr), sizeof(vmix_addr)) == 0) {
                    const char* sub = "SUBSCRIBE TALLY\r\n";
                    send(tcp_sock, sub, static_cast<int>(strlen(sub)), 0);
                    char buf[256];
                    while (g_running.load()) {
                        int n = recv(tcp_sock, buf, sizeof(buf) - 1, 0);
                        if (n <= 0) break;
                        buf[n] = '\0';
                        const char* p = strstr(buf, "TALLY OK ");
                        if (p) {
                            p += 9;
                            const char* tally_state = "off";
                            if (strchr(p, '1')) tally_state = "program";
                            else if (strchr(p, '2')) tally_state = "preview";

                            std::string cur_ip;
                            int cur_http = 8080;
                            {
                                std::lock_guard<std::mutex> lk(g_mutex);
                                cur_ip = g_active_ip;
                                cur_http = g_active_http_port;
                            }
                            SendTallyToPhone(cur_ip, cur_http, tally_state);
                        }
                    }
                }
                closesocket(tcp_sock);
                std::this_thread::sleep_for(std::chrono::seconds(2));
            }
            }).detach();
}

// ============================================================================
// 4. Responsive, DPI-Aware Win32 Studio GUI
// ============================================================================
static void RefreshDeviceComboAndFields() {
    std::vector<DiscoveredNode> nodes_copy;
    std::string active_ip;
    int srt_port = 10000, http_port = 8080, latency = 25;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        nodes_copy = g_nodes;
        active_ip = g_active_ip;
        srt_port = g_active_srt_port;
        http_port = g_active_http_port;
        latency = g_active_latency_ms;
    }

    SendMessageW(g_hComboDevices, CB_RESETCONTENT, 0, 0);
    if (nodes_copy.empty()) {
        SendMessageW(g_hComboDevices, CB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(L"[Scanning LAN for Lumen Node Cameras...]"));
        SendMessageW(g_hComboDevices, CB_SETCURSEL, 0, 0);
    }
    else {
        int sel_idx = 0;
        for (size_t i = 0; i < nodes_copy.size(); ++i) {
            wchar_t item_text[128];
            std::wstring wip = ToWide(nodes_copy[i].ip);
            swprintf(item_text, 128, L"Lumen Node (%s : %d)", wip.c_str(), nodes_copy[i].srt_port);
            SendMessageW(g_hComboDevices, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item_text));
            if (nodes_copy[i].ip == active_ip) sel_idx = static_cast<int>(i);
        }
        SendMessageW(g_hComboDevices, CB_SETCURSEL, sel_idx, 0);
    }

    if (!active_ip.empty()) {
        SetWindowTextA(g_hEditIp, active_ip.c_str());
        SetWindowTextA(g_hEditSrtPort, std::to_string(srt_port).c_str());
        SetWindowTextA(g_hEditHttpPort, std::to_string(http_port).c_str());
        SetWindowTextA(g_hEditLatency, std::to_string(latency).c_str());

        std::wstring status = L"Status: Ready — Discovered Lumen Node at " + ToWide(active_ip) +
            L":" + std::to_wstring(srt_port) + L" (Tally sync active).";
        SetWindowTextW(g_hLblStatus, status.c_str());
    }
}
static void DrawStudioCard(HDC hdc, const RECT& rc, COLORREF accentColor, bool hasHeaderDivider) {
    HBRUSH hCardBr = CreateSolidBrush(RGB(28, 31, 38));
    HPEN hBorderPen = CreatePen(PS_SOLID, 1, RGB(54, 61, 76));
    HGDIOBJ oldBr = SelectObject(hdc, hCardBr);
    HGDIOBJ oldPen = SelectObject(hdc, hBorderPen);

    int radius = ScaleDpi(10);
    RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, radius, radius);

    SelectObject(hdc, oldBr);
    SelectObject(hdc, oldPen);
    DeleteObject(hCardBr);
    DeleteObject(hBorderPen);

    // 绘制标题左侧高亮色条
    RECT rcAccent = {
        rc.left + ScaleDpi(14),
        rc.top + ScaleDpi(13),
        rc.left + ScaleDpi(18),
        rc.top + ScaleDpi(29)
    };
    HBRUSH hAccBr = CreateSolidBrush(accentColor);
    FillRect(hdc, &rcAccent, hAccBr);
    DeleteObject(hAccBr);

    // 绘制卡片标题分割线
    if (hasHeaderDivider) {
        HPEN hLinePen = CreatePen(PS_SOLID, 1, RGB(44, 50, 62));
        HGDIOBJ oldLinePen = SelectObject(hdc, hLinePen);
        MoveToEx(hdc, rc.left + ScaleDpi(14), rc.top + ScaleDpi(36), nullptr);
        LineTo(hdc, rc.right - ScaleDpi(14), rc.top + ScaleDpi(36));
        SelectObject(hdc, oldLinePen);
        DeleteObject(hLinePen);
    }
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        g_currentDpi = QueryWindowDpi(hwnd);
        g_hBrushBg = CreateSolidBrush(RGB(18, 20, 24));
        g_hBrushCard = CreateSolidBrush(RGB(28, 31, 38));
        g_hBrushEdit = CreateSolidBrush(RGB(20, 22, 28));

        auto CreateLabel = [&](const wchar_t* text) -> HWND {
            return CreateWindowW(L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_NOPREFIX,
                0, 0, 10, 10, hwnd, nullptr, nullptr, nullptr);
        };

        // 卡片 1: Lumen Nodes
        g_hGrpParams = CreateLabel(L"Lumen Nodes  —  Camera Discovery & Network");

        g_hLblDevice = CreateLabel(L"Discovered Camera:");
        g_hComboDevices = CreateWindowW(
            L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
            0, 0, 10, 10, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_COMBO_DEVICES)), nullptr, nullptr);

        g_hLblIp = CreateLabel(L"Target IP Address:");
        g_hEditIp = CreateWindowExW(
            0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP,
            0, 0, 10, 10, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_EDIT_IP)), nullptr, nullptr);

        g_hLblLatency = CreateLabel(L"SRT Latency (ms):");
        g_hEditLatency = CreateWindowExW(
            0, L"EDIT", L"80", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER | WS_TABSTOP,
            0, 0, 10, 10, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_EDIT_LATENCY)), nullptr, nullptr);

        g_hLblSrtPort = CreateLabel(L"SRT Stream Port:");
        g_hEditSrtPort = CreateWindowExW(
            0, L"EDIT", L"10000", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER | WS_TABSTOP,
            0, 0, 10, 10, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_EDIT_SRT_PORT)), nullptr, nullptr);

        g_hLblHttpPort = CreateLabel(L"Tally HTTP Port:");
        g_hEditHttpPort = CreateWindowExW(
            0, L"EDIT", L"8080", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER | WS_TABSTOP,
            0, 0, 10, 10, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_EDIT_HTTP_PORT)), nullptr, nullptr);

        // 卡片 2: vMix Integration (采用 BS_OWNERDRAW 自绘复选框与按钮，彻底解决黑字看不清问题)
        g_hGrpActions = CreateLabel(L"vMix  —  SRT Caller Auto-Mount & Tally Sync");

        g_hChkAutoFill = CreateWindowW(
            L"BUTTON", L"Auto-fill when Stream / SRT tab is opened",
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW | WS_TABSTOP, 0, 0, 10, 10, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_CHK_AUTO_FILL)), nullptr, nullptr);

        g_hBtnFillOnly = CreateWindowW(
            L"BUTTON", L"Fill Dialog",
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW | WS_TABSTOP, 0, 0, 10, 10, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BTN_FILL_ONLY)), nullptr, nullptr);

        g_hBtnFillAndOk = CreateWindowW(
            L"BUTTON", L"Add to vMix",
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW | WS_TABSTOP, 0, 0, 10, 10, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BTN_FILL_AND_OK)), nullptr, nullptr);

        g_hNoteFillOnly = CreateLabel(
            L"Opens vMix \"Stream / SRT\" tab and fills IP & port so you can review settings before clicking OK.");
        g_hNoteFillAndOk = CreateLabel(
            L"Fills IP & port and automatically clicks OK in vMix to mount the camera immediately.");

        // 卡片 3: OBS Studio Integration
        g_hGrpObs = CreateLabel(L"OBS Studio  —  Native Low-Latency Source Plugin");

        g_hBtnInstallObs = CreateWindowW(
            L"BUTTON", L"Install OBS Plugin",
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW | WS_TABSTOP, 0, 0, 10, 10, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BTN_INSTALL_OBS)), nullptr, nullptr);
        g_hLblObsStatus = CreateLabel(
            L"Installs 'Lumen Node Camera' source plugin into OBS Studio.\nRestart OBS after installing to auto-discover cameras.");

        // 底部状态栏
        g_hLblStatus = CreateLabel(L"Listening for Lumen Node on UDP 19800...");

        HICON hIcon = (HICON)LoadImageW(nullptr, L"lumen.ico", IMAGE_ICON, 0, 0, LR_LOADFROMFILE | LR_DEFAULTSIZE);
        if (hIcon) {
            SendMessageW(hwnd, WM_SETICON, ICON_BIG, (LPARAM)hIcon);
            SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)hIcon);
        }

        RebuildFontsForDpi();
        RefreshDeviceComboAndFields();
        SetTimer(hwnd, IDT_WATCH_VMIX, 500, nullptr);
        return 0;
    }

    case WM_SIZE: {
        int clientW = LOWORD(lParam);
        int clientH = HIWORD(lParam);
        LayoutControls(clientW, clientH);
        return 0;
    }

    case WM_GETMINMAXINFO: {
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
        mmi->ptMinTrackSize.x = ScaleDpi(580);
        mmi->ptMinTrackSize.y = ScaleDpi(600);
        return 0;
    }

    case WM_DPICHANGED: {
        g_currentDpi = HIWORD(wParam);
        RebuildFontsForDpi();
        auto* suggestedRect = reinterpret_cast<RECT*>(lParam);
        SetWindowPos(hwnd, nullptr,
            suggestedRect->left, suggestedRect->top,
            suggestedRect->right - suggestedRect->left,
            suggestedRect->bottom - suggestedRect->top,
            SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);

        // 绘制 3 个功能分区卡片框 + 1 个底部状态栏框
        DrawStudioCard(hdc, g_rcCardNodes, RGB(56, 189, 248), true);   // 青色色标：Lumen Nodes
        DrawStudioCard(hdc, g_rcCardVmix, RGB(59, 130, 246), true);    // 蓝色色标：vMix
        DrawStudioCard(hdc, g_rcCardObs, RGB(16, 185, 129), true);     // 翠绿色标：OBS Studio
        DrawStudioCard(hdc, g_rcCardStatus, RGB(45, 212, 191), false); // 状态栏框

        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_DRAWITEM: {
        auto* dis = reinterpret_cast<LPDRAWITEMSTRUCT>(lParam);
        if (!dis) return FALSE;

        HDC hdc = dis->hDC;
        RECT rc = dis->rcItem;
        SetBkMode(hdc, TRANSPARENT);

        // 1. 自绘高对比度复选框 (彻底消除 Windows 主题导致的黑字看不清问题)
        if (dis->CtlID == IDC_CHK_AUTO_FILL) {
            FillRect(hdc, &rc, g_hBrushCard);

            int boxSize = ScaleDpi(18);
            int boxTop = rc.top + ((rc.bottom - rc.top) - boxSize) / 2;
            RECT rcBox = { rc.left, boxTop, rc.left + boxSize, boxTop + boxSize };

            bool checked = g_auto_watch_vmix_dialog.load();
            COLORREF boxFill = checked ? RGB(37, 99, 235) : RGB(20, 22, 28);
            COLORREF boxBorder = checked ? RGB(96, 165, 250) : RGB(100, 116, 139);

            HBRUSH hBoxBr = CreateSolidBrush(boxFill);
            HPEN hBoxPen = CreatePen(PS_SOLID, 1, boxBorder);
            HGDIOBJ oldBr = SelectObject(hdc, hBoxBr);
            HGDIOBJ oldPen = SelectObject(hdc, hBoxPen);
            RoundRect(hdc, rcBox.left, rcBox.top, rcBox.right, rcBox.bottom, ScaleDpi(4), ScaleDpi(4));
            SelectObject(hdc, oldBr);
            SelectObject(hdc, oldPen);
            DeleteObject(hBoxBr);
            DeleteObject(hBoxPen);

            if (checked) {
                HPEN hCheckPen = CreatePen(PS_SOLID, std::max(2, ScaleDpi(2)), RGB(255, 255, 255));
                HGDIOBJ oldCkPen = SelectObject(hdc, hCheckPen);
                MoveToEx(hdc, rcBox.left + ScaleDpi(4), rcBox.top + ScaleDpi(9), nullptr);
                LineTo(hdc, rcBox.left + ScaleDpi(8), rcBox.bottom - ScaleDpi(4));
                LineTo(hdc, rcBox.right - ScaleDpi(3), rcBox.top + ScaleDpi(4));
                SelectObject(hdc, oldCkPen);
                DeleteObject(hCheckPen);
            }

            wchar_t text[128] = { 0 };
            GetWindowTextW(dis->hwndItem, text, 127);
            HGDIOBJ oldFont = SelectObject(hdc, g_hFontNormal);
            SetTextColor(hdc, RGB(240, 244, 250)); // 亮白色文字，清晰醒目
            RECT rcText = { rcBox.right + ScaleDpi(10), rc.top, rc.right, rc.bottom };
            DrawTextW(hdc, text, -1, &rcText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            SelectObject(hdc, oldFont);
            return TRUE;
        }

        // 2. 自绘现代化圆角工作室按钮 (Fill Dialog / Add to vMix / Install OBS Plugin)
        if (dis->CtlID == IDC_BTN_FILL_ONLY ||
            dis->CtlID == IDC_BTN_FILL_AND_OK ||
            dis->CtlID == IDC_BTN_INSTALL_OBS) {
            FillRect(hdc, &rc, g_hBrushCard);

            bool pressed = (dis->itemState & ODS_SELECTED) != 0;
            COLORREF bgColor = RGB(45, 52, 66);
            COLORREF borderColor = RGB(82, 94, 116);

            if (dis->CtlID == IDC_BTN_FILL_AND_OK) {
                bgColor = pressed ? RGB(29, 78, 216) : RGB(37, 99, 235);       // vMix 主按钮：亮蓝色
                borderColor = RGB(96, 165, 250);
            }
            else if (dis->CtlID == IDC_BTN_INSTALL_OBS) {
                bgColor = pressed ? RGB(5, 150, 105) : RGB(16, 185, 129);      // OBS 按钮：翠绿色
                borderColor = RGB(52, 211, 153);
            }
            else {
                bgColor = pressed ? RGB(36, 42, 54) : RGB(48, 56, 72);         // 次级按钮：深灰蓝
                borderColor = RGB(90, 102, 124);
            }

            HBRUSH hBtnBr = CreateSolidBrush(bgColor);
            HPEN hBtnPen = CreatePen(PS_SOLID, 1, borderColor);
            HGDIOBJ oldBr = SelectObject(hdc, hBtnBr);
            HGDIOBJ oldPen = SelectObject(hdc, hBtnPen);
            RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, ScaleDpi(8), ScaleDpi(8));
            SelectObject(hdc, oldBr);
            SelectObject(hdc, oldPen);
            DeleteObject(hBtnBr);
            DeleteObject(hBtnPen);

            wchar_t btnText[128] = { 0 };
            GetWindowTextW(dis->hwndItem, btnText, 127);
            HGDIOBJ oldFont = SelectObject(hdc, g_hFontGroup);
            SetTextColor(hdc, RGB(255, 255, 255));
            RECT rcText = rc;
            if (pressed) OffsetRect(&rcText, 0, 1);
            DrawTextW(hdc, btnText, -1, &rcText, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(hdc, oldFont);
            return TRUE;
        }
        return FALSE;
    }

    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        SetTextColor(hdc, RGB(242, 246, 252));
        SetBkColor(hdc, RGB(20, 22, 28));
        return reinterpret_cast<LRESULT>(g_hBrushEdit);
    }

    case WM_CTLCOLORSTATIC: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        HWND hCtrl = reinterpret_cast<HWND>(lParam);
        SetBkMode(hdc, TRANSPARENT);
        SetBkColor(hdc, RGB(28, 31, 38));

        if (hCtrl == g_hGrpParams || hCtrl == g_hGrpActions || hCtrl == g_hGrpObs) {
            SetTextColor(hdc, RGB(255, 255, 255));
        }
        else if (hCtrl == g_hLblStatus) {
            SetTextColor(hdc, RGB(94, 234, 212));
        }
        else if (hCtrl == g_hLblObsStatus) {
            SetTextColor(hdc, RGB(203, 213, 225));
        }
        else if (hCtrl == g_hNoteFillOnly || hCtrl == g_hNoteFillAndOk) {
            SetTextColor(hdc, RGB(165, 178, 198));
        }
        else {
            SetTextColor(hdc, RGB(230, 236, 245));
        }
        return reinterpret_cast<LRESULT>(g_hBrushCard);
    }

    case WM_CTLCOLORBTN: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        SetTextColor(hdc, RGB(255, 255, 255));
        SetBkMode(hdc, TRANSPARENT);
        return reinterpret_cast<LRESULT>(g_hBrushCard);
    }

    case WM_NODE_DISCOVERED: {
        g_lastAutoFilledDialog = nullptr;
        RefreshDeviceComboAndFields();
        return 0;
    }

    case WM_TIMER: {
        if (wParam == IDT_WATCH_VMIX && g_auto_watch_vmix_dialog.load()) {
            FindVmixWndCtx wndCtx;
            EnumWindows(EnumVmixWindowsProc, reinterpret_cast<LPARAM>(&wndCtx));
            if (!wndCtx.hInputSelect) {
                g_lastAutoFilledDialog = nullptr;
            }
            else if (wndCtx.hInputSelect != g_lastAutoFilledDialog) {
                std::wstring status;
                if (AutoFillVmixInputSelectWindow(false, false, status)) {
                    SetWindowTextW(g_hLblStatus, status.c_str());
                }
            }
        }
        return 0;
    }

    case WM_COMMAND: {
        int id = LOWORD(wParam);
        int code = HIWORD(wParam);

        if (id == IDC_COMBO_DEVICES && code == CBN_SELCHANGE) {
            int idx = static_cast<int>(SendMessageW(g_hComboDevices, CB_GETCURSEL, 0, 0));
            std::lock_guard<std::mutex> lk(g_mutex);
            if (idx >= 0 && idx < static_cast<int>(g_nodes.size())) {
                g_active_ip = g_nodes[idx].ip;
                g_active_srt_port = g_nodes[idx].srt_port;
                g_active_http_port = g_nodes[idx].http_port;
                g_active_latency_ms = g_nodes[idx].latency_ms;
            }
            g_lastAutoFilledDialog = nullptr;
            PostMessageW(hwnd, WM_NODE_DISCOVERED, 0, 0);
        }
        else if (id == IDC_CHK_AUTO_FILL && code == BN_CLICKED) {
            bool nextState = !g_auto_watch_vmix_dialog.load();
            g_auto_watch_vmix_dialog.store(nextState);
            InvalidateRect(g_hChkAutoFill, nullptr, FALSE);
        }
        else if (id == IDC_BTN_FILL_ONLY) {
            g_lastAutoFilledDialog = nullptr;
            std::wstring status;
            AutoFillVmixInputSelectWindow(true, false, status);
            SetWindowTextW(g_hLblStatus, status.c_str());
        }
        else if (id == IDC_BTN_FILL_AND_OK) {
            g_lastAutoFilledDialog = nullptr;
            std::wstring status;
            AutoFillVmixInputSelectWindow(true, true, status);
            SetWindowTextW(g_hLblStatus, status.c_str());
        }
        else if (id == IDC_BTN_INSTALL_OBS) {
            std::wstring err;
            if (InstallObsPlugin(err)) {
                SetWindowTextW(g_hLblObsStatus, L"Plugin installed successfully!\nPlease restart OBS Studio and add 'Lumen Node Camera'.");
                SetWindowTextW(g_hBtnInstallObs, L"OBS Plugin Installed");
                InvalidateRect(g_hBtnInstallObs, nullptr, FALSE);
                MessageBoxW(hwnd, L"OBS plugin installed successfully.\n\nPlease completely close and restart OBS Studio, then add the 'Lumen Node Camera' source.", L"Lumen Node Helper", MB_OK | MB_ICONINFORMATION);
            }
            else {
                SetWindowTextW(g_hLblObsStatus, err.c_str());
                MessageBoxW(hwnd, err.c_str(), L"Lumen Node Helper", MB_OK | MB_ICONWARNING);
            }
        }
        return 0;
    }

    case WM_DESTROY:
        g_running.store(false);
        if (g_hFontNormal) DeleteObject(g_hFontNormal);
        if (g_hFontGroup) DeleteObject(g_hFontGroup);
        if (g_hFontSmall) DeleteObject(g_hFontSmall);
        if (g_hBrushBg) DeleteObject(g_hBrushBg);
        if (g_hBrushCard) DeleteObject(g_hBrushCard);
        if (g_hBrushEdit) DeleteObject(g_hBrushEdit);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int main() {
    if (!IsRunAsAdmin()) {
        RelaunchAsAdmin();
        return 0;
    }
    EnablePerMonitorDpiAwareness();
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    HWND hConsole = GetConsoleWindow();
    if (hConsole) ShowWindow(hConsole, SW_HIDE);

    g_currentDpi = QueryWindowDpi(nullptr);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(RGB(18, 20, 24));
    wc.lpszClassName = L"LumenVmixBridgeGuiClass";
    RegisterClassExW(&wc);

    int initW = ScaleDpi(620);
    int initH = ScaleDpi(615);

    g_hMainWnd = CreateWindowExW(
        WS_EX_TOPMOST, L"LumenVmixBridgeGuiClass",
        L"Lumen Node Helper",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, initW, initH,
        nullptr, nullptr, wc.hInstance, nullptr);

    EnableDarkTitleBar(g_hMainWnd);

    StartBackgroundThreads();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (!IsDialogMessageW(g_hMainWnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    CoUninitialize();
    return 0;
}