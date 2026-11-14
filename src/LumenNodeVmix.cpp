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
    g_hFontGroup = CreateFontW(-ScaleDpi(14), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    g_hFontSmall = CreateFontW(-ScaleDpi(11), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

    HWND normalCtrls[] = {
        g_hLblDevice, g_hComboDevices, g_hLblIp, g_hEditIp,
        g_hLblSrtPort, g_hEditSrtPort, g_hLblHttpPort, g_hEditHttpPort,
        g_hLblLatency, g_hEditLatency, g_hChkAutoFill, g_hLblStatus,
        g_hBtnInstallObs, g_hLblObsStatus
    };
    for (HWND h : normalCtrls) {
        if (h) SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(g_hFontNormal), TRUE);
    }

    HWND boldCtrls[] = { g_hBtnFillOnly, g_hBtnFillAndOk };
    for (HWND h : boldCtrls) {
        if (h) SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(g_hFontNormal), TRUE);
    }
    if (g_hGrpParams) SendMessageW(g_hGrpParams, WM_SETFONT, reinterpret_cast<WPARAM>(g_hFontGroup), TRUE);
    if (g_hGrpActions) SendMessageW(g_hGrpActions, WM_SETFONT, reinterpret_cast<WPARAM>(g_hFontGroup), TRUE);

    HWND smallCtrls[] = { g_hNoteFillOnly, g_hNoteFillAndOk };
    for (HWND h : smallCtrls) {
        if (h) SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(g_hFontSmall), TRUE);
    }
}

static void LayoutControls(int clientW, int clientH) {
    (void)clientH;
    if (!g_hGrpParams) return;

    int margin = ScaleDpi(16);
    int innerPad = ScaleDpi(16);
    int groupW = std::max(ScaleDpi(360), clientW - margin * 2);

    // Group 1: Camera Discovery & Network Parameters (2-column compact grid for ports)
    int grp1Top = ScaleDpi(12);
    int grp1H = ScaleDpi(168);
    // Titles as labels
    MoveWindow(g_hGrpParams, margin, grp1Top, groupW, ScaleDpi(20), TRUE);

    int labelW = ScaleDpi(165);
    int leftX = margin + innerPad;
    int inputX = leftX + labelW + ScaleDpi(8);
    int inputW = std::max(ScaleDpi(140), (margin + groupW - innerPad) - inputX);
    int rowH = ScaleDpi(24);

    int y1 = grp1Top + ScaleDpi(28);
    MoveWindow(g_hLblDevice, leftX, y1 + ScaleDpi(3), labelW, rowH, TRUE);
    MoveWindow(g_hComboDevices, inputX, y1, inputW, ScaleDpi(200), TRUE);

    // Row 2: Target IP Address + Latency (two columns)
    int y2 = y1 + ScaleDpi(36);
    int colGap = ScaleDpi(16);
    int halfW = (groupW - innerPad * 2 - colGap) / 2;
    int col1LblW = ScaleDpi(115);
    int col1EditX = leftX + col1LblW;
    int col1EditW = std::max(ScaleDpi(80), halfW - col1LblW);

    int col2LeftX = leftX + halfW + colGap;
    int col2LblW = ScaleDpi(115);
    int col2EditX = col2LeftX + col2LblW;
    int col2EditW = std::max(ScaleDpi(60), (margin + groupW - innerPad) - col2EditX);

    MoveWindow(g_hLblIp, leftX, y2 + ScaleDpi(3), col1LblW, rowH, TRUE);
    MoveWindow(g_hEditIp, col1EditX, y2, col1EditW, rowH, TRUE);

    MoveWindow(g_hLblLatency, col2LeftX, y2 + ScaleDpi(3), col2LblW, rowH, TRUE);
    MoveWindow(g_hEditLatency, col2EditX, y2, col2EditW, rowH, TRUE);

    // Row 3: SRT Port + Tally HTTP Port (two columns)
    int y3 = y2 + ScaleDpi(36);
    MoveWindow(g_hLblSrtPort, leftX, y3 + ScaleDpi(3), col1LblW, rowH, TRUE);
    MoveWindow(g_hEditSrtPort, col1EditX, y3, col1EditW, rowH, TRUE);

    MoveWindow(g_hLblHttpPort, col2LeftX, y3 + ScaleDpi(3), col2LblW, rowH, TRUE);
    MoveWindow(g_hEditHttpPort, col2EditX, y3, col2EditW, rowH, TRUE);

    // === Section 2: vMix Actions ===
    int grp2Top = grp1Top + grp1H + ScaleDpi(16);
    MoveWindow(g_hGrpActions, margin, grp2Top, groupW, ScaleDpi(22), TRUE);

    int chkY = grp2Top + ScaleDpi(30);
    MoveWindow(g_hChkAutoFill, leftX, chkY, groupW - innerPad * 2, ScaleDpi(22), TRUE);

    int btnY = chkY + ScaleDpi(32);
    int btnH = ScaleDpi(36);
    int btnGap = ScaleDpi(16);
    int btnW = (groupW - innerPad * 2 - btnGap) / 2;
    int btn2X = leftX + btnW + btnGap;

    MoveWindow(g_hBtnFillOnly, leftX, btnY, btnW, btnH, TRUE);
    MoveWindow(g_hBtnFillAndOk, btn2X, btnY, btnW, btnH, TRUE);

    int noteY = btnY + btnH + ScaleDpi(8);
    int noteH = ScaleDpi(36);
    MoveWindow(g_hNoteFillOnly, leftX + ScaleDpi(2), noteY, btnW - ScaleDpi(4), noteH, TRUE);
    MoveWindow(g_hNoteFillAndOk, btn2X + ScaleDpi(2), noteY, btnW - ScaleDpi(4), noteH, TRUE);

    // === Section 3: OBS Plugin ===
    int obsTop = noteY + noteH + ScaleDpi(20);
    MoveWindow(g_hLblStatus, margin + ScaleDpi(4), obsTop, groupW - ScaleDpi(8), ScaleDpi(24), TRUE);

    int obsBtnY = obsTop + ScaleDpi(32);
    MoveWindow(g_hBtnInstallObs, margin, obsBtnY, ScaleDpi(260), ScaleDpi(38), TRUE);
    MoveWindow(g_hLblObsStatus, margin + ScaleDpi(4), obsBtnY + ScaleDpi(46), groupW - ScaleDpi(8), ScaleDpi(50), TRUE);

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

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        g_currentDpi = QueryWindowDpi(hwnd);
        g_hBrushBg = CreateSolidBrush(RGB(32, 32, 34));

        auto CreateLabel = [&](const wchar_t* text) -> HWND {
            return CreateWindowW(L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_NOPREFIX,
                0, 0, 10, 10, hwnd, nullptr, nullptr, nullptr);
        };

        // Section 1: Camera Discovery & Network Parameters
        g_hGrpParams = CreateLabel(L"Lumen Nodes");
        SendMessageW(g_hGrpParams, WM_SETFONT, reinterpret_cast<WPARAM>(g_hFontGroup), TRUE);

        g_hLblDevice = CreateLabel(L"Discovered Camera:");
        g_hComboDevices = CreateWindowW(
            L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
            0, 0, 10, 10, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_COMBO_DEVICES)), nullptr, nullptr);

        g_hLblIp = CreateLabel(L"Target IP Address:");
        g_hEditIp = CreateWindowExW(
            WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | WS_TABSTOP,
            0, 0, 10, 10, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_EDIT_IP)), nullptr, nullptr);

        g_hLblLatency = CreateLabel(L"SRT Latency (ms):");
        g_hEditLatency = CreateWindowExW(
            WS_EX_CLIENTEDGE, L"EDIT", L"25", WS_CHILD | WS_VISIBLE | ES_NUMBER | WS_TABSTOP,
            0, 0, 10, 10, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_EDIT_LATENCY)), nullptr, nullptr);

        g_hLblSrtPort = CreateLabel(L"SRT Stream Port:");
        g_hEditSrtPort = CreateWindowExW(
            WS_EX_CLIENTEDGE, L"EDIT", L"10000", WS_CHILD | WS_VISIBLE | ES_NUMBER | WS_TABSTOP,
            0, 0, 10, 10, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_EDIT_SRT_PORT)), nullptr, nullptr);

        g_hLblHttpPort = CreateLabel(L"Tally HTTP Port:");
        g_hEditHttpPort = CreateWindowExW(
            WS_EX_CLIENTEDGE, L"EDIT", L"8080", WS_CHILD | WS_VISIBLE | ES_NUMBER | WS_TABSTOP,
            0, 0, 10, 10, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_EDIT_HTTP_PORT)), nullptr, nullptr);

        // Section 2: vMix Integration Actions
        g_hGrpActions = CreateLabel(L"vMix");
        SendMessageW(g_hGrpActions, WM_SETFONT, reinterpret_cast<WPARAM>(g_hFontGroup), TRUE);

        g_hChkAutoFill = CreateWindowW(
            L"BUTTON", L"Auto-fill when Stream / SRT tab is opened",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP, 0, 0, 10, 10, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_CHK_AUTO_FILL)), nullptr, nullptr);
        SendMessageW(g_hChkAutoFill, BM_SETCHECK, BST_CHECKED, 0);

        g_hBtnFillOnly = CreateWindowW(
            L"BUTTON", L"Fill Dialog",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP, 0, 0, 10, 10, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BTN_FILL_ONLY)), nullptr, nullptr);

        g_hBtnFillAndOk = CreateWindowW(
            L"BUTTON", L"Add to vMix",
            WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON | WS_TABSTOP, 0, 0, 10, 10, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BTN_FILL_AND_OK)), nullptr, nullptr);

        // OBS section
        g_hBtnInstallObs = CreateWindowW(
            L"BUTTON", L"Install OBS Plugin",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP, 0, 0, 10, 10, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BTN_INSTALL_OBS)), nullptr, nullptr);
        g_hLblObsStatus = CreateLabel(L"OBS Studio\nClick the button to install the Lumen Node Camera source.");

        g_hNoteFillOnly = CreateLabel(
            L"Opens vMix \"Stream / SRT\" tab and fills IP & port so you can review settings before clicking OK.");
        g_hNoteFillAndOk = CreateLabel(
            L"Fills IP & port and automatically clicks OK in vMix to mount the camera immediately.");

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
        mmi->ptMinTrackSize.x = ScaleDpi(540);
        mmi->ptMinTrackSize.y = ScaleDpi(415);
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

    case WM_CTLCOLORSTATIC: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        HWND hCtrl = reinterpret_cast<HWND>(lParam);
        SetBkMode(hdc, TRANSPARENT);
        if (hCtrl == g_hLblStatus || hCtrl == g_hLblObsStatus) {
            SetTextColor(hdc, RGB(120, 230, 210));
        }
        else if (hCtrl == g_hNoteFillOnly || hCtrl == g_hNoteFillAndOk) {
            SetTextColor(hdc, RGB(200, 210, 220));
        }
        else {
            SetTextColor(hdc, RGB(245, 245, 245));
        }
        return reinterpret_cast<LRESULT>(g_hBrushBg);
    }

    case WM_CTLCOLORBTN: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        SetTextColor(hdc, RGB(255, 255, 255));
        SetBkMode(hdc, TRANSPARENT);
        return reinterpret_cast<LRESULT>(g_hBrushBg);
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
        else if (id == IDC_CHK_AUTO_FILL) {
            LRESULT chk = SendMessageW(g_hChkAutoFill, BM_GETCHECK, 0, 0);
            g_auto_watch_vmix_dialog.store(chk == BST_CHECKED);
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
                SetWindowTextW(g_hLblObsStatus, L"OBS: Plugin installed successfully.\nPlease restart OBS Studio, then add the 'Lumen Node Camera' source.");
                SetWindowTextW(g_hBtnInstallObs, L"OBS Plugin Installed");
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
    wc.hbrBackground = CreateSolidBrush(RGB(32, 32, 34));
    wc.lpszClassName = L"LumenVmixBridgeGuiClass";
    RegisterClassExW(&wc);

    int initW = ScaleDpi(580);
    int initH = ScaleDpi(640);

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