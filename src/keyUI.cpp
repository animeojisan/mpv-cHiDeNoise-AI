// keyUI.cpp - mpv portable input.conf keybind editor
// Build (MSVC x64):
//   cl /utf-8 /std:c++17 /EHsc keyUI.cpp user32.lib gdi32.lib comctl32.lib shlwapi.lib
//
// Portable layout:
//   mpv-portable/
//     mpv.exe
//     keyUI.exe
//     portable_config/
//       mpv.conf
//       input.conf
//
// Key field behavior (record-style):
// - You DO NOT type letters. WM_CHAR is suppressed while recording.
// - Press modifiers first => shows "Alt+" / "Ctrl+Alt+" etc and waits for next key.
// - Press next key => becomes "Alt+j" / "Ctrl+KP6" etc.
// - You may release modifier before pressing next key; it still combines (kept in pending mask).
// - Leaving the Key field or pressing OK while still waiting (ends with '+') auto-trims trailing '+'.

#define NOMINMAX
#define UNICODE
#define _UNICODE
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#include <windows.h>
#include <commctrl.h>
#include <shlwapi.h>

#include <string>
#include <vector>
#include <sstream>
#include <algorithm>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shlwapi.lib")

#pragma comment(linker, \
"\"/manifestdependency:type='win32' "\
"name='Microsoft.Windows.Common-Controls' "\
"version='6.0.0.0' processorArchitecture='*' "\
"publicKeyToken='6595b64144ccf1df' language='*'\"")

static const wchar_t* APP_TITLE_BASE = L"keyUI - mpv input.conf editor";

static const int IDC_SEARCH_LABEL = 100;
static const int IDC_SEARCH_EDIT  = 101;
static const int IDC_SEARCH_CLEAR = 102;

static const int IDC_LIST         = 200;

static const int IDC_BTN_ADD      = 300;
static const int IDC_BTN_EDIT     = 301;
static const int IDC_BTN_REMOVE   = 302;
static const int IDC_BTN_RELOAD   = 303;
static const int IDC_BTN_SAVE     = 304;
static const int IDC_BTN_CLOSE    = 305;

static const int IDC_STATUS       = 400;

static const int IDC_MODAL_MOUSE  = 510;
static const int IDC_MODAL_MANUAL = 511;

static HFONT gFont = nullptr;

static std::wstring gExeDir;
static std::wstring gInputPath;

struct Line { std::wstring raw; };

struct Binding {
    std::wstring key;          // key token
    std::wstring cmdDisplay;   // trimmed for list display
    std::wstring menuDisplay;  // extracted #menu: value (trimmed)

    // Format-preserving pieces for save
    std::wstring prefix;       // leading spaces before key
    std::wstring sep;          // whitespace between key and command
    std::wstring cmdRaw;       // command segment as-is
    std::wstring commentRaw;   // comment tail including leading '#', as-is (may be empty)

    int lineIndex = -1;        // line index in gLines, -1 for newly added
    bool deleted = false;
};

static std::vector<Line> gLines;
static std::vector<Binding> gBindings;
static std::vector<int> gFiltered;

static std::wstring gSearch;

static HWND gHwnd = nullptr;
static HWND gSearchEdit = nullptr;
static HWND gList = nullptr;
static HWND gStatus = nullptr;

static bool gInSizeMove = false;

static bool gDirty = false;
// Do NOT write to input.conf on each Add/Edit/Remove commit.
// Write only when user clicks Save.
static bool gAutoSave = false;

// ---------------- helpers ----------------
static std::wstring ws_trim(const std::wstring& s) {
    size_t b = 0, e = s.size();
    while (b < e && iswspace((wint_t)s[b])) b++;
    while (e > b && iswspace((wint_t)s[e - 1])) e--;
    return s.substr(b, e - b);
}
static std::wstring ws_lower(std::wstring s) {
    for (auto& ch : s) ch = (wchar_t)towlower((wint_t)ch);
    return s;
}
static void status_set(const std::wstring& msg) {
    if (!gStatus) return;
    SendMessageW(gStatus, SB_SETTEXTW, 0, (LPARAM)msg.c_str());
}
static std::wstring get_exe_dir() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    PathRemoveFileSpecW(path);
    return path;
}
static std::wstring join_path(const std::wstring& a, const std::wstring& b) {
    wchar_t buf[MAX_PATH]{};
    lstrcpynW(buf, a.c_str(), MAX_PATH);
    PathAppendW(buf, b.c_str());
    return buf;
}

static bool read_text_file_utf8_or_ansi(const std::wstring& path, std::wstring& out) {
    out.clear();
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD size = GetFileSize(h, nullptr);
    if (size == INVALID_FILE_SIZE) { CloseHandle(h); return false; }
    std::vector<char> buf(size + 1);
    DWORD read = 0;
    bool ok = ReadFile(h, buf.data(), size, &read, nullptr) != 0;
    CloseHandle(h);
    if (!ok) return false;
    buf[read] = 0;

    int wlen = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, buf.data(), (int)read, nullptr, 0);
    if (wlen > 0) {
        out.resize((size_t)wlen);
        MultiByteToWideChar(CP_UTF8, 0, buf.data(), (int)read, &out[0], wlen);
        return true;
    }
    wlen = MultiByteToWideChar(CP_ACP, 0, buf.data(), (int)read, nullptr, 0);
    if (wlen <= 0) return false;
    out.resize((size_t)wlen);
    MultiByteToWideChar(CP_ACP, 0, buf.data(), (int)read, &out[0], wlen);
    return true;
}

static bool write_text_file_utf8(const std::wstring& path, const std::wstring& text) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    int blen = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0, nullptr, nullptr);
    if (blen < 0) { CloseHandle(h); return false; }
    std::vector<char> buf((size_t)blen);
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), buf.data(), blen, nullptr, nullptr);

    DWORD written = 0;
    bool ok = WriteFile(h, buf.data(), (DWORD)buf.size(), &written, nullptr) != 0;
    CloseHandle(h);
    return ok && written == buf.size();
}

static std::wstring extract_menu_value(const std::wstring& commentRaw) {
    if (commentRaw.empty()) return L"";
    std::wstring low = ws_lower(commentRaw);
    size_t p = low.find(L"#menu:");
    if (p == std::wstring::npos) return L"";
    std::wstring v = commentRaw.substr(p + 6);
    return ws_trim(v);
}
static std::wstring apply_menu_to_comment(const std::wstring& oldCommentRaw, const std::wstring& newMenuTrimmed) {
    std::wstring c = oldCommentRaw;
    std::wstring low = ws_lower(c);
    size_t p = low.find(L"#menu:");
    if (p != std::wstring::npos) {
        size_t after = p + 6;
        size_t keep = after;
        while (keep < c.size() && iswspace((wint_t)c[keep])) keep++;
        std::wstring midWS = c.substr(after, keep - after);
        std::wstring head = c.substr(0, after) + midWS;
        return head + newMenuTrimmed;
    } else {
        if (newMenuTrimmed.empty()) return c;
        if (c.empty()) return L"#menu: " + newMenuTrimmed;
        return c + L"  #menu: " + newMenuTrimmed;
    }
}

// -------------- parsing / preservation --------------
static bool looks_like_binding_line(const std::wstring& raw) {
    size_t n = raw.size();
    size_t i = 0;
    while (i < n && iswspace((wint_t)raw[i])) i++;
    if (i >= n) return false;
    if (raw[i] == L'#') return false;

    size_t keyStart = i;
    while (i < n && !iswspace((wint_t)raw[i])) i++;
    if (i == keyStart) return false;

    size_t wsStart = i;
    while (i < n && iswspace((wint_t)raw[i])) i++;
    if (i == wsStart) return false;
    if (i >= n) return false;
    if (raw[i] == L'#') return false;
    return true;
}

static void parse_input_conf(const std::wstring& text) {
    gLines.clear();
    gBindings.clear();

    std::wstringstream ss(text);
    std::wstring raw;
    int lineIndex = 0;

    while (std::getline(ss, raw)) {
        if (!raw.empty() && raw.back() == L'\r') raw.pop_back();
        gLines.push_back(Line{raw});

        if (looks_like_binding_line(raw)) {
            Binding b;
            b.lineIndex = lineIndex;

            size_t n = raw.size();
            size_t i = 0;
            while (i < n && iswspace((wint_t)raw[i])) i++;
            size_t keyStart = i;
            while (i < n && !iswspace((wint_t)raw[i])) i++;
            size_t keyEnd = i;

            size_t wsStart = i;
            while (i < n && iswspace((wint_t)raw[i])) i++;
            size_t cmdStart = i;

            b.prefix = raw.substr(0, keyStart);
            b.key = raw.substr(keyStart, keyEnd - keyStart);
            b.sep = raw.substr(wsStart, cmdStart - wsStart);

            size_t hash = raw.find(L'#', cmdStart);
            if (hash == std::wstring::npos) {
                b.cmdRaw = raw.substr(cmdStart);
                b.commentRaw = L"";
            } else {
                b.cmdRaw = raw.substr(cmdStart, hash - cmdStart);
                b.commentRaw = raw.substr(hash);
            }

            b.cmdDisplay = ws_trim(b.cmdRaw);
            b.menuDisplay = extract_menu_value(b.commentRaw);

            gBindings.push_back(std::move(b));
        }
        lineIndex++;
    }
}

static bool load_input_conf() {
    std::wstring content;
    if (!read_text_file_utf8_or_ansi(gInputPath, content)) {
        std::wstring dir = join_path(gExeDir, L"portable_config");
        CreateDirectoryW(dir.c_str(), nullptr);
        content = L"# input.conf\r\n";
        if (!write_text_file_utf8(gInputPath, content)) return false;
    }
    parse_input_conf(content);
    return true;
}

static std::wstring build_line_from_binding(const Binding& b) {
    // If key is empty, emit "_" as the key token (common style in mpv input.conf)
    // so menu items can exist without a binding.
    std::wstring keyTok = ws_trim(b.key).empty() ? L"_" : b.key;
    return b.prefix + keyTok + b.sep + b.cmdRaw + b.commentRaw;
}

static bool save_input_conf() {
    std::vector<std::wstring> outLines;
    outLines.reserve(gLines.size());
    for (auto& L : gLines) outLines.push_back(L.raw);

    for (const auto& b : gBindings) {
        if (b.lineIndex >= 0 && b.lineIndex < (int)outLines.size()) {
            if (b.deleted) outLines[b.lineIndex] = L"";
            else outLines[b.lineIndex] = build_line_from_binding(b);
        }
    }
    for (const auto& b : gBindings) {
        if (b.lineIndex < 0 && !b.deleted) outLines.push_back(build_line_from_binding(b));
    }

    std::wstring text;
    for (size_t i = 0; i < outLines.size(); ++i) {
        text += outLines[i];
        text += L"\r\n";
    }
    return write_text_file_utf8(gInputPath, text);
}

// ---------------- list view ----------------
static void list_clear() {
    if (gList) SendMessageW(gList, LVM_DELETEALLITEMS, 0, 0);
}
static void list_insert_columns() {
    LVCOLUMNW col{};
    col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
    col.fmt = LVCFMT_LEFT;

    col.cx = 160; col.pszText = (LPWSTR)L"Key";
    SendMessageW(gList, LVM_INSERTCOLUMNW, 0, (LPARAM)&col);
    col.cx = 560; col.pszText = (LPWSTR)L"Command";
    SendMessageW(gList, LVM_INSERTCOLUMNW, 1, (LPARAM)&col);
    col.cx = 260; col.pszText = (LPWSTR)L"Menu (#menu:)";
    SendMessageW(gList, LVM_INSERTCOLUMNW, 2, (LPARAM)&col);
}
static void list_set_item_text(int row, int col, const std::wstring& text) {
    LVITEMW it{};
    it.iSubItem = col;
    it.pszText = (LPWSTR)text.c_str();
    SendMessageW(gList, LVM_SETITEMTEXTW, row, (LPARAM)&it);
}
static void rebuild_filtered() {
    gFiltered.clear();
    std::wstring q = ws_lower(ws_trim(gSearch));
    for (int i = 0; i < (int)gBindings.size(); ++i) {
        const auto& b = gBindings[i];
        if (b.deleted) continue;
        if (q.empty()) { gFiltered.push_back(i); continue; }
        std::wstring hay = ws_lower(b.key + L" " + b.cmdDisplay + L" " + b.menuDisplay);
        if (hay.find(q) != std::wstring::npos) gFiltered.push_back(i);
    }
}
static void list_populate() {
    list_clear();
    rebuild_filtered();

    LVITEMW item{};
    item.mask = LVIF_TEXT | LVIF_PARAM;

    for (int row = 0; row < (int)gFiltered.size(); ++row) {
        int idx = gFiltered[row];
        const auto& b = gBindings[idx];

        item.iItem = row;
        item.iSubItem = 0;
        item.pszText = (LPWSTR)b.key.c_str();
        item.lParam = (LPARAM)idx;
        int inserted = (int)SendMessageW(gList, LVM_INSERTITEMW, 0, (LPARAM)&item);
        if (inserted >= 0) {
            list_set_item_text(inserted, 1, b.cmdDisplay);
            list_set_item_text(inserted, 2, b.menuDisplay);
        }
    }

    std::wstringstream st;
    st << L"input.conf: " << gInputPath << L"    items: " << (int)gFiltered.size();
    status_set(st.str());
}
static int list_get_selected_binding_index() {
    int sel = (int)SendMessageW(gList, LVM_GETNEXTITEM, (WPARAM)-1, (LPARAM)LVNI_SELECTED);
    if (sel < 0) return -1;

    LVITEMW it{};
    it.mask = LVIF_PARAM;
    it.iItem = sel;
    it.iSubItem = 0;
    if (!SendMessageW(gList, LVM_GETITEMW, 0, (LPARAM)&it)) return -1;
    return (int)it.lParam;
}

static void list_select_binding_index(int bindingIndex) {
    if (bindingIndex < 0) return;
    int count = (int)SendMessageW(gList, LVM_GETITEMCOUNT, 0, 0);
    for (int row = 0; row < count; ++row) {
        LVITEMW it{};
        it.mask = LVIF_PARAM;
        it.iItem = row;
        it.iSubItem = 0;
        if (!SendMessageW(gList, LVM_GETITEMW, 0, (LPARAM)&it)) continue;
        if ((int)it.lParam == bindingIndex) {
            LVITEMW st{};
            st.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
            st.state = LVIS_SELECTED | LVIS_FOCUSED;
            SendMessageW(gList, LVM_SETITEMSTATE, (WPARAM)row, (LPARAM)&st);
            SendMessageW(gList, LVM_ENSUREVISIBLE, (WPARAM)row, (LPARAM)FALSE);
            break;
        }
    }
}

// ---------------- key naming (mpv-ish) ----------------
static std::wstring vk_to_base(UINT vk) {
    switch (vk) {
        case VK_LEFT: return L"LEFT";
        case VK_RIGHT: return L"RIGHT";
        case VK_UP: return L"UP";
        case VK_DOWN: return L"DOWN";
        case VK_SPACE: return L"SPACE";
        case VK_RETURN: return L"ENTER";
        case VK_ESCAPE: return L"ESC";
        case VK_TAB: return L"TAB";
        case VK_BACK: return L"BS";
        case VK_DELETE: return L"DEL";
        case VK_INSERT: return L"INS";
        case VK_HOME: return L"HOME";
        case VK_END: return L"END";
        case VK_PRIOR: return L"PGUP";
        case VK_NEXT: return L"PGDWN";
        case VK_PAUSE: return L"PAUSE";
        case VK_SNAPSHOT: return L"PRINT";
        case VK_APPS: return L"MENU";
        default: break;
    }
    if (vk >= VK_F1 && vk <= VK_F24) {
        wchar_t buf[16]; wsprintfW(buf, L"F%u", (unsigned)(vk - VK_F1 + 1));
        return buf;
    }
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) {
        wchar_t buf[16]; wsprintfW(buf, L"KP%u", (unsigned)(vk - VK_NUMPAD0));
        return buf;
    }
    if (vk >= 'A' && vk <= 'Z') {
        wchar_t ch = (wchar_t)vk;
        ch = (wchar_t)towlower((wint_t)ch); // always lower; Shift will be explicit
        return std::wstring(1, ch);
    }
    if (vk >= '0' && vk <= '9') return std::wstring(1, (wchar_t)vk);
    return L"";
}

static bool is_modifier_vk(UINT vk) {
    return (vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU ||
            vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL ||
            vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT ||
            vk == VK_LWIN || vk == VK_RWIN);
}

static std::wstring mod_name_from_vk(UINT vk) {
    switch (vk) {
        case VK_MENU: case VK_LMENU: case VK_RMENU: return L"Alt";
        case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL: return L"Ctrl";
        case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT: return L"Shift";
        case VK_LWIN: case VK_RWIN: return L"Win";
        default: return L"";
    }
}

enum ModBits : unsigned {
    KMOD_CTRL  = 1u << 0,
    KMOD_ALT   = 1u << 1,
    KMOD_SHIFT = 1u << 2,
    KMOD_WIN   = 1u << 3,
};

static unsigned modbit_from_vk(UINT vk) {
    switch (vk) {
        case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL: return KMOD_CTRL;
        case VK_MENU: case VK_LMENU: case VK_RMENU: return KMOD_ALT;
        case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT: return KMOD_SHIFT;
        case VK_LWIN: case VK_RWIN: return KMOD_WIN;
        default: return 0;
    }
}

static std::wstring mods_to_text(unsigned mask, bool trailingPlus) {
    std::wstring out;
    if (mask & KMOD_CTRL)  out += L"Ctrl+";
    if (mask & KMOD_ALT)   out += L"Alt+";
    if (mask & KMOD_SHIFT) out += L"Shift+";
    if (mask & KMOD_WIN)   out += L"Win+";
    if (!trailingPlus && !out.empty() && out.back() == L'+') out.pop_back();
    return out;
}

// ---------- edit dialog ----------
struct ModalEdit {
    int editingLineIndex = -1; // lineIndex of binding being edited, -1 when adding
    std::wstring originalKeyLower;
    HWND hwnd = nullptr;
    HWND edtKey = nullptr;
    HWND edtCmd = nullptr;
    HWND edtMenu = nullptr;
    HWND hint = nullptr;
    HWND btnMouse = nullptr;
    HWND btnManual = nullptr;

    bool recording = false;
    bool manualMode = false; // allow typing key names (mouse, etc.)

    unsigned pendingMods = 0;      // modifiers pressed before main key (sticky until combined)
    std::wstring keyBeforeRecord;

    bool done = false;
    bool ok = false;

    std::wstring key;
    std::wstring cmd;   // raw (spacing preserved)
    std::wstring menu;  // trimmed
};

static void key_record_begin(ModalEdit* st) {
    if (!st) return;
    st->manualMode = false;
    st->recording = true;
    st->pendingMods = 0;
    wchar_t buf[512]{};
    GetWindowTextW(st->edtKey, buf, 512);
    st->keyBeforeRecord = buf;
    status_set(L"Key capture: press modifiers then a key (e.g. Alt then j => Alt+j). Esc cancels.");
}

static void trim_trailing_plus(HWND edit) {
    wchar_t buf[512]{};
    GetWindowTextW(edit, buf, 512);
    std::wstring s = ws_trim(buf);
    while (!s.empty() && s.back() == L'+') s.pop_back();
    SetWindowTextW(edit, s.c_str());
}


static void key_set_or_append(HWND edit, const std::wstring& token) {
    if (!edit) return;
    wchar_t buf[512]{};
    GetWindowTextW(edit, buf, 512);
    std::wstring cur = ws_trim(buf);
    if (!cur.empty() && cur.back() == L'+') {
        cur += token;
        SetWindowTextW(edit, cur.c_str());
    } else {
        SetWindowTextW(edit, token.c_str());
    }
}

static void key_record_end(ModalEdit* st) {
    if (!st) return;
    st->recording = false;
    st->pendingMods = 0;
    if (st->edtKey) trim_trailing_plus(st->edtKey);
    status_set(L"");
}

// Key edit subclass: record-style
static LRESULT CALLBACK KeyEditSubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
                                            UINT_PTR, DWORD_PTR dwRefData) {
    ModalEdit* st = (ModalEdit*)dwRefData;

    if (st && st->manualMode) {
        // Manual mode: allow normal typing/editing (for MBTN_Left, Wheel_Up, etc.)
        return DefSubclassProc(hWnd, uMsg, wParam, lParam);
    }

    switch (uMsg) {
    case WM_SETFOCUS:
        if (st) key_record_begin(st);
        break;

    case WM_KILLFOCUS:
        if (st) key_record_end(st);
        break;

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (st && st->recording) {
            UINT vk = (UINT)wParam;

            if (vk == VK_ESCAPE) {
                SetWindowTextW(hWnd, st->keyBeforeRecord.c_str());
                key_record_end(st);
                status_set(L"Key capture cancelled");
                return 0;
            }
            if (vk == VK_BACK) {
                st->pendingMods = 0;
                SetWindowTextW(hWnd, L"");
                return 0;
            }

            // Do not capture bare Delete key while recording.
            // Use it to clear the key field instead (keeps "Delete to clear" behavior usable).
            // If modifiers are held (e.g. Ctrl+Delete), allow capturing it normally.
            if (vk == VK_DELETE && st->pendingMods == 0) {
                SetWindowTextW(hWnd, L"");
                key_record_end(st);
                status_set(L"Key cleared");
                return 0;
            }

            if (is_modifier_vk(vk)) {
                st->pendingMods |= modbit_from_vk(vk);
                std::wstring wait = mods_to_text(st->pendingMods, true); // with trailing '+'
                if (wait.empty()) wait = mod_name_from_vk(vk) + L"+";
                SetWindowTextW(hWnd, wait.c_str());
                return 0;
            }

            std::wstring base = vk_to_base(vk);
            if (base.empty()) return 0;

            std::wstring out;
            if (st->pendingMods) out = mods_to_text(st->pendingMods, true);
            out += base;

            SetWindowTextW(hWnd, out.c_str());

            // After combining, clear pending mods (so next key starts fresh)
            st->pendingMods = 0;
            return 0;
        }
        break;

    case WM_CHAR:
    case WM_SYSCHAR:
        if (st && st->recording) return 0; // suppress typing
        break;
    }
    return DefSubclassProc(hWnd, uMsg, wParam, lParam);
}

static LRESULT CALLBACK ModalEditProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    ModalEdit* st = (ModalEdit*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    switch (msg) {
    case WM_NCCREATE:
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)((LPCREATESTRUCTW)lParam)->lpCreateParams);
        return TRUE;

    case WM_CREATE: {
        st = (ModalEdit*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
        const int pad = 12;
        const int lblW = 90;
        const int rowH = 24;
        const int gapY = 10;

        RECT rc; GetClientRect(hwnd, &rc);
        int w = rc.right - rc.left;

        auto mkLabel = [&](int y, const wchar_t* text) {
            HWND h = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE,
                                     pad, y + 4, lblW, rowH, hwnd, 0, GetModuleHandleW(nullptr), 0);
            SendMessageW(h, WM_SETFONT, (WPARAM)gFont, TRUE);
            return h;
        };

        int xEdit = pad + lblW + 8;
        int wEdit = w - xEdit - pad;

        const int btnW = 90;
        const int btnGap = 8;
        int wKeyEdit = wEdit - (btnW * 2 + btnGap * 2);
        if (wKeyEdit < 120) wKeyEdit = 120;

        mkLabel(pad, L"Key");
        st->edtKey = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                     WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                     xEdit, pad, wKeyEdit, rowH, hwnd, (HMENU)(INT_PTR)10, GetModuleHandleW(nullptr), 0);

        st->btnMouse = CreateWindowExW(0, L"BUTTON", L"Mouse",
                                       WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                       xEdit + wKeyEdit + btnGap, pad, btnW, rowH, hwnd, (HMENU)(INT_PTR)IDC_MODAL_MOUSE, GetModuleHandleW(nullptr), 0);

        st->btnManual = CreateWindowExW(0, L"BUTTON", L"Manual",
                                        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                        xEdit + wKeyEdit + btnGap + btnW + btnGap, pad, btnW, rowH, hwnd, (HMENU)(INT_PTR)IDC_MODAL_MANUAL, GetModuleHandleW(nullptr), 0);

        mkLabel(pad + rowH + gapY, L"Command");
        st->edtCmd = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                     WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                     xEdit, pad + rowH + gapY, wEdit, rowH, hwnd, (HMENU)(INT_PTR)12, GetModuleHandleW(nullptr), 0);

        mkLabel(pad + 2 * (rowH + gapY), L"Menu");
        st->edtMenu = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                      WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                      xEdit, pad + 2 * (rowH + gapY), wEdit, rowH, hwnd, (HMENU)(INT_PTR)13, GetModuleHandleW(nullptr), 0);

        st->hint = CreateWindowExW(0, L"STATIC",
            L"Key: Record captures real keys (Alt then j => Alt+j, Numpad => KP6). Manual allows typing (e.g. MBTN_Left, Wheel_Up). Mouse inserts common mouse tokens.",
            WS_CHILD | WS_VISIBLE,
            pad, pad + 3 * (rowH + gapY) + 2, w - 2 * pad, rowH, hwnd, 0, GetModuleHandleW(nullptr), 0);

        HWND btnOK = CreateWindowExW(0, L"BUTTON", L"OK",
                                     WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                                     w - pad - 180, rc.bottom - pad - 28, 80, 28, hwnd, (HMENU)(INT_PTR)IDOK, GetModuleHandleW(nullptr), 0);
        HWND btnCancel = CreateWindowExW(0, L"BUTTON", L"Cancel",
                                         WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                         w - pad - 90, rc.bottom - pad - 28, 80, 28, hwnd, (HMENU)(INT_PTR)IDCANCEL, GetModuleHandleW(nullptr), 0);

        for (HWND h : { st->edtKey, st->edtCmd, st->edtMenu, st->btnMouse, st->btnManual, btnOK, btnCancel, st->hint }) {
            SendMessageW(h, WM_SETFONT, (WPARAM)gFont, TRUE);
        }
        SendMessageW(hwnd, WM_SETFONT, (WPARAM)gFont, TRUE);

        SetWindowSubclass(st->edtKey, KeyEditSubclassProc, 1, (DWORD_PTR)st);
        return 0;
    }

    case WM_ERASEBKGND: {
        RECT rc; GetClientRect(hwnd, &rc);
        FillRect((HDC)wParam, &rc, (HBRUSH)(COLOR_WINDOW + 1));
        return 1;
    }

    case WM_COMMAND:
        if (!st) break;
        switch (LOWORD(wParam)) {
        case IDC_MODAL_MANUAL: {
            st->manualMode = !st->manualMode;
            if (st->manualMode) {
                key_record_end(st);
                SetWindowTextW(st->btnManual, L"Record");
                status_set(L"Manual key entry: type key name (e.g. MBTN_Left, Wheel_Up).");
            } else {
                SetWindowTextW(st->btnManual, L"Manual");
                key_record_begin(st);
            }
            SetFocus(st->edtKey);
            return 0;
        }

        case IDC_MODAL_MOUSE: {
            HMENU m = CreatePopupMenu();
            struct Item { int id; const wchar_t* text; } items[] = {
                { 1, L"MBTN_Left" },
                { 2, L"MBTN_Right" },
                { 3, L"MBTN_Mid" },
                { 4, L"MBTN_Back" },
                { 5, L"MBTN_Forward" },
                { 6, L"Wheel_Up" },
                { 7, L"Wheel_Down" },
                { 8, L"Wheel_Left" },
                { 9, L"Wheel_Right" },
            };
            for (auto& it : items) AppendMenuW(m, MF_STRING, (UINT_PTR)it.id, it.text);

            POINT pt{};
            GetCursorPos(&pt);
            int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
            DestroyMenu(m);

            if (cmd >= 1 && cmd <= 9) {
                const wchar_t* token = items[cmd - 1].text;

                // Switch to manual so the user can tweak the text if needed.
                st->manualMode = true;
                SetWindowTextW(st->btnManual, L"Record");
                key_record_end(st);

                key_set_or_append(st->edtKey, token);
                SetFocus(st->edtKey);
            }
            return 0;
        }

        case IDOK: {
            if (st->edtKey) trim_trailing_plus(st->edtKey);

            wchar_t kbuf[512]{}, cbuf[4096]{}, mbuf[2048]{};
            GetWindowTextW(st->edtKey, kbuf, 512);
            GetWindowTextW(st->edtCmd, cbuf, 4096);
            GetWindowTextW(st->edtMenu, mbuf, 2048);

            st->key = ws_trim(kbuf);
            st->cmd = std::wstring(cbuf);
            st->menu = ws_trim(mbuf);

            // Allow empty key: treat it as "_" (common mpv input.conf style for unbound menu items).
            if (st->key.empty()) {
                st->key = L"_";
                SetWindowTextW(st->edtKey, L"_");
            }

			if (ws_trim(st->cmd).empty()) {
				MessageBoxW(hwnd, L"Command is required.\n\nコマンドは必須です。", APP_TITLE_BASE, MB_ICONWARNING);
                return 0;
            }

            // Duplicate key warning (ignore the binding being edited).
            if (!st->key.empty() && st->key != L"_") {
                std::wstring kLower = ws_lower(st->key);
                bool dup = false;
                for (const auto& b : gBindings) {
                    if (b.deleted) continue;
                    if (ws_lower(b.key) != kLower) continue;
                    if (st->editingLineIndex >= 0 && b.lineIndex == st->editingLineIndex) continue;
                    dup = true;
                    break;
                }
                if (dup) {
                    std::wstring msg = L"同じキーが既に存在しますがそれでも保存しますか？\r\n\r\nKey: ";
                    msg += st->key;
                    msg += L"\r\n\r\n(Another binding already uses this key. Save anyway?)";
                    int r = MessageBoxW(hwnd, msg.c_str(), APP_TITLE_BASE, MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
                    if (r != IDYES) return 0;
                }
            }

            st->ok = true;
            st->done = true;
            DestroyWindow(hwnd);
            return 0;
        }

        case IDCANCEL:
            st->ok = false;
            st->done = true;
            DestroyWindow(hwnd);
            return 0;
        }
        break;

    case WM_CLOSE:
        if (st) { st->ok = false; st->done = true; }
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        if (st && st->edtKey) RemoveWindowSubclass(st->edtKey, KeyEditSubclassProc, 1);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static bool modal_edit(HWND parent, const wchar_t* title,
                       const std::wstring& inKey,
                       const std::wstring& inCmdRaw,
                       const std::wstring& inMenu,
                       int editingLineIndex,
                       std::wstring& outKey,
                       std::wstring& outCmdRaw,
                       std::wstring& outMenu) {
    static bool classReady = false;
    if (!classReady) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = ModalEditProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"KEYUI_MODAL_EDIT";
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        RegisterClassW(&wc);
        classReady = true;
    }

    ModalEdit st;
    st.editingLineIndex = editingLineIndex;
    st.originalKeyLower = ws_lower(ws_trim(inKey));

    const int W = 760, H = 250;
    RECT pr{}; GetWindowRect(parent, &pr);
    int x = pr.left + ((pr.right - pr.left) - W) / 2;
    int y = pr.top + ((pr.bottom - pr.top) - H) / 2;

    HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE,
        L"KEYUI_MODAL_EDIT", title,
        WS_CAPTION | WS_POPUP | WS_SYSMENU,
        x, y, W, H,
        parent, nullptr, GetModuleHandleW(nullptr), &st);
    st.hwnd = hwnd;

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    st.edtKey = GetDlgItem(hwnd, 10);
    st.edtCmd = GetDlgItem(hwnd, 12);
    st.edtMenu = GetDlgItem(hwnd, 13);
    if (st.edtKey) SetWindowTextW(st.edtKey, inKey.c_str());
    if (st.edtCmd) SetWindowTextW(st.edtCmd, inCmdRaw.c_str());
    if (st.edtMenu) SetWindowTextW(st.edtMenu, inMenu.c_str());

    EnableWindow(parent, FALSE);

    MSG msg;
    while (IsWindow(hwnd) && GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
        if (st.done) break;
    }

    EnableWindow(parent, TRUE);
    SetActiveWindow(parent);

    if (st.ok) {
        outKey = st.key;
        outCmdRaw = st.cmd;
        outMenu = st.menu;
        return true;
    }
    return false;
}

// ---------------- UI creation / layout ----------------
static void create_ui(HWND hwnd) {
    if (!gFont) {
        LOGFONTW lf{};
        lf.lfHeight = -14;
        wcscpy_s(lf.lfFaceName, L"Segoe UI");
        gFont = CreateFontIndirectW(&lf);
    }

    HWND lbl = CreateWindowExW(0, L"STATIC", L"Search:",
                              WS_CHILD | WS_VISIBLE,
                              12, 12, 52, 20, hwnd, (HMENU)(INT_PTR)IDC_SEARCH_LABEL, GetModuleHandleW(nullptr), 0);

    gSearchEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                 WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                 70, 9, 520, 24, hwnd, (HMENU)(INT_PTR)IDC_SEARCH_EDIT, GetModuleHandleW(nullptr), 0);

    HWND clearBtn = CreateWindowExW(0, L"BUTTON", L"Clear",
                                   WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                   600, 9, 70, 24, hwnd, (HMENU)(INT_PTR)IDC_SEARCH_CLEAR, GetModuleHandleW(nullptr), 0);

    gList = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                           WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SHOWSELALWAYS | LVS_SINGLESEL,
                           12, 44, 760, 420, hwnd, (HMENU)(INT_PTR)IDC_LIST, GetModuleHandleW(nullptr), 0);

    ListView_SetExtendedListViewStyle(gList,
        LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_GRIDLINES | LVS_EX_INFOTIP);

    list_insert_columns();

    CreateWindowExW(0, L"BUTTON", L"Add", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                    12, 0, 80, 28, hwnd, (HMENU)(INT_PTR)IDC_BTN_ADD, GetModuleHandleW(nullptr), 0);
    CreateWindowExW(0, L"BUTTON", L"Edit", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                    100, 0, 80, 28, hwnd, (HMENU)(INT_PTR)IDC_BTN_EDIT, GetModuleHandleW(nullptr), 0);
    CreateWindowExW(0, L"BUTTON", L"Remove", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                    188, 0, 90, 28, hwnd, (HMENU)(INT_PTR)IDC_BTN_REMOVE, GetModuleHandleW(nullptr), 0);
    CreateWindowExW(0, L"BUTTON", L"Reload", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                    286, 0, 90, 28, hwnd, (HMENU)(INT_PTR)IDC_BTN_RELOAD, GetModuleHandleW(nullptr), 0);
    CreateWindowExW(0, L"BUTTON", L"Save", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                    384, 0, 90, 28, hwnd, (HMENU)(INT_PTR)IDC_BTN_SAVE, GetModuleHandleW(nullptr), 0);
    CreateWindowExW(0, L"BUTTON", L"Close", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                    0, 0, 90, 28, hwnd, (HMENU)(INT_PTR)IDC_BTN_CLOSE, GetModuleHandleW(nullptr), 0);

    gStatus = CreateWindowExW(0, STATUSCLASSNAMEW, nullptr,
                             WS_CHILD | WS_VISIBLE,
                             0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_STATUS, GetModuleHandleW(nullptr), 0);

    for (HWND h : { lbl, gSearchEdit, clearBtn, gList,
                    GetDlgItem(hwnd, IDC_BTN_ADD), GetDlgItem(hwnd, IDC_BTN_EDIT),
                    GetDlgItem(hwnd, IDC_BTN_REMOVE), GetDlgItem(hwnd, IDC_BTN_RELOAD),
                    GetDlgItem(hwnd, IDC_BTN_SAVE), GetDlgItem(hwnd, IDC_BTN_CLOSE),
                    gStatus }) {
        SendMessageW(h, WM_SETFONT, (WPARAM)gFont, TRUE);
    }

    RECT rc; GetClientRect(hwnd, &rc);
    SendMessageW(hwnd, WM_SIZE, 0, MAKELPARAM(rc.right - rc.left, rc.bottom - rc.top));
}

static void relayout(HWND hwnd, int w, int h) {
    const int pad = 12;
    const int topY = 10;
    const int topH = 24;
    const int rowGap = 10;
    const int btnH = 28;
    const int btnGap = 8;

    RECT sr{};
    SendMessageW(gStatus, WM_SIZE, 0, 0);
    GetWindowRect(gStatus, &sr);
    int statusH = sr.bottom - sr.top;
    if (statusH <= 0) statusH = 22;

    int searchY = topY;
    int listY = topY + topH + rowGap + 1;
    int btnY = h - statusH - pad - btnH;
    int listH = btnY - listY - rowGap;

    int clearW = 70;
    int lblW = 52;
    int searchEditX = pad + lblW + 6;
    int searchEditW = (std::max)(200, w - (searchEditX + clearW + pad + 8));
    int clearX = searchEditX + searchEditW + 8;

    int listX = pad;
    int listW = (std::max)(200, w - 2 * pad);

    int closeW = 90;
    int closeX = w - pad - closeW;

    HDWP dwp = BeginDeferWindowPos(10);

    HWND lbl = GetDlgItem(hwnd, IDC_SEARCH_LABEL);
    HWND clearBtn = GetDlgItem(hwnd, IDC_SEARCH_CLEAR);
    HWND btnAdd = GetDlgItem(hwnd, IDC_BTN_ADD);
    HWND btnEdit = GetDlgItem(hwnd, IDC_BTN_EDIT);
    HWND btnRemove = GetDlgItem(hwnd, IDC_BTN_REMOVE);
    HWND btnReload = GetDlgItem(hwnd, IDC_BTN_RELOAD);
    HWND btnSave = GetDlgItem(hwnd, IDC_BTN_SAVE);
    HWND btnClose = GetDlgItem(hwnd, IDC_BTN_CLOSE);

    dwp = DeferWindowPos(dwp, lbl, nullptr, pad, searchY + 4, lblW, topH, SWP_NOZORDER);
    dwp = DeferWindowPos(dwp, gSearchEdit, nullptr, searchEditX, searchY, searchEditW, topH, SWP_NOZORDER);
    dwp = DeferWindowPos(dwp, clearBtn, nullptr, clearX, searchY, clearW, topH, SWP_NOZORDER);

    dwp = DeferWindowPos(dwp, gList, nullptr, listX, listY, listW, (std::max)(120, listH), SWP_NOZORDER);

    int x = pad;
    dwp = DeferWindowPos(dwp, btnAdd, nullptr, x, btnY, 80, btnH, SWP_NOZORDER); x += 80 + btnGap;
    dwp = DeferWindowPos(dwp, btnEdit, nullptr, x, btnY, 80, btnH, SWP_NOZORDER); x += 80 + btnGap;
    dwp = DeferWindowPos(dwp, btnRemove, nullptr, x, btnY, 90, btnH, SWP_NOZORDER); x += 90 + btnGap;
    dwp = DeferWindowPos(dwp, btnReload, nullptr, x, btnY, 90, btnH, SWP_NOZORDER); x += 90 + btnGap;
    dwp = DeferWindowPos(dwp, btnSave, nullptr, x, btnY, 90, btnH, SWP_NOZORDER); x += 90 + btnGap;

    dwp = DeferWindowPos(dwp, btnClose, nullptr, closeX, btnY, closeW, btnH, SWP_NOZORDER);

    dwp = DeferWindowPos(dwp, gStatus, nullptr, 0, h - statusH, w, statusH, SWP_NOZORDER);

    EndDeferWindowPos(dwp);

    if (!gInSizeMove) RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
}

// ---------------- actions ----------------
static void do_reload() {
    if (!load_input_conf()) {
        MessageBoxW(gHwnd, L"Failed to load input.conf.\n\ninput.conf の読み込みに失敗しました。", APP_TITLE_BASE, MB_ICONERROR);
        return;
    }
    list_populate();
    gDirty = false;
}

static void maybe_autosave(const wchar_t* actionLabel) {
    (void)actionLabel;
    if (!gAutoSave) {
        gDirty = true;
        status_set(L"Pending changes (not saved). Click Save to write input.conf.");
        return;
    }

    if (!save_input_conf()) {
			gDirty = true;
			std::wstring m = L"Auto-save failed after ";
			m += actionLabel;
			m += L".\n\n自動保存に失敗しました。\n\nPlease click Save or check write permissions:\nSave を押すか、書き込み権限を確認してください:\n";
			m += gInputPath;
        MessageBoxW(gHwnd, m.c_str(), APP_TITLE_BASE, MB_ICONERROR);
        return;
    }
    // Important: reload to refresh lineIndex for newly added bindings, so we don't append duplicates on next save.
    std::wstring curSearch;
    if (gSearchEdit) {
        wchar_t buf[1024]{};
        GetWindowTextW(gSearchEdit, buf, 1024);
        curSearch = buf;
    }
    do_reload();
    if (gSearchEdit) {
        SetWindowTextW(gSearchEdit, curSearch.c_str());
        gSearch = curSearch;
        list_populate();
    }
    status_set(L"Auto-saved.");
}

static bool save_silent_on_close() {
    if (!gDirty) return true;
    if (save_input_conf()) {
        gDirty = false;
        return true;
    }
	std::wstring m = L"Failed to save input.conf on exit.\n\n終了時の保存に失敗しました。\n\n";
	m += gInputPath;
    MessageBoxW(gHwnd, m.c_str(), APP_TITLE_BASE, MB_ICONERROR);
    return false;
}


static void do_save() {
    if (!save_input_conf()) {
        MessageBoxW(gHwnd, L"Failed to save input.conf.\n\ninput.conf の保存に失敗しました。", APP_TITLE_BASE, MB_ICONERROR);
        return;
    }
    MessageBoxW(gHwnd, L"Saved input.conf (format preserved)\n\n保存しました（書式は保持されています）", APP_TITLE_BASE, MB_OK | MB_ICONINFORMATION);
    gDirty = false;
    do_reload();
}

static void do_add() {
    std::wstring k, cRaw, m;
    if (modal_edit(gHwnd, L"Add Binding", L"", L"", L"", -1, k, cRaw, m)) {
        Binding b;
        b.prefix = L"";
        b.sep = L" ";
        b.key = k;

        b.cmdRaw = cRaw;
        b.commentRaw = L"";
        if (!ws_trim(m).empty()) b.commentRaw = L"#menu: " + ws_trim(m);

        b.cmdDisplay = ws_trim(b.cmdRaw);
        b.menuDisplay = extract_menu_value(b.commentRaw);
        b.lineIndex = -1;

        gBindings.push_back(std::move(b));
        list_populate();
        maybe_autosave(L"do_add()");
    }
}

static void do_edit() {
    int bi = list_get_selected_binding_index();
    if (bi < 0 || bi >= (int)gBindings.size()) return;

    auto& b = gBindings[bi];

    std::wstring k, cRaw, m;
    if (modal_edit(gHwnd, L"Edit Binding", b.key, b.cmdRaw, b.menuDisplay, b.lineIndex, k, cRaw, m)) {
        b.key = k;

        b.cmdRaw = cRaw;
        b.cmdDisplay = ws_trim(b.cmdRaw);

        std::wstring newMenuTrim = ws_trim(m);
        b.commentRaw = apply_menu_to_comment(b.commentRaw, newMenuTrim);
        b.menuDisplay = extract_menu_value(b.commentRaw);

        list_populate();
        list_select_binding_index(bi);
        maybe_autosave(L"do_edit()");
    }
}

static void do_remove() {
    int bi = list_get_selected_binding_index();
    if (bi < 0 || bi >= (int)gBindings.size()) return;

    auto& b = gBindings[bi];
    std::wstringstream ss;
	ss << L"Remove this binding? / この割り当てを削除しますか？\n\n" << b.key << L"  " << b.cmdDisplay;
    if (MessageBoxW(gHwnd, ss.str().c_str(), APP_TITLE_BASE, MB_YESNO | MB_ICONQUESTION) == IDYES) {
        b.deleted = true;
        list_populate();
        maybe_autosave(L"do_remove()");
    }
}

static void do_search_changed() {
    wchar_t buf[1024]{};
    GetWindowTextW(gSearchEdit, buf, 1024);
    gSearch = buf;
    list_populate();
}

// ---------------- main wnd proc ----------------
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        gHwnd = hwnd;
        create_ui(hwnd);
        do_reload();
        return 0;

    case WM_ERASEBKGND: {
        RECT rc; GetClientRect(hwnd, &rc);
        FillRect((HDC)wParam, &rc, (HBRUSH)(COLOR_WINDOW + 1));
        return 1;
    }

    case WM_ENTERSIZEMOVE:
        gInSizeMove = true;
        return 0;

    case WM_EXITSIZEMOVE:
        gInSizeMove = false;
        RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
        return 0;

    case WM_SIZE:
        if (gList && gStatus) relayout(hwnd, LOWORD(lParam), HIWORD(lParam));
        return 0;

    case WM_GETMINMAXINFO: {
        MINMAXINFO* mmi = (MINMAXINFO*)lParam;
        // Minimum window size (track size)
        mmi->ptMinTrackSize.x = 600;
        mmi->ptMinTrackSize.y = 400;
        return 0;
    }

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_SEARCH_EDIT:
            if (HIWORD(wParam) == EN_CHANGE) do_search_changed();
            break;
        case IDC_SEARCH_CLEAR:
            SetWindowTextW(gSearchEdit, L"");
            do_search_changed();
            break;
        case IDC_BTN_ADD: do_add(); break;
        case IDC_BTN_EDIT: do_edit(); break;
        case IDC_BTN_REMOVE: do_remove(); break;
        case IDC_BTN_RELOAD: do_reload(); break;
        case IDC_BTN_SAVE: do_save(); break;
        case IDC_BTN_CLOSE: PostMessageW(hwnd, WM_CLOSE, 0, 0); break;
        }
        return 0;

    case WM_CLOSE:
        if (gDirty) {
            int r = MessageBoxW(hwnd,
                L"Exit without saving changes?\n変更を保存しないで終了しますか？",
                L"Confirm / 確認",
                MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
            if (r == IDNO) return 0; // cancel close
        }
        DestroyWindow(hwnd);
        return 0;
    case WM_NOTIFY: {
        LPNMHDR hdr = (LPNMHDR)lParam;
        if (hdr->idFrom == IDC_LIST && hdr->code == NM_DBLCLK) {
            do_edit();
            return 0;
        }
        return 0;
    }

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int nCmdShow) {
    gExeDir = get_exe_dir();
    gInputPath = join_path(gExeDir, L"portable_config\\input.conf");

    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    if (!gFont) {
        LOGFONTW lf{};
        lf.lfHeight = -14;
        wcscpy_s(lf.lfFaceName, L"Segoe UI");
        gFont = CreateFontIndirectW(&lf);
    }

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"KEYUI_MAIN";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClassW(&wc);

    DWORD style = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN;
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, APP_TITLE_BASE, style,
                                CW_USEDEFAULT, CW_USEDEFAULT, 980, 640,
                                nullptr, nullptr, hInst, nullptr);
    if (!hwnd) return 0;

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (gFont) { DeleteObject(gFont); gFont = nullptr; }
    return (int)msg.wParam;
}
