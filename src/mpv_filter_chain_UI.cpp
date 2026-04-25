#define UNICODE
#define _UNICODE

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>

#include <algorithm>
#include <cwctype>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#pragma comment(lib, "comctl32.lib")

// mpv_filter_chain_UI.cpp
// ------------------------------------------------------------
// Single-file Win32 UI for mpv filter-chain editing.
// Layout:
//   left  : current chain
//   right : available filters parsed from portable_config\input.conf
//
// Presets:
//   portable_config\cache\filter_chains\custom_chains.json
//
// Default mpv IPC pipe:
//   \\.\pipe\mpv-tool
// ------------------------------------------------------------

static const wchar_t* APP_CLASS_NAME = L"mpv_filter_chain_UI_window_v2";
static const wchar_t* APP_MUTEX_NAME = L"Local\\mpv_filter_chain_UI_single_instance_v2";
static const wchar_t* APP_RESET_EVENT_NAME = L"Local\\mpv_filter_chain_UI_reset_event_v2";

static const int APP_FIXED_CLIENT_W = 1080;
static const int APP_FIXED_CLIENT_H = 620;
static const DWORD APP_WINDOW_STYLE = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;

#define IDC_CANDIDATE_TREE  1001
#define IDC_CHAIN_LIST      1002
#define IDC_ADD             1003
#define IDC_REMOVE          1004
#define IDC_UP              1005
#define IDC_DOWN            1006
#define IDC_TOP             1007
#define IDC_BOTTOM          1008
#define IDC_PREVIEW           1009
#define IDC_SAVE            1010
#define IDC_LOAD            1011
#define IDC_CLEAR           1012
#define IDC_STATUS          1013
#define IDC_PRESET_NAME     1014
#define IDC_AUTO_APPLY      1015
#define IDC_REFRESH         1016
#define IDC_PRESET_COMBO    1017
#define IDC_NEW_PRESET      1018
#define IDC_DELETE_PRESET   1019
#define IDC_LANGUAGE        1020
#define IDC_ALWAYS_ON_TOP   1021
#define IDC_NAME_DIALOG_EDIT  2001
#define IDC_NAME_DIALOG_SAVE  2002
#define IDC_NAME_DIALOG_CANCEL 2003

struct Candidate {
    std::wstring name;
    std::wstring command;
    std::vector<std::wstring> menuPath; // #menu: A > B > leaf の A/B 部分
};

struct ChainItem {
    std::wstring name;
    std::wstring kind;     // VF / GLSL / ASPECT
    std::wstring payload;  // filter spec, shader path, or aspect value
};

struct Preset {
    std::wstring name;
    std::vector<ChainItem> items;
};

struct CmdOptions {
    bool resetUi = false;
    std::wstring pipePath;
};

struct AppState {
    HINSTANCE hInst = nullptr;
    HWND hwnd = nullptr;
    HWND hChain = nullptr;
    HWND hCandidates = nullptr;
    HWND hChainLabel = nullptr;
    HWND hCandidateLabel = nullptr;
    HWND hPresetCombo = nullptr;
    HWND hPresetName = nullptr;
    HWND hAutoApply = nullptr; // unused legacy slot
    HWND hStatus = nullptr;
    HWND hPresetGroup = nullptr;
    HWND hPresetSelectLabel = nullptr;
    HWND hPresetNameLabel = nullptr;
    HWND hPreviewCheck = nullptr;
    HWND hAlwaysOnTopCheck = nullptr;
    HWND hLanguageButton = nullptr;
    HANDLE hResetEvent = nullptr;

    std::wstring baseDir;
    std::wstring pipePath;

    std::vector<Candidate> candidates;
    std::vector<ChainItem> chain;
    std::vector<Preset> presets;
    int currentPresetIndex = -1;
    bool suppressPresetComboEvent = false;
    bool previewEnabled = true;
    bool alwaysOnTop = false;
    std::wstring language = L"ja";

    bool everConnectedToMpv = false;
    int pipeLostCount = 0;

    WNDPROC oldChainProc = nullptr;
    int dragIndex = -1;
};

static std::wstring Trim(const std::wstring& s) {
    size_t a = 0;
    while (a < s.size() && iswspace(s[a])) ++a;
    size_t b = s.size();
    while (b > a && iswspace(s[b - 1])) --b;
    return s.substr(a, b - a);
}

static bool StartsWithI(const std::wstring& s, const std::wstring& p) {
    if (s.size() < p.size()) return false;
    for (size_t i = 0; i < p.size(); ++i) {
        if (towlower(s[i]) != towlower(p[i])) return false;
    }
    return true;
}

static bool Contains(const std::wstring& s, const std::wstring& needle) {
    return s.find(needle) != std::wstring::npos;
}

static std::wstring RemoveOuterQuotes(std::wstring s) {
    s = Trim(s);
    if (s.size() >= 2 && s.front() == L'"' && s.back() == L'"') {
        s = s.substr(1, s.size() - 2);
    }
    return s;
}

static std::vector<std::wstring> SplitNotInQuotes(const std::wstring& s, wchar_t delim) {
    std::vector<std::wstring> out;
    std::wstring cur;
    bool inQuote = false;
    wchar_t prev = 0;
    for (wchar_t ch : s) {
        if (ch == L'"' && prev != L'\\') inQuote = !inQuote;
        if (ch == delim && !inQuote) {
            out.push_back(Trim(cur));
            cur.clear();
        } else {
            cur.push_back(ch);
        }
        prev = ch;
    }
    out.push_back(Trim(cur));
    return out;
}

static std::wstring Join(const std::vector<std::wstring>& v, const std::wstring& sep) {
    std::wstring r;
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) r += sep;
        r += v[i];
    }
    return r;
}

static std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int needed = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return std::string();
    std::string s((size_t)needed, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], needed, nullptr, nullptr);
    return s;
}

static std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int needed = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (needed <= 0) return std::wstring();
    std::wstring w((size_t)needed, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], needed);
    return w;
}

static std::wstring GetModuleDir() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    wchar_t* p = wcsrchr(path, L'\\');
    if (p) *p = 0;
    return path;
}

static bool FileExists(const std::wstring& path) {
    DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

static void EnsureDirectoryRecursive(const std::wstring& dir) {
    if (dir.empty()) return;
    std::wstring cur;
    for (size_t i = 0; i < dir.size(); ++i) {
        cur.push_back(dir[i]);
        if (dir[i] == L'\\' || dir[i] == L'/') {
            if (cur.size() > 3) CreateDirectoryW(cur.c_str(), nullptr);
        }
    }
    CreateDirectoryW(dir.c_str(), nullptr);
}

static std::wstring DirName(const std::wstring& path) {
    size_t p = path.find_last_of(L"\\/");
    if (p == std::wstring::npos) return L"";
    return path.substr(0, p);
}

static std::string ReadFileBytes(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return std::string();
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h, &size) || size.QuadPart <= 0 || size.QuadPart > 64LL * 1024LL * 1024LL) {
        CloseHandle(h);
        return std::string();
    }
    std::string data((size_t)size.QuadPart, '\0');
    DWORD read = 0;
    BOOL ok = ReadFile(h, &data[0], (DWORD)data.size(), &read, nullptr);
    CloseHandle(h);
    if (!ok) return std::string();
    data.resize(read);
    return data;
}

static bool WriteFileUtf8(const std::wstring& path, const std::wstring& text) {
    EnsureDirectoryRecursive(DirName(path));
    std::string u8 = WideToUtf8(text);
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    BOOL ok = WriteFile(h, u8.data(), (DWORD)u8.size(), &written, nullptr);
    CloseHandle(h);
    return ok && (size_t)written == u8.size();
}

static std::wstring JsonEscapeWide(const std::wstring& s) {
    std::wstring r;
    for (wchar_t ch : s) {
        switch (ch) {
        case L'\\': r += L"\\\\"; break;
        case L'"':  r += L"\\\""; break;
        case L'\n': r += L"\\n"; break;
        case L'\r': r += L"\\r"; break;
        case L'\t': r += L"\\t"; break;
        default: r += ch; break;
        }
    }
    return r;
}

static std::string JsonEscapeUtf8(const std::string& s) {
    std::string r;
    for (unsigned char c : s) {
        switch (c) {
        case '\\': r += "\\\\"; break;
        case '"':  r += "\\\""; break;
        case '\n': r += "\\n"; break;
        case '\r': r += "\\r"; break;
        case '\t': r += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                wsprintfA(buf, "\\u%04x", c);
                r += buf;
            } else {
                r.push_back((char)c);
            }
        }
    }
    return r;
}

static std::wstring JsonUnescapeWide(const std::wstring& s) {
    std::wstring r;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == L'\\' && i + 1 < s.size()) {
            wchar_t n = s[++i];
            switch (n) {
            case L'\\': r += L'\\'; break;
            case L'"': r += L'"'; break;
            case L'n': r += L'\n'; break;
            case L'r': r += L'\r'; break;
            case L't': r += L'\t'; break;
            default: r += n; break;
            }
        } else {
            r += s[i];
        }
    }
    return r;
}

static std::wstring ExtractJsonStringValue(const std::wstring& obj, const std::wstring& key) {
    std::wstring marker = L"\"" + key + L"\"";
    size_t p = obj.find(marker);
    if (p == std::wstring::npos) return L"";
    p = obj.find(L':', p + marker.size());
    if (p == std::wstring::npos) return L"";
    p = obj.find(L'"', p + 1);
    if (p == std::wstring::npos) return L"";
    ++p;
    std::wstring val;
    bool esc = false;
    for (; p < obj.size(); ++p) {
        wchar_t ch = obj[p];
        if (esc) {
            val.push_back(L'\\');
            val.push_back(ch);
            esc = false;
        } else if (ch == L'\\') {
            esc = true;
        } else if (ch == L'"') {
            break;
        } else {
            val.push_back(ch);
        }
    }
    return JsonUnescapeWide(val);
}

static size_t FindMatchingBracket(const std::wstring& s, size_t openPos, wchar_t openCh, wchar_t closeCh) {
    bool inQuote = false;
    bool esc = false;
    int depth = 0;
    for (size_t i = openPos; i < s.size(); ++i) {
        wchar_t ch = s[i];
        if (esc) { esc = false; continue; }
        if (ch == L'\\') { esc = true; continue; }
        if (ch == L'"') { inQuote = !inQuote; continue; }
        if (inQuote) continue;
        if (ch == openCh) ++depth;
        if (ch == closeCh) {
            --depth;
            if (depth == 0) return i;
        }
    }
    return std::wstring::npos;
}

static std::wstring SavedJsonPath(const AppState& st) {
    return st.baseDir + L"\\portable_config\\cache\\filter_chains\\custom_chains.json";
}

static std::wstring InputConfPath(const AppState& st) {
    std::wstring p = st.baseDir + L"\\portable_config\\input.conf";
    if (FileExists(p)) return p;
    return st.baseDir + L"\\input.conf";
}

static void EnsureDefaultJson(const AppState& st) {
    std::wstring path = SavedJsonPath(st);
    if (FileExists(path)) return;
    std::wstring json =
        L"{\n"
        L"  \"version\": 2,\n"
        L"  \"last_selected\": \"Custom 1\",\n"
        L"  \"preview_enabled\": \"true\",\n"
        L"  \"always_on_top\": \"false\",\n"
        L"  \"language\": \"ja\",\n"
        L"  \"chains\": [\n"
        L"    {\n"
        L"      \"name\": \"Custom 1\",\n"
        L"      \"items\": []\n"
        L"    }\n"
        L"  ]\n"
        L"}\n";
    WriteFileUtf8(path, json);
}

static bool IsEnglish(const AppState* st) {
    return st && st->language == L"en";
}

static const wchar_t* Txt(const AppState* st, const wchar_t* ja, const wchar_t* en) {
    return IsEnglish(st) ? en : ja;
}

static std::wstring TxtS(const AppState* st, const wchar_t* ja, const wchar_t* en) {
    return std::wstring(Txt(st, ja, en));
}

static void SetStatus(AppState* st, const std::wstring& msg) {
    if (st && st->hStatus) SetWindowTextW(st->hStatus, msg.c_str());
}

static std::wstring MakeDisplayName(const ChainItem& it) {
    // UI display only.
    // Keep the actual mpv command/payload internally, but do not show it in the list
    // so end users can choose filters by friendly names without being confused by syntax.
    return it.name;
}

static void RefreshChainList(AppState* st, int sel = -1) {
    SendMessageW(st->hChain, LB_RESETCONTENT, 0, 0);
    for (const auto& it : st->chain) {
        std::wstring d = MakeDisplayName(it);
        SendMessageW(st->hChain, LB_ADDSTRING, 0, (LPARAM)d.c_str());
    }
    if (!st->chain.empty()) {
        if (sel < 0) sel = 0;
        if (sel >= (int)st->chain.size()) sel = (int)st->chain.size() - 1;
        SendMessageW(st->hChain, LB_SETCURSEL, sel, 0);
    }
}

static bool IsAutoApply(AppState* st) {
    return st && st->previewEnabled;
}

static std::wstring RemoveInputConfKeyToken(std::wstring left) {
    left = Trim(left);
    if (left.empty()) return left;
    size_t p = 0;
    while (p < left.size() && !iswspace(left[p])) ++p;
    if (p >= left.size()) return L"";
    return Trim(left.substr(p));
}

static bool IsFilterOffMenu(const std::wstring& menu) {
    return Contains(menu, L"全OFF") ||
           Contains(menu, L"全オフ") ||
           Contains(menu, L"フィルターOFF") ||
           Contains(menu, L"フィルタOFF") ||
           Contains(menu, L"vfフィルタOFF") ||
           Contains(menu, L"シェーダーOFF") ||
           Contains(menu, L"フィルタ/シェーダー全OFF") ||
           Contains(menu, L"クリア") ||
           Contains(menu, L"リセット");
}

static std::wstring LastMenuPart(const std::wstring& menu) {
    std::vector<std::wstring> parts = SplitNotInQuotes(menu, L'>');
    return parts.empty() ? Trim(menu) : Trim(parts.back());
}

static std::vector<std::wstring> MenuParentPath(const std::wstring& menu) {
    std::vector<std::wstring> parts = SplitNotInQuotes(menu, L'>');
    std::vector<std::wstring> out;
    for (size_t i = 0; i + 1 < parts.size(); ++i) {
        std::wstring p = Trim(parts[i]);
        if (!p.empty()) out.push_back(p);
    }
    if (out.empty()) out.push_back(L"その他");
    return out;
}

static bool IsUsefulFilterSegment(const std::wstring& seg) {
    std::wstring s = Trim(seg);
    if (s.empty()) return false;
    if (StartsWithI(s, L"vf clr")) return false;
    if (StartsWithI(s, L"change-list glsl-shaders clr")) return false;
    if (StartsWithI(s, L"show-text")) return false;
    if (StartsWithI(s, L"run ")) return false;
    if (StartsWithI(s, L"ignore")) return false;
    return StartsWithI(s, L"vf toggle ") ||
           StartsWithI(s, L"vf set ") ||
           StartsWithI(s, L"change-list glsl-shaders toggle ") ||
           StartsWithI(s, L"change-list glsl-shaders set ") ||
           StartsWithI(s, L"set video-aspect-override ");
}

static std::vector<Candidate> LoadCandidatesFromInputConf(const AppState& st) {
    std::vector<Candidate> out;
    std::string bytes = ReadFileBytes(InputConfPath(st));
    if (bytes.empty()) return out;

    if (bytes.size() >= 3 && (unsigned char)bytes[0] == 0xEF && (unsigned char)bytes[1] == 0xBB && (unsigned char)bytes[2] == 0xBF) {
        bytes.erase(0, 3);
    }

    std::wstring text = Utf8ToWide(bytes);
    std::wstringstream ss(text);
    std::wstring line;
    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        std::wstring trimmed = Trim(line);
        if (trimmed.empty()) continue;
        if (StartsWithI(trimmed, L"#")) continue;

        size_t m = line.find(L"#menu:");
        if (m == std::wstring::npos) continue;

        std::wstring menu = Trim(line.substr(m + 6));
        if (Contains(menu, L"AIアップスケーリング∞：複合設定")) continue;
        if (IsFilterOffMenu(menu)) continue;

        std::wstring cmd = RemoveInputConfKeyToken(line.substr(0, m));
        if (cmd.empty()) continue;
        if (!Contains(cmd, L"vf ") && !Contains(cmd, L"glsl-shaders") && !Contains(cmd, L"video-aspect-override")) continue;

        bool useful = false;
        for (const auto& seg : SplitNotInQuotes(cmd, L';')) {
            if (IsUsefulFilterSegment(seg)) { useful = true; break; }
        }
        if (!useful) continue;

        Candidate c;
        c.name = LastMenuPart(menu);
        c.command = cmd;
        c.menuPath = MenuParentPath(menu);
        if (!c.name.empty()) out.push_back(c);
    }
    return out;
}

static std::vector<Candidate> DefaultCandidates() {
    std::vector<Candidate> v;
    v.push_back({L"強制480p入力", L"vf toggle scale=854:480", {L"強制fps/解像度", L"強制解像度"}});
    v.push_back({L"強制720p入力", L"vf toggle scale=1280:720", {L"強制fps/解像度", L"強制解像度"}});
    v.push_back({L"強制30fps", L"vf toggle fps=30:round=near", {L"強制fps/解像度", L"強制fps"}});
    v.push_back({L"yadif", L"vf toggle yadif=0:-1:1", {L"インターレース解除"}});
    v.push_back({L"bwdif", L"vf toggle bwdif=deint=1", {L"インターレース解除"}});
    v.push_back({L"FSRCNNX x2", L"change-list glsl-shaders toggle \"~~/shaders/FSRCNNX_x2_16_0_4_1.glsl\"", {L"GLSL"}});
    return v;
}

static void LoadCandidates(AppState* st) {
    st->candidates = LoadCandidatesFromInputConf(*st);
    if (st->candidates.empty()) {
        st->candidates = DefaultCandidates();
        SetStatus(st, TxtS(st, L"input.conf が見つからないため、最小候補だけ読み込みました。配置: portable_config\\input.conf", L"input.conf was not found, so only minimal built-in candidates were loaded. Location: portable_config\\input.conf"));
    } else {
        SetStatus(st, TxtS(st, L"input.conf から単体フィルター候補を読み込みました: ", L"Loaded available filters from input.conf: ") + std::to_wstring(st->candidates.size()) + TxtS(st, L" 件", L" items"));
    }
}

static std::wstring TreeKeyForPath(const std::vector<std::wstring>& parts, size_t count) {
    std::wstring key;
    for (size_t i = 0; i < count; ++i) {
        if (i) key += L"\x1f";
        key += parts[i];
    }
    return key;
}

static HTREEITEM InsertTreeNode(HWND tree, HTREEITEM parent, const std::wstring& text, LPARAM param) {
    TVINSERTSTRUCTW ti{};
    ti.hParent = parent ? parent : TVI_ROOT;
    ti.hInsertAfter = TVI_LAST;
    ti.item.mask = TVIF_TEXT | TVIF_PARAM;
    ti.item.pszText = const_cast<LPWSTR>(text.c_str());
    ti.item.lParam = param;
    return TreeView_InsertItem(tree, &ti);
}

static void RefreshCandidateTree(AppState* st) {
    TreeView_DeleteAllItems(st->hCandidates);
    std::map<std::wstring, HTREEITEM> nodes;

    for (size_t i = 0; i < st->candidates.size(); ++i) {
        const Candidate& c = st->candidates[i];
        HTREEITEM parent = nullptr;
        for (size_t d = 0; d < c.menuPath.size(); ++d) {
            std::wstring key = TreeKeyForPath(c.menuPath, d + 1);
            auto it = nodes.find(key);
            if (it == nodes.end()) {
                parent = InsertTreeNode(st->hCandidates, parent, c.menuPath[d], -1);
                nodes[key] = parent;
            } else {
                parent = it->second;
            }
        }
        InsertTreeNode(st->hCandidates, parent, c.name, (LPARAM)i);
    }

    HTREEITEM root = TreeView_GetRoot(st->hCandidates);
    while (root) {
        TreeView_Expand(st->hCandidates, root, TVE_COLLAPSE);
        root = TreeView_GetNextSibling(st->hCandidates, root);
    }
}

static int GetSelectedCandidateIndex(AppState* st) {
    HTREEITEM sel = TreeView_GetSelection(st->hCandidates);
    if (!sel) return -1;
    TVITEMW item{};
    item.mask = TVIF_PARAM;
    item.hItem = sel;
    if (!TreeView_GetItem(st->hCandidates, &item)) return -1;
    int idx = (int)item.lParam;
    if (idx < 0 || idx >= (int)st->candidates.size()) return -1;
    return idx;
}

static void AddSegmentToItems(std::vector<ChainItem>& out, const std::wstring& displayBase, const std::wstring& seg) {
    std::wstring s = Trim(seg);
    if (!IsUsefulFilterSegment(s)) return;

    if (StartsWithI(s, L"vf toggle ")) {
        out.push_back({displayBase, L"VF", Trim(s.substr(10))});
        return;
    }
    if (StartsWithI(s, L"vf set ")) {
        out.push_back({displayBase, L"VF", RemoveOuterQuotes(Trim(s.substr(7)))});
        return;
    }

    const std::wstring glslTogglePrefix = L"change-list glsl-shaders toggle ";
    const std::wstring glslSetPrefix = L"change-list glsl-shaders set ";

    if (StartsWithI(s, glslTogglePrefix)) {
        out.push_back({displayBase, L"GLSL", RemoveOuterQuotes(Trim(s.substr(glslTogglePrefix.size())))});
        return;
    }
    if (StartsWithI(s, glslSetPrefix)) {
        std::wstring payload = RemoveOuterQuotes(Trim(s.substr(glslSetPrefix.size())));
        std::vector<std::wstring> shaders = SplitNotInQuotes(payload, L';');
        int n = 1;
        for (auto& sh : shaders) {
            sh = RemoveOuterQuotes(Trim(sh));
            if (sh.empty()) continue;
            ChainItem it;
            it.kind = L"GLSL";
            it.name = displayBase + (shaders.size() > 1 ? (L" #" + std::to_wstring(n)) : L"");
            it.payload = sh;
            out.push_back(it);
            ++n;
        }
        return;
    }

    const std::wstring aspectPrefix = L"set video-aspect-override ";
    if (StartsWithI(s, aspectPrefix)) {
        out.push_back({displayBase, L"ASPECT", Trim(s.substr(aspectPrefix.size()))});
        return;
    }
}

static std::vector<ChainItem> ExpandedItemsFromCommand(const std::wstring& displayBase, const std::wstring& command) {
    std::vector<ChainItem> items;
    for (const auto& seg : SplitNotInQuotes(command, L';')) {
        AddSegmentToItems(items, displayBase, seg);
    }
    return items;
}

static std::vector<ChainItem> ExpandedItemsFromChainItem(const ChainItem& it) {
    if (it.kind == L"CMD") {
        return ExpandedItemsFromCommand(it.name, it.payload);
    }
    return std::vector<ChainItem>{it};
}

static std::vector<ChainItem> ItemsFromCandidate(const Candidate& c) {
    // A single #menu line in input.conf should appear as one visible item.
    // Some lines contain multiple mpv commands, e.g. color correction presets.
    // Store them as one compound command and expand only when applying to mpv.
    if (c.command.empty()) return {};
    return std::vector<ChainItem>{ ChainItem{c.name, L"CMD", c.command} };
}

static std::wstring ChainItemKey(const ChainItem& it) {
    return it.kind + L"\x1f" + it.payload;
}

static std::set<std::wstring> BuildValidItemKeySet(const AppState* st) {
    std::set<std::wstring> valid;
    for (const auto& c : st->candidates) {
        // New format: one compound command per input.conf menu line.
        for (const auto& it : ItemsFromCandidate(c)) {
            valid.insert(ChainItemKey(it));
        }
        // Backward compatibility: older custom_chains.json files saved each segment
        // as VF/GLSL/ASPECT. Keep those valid instead of treating them as deleted.
        for (const auto& it : ExpandedItemsFromCommand(c.name, c.command)) {
            valid.insert(ChainItemKey(it));
        }
    }
    return valid;
}

static std::vector<ChainItem> FilterDeletedItems(const AppState* st, const std::vector<ChainItem>& src, std::vector<ChainItem>* deleted = nullptr) {
    std::set<std::wstring> valid = BuildValidItemKeySet(st);
    std::vector<ChainItem> out;
    for (const auto& it : src) {
        if (valid.find(ChainItemKey(it)) != valid.end()) {
            out.push_back(it);
        } else if (deleted) {
            deleted->push_back(it);
        }
    }
    return out;
}

static void NotifyDeletedItems(AppState* st, const std::vector<ChainItem>& deleted, bool popup) {
    if (deleted.empty()) return;
    std::wstring msg = TxtS(st,
                            L"このプリセットには input.conf から削除されたフィルターが ",
                            L"This preset contains ") +
                       std::to_wstring(deleted.size()) +
                       TxtS(st,
                            L" 件あります。削除済み項目はUIに表示せず、mpvにも送信しません。",
                            L" removed filter item(s) from input.conf. Removed items are hidden in the UI and will not be sent to mpv.");
    if (popup) {
        std::wstring detail = msg + L"\n\n";
        size_t limit = deleted.size() < 8 ? deleted.size() : 8;
        for (size_t i = 0; i < limit; ++i) {
            detail += L"- " + deleted[i].name + L" [" + deleted[i].kind + L"] " + deleted[i].payload + L"\n";
        }
        if (deleted.size() > limit) detail += L"...\n";
        MessageBoxW(st->hwnd,
                    detail.c_str(),
                    Txt(st, L"削除済みフィルターを除外しました", L"Removed Filters Were Excluded"),
                    MB_OK | MB_ICONWARNING);
    }
    SetStatus(st, msg);
}


static void PruneCurrentChainAgainstCandidates(AppState* st, bool popup) {
    std::vector<ChainItem> deleted;
    std::vector<ChainItem> kept = FilterDeletedItems(st, st->chain, &deleted);
    if (!deleted.empty()) {
        st->chain = kept;
        RefreshChainList(st, 0);
        NotifyDeletedItems(st, deleted, popup);
    }
}

static bool AddSelectedCandidate(AppState* st) {
    int sel = GetSelectedCandidateIndex(st);
    if (sel < 0 || sel >= (int)st->candidates.size()) return false;
    int before = (int)st->chain.size();
    for (const auto& it : ItemsFromCandidate(st->candidates[(size_t)sel])) {
        st->chain.push_back(it);
    }
    if ((int)st->chain.size() == before) {
        SetStatus(st, TxtS(st, L"この項目から追加できるフィルター要素が見つかりませんでした。", L"No addable filter element was found for this item."));
        return false;
    }
    RefreshChainList(st, (int)st->chain.size() - 1);
    return true;
}

static std::vector<std::wstring> CandidatePipePaths(const std::wstring& /*preferred*/) {
    // Distribution build: use only the documented mpv IPC pipe.
    // This avoids accidentally controlling another mpv instance that happens
    // to expose a generic pipe name such as \\\\.\\pipe\\mpv.
    return std::vector<std::wstring>{ L"\\\\.\\pipe\\mpv-tool" };
}

static bool SendMpvCommandRaw(const std::wstring& pipePath, const std::vector<std::wstring>& args, std::wstring* usedPipe = nullptr) {
    std::vector<std::wstring> pipes = CandidatePipePaths(pipePath);
    std::string json = "{\"command\":[";
    for (size_t i = 0; i < args.size(); ++i) {
        if (i) json += ",";
        json += "\"" + JsonEscapeUtf8(WideToUtf8(args[i])) + "\"";
    }
    json += "]}\n";

    for (const auto& p : pipes) {
        HANDLE h = CreateFileW(p.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE && GetLastError() == ERROR_PIPE_BUSY) {
            if (WaitNamedPipeW(p.c_str(), 100)) {
                h = CreateFileW(p.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            }
        }
        if (h == INVALID_HANDLE_VALUE) continue;
        DWORD written = 0;
        BOOL ok = WriteFile(h, json.data(), (DWORD)json.size(), &written, nullptr);
        FlushFileBuffers(h);
        CloseHandle(h);
        if (ok && written == json.size()) {
            if (usedPipe) *usedPipe = p;
            return true;
        }
    }
    return false;
}

static bool TestPipe(AppState* st) {
    std::vector<std::wstring> pipes = CandidatePipePaths(st->pipePath);
    for (const auto& p : pipes) {
        HANDLE h = CreateFileW(p.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            CloseHandle(h);
            st->everConnectedToMpv = true;
            st->pipeLostCount = 0;
            if (st->pipePath.empty()) st->pipePath = p;
            return true;
        }

        // Another companion tool may be using the mpv IPC pipe at this exact moment.
        // ERROR_PIPE_BUSY still means the mpv pipe exists, so do not count it as mpv exit.
        if (GetLastError() == ERROR_PIPE_BUSY) {
            st->everConnectedToMpv = true;
            st->pipeLostCount = 0;
            if (st->pipePath.empty()) st->pipePath = p;
            return true;
        }
    }
    return false;
}

static bool ApplyChain(AppState* st, bool showStatus) {
    std::vector<std::wstring> vf;
    std::vector<std::wstring> glsl;
    std::wstring aspect;

    for (const auto& item : st->chain) {
        std::vector<ChainItem> expanded = ExpandedItemsFromChainItem(item);
        for (const auto& it : expanded) {
            if (it.kind == L"VF") vf.push_back(it.payload);
            else if (it.kind == L"GLSL") glsl.push_back(it.payload);
            else if (it.kind == L"ASPECT") aspect = it.payload;
        }
    }

    std::wstring used;
    bool ok = true;
    ok = ok && SendMpvCommandRaw(st->pipePath, {L"set", L"video-aspect-override", L"-1"}, &used);
    if (!aspect.empty()) {
        ok = ok && SendMpvCommandRaw(st->pipePath, {L"set", L"video-aspect-override", aspect}, &used);
    }

    if (vf.empty()) {
        ok = ok && SendMpvCommandRaw(st->pipePath, {L"vf", L"clr", L""}, &used);
    } else {
        ok = ok && SendMpvCommandRaw(st->pipePath, {L"vf", L"set", Join(vf, L",")}, &used);
    }

    if (glsl.empty()) {
        ok = ok && SendMpvCommandRaw(st->pipePath, {L"change-list", L"glsl-shaders", L"clr", L""}, &used);
    } else {
        ok = ok && SendMpvCommandRaw(st->pipePath, {L"change-list", L"glsl-shaders", L"set", Join(glsl, L";")}, &used);
    }

    ok = ok && SendMpvCommandRaw(st->pipePath, {L"show-text", L"Filter chain applied"}, &used);

    if (ok) {
        st->everConnectedToMpv = true;
        st->pipeLostCount = 0;
        if (!used.empty()) st->pipePath = used;
        if (showStatus) {
            SetStatus(st, TxtS(st, L"現在のチェーンをmpvに適用しました。VF=", L"Current chain applied to mpv. VF=") + std::to_wstring(vf.size()) + L" / GLSL=" + std::to_wstring(glsl.size()));
        }
    } else if (showStatus) {
        SetStatus(st, TxtS(st, L"mpv IPC pipe に接続できません。mpv.conf の input-ipc-server=\\\\.\\pipe\\mpv-tool を確認してください。UI状態は保存できます。", L"Could not connect to the mpv IPC pipe. Please check input-ipc-server=\\\\.\\pipe\\mpv-tool in mpv.conf. The UI state can still be saved."));
    }

    return ok;
}

static void UpdatePreviewCheck(AppState* st) {
    if (!st || !st->hPreviewCheck) return;
    SetWindowTextW(st->hPreviewCheck, Txt(st, L"プレビュー", L"Preview"));
    SendMessageW(st->hPreviewCheck, BM_SETCHECK, st->previewEnabled ? BST_CHECKED : BST_UNCHECKED, 0);
}

static void UpdateAlwaysOnTopCheck(AppState* st) {
    if (!st || !st->hAlwaysOnTopCheck) return;
    SetWindowTextW(st->hAlwaysOnTopCheck, Txt(st, L"常に最前面へ", L"Always on top"));
    SendMessageW(st->hAlwaysOnTopCheck, BM_SETCHECK, st->alwaysOnTop ? BST_CHECKED : BST_UNCHECKED, 0);
}

static void ApplyAlwaysOnTop(AppState* st) {
    if (!st || !st->hwnd) return;
    SetWindowPos(st->hwnd,
                 st->alwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST,
                 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

static bool ClearMpvFiltersOnly(AppState* st, bool showStatus);
static bool SaveAllPresets(AppState* st, const std::wstring& lastSelectedName);

static void MarkCurrentChainEdited(AppState* st) {
    if (!st) return;
    if (st->previewEnabled) {
        ApplyChain(st, false);
    } else {
        ClearMpvFiltersOnly(st, false);
    }
}

static void RealtimeApplyEditedChain(AppState* st) {
    if (!st) return;

    if (!st->previewEnabled) {
        ClearMpvFiltersOnly(st, false);
        SetStatus(st, TxtS(st, L"現在のチェーンを変更しました。プレビューOFFのため、mpv側は一時OFFのままです。", L"Current chain was changed. Preview is OFF, so mpv filters remain temporarily disabled."));
        return;
    }

    bool ok = ApplyChain(st, false);
    if (ok) {
        SetStatus(st, TxtS(st, L"現在のチェーンをmpvへリアルタイム反映しました。", L"Current chain was applied to mpv in real time."));
    } else {
        SetStatus(st, TxtS(st, L"現在のチェーンを変更しましたが、mpv IPC pipe に接続できません。mpv.conf の input-ipc-server を確認してください。", L"The current chain was changed, but the mpv IPC pipe could not be reached. Please check input-ipc-server in mpv.conf."));
    }
}

static bool ClearMpvFiltersOnly(AppState* st, bool showStatus) {
    std::wstring used;
    bool ok = true;
    ok = ok && SendMpvCommandRaw(st->pipePath, {L"set", L"video-aspect-override", L"-1"}, &used);
    // mpv's standard deinterlacer is controlled by the independent "deinterlace" property,
    // not by the normal vf list.  Therefore vf clr alone does not turn it off.
    ok = ok && SendMpvCommandRaw(st->pipePath, {L"set", L"deinterlace", L"no"}, &used);
    ok = ok && SendMpvCommandRaw(st->pipePath, {L"vf", L"clr", L""}, &used);
    ok = ok && SendMpvCommandRaw(st->pipePath, {L"change-list", L"glsl-shaders", L"clr", L""}, &used);
    ok = ok && SendMpvCommandRaw(st->pipePath, {L"show-text", L"フィルタ/シェーダー/インターレース解除 全OFF"}, &used);
    if (ok) {
        st->everConnectedToMpv = true;
        st->pipeLostCount = 0;
        if (!used.empty()) st->pipePath = used;
        if (showStatus) SetStatus(st, TxtS(st, L"mpv側のフィルターとインターレース解除をOFFにしました。現在チェーンは保持しています。", L"mpv-side filters and deinterlacing were turned OFF. The current chain is kept."));
    } else if (showStatus) {
        SetStatus(st, TxtS(st, L"mpv IPC pipe に接続できません。UI状態はそのままです。", L"Could not connect to the mpv IPC pipe. The UI state was kept as-is."));
    }
    return ok;
}

static bool LoadPresetByIndex(AppState* st, int index, bool popupDeleted);
static void ClearChainAndMpv(AppState* st);

static void TogglePreview(AppState* st) {
    if (!st) return;

    LRESULT checked = SendMessageW(st->hPreviewCheck, BM_GETCHECK, 0, 0);
    st->previewEnabled = (checked == BST_CHECKED);

    if (st->previewEnabled) {
        bool ok = ApplyChain(st, true);
        SetStatus(st, ok ? TxtS(st, L"プレビューON：現在のチェーンをmpvへ反映しました。", L"Preview ON: current chain was applied to mpv.")
                         : TxtS(st, L"プレビューONにしましたが、mpv IPC pipe に接続できません。", L"Preview was turned ON, but the mpv IPC pipe could not be reached."));
    } else {
        ClearMpvFiltersOnly(st, true);
        SetStatus(st, TxtS(st, L"プレビューOFF：現在のチェーンは保持したまま、mpv側のフィルターを一時OFFにしました。", L"Preview OFF: current chain is kept, while mpv filters are temporarily disabled."));
    }
    UpdatePreviewCheck(st);
}

static void ToggleAlwaysOnTop(AppState* st) {
    if (!st) return;

    LRESULT checked = SendMessageW(st->hAlwaysOnTopCheck, BM_GETCHECK, 0, 0);
    st->alwaysOnTop = (checked == BST_CHECKED);
    ApplyAlwaysOnTop(st);
    UpdateAlwaysOnTopCheck(st);

    std::wstring last = L"Custom 1";
    int sel = st->hPresetCombo ? (int)SendMessageW(st->hPresetCombo, CB_GETCURSEL, 0, 0) : -1;
    if (sel >= 0 && sel < (int)st->presets.size()) last = st->presets[(size_t)sel].name;
    SaveAllPresets(st, last);

    SetStatus(st, st->alwaysOnTop
        ? TxtS(st, L"常に最前面をONにしました。", L"Always on top was turned ON.")
        : TxtS(st, L"常に最前面をOFFにしました。", L"Always on top was turned OFF."));
}

static void ClearChainAndMpv(AppState* st) {
    st->chain.clear();
    RefreshChainList(st);
    ClearMpvFiltersOnly(st, false);
    SetStatus(st, TxtS(st, L"現在チェーンを空にし、mpv側のフィルターもOFFにしました。", L"Current chain was cleared and mpv filters were turned OFF."));
}

static std::vector<ChainItem> ParseItemsFromObject(const std::wstring& obj) {
    std::vector<ChainItem> loaded;
    size_t ip = obj.find(L"\"items\"");
    if (ip == std::wstring::npos) return loaded;
    size_t open = obj.find(L'[', ip);
    if (open == std::wstring::npos) return loaded;
    size_t close = FindMatchingBracket(obj, open, L'[', L']');
    if (close == std::wstring::npos) return loaded;
    std::wstring arr = obj.substr(open + 1, close - open - 1);

    size_t p = 0;
    while (true) {
        size_t o = arr.find(L'{', p);
        if (o == std::wstring::npos) break;
        size_t c = FindMatchingBracket(arr, o, L'{', L'}');
        if (c == std::wstring::npos) break;
        std::wstring itemObj = arr.substr(o, c - o + 1);
        ChainItem it;
        it.name = ExtractJsonStringValue(itemObj, L"name");
        it.kind = ExtractJsonStringValue(itemObj, L"kind");
        it.payload = ExtractJsonStringValue(itemObj, L"payload");
        if (!it.kind.empty() && !it.payload.empty()) loaded.push_back(it);
        p = c + 1;
    }
    return loaded;
}

static bool LoadAllPresets(AppState* st, std::wstring* lastSelected = nullptr) {
    EnsureDefaultJson(*st);
    std::string bytes = ReadFileBytes(SavedJsonPath(*st));
    if (bytes.empty()) return false;
    if (bytes.size() >= 3 && (unsigned char)bytes[0] == 0xEF && (unsigned char)bytes[1] == 0xBB && (unsigned char)bytes[2] == 0xBF) {
        bytes.erase(0, 3);
    }

    std::wstring text = Utf8ToWide(bytes);
    if (lastSelected) *lastSelected = ExtractJsonStringValue(text, L"last_selected");
    {
        std::wstring pv = ExtractJsonStringValue(text, L"preview_enabled");
        if (!pv.empty()) st->previewEnabled = (pv != L"false" && pv != L"0");
    }
    {
        std::wstring atop = ExtractJsonStringValue(text, L"always_on_top");
        if (!atop.empty()) st->alwaysOnTop = (atop != L"false" && atop != L"0");
    }
    {
        std::wstring lang = ExtractJsonStringValue(text, L"language");
        if (lang == L"en" || lang == L"ja") st->language = lang;
    }

    std::vector<Preset> presets;
    size_t cp = text.find(L"\"chains\"");
    if (cp != std::wstring::npos) {
        size_t open = text.find(L'[', cp);
        if (open != std::wstring::npos) {
            size_t close = FindMatchingBracket(text, open, L'[', L']');
            if (close != std::wstring::npos) {
                std::wstring arr = text.substr(open + 1, close - open - 1);
                size_t p = 0;
                while (true) {
                    size_t o = arr.find(L'{', p);
                    if (o == std::wstring::npos) break;
                    size_t c = FindMatchingBracket(arr, o, L'{', L'}');
                    if (c == std::wstring::npos) break;
                    std::wstring obj = arr.substr(o, c - o + 1);
                    Preset pr;
                    pr.name = ExtractJsonStringValue(obj, L"name");
                    pr.items = ParseItemsFromObject(obj);
                    if (pr.name.empty()) pr.name = L"Custom " + std::to_wstring(presets.size() + 1);
                    presets.push_back(pr);
                    p = c + 1;
                }
            }
        }
    }

    if (presets.empty()) {
        // v1 fallback: { "last_chain": { "name": ..., "items": [...] } }
        Preset pr;
        pr.name = ExtractJsonStringValue(text, L"name");
        if (pr.name.empty()) pr.name = L"Custom 1";
        pr.items = ParseItemsFromObject(text);
        presets.push_back(pr);
        if (lastSelected && lastSelected->empty()) *lastSelected = pr.name;
    }

    if (presets.empty()) presets.push_back({L"Custom 1", {}});
    st->presets = presets;
    return true;
}

static bool SaveAllPresets(AppState* st, const std::wstring& lastSelectedName) {
    std::wstring json;
    json += L"{\n";
    json += L"  \"version\": 2,\n";
    json += L"  \"last_selected\": \"" + JsonEscapeWide(lastSelectedName) + L"\",\n";
    json += L"  \"preview_enabled\": \"" + std::wstring(st->previewEnabled ? L"true" : L"false") + L"\",\n";
    json += L"  \"always_on_top\": \"" + std::wstring(st->alwaysOnTop ? L"true" : L"false") + L"\",\n";
    json += L"  \"language\": \"" + JsonEscapeWide(st->language) + L"\",\n";
    json += L"  \"chains\": [\n";
    for (size_t pi = 0; pi < st->presets.size(); ++pi) {
        const Preset& pr = st->presets[pi];
        json += L"    {\n";
        json += L"      \"name\": \"" + JsonEscapeWide(pr.name) + L"\",\n";
        json += L"      \"items\": [\n";
        for (size_t i = 0; i < pr.items.size(); ++i) {
            const ChainItem& it = pr.items[i];
            json += L"        {\"name\": \"" + JsonEscapeWide(it.name) +
                    L"\", \"kind\": \"" + JsonEscapeWide(it.kind) +
                    L"\", \"payload\": \"" + JsonEscapeWide(it.payload) + L"\"}";
            if (i + 1 < pr.items.size()) json += L",";
            json += L"\n";
        }
        json += L"      ]\n";
        json += L"    }";
        if (pi + 1 < st->presets.size()) json += L",";
        json += L"\n";
    }
    json += L"  ]\n";
    json += L"}\n";
    return WriteFileUtf8(SavedJsonPath(*st), json);
}

static int FindPresetByName(AppState* st, const std::wstring& name) {
    for (size_t i = 0; i < st->presets.size(); ++i) {
        if (st->presets[i].name == name) return (int)i;
    }
    return -1;
}

static std::wstring GetPresetNameFromEdit(AppState* st) {
    wchar_t nameBuf[256]{};
    GetWindowTextW(st->hPresetName, nameBuf, 256);
    std::wstring name = Trim(nameBuf);
    return name.empty() ? L"Custom 1" : name;
}

static void RefreshPresetCombo(AppState* st, int selectIndex = -1) {
    if (!st->hPresetCombo) return;

    st->suppressPresetComboEvent = true;
    SendMessageW(st->hPresetCombo, CB_RESETCONTENT, 0, 0);
    for (const auto& pr : st->presets) {
        SendMessageW(st->hPresetCombo, CB_ADDSTRING, 0, (LPARAM)pr.name.c_str());
    }

    if (selectIndex < 0) selectIndex = st->currentPresetIndex;
    if (selectIndex < 0 && !st->presets.empty()) selectIndex = 0;
    if (selectIndex >= (int)st->presets.size()) selectIndex = (int)st->presets.size() - 1;
    st->currentPresetIndex = selectIndex;

    if (selectIndex >= 0) {
        SendMessageW(st->hPresetCombo, CB_SETCURSEL, selectIndex, 0);
    }
    st->suppressPresetComboEvent = false;
}

static std::wstring MakeNewPresetName(AppState* st) {
    for (int i = 1; i < 1000; ++i) {
        std::wstring name = L"Custom " + std::to_wstring(i);
        if (FindPresetByName(st, name) < 0) return name;
    }
    return L"Custom";
}

struct NameDialogState {
    AppState* app = nullptr;
    HWND hwnd = nullptr;
    HWND hEdit = nullptr;
    std::wstring title;
    std::wstring label;
    std::wstring initial;
    std::wstring result;
    bool done = false;
    bool accepted = false;
};

static LRESULT CALLBACK NameDialogProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    NameDialogState* ds = (NameDialogState*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    switch (msg) {
    case WM_CREATE: {
        CREATESTRUCTW* cs = (CREATESTRUCTW*)lp;
        ds = (NameDialogState*)cs->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)ds);
        ds->hwnd = hwnd;

        HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        HWND hLabel = CreateWindowExW(0, L"STATIC", ds->label.c_str(), WS_CHILD | WS_VISIBLE,
                                      16, 16, 360, 20, hwnd, nullptr, nullptr, nullptr);
        ds->hEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", ds->initial.c_str(),
                                    WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                    16, 42, 360, 24, hwnd, (HMENU)IDC_NAME_DIALOG_EDIT, nullptr, nullptr);
        HWND hSave = CreateWindowExW(0, L"BUTTON", Txt(ds->app, L"保存", L"Save"), WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                                     202, 84, 82, 28, hwnd, (HMENU)IDC_NAME_DIALOG_SAVE, nullptr, nullptr);
        HWND hCancel = CreateWindowExW(0, L"BUTTON", Txt(ds->app, L"キャンセル", L"Cancel"), WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                       294, 84, 82, 28, hwnd, (HMENU)IDC_NAME_DIALOG_CANCEL, nullptr, nullptr);
        SendMessageW(hLabel, WM_SETFONT, (WPARAM)font, TRUE);
        SendMessageW(ds->hEdit, WM_SETFONT, (WPARAM)font, TRUE);
        SendMessageW(hSave, WM_SETFONT, (WPARAM)font, TRUE);
        SendMessageW(hCancel, WM_SETFONT, (WPARAM)font, TRUE);
        SendMessageW(ds->hEdit, EM_SETSEL, 0, -1);
        SetFocus(ds->hEdit);
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id == IDC_NAME_DIALOG_SAVE) {
            wchar_t buf[512]{};
            GetWindowTextW(ds->hEdit, buf, 512);
            std::wstring name = Trim(buf);
            if (name.empty()) {
                MessageBoxW(hwnd, Txt(ds->app, L"プリセット名を入力してください。", L"Please enter a preset name."), Txt(ds->app, L"プリセット名", L"Preset Name"), MB_OK | MB_ICONINFORMATION);
                SetFocus(ds->hEdit);
                return 0;
            }
            ds->result = name;
            ds->accepted = true;
            ds->done = true;
            DestroyWindow(hwnd);
            return 0;
        }
        if (id == IDC_NAME_DIALOG_CANCEL) {
            ds->accepted = false;
            ds->done = true;
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    }
    case WM_CLOSE:
        if (ds) {
            ds->accepted = false;
            ds->done = true;
        }
        DestroyWindow(hwnd);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static bool ShowPresetNameDialog(AppState* st, const wchar_t* title, const wchar_t* label,
                                 const std::wstring& initial, std::wstring* outName) {
    if (!st || !st->hwnd || !outName) return false;

    static bool registered = false;
    static const wchar_t* kClassName = L"mpv_filter_chain_UI_name_dialog";
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = NameDialogProc;
        wc.hInstance = st->hInst;
        wc.lpszClassName = kClassName;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        RegisterClassW(&wc);
        registered = true;
    }

    NameDialogState ds;
    ds.app = st;
    ds.title = title ? title : L"プリセット名";
    ds.label = label ? label : L"プリセット名";
    ds.initial = initial;

    RECT ownerRc{};
    GetWindowRect(st->hwnd, &ownerRc);
    int dlgW = 410;
    int dlgH = 160;
    int x = ownerRc.left + ((ownerRc.right - ownerRc.left) - dlgW) / 2;
    int y = ownerRc.top + ((ownerRc.bottom - ownerRc.top) - dlgH) / 2;

    EnableWindow(st->hwnd, FALSE);
    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, kClassName, ds.title.c_str(),
                               WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
                               x, y, dlgW, dlgH, st->hwnd, nullptr, st->hInst, &ds);
    if (!dlg) {
        EnableWindow(st->hwnd, TRUE);
        SetForegroundWindow(st->hwnd);
        return false;
    }

    MSG msg{};
    while (!ds.done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(dlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    EnableWindow(st->hwnd, TRUE);
    SetForegroundWindow(st->hwnd);
    if (ds.accepted) {
        *outName = ds.result;
        return true;
    }
    return false;
}

static bool LoadPresetByIndex(AppState* st, int index, bool popupDeleted) {
    if (index < 0 || index >= (int)st->presets.size()) return false;

    st->currentPresetIndex = index;
    std::vector<ChainItem> deleted;
    st->chain = FilterDeletedItems(st, st->presets[(size_t)index].items, &deleted);

    RefreshPresetCombo(st, index);
    RefreshChainList(st, 0);
    SaveAllPresets(st, st->presets[(size_t)index].name);

    if (!deleted.empty()) {
        NotifyDeletedItems(st, deleted, popupDeleted);
    } else {
        SetStatus(st, TxtS(st, L"プリセットを選択しました: ", L"Preset selected: ") + st->presets[(size_t)index].name + TxtS(st, L" / 項目数: ", L" / Items: ") + std::to_wstring(st->chain.size()));
    }
    return true;
}

static bool LoadSelectedPreset(AppState* st, bool popupDeleted) {
    int sel = (int)SendMessageW(st->hPresetCombo, CB_GETCURSEL, 0, 0);
    if (sel < 0) sel = st->currentPresetIndex;
    bool ok = LoadPresetByIndex(st, sel, popupDeleted);
    if (ok) MarkCurrentChainEdited(st);
    return ok;
}

static void LoadChain(AppState* st) {
    std::wstring lastSelected;
    LoadAllPresets(st, &lastSelected);
    int idx = lastSelected.empty() ? -1 : FindPresetByName(st, lastSelected);
    if (idx < 0) idx = 0;
    RefreshPresetCombo(st, idx);
    LoadPresetByIndex(st, idx, false);
}

static bool SaveChain(AppState* st) {
    int selected = (int)SendMessageW(st->hPresetCombo, CB_GETCURSEL, 0, 0);
    std::wstring presetName;
    int idx = selected;

    if (idx >= 0 && idx < (int)st->presets.size()) {
        presetName = st->presets[(size_t)idx].name;

        std::wstring prompt;
        if (IsEnglish(st)) {
            prompt = L"Overwrite preset \"" + presetName + L"\" with the current chain?";
        } else {
            prompt = L"プリセット「" + presetName + L"」に現在のチェーンを上書き保存しますか？";
        }

        if (MessageBoxW(st->hwnd,
                        prompt.c_str(),
                        Txt(st, L"プリセット保存", L"Save Preset"),
                        MB_YESNO | MB_ICONQUESTION) != IDYES) {
            SetStatus(st, TxtS(st, L"保存をキャンセルしました。", L"Save was canceled."));
            return false;
        }
        st->presets[(size_t)idx].items = st->chain;
    } else {
        presetName = MakeNewPresetName(st);
        if (!ShowPresetNameDialog(st,
                                  Txt(st, L"新規プリセット保存", L"Save New Preset"),
                                  Txt(st, L"保存するプリセット名", L"Preset name"),
                                  presetName,
                                  &presetName)) {
            SetStatus(st, TxtS(st, L"保存をキャンセルしました。", L"Save was canceled."));
            return false;
        }
        if (FindPresetByName(st, presetName) >= 0) {
            MessageBoxW(st->hwnd,
                        Txt(st, L"同じ名前のプリセットが既にあります。別の名前を指定してください。",
                                L"A preset with the same name already exists. Please choose another name."),
                        Txt(st, L"プリセット保存", L"Save Preset"),
                        MB_OK | MB_ICONWARNING);
            return false;
        }
        st->presets.push_back({presetName, st->chain});
        idx = (int)st->presets.size() - 1;
    }

    st->currentPresetIndex = idx;
    bool ok = SaveAllPresets(st, presetName);
    RefreshPresetCombo(st, idx);
    SetStatus(st, ok ? (TxtS(st, L"プリセットを保存しました: ", L"Preset saved: ") + presetName)
                     : TxtS(st, L"保存に失敗しました。書き込み権限を確認してください。", L"Failed to save. Please check write permissions."));
    return ok;
}


static void RenameSelectedPreset(AppState* st) {
    int sel = (int)SendMessageW(st->hPresetCombo, CB_GETCURSEL, 0, 0);
    if (sel < 0 || sel >= (int)st->presets.size()) {
        MessageBoxW(st->hwnd,
                    Txt(st, L"名前を変更するプリセットを選択してください。", L"Please select a preset to rename."),
                    Txt(st, L"名前変更", L"Rename"),
                    MB_OK | MB_ICONINFORMATION);
        return;
    }

    std::wstring oldName = st->presets[(size_t)sel].name;
    std::wstring newName = oldName;
    if (!ShowPresetNameDialog(st,
                              Txt(st, L"プリセット名変更", L"Rename Preset"),
                              Txt(st, L"新しいプリセット名", L"New preset name"),
                              oldName,
                              &newName)) {
        SetStatus(st, TxtS(st, L"名前変更をキャンセルしました。", L"Rename was canceled."));
        return;
    }

    int existing = FindPresetByName(st, newName);
    if (existing >= 0 && existing != sel) {
        MessageBoxW(st->hwnd,
                    Txt(st, L"同じ名前のプリセットが既にあります。別の名前を指定してください。",
                            L"A preset with the same name already exists. Please choose another name."),
                    Txt(st, L"名前変更", L"Rename"),
                    MB_OK | MB_ICONWARNING);
        return;
    }

    st->presets[(size_t)sel].name = newName;
    st->currentPresetIndex = sel;
    bool ok = SaveAllPresets(st, newName);
    RefreshPresetCombo(st, sel);
    SetStatus(st, ok ? (TxtS(st, L"プリセット名を変更しました: ", L"Preset renamed: ") + oldName + L" → " + newName)
                     : TxtS(st, L"名前変更の保存に失敗しました。", L"Failed to save the renamed preset."));
}


static void NewPreset(AppState* st) {
    if (!st) return;

    if (MessageBoxW(st->hwnd,
                    Txt(st, L"現在のチェーンを新規に保存しますか？", L"Save the current chain as a new preset?"),
                    Txt(st, L"新規保存", L"Save New Preset"),
                    MB_YESNO | MB_ICONQUESTION) != IDYES) {
        SetStatus(st, TxtS(st, L"新規保存をキャンセルしました。", L"Save new preset was canceled."));
        return;
    }

    std::wstring name = MakeNewPresetName(st);
    if (!ShowPresetNameDialog(st,
                              Txt(st, L"新規保存", L"Save New Preset"),
                              Txt(st, L"保存するプリセット名", L"Preset name"),
                              name,
                              &name)) {
        SetStatus(st, TxtS(st, L"新規保存をキャンセルしました。", L"Save new preset was canceled."));
        return;
    }

    if (FindPresetByName(st, name) >= 0) {
        MessageBoxW(st->hwnd,
                    Txt(st, L"同じ名前のプリセットが既にあります。別の名前を指定してください。", L"A preset with the same name already exists. Please choose another name."),
                    Txt(st, L"新規保存", L"Save New Preset"),
                    MB_OK | MB_ICONWARNING);
        return;
    }

    PruneCurrentChainAgainstCandidates(st, true);
    st->presets.push_back({name, st->chain});
    int idx = (int)st->presets.size() - 1;
    st->currentPresetIndex = idx;
    bool ok = SaveAllPresets(st, name);
    RefreshPresetCombo(st, idx);
    RefreshChainList(st, 0);
    SetStatus(st, ok ? (TxtS(st, L"現在のチェーンを新規プリセットとして保存しました: ", L"Current chain was saved as a new preset: ") + name)
                     : TxtS(st, L"新規保存に失敗しました。書き込み権限を確認してください。", L"Failed to save the new preset. Please check write permissions."));
}

static void DeleteSelectedPreset(AppState* st) {
    int sel = (int)SendMessageW(st->hPresetCombo, CB_GETCURSEL, 0, 0);
    if (sel < 0 || sel >= (int)st->presets.size()) return;

    std::wstring name = st->presets[(size_t)sel].name;

    std::wstring prompt;
    if (IsEnglish(st)) {
        prompt = L"Delete preset \"" + name + L"\"?";
    } else {
        prompt = L"プリセット「" + name + L"」を削除しますか？";
    }

    if (MessageBoxW(st->hwnd,
                    prompt.c_str(),
                    Txt(st, L"プリセット削除", L"Delete Preset"),
                    MB_YESNO | MB_ICONQUESTION) != IDYES) return;

    st->presets.erase(st->presets.begin() + sel);
    if (st->presets.empty()) st->presets.push_back({L"Custom 1", {}});

    int next = sel;
    if (next >= (int)st->presets.size()) next = (int)st->presets.size() - 1;
    SaveAllPresets(st, st->presets[(size_t)next].name);
    RefreshPresetCombo(st, next);
    LoadPresetByIndex(st, next, false);
    MarkCurrentChainEdited(st);
    SetStatus(st, TxtS(st, L"プリセットを削除しました: ", L"Preset deleted: ") + name);
}


static void MoveSelected(AppState* st, int mode) {
    int sel = (int)SendMessageW(st->hChain, LB_GETCURSEL, 0, 0);
    if (sel < 0 || sel >= (int)st->chain.size()) return;

    int target = sel;
    if (mode == 0) target = sel - 1;
    if (mode == 1) target = sel + 1;
    if (mode == 2) target = 0;
    if (mode == 3) target = (int)st->chain.size() - 1;

    if (target < 0) target = 0;
    if (target >= (int)st->chain.size()) target = (int)st->chain.size() - 1;
    if (target == sel) return;

    ChainItem item = st->chain[(size_t)sel];
    st->chain.erase(st->chain.begin() + sel);
    st->chain.insert(st->chain.begin() + target, item);
    RefreshChainList(st, target);
    RealtimeApplyEditedChain(st);
}

static void DeleteSelected(AppState* st) {
    int sel = (int)SendMessageW(st->hChain, LB_GETCURSEL, 0, 0);
    if (sel < 0 || sel >= (int)st->chain.size()) return;
    st->chain.erase(st->chain.begin() + sel);
    RefreshChainList(st, sel);
    RealtimeApplyEditedChain(st);
}

static LRESULT CALLBACK ChainListProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    AppState* st = (AppState*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (!st) return DefWindowProcW(hwnd, msg, wp, lp);

    switch (msg) {
    case WM_LBUTTONDOWN: {
        DWORD pos = (DWORD)lp;
        LRESULT r = SendMessageW(hwnd, LB_ITEMFROMPOINT, 0, pos);
        int index = LOWORD(r);
        BOOL outside = HIWORD(r);
        if (!outside && index >= 0 && index < (int)st->chain.size()) {
            st->dragIndex = index;
            SendMessageW(hwnd, LB_SETCURSEL, index, 0);
            SetCapture(hwnd);
        }
        break;
    }
    case WM_MOUSEMOVE: {
        if (st->dragIndex >= 0 && (wp & MK_LBUTTON)) {
            DWORD pos = (DWORD)lp;
            LRESULT r = SendMessageW(hwnd, LB_ITEMFROMPOINT, 0, pos);
            int target = LOWORD(r);
            BOOL outside = HIWORD(r);
            if (!outside && target >= 0 && target < (int)st->chain.size() && target != st->dragIndex) {
                ChainItem item = st->chain[(size_t)st->dragIndex];
                st->chain.erase(st->chain.begin() + st->dragIndex);
                st->chain.insert(st->chain.begin() + target, item);
                st->dragIndex = target;
                RefreshChainList(st, target);
                RealtimeApplyEditedChain(st);
            }
        }
        break;
    }
    case WM_LBUTTONUP:
        if (st->dragIndex >= 0) {
            st->dragIndex = -1;
            ReleaseCapture();
        }
        break;
    }
    return CallWindowProcW(st->oldChainProc, hwnd, msg, wp, lp);
}

static HWND MakeLabel(HWND parent, const wchar_t* text) {
    return CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE, 0, 0, 100, 20, parent, nullptr, nullptr, nullptr);
}

static HWND MakeButton(HWND parent, int id, const wchar_t* text) {
    return CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 100, 28, parent, (HMENU)(INT_PTR)id, nullptr, nullptr);
}

static void MoveWindowTop(HWND hwnd, int x, int y, int w, int h) {
    if (!hwnd) return;
    MoveWindow(hwnd, x, y, w, h, TRUE);
    SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

static void RedrawPresetControls(AppState* st) {
    if (!st) return;
    HWND ctrls[] = {
        st->hPresetSelectLabel,
        st->hPresetCombo,
        GetDlgItem(st->hwnd, IDC_NEW_PRESET),
        GetDlgItem(st->hwnd, IDC_SAVE),
        GetDlgItem(st->hwnd, IDC_LOAD),
        GetDlgItem(st->hwnd, IDC_DELETE_PRESET)
    };
    for (HWND h : ctrls) {
        if (h) RedrawWindow(h, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
    }
}

static void ApplyLanguage(AppState* st) {
    if (!st) return;
    if (st->hwnd) SetWindowTextW(st->hwnd, L"mpv filter chain UI");
    if (st->hPreviewCheck) SetWindowTextW(st->hPreviewCheck, Txt(st, L"プレビュー", L"Preview"));
    if (st->hAlwaysOnTopCheck) SetWindowTextW(st->hAlwaysOnTopCheck, Txt(st, L"常に最前面へ", L"Always on top"));
    if (st->hLanguageButton) SetWindowTextW(st->hLanguageButton, L"言語 / Language");
    if (st->hChainLabel) SetWindowTextW(st->hChainLabel, Txt(st, L"現在のチェーン（ドラッグで並べ替え可）", L"Current chain (drag to reorder)"));
    if (st->hCandidateLabel) SetWindowTextW(st->hCandidateLabel, Txt(st, L"利用可能フィルター（input.conf順）", L"Available filters (input.conf order)"));
    if (st->hPresetGroup) SetWindowTextW(st->hPresetGroup, Txt(st, L"プリセット管理", L"Preset Manager"));
    if (st->hPresetSelectLabel) SetWindowTextW(st->hPresetSelectLabel, Txt(st, L"プリセット", L"Preset"));

    SetWindowTextW(GetDlgItem(st->hwnd, IDC_ADD), Txt(st, L"← 追加", L"← Add"));
    SetWindowTextW(GetDlgItem(st->hwnd, IDC_REMOVE), Txt(st, L"削除", L"Remove"));
    SetWindowTextW(GetDlgItem(st->hwnd, IDC_UP), Txt(st, L"上へ", L"Up"));
    SetWindowTextW(GetDlgItem(st->hwnd, IDC_DOWN), Txt(st, L"下へ", L"Down"));
    SetWindowTextW(GetDlgItem(st->hwnd, IDC_CLEAR), Txt(st, L"フィルター全削除", L"Clear Filters"));
    SetWindowTextW(GetDlgItem(st->hwnd, IDC_NEW_PRESET), Txt(st, L"新規保存", L"Save New"));
    SetWindowTextW(GetDlgItem(st->hwnd, IDC_SAVE), Txt(st, L"上書き保存", L"Save"));
    SetWindowTextW(GetDlgItem(st->hwnd, IDC_LOAD), Txt(st, L"名前変更", L"Rename"));
    SetWindowTextW(GetDlgItem(st->hwnd, IDC_DELETE_PRESET), Txt(st, L"削除", L"Delete"));
    UpdatePreviewCheck(st);
    UpdateAlwaysOnTopCheck(st);
}

static void ToggleLanguage(AppState* st) {
    if (!st) return;
    st->language = IsEnglish(st) ? L"ja" : L"en";
    ApplyLanguage(st);

    std::wstring last = L"Custom 1";
    int sel = st->hPresetCombo ? (int)SendMessageW(st->hPresetCombo, CB_GETCURSEL, 0, 0) : -1;
    if (sel >= 0 && sel < (int)st->presets.size()) last = st->presets[(size_t)sel].name;
    SaveAllPresets(st, last);

    SetStatus(st, IsEnglish(st) ? L"Language switched to English." : L"言語を日本語に切り替えました。");
}

static void LayoutControls(AppState* st, int w, int clientH) {
    int margin = 10;
    int topBarH = 34;
    int bottom = 140;
    int midGap = 130;
    int listTop = margin + topBarH;
    int listH = clientH - listTop - bottom - 10;
    if (listH < 160) listH = 160;

    int leftW = (w - margin * 2 - midGap) / 2;
    int rightW = w - margin * 2 - midGap - leftW;
    if (leftW < 260) leftW = 260;
    if (rightW < 260) rightW = 260;

    int leftX = margin;
    int btnX = leftX + leftW + 10;
    int rightX = btnX + midGap - 10;

    // Top area: Preview and Always-on-top checkboxes on the far left, language button on the far right.
    int langW = 150;
    int previewW = 110;
    int topMostW = 150;
    int topMostX = leftX + previewW + 8;
    int statusX = topMostX + topMostW + 10;
    MoveWindow(st->hPreviewCheck, leftX, margin + 6, previewW, 24, TRUE);
    MoveWindow(st->hAlwaysOnTopCheck, topMostX, margin + 6, topMostW, 24, TRUE);
    MoveWindow(st->hLanguageButton, w - margin - langW, margin + 4, langW, 26, TRUE);
    MoveWindow(st->hStatus, statusX, margin + 4, w - margin - langW - 8 - statusX, 24, TRUE);

    MoveWindow(st->hChainLabel, leftX, listTop, leftW, 20, TRUE);
    MoveWindow(st->hCandidateLabel, rightX, listTop, rightW, 20, TRUE);
    MoveWindow(st->hChain, leftX, listTop + 24, leftW, listH, TRUE);
    MoveWindow(st->hCandidates, rightX, listTop + 24, rightW, listH, TRUE);

    int by = listTop + 24;
    HWND ctrl;
    ctrl = GetDlgItem(st->hwnd, IDC_ADD);           MoveWindow(ctrl, btnX, by, 110, 28, TRUE); by += 34;
    ctrl = GetDlgItem(st->hwnd, IDC_REMOVE);        MoveWindow(ctrl, btnX, by, 110, 28, TRUE); by += 42;
    ctrl = GetDlgItem(st->hwnd, IDC_UP);            MoveWindow(ctrl, btnX, by, 110, 28, TRUE); by += 34;
    ctrl = GetDlgItem(st->hwnd, IDC_DOWN);          MoveWindow(ctrl, btnX, by, 110, 28, TRUE); by += 42;
    ctrl = GetDlgItem(st->hwnd, IDC_CLEAR);         MoveWindow(ctrl, btnX, by, 110, 30, TRUE);

    // Removed confusing shortcut buttons from the visible layout.
    ctrl = GetDlgItem(st->hwnd, IDC_TOP);           if (ctrl) MoveWindow(ctrl, -300, -300, 1, 1, TRUE);
    ctrl = GetDlgItem(st->hwnd, IDC_BOTTOM);        if (ctrl) MoveWindow(ctrl, -300, -300, 1, 1, TRUE);
    ctrl = GetDlgItem(st->hwnd, IDC_REFRESH);       if (ctrl) MoveWindow(ctrl, -300, -300, 1, 1, TRUE);

    // Keep the preset groupbox below the list controls.
    // The old +8 offset overlapped the bottom few pixels of the list area,
    // which could clip the top of the "プリセット管理" group title.
    int groupY = clientH - bottom + 18;
    int groupH = bottom - 28;
    int groupW = w - margin * 2;
    if (groupW < 420) groupW = 420;

    MoveWindow(st->hPresetGroup, leftX, groupY, groupW, groupH, TRUE);
    SetWindowPos(st->hPresetGroup, HWND_BOTTOM, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    // Fixed-size build: keep the preset manager in a single stable row.
    // The combo box is intentionally kept to a normal width instead of stretching
    // across the whole group, so the preset buttons remain visually close and clear.
    int innerX = leftX + 14;
    int rowY = groupY + 46;
    const int labelW = 70;
    const int comboW = 300;

    MoveWindowTop(st->hPresetSelectLabel, innerX, rowY + 4, labelW, 20);
    MoveWindowTop(st->hPresetCombo, innerX + labelW, rowY, comboW, 260);

    if (st->hPresetNameLabel) MoveWindow(st->hPresetNameLabel, -300, -300, 1, 1, TRUE);
    if (st->hPresetName) MoveWindow(st->hPresetName, -300, -300, 1, 1, TRUE);

    int x = innerX + labelW + comboW + 14;
    ctrl = GetDlgItem(st->hwnd, IDC_NEW_PRESET);    MoveWindowTop(ctrl, x, rowY, 90, 28);  x += 98;
    ctrl = GetDlgItem(st->hwnd, IDC_SAVE);          MoveWindowTop(ctrl, x, rowY, 100, 28); x += 108;
    ctrl = GetDlgItem(st->hwnd, IDC_LOAD);          MoveWindowTop(ctrl, x, rowY, 90, 28);  x += 98;
    ctrl = GetDlgItem(st->hwnd, IDC_DELETE_PRESET); MoveWindowTop(ctrl, x, rowY, 70, 28);

    RedrawPresetControls(st);
}

static void CreateControls(AppState* st) {
    st->hPreviewCheck = CreateWindowExW(0, L"BUTTON", L"プレビュー", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 10, 10, 110, 24, st->hwnd, (HMENU)IDC_PREVIEW, st->hInst, nullptr);
    st->hAlwaysOnTopCheck = CreateWindowExW(0, L"BUTTON", L"常に最前面へ", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 10, 10, 150, 24, st->hwnd, (HMENU)IDC_ALWAYS_ON_TOP, st->hInst, nullptr);
    st->hLanguageButton = CreateWindowExW(0, L"BUTTON", L"言語 / Language", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 10, 10, 150, 26, st->hwnd, (HMENU)IDC_LANGUAGE, st->hInst, nullptr);
    st->hStatus = CreateWindowExW(0, L"STATIC", L"Ready", WS_CHILD | WS_VISIBLE, 10, 12, 940, 24, st->hwnd, (HMENU)IDC_STATUS, st->hInst, nullptr);

    st->hChainLabel = MakeLabel(st->hwnd, L"現在のチェーン（ドラッグで並べ替え可）");
    st->hCandidateLabel = MakeLabel(st->hwnd, L"利用可能フィルター（input.conf順）");

    st->hChain = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY, 10, 60, 360, 400, st->hwnd, (HMENU)IDC_CHAIN_LIST, st->hInst, nullptr);
    st->hCandidates = CreateWindowExW(WS_EX_CLIENTEDGE, WC_TREEVIEWW, L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | TVS_HASBUTTONS | TVS_HASLINES | TVS_LINESATROOT | TVS_SHOWSELALWAYS, 520, 60, 440, 400, st->hwnd, (HMENU)IDC_CANDIDATE_TREE, st->hInst, nullptr);

    MakeButton(st->hwnd, IDC_ADD, L"← 追加");
    MakeButton(st->hwnd, IDC_REMOVE, L"削除");
    MakeButton(st->hwnd, IDC_UP, L"上へ");
    MakeButton(st->hwnd, IDC_DOWN, L"下へ");
    // Keep these hidden for compatibility with older builds/layout state.
    CreateWindowExW(0, L"BUTTON", L"先頭へ", WS_CHILD, -300, -300, 1, 1, st->hwnd, (HMENU)IDC_TOP, st->hInst, nullptr);
    CreateWindowExW(0, L"BUTTON", L"末尾へ", WS_CHILD, -300, -300, 1, 1, st->hwnd, (HMENU)IDC_BOTTOM, st->hInst, nullptr);
    CreateWindowExW(0, L"BUTTON", L"候補更新", WS_CHILD, -300, -300, 1, 1, st->hwnd, (HMENU)IDC_REFRESH, st->hInst, nullptr);
    MakeButton(st->hwnd, IDC_CLEAR, L"フィルター全削除");

    st->hPresetGroup = CreateWindowExW(0, L"BUTTON", L"プリセット管理", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | BS_GROUPBOX, 10, 500, 940, 110, st->hwnd, nullptr, st->hInst, nullptr);
    st->hPresetSelectLabel = MakeLabel(st->hwnd, L"プリセット");
    st->hPresetNameLabel = MakeLabel(st->hwnd, L"");
    ShowWindow(st->hPresetNameLabel, SW_HIDE);
    st->hPresetCombo = CreateWindowExW(WS_EX_CLIENTEDGE, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL, 130, 524, 300, 300, st->hwnd, (HMENU)IDC_PRESET_COMBO, st->hInst, nullptr);
    st->hPresetName = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL, -300, -300, 1, 1, st->hwnd, (HMENU)IDC_PRESET_NAME, st->hInst, nullptr);
    MakeButton(st->hwnd, IDC_NEW_PRESET, L"新規保存");
    MakeButton(st->hwnd, IDC_SAVE, L"上書き保存");
    MakeButton(st->hwnd, IDC_LOAD, L"名前変更");
    MakeButton(st->hwnd, IDC_DELETE_PRESET, L"削除");

    SetWindowLongPtrW(st->hChain, GWLP_USERDATA, (LONG_PTR)st);
    st->oldChainProc = (WNDPROC)SetWindowLongPtrW(st->hChain, GWLP_WNDPROC, (LONG_PTR)ChainListProc);

    HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    HWND children[] = {
        st->hPreviewCheck, st->hAlwaysOnTopCheck, st->hLanguageButton, st->hStatus, st->hCandidateLabel, st->hChainLabel, st->hCandidates, st->hChain,
        st->hPresetGroup, st->hPresetSelectLabel, st->hPresetNameLabel, st->hPresetCombo, st->hPresetName,
        GetDlgItem(st->hwnd, IDC_ADD), GetDlgItem(st->hwnd, IDC_REMOVE), GetDlgItem(st->hwnd, IDC_UP),
        GetDlgItem(st->hwnd, IDC_DOWN), GetDlgItem(st->hwnd, IDC_CLEAR), GetDlgItem(st->hwnd, IDC_NEW_PRESET),
        GetDlgItem(st->hwnd, IDC_SAVE), GetDlgItem(st->hwnd, IDC_LOAD), GetDlgItem(st->hwnd, IDC_DELETE_PRESET),
        GetDlgItem(st->hwnd, IDC_TOP), GetDlgItem(st->hwnd, IDC_BOTTOM), GetDlgItem(st->hwnd, IDC_REFRESH)
    };
    for (HWND h : children) if (h) SendMessageW(h, WM_SETFONT, (WPARAM)font, TRUE);
    ApplyLanguage(st);
}

static void OnCommand(AppState* st, WPARAM wp, LPARAM) {
    int id = LOWORD(wp);
    int code = HIWORD(wp);

    switch (id) {
    case IDC_PRESET_COMBO:
        if (code == CBN_SELCHANGE && !st->suppressPresetComboEvent) {
            int sel = (int)SendMessageW(st->hPresetCombo, CB_GETCURSEL, 0, 0);
            if (sel >= 0 && sel < (int)st->presets.size()) {
                LoadPresetByIndex(st, sel, true);
                RealtimeApplyEditedChain(st);
            }
        }
        break;
    case IDC_ADD:
        if (AddSelectedCandidate(st)) RealtimeApplyEditedChain(st);
        break;
    case IDC_REMOVE:
        DeleteSelected(st);
        break;
    case IDC_UP:
        MoveSelected(st, 0);
        break;
    case IDC_DOWN:
        MoveSelected(st, 1);
        break;
    case IDC_TOP:
        break;
    case IDC_BOTTOM:
        break;
    case IDC_PREVIEW:
        TogglePreview(st);
        break;
    case IDC_ALWAYS_ON_TOP:
        ToggleAlwaysOnTop(st);
        break;
    case IDC_LANGUAGE:
        ToggleLanguage(st);
        break;
    case IDC_CLEAR:
        ClearChainAndMpv(st);
        break;
    case IDC_NEW_PRESET:
        NewPreset(st);
        break;
    case IDC_SAVE:
        PruneCurrentChainAgainstCandidates(st, true);
        SaveChain(st);
        break;
    case IDC_LOAD:
        RenameSelectedPreset(st);
        break;
    case IDC_DELETE_PRESET:
        DeleteSelectedPreset(st);
        break;
    case IDC_REFRESH:
        break;
    }
}

static void OnNotify(AppState* st, LPARAM lp) {
    NMHDR* hdr = (NMHDR*)lp;
    if (!hdr) return;
    if (hdr->idFrom == IDC_CANDIDATE_TREE && hdr->code == NM_DBLCLK) {
        if (AddSelectedCandidate(st)) RealtimeApplyEditedChain(st);
    }
}

static void DrawPreviewCheck(AppState* st, DRAWITEMSTRUCT* dis) {
    if (!st || !dis || dis->CtlID != IDC_PREVIEW) return;

    HDC dc = dis->hDC;
    RECT rc = dis->rcItem;
    bool pressed = (dis->itemState & ODS_SELECTED) != 0;

    COLORREF bg = st->previewEnabled ? RGB(40, 170, 90) : GetSysColor(COLOR_BTNFACE);
    COLORREF fg = st->previewEnabled ? RGB(255, 255, 255) : GetSysColor(COLOR_BTNTEXT);
    HBRUSH brush = CreateSolidBrush(bg);
    FillRect(dc, &rc, brush);
    DeleteObject(brush);

    DrawEdge(dc, &rc, pressed ? EDGE_SUNKEN : EDGE_RAISED, BF_RECT);
    if (pressed) OffsetRect(&rc, 1, 1);

    wchar_t text[128]{};
    GetWindowTextW(dis->hwndItem, text, 128);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, fg);
    DrawTextW(dc, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    if (dis->itemState & ODS_FOCUS) {
        RECT focus = dis->rcItem;
        InflateRect(&focus, -4, -4);
        DrawFocusRect(dc, &focus);
    }
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    AppState* st = (AppState*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    switch (msg) {
    case WM_GETMINMAXINFO: {
        // Fixed-size distribution UI.  The layout is intentionally not resizable,
        // because the center button column and preset area use fixed pixel widths.
        // Keeping min/max track sizes identical prevents controls from being pushed
        // out of view by horizontal or vertical resize operations.
        MINMAXINFO* mmi = (MINMAXINFO*)lp;
        RECT rc{0, 0, APP_FIXED_CLIENT_W, APP_FIXED_CLIENT_H};
        AdjustWindowRectEx(&rc, APP_WINDOW_STYLE, FALSE, 0);
        const LONG fixedW = rc.right - rc.left;
        const LONG fixedH = rc.bottom - rc.top;
        mmi->ptMinTrackSize.x = fixedW;
        mmi->ptMinTrackSize.y = fixedH;
        mmi->ptMaxTrackSize.x = fixedW;
        mmi->ptMaxTrackSize.y = fixedH;
        return 0;
    }
    case WM_DRAWITEM:
        if (st && ((DRAWITEMSTRUCT*)lp)->CtlID == IDC_PREVIEW) {
            DrawPreviewCheck(st, (DRAWITEMSTRUCT*)lp);
            return TRUE;
        }
        break;
    case WM_CREATE: {
        CREATESTRUCTW* cs = (CREATESTRUCTW*)lp;
        st = (AppState*)cs->lpCreateParams;
        st->hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)st);
        CreateControls(st);
        EnsureDefaultJson(*st);
        LoadCandidates(st);
        RefreshCandidateTree(st);
        LoadChain(st);
        ApplyLanguage(st);
        ApplyAlwaysOnTop(st);
        if (st->previewEnabled) ApplyChain(st, false);
        else ClearMpvFiltersOnly(st, false);
        SetTimer(hwnd, 1, 250, nullptr);
        return 0;
    }
    case WM_SIZE:
        if (st && wp != SIZE_MINIMIZED) {
            // The window is fixed-size, but WM_SIZE can still be delivered on
            // creation/restore.  Use the actual client size supplied by Windows.
            LayoutControls(st, LOWORD(lp), HIWORD(lp));
        }
        return 0;
    case WM_COMMAND:
        if (st) OnCommand(st, wp, lp);
        return 0;
    case WM_NOTIFY:
        if (st) OnNotify(st, lp);
        return 0;
    case WM_TIMER:
        if (st) {
            if (st->hResetEvent && WaitForSingleObject(st->hResetEvent, 0) == WAIT_OBJECT_0) {
                ResetEvent(st->hResetEvent);
                ClearChainAndMpv(st);
            }
            if (st->everConnectedToMpv) {
                if (!TestPipe(st)) {
                    st->pipeLostCount++;
                    if (st->pipeLostCount >= 2) DestroyWindow(hwnd);
                }
            }
        }
        return 0;
    case WM_DESTROY:
        if (st) {
            std::wstring last = L"Custom 1";
            int sel = (int)SendMessageW(st->hPresetCombo, CB_GETCURSEL, 0, 0);
            if (sel >= 0 && sel < (int)st->presets.size()) last = st->presets[(size_t)sel].name;
            SaveAllPresets(st, last);
        }
        KillTimer(hwnd, 1);
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wp, lp);
}

static CmdOptions ParseCommandLine() {
    CmdOptions opt;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return opt;
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        if (a == L"--reset-ui") opt.resetUi = true;
        else if (StartsWithI(a, L"--pipe=")) opt.pipePath = a.substr(7);
    }
    LocalFree(argv);
    return opt;
}

int APIENTRY wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int nCmdShow) {
    CmdOptions opt = ParseCommandLine();

    HANDLE hMutex = CreateMutexW(nullptr, FALSE, APP_MUTEX_NAME);
    bool already = (GetLastError() == ERROR_ALREADY_EXISTS);

    if (already) {
        if (opt.resetUi) {
            HANDLE ev = OpenEventW(EVENT_MODIFY_STATE, FALSE, APP_RESET_EVENT_NAME);
            if (ev) {
                SetEvent(ev);
                CloseHandle(ev);
            }
            SendMpvCommandRaw(opt.pipePath, {L"set", L"video-aspect-override", L"-1"});
            SendMpvCommandRaw(opt.pipePath, {L"vf", L"clr", L""});
            SendMpvCommandRaw(opt.pipePath, {L"change-list", L"glsl-shaders", L"clr", L""});
        } else {
            HWND existing = FindWindowW(APP_CLASS_NAME, nullptr);
            if (existing) {
                ShowWindow(existing, SW_SHOWNORMAL);
                SetForegroundWindow(existing);
            }
        }
        if (hMutex) CloseHandle(hMutex);
        return 0;
    }

    AppState st;
    st.hInst = hInst;
    st.baseDir = GetModuleDir();
    st.pipePath = opt.pipePath;
    st.hResetEvent = CreateEventW(nullptr, TRUE, FALSE, APP_RESET_EVENT_NAME);

    if (opt.resetUi) {
        EnsureDefaultJson(st);
        SendMpvCommandRaw(st.pipePath, {L"set", L"video-aspect-override", L"-1"});
        SendMpvCommandRaw(st.pipePath, {L"vf", L"clr", L""});
        SendMpvCommandRaw(st.pipePath, {L"change-list", L"glsl-shaders", L"clr", L""});
        if (st.hResetEvent) CloseHandle(st.hResetEvent);
        if (hMutex) CloseHandle(hMutex);
        return 0;
    }

    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_STANDARD_CLASSES | ICC_TREEVIEW_CLASSES;
    InitCommonControlsEx(&icc);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = APP_CLASS_NAME;
    RegisterClassExW(&wc);

    RECT windowRc{0, 0, APP_FIXED_CLIENT_W, APP_FIXED_CLIENT_H};
    AdjustWindowRectEx(&windowRc, APP_WINDOW_STYLE, FALSE, 0);
    const int fixedWindowW = windowRc.right - windowRc.left;
    const int fixedWindowH = windowRc.bottom - windowRc.top;

    HWND hwnd = CreateWindowExW(0, APP_CLASS_NAME, L"mpv filter chain UI", APP_WINDOW_STYLE,
        CW_USEDEFAULT, CW_USEDEFAULT, fixedWindowW, fixedWindowH, nullptr, nullptr, hInst, &st);
    if (!hwnd) {
        if (st.hResetEvent) CloseHandle(st.hResetEvent);
        if (hMutex) CloseHandle(hMutex);
        return 1;
    }

    // Always start in normal fixed-size mode.  This prevents shortcut settings or
    // shell launch state from opening the UI maximized and breaking the fixed layout.
    ShowWindow(hwnd, SW_SHOWNORMAL);
    UpdateWindow(hwnd);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (st.hResetEvent) CloseHandle(st.hResetEvent);
    if (hMutex) CloseHandle(hMutex);
    return 0;
}
