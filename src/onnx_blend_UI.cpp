// onnx_blend_UI.cpp
// GUI controller for onnx_blend_ui.vpy
// v18: restores Quality Preview as a practical ON/OFF preview switch.
// Quality Preview ON registers/reloads @onnx_blend_ui for realtime checking.
// Quality Preview OFF removes @onnx_blend_ui.
// v20: syncs Quality Preview checkbox exactly with onnx_blend_ui.vpy mpv vf on/off changes.
// Build:
// cl /nologo /EHsc /std:c++17 /utf-8 onnx_blend_UI.cpp /Fe:onnx_blend_UI.exe ^
//   user32.lib gdi32.lib comctl32.lib comdlg32.lib shell32.lib advapi32.lib

#define UNICODE
#define _UNICODE
#define NOMINMAX

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>

#include <algorithm>
#include <cctype>
#include <cwctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")

namespace fs = std::filesystem;

namespace {

constexpr wchar_t kAppTitle[] = L"mpv ONNX Blend UI";
constexpr wchar_t kMutexName[] = L"Local\\onnx_blend_UI_single_instance_v6";
constexpr wchar_t kModelDirRel[] = L"vs-plugins/models/55ai";
constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\mpv-tool";
constexpr UINT_PTR kAutoApplyTimer = 7001;
constexpr UINT_PTR kMpvWatchTimer = 7002;
constexpr UINT kAutoApplyDelayMs = 900;
constexpr UINT kMpvWatchIntervalMs = 150;
constexpr DWORD kMpvFilterSyncIntervalMs = 600;

constexpr int IDC_ENABLE          = 1001;
constexpr int IDC_AUTO_APPLY      = 1002;
constexpr int IDC_LANGUAGE        = 1003;
constexpr int IDC_ALWAYS_ON_TOP   = 1004;
constexpr int IDC_ENGINE1_EDIT    = 1101;
constexpr int IDC_ENGINE1_BROWSE  = 1102;
constexpr int IDC_USE_ENGINE2     = 1200;
constexpr int IDC_ENGINE2_EDIT    = 1201;
constexpr int IDC_ENGINE2_BROWSE  = 1202;
constexpr int IDC_SWAP            = 1203;
constexpr int IDC_BLEND_MODE      = 1301;
constexpr int IDC_LUMA_ONLY       = 1302;
constexpr int IDC_COMPARE_MODE    = 1303;
constexpr int IDC_ENGINE1_TRACK   = 1401;
constexpr int IDC_ENGINE1_VALUE   = 1402;
constexpr int IDC_ENGINE2_TRACK   = 1411;
constexpr int IDC_ENGINE2_VALUE   = 1412;
constexpr int IDC_ORIGINAL_TRACK  = 1421;
constexpr int IDC_ORIGINAL_VALUE  = 1422;
constexpr int IDC_APPLY           = 1501;
constexpr int IDC_REMOVE          = 1502;
constexpr int IDC_RESET           = 1503;
constexpr int IDC_OPEN_SETTINGS   = 1504;
constexpr int IDC_MODEL_LIST      = 2101;

struct Settings {
    // Quality preview: true = ONNX blend output, false = simple 2x bypass inside vpy.
    bool enabled = true;
    std::wstring engine1Model = L"vs-plugins/models/55ai/2x_AnimeJaNai_SD_V1beta34_Compact_93k-fp16.onnx";
    std::wstring engine2Model = L"vs-plugins/models/55ai/ToHeart_35mm_Compact_V1_1_fp16.onnx";
    bool useEngine2 = false;
    std::wstring blendMode = L"alpha";
    double engine1Amount = 0.55;
    double engine2Amount = 0.35;
    double originalOpacity = 1.00;
    bool diffLumaOnly = false;
    bool autoApply = true;
    std::wstring testMode = L"off";
    std::wstring language = L"ja"; // Japanese default
    bool alwaysOnTop = false;
    // v12+: no user-facing chain-managed switch. Order is automatically preserved when the filter already exists.
    int runtimeSerial = 0;
};

Settings g_settings;
std::wstring g_exeDir;
std::wstring g_configDir;
std::wstring g_configPath;
bool g_loading = false;
bool g_everConnectedToMpv = false;
bool g_applyingToMpv = false;
bool g_pendingApply = false;
ULONGLONG g_lastMpvFilterSyncTick = 0;

HWND g_hwnd = nullptr;
HWND hTitle = nullptr;
HWND hEnable = nullptr;
HWND hAutoApply = nullptr;
HWND hLanguage = nullptr;
HWND hAlwaysOnTop = nullptr;
HWND hEngine1Label = nullptr;
HWND hEngine1Edit = nullptr;
HWND hEngine1Browse = nullptr;
HWND hUseEngine2 = nullptr;
HWND hEngine2Label = nullptr;
HWND hEngine2Edit = nullptr;
HWND hEngine2Browse = nullptr;
HWND hSwap = nullptr;
HWND hBlendModeLabel = nullptr;
HWND hBlendMode = nullptr;
HWND hLumaOnly = nullptr;
HWND hCompareLabel = nullptr;
HWND hCompareMode = nullptr;
HWND hEngine1AmountLabel = nullptr;
HWND hEngine1Track = nullptr;
HWND hEngine1Value = nullptr;
HWND hEngine2AmountLabel = nullptr;
HWND hEngine2Track = nullptr;
HWND hEngine2Value = nullptr;
HWND hOriginalOpacityLabel = nullptr;
HWND hOriginalTrack = nullptr;
HWND hOriginalValue = nullptr;
HWND hApplyButton = nullptr;
HWND hRemoveButton = nullptr;
HWND hResetButton = nullptr;
HWND hOpenSettingsButton = nullptr;
HWND hStatus = nullptr;

bool IsEnglish() {
    return g_settings.language == L"en";
}

const wchar_t* Txt(const wchar_t* ja, const wchar_t* en) {
    return IsEnglish() ? en : ja;
}

std::wstring TxtS(const wchar_t* ja, const wchar_t* en) {
    return std::wstring(Txt(ja, en));
}

std::string WideToUtf8(const std::wstring& ws) {
    if (ws.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string out(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, out.data(), size, nullptr, nullptr);
    if (!out.empty() && out.back() == '\0') out.pop_back();
    return out;
}

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int size = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (size <= 0) return {};
    std::wstring out(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), size);
    if (!out.empty() && out.back() == L'\0') out.pop_back();
    return out;
}

std::string JsonEscapeUtf8(const std::wstring& ws) {
    std::string s = WideToUtf8(ws);
    std::string out;
    out.reserve(s.size() + 16);
    for (unsigned char ch : s) {
        switch (ch) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (ch < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", ch);
                out += buf;
            } else {
                out += static_cast<char>(ch);
            }
        }
    }
    return out;
}

std::wstring JsonUnescapeToWide(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '\\' && i + 1 < s.size()) {
            char n = s[++i];
            switch (n) {
            case '\\': out.push_back('\\'); break;
            case '"': out.push_back('"'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u':
                if (i + 4 < s.size()) {
                    out.push_back('?');
                    i += 4;
                }
                break;
            default:
                out.push_back(n);
            }
        } else {
            out.push_back(c);
        }
    }
    return Utf8ToWide(out);
}

std::string ReadTextFileUtf8(const std::wstring& path) {
    std::ifstream ifs(fs::path(path), std::ios::binary);
    if (!ifs) return {};
    std::ostringstream ss;
    ss << ifs.rdbuf();
    return ss.str();
}

void EnsureDirectories() {
    try { fs::create_directories(fs::path(g_configDir)); } catch (...) {}
}

std::wstring GetModuleDir() {
    wchar_t buf[MAX_PATH * 4] = {};
    DWORD len = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    fs::path p(std::wstring(buf, len));
    return p.parent_path().wstring();
}

bool FileExists(const std::wstring& path) {
    DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring NormalizeSlashes(std::wstring s) {
    std::replace(s.begin(), s.end(), L'\\', L'/');
    return s;
}

std::wstring ToLower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    return s;
}

std::wstring ResolvePathFromExeDir(const std::wstring& path) {
    try {
        fs::path p(path);
        if (p.is_absolute()) return p.wstring();
        return (fs::path(g_exeDir) / p).wstring();
    } catch (...) {
        return path;
    }
}

std::wstring Make55aiPortablePath(const std::wstring& selected, bool* okIn55ai = nullptr) {
    if (okIn55ai) *okIn55ai = false;
    try {
        fs::path selectedAbs = fs::absolute(fs::path(selected));
        fs::path base55 = fs::absolute(fs::path(g_exeDir) / kModelDirRel);
        std::wstring sLower = ToLower(selectedAbs.native());
        std::wstring bLower = ToLower(base55.native());
        if (!bLower.empty() && bLower.back() != L'\\' && bLower.back() != L'/') bLower += L"\\";
        if (sLower.rfind(bLower, 0) == 0) {
            fs::path rel = fs::relative(selectedAbs, fs::path(g_exeDir));
            if (okIn55ai) *okIn55ai = true;
            return NormalizeSlashes(rel.generic_wstring());
        }
    } catch (...) {}
    return selected;
}

bool ExistingModelIsIn55ai(const std::wstring& modelPath) {
    bool ok = false;
    Make55aiPortablePath(ResolvePathFromExeDir(modelPath), &ok);
    return ok;
}

bool RegexString(const std::string& src, const char* key, std::wstring& out) {
    try {
        std::regex re(std::string("\\\"") + key + "\\\"\\s*:\\s*\\\"((?:\\\\.|[^\\\"])*)\\\"");
        std::smatch m;
        if (std::regex_search(src, m, re)) {
            out = JsonUnescapeToWide(m[1].str());
            return true;
        }
    } catch (...) {}
    return false;
}

bool RegexBool(const std::string& src, const char* key, bool& out) {
    try {
        std::regex re(std::string("\\\"") + key + "\\\"\\s*:\\s*(true|false)", std::regex::icase);
        std::smatch m;
        if (std::regex_search(src, m, re)) {
            std::string v = m[1].str();
            std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
            out = (v == "true");
            return true;
        }
    } catch (...) {}
    return false;
}

bool RegexDouble(const std::string& src, const char* key, double& out) {
    try {
        std::regex re(std::string("\\\"") + key + "\\\"\\s*:\\s*(-?\\d+(?:\\.\\d+)?)");
        std::smatch m;
        if (std::regex_search(src, m, re)) {
            out = std::stod(m[1].str());
            return true;
        }
    } catch (...) {}
    return false;
}

bool RegexInt(const std::string& src, const char* key, int& out) {
    try {
        std::regex re(std::string("\\\"") + key + "\\\"\\s*:\\s*(-?\\d+)");
        std::smatch m;
        if (std::regex_search(src, m, re)) {
            out = std::stoi(m[1].str());
            return true;
        }
    } catch (...) {}
    return false;
}

double Clamp01(double v) {
    if (v < 0.0) return 0.0;
    if (v > 1.0) return 1.0;
    return v;
}

void LoadSettings() {
    std::string text = ReadTextFileUtf8(g_configPath);
    if (text.empty()) return;
    // v17: prefer preview_enabled, but keep enabled for backward compatibility.
    bool previewFromNewKey = g_settings.enabled;
    if (RegexBool(text, "preview_enabled", previewFromNewKey)) {
        g_settings.enabled = previewFromNewKey;
    } else {
        RegexBool(text, "enabled", g_settings.enabled);
    }
    RegexString(text, "engine1_model", g_settings.engine1Model);
    RegexString(text, "engine2_model", g_settings.engine2Model);
    RegexBool(text, "use_engine2", g_settings.useEngine2);
    RegexString(text, "blend_mode", g_settings.blendMode);
    RegexDouble(text, "engine1_amount", g_settings.engine1Amount);
    RegexDouble(text, "engine2_amount", g_settings.engine2Amount);
    RegexDouble(text, "original_opacity", g_settings.originalOpacity);
    RegexBool(text, "diff_luma_only", g_settings.diffLumaOnly);
    RegexBool(text, "auto_apply", g_settings.autoApply);
    RegexString(text, "test_mode", g_settings.testMode);
    RegexBool(text, "always_on_top", g_settings.alwaysOnTop);
    RegexInt(text, "runtime_serial", g_settings.runtimeSerial);
    // v12: ignore older chain_managed setting. The UI now auto-preserves existing chain position.
    std::wstring lang;
    if (RegexString(text, "language", lang) && (lang == L"ja" || lang == L"en")) {
        g_settings.language = lang;
    }
    if (!ExistingModelIsIn55ai(g_settings.engine1Model)) {
        g_settings.engine1Model = Settings{}.engine1Model;
    }
    if (!ExistingModelIsIn55ai(g_settings.engine2Model)) {
        g_settings.engine2Model = Settings{}.engine2Model;
    }
}

void SaveSettings() {
    EnsureDirectories();
    g_settings.engine1Amount = Clamp01(g_settings.engine1Amount);
    g_settings.engine2Amount = Clamp01(g_settings.engine2Amount);
    g_settings.originalOpacity = Clamp01(g_settings.originalOpacity);

    // Keep the config portable and restrict the normal GUI path to 55ai.
    if (!ExistingModelIsIn55ai(g_settings.engine1Model)) {
        bool ok = false;
        std::wstring p = Make55aiPortablePath(ResolvePathFromExeDir(g_settings.engine1Model), &ok);
        if (ok) g_settings.engine1Model = p;
    }
    if (!ExistingModelIsIn55ai(g_settings.engine2Model)) {
        bool ok = false;
        std::wstring p = Make55aiPortablePath(ResolvePathFromExeDir(g_settings.engine2Model), &ok);
        if (ok) g_settings.engine2Model = p;
    }

    std::ostringstream ss;
    ss.setf(std::ios::fixed);
    ss.precision(3);
    ss << "{\n";
    ss << "  \"preview_enabled\": " << (g_settings.enabled ? "true" : "false") << ",\n";
    ss << "  \"enabled\": " << (g_settings.enabled ? "true" : "false") << ",\n";  // legacy alias
    ss << "  \"engine1_model\": \"" << JsonEscapeUtf8(g_settings.engine1Model) << "\",\n";
    ss << "  \"engine2_model\": \"" << JsonEscapeUtf8(g_settings.engine2Model) << "\",\n";
    ss << "  \"use_engine2\": " << (g_settings.useEngine2 ? "true" : "false") << ",\n";
    ss << "  \"blend_mode\": \"" << JsonEscapeUtf8(g_settings.blendMode) << "\",\n";
    ss << "  \"engine1_amount\": " << g_settings.engine1Amount << ",\n";
    ss << "  \"engine2_amount\": " << g_settings.engine2Amount << ",\n";
    ss << "  \"original_opacity\": " << g_settings.originalOpacity << ",\n";
    ss << "  \"diff_luma_only\": " << (g_settings.diffLumaOnly ? "true" : "false") << ",\n";
    ss << "  \"auto_apply\": " << (g_settings.autoApply ? "true" : "false") << ",\n";
    ss << "  \"test_mode\": \"" << JsonEscapeUtf8(g_settings.testMode) << "\",\n";
    ss << "  \"language\": \"" << JsonEscapeUtf8(g_settings.language) << "\",\n";
    ss << "  \"always_on_top\": " << (g_settings.alwaysOnTop ? "true" : "false") << ",\n";
    ss << "  \"runtime_serial\": " << g_settings.runtimeSerial << ",\n";
    ss << "  \"fp16_qnt\": true,\n";
    ss << "  \"gpu\": 0,\n";
    ss << "  \"gpu_t\": 2,\n";
    ss << "  \"h_pre\": 1080,\n";
    ss << "  \"h_max\": 2160,\n";
    ss << "  \"lk_fmt\": false,\n";
    ss << "  \"source_aspect_mode\": \"keep\",\n";
    ss << "  \"show_compare_labels\": true,\n";
    ss << "  \"split_line_width\": 10,\n";
    ss << "  \"show_text\": true\n";
    ss << "}\n";

    const std::string jsonText = ss.str();

    std::ofstream ofs(fs::path(g_configPath), std::ios::binary | std::ios::trunc);
    ofs << jsonText;

    // v15 compatibility:
    // Some existing input.conf entries may still load the old script name:
    //   portable_config\vs\onnx_blend_gui.vpy
    // Older vpy files look for:
    //   portable_config\cache\onnx_blend_gui\settings.json
    // Write the same settings there as well so the UI controls still affect
    // a legacy onnx_blend_gui.vpy filter already registered by mpv_filter_chain_UI.
    try {
        std::wstring legacyConfigDir = (fs::path(g_exeDir) / L"portable_config" / L"cache" / L"onnx_blend_gui").wstring();
        fs::create_directories(fs::path(legacyConfigDir));
        std::wstring legacyConfigPath = (fs::path(legacyConfigDir) / L"settings.json").wstring();
        std::ofstream legacyOfs(fs::path(legacyConfigPath), std::ios::binary | std::ios::trunc);
        legacyOfs << jsonText;
    } catch (...) {
    }
}

void SetStatus(const std::wstring& msg) {
    if (hStatus) SetWindowTextW(hStatus, msg.c_str());
}

std::wstring GetWindowTextString(HWND h) {
    int len = GetWindowTextLengthW(h);
    if (len <= 0) return L"";
    std::wstring s(len + 1, L'\0');
    GetWindowTextW(h, s.data(), len + 1);
    s.resize(len);
    return s;
}

void SetCheck(HWND h, bool checked) {
    if (h) SendMessageW(h, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
}

bool IsChecked(HWND h) {
    return h && SendMessageW(h, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

void SetComboText(HWND h, const std::wstring& text) {
    if (!h) return;
    int count = static_cast<int>(SendMessageW(h, CB_GETCOUNT, 0, 0));
    for (int i = 0; i < count; ++i) {
        wchar_t buf[128] = {};
        SendMessageW(h, CB_GETLBTEXT, i, reinterpret_cast<LPARAM>(buf));
        if (_wcsicmp(buf, text.c_str()) == 0) {
            SendMessageW(h, CB_SETCURSEL, i, 0);
            return;
        }
    }
    SendMessageW(h, CB_SETCURSEL, 0, 0);
}

std::wstring GetComboText(HWND h) {
    int sel = static_cast<int>(SendMessageW(h, CB_GETCURSEL, 0, 0));
    if (sel == CB_ERR) return L"";
    wchar_t buf[128] = {};
    SendMessageW(h, CB_GETLBTEXT, sel, reinterpret_cast<LPARAM>(buf));
    return buf;
}

std::wstring FormatAmount(double v) {
    wchar_t buf[32] = {};
    swprintf_s(buf, L"%.2f", Clamp01(v));
    return buf;
}

void SetTrack(HWND track, HWND edit, double v) {
    int pos = static_cast<int>(std::lround(Clamp01(v) * 100.0));
    SendMessageW(track, TBM_SETPOS, TRUE, pos);
    SetWindowTextW(edit, FormatAmount(v).c_str());
}

double GetTrackAmount(HWND track) {
    int pos = static_cast<int>(SendMessageW(track, TBM_GETPOS, 0, 0));
    return Clamp01(pos / 100.0);
}

void InitTrack(HWND h) {
    SendMessageW(h, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
    SendMessageW(h, TBM_SETTICFREQ, 10, 0);
}

void SetComboItems(HWND combo, const std::vector<std::wstring>& items, const std::wstring& current) {
    if (!combo) return;
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    for (const auto& item : items) {
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item.c_str()));
    }
    SetComboText(combo, current);
}

void GatherSettingsFromUi() {
    if (!g_hwnd || g_loading) return;
    g_settings.enabled = IsChecked(hEnable);
    g_settings.engine1Model = GetWindowTextString(hEngine1Edit);
    g_settings.engine2Model = GetWindowTextString(hEngine2Edit);
    g_settings.useEngine2 = IsChecked(hUseEngine2);
    g_settings.blendMode = GetComboText(hBlendMode);
    g_settings.testMode = GetComboText(hCompareMode);
    try { g_settings.engine1Amount = Clamp01(std::stod(GetWindowTextString(hEngine1Value))); }
    catch (...) { g_settings.engine1Amount = GetTrackAmount(hEngine1Track); }
    try { g_settings.engine2Amount = Clamp01(std::stod(GetWindowTextString(hEngine2Value))); }
    catch (...) { g_settings.engine2Amount = GetTrackAmount(hEngine2Track); }
    try { g_settings.originalOpacity = Clamp01(std::stod(GetWindowTextString(hOriginalValue))); }
    catch (...) { g_settings.originalOpacity = GetTrackAmount(hOriginalTrack); }
    SetTrack(hEngine1Track, hEngine1Value, g_settings.engine1Amount);
    SetTrack(hEngine2Track, hEngine2Value, g_settings.engine2Amount);
    SetTrack(hOriginalTrack, hOriginalValue, g_settings.originalOpacity);
    g_settings.diffLumaOnly = IsChecked(hLumaOnly);
    g_settings.autoApply = IsChecked(hAutoApply);
    g_settings.alwaysOnTop = IsChecked(hAlwaysOnTop);
}

void UpdateLanguageUi() {
    if (!g_hwnd) return;

    SetWindowTextW(g_hwnd, L"mpv ONNX Blend UI");
    if (hTitle) SetWindowTextW(hTitle, L"ONNX Blend Filter");
    if (hEnable) SetWindowTextW(hEnable, Txt(L"画質プレビュー", L"Quality preview"));
    if (hAutoApply) SetWindowTextW(hAutoApply, Txt(L"自動反映", L"Auto apply"));
    if (hAlwaysOnTop) SetWindowTextW(hAlwaysOnTop, Txt(L"常に最前面へ", L"Always on top"));
    if (hLanguage) SetWindowTextW(hLanguage, L"言語/language");

    if (hEngine1Label) SetWindowTextW(hEngine1Label, Txt(L"Engine 1 ONNX", L"Engine 1 ONNX"));
    if (hEngine1Browse) SetWindowTextW(hEngine1Browse, Txt(L"選択", L"Select"));
    if (hUseEngine2) SetWindowTextW(hUseEngine2, Txt(L"Engine 2 を使う", L"Use Engine 2"));
    if (hEngine2Label) SetWindowTextW(hEngine2Label, Txt(L"Engine 2 ONNX", L"Engine 2 ONNX"));
    if (hEngine2Browse) SetWindowTextW(hEngine2Browse, Txt(L"選択", L"Select"));
    if (hSwap) SetWindowTextW(hSwap, Txt(L"1 / 2 入替", L"Swap 1 / 2"));

    if (hBlendModeLabel) SetWindowTextW(hBlendModeLabel, Txt(L"ブレンド方式", L"Blend mode"));
    if (hLumaOnly) SetWindowTextW(hLumaOnly, Txt(L"輝度のみ", L"Luma only"));
    if (hCompareLabel) SetWindowTextW(hCompareLabel, Txt(L"比較表示", L"Compare"));

    if (hEngine1AmountLabel) SetWindowTextW(hEngine1AmountLabel, Txt(L"Engine 1 強度", L"Engine 1 amount"));
    if (hEngine2AmountLabel) SetWindowTextW(hEngine2AmountLabel, Txt(L"Engine 2 強度", L"Engine 2 amount"));
    if (hOriginalOpacityLabel) SetWindowTextW(hOriginalOpacityLabel, Txt(L"元映像の濃さ", L"Original opacity"));

    // Apply / Remove buttons are not used. Quality preview checkbox is the main ON/OFF control.
    if (hResetButton) SetWindowTextW(hResetButton, Txt(L"初期化", L"Reset"));
    if (hOpenSettingsButton) SetWindowTextW(hOpenSettingsButton, Txt(L"設定フォルダを開く", L"Open settings folder"));
}

void ApplyAlwaysOnTop() {
    if (!g_hwnd) return;
    SetWindowPos(g_hwnd,
                 g_settings.alwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST,
                 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void RefreshUiFromSettings() {
    g_loading = true;
    SetCheck(hEnable, g_settings.enabled);
    SetWindowTextW(hEngine1Edit, g_settings.engine1Model.c_str());
    SetWindowTextW(hEngine2Edit, g_settings.engine2Model.c_str());
    SetCheck(hUseEngine2, g_settings.useEngine2);
    SetComboItems(hBlendMode, {L"alpha", L"residual"}, g_settings.blendMode);
    SetComboItems(hCompareMode, {L"off", L"blend", L"half", L"side_by_side", L"simple2x", L"engine1_only", L"engine2_only"}, g_settings.testMode);
    SetTrack(hEngine1Track, hEngine1Value, g_settings.engine1Amount);
    SetTrack(hEngine2Track, hEngine2Value, g_settings.engine2Amount);
    SetTrack(hOriginalTrack, hOriginalValue, g_settings.originalOpacity);
    SetCheck(hLumaOnly, g_settings.diffLumaOnly);
    SetCheck(hAutoApply, g_settings.autoApply);
    SetCheck(hAlwaysOnTop, g_settings.alwaysOnTop);
    EnableWindow(hEngine2Edit, g_settings.useEngine2);
    EnableWindow(hEngine2Browse, g_settings.useEngine2);
    EnableWindow(hEngine2Track, g_settings.useEngine2);
    EnableWindow(hEngine2Value, g_settings.useEngine2);
    UpdateLanguageUi();
    ApplyAlwaysOnTop();
    g_loading = false;
}

bool WritePipeUtf8(const std::string& line) {
    std::string msg = line;
    if (msg.empty() || msg.back() != '\n') msg.push_back('\n');

    for (int retry = 0; retry < 8; ++retry) {
        HANDLE hPipe = CreateFileW(kPipeName, GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (hPipe == INVALID_HANDLE_VALUE) {
            DWORD err = GetLastError();
            if (err == ERROR_PIPE_BUSY) {
                WaitNamedPipeW(kPipeName, 80);
                Sleep(20);
                continue;
            }
            Sleep(20);
            continue;
        }

        DWORD written = 0;
        BOOL ok = WriteFile(hPipe, msg.data(), static_cast<DWORD>(msg.size()), &written, nullptr);
        CloseHandle(hPipe);
        if (ok && written == msg.size()) return true;
        Sleep(30);
    }
    return false;
}

bool TestMpvPipe() {
    HANDLE hPipe = CreateFileW(kPipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (hPipe != INVALID_HANDLE_VALUE) {
        CloseHandle(hPipe);
        g_everConnectedToMpv = true;
        return true;
    }
    if (GetLastError() == ERROR_PIPE_BUSY) {
        g_everConnectedToMpv = true;
        return true;
    }
    return false;
}

std::string JsonEscapeUtf8Raw(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 16);
    for (unsigned char ch : s) {
        switch (ch) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (ch < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", ch);
                out += buf;
            } else {
                out += static_cast<char>(ch);
            }
        }
    }
    return out;
}

std::string BuildMpvCommandJson(const std::vector<std::wstring>& args, int requestId = 0) {
    std::string json = "{\"command\":[";
    for (size_t i = 0; i < args.size(); ++i) {
        if (i) json += ",";
        json += "\"" + JsonEscapeUtf8(args[i]) + "\"";
    }
    json += "]";
    if (requestId > 0) {
        json += ",\"request_id\":" + std::to_string(requestId);
    }
    json += "}";
    return json;
}

bool SendMpvCommandRaw(const std::vector<std::wstring>& args) {
    return WritePipeUtf8(BuildMpvCommandJson(args));
}

bool ExtractJsonDataString(const std::string& json, std::wstring* out) {
    if (!out) return false;
    size_t p = json.find("\"data\"");
    if (p == std::string::npos) return false;
    p = json.find(':', p);
    if (p == std::string::npos) return false;
    ++p;
    while (p < json.size() && (json[p] == ' ' || json[p] == '\t' || json[p] == '\r' || json[p] == '\n')) ++p;
    if (p >= json.size() || json.compare(p, 4, "null") == 0 || json[p] != '"') return false;
    ++p;

    std::string value;
    bool esc = false;
    for (; p < json.size(); ++p) {
        char ch = json[p];
        if (esc) {
            value.push_back('\\');
            value.push_back(ch);
            esc = false;
        } else if (ch == '\\') {
            esc = true;
        } else if (ch == '"') {
            *out = JsonUnescapeToWide(value);
            return true;
        } else {
            value.push_back(ch);
        }
    }
    return false;
}

bool ReadMpvPropertyString(const std::wstring& property, std::wstring* out) {
    if (!out) return false;
    std::string request = BuildMpvCommandJson({L"get_property_string", property}, 9711) + "\n";

    for (int retry = 0; retry < 6; ++retry) {
        HANDLE hPipe = CreateFileW(kPipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (hPipe == INVALID_HANDLE_VALUE) {
            if (GetLastError() == ERROR_PIPE_BUSY) WaitNamedPipeW(kPipeName, 80);
            Sleep(30);
            continue;
        }

        DWORD written = 0;
        BOOL ok = WriteFile(hPipe, request.data(), static_cast<DWORD>(request.size()), &written, nullptr);
        if (!ok || written != request.size()) {
            CloseHandle(hPipe);
            Sleep(30);
            continue;
        }
        FlushFileBuffers(hPipe);

        std::string response;
        char buf[4096];
        for (int i = 0; i < 30; ++i) {
            DWORD available = 0;
            if (!PeekNamedPipe(hPipe, nullptr, 0, nullptr, &available, nullptr)) break;
            if (available == 0) {
                Sleep(10);
                continue;
            }
            DWORD toRead = available < sizeof(buf) - 1 ? available : static_cast<DWORD>(sizeof(buf) - 1);
            DWORD read = 0;
            if (!ReadFile(hPipe, buf, toRead, &read, nullptr) || read == 0) break;
            response.append(buf, buf + read);
            if (response.find('\n') != std::string::npos || response.size() > 65536) break;
        }
        CloseHandle(hPipe);

        std::wstring parsed;
        if (ExtractJsonDataString(response, &parsed)) {
            *out = parsed;
            g_everConnectedToMpv = true;
            return true;
        }
        Sleep(30);
    }
    return false;
}

bool VFilterStringContainsOnnxBlend(const std::wstring& vf) {
    std::wstring s = ToLower(vf);
    return s.find(L"@onnx_blend_ui") != std::wstring::npos ||
           s.find(L"onnx_blend_ui.vpy") != std::wstring::npos ||
           s.find(L"@onnx_blend_gui") != std::wstring::npos ||
           s.find(L"onnx_blend_gui.vpy") != std::wstring::npos;
}

void SyncPreviewCheckboxFromMpvChain(bool filterPresent) {
    if (!g_hwnd || g_loading) return;
    if (g_settings.enabled == filterPresent && IsChecked(hEnable) == filterPresent) return;

    // Preserve any current UI edits, then overwrite only the preview ON/OFF state
    // to mirror the actual mpv vf chain. Do not send add/remove commands here:
    // this is an external-state reflection path only.
    GatherSettingsFromUi();
    g_settings.enabled = filterPresent;
    SetCheck(hEnable, filterPresent);
    SaveSettings();
    KillTimer(g_hwnd, kAutoApplyTimer);

    SetStatus(filterPresent
        ? TxtS(L"mpv側の状態を反映しました。画質プレビュー: ON",
               L"Synced from mpv. Quality preview: ON")
        : TxtS(L"mpv側の状態を反映しました。画質プレビュー: OFF",
               L"Synced from mpv. Quality preview: OFF"));
}

void SyncPreviewStateFromMpvIfNeeded() {
    if (!g_hwnd || g_loading || g_applyingToMpv) return;

    ULONGLONG now = GetTickCount64();
    if (g_lastMpvFilterSyncTick != 0 &&
        now - g_lastMpvFilterSyncTick < kMpvFilterSyncIntervalMs) {
        return;
    }
    g_lastMpvFilterSyncTick = now;

    std::wstring vfString;
    if (!ReadMpvPropertyString(L"vf", &vfString)) return;
    SyncPreviewCheckboxFromMpvChain(VFilterStringContainsOnnxBlend(vfString));
}

bool ReloadExistingOnnxBlendInCurrentChain(bool showMissingStatus) {
    std::wstring vfString;
    if (!ReadMpvPropertyString(L"vf", &vfString)) {
        SetStatus(TxtS(L"保存しました。mpv pipe または現在のvfを読めません。", L"Saved. Could not read the current mpv vf chain."));
        return false;
    }

    if (!VFilterStringContainsOnnxBlend(vfString)) {
        if (showMissingStatus) {
            SetStatus(TxtS(L"保存しました。onnx_blend は現在のチェーンにありません。mpv_filter_chain_UIから追加してください。",
                           L"Saved. onnx_blend is not in the current chain. Add it from mpv_filter_chain_UI."));
        }
        return false;
    }

    bool ok = false;
    for (int retry = 0; retry < 8 && !ok; ++retry) {
        ok = SendMpvCommandRaw({L"vf", L"set", vfString});
        if (!ok) Sleep(120);
    }
    SetStatus(ok
        ? TxtS(L"保存しました。現在のチェーン内の onnx_blend を再読み込みしました。",
               L"Saved. Reloaded onnx_blend inside the current chain.")
        : TxtS(L"保存しましたが、現在チェーンの再読み込みに失敗しました。",
               L"Saved, but failed to reload the current chain."));
    return ok;
}

std::wstring VpyRelativePathForMpv() {
    // mpv-cHiDeNoise-AI portable layout:
    //   portable_config\vs\onnx_blend_ui.vpy
    // Use mpv's ~~ prefix so the path is resolved from portable_config,
    // without a Windows drive-letter ':' in the filter option string.
    std::wstring portableVsPath = (fs::path(g_exeDir) / L"portable_config" / L"vs" / L"onnx_blend_ui.vpy").wstring();
    if (FileExists(portableVsPath)) return L"~~/vs/onnx_blend_ui.vpy";

    // Backward-compatible fallbacks for earlier test layouts.
    std::wstring rootVsPath = (fs::path(g_exeDir) / L"vs" / L"onnx_blend_ui.vpy").wstring();
    if (FileExists(rootVsPath)) return L"vs/onnx_blend_ui.vpy";

    return L"onnx_blend_ui.vpy";
}

bool SendMpvRemove() {
    bool removedNew = WritePipeUtf8("{\"command\":[\"vf\",\"remove\",\"@onnx_blend_ui\"]}");
    bool removedLegacy = WritePipeUtf8("{\"command\":[\"vf\",\"remove\",\"@onnx_blend_gui\"]}");
    return removedNew || removedLegacy;
}

bool SendMpvAppend() {
    // Keep this relative so Windows drive-letter ':' does not interfere with mpv's filter option parser.
    std::wstring rel = VpyRelativePathForMpv();
    std::string filter = "@onnx_blend_ui:vapoursynth=file=" + WideToUtf8(rel);
    std::string json = "{\"command\":[\"vf\",\"add\",\"";
    for (char c : filter) {
        if (c == '\\') json += "\\\\";
        else if (c == '"') json += "\\\"";
        else json += c;
    }
    json += "\"]}";
    return WritePipeUtf8(json);
}

void BumpRuntimeSerial() {
    if (g_settings.runtimeSerial < 0 || g_settings.runtimeSerial > 1000000000) {
        g_settings.runtimeSerial = 0;
    }
    ++g_settings.runtimeSerial;
}

void ApplyToMpv(bool appendIfEnabled, bool allowAppendIfMissing) {
    if (g_applyingToMpv) {
        g_pendingApply = true;
        return;
    }

    g_applyingToMpv = true;
    do {
        g_pendingApply = false;
        GatherSettingsFromUi();
        BumpRuntimeSerial();
        SaveSettings();

        if (!appendIfEnabled || !g_settings.enabled) {
            bool removed = SendMpvRemove();
            SetStatus(removed ? TxtS(L"画質プレビューをOFFにしました。", L"Quality preview was turned OFF.")
                              : TxtS(L"保存しました。mpv pipe が見つかりません。", L"Saved. mpv pipe not found."));
            continue;
        }

        // If mpv_filter_chain_UI already placed @onnx_blend_ui somewhere in the vf chain,
        // reload the same vf string. This keeps its chain position unchanged while making
        // onnx_blend_ui.vpy read the latest settings.json.
        if (ReloadExistingOnnxBlendInCurrentChain(false)) {
            continue;
        }

        // Auto apply may reload an existing filter, but it should not unexpectedly add a new
        // one unless this call explicitly allows it (e.g. Quality Preview was turned ON).
        if (!allowAppendIfMissing) {
            SetStatus(TxtS(L"保存しました。onnx_blend が現在のチェーンに無いため、自動追加はしません。画質プレビューをONにすると登録します。",
                           L"Saved. onnx_blend is not in the current chain, so it was not auto-added. Turn Quality preview ON to register it."));
            continue;
        }

        // Standalone preview fallback: register/reload the filter from this GUI.
        // This is intentionally allowed for practical realtime quality checking.
        SendMpvRemove();
        Sleep(80);

        bool appended = false;
        for (int retry = 0; retry < 10 && !appended; ++retry) {
            appended = SendMpvAppend();
            if (!appended) Sleep(150);
        }

        if (appended) {
            SetStatus(TxtS(L"画質プレビューをONにしました。", L"Quality preview was turned ON."));
        } else {
            SetStatus(TxtS(L"保存しましたが、mpvへの登録に失敗しました。画質プレビューを一度OFF→ONにして再試行してください。",
                           L"Saved, but registering the filter failed. Turn Quality preview OFF and ON to retry."));
        }
    } while (g_pendingApply);

    g_applyingToMpv = false;
}

void SaveSettingsOnly(const wchar_t* jaStatus, const wchar_t* enStatus) {
    if (g_loading) return;
    GatherSettingsFromUi();
    SaveSettings();
    KillTimer(g_hwnd, kAutoApplyTimer);
    SetStatus(TxtS(jaStatus, enStatus));
}

void ScheduleAutoApply();

void ScheduleDynamicAmountUpdate() {
    // v18: amount/mode changes are handled like other changes: save settings.json,
    // then debounce a normal filter reload when Auto apply is ON. This restores
    // practical realtime checking while keeping the operation reasonably robust.
    ScheduleAutoApply();
}

void ScheduleAutoApply() {
    if (g_loading) return;
    GatherSettingsFromUi();
    if (g_settings.autoApply) {
        BumpRuntimeSerial();
        SaveSettings();
        KillTimer(g_hwnd, kAutoApplyTimer);
        SetTimer(g_hwnd, kAutoApplyTimer, kAutoApplyDelayMs, nullptr);
        SetStatus(TxtS(L"保存しました。画質プレビュー再反映待ちです...", L"Saved. Quality preview reload pending..."));
    } else {
        SaveSettings();
        SetStatus(TxtS(L"保存しました。自動反映OFFのため、フィルターは再読み込みしていません。",
                       L"Saved. Auto apply is OFF, so the filter was not reloaded."));
    }
}


HMENU ControlId(int id);

struct OnnxModelItem {
    std::wstring display;
    std::wstring rel;
};

bool IsOnnxFilePath(const fs::path& p) {
    std::wstring ext = ToLower(p.extension().wstring());
    return ext == L".onnx";
}

std::vector<OnnxModelItem> Enumerate55aiModels() {
    std::vector<OnnxModelItem> out;
    try {
        fs::path base55 = fs::path(g_exeDir) / kModelDirRel;
        if (!fs::exists(base55) || !fs::is_directory(base55)) return out;

        for (const auto& e : fs::recursive_directory_iterator(base55)) {
            if (!e.is_regular_file()) continue;
            if (!IsOnnxFilePath(e.path())) continue;

            fs::path relFromExe = fs::relative(e.path(), fs::path(g_exeDir));
            fs::path relFrom55 = fs::relative(e.path(), base55);
            OnnxModelItem item;
            item.rel = NormalizeSlashes(relFromExe.generic_wstring());
            item.display = NormalizeSlashes(relFrom55.generic_wstring());
            out.push_back(item);
        }
    } catch (...) {}

    std::sort(out.begin(), out.end(), [](const OnnxModelItem& a, const OnnxModelItem& b) {
        return ToLower(a.display) < ToLower(b.display);
    });
    return out;
}

struct ModelSelectState {
    HWND owner = nullptr;
    HWND hwnd = nullptr;
    HWND hList = nullptr;
    std::vector<OnnxModelItem> models;
    std::wstring current;
    std::wstring selected;
    bool done = false;
    bool accepted = false;
};

LRESULT CALLBACK ModelSelectProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    ModelSelectState* st = reinterpret_cast<ModelSelectState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
    case WM_CREATE: {
        CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        st = reinterpret_cast<ModelSelectState*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));
        st->hwnd = hwnd;

        HFONT font = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        HWND label = CreateWindowExW(0, L"STATIC",
            Txt(L"vs-plugins\\models\\55ai フォルダ内のONNXだけを選択できます。", L"Select an ONNX model from vs-plugins\\models\\55ai."),
            WS_CHILD | WS_VISIBLE,
            14, 12, 490, 22, hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
        st->hList = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
            14, 40, 490, 280, hwnd, ControlId(IDC_MODEL_LIST), GetModuleHandleW(nullptr), nullptr);
        HWND ok = CreateWindowExW(0, L"BUTTON", Txt(L"選択", L"Select"),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
            318, 334, 86, 30, hwnd, ControlId(IDOK), GetModuleHandleW(nullptr), nullptr);
        HWND cancel = CreateWindowExW(0, L"BUTTON", Txt(L"キャンセル", L"Cancel"),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP,
            418, 334, 86, 30, hwnd, ControlId(IDCANCEL), GetModuleHandleW(nullptr), nullptr);

        SendMessageW(label, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        SendMessageW(st->hList, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        SendMessageW(ok, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        SendMessageW(cancel, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);

        int selectIndex = 0;
        std::wstring cur = ToLower(NormalizeSlashes(st->current));
        for (size_t i = 0; i < st->models.size(); ++i) {
            SendMessageW(st->hList, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(st->models[i].display.c_str()));
            if (ToLower(st->models[i].rel) == cur) selectIndex = static_cast<int>(i);
        }
        if (!st->models.empty()) SendMessageW(st->hList, LB_SETCURSEL, selectIndex, 0);
        SetFocus(st->hList);
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(wp);
        int code = HIWORD(wp);
        if (id == IDOK || (id == IDC_MODEL_LIST && code == LBN_DBLCLK)) {
            int sel = static_cast<int>(SendMessageW(st->hList, LB_GETCURSEL, 0, 0));
            if (sel >= 0 && sel < static_cast<int>(st->models.size())) {
                st->selected = st->models[static_cast<size_t>(sel)].rel;
                st->accepted = true;
                st->done = true;
                DestroyWindow(hwnd);
            }
            return 0;
        }
        if (id == IDCANCEL) {
            st->accepted = false;
            st->done = true;
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    }
    case WM_CLOSE:
        if (st) {
            st->accepted = false;
            st->done = true;
        }
        DestroyWindow(hwnd);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool ShowOnnxModelDialog(const std::wstring& current, std::wstring* outRelPath) {
    if (!outRelPath) return false;

    std::vector<OnnxModelItem> models = Enumerate55aiModels();
    if (models.empty()) {
        MessageBoxW(g_hwnd,
                    Txt(L"vs-plugins\\models\\55ai フォルダ内に .onnx が見つかりません。",
                        L"No .onnx files were found in vs-plugins\\models\\55ai."),
                    kAppTitle,
                    MB_OK | MB_ICONWARNING);
        return false;
    }

    static bool registered = false;
    static const wchar_t* kModelDialogClass = L"onnx_blend_UI_model_select_dialog_v6";
    if (!registered) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc = ModelSelectProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kModelDialogClass;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        RegisterClassW(&wc);
        registered = true;
    }

    ModelSelectState st;
    st.owner = g_hwnd;
    st.models = std::move(models);
    st.current = NormalizeSlashes(current);

    RECT rc{};
    GetWindowRect(g_hwnd, &rc);
    int w = 535;
    int h = 410;
    int x = rc.left + ((rc.right - rc.left) - w) / 2;
    int y = rc.top + ((rc.bottom - rc.top) - h) / 2;

    EnableWindow(g_hwnd, FALSE);
    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME,
                               kModelDialogClass,
                               Txt(L"ONNXモデル選択", L"Select ONNX Model"),
                               WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
                               x, y, w, h,
                               g_hwnd, nullptr, GetModuleHandleW(nullptr), &st);
    if (!dlg) {
        EnableWindow(g_hwnd, TRUE);
        SetForegroundWindow(g_hwnd);
        return false;
    }

    MSG msg{};
    while (!st.done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(dlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    EnableWindow(g_hwnd, TRUE);
    SetForegroundWindow(g_hwnd);

    if (st.accepted) {
        *outRelPath = st.selected;
        return true;
    }
    return false;
}

void BrowseOnnx(HWND edit) {
    std::wstring selected;
    if (ShowOnnxModelDialog(GetWindowTextString(edit), &selected)) {
        SetWindowTextW(edit, selected.c_str());
        ScheduleAutoApply();
    }
}

HMENU ControlId(int id) {
    return reinterpret_cast<HMENU>(static_cast<INT_PTR>(id));
}

HWND MakeControl(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int width, int height, HMENU id) {
    return CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style,
                           x, y, width, height, g_hwnd, id, GetModuleHandleW(nullptr), nullptr);
}

HWND MakeButton(const wchar_t* text, int x, int y, int width, int height, int id) {
    return MakeControl(L"BUTTON", text, BS_PUSHBUTTON | WS_TABSTOP, x, y, width, height, ControlId(id));
}

HWND MakeCheck(const wchar_t* text, int x, int y, int width, int height, int id) {
    return MakeControl(L"BUTTON", text, BS_AUTOCHECKBOX | WS_TABSTOP, x, y, width, height, ControlId(id));
}

HWND MakeEdit(const wchar_t* text, int x, int y, int width, int height, int id) {
    return CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", text,
                           WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                           x, y, width, height, g_hwnd, ControlId(id), GetModuleHandleW(nullptr), nullptr);
}

HWND MakeCombo(int x, int y, int width, int height, int id, const std::vector<std::wstring>& items) {
    HWND hwndCombo = CreateWindowExW(0, WC_COMBOBOXW, L"",
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
                                     x, y, width, height, g_hwnd, ControlId(id), GetModuleHandleW(nullptr), nullptr);
    for (const auto& item : items) {
        SendMessageW(hwndCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item.c_str()));
    }
    SendMessageW(hwndCombo, CB_SETCURSEL, 0, 0);
    return hwndCombo;
}

HWND MakeTrack(int x, int y, int width, int height, int id) {
    HWND hwndTrack = CreateWindowExW(0, TRACKBAR_CLASSW, L"",
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_AUTOTICKS,
                                     x, y, width, height, g_hwnd, ControlId(id), GetModuleHandleW(nullptr), nullptr);
    InitTrack(hwndTrack);
    return hwndTrack;
}

void CreateUi(HWND hwnd) {
    g_hwnd = hwnd;
    HFONT font = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));

    hTitle = MakeControl(L"STATIC", L"ONNX Blend Filter", 0, 16, 12, 240, 24, nullptr);
    hLanguage = MakeButton(L"言語/language", 642, 10, 110, 28, IDC_LANGUAGE);
    hEnable = MakeCheck(L"画質プレビュー", 16, 42, 150, 24, IDC_ENABLE);
    hAutoApply = MakeCheck(L"自動反映", 180, 42, 120, 24, IDC_AUTO_APPLY);
    hAlwaysOnTop = MakeCheck(L"常に最前面へ", 320, 42, 150, 24, IDC_ALWAYS_ON_TOP);

    hEngine1Label = MakeControl(L"STATIC", L"Engine 1 ONNX", 0, 16, 82, 120, 22, nullptr);
    hEngine1Edit = MakeEdit(L"", 140, 78, 520, 25, IDC_ENGINE1_EDIT);
    SendMessageW(hEngine1Edit, EM_SETREADONLY, TRUE, 0);
    hEngine1Browse = MakeButton(L"選択", 670, 76, 82, 28, IDC_ENGINE1_BROWSE);

    hUseEngine2 = MakeCheck(L"Engine 2 を使う", 16, 122, 150, 24, IDC_USE_ENGINE2);
    hEngine2Label = MakeControl(L"STATIC", L"Engine 2 ONNX", 0, 16, 158, 120, 22, nullptr);
    hEngine2Edit = MakeEdit(L"", 140, 154, 520, 25, IDC_ENGINE2_EDIT);
    SendMessageW(hEngine2Edit, EM_SETREADONLY, TRUE, 0);
    hEngine2Browse = MakeButton(L"選択", 670, 152, 82, 28, IDC_ENGINE2_BROWSE);
    hSwap = MakeButton(L"1 / 2 入替", 670, 116, 82, 28, IDC_SWAP);

    hBlendModeLabel = MakeControl(L"STATIC", L"ブレンド方式", 0, 16, 202, 110, 22, nullptr);
    hBlendMode = MakeCombo(140, 198, 140, 180, IDC_BLEND_MODE, {L"alpha", L"residual"});
    hLumaOnly = MakeCheck(L"輝度のみ", 300, 198, 120, 24, IDC_LUMA_ONLY);

    hCompareLabel = MakeControl(L"STATIC", L"比較表示", 0, 440, 202, 80, 22, nullptr);
    hCompareMode = MakeCombo(520, 198, 160, 220, IDC_COMPARE_MODE, {L"off", L"blend", L"half", L"side_by_side", L"simple2x", L"engine1_only", L"engine2_only"});

    hEngine1AmountLabel = MakeControl(L"STATIC", L"Engine 1 強度", 0, 16, 248, 130, 22, nullptr);
    hEngine1Track = MakeTrack(150, 240, 430, 36, IDC_ENGINE1_TRACK);
    hEngine1Value = MakeEdit(L"0.55", 600, 244, 60, 24, IDC_ENGINE1_VALUE);

    hEngine2AmountLabel = MakeControl(L"STATIC", L"Engine 2 強度", 0, 16, 294, 130, 22, nullptr);
    hEngine2Track = MakeTrack(150, 286, 430, 36, IDC_ENGINE2_TRACK);
    hEngine2Value = MakeEdit(L"0.35", 600, 290, 60, 24, IDC_ENGINE2_VALUE);

    hOriginalOpacityLabel = MakeControl(L"STATIC", L"元映像の濃さ", 0, 16, 340, 130, 22, nullptr);
    hOriginalTrack = MakeTrack(150, 332, 430, 36, IDC_ORIGINAL_TRACK);
    hOriginalValue = MakeEdit(L"1.00", 600, 336, 60, 24, IDC_ORIGINAL_VALUE);

    // This tool is a quality settings editor.  It does not add/remove the mpv vf filter.
    // mpv_filter_chain_UI should manage insertion/removal/order.
    hResetButton = MakeButton(L"初期化", 16, 392, 110, 34, IDC_RESET);
    hOpenSettingsButton = MakeButton(L"設定フォルダを開く", 138, 392, 170, 34, IDC_OPEN_SETTINGS);

    hStatus = MakeControl(L"STATIC", L"準備完了。", 0, 16, 444, 730, 28, nullptr);

    EnumChildWindows(hwnd, [](HWND child, LPARAM lp) -> BOOL {
        SendMessageW(child, WM_SETFONT, lp, TRUE);
        return TRUE;
    }, reinterpret_cast<LPARAM>(font));
}

void ResetDefaults() {
    std::wstring keepLang = g_settings.language;
    g_settings = Settings{};
    g_settings.language = keepLang.empty() ? L"ja" : keepLang;
    RefreshUiFromSettings();
    SaveSettings();
    ScheduleAutoApply();
}

void ToggleLanguage() {
    GatherSettingsFromUi();
    g_settings.language = IsEnglish() ? L"ja" : L"en";
    SaveSettings();
    UpdateLanguageUi();
    SetStatus(IsEnglish() ? L"Language switched to English." : L"日本語UIに切り替えました。");
}

void OnCommand(HWND hwnd, WPARAM wParam, LPARAM) {
    int id = LOWORD(wParam);
    int code = HIWORD(wParam);

    if (id == IDC_LANGUAGE && code == BN_CLICKED) {
        ToggleLanguage();
    } else if (id == IDC_ALWAYS_ON_TOP && code == BN_CLICKED) {
        g_settings.alwaysOnTop = IsChecked(hAlwaysOnTop);
        ApplyAlwaysOnTop();
        SaveSettings();
        SetStatus(g_settings.alwaysOnTop ? TxtS(L"常に最前面をONにしました。", L"Always on top enabled.")
                                         : TxtS(L"常に最前面をOFFにしました。", L"Always on top disabled."));
    } else if (id == IDC_ENGINE1_BROWSE && code == BN_CLICKED) {
        BrowseOnnx(hEngine1Edit);
    } else if (id == IDC_ENGINE2_BROWSE && code == BN_CLICKED) {
        BrowseOnnx(hEngine2Edit);
    } else if (id == IDC_SWAP && code == BN_CLICKED) {
        std::wstring a = GetWindowTextString(hEngine1Edit);
        std::wstring b = GetWindowTextString(hEngine2Edit);
        SetWindowTextW(hEngine1Edit, b.c_str());
        SetWindowTextW(hEngine2Edit, a.c_str());
        ScheduleAutoApply();
    } else if (id == IDC_RESET && code == BN_CLICKED) {
        ResetDefaults();
    } else if (id == IDC_OPEN_SETTINGS && code == BN_CLICKED) {
        EnsureDirectories();
        ShellExecuteW(hwnd, L"open", g_configDir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    } else if (id == IDC_USE_ENGINE2 && code == BN_CLICKED) {
        bool use2 = IsChecked(hUseEngine2);
        EnableWindow(hEngine2Edit, use2);
        EnableWindow(hEngine2Browse, use2);
        EnableWindow(hEngine2Track, use2);
        EnableWindow(hEngine2Value, use2);
        ScheduleAutoApply();
    } else if (id == IDC_AUTO_APPLY && code == BN_CLICKED) {
        GatherSettingsFromUi();
        SaveSettings();
        if (g_settings.autoApply) {
            ScheduleAutoApply();
        } else {
            KillTimer(g_hwnd, kAutoApplyTimer);
            SetStatus(TxtS(L"自動反映をOFFにしました。設定変更は保存されますが、自動再読み込みはしません。",
                           L"Auto apply is OFF. Setting changes are saved, but the filter will not be reloaded automatically."));
        }
    } else if (id == IDC_LUMA_ONLY && code == BN_CLICKED) {
        ScheduleDynamicAmountUpdate();
    } else if (id == IDC_ENABLE && code == BN_CLICKED) {
        // v18: Quality preview is the user-facing ON/OFF switch for practical checking.
        // ON registers/reloads the filter. OFF removes it.
        GatherSettingsFromUi();
        SaveSettings();
        KillTimer(g_hwnd, kAutoApplyTimer);
        ApplyToMpv(g_settings.enabled, true);
    } else if (id == IDC_BLEND_MODE && code == CBN_SELCHANGE) {
        ScheduleDynamicAmountUpdate();
    } else if (id == IDC_COMPARE_MODE && code == CBN_SELCHANGE) {
        ScheduleAutoApply();
    } else if ((id == IDC_ENGINE1_EDIT || id == IDC_ENGINE2_EDIT) && code == EN_CHANGE) {
        ScheduleAutoApply();
    }
}

void OnHScroll(HWND, WPARAM, LPARAM lParam) {
    HWND h = reinterpret_cast<HWND>(lParam);
    if (h == hEngine1Track) {
        g_settings.engine1Amount = GetTrackAmount(hEngine1Track);
        SetWindowTextW(hEngine1Value, FormatAmount(g_settings.engine1Amount).c_str());
        ScheduleDynamicAmountUpdate();
    } else if (h == hEngine2Track) {
        g_settings.engine2Amount = GetTrackAmount(hEngine2Track);
        SetWindowTextW(hEngine2Value, FormatAmount(g_settings.engine2Amount).c_str());
        ScheduleDynamicAmountUpdate();
    } else if (h == hOriginalTrack) {
        g_settings.originalOpacity = GetTrackAmount(hOriginalTrack);
        SetWindowTextW(hOriginalValue, FormatAmount(g_settings.originalOpacity).c_str());
        ScheduleDynamicAmountUpdate();
    }
}

void CommitEditAmount(HWND edit, HWND track, double& value) {
    std::wstring s = GetWindowTextString(edit);
    try { value = Clamp01(std::stod(s)); }
    catch (...) { value = GetTrackAmount(track); }
    SetTrack(track, edit, value);
    ScheduleDynamicAmountUpdate();
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        CreateUi(hwnd);
        LoadSettings();
        RefreshUiFromSettings();
        SaveSettings();
        SetTimer(hwnd, kMpvWatchTimer, kMpvWatchIntervalMs, nullptr);
        return 0;
    case WM_COMMAND:
        OnCommand(hwnd, wParam, lParam);
        if (HIWORD(wParam) == EN_KILLFOCUS) {
            int id = LOWORD(wParam);
            if (id == IDC_ENGINE1_VALUE) CommitEditAmount(hEngine1Value, hEngine1Track, g_settings.engine1Amount);
            else if (id == IDC_ENGINE2_VALUE) CommitEditAmount(hEngine2Value, hEngine2Track, g_settings.engine2Amount);
            else if (id == IDC_ORIGINAL_VALUE) CommitEditAmount(hOriginalValue, hOriginalTrack, g_settings.originalOpacity);
        }
        return 0;
    case WM_HSCROLL:
        OnHScroll(hwnd, wParam, lParam);
        return 0;
    case WM_TIMER:
        if (wParam == kAutoApplyTimer) {
            KillTimer(hwnd, kAutoApplyTimer);
            ApplyToMpv(true, true);
            return 0;
        }
        if (wParam == kMpvWatchTimer) {
            bool pipeAlive = TestMpvPipe();
            if (!pipeAlive && g_everConnectedToMpv) {
                DestroyWindow(hwnd);
                return 0;
            }
            if (pipeAlive) {
                SyncPreviewStateFromMpvIfNeeded();
            }
            return 0;
        }
        break;
    case WM_CLOSE:
        GatherSettingsFromUi();
        SaveSettings();
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, kAutoApplyTimer);
        KillTimer(hwnd, kMpvWatchTimer);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
    HANDLE mutex = CreateMutexW(nullptr, FALSE, kMutexName);
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(nullptr, L"ONNX Blend UI は既に起動しています。", kAppTitle, MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    g_exeDir = GetModuleDir();
    g_configDir = (fs::path(g_exeDir) / L"portable_config" / L"cache" / L"onnx_blend_ui").wstring();
    g_configPath = (fs::path(g_configDir) / L"settings.json").wstring();

    // v15: migrate old settings folder name if present.
    // Old: portable_config\cache\onnx_blend_gui\settings.json
    // New: portable_config\cache\onnx_blend_ui\settings.json
    try {
        std::wstring legacyConfigPath = (fs::path(g_exeDir) / L"portable_config" / L"cache" / L"onnx_blend_gui" / L"settings.json").wstring();
        if (!FileExists(g_configPath) && FileExists(legacyConfigPath)) {
            fs::create_directories(fs::path(g_configDir));
            fs::copy_file(fs::path(legacyConfigPath), fs::path(g_configPath), fs::copy_options::overwrite_existing);
        }
    } catch (...) {
    }

    INITCOMMONCONTROLSEX icc = {};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_BAR_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"onnx_blend_UI_window_v3";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(
        0,
        wc.lpszClassName,
        kAppTitle,
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        790,
        530,
        nullptr,
        nullptr,
        hInstance,
        nullptr
    );

    if (!hwnd) {
        if (mutex) CloseHandle(mutex);
        return 1;
    }

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (mutex) CloseHandle(mutex);
    return static_cast<int>(msg.wParam);
}
