#define UNICODE
#define _UNICODE

#include <windows.h>
#include <commctrl.h>
#include <string>
#include <sstream>
#include <iomanip>
#include <vector>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cctype>

#pragma comment(lib, "comctl32.lib")

namespace {

constexpr int IDC_STATUS_STATIC      = 101;
constexpr int IDC_APPLY_BUTTON       = 102;
constexpr int IDC_RESET_BUTTON       = 103;
constexpr int IDC_BYPASS_CHECK       = 104;
constexpr int IDC_CUSTOM1_BUTTON     = 105;
constexpr int IDC_CUSTOM1_SAVE       = 106;
constexpr int IDC_CUSTOM2_BUTTON     = 107;
constexpr int IDC_CUSTOM2_SAVE       = 108;
constexpr int IDC_TIMER_APPLY        = 9001;
constexpr int IDC_TIMER_PIPE_MONITOR = 9002;

constexpr int BASE_LABEL_ID          = 200;
constexpr int BASE_VALUE_ID          = 300;
constexpr int BASE_SLIDER_ID         = 400;

constexpr wchar_t kWindowClassName[] = L"MpvColorUiWindow";
constexpr wchar_t kSingleInstanceMutexName[] = L"Local\\MpvColorUiWindow_SingleInstance";
constexpr wchar_t kDefaultPipe[]     = L"\\\\.\\pipe\\mpv-tool";
constexpr char kEqFilterLabel[]      = "@colorui_eq";
constexpr char kHueFilterLabel[]     = "@colorui_hue";
constexpr int kPipeMonitorIntervalMs = 1000;
constexpr int kPipeMonitorMissLimit  = 3;
constexpr UINT WMAPP_RESET_UI   = WM_APP + 100;

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
    HANDLE singleInstanceMutex = nullptr;
    HWND statusStatic = nullptr;
    HWND bypassCheck = nullptr;
    HWND sliders[kSliderCount]{};
    HWND values[kSliderCount]{};
    HWND customButtons[2]{};
    HWND saveButtons[2]{};
    bool pendingApply = false;
    unsigned int requestId = 1;
    int dpi = 96;
    HFONT fontNormal = nullptr;
    HFONT fontSmall = nullptr;
    HFONT fontTitle = nullptr;
    int pipeMonitorMissCount = 0;
    bool eqFilterApplied = false;
    bool hueFilterApplied = false;

    bool customSaved[2]{};
    bool customSaveLatched[2]{};
    bool customBypass[2]{};
    int customSliderValues[2][kSliderCount]{};
    int activeCustomSlot = 0;

    std::wstring configDir;
    std::wstring configFilePath;
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

std::wstring GetModuleDir()
{
    wchar_t path[MAX_PATH]{};
    const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring s(path, path + n);
    const size_t pos = s.find_last_of(L"\\/");
    if (pos != std::wstring::npos) {
        s.resize(pos);
    }
    return s;
}

std::wstring GetCurrentDirWString()
{
    const DWORD n = GetCurrentDirectoryW(0, nullptr);
    std::wstring s;
    s.resize(n ? (n - 1) : 0);
    if (n > 1) {
        GetCurrentDirectoryW(n, s.data());
    }
    return s;
}

bool DirExists(const std::wstring& path)
{
    const DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
}

bool EnsureDirRecursive(const std::wstring& path)
{
    if (path.empty()) {
        return false;
    }
    if (DirExists(path)) {
        return true;
    }

    std::wstring current;
    size_t i = 0;
    if (path.size() >= 2 && path[1] == L':') {
        current = path.substr(0, 2);
        i = 2;
    } else if (path.rfind(L"\\\\", 0) == 0) {
        current = L"\\\\";
        i = 2;
    }

    while (i < path.size()) {
        while (i < path.size() && (path[i] == L'\\' || path[i] == L'/')) {
            current.push_back(L'\\');
            ++i;
        }
        size_t j = i;
        while (j < path.size() && path[j] != L'\\' && path[j] != L'/') {
            ++j;
        }
        if (j > i) {
            current.append(path, i, j - i);
            const DWORD attr = GetFileAttributesW(current.c_str());
            if (attr == INVALID_FILE_ATTRIBUTES) {
                if (!CreateDirectoryW(current.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
                    return false;
                }
            } else if (!(attr & FILE_ATTRIBUTE_DIRECTORY)) {
                return false;
            }
        }
        i = j;
    }

    return DirExists(path);
}

void ResolveConfigPaths()
{
    if (!g_app.configFilePath.empty()) {
        return;
    }

    std::vector<std::wstring> bases;
    const std::wstring cwd = GetCurrentDirWString();
    const std::wstring mod = GetModuleDir();
    if (!cwd.empty()) {
        bases.push_back(cwd);
    }
    if (!mod.empty() && mod != cwd) {
        bases.push_back(mod);
    }

    std::wstring portableBase;
    for (const auto& base : bases) {
        std::wstring candidate = base + L"\\portable_config";
        if (DirExists(candidate)) {
            portableBase = candidate;
            break;
        }
    }
    if (portableBase.empty()) {
        portableBase = (cwd.empty() ? mod : cwd) + L"\\portable_config";
    }

    g_app.configDir = portableBase + L"\\cache\\Color";
    g_app.configFilePath = g_app.configDir + L"\\color_state.json";
    EnsureDirRecursive(g_app.configDir);
}

bool ParseBoolField(const std::string& src, const char* key, bool& out)
{
    const std::string pat = std::string("\"") + key + "\"";
    size_t p = src.find(pat);
    if (p == std::string::npos) {
        return false;
    }
    p = src.find(':', p);
    if (p == std::string::npos) {
        return false;
    }
    ++p;
    while (p < src.size() && std::isspace(static_cast<unsigned char>(src[p]))) {
        ++p;
    }
    if (src.compare(p, 4, "true") == 0) {
        out = true;
        return true;
    }
    if (src.compare(p, 5, "false") == 0) {
        out = false;
        return true;
    }
    return false;
}

bool ParseIntField(const std::string& src, const char* key, int& out)
{
    const std::string pat = std::string("\"") + key + "\"";
    size_t p = src.find(pat);
    if (p == std::string::npos) {
        return false;
    }
    p = src.find(':', p);
    if (p == std::string::npos) {
        return false;
    }
    ++p;
    while (p < src.size() && std::isspace(static_cast<unsigned char>(src[p]))) {
        ++p;
    }
    char* endp = nullptr;
    const long v = std::strtol(src.c_str() + p, &endp, 10);
    if (endp == src.c_str() + p) {
        return false;
    }
    out = static_cast<int>(v);
    return true;
}

bool ExtractObjectField(const std::string& src, const char* key, std::string& out)
{
    const std::string pat = std::string("\"") + key + "\"";
    size_t p = src.find(pat);
    if (p == std::string::npos) {
        return false;
    }
    p = src.find('{', p);
    if (p == std::string::npos) {
        return false;
    }

    int depth = 0;
    const size_t start = p;
    for (; p < src.size(); ++p) {
        if (src[p] == '{') {
            ++depth;
        } else if (src[p] == '}') {
            --depth;
            if (depth == 0) {
                out = src.substr(start, p - start + 1);
                return true;
            }
        }
    }
    return false;
}

bool ParseSliderValuesField(const std::string& src, int outValues[kSliderCount])
{
    size_t p = src.find("\"slider_values\"");
    if (p == std::string::npos) {
        return false;
    }
    p = src.find('[', p);
    if (p == std::string::npos) {
        return false;
    }
    const size_t end = src.find(']', p);
    if (end == std::string::npos || end <= p) {
        return false;
    }

    std::string arr = src.substr(p + 1, end - p - 1);
    std::stringstream ss(arr);
    std::vector<int> vals;
    while (ss) {
        while (ss && (ss.peek() == ' ' || ss.peek() == '\t' || ss.peek() == '\r' || ss.peek() == '\n' || ss.peek() == ',')) {
            ss.get();
        }
        if (!ss) {
            break;
        }
        int v = 0;
        ss >> v;
        if (!ss.fail()) {
            vals.push_back(v);
        }
    }

    if (static_cast<int>(vals.size()) != kSliderCount) {
        return false;
    }

    for (int i = 0; i < kSliderCount; ++i) {
        outValues[i] = std::clamp(vals[i], kSliders[i].minValue, kSliders[i].maxValue);
    }
    return true;
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

bool GetBypassChecked()
{
    return static_cast<LRESULT>(SendMessageW(g_app.bypassCheck, BM_GETCHECK, 0, 0)) == BST_CHECKED;
}

void UpdateCustomButtonVisuals()
{
    for (int i = 0; i < 2; ++i) {
        if (g_app.customButtons[i]) {
            InvalidateRect(g_app.customButtons[i], nullptr, TRUE);
        }
        if (g_app.saveButtons[i]) {
            InvalidateRect(g_app.saveButtons[i], nullptr, TRUE);
        }
    }
}

void ClearActiveCustomSelection()
{
    if (g_app.activeCustomSlot != 0) {
        g_app.activeCustomSlot = 0;
        UpdateCustomButtonVisuals();
    }
}

void CopyCurrentUiToSlot(int slotIndex)
{
    if (slotIndex < 0 || slotIndex >= 2) {
        return;
    }

    g_app.customSaved[slotIndex] = true;
    g_app.customSaveLatched[slotIndex] = true;
    for (int i = 0; i < kSliderCount; ++i) {
        g_app.customSliderValues[slotIndex][i] = GetSliderValue(i);
    }
}

void ApplySlotToUi(int slotIndex)
{
    if (slotIndex < 0 || slotIndex >= 2 || !g_app.customSaved[slotIndex]) {
        return;
    }

    for (int i = 0; i < kSliderCount; ++i) {
        SendMessageW(g_app.sliders[i], TBM_SETPOS, TRUE, g_app.customSliderValues[slotIndex][i]);
    }
    UpdateValueTexts();
    g_app.activeCustomSlot = slotIndex + 1;
    UpdateCustomButtonVisuals();
    if (g_app.hwnd) {
        InvalidateRect(g_app.hwnd, nullptr, FALSE);
    }
}

bool SaveStateToJson(bool updateStatus = false)
{
    ResolveConfigPaths();

    FILE* fp = nullptr;
    if (_wfopen_s(&fp, g_app.configFilePath.c_str(), L"wb") != 0 || !fp) {
        if (updateStatus) {
            SetStatus(L"Color 設定ファイルの保存に失敗しました。");
        }
        return false;
    }

    std::ostringstream oss;
    oss << "{\n";
    oss << "  \"bypass\": " << (GetBypassChecked() ? "true" : "false") << ",\n";
    oss << "  \"active_custom_slot\": " << g_app.activeCustomSlot << ",\n";
    oss << "  \"slider_values\": [";
    for (int i = 0; i < kSliderCount; ++i) {
        if (i) {
            oss << ", ";
        }
        oss << GetSliderValue(i);
    }
    oss << "],\n";

    for (int slot = 0; slot < 2; ++slot) {
        oss << "  \"custom" << (slot + 1) << "\": {\n";
        oss << "    \"saved\": " << (g_app.customSaved[slot] ? "true" : "false") << ",\n";
        oss << "    \"save_latched\": " << (g_app.customSaveLatched[slot] ? "true" : "false") << ",\n";
        oss << "    \"bypass\": false,\n"; // legacy compatibility; bypass is not part of custom slot state
        oss << "    \"slider_values\": [";
        for (int i = 0; i < kSliderCount; ++i) {
            if (i) {
                oss << ", ";
            }
            oss << g_app.customSliderValues[slot][i];
        }
        oss << "]\n";
        oss << "  }" << (slot == 0 ? "," : "") << "\n";
    }
    oss << "}\n";

    const std::string json = oss.str();
    const size_t written = fwrite(json.data(), 1, json.size(), fp);
    fclose(fp);

    if (written != json.size()) {
        if (updateStatus) {
            SetStatus(L"Color 設定ファイルの書き込みに失敗しました。");
        }
        return false;
    }

    if (updateStatus) {
        SetStatus(L"Color 設定を保存しました。");
    }
    return true;
}

bool LoadStateFromJson(bool quietIfMissing = true)
{
    ResolveConfigPaths();

    FILE* fp = nullptr;
    if (_wfopen_s(&fp, g_app.configFilePath.c_str(), L"rb") != 0 || !fp) {
        if (!quietIfMissing) {
            SetStatus(L"保存済み Color 設定が見つかりません。");
        }
        return false;
    }

    fseek(fp, 0, SEEK_END);
    const long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz <= 0) {
        fclose(fp);
        if (!quietIfMissing) {
            SetStatus(L"Color 設定ファイルが空です。");
        }
        return false;
    }

    std::string json(static_cast<size_t>(sz), '\0');
    const size_t read = fread(json.data(), 1, static_cast<size_t>(sz), fp);
    fclose(fp);
    if (read != static_cast<size_t>(sz)) {
        if (!quietIfMissing) {
            SetStatus(L"Color 設定ファイルの読み込みに失敗しました。");
        }
        return false;
    }

    bool currentBypass = false;
    int currentValues[kSliderCount]{};
    if (!ParseSliderValuesField(json, currentValues)) {
        if (!quietIfMissing) {
            SetStatus(L"Color 設定ファイルの slider_values を読めませんでした。");
        }
        return false;
    }
    ParseBoolField(json, "bypass", currentBypass);

    int activeSlot = 0;
    ParseIntField(json, "active_custom_slot", activeSlot);
    activeSlot = std::clamp(activeSlot, 0, 2);

    for (int slot = 0; slot < 2; ++slot) {
        g_app.customSaved[slot] = false;
        g_app.customSaveLatched[slot] = false;
        g_app.customBypass[slot] = false;
        for (int i = 0; i < kSliderCount; ++i) {
            g_app.customSliderValues[slot][i] = kSliders[i].defaultValue;
        }

        std::string obj;
        std::string key = std::string("custom") + char('1' + slot);
        if (!ExtractObjectField(json, key.c_str(), obj)) {
            continue;
        }

        bool saved = false;
        bool saveLatched = false;
        bool bypass = false;
        int values[kSliderCount]{};
        ParseBoolField(obj, "saved", saved);
        ParseBoolField(obj, "save_latched", saveLatched);
        ParseBoolField(obj, "bypass", bypass); // legacy field; bypass is now treated as global preview state
        if (saved && ParseSliderValuesField(obj, values)) {
            g_app.customSaved[slot] = true;
            g_app.customSaveLatched[slot] = saveLatched;
            g_app.customBypass[slot] = bypass;
            for (int i = 0; i < kSliderCount; ++i) {
                g_app.customSliderValues[slot][i] = values[i];
            }
        }
    }

    for (int i = 0; i < kSliderCount; ++i) {
        SendMessageW(g_app.sliders[i], TBM_SETPOS, TRUE, currentValues[i]);
    }
    SendMessageW(g_app.bypassCheck, BM_SETCHECK, currentBypass ? BST_CHECKED : BST_UNCHECKED, 0);
    UpdateValueTexts();

    g_app.activeCustomSlot = 0;
    if (activeSlot >= 1 && activeSlot <= 2 && g_app.customSaved[activeSlot - 1]) {
        ApplySlotToUi(activeSlot - 1);
    }

    UpdateCustomButtonVisuals();
    return true;
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

bool RemoveTrackedFilter(const char* filterLabel, bool& tracked, std::wstring& errorText)
{
    if (!tracked) {
        return true;
    }
    if (!SendCommand(BuildRemoveCommandJson(filterLabel, g_app.requestId++), errorText)) {
        return false;
    }
    tracked = false;
    return true;
}

bool AddEqFilterTracked(std::wstring& errorText)
{
    if (!SendCommand(BuildEqFilterCommandJson(g_app.requestId++), errorText)) {
        return false;
    }
    g_app.eqFilterApplied = true;
    return true;
}

bool AddHueFilterTracked(std::wstring& errorText)
{
    if (!SendCommand(BuildHueFilterCommandJson(g_app.requestId++), errorText)) {
        return false;
    }
    g_app.hueFilterApplied = true;
    return true;
}

bool ApplyCurrentValues()
{
    UpdateValueTexts();
    SaveStateToJson(false);

    const bool bypass = GetBypassChecked();
    std::wstring errorText;

    if (bypass || IsDefaultState()) {
        if (!RemoveTrackedFilter(kEqFilterLabel, g_app.eqFilterApplied, errorText)) {
            SetStatus(errorText);
            return false;
        }
        if (!RemoveTrackedFilter(kHueFilterLabel, g_app.hueFilterApplied, errorText)) {
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
        if (!RemoveTrackedFilter(kEqFilterLabel, g_app.eqFilterApplied, errorText)) {
            SetStatus(errorText);
            return false;
        }
    } else {
        if (!AddEqFilterTracked(errorText)) {
            SetStatus(errorText);
            return false;
        }
    }

    if (IsHueDefaultState()) {
        if (!RemoveTrackedFilter(kHueFilterLabel, g_app.hueFilterApplied, errorText)) {
            SetStatus(errorText);
            return false;
        }
    } else {
        if (!AddHueFilterTracked(errorText)) {
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
    ClearActiveCustomSelection();
    UpdateValueTexts();
    SaveStateToJson(false);
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

HWND CreateOwnerDrawButton(HWND parent, int x, int y, int w, int h, const wchar_t* text, int id, HFONT font)
{
    HWND hwnd = CreateWindowExW(0, L"BUTTON", text,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
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

COLORREF BlendColor(COLORREF color, int delta)
{
    const int r = std::clamp(static_cast<int>(GetRValue(color)) + delta, 0, 255);
    const int g = std::clamp(static_cast<int>(GetGValue(color)) + delta, 0, 255);
    const int b = std::clamp(static_cast<int>(GetBValue(color)) + delta, 0, 255);
    return RGB(r, g, b);
}

bool IsCustomButtonId(UINT id)
{
    return id == IDC_CUSTOM1_BUTTON || id == IDC_CUSTOM2_BUTTON;
}

bool IsSaveButtonId(UINT id)
{
    return id == IDC_CUSTOM1_SAVE || id == IDC_CUSTOM2_SAVE;
}

void DrawSlotButton(const DRAWITEMSTRUCT* dis)
{
    const int slotIndex = (dis->CtlID == IDC_CUSTOM1_BUTTON || dis->CtlID == IDC_CUSTOM1_SAVE) ? 0 : 1;
    const bool isCustomButton = IsCustomButtonId(dis->CtlID);
    const bool isPressed = (dis->itemState & ODS_SELECTED) != 0;
    const bool isFocused = (dis->itemState & ODS_FOCUS) != 0;

    const bool active = isCustomButton
        ? (g_app.activeCustomSlot == slotIndex + 1)
        : g_app.customSaveLatched[slotIndex];
    const bool saved = g_app.customSaved[slotIndex];
    const bool drawSunken = isPressed || active;

    COLORREF bg = GetSysColor(COLOR_BTNFACE);
    COLORREF border = RGB(120, 120, 120);
    COLORREF text = RGB(40, 40, 40);

    if (isCustomButton) {
        if (active) {
            bg = RGB(46, 146, 74);
            border = RGB(24, 88, 42);
            text = RGB(255, 255, 255);
        } else if (saved) {
            bg = RGB(236, 244, 236);
            border = RGB(122, 148, 122);
            text = RGB(30, 60, 30);
        } else {
            bg = RGB(240, 240, 240);
            border = RGB(168, 168, 168);
            text = RGB(70, 70, 70);
        }
    } else {
        if (active) {
            bg = RGB(76, 120, 216);
            border = RGB(47, 77, 142);
            text = RGB(255, 255, 255);
        } else if (saved) {
            bg = RGB(246, 238, 220);
            border = RGB(172, 145, 92);
            text = RGB(96, 72, 26);
        } else {
            bg = RGB(242, 242, 242);
            border = RGB(170, 170, 170);
            text = RGB(96, 96, 96);
        }
    }

    if (isPressed) {
        bg = BlendColor(bg, -14);
        border = BlendColor(border, -10);
    }

    RECT rcFace = dis->rcItem;
    HBRUSH brush = CreateSolidBrush(bg);
    FillRect(dis->hDC, &rcFace, brush);
    DeleteObject(brush);

    RECT rcEdge = dis->rcItem;
    DrawEdge(dis->hDC, &rcEdge, drawSunken ? EDGE_SUNKEN : EDGE_RAISED, BF_RECT | BF_SOFT | BF_ADJUST);

    HPEN pen = CreatePen(PS_SOLID, 1, border);
    HPEN oldPen = static_cast<HPEN>(SelectObject(dis->hDC, pen));
    HBRUSH oldBrush = static_cast<HBRUSH>(SelectObject(dis->hDC, GetStockObject(NULL_BRUSH)));
    Rectangle(dis->hDC, rcEdge.left, rcEdge.top, rcEdge.right, rcEdge.bottom);
    SelectObject(dis->hDC, oldBrush);
    SelectObject(dis->hDC, oldPen);
    DeleteObject(pen);

    wchar_t textBuf[64]{};
    GetWindowTextW(dis->hwndItem, textBuf, static_cast<int>(std::size(textBuf)));

    RECT rcText = rcEdge;
    InflateRect(&rcText, -2, -1);
    if (drawSunken) {
        OffsetRect(&rcText, 1, 1);
    }

    SetBkMode(dis->hDC, TRANSPARENT);
    SetTextColor(dis->hDC, text);
    DrawTextW(dis->hDC, textBuf, -1, &rcText, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    if (isFocused) {
        RECT rcFocus = rcEdge;
        InflateRect(&rcFocus, -3, -3);
        DrawFocusRect(dis->hDC, &rcFocus);
    }
}

void SaveCurrentToCustomSlot(int slotIndex)
{
    if (slotIndex < 0 || slotIndex >= 2) {
        return;
    }

    CopyCurrentUiToSlot(slotIndex);
    SaveStateToJson(false);
    UpdateCustomButtonVisuals();
    SetStatus(slotIndex == 0 ? L"Custom1 に現在の色調補正を保存しました。" : L"Custom2 に現在の色調補正を保存しました。");
}

void ToggleSaveSlot(int slotIndex)
{
    if (slotIndex < 0 || slotIndex >= 2) {
        return;
    }

    if (g_app.customSaveLatched[slotIndex]) {
        g_app.customSaveLatched[slotIndex] = false;
        SaveStateToJson(false);
        UpdateCustomButtonVisuals();
        SetStatus(slotIndex == 0
            ? L"Custom1 Save を解除しました。もう一度 Save で現在値を上書き保存します。"
            : L"Custom2 Save を解除しました。もう一度 Save で現在値を上書き保存します。");
        return;
    }

    SaveCurrentToCustomSlot(slotIndex);
}

void ActivateCustomSlot(int slotIndex)
{
    if (slotIndex < 0 || slotIndex >= 2) {
        return;
    }
    if (!g_app.customSaved[slotIndex]) {
        SetStatus(slotIndex == 0 ? L"Custom1 はまだ保存されていません。" : L"Custom2 はまだ保存されていません。");
        return;
    }

    KillTimer(g_app.hwnd, IDC_TIMER_APPLY);
    g_app.pendingApply = false;

    if (g_app.activeCustomSlot == slotIndex + 1) {
        ResetSliders();
        UpdateWindow(g_app.customButtons[0]);
        UpdateWindow(g_app.customButtons[1]);
        UpdateWindow(g_app.saveButtons[0]);
        UpdateWindow(g_app.saveButtons[1]);

        ApplyCurrentValues();
        SaveStateToJson(false);
        SetStatus(slotIndex == 0
            ? L"Custom1 を解除して初期値に戻しました。"
            : L"Custom2 を解除して初期値に戻しました。");
        return;
    }

    ApplySlotToUi(slotIndex);
    UpdateWindow(g_app.customButtons[0]);
    UpdateWindow(g_app.customButtons[1]);
    UpdateWindow(g_app.saveButtons[0]);
    UpdateWindow(g_app.saveButtons[1]);

    ApplyCurrentValues();
    SaveStateToJson(false);
    SetStatus(slotIndex == 0 ? L"Custom1 を反映しました。" : L"Custom2 を反映しました。");
}

void CreateUi(HWND hwnd)
{
    g_app.hwnd = hwnd;
    CreateFonts(hwnd);

    const int margin = Scale(12);
    const int titleY = Scale(10);
    const int headerH = Scale(20);
    const int customY1 = Scale(8);
    const int customRowGap = Scale(6);
    const int customBtnH = Scale(24);
    const int controlsY = Scale(66);
    const int statusY = Scale(98);
    const int top = Scale(128);
    const int rowH = Scale(34);
    const int labelX = margin;
    const int labelW = Scale(110);
    const int sliderX = Scale(126);
    const int sliderW = Scale(270);
    const int valueX = Scale(408);
    const int valueW = Scale(72);

    const int customBtnW = Scale(72);
    const int saveBtnW = Scale(48);
    const int customGap = Scale(6);
    const int rightMargin = Scale(12);
    const int rowTotalW = customBtnW + customGap + saveBtnW;
    const int rowX = Scale(508) - rightMargin - rowTotalW;
    const int customY2 = customY1 + customBtnH + customRowGap;
    const int resetY = top + kSliderCount * rowH + Scale(16);

    CreateStatic(hwnd, margin, titleY, Scale(180), headerH, L"mpv Color UI", 0, g_app.fontTitle);

    g_app.customButtons[0] = CreateOwnerDrawButton(hwnd, rowX, customY1, customBtnW, customBtnH, L"Custom1", IDC_CUSTOM1_BUTTON, g_app.fontSmall);
    g_app.saveButtons[0]   = CreateOwnerDrawButton(hwnd, rowX + customBtnW + customGap, customY1, saveBtnW, customBtnH, L"Save", IDC_CUSTOM1_SAVE, g_app.fontSmall);
    g_app.customButtons[1] = CreateOwnerDrawButton(hwnd, rowX, customY2, customBtnW, customBtnH, L"Custom2", IDC_CUSTOM2_BUTTON, g_app.fontSmall);
    g_app.saveButtons[1]   = CreateOwnerDrawButton(hwnd, rowX + customBtnW + customGap, customY2, saveBtnW, customBtnH, L"Save", IDC_CUSTOM2_SAVE, g_app.fontSmall);

    g_app.bypassCheck = CreateCheckbox(hwnd, margin, controlsY + Scale(4), Scale(150), Scale(22), L"Bypass (一時無効)", IDC_BYPASS_CHECK, g_app.fontNormal);
    CreateButtonCtrl(hwnd, Scale(410), resetY, Scale(86), Scale(28), L"Reset", IDC_RESET_BUTTON, g_app.fontNormal);

    g_app.statusStatic = CreateStatic(hwnd, margin, statusY, Scale(454), Scale(24),
        L"Ready", IDC_STATUS_STATIC, g_app.fontSmall);

    for (int i = 0; i < kSliderCount; ++i) {
        const int y = top + i * rowH;
        CreateStatic(hwnd, labelX, y + Scale(4), labelW, Scale(18), kSliders[i].title, BASE_LABEL_ID + i, g_app.fontNormal);
        g_app.sliders[i] = CreateSlider(hwnd, sliderX, y, sliderW, Scale(28), BASE_SLIDER_ID + i, kSliders[i]);
        g_app.values[i] = CreateStatic(hwnd, valueX, y + Scale(4), valueW, Scale(18), L"", BASE_VALUE_ID + i, g_app.fontNormal, WS_CHILD | WS_VISIBLE | SS_RIGHT);
    }

    UpdateValueTexts();
    const bool loadedState = LoadStateFromJson(true);
    UpdateCustomButtonVisuals();

    if (loadedState) {
        SetStatus(L"前回の Color 設定を読み込み、mpv へ自動反映します。");
        ScheduleApply(hwnd);
    } else {
        SetStatus(L"Ready");
    }

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
        case IDC_RESET_BUTTON:
            ResetSliders();
            ApplyCurrentValues();
            return 0;
        case IDC_BYPASS_CHECK:
            SaveStateToJson(false);
            ApplyCurrentValues();
            return 0;
        case IDC_CUSTOM1_BUTTON:
            ActivateCustomSlot(0);
            return 0;
        case IDC_CUSTOM2_BUTTON:
            ActivateCustomSlot(1);
            return 0;
        case IDC_CUSTOM1_SAVE:
            ToggleSaveSlot(0);
            return 0;
        case IDC_CUSTOM2_SAVE:
            ToggleSaveSlot(1);
            return 0;
        default:
            break;
        }
        break;

    case WM_HSCROLL:
        for (int i = 0; i < kSliderCount; ++i) {
            if (reinterpret_cast<HWND>(lParam) == g_app.sliders[i]) {
                UpdateValueTexts();
                if (GetBypassChecked()) {
                    SendMessageW(g_app.bypassCheck, BM_SETCHECK, BST_UNCHECKED, 0);
                }
                ClearActiveCustomSelection();
                SaveStateToJson(false);
                ScheduleApply(hwnd);
                return 0;
            }
        }
        break;

    case WM_DRAWITEM:
        if (IsCustomButtonId(static_cast<UINT>(wParam)) || IsSaveButtonId(static_cast<UINT>(wParam))) {
            DrawSlotButton(reinterpret_cast<const DRAWITEMSTRUCT*>(lParam));
            return TRUE;
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

    case WMAPP_RESET_UI:
        KillTimer(hwnd, IDC_TIMER_APPLY);
        g_app.pendingApply = false;
        ResetSliders();
        ApplyCurrentValues();
        SetStatus(L"外部から全フィルターOFFが実行されたため、Color UIも初期化しました。");
        return 0;

    case WM_DESTROY:
        SaveStateToJson(false);
        KillTimer(hwnd, IDC_TIMER_APPLY);
        KillTimer(hwnd, IDC_TIMER_PIPE_MONITOR);
        if (g_app.fontNormal) DeleteObject(g_app.fontNormal);
        if (g_app.fontSmall) DeleteObject(g_app.fontSmall);
        if (g_app.fontTitle) DeleteObject(g_app.fontTitle);
        if (g_app.singleInstanceMutex) { CloseHandle(g_app.singleInstanceMutex); g_app.singleInstanceMutex = nullptr; }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR cmdLine, int nCmdShow)
{
    const std::wstring args = cmdLine ? cmdLine : L"";
    const bool resetUiOnly = (args.find(L"--reset-ui") != std::wstring::npos);

    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_BAR_CLASSES;
    InitCommonControlsEx(&icc);

    g_app.singleInstanceMutex = CreateMutexW(nullptr, TRUE, kSingleInstanceMutexName);
    if (!g_app.singleInstanceMutex) {
        MessageBoxW(nullptr, L"多重起動防止用ミューテックスの作成に失敗しました。", L"mpv_color_ui", MB_ICONERROR | MB_OK);
        return 1;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND existing = FindWindowW(kWindowClassName, nullptr);
        if (existing) {
            if (resetUiOnly) {
                SendMessageW(existing, WMAPP_RESET_UI, 0, 0);
            } else {
                ShowWindow(existing, IsIconic(existing) ? SW_RESTORE : SW_SHOW);
                SetForegroundWindow(existing);
            }
        }
        CloseHandle(g_app.singleInstanceMutex);
        g_app.singleInstanceMutex = nullptr;
        return 0;
    }

    if (resetUiOnly) {
        CloseHandle(g_app.singleInstanceMutex);
        g_app.singleInstanceMutex = nullptr;
        return 0;
    }

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
        532, 530,
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
