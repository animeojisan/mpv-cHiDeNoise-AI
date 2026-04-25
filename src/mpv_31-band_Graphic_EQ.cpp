
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <windowsx.h>
#include <mmdeviceapi.h>
#include <Audioclient.h>
#include <string>
#include <vector>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cctype>
#include <cstdlib>
#include <ksmedia.h>

#pragma comment(lib, "comctl32.lib")

struct BandDef {
    double freq;
    const wchar_t* label;
};

static const BandDef kBands[31] = {
    {20.0,    L"20"},   {25.0,    L"25"},   {31.5,    L"31.5"}, {40.0,    L"40"},
    {50.0,    L"50"},   {63.0,    L"63"},   {80.0,    L"80"},   {100.0,   L"100"},
    {125.0,   L"125"},  {160.0,   L"160"},  {200.0,   L"200"},  {250.0,   L"250"},
    {315.0,   L"315"},  {400.0,   L"400"},  {500.0,   L"500"},  {630.0,   L"630"},
    {800.0,   L"800"},  {1000.0,  L"1k"},   {1250.0,  L"1.25k"},{1600.0,  L"1.6k"},
    {2000.0,  L"2k"},   {2500.0,  L"2.5k"}, {3150.0,  L"3.15k"},{4000.0,  L"4k"},
    {5000.0,  L"5k"},   {6300.0,  L"6.3k"}, {8000.0,  L"8k"},   {10000.0, L"10k"},
    {12500.0, L"12.5k"},{16000.0, L"16k"},  {20000.0, L"20k"}
};

enum ControlIds {
    IDC_STATUS          = 100,
    IDC_EQ_ENABLE       = 101,
    IDC_RESET           = 104,
    IDC_APPLY           = 105,
    IDC_FLAT            = 106,
    IDC_VSHAPE          = 107,
    IDC_LOFI            = 108,
    IDC_BASS            = 109,
    IDC_VOCAL           = 110,
    IDC_TREBLE          = 111,
    IDC_SAVE            = 112,
    IDC_LOAD            = 113,
    IDC_CUSTOM1         = 114,
    IDC_CUSTOM1_SAVE    = 115,
    IDC_CUSTOM1_CLEAR   = 116,
    IDC_CUSTOM2         = 117,
    IDC_CUSTOM2_SAVE    = 118,
    IDC_CUSTOM2_CLEAR   = 119,
    IDC_GAINLINE        = 120,
    IDC_REVERB_KNOB     = 121,
    IDC_REVERB_DIST_KNOB= 122,
    IDC_REVERB_TIME_KNOB= 123,
    IDC_DELAY_KNOB      = 124,
    IDC_MIX             = 125,
    IDC_DELAY_FDB_KNOB  = 126,
    IDC_DELAY_SPD_KNOB  = 127,
    IDC_DUB_HPF_KNOB    = 128,
    IDC_DUB_LPF_KNOB    = 129,
    IDC_MIX_KNOB        = 130,
    IDC_BAND_BASE       = 1000,
    IDC_VALUE_BASE      = 2000
};

enum TimerIds {
    TIMER_APPLY = 1,
    TIMER_ANALYZER = 2,
    TIMER_PIPE_WATCH = 3
};

enum PresetMode {
    PRESET_NONE = 0,
    PRESET_FLAT = 1,
    PRESET_VSHAPE = 2,
    PRESET_LOFI = 3,
    PRESET_BASS = 4,
    PRESET_VOCAL = 5,
    PRESET_TREBLE = 6
};

static const int kGainMin = -20;
static const int kGainMax = 20;
static const int kWindowW = 1008;
static const int kWindowH = 752;
static const int kAnalysisWindow = 4096;
static const int kHistoryLimit = 32768;
static const double kPi = 3.14159265358979323846;

template<typename T>
static void SafeRelease(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

static void SafeCoTaskMemFree(void*& p) {
    if (p) {
        CoTaskMemFree(p);
        p = nullptr;
    }
}

struct AppState {
    HINSTANCE hInst = nullptr;
    HWND hwnd = nullptr;
    HWND status = nullptr;
    HWND eqEnable = nullptr;
    HWND resetBtn = nullptr;
    HWND applyBtn = nullptr;
    HWND flatBtn = nullptr;
    HWND vshapeBtn = nullptr;
    HWND lofiBtn = nullptr;
    HWND bassBtn = nullptr;
    HWND vocalBtn = nullptr;
    HWND trebleBtn = nullptr;
    HWND saveBtn = nullptr;
    HWND loadBtn = nullptr;
    HWND gainLineBtn = nullptr;
    HWND mixBtn = nullptr;
    HWND mixKnob = nullptr;
    HWND reverbKnob = nullptr;
    HWND reverbDistKnob = nullptr;
    HWND reverbTimeKnob = nullptr;
    HWND delayKnob = nullptr;
    HWND delayFeedbackKnob = nullptr;
    HWND delaySpeedKnob = nullptr;
    HWND dubHighpassKnob = nullptr;
    HWND dubLowpassKnob = nullptr;
    HWND customBtns[2]{};
    HWND customSaveBtns[2]{};
    HWND customClearBtns[2]{};
    HWND sliders[31]{};
    HWND valueLabels[31]{};
    RECT meterRect{};
    HFONT font = nullptr;
    std::wstring pipePath = L"\\\\.\\pipe\\mpv-tool";
    bool eqEnabled = true;
    int gains[31]{};
    std::wstring statusText = L"Ready";

    IMMDeviceEnumerator* deviceEnum = nullptr;
    IMMDevice* device = nullptr;
    IAudioClient* audioClient = nullptr;
    IAudioCaptureClient* captureClient = nullptr;
    WAVEFORMATEX* mixFormat = nullptr;
    double sampleRate = 48000.0;
    std::vector<float> sampleHistory;
    float bandLevel[31]{};
    float bandPeak[31]{};
    int bandPeakTicks[31]{};
    int pipeMissCount = 0;
    bool autoCloseOnPipeLoss = true;
    bool eq31FilterApplied = false;
    bool customSaved[2]{};
    bool customEqEnabled[2]{ true, true };
    int customGains[2][31]{};
    bool customEffectsMixEnabled[2]{ true, true };
    int customEffectsMixAmount[2]{ 0, 0 };
    int customReverbAmount[2]{};
    int customReverbDistance[2]{};
    int customReverbTime[2]{};
    int customDelayAmount[2]{};
    int customDelayFeedback[2]{};
    int customDelaySpeed[2]{};
    int customDubHighpass[2]{};
    int customDubLowpass[2]{};
    int activeCustomSlot = 0;
    int activePreset = PRESET_NONE;
    bool showGainLine = false;
    bool effectsMixEnabled = true;
    int effectsMixAmount = 0;
    double effectsMixCurrent = 0.0;
    double effectsMixTarget = 0.0;
    bool effectsMixFading = false;
    int hqResamplerMode = 1; // 1 = prefer soxr, 0 = HQ swr fallback
    int reverbAmount = 0;
    int reverbDistance = 0;
    int reverbTime = 0;
    int delayAmount = 0;
    int delayFeedback = 0;
    int delaySpeed = 0;
    int dubHighpass = 0;
    int dubLowpass = 0;
    bool knobTracking = false;
    int knobTrackingId = 0;
    int knobTrackingStartY = 0;
    int knobTrackingStartValue = 0;
    std::wstring configDir;
    std::wstring configFilePath;
};

static AppState g_app;

static void SyncSlidersFromState();
static void ScheduleApply();
static void ScheduleApplyWithDelay(UINT delayMs);
static void SetStatus(const std::wstring& text);
static void UpdatePresetButtonsVisual();
static void UpdateCustomButtonsVisual();
static void UpdateGainLineButtonVisual();
static void UpdateMixButtonVisual();
static void UpdateEffectKnobsVisual();
static void ActivatePresetButton(int presetId);
static int* GetKnobValuePtr(int controlId);
static const wchar_t* GetKnobLabel(int controlId);
static void InvalidateEffectKnob(HWND knob);
static void SetKnobValue(int controlId, int value, bool fromUser);
static bool HasActiveEffectSettings();
static std::string BuildHighQualityResampleFilter();
static double GetDesiredEffectsMix();
static void SetEffectsMixEnabled(bool enabled, bool animate);
static void RefreshEffectsMixTarget(bool animate);
static void AdvanceEffectsMixFadeStep();
static std::string BuildFilterGraph();
static void DrawKnobControl(HWND hwnd, HDC hdc);
static LRESULT CALLBACK KnobWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
static void ActivateCustomSlot(int slotIndex);
static void SaveCurrentToCustomSlot(int slotIndex);
static void ApplyFlatNeutralState();
static void ClearCustomSlot(int slotIndex);
static void ResetCurrentState();
static void ApplyPresetById(int presetId);
static bool IsToggleButtonActive(int controlId);
static void InvalidateToggleButton(HWND btn);
static void DrawOwnerToggleButton(const DRAWITEMSTRUCT* dis);

struct BandsLayout {
    int sliderTop = 0;
    int sliderW = 0;
    int sliderH = 0;
    int gap = 0;
    int totalW = 0;
    int startX = 0;
    int meterBarW = 0;
};


static std::string JsonEscape(const std::string& s) {
    std::ostringstream oss;
    for (unsigned char c : s) {
        switch (c) {
        case '"': oss << "\\\""; break;
        case '\\': oss << "\\\\"; break;
        case '\b': oss << "\\b"; break;
        case '\f': oss << "\\f"; break;
        case '\n': oss << "\\n"; break;
        case '\r': oss << "\\r"; break;
        case '\t': oss << "\\t"; break;
        default:
            if (c < 0x20) {
                oss << "\\u" << std::hex << std::setw(4) << std::setfill('0') << (int)c;
            } else {
                oss << (char)c;
            }
            break;
        }
    }
    return oss.str();
}


static std::wstring GetModuleDir() {
    wchar_t path[MAX_PATH]{};
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring s(path, path + n);
    size_t pos = s.find_last_of(L"\\/");
    if (pos != std::wstring::npos) s.resize(pos);
    return s;
}

static std::wstring GetCurrentDirWString() {
    DWORD n = GetCurrentDirectoryW(0, nullptr);
    std::wstring s;
    s.resize(n ? (n - 1) : 0);
    if (n > 1) GetCurrentDirectoryW(n, s.data());
    return s;
}

static bool DirExists(const std::wstring& path) {
    DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
}

static bool EnsureDirRecursive(const std::wstring& path) {
    if (path.empty()) return false;
    if (DirExists(path)) return true;

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
        while (j < path.size() && path[j] != L'\\' && path[j] != L'/') ++j;
        if (j > i) {
            current.append(path, i, j - i);
            DWORD attr = GetFileAttributesW(current.c_str());
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


static bool ParseIntField(const std::string& src, const char* key, int& out) {
    const std::string pat = std::string("\"") + key + "\"";
    size_t p = src.find(pat);
    if (p == std::string::npos) return false;
    p = src.find(':', p);
    if (p == std::string::npos) return false;
    ++p;
    while (p < src.size() && std::isspace((unsigned char)src[p])) ++p;
    char* endp = nullptr;
    long v = std::strtol(src.c_str() + p, &endp, 10);
    if (endp == src.c_str() + p) return false;
    out = (int)v;
    return true;
}

static bool ExtractObjectField(const std::string& src, const char* key, std::string& out) {
    const std::string pat = std::string("\"") + key + "\"";
    size_t p = src.find(pat);
    if (p == std::string::npos) return false;
    p = src.find('{', p);
    if (p == std::string::npos) return false;
    int depth = 0;
    size_t start = p;
    for (; p < src.size(); ++p) {
        if (src[p] == '{') ++depth;
        else if (src[p] == '}') {
            --depth;
            if (depth == 0) {
                out = src.substr(start, p - start + 1);
                return true;
            }
        }
    }
    return false;
}

static bool ParseGainsField(const std::string& src, int outGains[31]) {
    size_t pg = src.find("\"gains\"");
    if (pg == std::string::npos) return false;
    pg = src.find('[', pg);
    size_t pe = (pg == std::string::npos) ? std::string::npos : src.find(']', pg);
    if (pg == std::string::npos || pe == std::string::npos || pe <= pg) return false;

    std::string arr = src.substr(pg + 1, pe - pg - 1);
    std::vector<int> vals;
    std::stringstream ss(arr);
    while (ss) {
        while (ss && (ss.peek() == ' ' || ss.peek() == '\t' || ss.peek() == '\r' || ss.peek() == '\n' || ss.peek() == ',')) ss.get();
        if (!ss) break;
        int v = 0;
        ss >> v;
        if (!ss.fail()) vals.push_back(std::clamp(v, kGainMin, kGainMax));
    }
    if ((int)vals.size() != 31) return false;
    for (int i = 0; i < 31; ++i) outGains[i] = vals[i];
    return true;
}

static void ResolveConfigPaths() {
    if (!g_app.configFilePath.empty()) return;

    std::vector<std::wstring> bases;
    const std::wstring cwd = GetCurrentDirWString();
    const std::wstring mod = GetModuleDir();
    if (!cwd.empty()) bases.push_back(cwd);
    if (!mod.empty() && mod != cwd) bases.push_back(mod);

    std::wstring portableBase;
    for (const auto& b : bases) {
        std::wstring cand = b + L"\\portable_config";
        if (DirExists(cand)) {
            portableBase = cand;
            break;
        }
    }
    if (portableBase.empty()) {
        portableBase = (cwd.empty() ? mod : cwd) + L"\\portable_config";
    }

    g_app.configDir = portableBase + L"\\cache\\EQ31";
    g_app.configFilePath = g_app.configDir + L"\\eq31_state.json";
    EnsureDirRecursive(g_app.configDir);
}

static bool SaveStateToJson(bool updateStatus = true) {
    ResolveConfigPaths();
    FILE* fp = nullptr;
    if (_wfopen_s(&fp, g_app.configFilePath.c_str(), L"wb") != 0 || !fp) {
        SetStatus(L"EQ設定の保存先を開けませんでした。");
        return false;
    }

    std::ostringstream oss;
    oss << "{\n";
    oss << "  \"eq_enabled\": " << (g_app.eqEnabled ? "true" : "false") << ",\n";
    oss << "  \"gains\": [";
    for (int i = 0; i < 31; ++i) {
        if (i) oss << ", ";
        oss << g_app.gains[i];
    }
    oss << "],\n";
    oss << "  \"active_preset\": " << g_app.activePreset << ",\n";
    oss << "  \"show_gain_line\": " << (g_app.showGainLine ? "true" : "false") << ",\n";
    oss << "  \"effects_mix_enabled\": " << (g_app.effectsMixEnabled ? "true" : "false") << ",\n";
    oss << "  \"effects_mix_amount\": " << g_app.effectsMixAmount << ",\n";
    oss << "  \"reverb_amount\": " << g_app.reverbAmount << ",\n";
    oss << "  \"reverb_distance\": " << g_app.reverbDistance << ",\n";
    oss << "  \"reverb_time\": " << g_app.reverbTime << ",\n";
    oss << "  \"echo_amount\": " << g_app.delayAmount << ",\n";
    oss << "  \"echo_repeat\": " << g_app.delayFeedback << ",\n";
    oss << "  \"echo_interval\": " << g_app.delaySpeed << ",\n";
    oss << "  \"wet_tone\": " << g_app.dubHighpass << ",\n";
    oss << "  \"wet_diffusion\": " << g_app.dubLowpass << ",\n";
    oss << "  \"active_custom_slot\": " << g_app.activeCustomSlot << ",\n";
    for (int s = 0; s < 2; ++s) {
        oss << "  \"custom" << (s + 1) << "\": {\n";
        oss << "    \"saved\": " << (g_app.customSaved[s] ? "true" : "false") << ",\n";
        oss << "    \"eq_enabled\": " << (g_app.customEqEnabled[s] ? "true" : "false") << ",\n";
        oss << "    \"effects_mix_enabled\": " << (g_app.customEffectsMixEnabled[s] ? "true" : "false") << ",\n";
        oss << "    \"effects_mix_amount\": " << g_app.customEffectsMixAmount[s] << ",\n";
        oss << "    \"reverb_amount\": " << g_app.customReverbAmount[s] << ",\n";
        oss << "    \"reverb_distance\": " << g_app.customReverbDistance[s] << ",\n";
        oss << "    \"reverb_time\": " << g_app.customReverbTime[s] << ",\n";
        oss << "    \"echo_amount\": " << g_app.customDelayAmount[s] << ",\n";
        oss << "    \"echo_repeat\": " << g_app.customDelayFeedback[s] << ",\n";
        oss << "    \"echo_interval\": " << g_app.customDelaySpeed[s] << ",\n";
        oss << "    \"wet_tone\": " << g_app.customDubHighpass[s] << ",\n";
        oss << "    \"wet_diffusion\": " << g_app.customDubLowpass[s] << ",\n";
        oss << "    \"gains\": [";
        for (int i = 0; i < 31; ++i) {
            if (i) oss << ", ";
            oss << g_app.customGains[s][i];
        }
        oss << "]\n";
        oss << "  }" << (s == 0 ? "," : "") << "\n";
    }
    oss << "}\n";
    const std::string json = oss.str();
    const size_t written = fwrite(json.data(), 1, json.size(), fp);
    fclose(fp);
    if (written != json.size()) {
        if (updateStatus) SetStatus(L"EQ設定ファイルの書き込みに失敗しました。");
        return false;
    }
    if (updateStatus) SetStatus(L"EQ設定を保存しました。");
    return true;
}

static bool ParseBoolField(const std::string& src, const char* key, bool& out) {
    const std::string pat = std::string("\"") + key + "\"";
    size_t p = src.find(pat);
    if (p == std::string::npos) return false;
    p = src.find(':', p);
    if (p == std::string::npos) return false;
    ++p;
    while (p < src.size() && std::isspace((unsigned char)src[p])) ++p;
    if (src.compare(p, 4, "true") == 0) { out = true; return true; }
    if (src.compare(p, 5, "false") == 0) { out = false; return true; }
    return false;
}

static bool LoadStateFromJson(bool quietIfMissing = false) {
    ResolveConfigPaths();
    FILE* fp = nullptr;
    if (_wfopen_s(&fp, g_app.configFilePath.c_str(), L"rb") != 0 || !fp) {
        if (!quietIfMissing) SetStatus(L"保存済みEQ設定が見つかりません。");
        return false;
    }

    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz <= 0) {
        fclose(fp);
        if (!quietIfMissing) SetStatus(L"EQ設定ファイルが空です。");
        return false;
    }

    std::string json((size_t)sz, '\0');
    const size_t read = fread(json.data(), 1, (size_t)sz, fp);
    fclose(fp);
    if (read != (size_t)sz) {
        if (!quietIfMissing) SetStatus(L"EQ設定ファイルの読み込みに失敗しました。");
        return false;
    }

    bool enabled = g_app.eqEnabled;
    ParseBoolField(json, "eq_enabled", enabled);
    bool showGainLine = false;
    ParseBoolField(json, "show_gain_line", showGainLine);
    int currentGains[31]{};
    if (!ParseGainsField(json, currentGains)) {
        if (!quietIfMissing) SetStatus(L"EQ設定ファイルの gains を読めませんでした。");
        return false;
    }

    int activePreset = PRESET_NONE;
    ParseIntField(json, "active_preset", activePreset);
    activePreset = std::clamp(activePreset, (int)PRESET_NONE, (int)PRESET_TREBLE);

    bool effectsMixEnabled = true;
    ParseBoolField(json, "effects_mix_enabled", effectsMixEnabled);
    ParseIntField(json, "effects_mix_amount", g_app.effectsMixAmount);
    ParseIntField(json, "reverb_amount", g_app.reverbAmount);
    ParseIntField(json, "reverb_distance", g_app.reverbDistance);
    ParseIntField(json, "reverb_time", g_app.reverbTime);
    ParseIntField(json, "echo_amount", g_app.delayAmount);
    ParseIntField(json, "echo_repeat", g_app.delayFeedback);
    ParseIntField(json, "echo_interval", g_app.delaySpeed);
    ParseIntField(json, "wet_tone", g_app.dubHighpass);
    ParseIntField(json, "wet_diffusion", g_app.dubLowpass);
    g_app.reverbAmount = std::clamp(g_app.reverbAmount, 0, 100);
    g_app.reverbDistance = std::clamp(g_app.reverbDistance, 0, 100);
    g_app.reverbTime = std::clamp(g_app.reverbTime, 0, 100);
    g_app.delayAmount = std::clamp(g_app.delayAmount, 0, 100);
    g_app.delayFeedback = std::clamp(g_app.delayFeedback, 0, 100);
    g_app.delaySpeed = std::clamp(g_app.delaySpeed, 0, 100);
    g_app.dubHighpass = std::clamp(g_app.dubHighpass, 0, 100);
    g_app.dubLowpass = std::clamp(g_app.dubLowpass, 0, 100);
    g_app.effectsMixAmount = std::clamp(g_app.effectsMixAmount, 0, 100);
    g_app.effectsMixEnabled = effectsMixEnabled;
    g_app.effectsMixCurrent = effectsMixEnabled ? (double)g_app.effectsMixAmount / 100.0 : 0.0;
    g_app.effectsMixTarget = g_app.effectsMixCurrent;
    g_app.effectsMixFading = false;

    int activeSlot = 0;
    ParseIntField(json, "active_custom_slot", activeSlot);
    activeSlot = std::clamp(activeSlot, 0, 2);

    for (int s = 0; s < 2; ++s) {
        g_app.customSaved[s] = false;
        g_app.customEqEnabled[s] = true;
        g_app.customEffectsMixEnabled[s] = true;
        g_app.customEffectsMixAmount[s] = 0;
        g_app.customReverbAmount[s] = 0;
        g_app.customReverbDistance[s] = 0;
        g_app.customReverbTime[s] = 0;
        g_app.customDelayAmount[s] = 0;
        g_app.customDelayFeedback[s] = 0;
        g_app.customDelaySpeed[s] = 0;
        g_app.customDubHighpass[s] = 0;
        g_app.customDubLowpass[s] = 0;
        for (int i = 0; i < 31; ++i) g_app.customGains[s][i] = 0;
        std::string obj;
        std::string key = std::string("custom") + char('1' + s);
        if (ExtractObjectField(json, key.c_str(), obj)) {
            bool saved = false;
            bool slotEq = true;
            bool slotFxEnabled = true;
            int slotFxAmount = 0;
            int slotReverbAmount = 0;
            int slotReverbDistance = 0;
            int slotReverbTime = 0;
            int slotDelayAmount = 0;
            int slotDelayFeedback = 0;
            int slotDelaySpeed = 0;
            int slotDubHighpass = 0;
            int slotDubLowpass = 0;
            int slotGains[31]{};
            ParseBoolField(obj, "saved", saved);
            ParseBoolField(obj, "eq_enabled", slotEq);
            ParseBoolField(obj, "effects_mix_enabled", slotFxEnabled);
            ParseIntField(obj, "effects_mix_amount", slotFxAmount);
            ParseIntField(obj, "reverb_amount", slotReverbAmount);
            ParseIntField(obj, "reverb_distance", slotReverbDistance);
            ParseIntField(obj, "reverb_time", slotReverbTime);
            ParseIntField(obj, "echo_amount", slotDelayAmount);
            ParseIntField(obj, "echo_repeat", slotDelayFeedback);
            ParseIntField(obj, "echo_interval", slotDelaySpeed);
            ParseIntField(obj, "wet_tone", slotDubHighpass);
            ParseIntField(obj, "wet_diffusion", slotDubLowpass);
            if (saved && ParseGainsField(obj, slotGains)) {
                g_app.customSaved[s] = true;
                g_app.customEqEnabled[s] = slotEq;
                g_app.customEffectsMixEnabled[s] = slotFxEnabled;
                g_app.customEffectsMixAmount[s] = std::clamp(slotFxAmount, 0, 100);
                g_app.customReverbAmount[s] = std::clamp(slotReverbAmount, 0, 100);
                g_app.customReverbDistance[s] = std::clamp(slotReverbDistance, 0, 100);
                g_app.customReverbTime[s] = std::clamp(slotReverbTime, 0, 100);
                g_app.customDelayAmount[s] = std::clamp(slotDelayAmount, 0, 100);
                g_app.customDelayFeedback[s] = std::clamp(slotDelayFeedback, 0, 100);
                g_app.customDelaySpeed[s] = std::clamp(slotDelaySpeed, 0, 100);
                g_app.customDubHighpass[s] = std::clamp(slotDubHighpass, 0, 100);
                g_app.customDubLowpass[s] = std::clamp(slotDubLowpass, 0, 100);
                for (int i = 0; i < 31; ++i) g_app.customGains[s][i] = slotGains[i];
            }
        }
    }

    g_app.eqEnabled = enabled;
    for (int i = 0; i < 31; ++i) g_app.gains[i] = currentGains[i];
    g_app.activeCustomSlot = 0;
    g_app.activePreset = PRESET_NONE;
    g_app.showGainLine = showGainLine;

    if (activeSlot >= 1 && activeSlot <= 2 && g_app.customSaved[activeSlot - 1]) {
        g_app.activeCustomSlot = activeSlot;
        g_app.eqEnabled = g_app.customEqEnabled[activeSlot - 1];
        g_app.effectsMixEnabled = g_app.customEffectsMixEnabled[activeSlot - 1];
        g_app.effectsMixAmount = g_app.customEffectsMixAmount[activeSlot - 1];
        g_app.reverbAmount = g_app.customReverbAmount[activeSlot - 1];
        g_app.reverbDistance = g_app.customReverbDistance[activeSlot - 1];
        g_app.reverbTime = g_app.customReverbTime[activeSlot - 1];
        g_app.delayAmount = g_app.customDelayAmount[activeSlot - 1];
        g_app.delayFeedback = g_app.customDelayFeedback[activeSlot - 1];
        g_app.delaySpeed = g_app.customDelaySpeed[activeSlot - 1];
        g_app.dubHighpass = g_app.customDubHighpass[activeSlot - 1];
        g_app.dubLowpass = g_app.customDubLowpass[activeSlot - 1];
        g_app.effectsMixCurrent = g_app.effectsMixEnabled ? (double)g_app.effectsMixAmount / 100.0 : 0.0;
        g_app.effectsMixTarget = g_app.effectsMixCurrent;
        g_app.effectsMixFading = false;
        for (int i = 0; i < 31; ++i) g_app.gains[i] = g_app.customGains[activeSlot - 1][i];
    } else if (activePreset != PRESET_NONE) {
        g_app.activePreset = activePreset;
        ApplyPresetById(activePreset);
    }

    if (g_app.hwnd) {
        SyncSlidersFromState();
        ScheduleApply();
    }
    SetStatus(L"EQ設定を読み込みました。");
    return true;
}

static void SetStatus(const std::wstring& text) {
    g_app.statusText = text;
    if (g_app.status && IsWindow(g_app.status)) {
        SetWindowTextW(g_app.status, text.c_str());
    }
}

static void InvalidateToggleButton(HWND btn) {
    if (btn) InvalidateRect(btn, nullptr, TRUE);
}

static bool IsToggleButtonActive(int controlId) {
    switch (controlId) {
    case IDC_FLAT:   return g_app.activePreset == PRESET_FLAT;
    case IDC_VSHAPE: return g_app.activePreset == PRESET_VSHAPE;
    case IDC_LOFI:   return g_app.activePreset == PRESET_LOFI;
    case IDC_BASS:   return g_app.activePreset == PRESET_BASS;
    case IDC_VOCAL:  return g_app.activePreset == PRESET_VOCAL;
    case IDC_TREBLE: return g_app.activePreset == PRESET_TREBLE;
    case IDC_CUSTOM1:return g_app.activeCustomSlot == 1;
    case IDC_CUSTOM2:return g_app.activeCustomSlot == 2;
    case IDC_GAINLINE:return g_app.showGainLine;
    case IDC_MIX:     return g_app.effectsMixEnabled;
    default:         return false;
    }
}

static void UpdatePresetButtonsVisual() {
    InvalidateToggleButton(g_app.flatBtn);
    InvalidateToggleButton(g_app.vshapeBtn);
    InvalidateToggleButton(g_app.lofiBtn);
    InvalidateToggleButton(g_app.bassBtn);
    InvalidateToggleButton(g_app.vocalBtn);
    InvalidateToggleButton(g_app.trebleBtn);
}

static void UpdateCustomButtonsVisual() {
    for (int s = 0; s < 2; ++s) {
        InvalidateToggleButton(g_app.customBtns[s]);
        if (g_app.customSaveBtns[s]) {
            Button_SetCheck(g_app.customSaveBtns[s], g_app.customSaved[s] ? BST_CHECKED : BST_UNCHECKED);
        }
    }
}

static void UpdateGainLineButtonVisual() {
    InvalidateToggleButton(g_app.gainLineBtn);
    if (g_app.hwnd) InvalidateRect(g_app.hwnd, &g_app.meterRect, FALSE);
}

static void UpdateMixButtonVisual() {
    InvalidateToggleButton(g_app.mixBtn);
}

static void UpdateEffectKnobsVisual() {
    InvalidateEffectKnob(g_app.mixKnob);
    InvalidateEffectKnob(g_app.reverbKnob);
    InvalidateEffectKnob(g_app.reverbDistKnob);
    InvalidateEffectKnob(g_app.reverbTimeKnob);
    InvalidateEffectKnob(g_app.delayKnob);
    InvalidateEffectKnob(g_app.delayFeedbackKnob);
    InvalidateEffectKnob(g_app.delaySpeedKnob);
    InvalidateEffectKnob(g_app.dubHighpassKnob);
    InvalidateEffectKnob(g_app.dubLowpassKnob);
}

static int* GetKnobValuePtr(int controlId) {
    switch (controlId) {
    case IDC_MIX_KNOB:         return &g_app.effectsMixAmount;
    case IDC_REVERB_KNOB:      return &g_app.reverbAmount;
    case IDC_REVERB_DIST_KNOB: return &g_app.reverbDistance;
    case IDC_REVERB_TIME_KNOB: return &g_app.reverbTime;
    case IDC_DELAY_KNOB:       return &g_app.delayAmount;
    case IDC_DELAY_FDB_KNOB:   return &g_app.delayFeedback;
    case IDC_DELAY_SPD_KNOB:   return &g_app.delaySpeed;
    case IDC_DUB_HPF_KNOB:     return &g_app.dubHighpass;
    case IDC_DUB_LPF_KNOB:     return &g_app.dubLowpass;
    default:                   return nullptr;
    }
}

static const wchar_t* GetKnobLabel(int controlId) {
    switch (controlId) {
    case IDC_MIX_KNOB:         return L"";
    case IDC_REVERB_KNOB:      return L"Rev";
    case IDC_REVERB_DIST_KNOB: return L"Rom";
    case IDC_REVERB_TIME_KNOB: return L"Tim";
    case IDC_DELAY_KNOB:       return L"Ech";
    case IDC_DELAY_FDB_KNOB:   return L"Rpt";
    case IDC_DELAY_SPD_KNOB:   return L"Int";
    case IDC_DUB_HPF_KNOB:     return L"Ton";
    case IDC_DUB_LPF_KNOB:     return L"Dif";
    default:                   return L"";
    }
}

static void InvalidateEffectKnob(HWND knob) {
    if (knob) InvalidateRect(knob, nullptr, TRUE);
}

static HWND GetKnobHwnd(int controlId) {
    switch (controlId) {
    case IDC_MIX_KNOB:         return g_app.mixKnob;
    case IDC_REVERB_KNOB:      return g_app.reverbKnob;
    case IDC_REVERB_DIST_KNOB: return g_app.reverbDistKnob;
    case IDC_REVERB_TIME_KNOB: return g_app.reverbTimeKnob;
    case IDC_DELAY_KNOB:       return g_app.delayKnob;
    case IDC_DELAY_FDB_KNOB:   return g_app.delayFeedbackKnob;
    case IDC_DELAY_SPD_KNOB:   return g_app.delaySpeedKnob;
    case IDC_DUB_HPF_KNOB:     return g_app.dubHighpassKnob;
    case IDC_DUB_LPF_KNOB:     return g_app.dubLowpassKnob;
    default:                   return nullptr;
    }
}

static void SetKnobValue(int controlId, int value, bool fromUser) {
    int* pValue = GetKnobValuePtr(controlId);
    if (!pValue) return;

    value = std::clamp(value, 0, 100);
    if (*pValue == value) {
        InvalidateEffectKnob(GetKnobHwnd(controlId));
        return;
    }

    *pValue = value;
    InvalidateEffectKnob(GetKnobHwnd(controlId));

    if (fromUser) {
        if (g_app.activePreset != PRESET_NONE || g_app.activeCustomSlot != 0) {
            g_app.activePreset = PRESET_NONE;
            g_app.activeCustomSlot = 0;
            UpdatePresetButtonsVisual();
            UpdateCustomButtonsVisual();
        }

        if (controlId == IDC_MIX_KNOB) {
            RefreshEffectsMixTarget(true);
        } else {
            ScheduleApplyWithDelay(g_app.knobTracking ? 220 : 120);
        }
    }
}


static void ApplyFlatNeutralState() {
    g_app.activePreset = PRESET_NONE;
    g_app.activeCustomSlot = 0;
    for (int i = 0; i < 31; ++i) g_app.gains[i] = 0;
}

static void SaveCurrentToCustomSlot(int slotIndex) {
    if (slotIndex < 0 || slotIndex >= 2) return;
    g_app.customSaved[slotIndex] = true;
    g_app.customEqEnabled[slotIndex] = g_app.eqEnabled;
    g_app.customEffectsMixEnabled[slotIndex] = g_app.effectsMixEnabled;
    g_app.customEffectsMixAmount[slotIndex] = g_app.effectsMixAmount;
    g_app.customReverbAmount[slotIndex] = g_app.reverbAmount;
    g_app.customReverbDistance[slotIndex] = g_app.reverbDistance;
    g_app.customReverbTime[slotIndex] = g_app.reverbTime;
    g_app.customDelayAmount[slotIndex] = g_app.delayAmount;
    g_app.customDelayFeedback[slotIndex] = g_app.delayFeedback;
    g_app.customDelaySpeed[slotIndex] = g_app.delaySpeed;
    g_app.customDubHighpass[slotIndex] = g_app.dubHighpass;
    g_app.customDubLowpass[slotIndex] = g_app.dubLowpass;
    for (int i = 0; i < 31; ++i) g_app.customGains[slotIndex][i] = g_app.gains[i];
    UpdateCustomButtonsVisual();
    SaveStateToJson();
    SetStatus(slotIndex == 0 ? L"Custom 1 に現在のEQ+Mix設定を保存しました。" : L"Custom 2 に現在のEQ+Mix設定を保存しました。");
}

static void ClearCustomSlot(int slotIndex) {
    if (slotIndex < 0 || slotIndex >= 2) return;
    const bool wasActive = (g_app.activeCustomSlot == slotIndex + 1);
    g_app.customSaved[slotIndex] = false;
    g_app.customEqEnabled[slotIndex] = true;
    g_app.customEffectsMixEnabled[slotIndex] = true;
    g_app.customEffectsMixAmount[slotIndex] = 0;
    g_app.customReverbAmount[slotIndex] = 0;
    g_app.customReverbDistance[slotIndex] = 0;
    g_app.customReverbTime[slotIndex] = 0;
    g_app.customDelayAmount[slotIndex] = 0;
    g_app.customDelayFeedback[slotIndex] = 0;
    g_app.customDelaySpeed[slotIndex] = 0;
    g_app.customDubHighpass[slotIndex] = 0;
    g_app.customDubLowpass[slotIndex] = 0;
    for (int i = 0; i < 31; ++i) g_app.customGains[slotIndex][i] = 0;
    if (wasActive) {
        ApplyFlatNeutralState();
        SyncSlidersFromState();
        UpdatePresetButtonsVisual();
        UpdateCustomButtonsVisual();
        ScheduleApply();
        SaveStateToJson();
        SetStatus(slotIndex == 0 ? L"Custom 1 をクリアしてフラット状態に戻しました。" : L"Custom 2 をクリアしてフラット状態に戻しました。");
        return;
    }
    UpdatePresetButtonsVisual();
    UpdateCustomButtonsVisual();
    SaveStateToJson();
    SetStatus(slotIndex == 0 ? L"Custom 1 をクリアしました。" : L"Custom 2 をクリアしました。");
}

static void ActivateCustomSlot(int slotIndex) {
    if (slotIndex < 0 || slotIndex >= 2) return;
    if (!g_app.customSaved[slotIndex]) {
        UpdatePresetButtonsVisual();
        UpdateCustomButtonsVisual();
        SetStatus(slotIndex == 0 ? L"Custom 1 はまだ保存されていません。先に Save を押してください。" : L"Custom 2 はまだ保存されていません。先に Save を押してください。");
        return;
    }

    if (g_app.activeCustomSlot == slotIndex + 1) {
        ApplyFlatNeutralState();
        SyncSlidersFromState();
        UpdatePresetButtonsVisual();
        UpdateCustomButtonsVisual();
        ScheduleApply();
        SaveStateToJson();
        SetStatus(slotIndex == 0 ? L"Custom 1 を解除してフラット状態に戻しました。" : L"Custom 2 を解除してフラット状態に戻しました。");
        return;
    }

    g_app.activePreset = PRESET_NONE;
    g_app.activeCustomSlot = slotIndex + 1;
    g_app.eqEnabled = g_app.customEqEnabled[slotIndex];
    g_app.effectsMixEnabled = g_app.customEffectsMixEnabled[slotIndex];
    g_app.effectsMixAmount = g_app.customEffectsMixAmount[slotIndex];
    g_app.reverbAmount = g_app.customReverbAmount[slotIndex];
    g_app.reverbDistance = g_app.customReverbDistance[slotIndex];
    g_app.reverbTime = g_app.customReverbTime[slotIndex];
    g_app.delayAmount = g_app.customDelayAmount[slotIndex];
    g_app.delayFeedback = g_app.customDelayFeedback[slotIndex];
    g_app.delaySpeed = g_app.customDelaySpeed[slotIndex];
    g_app.dubHighpass = g_app.customDubHighpass[slotIndex];
    g_app.dubLowpass = g_app.customDubLowpass[slotIndex];
    g_app.effectsMixCurrent = g_app.effectsMixEnabled ? (double)g_app.effectsMixAmount / 100.0 : 0.0;
    g_app.effectsMixTarget = g_app.effectsMixCurrent;
    g_app.effectsMixFading = false;
    for (int i = 0; i < 31; ++i) g_app.gains[i] = g_app.customGains[slotIndex][i];
    SyncSlidersFromState();
    UpdatePresetButtonsVisual();
    UpdateCustomButtonsVisual();
    ScheduleApply();
    SaveStateToJson();
    SetStatus(slotIndex == 0 ? L"Custom 1 を反映しました。" : L"Custom 2 を反映しました。");
}

static void ResetCurrentState() {
    g_app.eqEnabled = true;
    for (int i = 0; i < 31; ++i) g_app.gains[i] = 0;
    g_app.activeCustomSlot = 0;
    g_app.activePreset = PRESET_NONE;
    g_app.reverbAmount = 0;
    g_app.reverbDistance = 0;
    g_app.reverbTime = 0;
    g_app.delayAmount = 0;
    g_app.delayFeedback = 0;
    g_app.delaySpeed = 0;
    g_app.dubHighpass = 0;
    g_app.dubLowpass = 0;
    g_app.effectsMixEnabled = true;
    g_app.effectsMixAmount = 0;
    g_app.effectsMixCurrent = 0.0;
    g_app.effectsMixTarget = 0.0;
    g_app.effectsMixFading = false;
    SyncSlidersFromState();
    UpdatePresetButtonsVisual();
    UpdateCustomButtonsVisual();
    ScheduleApply();
    SaveStateToJson();
    SetStatus(L"現在のEQ / FX状態をリセットしました。保存済みCustomスロットはそのまま維持されます。");
}

static bool ConnectPipe(HANDLE& hPipe) {
    hPipe = INVALID_HANDLE_VALUE;
    if (!WaitNamedPipeW(g_app.pipePath.c_str(), 250)) {
        hPipe = CreateFileW(g_app.pipePath.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        return hPipe != INVALID_HANDLE_VALUE;
    }
    hPipe = CreateFileW(g_app.pipePath.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    return hPipe != INVALID_HANDLE_VALUE;
}

static bool ProbePipeAlive() {
    HANDLE hPipe = INVALID_HANDLE_VALUE;
    if (!ConnectPipe(hPipe)) {
        return false;
    }
    CloseHandle(hPipe);
    return true;
}

static void CheckPipeAndCloseIfNeeded(HWND hwnd) {
    if (!g_app.autoCloseOnPipeLoss) return;

    if (ProbePipeAlive()) {
        g_app.pipeMissCount = 0;
        return;
    }

    ++g_app.pipeMissCount;
    if (g_app.pipeMissCount >= 3) {
        KillTimer(hwnd, TIMER_APPLY);
        KillTimer(hwnd, TIMER_ANALYZER);
        KillTimer(hwnd, TIMER_PIPE_WATCH);
        SetStatus(L"mpv IPC pipe が見つからなくなったため、このEQツールも終了します。");
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
    }
}

static bool SendJson(const std::string& json, std::string* reply = nullptr) {
    HANDLE hPipe = INVALID_HANDLE_VALUE;
    if (!ConnectPipe(hPipe)) {
        SetStatus(L"mpv IPC pipe に接続できません。mpv.conf の input-ipc-server を確認してください。");
        return false;
    }

    DWORD written = 0;
    std::string payload = json + "\n";
    if (!WriteFile(hPipe, payload.data(), (DWORD)payload.size(), &written, nullptr)) {
        CloseHandle(hPipe);
        SetStatus(L"mpv IPC への送信に失敗しました。");
        return false;
    }

    std::string result;
    char buf[4096];
    for (;;) {
        DWORD read = 0;
        if (!ReadFile(hPipe, buf, sizeof(buf), &read, nullptr)) break;
        if (read == 0) break;
        result.append(buf, buf + read);
        if (result.find('\n') != std::string::npos) break;
    }

    CloseHandle(hPipe);
    if (reply) {
        size_t nl = result.find('\n');
        if (nl != std::string::npos) result.resize(nl);
        *reply = result;
    }
    return true;
}

static std::string MakeCommand3(const std::string& a, const std::string& b, const std::string& c) {
    return "{\"command\":[\"" + JsonEscape(a) + "\",\"" + JsonEscape(b) + "\",\"" + JsonEscape(c) + "\"]}";
}

static bool JsonSuccess(const std::string& reply) {
    return reply.find("\"error\":\"success\"") != std::string::npos;
}

static int GainToSliderPos(int gain) {
    gain = std::clamp(gain, kGainMin, kGainMax);
    return kGainMax - gain;
}

static int SliderPosToGain(int pos) {
    pos = std::clamp(pos, 0, kGainMax - kGainMin);
    return kGainMax - pos;
}

static std::wstring FormatGainShort(int gain) {
    std::wstringstream ss;
    if (gain >= 0) ss << L'+';
    ss << gain;
    return ss.str();
}

static BandsLayout GetBandsLayout() {
    RECT rc{};
    GetClientRect(g_app.hwnd, &rc);
    BandsLayout l;
    l.sliderTop = 496;
    l.sliderW = 18;
    l.sliderH = 174;
    l.gap = 12;
    l.totalW = 31 * l.sliderW + 30 * l.gap;
    l.startX = (rc.right - l.totalW) / 2;
    l.meterBarW = 14;
    return l;
}

static void UpdateValueLabels() {
    for (int i = 0; i < 31; ++i) {
        std::wstring t = FormatGainShort(g_app.gains[i]);
        SetWindowTextW(g_app.valueLabels[i], t.c_str());
    }
    if (g_app.hwnd) {
        InvalidateRect(g_app.hwnd, &g_app.meterRect, FALSE);
    }
}

static void ApplyPresetFlat() {
    for (int i = 0; i < 31; ++i) g_app.gains[i] = 0;
}

static void ApplyPresetVShape() {
    const int vals[31] = {
        6, 6, 6, 5, 5, 4, 4, 3, 2, 1, 0, -1, -2, -2, -3, -3,
        -3, -3, -2, -2, -1, 0, 1, 2, 3, 4, 4, 5, 5, 6, 6
    };
    std::copy(std::begin(vals), std::end(vals), g_app.gains);
}

static void ApplyPresetRadio() {
    const int vals[31] = {
        -16, -15, -14, -13, -12, -10, -8, -6, -4, -2, 0, 2, 3, 4, 5, 5,
          4,   3,   2,   1,   0, -1, -3, -5, -7, -9, -12, -15, -17, -19, -20
    };
    std::copy(std::begin(vals), std::end(vals), g_app.gains);
}

static void ApplyPresetBass() {
    const int vals[31] = {
        8, 8, 7, 7, 6, 6, 5, 4, 3, 2, 1, 0, 0, 0, 0, 0,
        -1, -1, -1, -1, -1, -1, -2, -2, -2, -2, -2, -3, -3, -3, -4
    };
    std::copy(std::begin(vals), std::end(vals), g_app.gains);
}

static void ApplyPresetVocal() {
    const int vals[31] = {
        -4, -4, -4, -3, -3, -2, -2, -1, -1, 0, 1, 1, 2, 2, 3, 3,
        4, 4, 4, 4, 3, 3, 2, 1, 0, 0, 0, -1, -2, -3, -4
    };
    std::copy(std::begin(vals), std::end(vals), g_app.gains);
}

static void ApplyPresetTreble() {
    const int vals[31] = {
        -4, -4, -4, -3, -3, -3, -2, -2, -1, -1, 0, 0, 0, 0, 0, 0,
        1, 1, 1, 2, 2, 3, 3, 4, 5, 5, 6, 6, 7, 7, 8
    };
    std::copy(std::begin(vals), std::end(vals), g_app.gains);
}

static void ApplyPresetById(int presetId) {
    switch (presetId) {
    case PRESET_FLAT:   ApplyPresetFlat(); break;
    case PRESET_VSHAPE: ApplyPresetVShape(); break;
    case PRESET_LOFI:   ApplyPresetRadio(); break;
    case PRESET_BASS:   ApplyPresetBass(); break;
    case PRESET_VOCAL:  ApplyPresetVocal(); break;
    case PRESET_TREBLE: ApplyPresetTreble(); break;
    default: break;
    }
}

static void ActivatePresetButton(int presetId) {
    if (presetId < PRESET_FLAT || presetId > PRESET_TREBLE) return;

    if (g_app.activePreset == presetId) {
        ApplyFlatNeutralState();
        SyncSlidersFromState();
        UpdatePresetButtonsVisual();
        UpdateCustomButtonsVisual();
        ScheduleApply();
        SaveStateToJson();
        switch (presetId) {
        case PRESET_FLAT:   SetStatus(L"Flat を解除しました。"); break;
        case PRESET_VSHAPE: SetStatus(L"V-Shape を解除してフラット状態に戻しました。"); break;
        case PRESET_LOFI:   SetStatus(L"Lo-fi を解除してフラット状態に戻しました。"); break;
        case PRESET_BASS:   SetStatus(L"Bass+ を解除してフラット状態に戻しました。"); break;
        case PRESET_VOCAL:  SetStatus(L"Vocal を解除してフラット状態に戻しました。"); break;
        case PRESET_TREBLE: SetStatus(L"Treble+ を解除してフラット状態に戻しました。"); break;
        }
        return;
    }

    g_app.activePreset = presetId;
    g_app.activeCustomSlot = 0;
    ApplyPresetById(presetId);
    SyncSlidersFromState();
    UpdatePresetButtonsVisual();
    UpdateCustomButtonsVisual();
    ScheduleApply();
    SaveStateToJson();
    switch (presetId) {
    case PRESET_FLAT:   SetStatus(L"Flat を反映しました。EQは原音周波数のままです。"); break;
    case PRESET_VSHAPE: SetStatus(L"V-Shape を反映しました。低音と高音を持ち上げたメリハリ重視です。"); break;
    case PRESET_LOFI:   SetStatus(L"Lo-fi を反映しました。"); break;
    case PRESET_BASS:   SetStatus(L"Bass+ を反映しました。"); break;
    case PRESET_VOCAL:  SetStatus(L"Vocal を反映しました。"); break;
    case PRESET_TREBLE: SetStatus(L"Treble+ を反映しました。"); break;
    }
}

static std::string BuildEqualizerGraph() {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(1);
    for (int i = 0; i < 31; ++i) {
        if (i) oss << ",";
        oss << "equalizer=f=" << kBands[i].freq << ":t=o:w=0.333:g=" << g_app.gains[i];
    }
    return oss.str();
}

static bool HasActiveEffectSettings() {
    return g_app.reverbAmount > 0 ||
        g_app.reverbDistance > 0 ||
        g_app.reverbTime > 0 ||
        g_app.delayAmount > 0 ||
        g_app.delayFeedback > 0 ||
        g_app.delaySpeed > 0 ||
        g_app.dubHighpass > 0 ||
        g_app.dubLowpass > 0;
}

static std::string BuildHighQualityResampleFilter() {
    const int sr = std::clamp((int)std::llround(g_app.sampleRate > 1000.0 ? g_app.sampleRate : 48000.0), 8000, 384000);
    std::ostringstream oss;
    if (g_app.hqResamplerMode == 1) {
        oss << "aresample=" << sr << ":resampler=soxr:precision=28:cheby=1:osf=fltp:flags=res";
    } else {
        oss << "aresample=" << sr << ":osf=fltp:tsf=fltp:filter_size=64:phase_shift=24:linear_interp=0:exact_rational=1:cutoff=0.98:flags=res";
    }
    return oss.str();
}

static double GetDesiredEffectsMix() {
    return g_app.effectsMixEnabled ? std::clamp((double)g_app.effectsMixAmount / 100.0, 0.0, 1.0) : 0.0;
}

static void SetEffectsMixEnabled(bool enabled, bool animate) {
    g_app.effectsMixEnabled = enabled;
    g_app.effectsMixTarget = GetDesiredEffectsMix();
    g_app.effectsMixCurrent = g_app.effectsMixTarget;
    g_app.effectsMixFading = false;

    if (!animate) {
        ScheduleApply();
        return;
    }

    // Mix側は短間隔の remove/add を連打するとクリックノイズが出やすいので、
    // ON/OFF時も段階フェードではなく、少しだけ間を置いた1回の再適用に抑える。
    ScheduleApplyWithDelay(90);
}

static void RefreshEffectsMixTarget(bool animate) {
    g_app.effectsMixTarget = GetDesiredEffectsMix();
    g_app.effectsMixCurrent = g_app.effectsMixTarget;
    g_app.effectsMixFading = false;

    if (!animate) {
        ScheduleApply();
        return;
    }

    // Mixノブ追従中は再適用を少し間引いて、ブツッというクリックを減らす。
    ScheduleApplyWithDelay(g_app.knobTracking ? 150 : 80);
}

static void AdvanceEffectsMixFadeStep() {
    // 以前はここでMixフェード用に短周期でフィルタを掛け直していたが、
    // mpv側の af remove/add 連打でクリックノイズが出やすいため現在は使用しない。
    g_app.effectsMixCurrent = g_app.effectsMixTarget;
    g_app.effectsMixFading = false;
}

static std::string BuildFilterGraph() {
    const bool hasEq = g_app.eqEnabled && g_app.activePreset != PRESET_FLAT;
    const bool hasWetFx = HasActiveEffectSettings();
    const bool wantWetPath = hasWetFx && g_app.effectsMixEnabled && g_app.effectsMixAmount > 0;

    if (!hasEq && !wantWetPath) {
        return std::string();
    }

    std::ostringstream oss;
    bool hasBase = false;
    if (hasEq) {
        oss << BuildEqualizerGraph();
        hasBase = true;
    }
    if (wantWetPath) {
        if (hasBase) oss << ",";
        oss << BuildHighQualityResampleFilter();
        hasBase = true;
    }

    const double wetMix = std::clamp(g_app.effectsMixCurrent, 0.0, 1.0);
    if (!wantWetPath) {
        return oss.str();
    }

    const double rev = std::clamp(g_app.reverbAmount / 100.0, 0.0, 1.0);
    const double room = std::clamp(g_app.reverbDistance / 100.0, 0.0, 1.0);
    const double tail = std::clamp(g_app.reverbTime / 100.0, 0.0, 1.0);
    const double echo = std::clamp(g_app.delayAmount / 100.0, 0.0, 1.0);
    const double repeat = std::clamp(g_app.delayFeedback / 100.0, 0.0, 1.0);
    const double interval = std::clamp(g_app.delaySpeed / 100.0, 0.0, 1.0);
    const double tone = std::clamp(g_app.dubHighpass / 100.0, 0.0, 1.0);
    const double diffusion = std::clamp(g_app.dubLowpass / 100.0, 0.0, 1.0);

    // Mix=0付近でも経路を急に消さず、wet量だけを0へ寄せてクリックを減らす。
    const double dryWeight = std::clamp(1.0 - wetMix * 0.88, 0.12, 1.0);
    const double wetWeight = std::clamp(wetMix * 2.10, 0.0, 2.10);

    oss << std::fixed << std::setprecision(3);
    oss << ",asplit[dry][wet];";
    oss << "[dry]volume=" << dryWeight << "[dryv];";
    oss << "[wet]";

    const int wetHP = std::clamp((int)std::llround(28.0 + tone * 260.0), 25, 360);
    const int wetLP = std::clamp((int)std::llround(20000.0 - std::pow(tone, 1.15) * 15200.0), 4200, 20000);
    oss << "highpass=f=" << wetHP << ":p=2,lowpass=f=" << wetLP << ":p=2";

    if (echo > 0.0001 || repeat > 0.0001 || interval > 0.0001) {
        const int baseMs = std::clamp((int)std::llround(82.0 + (1.0 - interval) * 560.0), 58, 680);
        const int ms1 = baseMs;
        const int ms2 = std::clamp((int)std::llround(ms1 * (1.82 + repeat * 0.18)), ms1 + 36, 1750);
        const int ms3 = std::clamp((int)std::llround(ms2 * (1.52 + repeat * 0.12)), ms2 + 52, 2800);
        const int ms4 = std::clamp((int)std::llround(ms3 * (1.34 + repeat * 0.10)), ms3 + 66, 4200);

        const double echoSeed = std::clamp(0.18 + echo * 0.34 + repeat * 0.22, 0.10, 0.86);
        const double echoKeep = std::clamp(0.58 + repeat * 0.32, 0.52, 0.92);
        const double ec1 = std::clamp(echoSeed, 0.10, 0.86);
        const double ec2 = std::clamp(ec1 * echoKeep, 0.08, 0.82);
        const double ec3 = std::clamp(ec2 * echoKeep, 0.06, 0.76);
        const double ec4 = std::clamp(ec3 * echoKeep, 0.05, 0.68);
        const double echoOut = std::clamp(0.34 + echo * 0.30, 0.20, 0.82);

        oss << ",aecho=0.950:" << echoOut << ":"
            << ms1 << "|" << ms2 << "|" << ms3 << "|" << ms4
            << ":" << ec1 << "|" << ec2 << "|" << ec3 << "|" << ec4;
    }

    if (rev > 0.0001 || room > 0.0001 || tail > 0.0001 || diffusion > 0.0001) {
        const int er1 = std::clamp((int)std::llround(16.0 + room * 54.0), 10, 84);
        const int er2 = std::clamp((int)std::llround(er1 + 12.0 + diffusion * 14.0), er1 + 5, 120);
        const int er3 = std::clamp((int)std::llround(er2 + 18.0 + diffusion * 18.0), er2 + 8, 168);
        const int er4 = std::clamp((int)std::llround(er3 + 24.0 + diffusion * 22.0), er3 + 10, 236);

        const double erBase = std::clamp(0.14 + rev * 0.24 + diffusion * 0.12 + echo * 0.04, 0.10, 0.84);
        const double er1d = std::clamp(erBase + 0.18, 0.12, 0.88);
        const double er2d = std::clamp(erBase + 0.10, 0.10, 0.82);
        const double er3d = std::clamp(erBase + 0.02, 0.08, 0.76);
        const double er4d = std::clamp(erBase - 0.06, 0.06, 0.70);

        const int tail1 = std::clamp((int)std::llround(96.0 + room * 180.0), 80, 300);
        const int tail2 = std::clamp((int)std::llround(tail1 + 62.0 + diffusion * 34.0), tail1 + 24, 460);
        const int tail3 = std::clamp((int)std::llround(tail2 + 96.0 + diffusion * 48.0), tail2 + 32, 720);
        const int tail4 = std::clamp((int)std::llround(tail3 + 142.0 + diffusion * 66.0), tail3 + 42, 1060);
        const int tail5 = std::clamp((int)std::llround(tail4 + 208.0 + diffusion * 88.0), tail4 + 56, 1500);
        const int tail6 = std::clamp((int)std::llround(tail5 + 286.0 + diffusion * 118.0), tail5 + 72, 2100);

        const double tailSeed = std::clamp(0.24 + rev * 0.28 + tail * 0.42 + room * 0.12 + echo * 0.06, 0.16, 0.94);
        const double tailKeep = std::clamp(0.76 + tail * 0.16 + diffusion * 0.10, 0.72, 0.96);
        const double td1 = std::clamp(tailSeed, 0.16, 0.94);
        const double td2 = std::clamp(td1 * tailKeep, 0.14, 0.90);
        const double td3 = std::clamp(td2 * tailKeep, 0.12, 0.86);
        const double td4 = std::clamp(td3 * tailKeep, 0.10, 0.82);
        const double td5 = std::clamp(td4 * tailKeep, 0.09, 0.78);
        const double td6 = std::clamp(td5 * tailKeep, 0.08, 0.74);

        oss << ",aecho=0.860:0.520:"
            << er1 << "|" << er2 << "|" << er3 << "|" << er4
            << ":" << er1d << "|" << er2d << "|" << er3d << "|" << er4d;

        oss << ",aecho=0.830:0.580:"
            << tail1 << "|" << tail2 << "|" << tail3 << "|" << tail4 << "|" << tail5 << "|" << tail6
            << ":" << td1 << "|" << td2 << "|" << td3 << "|" << td4 << "|" << td5 << "|" << td6;
    }

    oss << ",alimiter=limit=0.970,volume=" << wetWeight << "[wetv];";
    oss << "[dryv][wetv]amix=inputs=2:normalize=0,alimiter=limit=0.970";

    return oss.str();
}

static bool ApplyToMpv() {
    std::string reply;

    auto removeEq31IfTracked = [&]() -> bool {
        if (!g_app.eq31FilterApplied) {
            return true;
        }
        if (!SendJson(MakeCommand3("af", "remove", "@eq31"), &reply)) {
            return false;
        }
        g_app.eq31FilterApplied = false;
        return true;
    };

    const bool hasEqPath = g_app.eqEnabled && g_app.activePreset != PRESET_FLAT;
    const std::string graph = BuildFilterGraph();
    if (graph.empty()) {
        if (!removeEq31IfTracked()) {
            SetStatus(L"EQ / FX フィルタの解除に失敗しました。");
            return false;
        }
        if (!g_app.eqEnabled) {
            SetStatus(L"EQ を OFF にしました。Mix / FX は必要時のみ個別に有効になります。");
        } else if (g_app.activePreset == PRESET_FLAT) {
            SetStatus(L"Flat を反映しました。EQは原音周波数のままです。");
        } else {
            SetStatus(L"EQ / FX を OFF にしました。GUI のアナライザー表示は継続します。");
        }
        return true;
    }

    if (!removeEq31IfTracked()) {
        SetStatus(L"既存EQフィルタの解除に失敗しました。");
        return false;
    }

    auto tryApply = [&](const std::string& graph) -> bool {
        std::string spec = "@eq31:lavfi=[" + graph + "]";
        if (!(SendJson(MakeCommand3("af", "add", spec), &reply) && JsonSuccess(reply))) {
            return false;
        }
        g_app.eq31FilterApplied = true;
        return true;
    };

    if (tryApply(graph)) {
        SetStatus(hasEqPath
            ? L"31-band EQ / Reverb / Echo を mpv に反映しました。アナライザーは既定出力全体を表示します。"
            : L"Mix / FX のみを mpv に反映しました。EQは原音のままです。");
        return true;
    }

    if (g_app.hqResamplerMode == 1) {
        g_app.hqResamplerMode = 0;
        if (tryApply(BuildFilterGraph())) {
            SetStatus(hasEqPath
                ? L"31-band EQ / Reverb / Echo を mpv に反映しました。高精度リサンプラは互換性の高いHQ設定へ自動フォールバックしました。"
                : L"Mix / FX のみを mpv に反映しました。高精度リサンプラは互換性の高いHQ設定へ自動フォールバックしました。");
            return true;
        }
    }

    SetStatus(L"EQ / FX フィルタの適用に失敗しました。mpv が再生中か確認してください。");
    return false;
}

static void ScheduleApplyWithDelay(UINT delayMs) {
    if (!g_app.hwnd) return;
    KillTimer(g_app.hwnd, TIMER_APPLY);
    SetTimer(g_app.hwnd, TIMER_APPLY, (delayMs < 15 ? 15 : delayMs), nullptr);
}

static void ScheduleApply() {
    ScheduleApplyWithDelay(90);
}

static void ReleaseAudioAnalyzer() {
    if (g_app.audioClient) {
        g_app.audioClient->Stop();
    }
    SafeRelease(g_app.captureClient);
    SafeRelease(g_app.audioClient);
    SafeRelease(g_app.device);
    SafeRelease(g_app.deviceEnum);
    void* p = g_app.mixFormat;
    SafeCoTaskMemFree(p);
    g_app.mixFormat = nullptr;
    g_app.sampleHistory.clear();
}

static bool InitAudioAnalyzer() {
    ReleaseAudioAnalyzer();

    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator), (void**)&g_app.deviceEnum);
    if (FAILED(hr) || !g_app.deviceEnum) {
        return false;
    }

    hr = g_app.deviceEnum->GetDefaultAudioEndpoint(eRender, eConsole, &g_app.device);
    if (FAILED(hr) || !g_app.device) {
        return false;
    }

    hr = g_app.device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&g_app.audioClient);
    if (FAILED(hr) || !g_app.audioClient) {
        return false;
    }

    hr = g_app.audioClient->GetMixFormat(&g_app.mixFormat);
    if (FAILED(hr) || !g_app.mixFormat) {
        return false;
    }

    g_app.sampleRate = (g_app.mixFormat->nSamplesPerSec > 0) ? (double)g_app.mixFormat->nSamplesPerSec : 48000.0;

    REFERENCE_TIME bufferDuration = 10000000;
    hr = g_app.audioClient->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_LOOPBACK,
        bufferDuration,
        0,
        g_app.mixFormat,
        nullptr);
    if (FAILED(hr)) {
        return false;
    }

    hr = g_app.audioClient->GetService(__uuidof(IAudioCaptureClient), (void**)&g_app.captureClient);
    if (FAILED(hr) || !g_app.captureClient) {
        return false;
    }

    hr = g_app.audioClient->Start();
    if (FAILED(hr)) {
        return false;
    }

    return true;
}

static bool ResolveMixFormat(bool& isFloat, bool& isPCM, WORD& bitsPerSample, WORD& channels) {
    isFloat = false;
    isPCM = false;
    bitsPerSample = 0;
    channels = 0;
    if (!g_app.mixFormat) return false;

    channels = g_app.mixFormat->nChannels;
    bitsPerSample = g_app.mixFormat->wBitsPerSample;

    if (g_app.mixFormat->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        isFloat = true;
        return true;
    }
    if (g_app.mixFormat->wFormatTag == WAVE_FORMAT_PCM) {
        isPCM = true;
        return true;
    }
    if (g_app.mixFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        WAVEFORMATEXTENSIBLE* ex = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(g_app.mixFormat);
        if (IsEqualGUID(ex->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)) {
            isFloat = true;
            bitsPerSample = ex->Samples.wValidBitsPerSample ? ex->Samples.wValidBitsPerSample : g_app.mixFormat->wBitsPerSample;
            return true;
        }
        if (IsEqualGUID(ex->SubFormat, KSDATAFORMAT_SUBTYPE_PCM)) {
            isPCM = true;
            bitsPerSample = ex->Samples.wValidBitsPerSample ? ex->Samples.wValidBitsPerSample : g_app.mixFormat->wBitsPerSample;
            return true;
        }
    }
    return false;
}

static void AppendSamplesFromBuffer(const BYTE* data, UINT32 frames, DWORD flags) {
    bool isFloat = false, isPCM = false;
    WORD bits = 0, channels = 0;
    if (!ResolveMixFormat(isFloat, isPCM, bits, channels) || channels == 0) {
        return;
    }

    if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
        g_app.sampleHistory.insert(g_app.sampleHistory.end(), frames, 0.0f);
    } else if (isFloat && bits == 32) {
        const float* src = reinterpret_cast<const float*>(data);
        for (UINT32 i = 0; i < frames; ++i) {
            double mono = 0.0;
            for (WORD ch = 0; ch < channels; ++ch) {
                mono += src[(size_t)i * channels + ch];
            }
            g_app.sampleHistory.push_back((float)(mono / channels));
        }
    } else if (isPCM && bits == 16) {
        const int16_t* src = reinterpret_cast<const int16_t*>(data);
        for (UINT32 i = 0; i < frames; ++i) {
            double mono = 0.0;
            for (WORD ch = 0; ch < channels; ++ch) {
                mono += (double)src[(size_t)i * channels + ch] / 32768.0;
            }
            g_app.sampleHistory.push_back((float)(mono / channels));
        }
    } else if (isPCM && bits == 24) {
        const BYTE* src = data;
        const int bytesPerFrame = g_app.mixFormat->nBlockAlign;
        const int bytesPerSample = bytesPerFrame / channels;
        for (UINT32 i = 0; i < frames; ++i) {
            double mono = 0.0;
            for (WORD ch = 0; ch < channels; ++ch) {
                const BYTE* p = src + (size_t)i * bytesPerFrame + ch * bytesPerSample;
                int32_t v = (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16));
                if (v & 0x800000) v |= ~0xFFFFFF;
                mono += (double)v / 8388608.0;
            }
            g_app.sampleHistory.push_back((float)(mono / channels));
        }
    } else if (isPCM && bits == 32) {
        const int32_t* src = reinterpret_cast<const int32_t*>(data);
        for (UINT32 i = 0; i < frames; ++i) {
            double mono = 0.0;
            for (WORD ch = 0; ch < channels; ++ch) {
                mono += (double)src[(size_t)i * channels + ch] / 2147483648.0;
            }
            g_app.sampleHistory.push_back((float)(mono / channels));
        }
    } else {
        return;
    }

    if ((int)g_app.sampleHistory.size() > kHistoryLimit) {
        int excess = (int)g_app.sampleHistory.size() - kHistoryLimit;
        g_app.sampleHistory.erase(g_app.sampleHistory.begin(), g_app.sampleHistory.begin() + excess);
    }
}

static void DecayBandLevels() {
    for (int i = 0; i < 31; ++i) {
        g_app.bandLevel[i] = std::max(0.0f, g_app.bandLevel[i] - 0.025f);
        if (g_app.bandPeakTicks[i] > 0) {
            --g_app.bandPeakTicks[i];
        } else {
            g_app.bandPeak[i] = std::max(0.0f, g_app.bandPeak[i] - 0.03f);
        }
        if (g_app.bandPeak[i] < g_app.bandLevel[i]) {
            g_app.bandPeak[i] = g_app.bandLevel[i];
            g_app.bandPeakTicks[i] = 10;
        }
    }
}

static void AnalyzeBands() {
    int available = (int)g_app.sampleHistory.size();
    if (available < 512 || g_app.sampleRate < 1000.0) {
        DecayBandLevels();
        return;
    }

    int n = std::min(kAnalysisWindow, available);
    int start = available - n;

    std::vector<double> windowed(n);
    double energy = 0.0;
    for (int i = 0; i < n; ++i) {
        double w = 0.5 - 0.5 * std::cos((2.0 * kPi * i) / (n - 1));
        double s = g_app.sampleHistory[start + i];
        windowed[i] = s * w;
        energy += s * s;
    }

    const double rms = std::sqrt(energy / n);
    if (rms < 0.00035) {
        for (int bi = 0; bi < 31; ++bi) {
            g_app.bandLevel[bi] *= 0.72f;
            if (g_app.bandLevel[bi] < 0.004f) g_app.bandLevel[bi] = 0.0f;

            if (g_app.bandPeakTicks[bi] > 0) {
                --g_app.bandPeakTicks[bi];
            } else {
                g_app.bandPeak[bi] *= 0.68f;
            }
            if (g_app.bandPeak[bi] < 0.004f) g_app.bandPeak[bi] = 0.0f;
        }
        return;
    }

    for (int bi = 0; bi < 31; ++bi) {
        double freq = kBands[bi].freq;
        double omega = 2.0 * kPi * freq / g_app.sampleRate;
        double coeff = 2.0 * std::cos(omega);
        double s0 = 0.0, s1 = 0.0, s2 = 0.0;

        for (int i = 0; i < n; ++i) {
            s0 = windowed[i] + coeff * s1 - s2;
            s2 = s1;
            s1 = s0;
        }

        double power = s1 * s1 + s2 * s2 - coeff * s1 * s2;
        if (power < 0.0) power = 0.0;
        double magnitude = std::sqrt(power) / (n * 0.5);
        double db = 20.0 * std::log10(magnitude + 1e-9);

        float target = (float)std::clamp((db + 78.0) / 58.0, 0.0, 1.0);
        float prev = g_app.bandLevel[bi];
        float alpha = (target > prev) ? 0.55f : 0.18f;
        float level = prev + (target - prev) * alpha;
        level = std::clamp(level, 0.0f, 1.0f);
        g_app.bandLevel[bi] = level;

        if (level >= g_app.bandPeak[bi]) {
            g_app.bandPeak[bi] = level;
            g_app.bandPeakTicks[bi] = 12;
        } else if (g_app.bandPeakTicks[bi] > 0) {
            --g_app.bandPeakTicks[bi];
        } else {
            g_app.bandPeak[bi] = std::max(0.0f, g_app.bandPeak[bi] - 0.028f);
        }
    }
}

static void PollAudioAnalyzer() {
    if (!g_app.captureClient || !g_app.audioClient) {
        InitAudioAnalyzer();
    }

    if (!g_app.captureClient) {
        DecayBandLevels();
        if (g_app.hwnd) InvalidateRect(g_app.hwnd, &g_app.meterRect, FALSE);
        return;
    }

    HRESULT hr = S_OK;
    for (;;) {
        UINT32 packetFrames = 0;
        hr = g_app.captureClient->GetNextPacketSize(&packetFrames);
        if (FAILED(hr)) break;
        if (packetFrames == 0) break;

        BYTE* data = nullptr;
        UINT32 frames = 0;
        DWORD flags = 0;
        hr = g_app.captureClient->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
        if (FAILED(hr)) break;

        AppendSamplesFromBuffer(data, frames, flags);
        g_app.captureClient->ReleaseBuffer(frames);
    }

    if (FAILED(hr)) {
        ReleaseAudioAnalyzer();
        DecayBandLevels();
    } else {
        AnalyzeBands();
    }

    if (g_app.hwnd) {
        InvalidateRect(g_app.hwnd, &g_app.meterRect, FALSE);
    }
}

static void SyncSlidersFromState() {
    for (int i = 0; i < 31; ++i) {
        SendMessageW(g_app.sliders[i], TBM_SETPOS, TRUE, GainToSliderPos(g_app.gains[i]));
    }
    Button_SetCheck(g_app.eqEnable, g_app.eqEnabled ? BST_CHECKED : BST_UNCHECKED);
    UpdatePresetButtonsVisual();
    UpdateCustomButtonsVisual();
    UpdateGainLineButtonVisual();
    UpdateMixButtonVisual();
    UpdateEffectKnobsVisual();
    UpdateValueLabels();
}

static void LayoutControls(HWND hwnd) {
    RECT rc{};
    GetClientRect(hwnd, &rc);

    const int margin = 12;
    const int buttonH = 28;
    const int top = 12;
    const int customH = 20;
    const int customBtnW = 86;
    const int customSaveW = 46;
    const int customClearW = 48;
    const int customGap = 4;
    const int customBlockW = customBtnW + customGap + customSaveW + customGap + customClearW;
    int x = margin;

    MoveWindow(g_app.resetBtn, x, top, 72, buttonH, TRUE); x += 84;
    MoveWindow(g_app.eqEnable, x, top, 110, buttonH, TRUE); x += 122;

    const int knobW = 40;
    const int knobH = 48;
    const int knobGap = 6;
    const int knobY1 = 2;
    const int knobY2 = knobY1 + knobH + 2;

    ShowWindow(g_app.applyBtn, SW_HIDE);
    ShowWindow(g_app.saveBtn, SW_HIDE);
    ShowWindow(g_app.loadBtn, SW_HIDE);

    int cx = rc.right - margin - customBlockW;

    const int knobCols = 4;
    const int knobTotalW = knobW * knobCols + knobGap * (knobCols - 1);
    const int mixBtnW = 34;
    const int mixBtnH = 22;
    const int mixGap = 8;
    const int mixKnobW = 66;
    const int mixKnobH = 58;
    const int knobBaseX = cx - knobTotalW - 8;
    const int mixBtnX = knobBaseX - mixGap - mixBtnW;
    const int mixBtnY = 4;
    const int mixKnobX = mixBtnX - (mixKnobW - mixBtnW) / 2;
    const int mixKnobY = mixBtnY + mixBtnH + 4;

    const int presetGap = 4;
    const int presetCount = 6;
    const int presetBtnW = 52;
    const int presetRowW = presetBtnW * presetCount + presetGap * (presetCount - 1);
    const int presetAreaRight = mixKnobX - 10;
    int px = x;
    if (presetAreaRight - x > presetRowW) {
        px = x + ((presetAreaRight - x) - presetRowW) / 2;
    }
    MoveWindow(g_app.flatBtn, px, top, presetBtnW, buttonH, TRUE); px += presetBtnW + presetGap;
    MoveWindow(g_app.vshapeBtn, px, top, presetBtnW, buttonH, TRUE); px += presetBtnW + presetGap;
    MoveWindow(g_app.lofiBtn, px, top, presetBtnW, buttonH, TRUE); px += presetBtnW + presetGap;
    MoveWindow(g_app.bassBtn, px, top, presetBtnW, buttonH, TRUE); px += presetBtnW + presetGap;
    MoveWindow(g_app.vocalBtn, px, top, presetBtnW, buttonH, TRUE); px += presetBtnW + presetGap;
    MoveWindow(g_app.trebleBtn, px, top, presetBtnW, buttonH, TRUE);

    int cy1 = top;
    int cy2 = cy1 + customH + 2;
    MoveWindow(g_app.customBtns[0], cx, cy1, customBtnW, customH, TRUE);
    MoveWindow(g_app.customSaveBtns[0], cx + customBtnW + customGap, cy1, customSaveW, customH, TRUE);
    MoveWindow(g_app.customClearBtns[0], cx + customBtnW + customGap + customSaveW + customGap, cy1, customClearW, customH, TRUE);
    MoveWindow(g_app.customBtns[1], cx, cy2, customBtnW, customH, TRUE);
    MoveWindow(g_app.customSaveBtns[1], cx + customBtnW + customGap, cy2, customSaveW, customH, TRUE);
    MoveWindow(g_app.customClearBtns[1], cx + customBtnW + customGap + customSaveW + customGap, cy2, customClearW, customH, TRUE);

    MoveWindow(g_app.mixBtn, mixBtnX, mixBtnY, mixBtnW, mixBtnH, TRUE);
    MoveWindow(g_app.mixKnob, mixKnobX, mixKnobY, mixKnobW, mixKnobH, TRUE);

    MoveWindow(g_app.reverbKnob,         knobBaseX + (knobW + knobGap) * 0, knobY1, knobW, knobH, TRUE);
    MoveWindow(g_app.reverbDistKnob,     knobBaseX + (knobW + knobGap) * 1, knobY1, knobW, knobH, TRUE);
    MoveWindow(g_app.reverbTimeKnob,     knobBaseX + (knobW + knobGap) * 2, knobY1, knobW, knobH, TRUE);
    MoveWindow(g_app.delayKnob,          knobBaseX + (knobW + knobGap) * 3, knobY1, knobW, knobH, TRUE);
    MoveWindow(g_app.delayFeedbackKnob,  knobBaseX + (knobW + knobGap) * 0, knobY2, knobW, knobH, TRUE);
    MoveWindow(g_app.delaySpeedKnob,     knobBaseX + (knobW + knobGap) * 1, knobY2, knobW, knobH, TRUE);
    MoveWindow(g_app.dubHighpassKnob,    knobBaseX + (knobW + knobGap) * 2, knobY2, knobW, knobH, TRUE);
    MoveWindow(g_app.dubLowpassKnob,     knobBaseX + (knobW + knobGap) * 3, knobY2, knobW, knobH, TRUE);

    const int miniBtnW = 42;
    const int miniBtnH = 18;
    MoveWindow(g_app.status, margin, 102, rc.right - margin * 2, 20, TRUE);
    MoveWindow(g_app.gainLineBtn, rc.right - margin - miniBtnW, rc.bottom - margin - miniBtnH, miniBtnW, miniBtnH, TRUE);

    g_app.meterRect = { 4, 128, rc.right - 4, 464 };

    BandsLayout l = GetBandsLayout();
    const int valueW = 44;

    for (int i = 0; i < 31; ++i) {
        const int sx = l.startX + i * (l.sliderW + l.gap);
        MoveWindow(g_app.valueLabels[i], sx - (valueW - l.sliderW) / 2, l.sliderTop - 24, valueW, 18, TRUE);
        MoveWindow(g_app.sliders[i], sx, l.sliderTop, l.sliderW, l.sliderH, TRUE);
    }

    InvalidateRect(hwnd, nullptr, TRUE);
}

static COLORREF MeterColorForNormalized(float tFromBottom) {
    if (tFromBottom < 0.72f) return RGB(46, 208, 72);
    if (tFromBottom < 0.90f) return RGB(230, 208, 52);
    return RGB(228, 72, 72);
}

static void DrawBandMeters(HDC hdc) {
    RECT r = g_app.meterRect;
    HBRUSH bg = CreateSolidBrush(RGB(20, 20, 24));
    FillRect(hdc, &r, bg);
    DeleteObject(bg);

    SetBkMode(hdc, TRANSPARENT);
    SelectObject(hdc, g_app.font);
    SetTextColor(hdc, RGB(220, 220, 228));

    RECT titleRect = { r.left + 10, r.top + 6, r.right - 10, r.top + 26 };
    DrawTextW(hdc, L"31-band Analyzer + Gain markers  (system output analyzer / mpv EQ control)", -1, &titleRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);


    BandsLayout l = GetBandsLayout();
    const int leftScaleW = 40;
    RECT inner = {
        std::max<int>(r.left + leftScaleW, l.startX - 4),
        r.top + 56,
        std::min<int>(r.right - 6, l.startX + l.totalW + 2),
        r.bottom - 18
    };

    HPEN grid = CreatePen(PS_SOLID, 1, RGB(52, 52, 58));
    HGDIOBJ oldPen0 = SelectObject(hdc, grid);
    for (int i = 0; i <= 10; ++i) {
        int y = inner.bottom - (int)std::llround((double)(inner.bottom - inner.top) * i / 10.0);
        MoveToEx(hdc, inner.left, y, nullptr);
        LineTo(hdc, inner.right, y);
    }

    SetTextColor(hdc, RGB(160, 160, 170));
    const int scaleSteps = 10;
    const int barHForScale = inner.bottom - inner.top;
    for (int i = 0; i <= scaleSteps; ++i) {
        const int gain = kGainMax - ((kGainMax - kGainMin) * i) / scaleSteps;
        const double t = (double)(gain - kGainMin) / (double)(kGainMax - kGainMin);
        const int y = inner.bottom - (int)std::llround(std::clamp(t, 0.0, 1.0) * barHForScale);

        std::wstring label;
        if (gain > 0) label += L"+";
        label += std::to_wstring(gain);

        RECT lr = { r.left + 6, y - 9, inner.left - 8, y + 9 };
        DrawTextW(hdc, label.c_str(), -1, &lr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }

    const int segs = 44;
    const int segGap = 1;
    const int barH = inner.bottom - inner.top;
    const int segH = std::max<int>(3, static_cast<int>(barH - (segs - 1) * segGap) / segs);

    for (int i = 0; i < 31; ++i) {
        int sx = l.startX + i * (l.sliderW + l.gap);
        int cx = sx + l.sliderW / 2;
        int bx = cx - l.meterBarW / 2;
        RECT col = { bx - 2, inner.top - 1, bx + l.meterBarW + 2, inner.bottom + 1 };
        HBRUSH colBg = CreateSolidBrush(RGB(24, 24, 30));
        FillRect(hdc, &col, colBg);
        DeleteObject(colBg);

        int active = (int)std::floor(std::clamp(g_app.bandLevel[i], 0.0f, 1.0f) * segs + 0.0001f);
        for (int s = 0; s < segs; ++s) {
            int y1 = inner.bottom - s * (segH + segGap);
            RECT seg = { bx, y1 - segH, bx + l.meterBarW, y1 };
            float t = (float)(s + 1) / (float)segs;
            COLORREF fill = (s < active) ? MeterColorForNormalized(t) : RGB(46, 46, 54);
            HBRUSH br = CreateSolidBrush(fill);
            FillRect(hdc, &seg, br);
            DeleteObject(br);
        }

        int holdY = inner.bottom - (int)std::llround(std::clamp(g_app.bandPeak[i], 0.0f, 1.0f) * barH);
        HPEN holdPen = CreatePen(PS_SOLID, 2, RGB(245, 245, 245));
        HGDIOBJ old = SelectObject(hdc, holdPen);
        MoveToEx(hdc, bx - 1, holdY, nullptr);
        LineTo(hdc, bx + l.meterBarW + 1, holdY);
        SelectObject(hdc, old);
        DeleteObject(holdPen);
    }

    HPEN zeroPen = CreatePen(PS_DOT, 1, RGB(120, 120, 145));
    SelectObject(hdc, zeroPen);
    int zeroY = inner.bottom - (int)std::llround((double)(0 - kGainMin) / (double)(kGainMax - kGainMin) * barH);
    MoveToEx(hdc, inner.left, zeroY, nullptr);
    LineTo(hdc, inner.right, zeroY);

    HPEN gainPen = nullptr;
    if (g_app.showGainLine) {
        gainPen = CreatePen(PS_SOLID, 2, RGB(84, 178, 255));
        SelectObject(hdc, gainPen);
        bool first = true;
        for (int i = 0; i < 31; ++i) {
            int sx = l.startX + i * (l.sliderW + l.gap);
            int cx = sx + l.sliderW / 2;
            double t = (double)(g_app.gains[i] - kGainMin) / (double)(kGainMax - kGainMin);
            int gy = inner.bottom - (int)std::llround(std::clamp(t, 0.0, 1.0) * barH);
            if (first) {
                MoveToEx(hdc, cx, gy, nullptr);
                first = false;
            } else {
                LineTo(hdc, cx, gy);
            }
        }
        for (int i = 0; i < 31; ++i) {
            int sx = l.startX + i * (l.sliderW + l.gap);
            int cx = sx + l.sliderW / 2;
            double t = (double)(g_app.gains[i] - kGainMin) / (double)(kGainMax - kGainMin);
            int gy = inner.bottom - (int)std::llround(std::clamp(t, 0.0, 1.0) * barH);
            Ellipse(hdc, cx - 3, gy - 3, cx + 4, gy + 4);
        }
    }

    HPEN border = CreatePen(PS_SOLID, 1, RGB(70, 70, 78));
    SelectObject(hdc, border);
    HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(HOLLOW_BRUSH));
    Rectangle(hdc, r.left, r.top, r.right, r.bottom);
    Rectangle(hdc, inner.left, inner.top, inner.right, inner.bottom);

    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen0);
    DeleteObject(grid);
    if (gainPen) DeleteObject(gainPen);
    DeleteObject(zeroPen);
    DeleteObject(border);
}

static void DrawBandLabels(HDC hdc) {
    BandsLayout l = GetBandsLayout();

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(35, 35, 35));
    SelectObject(hdc, g_app.font);

    for (int i = 0; i < 31; ++i) {
        int sx = l.startX + i * (l.sliderW + l.gap);
        RECT tr = { sx - 14, l.sliderTop + l.sliderH + 4, sx + l.sliderW + 14, l.sliderTop + l.sliderH + 22 };
        DrawTextW(hdc, kBands[i].label, -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    RECT rc{};
    GetClientRect(g_app.hwnd, &rc);
    RECT info = { 12, l.sliderTop + l.sliderH + 30, rc.right - 24, l.sliderTop + l.sliderH + 50 };
    SetTextColor(hdc, RGB(70, 70, 76));
    DrawTextW(hdc, L"スライダーは上方向で増幅です。アナライザーは既定出力デバイス全体を監視するため、他アプリ音声も反応します。", -1, &info, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}

static void DrawOwnerToggleButton(const DRAWITEMSTRUCT* dis) {
    if (!dis) return;

    const int id = (int)dis->CtlID;
    const bool active = IsToggleButtonActive(id);
    const bool pressed = (dis->itemState & ODS_SELECTED) != 0;
    const bool disabled = (dis->itemState & ODS_DISABLED) != 0;

    RECT rc = dis->rcItem;
    HDC hdc = dis->hDC;

    const bool isPresetOrCustom =
        (id == IDC_FLAT || id == IDC_VSHAPE || id == IDC_LOFI || id == IDC_BASS || id == IDC_VOCAL || id == IDC_TREBLE ||
         id == IDC_CUSTOM1 || id == IDC_CUSTOM2);

    if (isPresetOrCustom) {
        const bool sunken = active || pressed;
        COLORREF face = GetSysColor(COLOR_BTNFACE);
        COLORREF text = GetSysColor(COLOR_BTNTEXT);
        COLORREF hi = sunken ? RGB(110, 110, 110) : RGB(255, 255, 255);
        COLORREF shadow = sunken ? RGB(255, 255, 255) : RGB(110, 110, 110);
        COLORREF dark = sunken ? RGB(245, 245, 245) : RGB(64, 64, 64);
        COLORREF light = sunken ? RGB(64, 64, 64) : RGB(232, 232, 232);

        if (active) {
            face = pressed ? RGB(58, 144, 76) : RGB(74, 170, 92);
            text = RGB(255, 255, 255);
        } else if (pressed) {
            face = RGB(208, 208, 208);
        }

        if (disabled) {
            face = RGB(212, 212, 212);
            text = RGB(140, 140, 140);
        }

        HBRUSH br = CreateSolidBrush(face);
        FillRect(hdc, &rc, br);
        DeleteObject(br);

        HPEN penHi = CreatePen(PS_SOLID, 1, hi);
        HPEN penShadow = CreatePen(PS_SOLID, 1, shadow);
        HPEN penDark = CreatePen(PS_SOLID, 1, dark);
        HPEN penLight = CreatePen(PS_SOLID, 1, light);
        HGDIOBJ oldPen = SelectObject(hdc, penHi);

        MoveToEx(hdc, rc.left, rc.bottom - 1, nullptr);
        LineTo(hdc, rc.left, rc.top);
        LineTo(hdc, rc.right - 1, rc.top);

        SelectObject(hdc, penShadow);
        MoveToEx(hdc, rc.left, rc.bottom - 1, nullptr);
        LineTo(hdc, rc.right - 1, rc.bottom - 1);
        LineTo(hdc, rc.right - 1, rc.top - 1);

        RECT inner = rc;
        InflateRect(&inner, -1, -1);

        SelectObject(hdc, penLight);
        MoveToEx(hdc, inner.left, inner.bottom - 1, nullptr);
        LineTo(hdc, inner.left, inner.top);
        LineTo(hdc, inner.right - 1, inner.top);

        SelectObject(hdc, penDark);
        MoveToEx(hdc, inner.left, inner.bottom - 1, nullptr);
        LineTo(hdc, inner.right - 1, inner.bottom - 1);
        LineTo(hdc, inner.right - 1, inner.top - 1);

        SelectObject(hdc, oldPen);
        DeleteObject(penHi);
        DeleteObject(penShadow);
        DeleteObject(penDark);
        DeleteObject(penLight);

        if (active && !disabled) {
            RECT glow = rc;
            InflateRect(&glow, -3, -3);
            HPEN glowPen = CreatePen(PS_SOLID, 1, RGB(140, 220, 154));
            HGDIOBJ oldGlowPen = SelectObject(hdc, glowPen);
            HGDIOBJ oldGlowBrush = SelectObject(hdc, GetStockObject(HOLLOW_BRUSH));
            Rectangle(hdc, glow.left, glow.top, glow.right, glow.bottom);
            SelectObject(hdc, oldGlowBrush);
            SelectObject(hdc, oldGlowPen);
            DeleteObject(glowPen);
        }

        wchar_t textBuf[64]{};
        GetWindowTextW(dis->hwndItem, textBuf, (int)(sizeof(textBuf) / sizeof(textBuf[0])));
        RECT tr = rc;
        if (sunken) OffsetRect(&tr, 1, 1);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, text);
        SelectObject(hdc, g_app.font);
        DrawTextW(hdc, textBuf, -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

        if (dis->itemState & ODS_FOCUS) {
            RECT fr = rc;
            InflateRect(&fr, -4, -4);
            DrawFocusRect(hdc, &fr);
        }
        return;
    }

    const bool isBlueToggle = (id == IDC_GAINLINE || id == IDC_MIX);
    COLORREF bg = active ? (isBlueToggle ? RGB(66, 122, 214) : RGB(74, 170, 92)) : GetSysColor(COLOR_BTNFACE);
    COLORREF border = active ? (isBlueToggle ? RGB(34, 76, 160) : RGB(34, 110, 52)) : RGB(120, 120, 120);
    COLORREF text = active ? RGB(255, 255, 255) : GetSysColor(COLOR_BTNTEXT);
    if (pressed) {
        bg = active ? (isBlueToggle ? RGB(52, 100, 188) : RGB(58, 144, 76)) : RGB(208, 208, 208);
    }
    if (disabled) {
        bg = RGB(210, 210, 210);
        border = RGB(170, 170, 170);
        text = RGB(140, 140, 140);
    }

    HBRUSH br = CreateSolidBrush(bg);
    FillRect(hdc, &rc, br);
    DeleteObject(br);

    HPEN pen = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ oldPen = SelectObject(hdc, pen);
    HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(HOLLOW_BRUSH));
    Rectangle(hdc, rc.left, rc.top, rc.right, rc.bottom);
    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(pen);

    if (active) {
        RECT glow = rc;
        InflateRect(&glow, -2, -2);
        HPEN glowPen = CreatePen(PS_SOLID, 1, isBlueToggle ? RGB(136, 186, 255) : RGB(140, 220, 154));
        oldPen = SelectObject(hdc, glowPen);
        oldBrush = SelectObject(hdc, GetStockObject(HOLLOW_BRUSH));
        Rectangle(hdc, glow.left, glow.top, glow.right, glow.bottom);
        SelectObject(hdc, oldBrush);
        SelectObject(hdc, oldPen);
        DeleteObject(glowPen);
    }

    wchar_t textBuf[64]{};
    GetWindowTextW(dis->hwndItem, textBuf, (int)(sizeof(textBuf) / sizeof(textBuf[0])));
    RECT tr = rc;
    if (pressed) OffsetRect(&tr, 1, 1);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, text);
    SelectObject(hdc, g_app.font);
    DrawTextW(hdc, textBuf, -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    if (dis->itemState & ODS_FOCUS) {
        RECT fr = rc;
        InflateRect(&fr, -4, -4);
        DrawFocusRect(hdc, &fr);
    }
}

static void DrawKnobControl(HWND hwnd, HDC hdc) {
    RECT rc{};
    GetClientRect(hwnd, &rc);

    HBRUSH bg = CreateSolidBrush(GetSysColor(COLOR_BTNFACE));
    FillRect(hdc, &rc, bg);
    DeleteObject(bg);

    const int id = GetDlgCtrlID(hwnd);
    const int value = GetKnobValuePtr(id) ? *GetKnobValuePtr(id) : 0;
    const wchar_t* label = GetKnobLabel(id);

    const int w = (int)(rc.right - rc.left);
    const int h = (int)(rc.bottom - rc.top);
    const bool showLabel = (label && label[0] != L'\0');
    const bool isBigKnob = (id == IDC_MIX_KNOB);

    const int labelH = showLabel ? (isBigKnob ? 0 : 12) : 0;
    const int valueH = isBigKnob ? 14 : 12;
    const int topPad = isBigKnob ? 4 : 2;
    const int bottomPad = isBigKnob ? 2 : 2;
    const int valueGap = isBigKnob ? 1 : 1;
    const int usableBottom = std::max(topPad + 16, h - labelH - valueH - valueGap - bottomPad);
    const int cx = w / 2;
    const int minRadius = isBigKnob ? 19 : 8;
    const int radius = std::max(minRadius, std::min((w / 2) - 6, ((usableBottom - topPad) / 2) - 2));
    const int cy = std::max(topPad + radius, usableBottom - radius);

    HBRUSH knobBrush = CreateSolidBrush(RGB(232, 232, 236));
    HPEN borderPen = CreatePen(PS_SOLID, 1, RGB(118, 118, 126));
    HGDIOBJ oldBrush = SelectObject(hdc, knobBrush);
    HGDIOBJ oldPen = SelectObject(hdc, borderPen);
    Ellipse(hdc, cx - radius, cy - radius, cx + radius + 1, cy + radius + 1);
    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(knobBrush);
    DeleteObject(borderPen);

    const double t = std::clamp(value / 100.0, 0.0, 1.0);
    const double angle = (-135.0 + 270.0 * t) * (kPi / 180.0);
    const int pointerLen = std::max(6, radius - 4);
    const int px = cx + (int)std::llround(std::cos(angle) * pointerLen);
    const int py = cy + (int)std::llround(std::sin(angle) * pointerLen);
    HPEN pointerPen = CreatePen(PS_SOLID, isBigKnob ? 3 : 2, value > 0 ? RGB(84, 178, 255) : RGB(92, 92, 100));
    oldPen = SelectObject(hdc, pointerPen);
    MoveToEx(hdc, cx, cy, nullptr);
    LineTo(hdc, px, py);
    SelectObject(hdc, oldPen);
    DeleteObject(pointerPen);

    SetBkMode(hdc, TRANSPARENT);
    SelectObject(hdc, g_app.font);
    SetTextColor(hdc, RGB(40, 40, 46));
    RECT vr = { 0, cy + radius + valueGap, w, std::min(h - labelH, cy + radius + valueGap + valueH + 2) };
    std::wstring valueText = std::to_wstring(value);
    DrawTextW(hdc, valueText.c_str(), -1, &vr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    if (showLabel) {
        RECT lr = { 0, h - labelH, w, h };
        SetTextColor(hdc, RGB(70, 70, 76));
        DrawTextW(hdc, label, -1, &lr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
}

static LRESULT CALLBACK KnobWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    const int id = GetDlgCtrlID(hwnd);
    switch (msg) {
    case WM_LBUTTONDOWN: {
        int* pValue = GetKnobValuePtr(id);
        if (!pValue) return 0;
        SetFocus(hwnd);
        SetCapture(hwnd);
        g_app.knobTracking = true;
        g_app.knobTrackingId = id;
        g_app.knobTrackingStartY = GET_Y_LPARAM(lParam);
        g_app.knobTrackingStartValue = *pValue;
        return 0;
    }
    case WM_MOUSEMOVE:
        if (g_app.knobTracking && g_app.knobTrackingId == id && (wParam & MK_LBUTTON)) {
            const int delta = (g_app.knobTrackingStartY - GET_Y_LPARAM(lParam)) / 2;
            SetKnobValue(id, g_app.knobTrackingStartValue + delta, true);
        }
        return 0;
    case WM_LBUTTONUP:
        if (g_app.knobTracking && g_app.knobTrackingId == id) {
            ReleaseCapture();
            g_app.knobTracking = false;
            g_app.knobTrackingId = 0;
            ScheduleApplyWithDelay(30);
            SaveStateToJson(false);
        }
        return 0;
    case WM_CAPTURECHANGED:
        if (g_app.knobTracking && g_app.knobTrackingId == id) {
            g_app.knobTracking = false;
            g_app.knobTrackingId = 0;
        }
        return 0;
    case WM_MOUSEWHEEL: {
        const int step = (GET_WHEEL_DELTA_WPARAM(wParam) > 0) ? 2 : -2;
        int* pValue = GetKnobValuePtr(id);
        if (pValue) {
            SetKnobValue(id, *pValue + step, true);
            SaveStateToJson(false);
        }
        return 0;
    }
    case WM_LBUTTONDBLCLK:
        SetKnobValue(id, 0, true);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC hdc = BeginPaint(hwnd, &ps);
        DrawKnobControl(hwnd, hdc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static void CreateChildControls(HWND hwnd) {
    g_app.hwnd = hwnd;
    g_app.font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

    g_app.eqEnable = CreateWindowW(L"BUTTON", L"Enable EQ",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_EQ_ENABLE, g_app.hInst, nullptr);

    g_app.resetBtn = CreateWindowW(L"BUTTON", L"Reset",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_RESET, g_app.hInst, nullptr);
    g_app.applyBtn = CreateWindowW(L"BUTTON", L"Apply",
        WS_CHILD | BS_PUSHBUTTON,
        0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_APPLY, g_app.hInst, nullptr);
    g_app.flatBtn = CreateWindowW(L"BUTTON", L"Flat",
        WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
        0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_FLAT, g_app.hInst, nullptr);
    g_app.vshapeBtn = CreateWindowW(L"BUTTON", L"V-Shape",
        WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
        0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_VSHAPE, g_app.hInst, nullptr);
    g_app.lofiBtn = CreateWindowW(L"BUTTON", L"Lo-fi",
        WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
        0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_LOFI, g_app.hInst, nullptr);
    g_app.bassBtn = CreateWindowW(L"BUTTON", L"Bass+",
        WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
        0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_BASS, g_app.hInst, nullptr);
    g_app.vocalBtn = CreateWindowW(L"BUTTON", L"Vocal",
        WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
        0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_VOCAL, g_app.hInst, nullptr);
    g_app.trebleBtn = CreateWindowW(L"BUTTON", L"Treble+",
        WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
        0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_TREBLE, g_app.hInst, nullptr);
    g_app.saveBtn = CreateWindowW(L"BUTTON", L"Save",
        WS_CHILD | BS_PUSHBUTTON,
        0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_SAVE, g_app.hInst, nullptr);
    g_app.loadBtn = CreateWindowW(L"BUTTON", L"Load",
        WS_CHILD | BS_PUSHBUTTON,
        0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_LOAD, g_app.hInst, nullptr);
    g_app.gainLineBtn = CreateWindowW(L"BUTTON", L"Line",
        WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
        0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_GAINLINE, g_app.hInst, nullptr);
    g_app.mixBtn = CreateWindowW(L"BUTTON", L"Mix",
        WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
        0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_MIX, g_app.hInst, nullptr);
    g_app.mixKnob = CreateWindowW(L"mpv_eq31_knob", L"",
        WS_CHILD | WS_VISIBLE,
        0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_MIX_KNOB, g_app.hInst, nullptr);

    g_app.reverbKnob = CreateWindowW(L"mpv_eq31_knob", L"",
        WS_CHILD | WS_VISIBLE,
        0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_REVERB_KNOB, g_app.hInst, nullptr);
    g_app.reverbDistKnob = CreateWindowW(L"mpv_eq31_knob", L"",
        WS_CHILD | WS_VISIBLE,
        0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_REVERB_DIST_KNOB, g_app.hInst, nullptr);
    g_app.reverbTimeKnob = CreateWindowW(L"mpv_eq31_knob", L"",
        WS_CHILD | WS_VISIBLE,
        0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_REVERB_TIME_KNOB, g_app.hInst, nullptr);
    g_app.delayKnob = CreateWindowW(L"mpv_eq31_knob", L"",
        WS_CHILD | WS_VISIBLE,
        0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_DELAY_KNOB, g_app.hInst, nullptr);
    g_app.delayFeedbackKnob = CreateWindowW(L"mpv_eq31_knob", L"",
        WS_CHILD | WS_VISIBLE,
        0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_DELAY_FDB_KNOB, g_app.hInst, nullptr);
    g_app.delaySpeedKnob = CreateWindowW(L"mpv_eq31_knob", L"",
        WS_CHILD | WS_VISIBLE,
        0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_DELAY_SPD_KNOB, g_app.hInst, nullptr);
    g_app.dubHighpassKnob = CreateWindowW(L"mpv_eq31_knob", L"",
        WS_CHILD | WS_VISIBLE,
        0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_DUB_HPF_KNOB, g_app.hInst, nullptr);
    g_app.dubLowpassKnob = CreateWindowW(L"mpv_eq31_knob", L"",
        WS_CHILD | WS_VISIBLE,
        0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_DUB_LPF_KNOB, g_app.hInst, nullptr);

    g_app.customBtns[0] = CreateWindowW(L"BUTTON", L"Custom 1",
        WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
        0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_CUSTOM1, g_app.hInst, nullptr);
    g_app.customSaveBtns[0] = CreateWindowW(L"BUTTON", L"Save",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | BS_PUSHLIKE,
        0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_CUSTOM1_SAVE, g_app.hInst, nullptr);
    g_app.customClearBtns[0] = CreateWindowW(L"BUTTON", L"Clear",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_CUSTOM1_CLEAR, g_app.hInst, nullptr);

    g_app.customBtns[1] = CreateWindowW(L"BUTTON", L"Custom 2",
        WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
        0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_CUSTOM2, g_app.hInst, nullptr);
    g_app.customSaveBtns[1] = CreateWindowW(L"BUTTON", L"Save",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | BS_PUSHLIKE,
        0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_CUSTOM2_SAVE, g_app.hInst, nullptr);
    g_app.customClearBtns[1] = CreateWindowW(L"BUTTON", L"Clear",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_CUSTOM2_CLEAR, g_app.hInst, nullptr);

    g_app.status = CreateWindowW(L"STATIC", L"Ready",
        WS_CHILD | WS_VISIBLE,
        0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_STATUS, g_app.hInst, nullptr);

    for (int i = 0; i < 31; ++i) {
        g_app.valueLabels[i] = CreateWindowW(L"STATIC", L"+0",
            WS_CHILD | WS_VISIBLE | SS_CENTER,
            0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)(IDC_VALUE_BASE + i), g_app.hInst, nullptr);

        g_app.sliders[i] = CreateWindowW(TRACKBAR_CLASSW, L"",
            WS_CHILD | WS_VISIBLE | TBS_VERT | TBS_AUTOTICKS | TBS_BOTH,
            0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)(IDC_BAND_BASE + i), g_app.hInst, nullptr);
        SendMessageW(g_app.sliders[i], TBM_SETRANGEMIN, TRUE, 0);
        SendMessageW(g_app.sliders[i], TBM_SETRANGEMAX, TRUE, kGainMax - kGainMin);
        SendMessageW(g_app.sliders[i], TBM_SETTICFREQ, 5, 0);
        SendMessageW(g_app.sliders[i], TBM_SETPAGESIZE, 0, 2);
    }

    HWND ctrls[] = {
        g_app.eqEnable, g_app.resetBtn, g_app.applyBtn, g_app.flatBtn, g_app.vshapeBtn, g_app.lofiBtn, g_app.bassBtn,
        g_app.vocalBtn, g_app.trebleBtn, g_app.saveBtn, g_app.loadBtn, g_app.gainLineBtn, g_app.mixBtn,
        g_app.customBtns[0], g_app.customSaveBtns[0], g_app.customClearBtns[0],
        g_app.mixKnob, g_app.reverbKnob, g_app.reverbDistKnob, g_app.reverbTimeKnob, g_app.delayKnob,
        g_app.delayFeedbackKnob, g_app.delaySpeedKnob, g_app.dubHighpassKnob, g_app.dubLowpassKnob,
        g_app.customBtns[1], g_app.customSaveBtns[1], g_app.customClearBtns[1],
        g_app.status
    };
    for (HWND c : ctrls) {
        SendMessageW(c, WM_SETFONT, (WPARAM)g_app.font, TRUE);
    }
    for (int i = 0; i < 31; ++i) {
        SendMessageW(g_app.valueLabels[i], WM_SETFONT, (WPARAM)g_app.font, TRUE);
    }

    SyncSlidersFromState();
    LayoutControls(hwnd);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        CreateChildControls(hwnd);
        ResolveConfigPaths();
        LoadStateFromJson(true);
        SetTimer(hwnd, TIMER_ANALYZER, 40, nullptr);
        SetTimer(hwnd, TIMER_PIPE_WATCH, 1000, nullptr);
        return 0;

    case WM_GETMINMAXINFO: {
        MINMAXINFO* mmi = (MINMAXINFO*)lParam;
        RECT wr = { 0, 0, kWindowW, kWindowH };
        AdjustWindowRect(&wr, GetWindowLongW(hwnd, GWL_STYLE), FALSE);
        int w = wr.right - wr.left;
        int h = wr.bottom - wr.top;
        mmi->ptMinTrackSize.x = w;
        mmi->ptMinTrackSize.y = h;
        mmi->ptMaxTrackSize.x = w;
        mmi->ptMaxTrackSize.y = h;
        return 0;
    }

    case WM_COMMAND: {
        const int id = LOWORD(wParam);
        const int code = HIWORD(wParam);
        if (id == IDC_EQ_ENABLE && code == BN_CLICKED) {
            g_app.eqEnabled = (Button_GetCheck(g_app.eqEnable) == BST_CHECKED);
            SaveStateToJson(false);
            ScheduleApply();
        } else if (id == IDC_RESET && code == BN_CLICKED) {
            ResetCurrentState();
        } else if (id == IDC_APPLY && code == BN_CLICKED) {
            ApplyToMpv();
        } else if (id == IDC_FLAT && code == BN_CLICKED) {
            ActivatePresetButton(PRESET_FLAT);
        } else if (id == IDC_VSHAPE && code == BN_CLICKED) {
            ActivatePresetButton(PRESET_VSHAPE);
        } else if (id == IDC_LOFI && code == BN_CLICKED) {
            ActivatePresetButton(PRESET_LOFI);
        } else if (id == IDC_BASS && code == BN_CLICKED) {
            ActivatePresetButton(PRESET_BASS);
        } else if (id == IDC_VOCAL && code == BN_CLICKED) {
            ActivatePresetButton(PRESET_VOCAL);
        } else if (id == IDC_TREBLE && code == BN_CLICKED) {
            ActivatePresetButton(PRESET_TREBLE);
        } else if (id == IDC_SAVE && code == BN_CLICKED) {
            SaveStateToJson();
        } else if (id == IDC_LOAD && code == BN_CLICKED) {
            LoadStateFromJson(false);
        } else if (id == IDC_GAINLINE && code == BN_CLICKED) {
            g_app.showGainLine = !g_app.showGainLine;
            UpdateGainLineButtonVisual();
            SaveStateToJson(false);
            SetStatus(g_app.showGainLine ? L"青線グラフ表示を ON にしました。" : L"青線グラフ表示を OFF にしました。");
        } else if (id == IDC_MIX && code == BN_CLICKED) {
            SetEffectsMixEnabled(!g_app.effectsMixEnabled, true);
            UpdateMixButtonVisual();
            UpdateEffectKnobsVisual();
            SaveStateToJson(false);
            SetStatus(g_app.effectsMixEnabled ? L"Mix を ON にしました。下の大きいMixノブの量までスムースに戻します。" : L"Mix を OFF にしました。下の大きいMixノブ値は保持したまま、丸ノブエフェクトだけをスムースに引きます。");
        } else if (id == IDC_CUSTOM1 && code == BN_CLICKED) {
            ActivateCustomSlot(0);
        } else if (id == IDC_CUSTOM2 && code == BN_CLICKED) {
            ActivateCustomSlot(1);
        } else if (id == IDC_CUSTOM1_SAVE && code == BN_CLICKED) {
            SaveCurrentToCustomSlot(0);
        } else if (id == IDC_CUSTOM2_SAVE && code == BN_CLICKED) {
            SaveCurrentToCustomSlot(1);
        } else if (id == IDC_CUSTOM1_CLEAR && code == BN_CLICKED) {
            ClearCustomSlot(0);
        } else if (id == IDC_CUSTOM2_CLEAR && code == BN_CLICKED) {
            ClearCustomSlot(1);
        }
        return 0;
    }

    case WM_HSCROLL:
    case WM_VSCROLL: {
        HWND ctl = (HWND)lParam;
        for (int i = 0; i < 31; ++i) {
            if (ctl == g_app.sliders[i]) {
                if (g_app.activePreset != PRESET_NONE || g_app.activeCustomSlot != 0) {
                    g_app.activePreset = PRESET_NONE;
                    g_app.activeCustomSlot = 0;
                                    UpdatePresetButtonsVisual();
                    UpdateCustomButtonsVisual();
                }
                int pos = (int)SendMessageW(g_app.sliders[i], TBM_GETPOS, 0, 0);
                g_app.gains[i] = SliderPosToGain(pos);
                UpdateValueLabels();
                ScheduleApply();
                break;
            }
        }
        return 0;
    }

    case WM_DRAWITEM: {
        const DRAWITEMSTRUCT* dis = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
        if (dis) {
            switch (dis->CtlID) {
            case IDC_FLAT:
            case IDC_VSHAPE:
            case IDC_LOFI:
            case IDC_BASS:
            case IDC_VOCAL:
            case IDC_TREBLE:
            case IDC_CUSTOM1:
            case IDC_CUSTOM2:
            case IDC_GAINLINE:
            case IDC_MIX:
                DrawOwnerToggleButton(dis);
                return TRUE;
            default:
                break;
            }
        }
        break;
    }

    case WM_TIMER:
        if (wParam == TIMER_APPLY) {
            KillTimer(hwnd, TIMER_APPLY);
            AdvanceEffectsMixFadeStep();
            ApplyToMpv();
            if (g_app.effectsMixFading) {
                SetTimer(hwnd, TIMER_APPLY, 28, nullptr);
            }
            return 0;
        }
        if (wParam == TIMER_ANALYZER) {
            PollAudioAnalyzer();
            return 0;
        }
        if (wParam == TIMER_PIPE_WATCH) {
            CheckPipeAndCloseIfNeeded(hwnd);
            return 0;
        }
        break;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC hdc = BeginPaint(hwnd, &ps);

        RECT client{};
        GetClientRect(hwnd, &client);
        const int paintW = std::max<int>(1, static_cast<int>(client.right - client.left));
        const int paintH = std::max<int>(1, static_cast<int>(client.bottom - client.top));

        HDC memdc = CreateCompatibleDC(hdc);
        HBITMAP membmp = memdc ? CreateCompatibleBitmap(hdc, paintW, paintH) : nullptr;
        if (!memdc || !membmp) {
            if (membmp) DeleteObject(membmp);
            if (memdc) DeleteDC(memdc);

            HBRUSH winBg = CreateSolidBrush(GetSysColor(COLOR_BTNFACE));
            FillRect(hdc, &client, winBg);
            DeleteObject(winBg);
            DrawBandMeters(hdc);
            DrawBandLabels(hdc);
            EndPaint(hwnd, &ps);
            return 0;
        }

        HGDIOBJ oldBmp = SelectObject(memdc, membmp);

        HBRUSH winBg = CreateSolidBrush(GetSysColor(COLOR_BTNFACE));
        FillRect(memdc, &client, winBg);
        DeleteObject(winBg);

        DrawBandMeters(memdc);
        DrawBandLabels(memdc);

        BitBlt(hdc, 0, 0, paintW, paintH, memdc, 0, 0, SRCCOPY);

        SelectObject(memdc, oldBmp);
        DeleteObject(membmp);
        DeleteDC(memdc);

        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_DESTROY:
        SaveStateToJson(false);
        KillTimer(hwnd, TIMER_APPLY);
        KillTimer(hwnd, TIMER_ANALYZER);
        KillTimer(hwnd, TIMER_PIPE_WATCH);
        ReleaseAudioAnalyzer();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static void FocusExistingMainWindow() {
    HWND hwnd = FindWindowW(L"mpv_eq31_window", nullptr);
    if (!hwnd) return;
    if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
    BringWindowToTop(hwnd);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
    HANDLE hSingleInstanceMutex = CreateMutexW(nullptr, FALSE, L"Local\\mpv_31_band_Graphic_EQ_single_instance");
    if (!hSingleInstanceMutex) {
        return 0;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        FocusExistingMainWindow();
        CloseHandle(hSingleInstanceMutex);
        return 0;
    }
    g_app.hInst = hInstance;

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv && argc >= 2 && argv[1] && argv[1][0]) {
        g_app.pipePath = argv[1];
    }
    if (argv) LocalFree(argv);

    const HRESULT coInitHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool shouldCoUninitialize = SUCCEEDED(coInitHr);

    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_BAR_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    WNDCLASSW kwc{};
    kwc.style = CS_DBLCLKS;
    kwc.lpfnWndProc = KnobWndProc;
    kwc.hInstance = hInstance;
    kwc.lpszClassName = L"mpv_eq31_knob";
    kwc.hCursor = LoadCursor(nullptr, IDC_HAND);
    kwc.hbrBackground = nullptr;
    RegisterClassW(&kwc);

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"mpv_eq31_window";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    RegisterClassW(&wc);

    RECT wr = { 0, 0, kWindowW, kWindowH };
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRect(&wr, style, FALSE);

    std::wstring title = L"mpv 31-band Graphic EQ";
    HWND hwnd = CreateWindowExW(
        0, wc.lpszClassName, title.c_str(),
        style | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT,
        wr.right - wr.left, wr.bottom - wr.top,
        nullptr, nullptr, hInstance, nullptr);

    if (!hwnd) {
        if (shouldCoUninitialize) CoUninitialize();
        CloseHandle(hSingleInstanceMutex);
        return 0;
    }

    g_app.hwnd = hwnd;
    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    SetStatus(L"Ready - mpv終了時はIPC pipe監視でこのEQツールも自動終了します。保存済みCustom 1 / 2 は次回起動時にも復元されます。");
    InitAudioAnalyzer();

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (shouldCoUninitialize) CoUninitialize();
    CloseHandle(hSingleInstanceMutex);
    return (int)msg.wParam;
}
