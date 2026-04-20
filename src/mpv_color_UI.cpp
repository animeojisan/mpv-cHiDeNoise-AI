#define UNICODE
#define _UNICODE

#include <windows.h>
#include <commctrl.h>
#include <string>
#include <sstream>
#include <iomanip>

#pragma comment(lib, "comctl32.lib")

namespace {

constexpr int IDC_STATUS_STATIC     = 101;
constexpr int IDC_APPLY_BUTTON      = 102;
constexpr int IDC_RESET_BUTTON      = 103;
constexpr int IDC_BYPASS_CHECK      = 104;
constexpr int IDC_TIMER_APPLY       = 9001;
constexpr int IDC_TIMER_PIPE_MONITOR = 9002;

constexpr int BASE_LABEL_ID         = 200;
constexpr int BASE_VALUE_ID         = 300;
constexpr int BASE_SLIDER_ID        = 400;

constexpr wchar_t kWindowClassName[] = L"MpvColorUiWindow";
constexpr wchar_t kDefaultPipe[]      = L"\\\\.\\pipe\\mpv-colorui";
constexpr char kEqFilterLabel[]       = "@colorui_eq";
constexpr char kHueFilterLabel[]      = "@colorui_hue";
constexpr int kPipeMonitorIntervalMs = 1000;
constexpr int kPipeMonitorMissLimit  = 3;

struct SliderDef {
    const wchar_t* title;
    int minValue;
    int maxValue;
    int defaultValue;
    double scale;
    int tickFreq;
};

enum SliderIndex {
    IDX_BRIGHTNESS = 0,
    IDX_CONTRAST,
    IDX_SATURATION,
    IDX_HUE,
    IDX_GAMMA_MASTER,
    IDX_GAMMA_R,
    IDX_GAMMA_G,
    IDX_GAMMA_B,
    IDX_GAMMA_RGB_BLEND,
};

const SliderDef kSliders[] = {
    { L"Brightness",       -100,  100,    0, 100.0, 10 },
    { L"Contrast",            0,  300,  100, 100.0, 20 },
    { L"Saturation",          0,  300,  100, 100.0, 20 },
    { L"Hue",              -180,  180,    0,   1.0, 30 },
    { L"Gamma Master",       10,  300,  100, 100.0, 20 },
    { L"Gamma R",            10,  300,  100, 100.0, 20 },
    { L"Gamma G",            10,  300,  100, 100.0, 20 },
    { L"Gamma B",            10,  300,  100, 100.0, 20 },
    { L"Gamma RGB Blend",     0,  100,  100, 100.0, 10 },
};

constexpr int kSliderCount = static_cast<int>(sizeof(kSliders) / sizeof(kSliders[0]));

struct AppState {
    HWND hwnd = nullptr;
    HWND statusStatic = nullptr;
    HWND bypassCheck = nullptr;
    HWND sliders[kSliderCount]{};
    HWND values[kSliderCount]{};
    bool pendingApply = false;
    unsigned int requestId = 1;
    int dpi = 96;
    HFONT fontNormal = nullptr;
    HFONT fontSmall = nullptr;
    HFONT fontTitle = nullptr;
    int pipeMonitorMissCount = 0;
};

AppState g_app;

int Scale(int value)
{
    return MulDiv(value, g_app.dpi, 96);
}

void CreateFonts(HWND hwnd)
{
    HDC hdc = GetDC(hwnd);
    const int dpi = hdc ? GetDeviceCaps(hdc, LOGPIXELSY) : 96;
    if (hdc) {
        ReleaseDC(hwnd, hdc);
    }
    g_app.dpi = dpi > 0 ? dpi : 96;

    g_app.fontNormal = CreateFontW(
        -MulDiv(9, g_app.dpi, 72), 0, 0, 0,
        FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

    g_app.fontSmall = CreateFontW(
        -MulDiv(8, g_app.dpi, 72), 0, 0, 0,
        FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

    g_app.fontTitle = CreateFontW(
        -MulDiv(11, g_app.dpi, 72), 0, 0, 0,
        FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI Semibold");
}

std::wstring FormatDouble(double v)
{
    std::wostringstream oss;
    oss << std::fixed << std::setprecision(2) << v;
    return oss.str();
}

void SetStatus(const std::wstring& text)
{
    if (g_app.statusStatic) {
        SetWindowTextW(g_app.statusStatic, text.c_str());
    }
}

bool SendJsonLineToPipe(const std::wstring& pipeName, const std::string& jsonUtf8, std::wstring& errorText)
{
    WaitNamedPipeW(pipeName.c_str(), 200);

    HANDLE hPipe = CreateFileW(
        pipeName.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        0,
        nullptr,
        OPEN_EXISTING,
        0,
        nullptr);

    if (hPipe == INVALID_HANDLE_VALUE) {
        const DWORD err = GetLastError();
        std::wostringstream oss;
        oss << L"mpv の IPC pipe に接続できません (Win32=" << err << L")";
        errorText = oss.str();
        return false;
    }

    DWORD mode = PIPE_READMODE_BYTE;
    SetNamedPipeHandleState(hPipe, &mode, nullptr, nullptr);

    DWORD written = 0;
    const BOOL ok = WriteFile(hPipe, jsonUtf8.data(), static_cast<DWORD>(jsonUtf8.size()), &written, nullptr);
    FlushFileBuffers(hPipe);
    CloseHandle(hPipe);

    if (!ok || written != jsonUtf8.size()) {
        const DWORD err = GetLastError();
        std::wostringstream oss;
        oss << L"mpv への送信に失敗しました (Win32=" << err << L")";
        errorText = oss.str();
        return false;
    }

    return true;
}

bool IsPipeReachable(const wchar_t* pipeName)
{
    HANDLE hPipe = CreateFileW(
        pipeName,
        GENERIC_READ | GENERIC_WRITE,
        0,
        nullptr,
        OPEN_EXISTING,
        0,
        nullptr);

    if (hPipe != INVALID_HANDLE_VALUE) {
        CloseHandle(hPipe);
        return true;
    }

    const DWORD err = GetLastError();
    if (err == ERROR_PIPE_BUSY) {
        return WaitNamedPipeW(pipeName, 0) != FALSE;
    }

    return false;
}

int GetSliderValue(int index)
{
    return static_cast<int>(SendMessageW(g_app.sliders[index], TBM_GETPOS, 0, 0));
}

double GetSliderFloatValue(int index)
{
    return static_cast<double>(GetSliderValue(index)) / kSliders[index].scale;
}

void UpdateValueTexts()
{
    for (int i = 0; i < kSliderCount; ++i) {
        SetWindowTextW(g_app.values[i], FormatDouble(GetSliderFloatValue(i)).c_str());
    }
}

bool IsSliderDefault(int index)
{
    return GetSliderValue(index) == kSliders[index].defaultValue;
}

bool IsEqDefaultState()
{
    for (int i = IDX_BRIGHTNESS; i <= IDX_GAMMA_RGB_BLEND; ++i) {
        if (i == IDX_HUE) {
            continue;
        }
        if (!IsSliderDefault(i)) {
            return false;
        }
    }
    return true;
}

bool IsHueDefaultState()
{
    return IsSliderDefault(IDX_HUE);
}

bool IsDefaultState()
{
    return IsEqDefaultState() && IsHueDefaultState();
}

std::string BuildRemoveCommandJson(const char* filterLabel, unsigned int requestId)
{
    std::ostringstream oss;
    oss << "{\"command\":[\"vf\",\"remove\",\"" << filterLabel << "\"],\"request_id\":" << requestId << "}\n";
    return oss.str();
}

std::string BuildEqFilterCommandJson(unsigned int requestId)
{
    const double brightness    = GetSliderFloatValue(IDX_BRIGHTNESS);
    const double contrast      = GetSliderFloatValue(IDX_CONTRAST);
    const double saturation    = GetSliderFloatValue(IDX_SATURATION);
    const double gammaMaster   = GetSliderFloatValue(IDX_GAMMA_MASTER);
    const double gammaR        = GetSliderFloatValue(IDX_GAMMA_R);
    const double gammaG        = GetSliderFloatValue(IDX_GAMMA_G);
    const double gammaB        = GetSliderFloatValue(IDX_GAMMA_B);
    const double gammaRgbBlend = GetSliderFloatValue(IDX_GAMMA_RGB_BLEND);

    std::ostringstream f;
    f << std::fixed << std::setprecision(2);
    f << kEqFilterLabel << ":lavfi=[eq="
      << "brightness="    << brightness
      << ":contrast="     << contrast
      << ":saturation="   << saturation
      << ":gamma="        << gammaMaster
      << ":gamma_r="      << gammaR
      << ":gamma_g="      << gammaG
      << ":gamma_b="      << gammaB
      << ":gamma_weight=" << gammaRgbBlend
      << "]";

    std::ostringstream oss;
    oss << "{\"command\":[\"vf\",\"add\",\"" << f.str() << "\"],\"request_id\":" << requestId << "}\n";
    return oss.str();
}

std::string BuildHueFilterCommandJson(unsigned int requestId)
{
    const double hue = GetSliderFloatValue(IDX_HUE);

    std::ostringstream f;
    f << std::fixed << std::setprecision(2);
    f << kHueFilterLabel << ":lavfi=[hue=h=" << hue << "]";

    std::ostringstream oss;
    oss << "{\"command\":[\"vf\",\"add\",\"" << f.str() << "\"],\"request_id\":" << requestId << "}\n";
    return oss.str();
}

bool SendCommand(const std::string& json, std::wstring& errorText)
{
    return SendJsonLineToPipe(kDefaultPipe, json, errorText);
}

bool ApplyCurrentValues()
{
    UpdateValueTexts();

    const bool bypass = (static_cast<LRESULT>(SendMessageW(g_app.bypassCheck, BM_GETCHECK, 0, 0)) == BST_CHECKED);
    std::wstring errorText;

    if (bypass || IsDefaultState()) {
        if (!SendCommand(BuildRemoveCommandJson(kEqFilterLabel, g_app.requestId++), errorText)) {
            SetStatus(errorText);
            return false;
        }
        if (!SendCommand(BuildRemoveCommandJson(kHueFilterLabel, g_app.requestId++), errorText)) {
            SetStatus(errorText);
            return false;
        }

        if (bypass) {
            SetStatus(L"Bypass: 色調補正を一時的に外しました。");
        } else {
            SetStatus(L"デフォルト値なので補正フィルタを外しました。");
        }
        g_app.pipeMonitorMissCount = 0;
        return true;
    }

    if (IsEqDefaultState()) {
        if (!SendCommand(BuildRemoveCommandJson(kEqFilterLabel, g_app.requestId++), errorText)) {
            SetStatus(errorText);
            return false;
        }
    } else {
        if (!SendCommand(BuildEqFilterCommandJson(g_app.requestId++), errorText)) {
            SetStatus(errorText);
            return false;
        }
    }

    if (IsHueDefaultState()) {
        if (!SendCommand(BuildRemoveCommandJson(kHueFilterLabel, g_app.requestId++), errorText)) {
            SetStatus(errorText);
            return false;
        }
    } else {
        if (!SendCommand(BuildHueFilterCommandJson(g_app.requestId++), errorText)) {
            SetStatus(errorText);
            return false;
        }
    }

    SetStatus(L"mpv に色調補正を反映しました。");
    g_app.pipeMonitorMissCount = 0;
    return true;
}

void ScheduleApply(HWND hwnd)
{
    g_app.pendingApply = true;
    KillTimer(hwnd, IDC_TIMER_APPLY);
    SetTimer(hwnd, IDC_TIMER_APPLY, 35, nullptr);
}

void ResetSliders()
{
    for (int i = 0; i < kSliderCount; ++i) {
        SendMessageW(g_app.sliders[i], TBM_SETPOS, TRUE, kSliders[i].defaultValue);
    }
    SendMessageW(g_app.bypassCheck, BM_SETCHECK, BST_UNCHECKED, 0);
    UpdateValueTexts();
}

HWND CreateStatic(HWND parent, int x, int y, int w, int h, const wchar_t* text, int id, HFONT font, DWORD style = WS_CHILD | WS_VISIBLE)
{
    HWND hwnd = CreateWindowExW(0, L"STATIC", text, style, x, y, w, h, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    return hwnd;
}

HWND CreateButtonCtrl(HWND parent, int x, int y, int w, int h, const wchar_t* text, int id, HFONT font, DWORD extraStyle = 0)
{
    HWND hwnd = CreateWindowExW(0, L"BUTTON", text,
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | extraStyle,
        x, y, w, h, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    return hwnd;
}

HWND CreateCheckbox(HWND parent, int x, int y, int w, int h, const wchar_t* text, int id, HFONT font)
{
    HWND hwnd = CreateWindowExW(0, L"BUTTON", text,
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        x, y, w, h, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    return hwnd;
}

HWND CreateSlider(HWND parent, int x, int y, int w, int h, int id, const SliderDef& def)
{
    HWND slider = CreateWindowExW(0, TRACKBAR_CLASSW, L"",
        WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS | TBS_HORZ | TBS_TOOLTIPS,
        x, y, w, h, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);

    SendMessageW(slider, TBM_SETRANGE, TRUE, MAKELPARAM(def.minValue, def.maxValue));
    SendMessageW(slider, TBM_SETTICFREQ, def.tickFreq, 0);
    SendMessageW(slider, TBM_SETPAGESIZE, 0, 10);
    SendMessageW(slider, TBM_SETPOS, TRUE, def.defaultValue);
    return slider;
}

void CreateUi(HWND hwnd)
{
    g_app.hwnd = hwnd;
    CreateFonts(hwnd);

    const int margin = Scale(12);
    const int titleY = Scale(10);
    const int headerH = Scale(20);
    const int subY = Scale(30);
    const int controlsY = Scale(50);
    const int statusY = Scale(84);
    const int top = Scale(114);
    const int rowH = Scale(34);
    const int labelX = margin;
    const int labelW = Scale(110);
    const int sliderX = Scale(126);
    const int sliderW = Scale(270);
    const int valueX = Scale(408);
    const int valueW = Scale(72);

    CreateStatic(hwnd, margin, titleY, Scale(260), headerH, L"mpv Color UI", 0, g_app.fontTitle);

    CreateButtonCtrl(hwnd, Scale(286), controlsY, Scale(86), Scale(28), L"Apply", IDC_APPLY_BUTTON, g_app.fontNormal);
    CreateButtonCtrl(hwnd, Scale(378), controlsY, Scale(86), Scale(28), L"Reset", IDC_RESET_BUTTON, g_app.fontNormal);
    g_app.bypassCheck = CreateCheckbox(hwnd, margin, controlsY + Scale(4), Scale(150), Scale(22), L"Bypass (一時無効)", IDC_BYPASS_CHECK, g_app.fontNormal);

    g_app.statusStatic = CreateStatic(hwnd, margin, statusY, Scale(454), Scale(24),
        L"Ready", IDC_STATUS_STATIC, g_app.fontSmall);

    for (int i = 0; i < kSliderCount; ++i) {
        const int y = top + i * rowH;
        CreateStatic(hwnd, labelX, y + Scale(4), labelW, Scale(18), kSliders[i].title, BASE_LABEL_ID + i, g_app.fontNormal);
        g_app.sliders[i] = CreateSlider(hwnd, sliderX, y, sliderW, Scale(28), BASE_SLIDER_ID + i, kSliders[i]);
        g_app.values[i] = CreateStatic(hwnd, valueX, y + Scale(4), valueW, Scale(18), L"", BASE_VALUE_ID + i, g_app.fontNormal, WS_CHILD | WS_VISIBLE | SS_RIGHT);
    }


    UpdateValueTexts();
    SetTimer(hwnd, IDC_TIMER_PIPE_MONITOR, kPipeMonitorIntervalMs, nullptr);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE:
        CreateUi(hwnd);
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_APPLY_BUTTON:
            ApplyCurrentValues();
            return 0;
        case IDC_RESET_BUTTON:
            ResetSliders();
            ApplyCurrentValues();
            return 0;
        case IDC_BYPASS_CHECK:
            ApplyCurrentValues();
            return 0;
        default:
            break;
        }
        break;

    case WM_HSCROLL:
        for (int i = 0; i < kSliderCount; ++i) {
            if (reinterpret_cast<HWND>(lParam) == g_app.sliders[i]) {
                UpdateValueTexts();
                if (static_cast<LRESULT>(SendMessageW(g_app.bypassCheck, BM_GETCHECK, 0, 0)) == BST_CHECKED) {
                    SendMessageW(g_app.bypassCheck, BM_SETCHECK, BST_UNCHECKED, 0);
                }
                ScheduleApply(hwnd);
                return 0;
            }
        }
        break;

    case WM_TIMER:
        if (wParam == IDC_TIMER_APPLY) {
            KillTimer(hwnd, IDC_TIMER_APPLY);
            if (g_app.pendingApply) {
                g_app.pendingApply = false;
                ApplyCurrentValues();
            }
            return 0;
        }
        if (wParam == IDC_TIMER_PIPE_MONITOR) {
            if (IsPipeReachable(kDefaultPipe)) {
                g_app.pipeMonitorMissCount = 0;
            } else {
                ++g_app.pipeMonitorMissCount;
                if (g_app.pipeMonitorMissCount >= kPipeMonitorMissLimit) {
                    DestroyWindow(hwnd);
                    return 0;
                }
            }
            return 0;
        }
        break;

    case WM_DESTROY:
        KillTimer(hwnd, IDC_TIMER_APPLY);
        KillTimer(hwnd, IDC_TIMER_PIPE_MONITOR);
        if (g_app.fontNormal) DeleteObject(g_app.fontNormal);
        if (g_app.fontSmall) DeleteObject(g_app.fontSmall);
        if (g_app.fontTitle) DeleteObject(g_app.fontTitle);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow)
{
    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_BAR_CLASSES;
    InitCommonControlsEx(&icc);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = kWindowClassName;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.style = CS_HREDRAW | CS_VREDRAW;

    if (!RegisterClassExW(&wc)) {
        MessageBoxW(nullptr, L"ウィンドウクラスの登録に失敗しました。", L"mpv_color_ui", MB_ICONERROR | MB_OK);
        return 1;
    }

    HWND hwnd = CreateWindowExW(
        0,
        kWindowClassName,
        L"mpv Color UI",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT,
        532, 485,
        nullptr,
        nullptr,
        hInstance,
        nullptr);

    if (!hwnd) {
        MessageBoxW(nullptr, L"ウィンドウの作成に失敗しました。", L"mpv_color_ui", MB_ICONERROR | MB_OK);
        return 1;
    }

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}
