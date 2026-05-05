// mpv_yt_comments.cpp
// YouTube comments popup helper for mpv + yt-dlp on Windows.
// Debug logging is disabled by default.
// Playlist/Mix URLs are normalized to single-video watch URLs for comment fetching.
// Build:
//   cl /nologo /EHsc /std:c++17 /utf-8 mpv_yt_comments.cpp /Fe:mpv_yt_comments.exe ^
//      user32.lib advapi32.lib shell32.lib /link /SUBSYSTEM:WINDOWS

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cmath>
#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr wchar_t kMutexName[] = L"Local\\mpv_yt_comments_single_instance_v1";
constexpr wchar_t kToggleEventName[] = L"Local\\mpv_yt_comments_toggle_event_v1";
constexpr int kOverlayId = 52;
constexpr int kAssResX = 1280;
constexpr int kAssResY = 720;

struct Options {
    std::wstring pipe_name;          // empty = auto detect
    std::wstring yt_dlp_path;        // empty = auto detect
    std::string  url_override;       // manual URL override for troubleshooting
    int interval_ms = 10000;
    int duration_ms = 4500;      // minimum display time; actual time is auto-extended by text length
    int max_duration_ms = 16000; // upper bound for auto display time
    int max_comments = 80;       // number of display candidates to keep
    int max_chars = 160;         // Unicode codepoint limit; longer comments are skipped, not truncated
    bool debug = false;
    bool enable_shake = true;  // playful comments get a light ASS transform shake
    bool language_priority = true;
    std::string preferred_lang = "auto"; // auto / ja / en / ko / zh / all
    std::string resolved_lang;           // filled from Windows user locale when auto
    bool rhythm_mode = true;             // stagger comments like musical phrases instead of one-by-one
    int max_concurrent = 0;              // 0 = auto by comment volume; otherwise hard limit
    bool duration_pacing = true;           // spread comments according to video duration and candidate count
    bool time_comment_sync = true;          // show comments containing 1:23 / 01:23 before that playback time
    int time_comment_lead_sec = 4;          // preferred seconds before the referenced playback time
};

struct Comment {
    std::string text;
    std::string author;
    std::string id;
    std::string parent;
    int likes = 0;
    long long timestamp = 0;      // Unix time if provided by yt-dlp
    int reply_count = 0;          // number of replies if yt-dlp exposes it; otherwise detected fetched replies
    bool threaded = false;
    bool from_new_sort = false;   // obtained from the new-comments pass
    int tier = 0;                 // 0 normal, 1 fresh/good, 2 good, 3 hot, 4 very hot
    double score = 0.0;
    std::string lang;             // rough text language/script profile: ja/en/ko/zh/mixed/other
    int video_time_sec = -2;       // -2 = not scanned yet, -1 = no timestamp cue, >=0 = referenced playback time
};

std::atomic<bool> g_enabled{true};
std::atomic<bool> g_quit{false};
std::atomic<bool> g_debug_enabled{false};
std::mutex g_yt_dlp_mutex;


std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return L"";
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return "";
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring GetExeDir() {
    wchar_t path[MAX_PATH]{};
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring p(path, path + n);
    size_t pos = p.find_last_of(L"\\/");
    if (pos == std::wstring::npos) return L".";
    return p.substr(0, pos);
}

std::string NowText() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    char buf[64]{};
    std::snprintf(buf, sizeof(buf), "%04u-%02u-%02u %02u:%02u:%02u.%03u",
                  st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    return std::string(buf);
}

std::string OneLineForLog(std::string s, size_t limit = 1600) {
    for (char& c : s) {
        if (c == '\r' || c == '\n' || c == '\t') c = ' ';
    }
    if (s.size() > limit) s = s.substr(0, limit) + " ...[truncated]";
    return s;
}

void DebugLog(const std::string& msg) {
    if (!g_debug_enabled.load()) return;
    std::wstring path = GetExeDir() + L"\\mpv_yt_comments_debug.txt";
    std::string line = "[" + NowText() + "] " + msg + "\r\n";
    HANDLE f = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(f, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    CloseHandle(f);
}

void DebugLogW(const std::wstring& msg) {
    DebugLog(WideToUtf8(msg));
}

void DebugLogEvery(const char* key, const std::string& msg, DWORD interval_ms = 5000) {
    if (!g_debug_enabled.load()) return;
    static std::mutex s_mutex;
    static std::map<std::string, DWORD> s_next_tick;
    const DWORD now = GetTickCount();
    std::lock_guard<std::mutex> lock(s_mutex);
    const std::string k = key ? key : "";
    auto it = s_next_tick.find(k);
    if (it != s_next_tick.end() && static_cast<LONG>(now - it->second) < 0) return;
    s_next_tick[k] = now + interval_ms;
    DebugLog(msg);
}


bool FileExistsW(const std::wstring& path) {
    DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring QuoteArg(const std::wstring& s) {
    std::wstring out = L"\"";
    int backslashes = 0;
    for (wchar_t ch : s) {
        if (ch == L'\\') {
            backslashes++;
        } else if (ch == L'\"') {
            out.append(backslashes * 2 + 1, L'\\');
            out.push_back(ch);
            backslashes = 0;
        } else {
            out.append(backslashes, L'\\');
            backslashes = 0;
            out.push_back(ch);
        }
    }
    out.append(backslashes * 2, L'\\');
    out.push_back(L'\"');
    return out;
}

std::string JsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 16);
    for (unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out.push_back(static_cast<char>(c));
            }
        }
    }
    return out;
}

void AppendUtf8(std::string& out, uint32_t cp) {
    if (cp <= 0x7F) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

int HexVal(char c) {
    if ('0' <= c && c <= '9') return c - '0';
    if ('a' <= c && c <= 'f') return c - 'a' + 10;
    if ('A' <= c && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool ParseHex4(const std::string& s, size_t pos, uint32_t& cp) {
    if (pos + 4 > s.size()) return false;
    cp = 0;
    for (int i = 0; i < 4; ++i) {
        int v = HexVal(s[pos + i]);
        if (v < 0) return false;
        cp = (cp << 4) | static_cast<uint32_t>(v);
    }
    return true;
}

bool ParseJsonStringAt(const std::string& s, size_t quote_pos, std::string& out, size_t* end_pos = nullptr) {
    if (quote_pos >= s.size() || s[quote_pos] != '"') return false;
    out.clear();
    for (size_t i = quote_pos + 1; i < s.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == '"') {
            if (end_pos) *end_pos = i + 1;
            return true;
        }
        if (c != '\\') {
            out.push_back(static_cast<char>(c));
            continue;
        }
        if (++i >= s.size()) return false;
        char e = s[i];
        switch (e) {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'u': {
            uint32_t cp = 0;
            if (!ParseHex4(s, i + 1, cp)) return false;
            i += 4;
            if (0xD800 <= cp && cp <= 0xDBFF) {
                if (i + 6 < s.size() && s[i + 1] == '\\' && s[i + 2] == 'u') {
                    uint32_t low = 0;
                    if (ParseHex4(s, i + 3, low) && 0xDC00 <= low && low <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                        i += 6;
                    }
                }
            }
            AppendUtf8(out, cp);
            break;
        }
        default:
            out.push_back(e);
            break;
        }
    }
    return false;
}

size_t SkipWs(const std::string& s, size_t pos) {
    while (pos < s.size() && std::isspace(static_cast<unsigned char>(s[pos]))) ++pos;
    return pos;
}

bool JsonGetString(const std::string& obj, const std::string& key, std::string& value) {
    std::string needle = "\"" + key + "\"";
    size_t pos = 0;
    while ((pos = obj.find(needle, pos)) != std::string::npos) {
        size_t colon = obj.find(':', pos + needle.size());
        if (colon == std::string::npos) return false;
        size_t vpos = SkipWs(obj, colon + 1);
        if (vpos < obj.size() && obj[vpos] == '"') {
            return ParseJsonStringAt(obj, vpos, value, nullptr);
        }
        pos = colon + 1;
    }
    return false;
}

bool JsonGetInt(const std::string& obj, const std::string& key, int& value) {
    std::string needle = "\"" + key + "\"";
    size_t pos = obj.find(needle);
    if (pos == std::string::npos) return false;
    size_t colon = obj.find(':', pos + needle.size());
    if (colon == std::string::npos) return false;
    size_t vpos = SkipWs(obj, colon + 1);
    if (vpos >= obj.size()) return false;
    if (obj.compare(vpos, 4, "null") == 0) { value = 0; return true; }
    bool neg = false;
    if (obj[vpos] == '-') { neg = true; ++vpos; }
    long long n = 0;
    bool any = false;
    while (vpos < obj.size() && std::isdigit(static_cast<unsigned char>(obj[vpos]))) {
        any = true;
        n = n * 10 + (obj[vpos] - '0');
        if (n > 2147483647LL) { n = 2147483647LL; break; }
        ++vpos;
    }
    if (!any) return false;
    value = static_cast<int>(neg ? -n : n);
    return true;
}

bool JsonGetInt64(const std::string& obj, const std::string& key, long long& value) {
    std::string needle = "\"" + key + "\"";
    size_t pos = obj.find(needle);
    if (pos == std::string::npos) return false;
    size_t colon = obj.find(':', pos + needle.size());
    if (colon == std::string::npos) return false;
    size_t vpos = SkipWs(obj, colon + 1);
    if (vpos >= obj.size()) return false;
    if (obj.compare(vpos, 4, "null") == 0) { value = 0; return true; }
    bool neg = false;
    if (obj[vpos] == '-') { neg = true; ++vpos; }
    long long n = 0;
    bool any = false;
    while (vpos < obj.size() && std::isdigit(static_cast<unsigned char>(obj[vpos]))) {
        any = true;
        int d = obj[vpos] - '0';
        if (n <= (9223372036854775807LL - d) / 10) n = n * 10 + d;
        ++vpos;
    }
    if (!any) return false;
    value = neg ? -n : n;
    return true;
}

bool JsonGetDouble(const std::string& obj, const std::string& key, double& value) {
    std::string needle = "\"" + key + "\"";
    size_t pos = obj.find(needle);
    if (pos == std::string::npos) return false;
    size_t colon = obj.find(':', pos + needle.size());
    if (colon == std::string::npos) return false;
    size_t vpos = SkipWs(obj, colon + 1);
    if (vpos >= obj.size()) return false;
    if (obj.compare(vpos, 4, "null") == 0) { value = 0.0; return true; }

    char* endp = nullptr;
    value = std::strtod(obj.c_str() + vpos, &endp);
    return endp && endp != obj.c_str() + vpos;
}

bool JsonGetBool(const std::string& obj, const std::string& key, bool& value) {
    std::string needle = "\"" + key + "\"";
    size_t pos = obj.find(needle);
    if (pos == std::string::npos) return false;
    size_t colon = obj.find(':', pos + needle.size());
    if (colon == std::string::npos) return false;
    size_t vpos = SkipWs(obj, colon + 1);
    if (vpos >= obj.size()) return false;
    if (obj.compare(vpos, 4, "true") == 0) { value = true; return true; }
    if (obj.compare(vpos, 5, "false") == 0) { value = false; return true; }
    if (obj.compare(vpos, 4, "null") == 0) { value = false; return true; }
    return false;
}

bool IsAsciiDigitChar(unsigned char c) {
    return c >= '0' && c <= '9';
}

bool IsTimestampLeftBoundary(const std::string& s, size_t pos) {
    if (pos == 0) return true;
    unsigned char prev = static_cast<unsigned char>(s[pos - 1]);
    return !IsAsciiDigitChar(prev) && prev != ':' && prev != '.';
}

bool IsTimestampRightBoundary(const std::string& s, size_t pos) {
    if (pos >= s.size()) return true;
    unsigned char next = static_cast<unsigned char>(s[pos]);
    return !IsAsciiDigitChar(next) && next != ':' && next != '.';
}

bool ReadAsciiNumberRun(const std::string& s, size_t& p, int min_digits, int max_digits, int& value, int& digits) {
    value = 0;
    digits = 0;
    size_t q = p;
    while (q < s.size() && digits < max_digits && IsAsciiDigitChar(static_cast<unsigned char>(s[q]))) {
        value = value * 10 + (s[q] - '0');
        ++q;
        ++digits;
    }
    if (digits < min_digits) return false;
    p = q;
    return true;
}

bool TryParseTimestampAt(const std::string& s, size_t pos, int& seconds_out, size_t& end_pos) {
    if (pos >= s.size() || !IsTimestampLeftBoundary(s, pos)) return false;
    size_t p = pos;
    int a = 0, b = 0, c = 0;
    int da = 0, db = 0, dc = 0;
    if (!ReadAsciiNumberRun(s, p, 1, 3, a, da)) return false;
    if (p >= s.size() || s[p] != ':') return false;
    ++p;
    if (!ReadAsciiNumberRun(s, p, 2, 2, b, db)) return false;
    bool hms = false;
    if (p < s.size() && s[p] == ':') {
        hms = true;
        ++p;
        if (!ReadAsciiNumberRun(s, p, 2, 2, c, dc)) return false;
    }
    if (!IsTimestampRightBoundary(s, p)) return false;
    int total = -1;
    if (hms) {
        if (b >= 60 || c >= 60) return false;
        total = a * 3600 + b * 60 + c;
    } else {
        if (b >= 60) return false;
        total = a * 60 + b;
    }
    if (total <= 0) return false;
    seconds_out = total;
    end_pos = p;
    return true;
}

int ExtractVideoTimestampSeconds(const std::string& text, double duration_sec) {
    int first_valid = -1;
    for (size_t i = 0; i < text.size();) {
        unsigned char ch = static_cast<unsigned char>(text[i]);
        if (!IsAsciiDigitChar(ch)) { ++i; continue; }
        int sec = -1;
        size_t end = i;
        if (TryParseTimestampAt(text, i, sec, end)) {
            bool valid = true;
            if (duration_sec > 1.0) valid = sec <= static_cast<int>(duration_sec + 10.0);
            else valid = sec <= 6 * 3600;
            if (valid) { first_valid = sec; break; }
            i = std::max(end, i + 1);
            continue;
        }
        ++i;
    }
    return first_valid;
}

int EnsureCommentVideoTime(Comment& c, double duration_sec) {
    if (c.video_time_sec == -2 && duration_sec > 1.0) {
        c.video_time_sec = ExtractVideoTimestampSeconds(c.text, duration_sec);
    }
    return c.video_time_sec;
}

void ResetCommentVideoTimeScan(std::vector<Comment>& comments) {
    for (auto& c : comments) c.video_time_sec = -2;
}


std::string TrimSpaces(const std::string& s) {
    size_t a = 0;
    while (a < s.size() && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    size_t b = s.size();
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

std::string CollapseWhitespace(const std::string& s) {
    std::string out;
    bool in_ws = false;
    for (unsigned char c : s) {
        if (c == '\r' || c == '\n' || c == '\t' || c == ' ') {
            if (!in_ws) out.push_back(' ');
            in_ws = true;
        } else {
            out.push_back(static_cast<char>(c));
            in_ws = false;
        }
    }
    return TrimSpaces(out);
}

std::string CommentDisplayKey(const Comment& c) {
    if (!c.id.empty()) return std::string("id:") + c.id;
    std::string t = CollapseWhitespace(c.text);
    std::string out;
    out.reserve(t.size());
    for (unsigned char ch : t) {
        if (ch >= 'A' && ch <= 'Z') out.push_back(static_cast<char>(ch - 'A' + 'a'));
        else out.push_back(static_cast<char>(ch));
    }
    return std::string("text:") + out;
}

std::string CommentRepeatKey(const Comment& c) {
    // For repeat prevention, text is more reliable than the YouTube comment id:
    // the same visible comment can arrive once from the quick seed pass and
    // again from the full/background extraction pass, with different ids.
    // Normalize the visible text so identical comments do not reappear in the
    // same playback session just because they came from a different lane.
    std::string t = CollapseWhitespace(c.text);
    t = TrimSpaces(t);
    while (!t.empty()) {
        if (t.rfind("↳", 0) == 0) {
            t.erase(0, std::string("↳").size());
            t = TrimSpaces(t);
            continue;
        }
        if (!t.empty() && (t[0] == '>' || t[0] == '-')) {
            t.erase(t.begin());
            t = TrimSpaces(t);
            continue;
        }
        break;
    }
    std::string out;
    out.reserve(t.size());
    for (unsigned char ch : t) {
        if (ch >= 'A' && ch <= 'Z') out.push_back(static_cast<char>(ch - 'A' + 'a'));
        else out.push_back(static_cast<char>(ch));
    }
    if (out.size() > 512) out.resize(512);
    if (!out.empty()) return std::string("text:") + out;
    return CommentDisplayKey(c);
}

size_t Utf8CharLenAt(const std::string& s, size_t pos) {
    if (pos >= s.size()) return 0;
    unsigned char c = static_cast<unsigned char>(s[pos]);
    size_t len = 1;
    if ((c & 0x80) == 0) len = 1;
    else if ((c & 0xE0) == 0xC0) len = 2;
    else if ((c & 0xF0) == 0xE0) len = 3;
    else if ((c & 0xF8) == 0xF0) len = 4;
    if (pos + len > s.size()) len = 1;
    return len;
}

size_t Utf8CharCount(const std::string& s) {
    size_t count = 0;
    for (size_t i = 0; i < s.size();) {
        size_t len = Utf8CharLenAt(s, i);
        if (len == 0) break;
        i += len;
        ++count;
    }
    return count;
}

int Utf8DisplayWidthAt(const std::string& s, size_t pos, size_t len) {
    if (len == 1) {
        unsigned char c = static_cast<unsigned char>(s[pos]);
        if (c == '\n') return 0;
        if (c == '\t') return 1;
        return 1;
    }
    // Japanese/CJK/emoji are normally wider than ASCII on OSD.
    return 2;
}

int Utf8DisplayWidth(const std::string& s) {
    int width = 0;
    for (size_t i = 0; i < s.size();) {
        size_t len = Utf8CharLenAt(s, i);
        if (len == 0) break;
        width += Utf8DisplayWidthAt(s, i, len);
        i += len;
    }
    return width;
}

bool IsWithinCharLimit(const std::string& s, int max_chars) {
    return Utf8CharCount(s) <= static_cast<size_t>(std::max(1, max_chars));
}


struct Utf8TextUnit {
    std::string s;
    int cols = 0;
};

std::vector<Utf8TextUnit> SplitUtf8TextUnits(const std::string& s) {
    std::vector<Utf8TextUnit> units;
    for (size_t i = 0; i < s.size();) {
        size_t len = Utf8CharLenAt(s, i);
        if (len == 0) break;
        Utf8TextUnit u;
        u.s.assign(s, i, len);
        u.cols = Utf8DisplayWidthAt(s, i, len);
        units.push_back(u);
        i += len;
    }
    return units;
}

std::string JoinUtf8Units(const std::vector<Utf8TextUnit>& units, size_t first, size_t last) {
    std::string out;
    for (size_t i = first; i < last && i < units.size(); ++i) out += units[i].s;
    return out;
}

std::string JoinUtf8UnitsAll(const std::vector<Utf8TextUnit>& units) {
    return JoinUtf8Units(units, 0, units.size());
}

int Utf8UnitsWidth(const std::vector<Utf8TextUnit>& units, size_t first, size_t last) {
    int w = 0;
    for (size_t i = first; i < last && i < units.size(); ++i) w += units[i].cols;
    return w;
}

bool IsUtf8UnitInSet(const Utf8TextUnit& u, const std::set<std::string>& table) {
    return table.find(u.s) != table.end();
}

bool IsLineStartProhibitedUnit(const Utf8TextUnit& u) {
    static const std::set<std::string> k = {
        "、", "。", "，", "．", ",", ".", "・", "：", "；", ":", ";",
        "！", "？", "!", "?", "‼", "⁉",
        "）", ")", "］", "]", "｝", "}", "〉", "》", "」", "』", "】", "〕", "〗", "〙",
        "’", "”", "»", "〟", "…", "‥",
        "ぁ", "ぃ", "ぅ", "ぇ", "ぉ", "っ", "ゃ", "ゅ", "ょ", "ゎ",
        "ァ", "ィ", "ゥ", "ェ", "ォ", "ッ", "ャ", "ュ", "ョ", "ヮ", "ヶ",
        "ー", "〜", "～", "々", "ゝ", "ゞ", "ヽ", "ヾ"
    };
    return IsUtf8UnitInSet(u, k);
}

bool IsLineEndProhibitedUnit(const Utf8TextUnit& u) {
    static const std::set<std::string> k = {
        "（", "(", "［", "[", "｛", "{", "〈", "《", "「", "『", "【", "〔", "〖", "〘",
        "‘", "“", "«", "〝"
    };
    return IsUtf8UnitInSet(u, k);
}

bool IsNaturalBreakAfterUnit(const Utf8TextUnit& u) {
    static const std::set<std::string> k = {
        "、", "。", "，", "．", ",", ".", "！", "？", "!", "?", "‼", "⁉",
        "」", "』", "）", ")", "］", "]", "】", "…", "‥", " ", "　"
    };
    return IsUtf8UnitInSet(u, k);
}

void TrimUnitVector(std::vector<Utf8TextUnit>& v) {
    while (!v.empty() && (v.front().s == " " || v.front().s == "　" || v.front().s == "\t")) v.erase(v.begin());
    while (!v.empty() && (v.back().s == " " || v.back().s == "　" || v.back().s == "\t")) v.pop_back();
}

int UnitVectorWidth(const std::vector<Utf8TextUnit>& v) {
    int w = 0;
    for (const auto& u : v) w += u.cols;
    return w;
}

void FixKinsokuBetweenLines(std::vector<std::vector<Utf8TextUnit>>& lines, int max_cols) {
    for (size_t pass = 0; pass < 3; ++pass) {
        for (size_t i = 0; i + 1 < lines.size(); ++i) {
            TrimUnitVector(lines[i]);
            TrimUnitVector(lines[i + 1]);
            if (lines[i].empty() || lines[i + 1].empty()) continue;

            int guard = 0;
            while (!lines[i + 1].empty() && IsLineStartProhibitedUnit(lines[i + 1].front()) && guard++ < 3) {
                int prev_w = UnitVectorWidth(lines[i]);
                if (prev_w <= max_cols + 6 || lines[i].size() <= 2) {
                    lines[i].push_back(lines[i + 1].front());
                    lines[i + 1].erase(lines[i + 1].begin());
                } else {
                    lines[i + 1].insert(lines[i + 1].begin(), lines[i].back());
                    lines[i].pop_back();
                    break;
                }
            }

            guard = 0;
            while (!lines[i].empty() && IsLineEndProhibitedUnit(lines[i].back()) && guard++ < 3) {
                lines[i + 1].insert(lines[i + 1].begin(), lines[i].back());
                lines[i].pop_back();
            }
        }
    }
}

void FixJapaneseOrphanLines(std::vector<std::vector<Utf8TextUnit>>& lines, int max_cols) {
    if (lines.size() < 2) return;
    const int min_tail_cols = std::min(8, std::max(4, max_cols / 5));

    for (size_t i = 1; i < lines.size(); ++i) {
        TrimUnitVector(lines[i - 1]);
        TrimUnitVector(lines[i]);
        if (lines[i - 1].empty() || lines[i].empty()) continue;

        int tail_w = UnitVectorWidth(lines[i]);
        int prev_w = UnitVectorWidth(lines[i - 1]);
        while (tail_w < min_tail_cols && prev_w > std::max(8, max_cols / 2) && lines[i - 1].size() > 2) {
            Utf8TextUnit moved = lines[i - 1].back();
            lines[i - 1].pop_back();
            lines[i].insert(lines[i].begin(), moved);
            tail_w += moved.cols;
            prev_w -= moved.cols;
            if (lines[i].size() >= 4) break;
        }
    }
}

std::vector<std::string> SplitIntoDisplayLines(const std::string& s, int max_cols, int max_lines, bool* overflow = nullptr) {
    if (overflow) *overflow = false;
    std::vector<std::vector<Utf8TextUnit>> unit_lines;

    std::vector<Utf8TextUnit> paragraph;
    auto flush_paragraph = [&]() {
        TrimUnitVector(paragraph);
        if (paragraph.empty()) return;

        size_t pos = 0;
        while (pos < paragraph.size()) {
            size_t start = pos;
            int cols = 0;
            size_t end = start;
            size_t natural_break = std::string::npos;
            int natural_cols = 0;

            while (end < paragraph.size()) {
                int w = paragraph[end].cols;
                if (cols + w > max_cols && end > start) break;
                cols += w;
                if (IsNaturalBreakAfterUnit(paragraph[end]) && cols >= std::max(8, max_cols / 3)) {
                    natural_break = end + 1;
                    natural_cols = cols;
                }
                ++end;
            }

            if (end >= paragraph.size()) {
                std::vector<Utf8TextUnit> line(paragraph.begin() + start, paragraph.end());
                TrimUnitVector(line);
                if (!line.empty()) unit_lines.push_back(line);
                break;
            }

            size_t break_pos = end;
            if (natural_break != std::string::npos && natural_break > start + 1 && natural_cols >= max_cols * 62 / 100) {
                break_pos = natural_break;
            }

            int guard = 0;
            while (break_pos < paragraph.size() && break_pos > start + 1 && IsLineStartProhibitedUnit(paragraph[break_pos]) && guard++ < 3) {
                ++break_pos;
            }

            guard = 0;
            while (break_pos > start + 1 && IsLineEndProhibitedUnit(paragraph[break_pos - 1]) && guard++ < 3) {
                --break_pos;
            }

            int remain_w = Utf8UnitsWidth(paragraph, break_pos, paragraph.size());
            const int min_tail_cols = std::min(8, std::max(4, max_cols / 5));
            while (remain_w > 0 && remain_w < min_tail_cols && break_pos > start + 3) {
                --break_pos;
                remain_w += paragraph[break_pos].cols;
            }

            if (break_pos <= start) break_pos = std::min(start + 1, paragraph.size());
            std::vector<Utf8TextUnit> line(paragraph.begin() + start, paragraph.begin() + break_pos);
            TrimUnitVector(line);
            if (!line.empty()) unit_lines.push_back(line);
            pos = break_pos;

            if (static_cast<int>(unit_lines.size()) > max_lines) {
                if (overflow) *overflow = true;
                break;
            }
        }
        paragraph.clear();
    };

    for (size_t i = 0; i < s.size();) {
        size_t len = Utf8CharLenAt(s, i);
        if (len == 0) break;
        if (s[i] == '\r') { i += len; continue; }
        if (s[i] == '\n') {
            flush_paragraph();
            i += len;
            continue;
        }
        Utf8TextUnit u;
        u.s.assign(s, i, len);
        u.cols = Utf8DisplayWidthAt(s, i, len);
        paragraph.push_back(u);
        i += len;
    }
    flush_paragraph();

    FixKinsokuBetweenLines(unit_lines, max_cols);
    FixJapaneseOrphanLines(unit_lines, max_cols);
    FixKinsokuBetweenLines(unit_lines, max_cols);

    std::vector<std::string> lines;
    for (auto& ul : unit_lines) {
        TrimUnitVector(ul);
        std::string line = TrimSpaces(JoinUtf8UnitsAll(ul));
        if (!line.empty()) lines.push_back(line);
    }

    if (static_cast<int>(lines.size()) > max_lines) {
        if (overflow) *overflow = true;
        lines.resize(max_lines);
    }
    return lines;
}

std::vector<std::string> SplitIntoUtf8Chunks(const std::string& s, size_t max_bytes_per_line, int max_lines) {
    std::vector<std::string> lines;
    std::string cur;
    size_t last_space = std::string::npos;

    auto flush = [&]() {
        if (!cur.empty()) {
            lines.push_back(TrimSpaces(cur));
            cur.clear();
            last_space = std::string::npos;
        }
    };

    for (size_t i = 0; i < s.size();) {
        size_t char_len = 1;
        unsigned char c = static_cast<unsigned char>(s[i]);
        if ((c & 0x80) == 0) char_len = 1;
        else if ((c & 0xE0) == 0xC0) char_len = 2;
        else if ((c & 0xF0) == 0xE0) char_len = 3;
        else if ((c & 0xF8) == 0xF0) char_len = 4;
        if (i + char_len > s.size()) char_len = 1;

        if (cur.size() + char_len > max_bytes_per_line && !cur.empty()) {
            if (last_space != std::string::npos && last_space > 0 && last_space + 1 < cur.size()) {
                std::string next = TrimSpaces(cur.substr(last_space + 1));
                cur = TrimSpaces(cur.substr(0, last_space));
                flush();
                cur = next;
            } else {
                flush();
            }
            if (static_cast<int>(lines.size()) >= max_lines) break;
        }
        cur.append(s, i, char_len);
        if (s[i] == ' ') last_space = cur.size() - 1;
        i += char_len;
    }
    if (static_cast<int>(lines.size()) < max_lines) flush();
    return lines;
}

std::string EscapeAssText(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 16);
    for (unsigned char c : s) {
        switch (c) {
        case '{': out += "\\{"; break;
        case '}': out += "\\}"; break;
        case '\\': out += "\\\\"; break;
        case '\r': break;
        case '\n': out += "\\N"; break;
        default: out.push_back(static_cast<char>(c)); break;
        }
    }
    return out;
}


uint32_t Utf8CodepointAt(const std::string& s, size_t pos, size_t* len_out = nullptr) {
    size_t len = Utf8CharLenAt(s, pos);
    if (len_out) *len_out = len;
    if (len == 0 || pos >= s.size()) return 0;
    const unsigned char b0 = static_cast<unsigned char>(s[pos]);
    if (len == 1) return b0;
    if (len == 2) {
        return ((b0 & 0x1F) << 6) |
               (static_cast<unsigned char>(s[pos + 1]) & 0x3F);
    }
    if (len == 3) {
        return ((b0 & 0x0F) << 12) |
               ((static_cast<unsigned char>(s[pos + 1]) & 0x3F) << 6) |
               (static_cast<unsigned char>(s[pos + 2]) & 0x3F);
    }
    return ((b0 & 0x07) << 18) |
           ((static_cast<unsigned char>(s[pos + 1]) & 0x3F) << 12) |
           ((static_cast<unsigned char>(s[pos + 2]) & 0x3F) << 6) |
           (static_cast<unsigned char>(s[pos + 3]) & 0x3F);
}

bool IsEmojiCodepoint(uint32_t cp) {
    return (cp >= 0x1F000 && cp <= 0x1FAFF) ||
           (cp >= 0x2600 && cp <= 0x27BF) ||
           cp == 0x3030 || cp == 0x303D || cp == 0x3297 || cp == 0x3299 ||
           cp == 0x00A9 || cp == 0x00AE;
}

bool IsEmojiContinuationCodepoint(uint32_t cp) {
    return cp == 0xFE0F || cp == 0x200D || (cp >= 0x1F3FB && cp <= 0x1F3FF);
}

bool ContainsEmoji(const std::string& s) {
    for (size_t i = 0; i < s.size();) {
        size_t len = 0;
        uint32_t cp = Utf8CodepointAt(s, i, &len);
        if (len == 0) break;
        if (IsEmojiCodepoint(cp)) return true;
        i += len;
    }
    return false;
}

int CountCodepointIf(const std::string& s, bool (*pred)(uint32_t)) {
    int count = 0;
    for (size_t i = 0; i < s.size();) {
        size_t len = 0;
        uint32_t cp = Utf8CodepointAt(s, i, &len);
        if (len == 0) break;
        if (pred(cp)) ++count;
        i += len;
    }
    return count;
}

std::string LowerAscii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool ContainsAnyLiteral(const std::string& s, const std::vector<std::string>& words) {
    for (const auto& w : words) {
        if (!w.empty() && s.find(w) != std::string::npos) return true;
    }
    return false;
}

bool ContainsAnyPairLiteral(const std::string& s,
                            const std::vector<std::string>& a,
                            const std::vector<std::string>& b) {
    bool has_a = false;
    for (const auto& w : a) {
        if (!w.empty() && s.find(w) != std::string::npos) { has_a = true; break; }
    }
    if (!has_a) return false;
    for (const auto& w : b) {
        if (!w.empty() && s.find(w) != std::string::npos) return true;
    }
    return false;
}

int CountAnyLiteral(const std::string& s, const std::vector<std::string>& words) {
    int count = 0;
    for (const auto& w : words) {
        if (!w.empty() && s.find(w) != std::string::npos) ++count;
    }
    return count;
}

bool ContainsAnyPairNearLiteral(const std::string& s,
                                const std::vector<std::string>& a,
                                const std::vector<std::string>& b,
                                size_t max_span_bytes) {
    for (const auto& wa : a) {
        if (wa.empty()) continue;
        size_t pa = s.find(wa);
        while (pa != std::string::npos) {
            for (const auto& wb : b) {
                if (wb.empty()) continue;
                size_t pb = s.find(wb);
                while (pb != std::string::npos) {
                    const size_t lo = std::min(pa, pb);
                    const size_t hi = std::max(pa + wa.size(), pb + wb.size());
                    if (hi >= lo && hi - lo <= max_span_bytes) return true;
                    pb = s.find(wb, pb + 1);
                }
            }
            pa = s.find(wa, pa + 1);
        }
    }
    return false;
}

bool LooksAmbivalentOrControversialComment(const std::string& text, const std::string& lower) {
    // High-like comments on divisive works are often not positive; they can be
    // clever criticism or agreement with a negative take. This tool is intended
    // to show only comments that feel supportive while watching, so skip
    // ambivalent / controversy-framing language even when it has many likes.
    if (ContainsAnyLiteral(text, {
        "賛否", "賛否両論", "賛否ある", "荒れ", "荒れて", "炎上", "物議", "議論", "問題作",
        "好みが分か", "好き嫌い", "人を選ぶ", "刺さらない", "合わない", "受け付けない",
        "モヤる", "もやる", "モヤモヤ", "もやもや", "複雑", "納得いか", "納得でき",
        "賛同でき", "擁護でき", "肯定でき", "評価に困", "判断に困", "微妙な気持ち"
    })) return true;

    if (ContainsAnyLiteral(lower, {
        "mixed feelings", "divisive", "controversial", "not for everyone", "not my thing",
        "not for me", "hard to like", "hard to enjoy", "can't defend", "cannot defend",
        "i get why people hate", "i see why people hate", "i wanted to like"
    })) return true;

    return false;
}


int DerivativeOrAttributionDoubtRiskScore(const std::string& text, const std::string& lower) {
    // Soft mood-killer score for attribution / plagiarism / source-copy hints.
    // This is intentionally NOT a simple NG-word list.  The goal is to catch
    // comments whose main intent is "this is basically another work" while
    // avoiding false positives such as harmless "this reminds me of summer" or
    // positive "influenced by / homage" discussions.

    const int hard_accusation = CountAnyLiteral(text, {
        "パクリ", "ぱくり", "盗作", "盗用", "剽窃", "丸パクリ", "丸ぱくり"
    }) + CountAnyLiteral(lower, {
        "plagiar", "ripoff", "rip-off", "rip off", "stole from", "stolen from",
        "copied from", "straight copy", "basically copied"
    });
    if (hard_accusation > 0) return 100;

    int risk = 0;
    const int strong_similarity = CountAnyLiteral(text, {
        "似すぎ", "似過ぎ", "そっくり", "そっくりすぎ", "まんま", "そのまんま",
        "そのままじゃ", "そのままだ", "そのままだよ", "完全一致", "丸ごと"
    }) + CountAnyLiteral(lower, {
        "same as", "copy of", "basically the same", "pretty much the same", "almost the same"
    });

    const int soft_similarity = CountAnyLiteral(text, {
        "似てる", "似ている", "似てんな", "似てます", "似てきこえる", "似て聞こえる",
        "に似て", "と似て", "にそっくり", "とそっくり", "っぽく聞こえる"
    }) + CountAnyLiteral(lower, {
        "sounds like", "sound like", "reminds me of", "reminded me of", "similar to"
    });

    const int attribution_words = CountAnyLiteral(text, {
        "元ネタ", "元曲", "元歌", "原曲", "原曲そのまま", "元になった曲"
    });

    const int intensifiers = CountAnyLiteral(text, {
        "完全に", "ほぼ", "ほとんど", "まるで", "まさに", "どう聞いても", "どう聴いても"
    }) + CountAnyLiteral(lower, {
        "literally", "basically", "pretty much", "almost", "exactly"
    });

    const int contrast_prefix = CountAnyLiteral(text, {
        "好きだけど", "好きな曲だけど", "好きなんだけど", "良い曲だけど", "いい曲だけど", "名曲だけど",
        "嫌いじゃないけど", "悪くないけど"
    }) + CountAnyLiteral(lower, {
        "i like it but", "i love it but", "good song but", "not bad but"
    });

    const int assertion = CountAnyLiteral(text, {
        "だよね", "じゃん", "でしょ", "でしょ？", "よな", "よね", "じゃない？", "じゃないか"
    }) + CountAnyLiteral(lower, {
        "isn't it", "isnt it", "right?", "no?"
    });

    // A quoted outside work / artist reference is a weak source-reference hint.
    // By itself it is harmless, but with "完全に" / "だよね" / contrast framing
    // it becomes a likely attribution-doubt comment.
    const bool quoted_source_ref = ContainsAnyLiteral(text, {
        "の「", "の『", "の\"", "の'", " by ", " from "
    });

    risk += strong_similarity * 3;
    risk += soft_similarity * 2;
    risk += attribution_words * 2;
    if (intensifiers > 0) risk += 1;
    if (contrast_prefix > 0) risk += 1;
    if (assertion > 0) risk += 1;
    if (quoted_source_ref && (intensifiers > 0 || contrast_prefix > 0 || assertion > 0)) risk += 3;

    // Near-pair evidence is stronger than mere coexistence.  This catches
    // softened phrasing like "好きだけど、完全に○○の『××』だよね" without
    // relying on that exact sentence.
    if (ContainsAnyPairNearLiteral(text,
        {"好きだけど", "好きな曲だけど", "好きなんだけど", "良い曲だけど", "いい曲だけど", "名曲だけど"},
        {"完全に", "ほぼ", "まんま", "そのまま", "似て", "そっくり", "元ネタ", "元曲", "原曲", "の「", "の『"},
        120)) risk += 2;

    if (ContainsAnyPairNearLiteral(text,
        {"完全に", "ほぼ", "ほとんど", "どう聞いても", "どう聴いても", "まんま", "そのまま"},
        {"だよね", "じゃん", "同じ", "そっくり", "似て", "元ネタ", "元曲", "原曲", "の「", "の『"},
        140)) risk += 2;

    if (ContainsAnyPairNearLiteral(lower,
        {"sounds like", "reminds me of", "basically", "literally", "pretty much", "almost"},
        {"same", "copy", "copied", "ripoff", "rip-off", "stole", "stolen", "plagiar"},
        120)) risk += 2;

    // Positive framing lowers mild association comments, but does not rescue
    // high-risk attribution-doubt phrasing.
    const int clearly_appreciative = CountAnyLiteral(text, {
        "オマージュ", "リスペクト", "影響を受け", "影響受け", "好きな曲には共通点", "共通点がある"
    }) + CountAnyLiteral(lower, {
        "homage", "tribute", "inspired by", "influence", "influenced"
    });
    if (clearly_appreciative > 0 && risk < 6) risk -= 1;

    return std::max(0, risk);
}

bool LooksDerivativeOrAttributionDoubtComment(const std::string& text, const std::string& lower) {
    return DerivativeOrAttributionDoubtRiskScore(text, lower) >= 5;
}


int CoverOriginalComparisonRiskScore(const std::string& text, const std::string& lower) {
    // Cover/original comparison guard.
    // These comments are not always hostile, but on cover videos they often
    // pull the viewer away from the current performance by comparing it with
    // the original / canonical singer / another rapper.  Score intent from the
    // whole sentence instead of blocking a single word such as "本家".
    int risk = 0;

    const int source_terms = CountAnyLiteral(text, {
        "本家", "原曲", "元曲", "オリジナル", "Original", "original", "原版",
        "カバー", "cover", "Cover"
    }) + CountAnyLiteral(lower, {
        "original", "cover version", "cover song", "the cover", "the original"
    });

    const int direct_compare = CountAnyLiteral(text, {
        "より", "の方が", "のほうが", "方が", "ほうが", "とは違", "とはまた違", "また違った",
        "比べ", "比較", "敵わない", "かなわない", "勝てない", "越え", "超え",
        "じゃないと", "じゃなきゃ", "じゃないな", "やっぱり", "やはり", "圧倒的",
        "こんな感じにはならない", "こんな感じじゃない", "感じにはならない"
    }) + CountAnyLiteral(lower, {
        "better than", "worse than", "not as", "can't beat", "cannot beat",
        "original is", "the original is", "compared to", "different from"
    });

    const int performer_part_terms = CountAnyLiteral(text, {
        "ラップ", "RAP", "rap", "ボーカル", "vocal", "歌声", "歌い方", "声", "歌唱", "歌うま",
        "アレンジ", "編曲", "ver", "Ver", "バージョン"
    }) + CountAnyLiteral(lower, {
        "rap", "vocal", "voice", "singer", "version", "arrangement"
    });

    const int named_artist_hint = CountAnyLiteral(text, {
        "オザケン", "スチャダラ", "宇多田", "加藤ミリヤ", "KREVA", "kreva", "久保田", "小沢健二"
    });

    const int preference_words = CountAnyLiteral(text, {
        "好き", "良い", "いい", "上手", "うまい", "上手い", "敵わない", "なぁ", "がなぁ",
        "じゃないと", "かな", "かと思った", "思った", "聴きたい", "聞きたい"
    }) + CountAnyLiteral(lower, {
        "like", "love", "better", "good", "nice", "thought it was"
    });

    if (source_terms > 0) risk += 2;
    if (direct_compare > 0) risk += 2;
    if (performer_part_terms > 0) risk += 1;
    if (named_artist_hint > 0) risk += 1;
    if (preference_words > 0 && (source_terms > 0 || direct_compare > 0 || named_artist_hint > 0)) risk += 1;

    // Strong patterns for cover videos.  These are phrasing templates rather
    // than exact comment text, so they cover many variants while avoiding a raw
    // single-word NG list.
    if (ContainsAnyPairNearLiteral(text,
        {"本家", "原曲", "オリジナル", "Original", "original"},
        {"より", "敵わ", "かなわ", "勝て", "越え", "超え", "好き", "良い", "いい", "上手", "うま", "ずっと"},
        120)) risk += 4;

    if (ContainsAnyPairNearLiteral(text,
        {"カバー", "cover", "Cover"},
        {"なら", "より", "本家", "原曲", "オリジナル", "じゃないと", "かな", "やっぱり"},
        120)) risk += 4;

    if (ContainsAnyPairNearLiteral(text,
        {"ラップ", "RAP", "rap"},
        {"じゃないと", "じゃなきゃ", "の方が", "のほうが", "圧倒的", "やっぱり", "やはり", "がなぁ", "なら"},
        140)) risk += 4;

    if (ContainsAnyPairNearLiteral(text,
        {"とは", "と"},
        {"また違った良さ", "違った良さ", "こんな感じにはならない", "こんなマッチョ", "かと思った"},
        160)) risk += 3;

    if (ContainsAnyPairNearLiteral(text,
        {"宇多田", "オザケン", "スチャダラ", "加藤ミリヤ", "KREVA", "kreva"},
        {"かと思った", "じゃないと", "の方が", "のほうが", "圧倒的", "とはまた違", "なら"},
        160)) risk += 4;

    if (ContainsAnyPairNearLiteral(lower,
        {"original", "cover", "rap", "vocal", "voice", "version"},
        {"better", "worse", "can't beat", "cannot beat", "not as", "thought it was", "has to be"},
        140)) risk += 4;

    // Softly appreciative lines such as "本家もカバーも好き" are usually safe.
    // Reduce only low-risk balanced praise; strong comparison patterns above
    // remain blocked.
    if (CountAnyLiteral(text, {"どっちも好き", "どちらも好き", "両方好き", "本家も好き", "カバーも好き"}) > 0 && risk < 6) {
        risk -= 2;
    }

    return std::max(0, risk);
}

bool LooksCoverOriginalComparisonComment(const std::string& text, const std::string& lower) {
    return CoverOriginalComparisonRiskScore(text, lower) >= 5;
}


int ImitationRevivalPraiseScore(const std::string& text, const std::string& lower) {
    // Broad positive imitation / revival guard.
    // This is intentionally not an exact-comment matcher.  It combines several
    // weak signals: old-era nostalgia, revival/reproduction wording, resemblance,
    // performance details, "cannot hear the original anymore" relief, and praise.
    // The goal is to rescue comments that are valuable on impersonation / revival
    // videos while still blocking ordinary cover-vs-original ranking comments.
    int score = 0;

    const int era_terms = CountAnyLiteral(text, {
        "あの頃", "あのころ", "この頃", "このころ", "若い頃", "若いころ", "若かりし", "若かった",
        "昔", "昔の", "当時", "全盛期", "全盛", "黄金期", "ピーク", "初期", "デビュー当時",
        "あの時", "あの時代", "あの頃の", "この頃の", "懐か", "青春", "平成", "昭和"
    }) + CountAnyLiteral(lower, {
        "old days", "back then", "younger", "young", "prime", "golden age", "classic era", "nostalgic"
    });

    const int revival_terms = CountAnyLiteral(text, {
        "よみがえ", "蘇", "甦", "生き返", "復活", "再来", "降臨", "帰ってき", "戻った", "戻ってき",
        "再現", "完全再現", "再現度", "リバイバル", "タイムスリップ", "憑依", "宿って", "現れた",
        "蘇らせ", "甦らせ", "再現してくれ", "連れてき", "連れ戻", "呼び戻"
    }) + CountAnyLiteral(lower, {
        "revive", "revived", "revival", "recreate", "recreated", "recreation", "brought back",
        "brings back", "came back", "time slip", "returns", "resurrect"
    });

    const int identity_terms = CountAnyLiteral(text, {
        "本人", "ご本人", "本家", "オリジナル", "原曲", "元祖", "本物", "本体", "実物", "本人感",
        "本人より", "本家より", "オリジナルより"
    }) + CountAnyLiteral(lower, {
        "original", "real one", "real thing", "the original", "actual artist", "本人"
    });

    const int resemblance_terms = CountAnyLiteral(text, {
        "似て", "似すぎ", "似過ぎ", "そっくり", "ソックリ", "瓜二つ", "激似", "酷似", "まんま",
        "そのもの", "本人みたい", "本物みたい", "まるで本人", "ほぼ本人", "本人に近", "本家に近",
        "近づいて", "近付いて", "寄せて", "寄り", "声が似", "声似", "雰囲気似", "似てきた"
    }) + CountAnyLiteral(lower, {
        "sounds like", "looks like", "just like", "so close", "close to", "similar to", "almost the same",
        "exactly like", "spot on", "dead ringer", "uncanny"
    });

    const int performance_terms = CountAnyLiteral(text, {
        "声", "歌声", "声質", "発声", "歌い方", "歌唱", "音域", "高音", "低音", "ビブラート", "しゃくり",
        "語尾", "節回し", "間", "空気感", "雰囲気", "表現", "表情", "仕草", "所作", "動き",
        "パフォーマンス", "ライブ感", "ステージ", "ダンス", "演技", "ものまね", "モノマネ", "物真似",
        "模写", "なりきり", "コピー", "トレース", "本質", "本質的", "魂", "憑依"
    }) + CountAnyLiteral(lower, {
        "voice", "vocal", "tone", "singing", "performance", "stage", "gesture", "expression", "vibe",
        "essence", "soul", "impersonation", "imitation", "mimic", "tribute", "reproduction"
    });

    const int unavailable_or_loss_terms = CountAnyLiteral(text, {
        "もう聞けない", "もう聴けない", "二度と聞けない", "二度と聴けない", "今では聞けない", "今では聴けない",
        "今は聞けない", "今は聴けない", "聞けない声", "聴けない声", "失われた", "戻らない", "戻れない",
        "今の本人", "今の本家", "今となっては", "加齢", "歳", "年齢", "昔みたいには", "あの声はもう"
    }) + CountAnyLiteral(lower, {
        "can't hear", "cannot hear", "will never hear", "no longer", "not anymore", "lost voice", "aged", "ageing", "aging"
    });

    const int relief_terms = CountAnyLiteral(text, {
        "聞ける", "聴ける", "聞けて", "聴けて", "よかった", "良かった", "ありがたい", "嬉しい", "救われる",
        "満たされる", "補完", "代わり", "代替", "受け継", "継いで", "残して", "残る", "再現してくれて",
        "蘇らせてくれて", "甦らせてくれて", "ありがとう", "助かる", "望んでる", "望んでいる", "待ってた"
    }) + CountAnyLiteral(lower, {
        "glad", "happy", "thank", "thanks", "grateful", "can hear", "able to hear", "keeps it alive",
        "carry on", "fills the gap", "wanted this", "waiting for this"
    });

    const int praise_terms = CountAnyLiteral(text, {
        "最高", "すごい", "凄い", "素晴ら", "うまい", "上手", "上手い", "うますぎ", "巧い", "しぶ", "渋",
        "泣ける", "鳥肌", "感動", "エモ", "好き", "良い", "いい", "やばい", "ヤバい", "痺れ", "完璧",
        "天才", "本物", "圧巻", "完成度", "クオリティ", "レベル", "尊い", "胸熱", "刺さる", "グッとくる",
        "超えて", "超え", "越えて", "越え", "凌駕", "超越", "超えた", "別次元", "本質的"
    }) + CountAnyLiteral(lower, {
        "great", "amazing", "awesome", "fantastic", "love", "perfect", "incredible", "surpass", "surpassed",
        "exceed", "exceeded", "better", "goosebumps", "moving", "emotional"
    });

    // Basic semantic composition.  A single word is not enough; comments become
    // rescue-worthy when multiple categories support the same interpretation.
    if (era_terms > 0) score += 1;
    if (revival_terms > 0) score += 2;
    if (identity_terms > 0) score += 1;
    if (resemblance_terms > 0) score += 2;
    if (performance_terms > 0) score += 1;
    if (unavailable_or_loss_terms > 0) score += 2;
    if (relief_terms > 0) score += 1;
    if (praise_terms > 0) score += 1;

    // Cross-category bonuses.  These make the detector general: many different
    // phrases can trigger it as long as they express the same "revived original /
    // best-era reproduction" meaning.
    if (era_terms > 0 && (revival_terms > 0 || performance_terms > 0 || identity_terms > 0)) score += 2;
    if (revival_terms > 0 && (identity_terms > 0 || resemblance_terms > 0 || performance_terms > 0)) score += 2;
    if (resemblance_terms > 0 && (identity_terms > 0 || performance_terms > 0)) score += 2;
    if (unavailable_or_loss_terms > 0 && (relief_terms > 0 || performance_terms > 0 || identity_terms > 0)) score += 3;
    if (praise_terms > 0 && (revival_terms > 0 || resemblance_terms > 0 || unavailable_or_loss_terms > 0)) score += 1;

    // Strong contextual templates.  These are not exact sample sentences; each
    // side is a broad semantic family and only requires the concepts to be near.
    if (ContainsAnyPairNearLiteral(text,
        {"あの頃", "あのころ", "この頃", "このころ", "若い頃", "若いころ", "若かりし", "昔", "当時", "全盛", "黄金期", "ピーク"},
        {"よみがえ", "蘇", "甦", "戻", "復活", "再来", "声", "歌声", "雰囲気", "再現", "聞け", "聴け", "望ん"},
        180)) score += 4;

    if (ContainsAnyPairNearLiteral(text,
        {"本人", "ご本人", "本家", "オリジナル", "原曲", "元祖", "本物", "実物"},
        {"聞けない", "聴けない", "聞ける", "聴ける", "近づ", "近付", "似て", "そっくり", "激似", "再現", "よみがえ", "蘇", "甦", "戻"},
        180)) score += 4;

    if (ContainsAnyPairNearLiteral(text,
        {"声", "歌声", "声質", "歌い方", "発声", "ビブラート", "節回し", "雰囲気", "仕草", "表情", "パフォーマンス", "再現度"},
        {"似て", "そっくり", "近い", "まんま", "そのもの", "本人", "本家", "再現", "蘇", "よみがえ", "戻"},
        160)) score += 3;

    if (ContainsAnyPairNearLiteral(text,
        {"年々", "どんどん", "ますます", "回を追う", "見るたび", "聴くたび", "引くほど", "鳥肌", "まるで", "ほぼ", "完全に"},
        {"本人", "本家", "似て", "そっくり", "近づ", "近付", "再現度", "声", "歌声"},
        160)) score += 3;

    if (ContainsAnyPairNearLiteral(text,
        {"本人", "本家", "オリジナル", "原曲", "本質", "本質的", "再現度", "パフォーマンス"},
        {"超えて", "超え", "越えて", "越え", "凌駕", "超越", "上回", "別次元", "本物", "最高"},
        160)) score += 3;

    if (ContainsAnyPairNearLiteral(lower,
        {"old days", "back then", "young", "prime", "classic era", "original", "real one", "voice", "vocal", "performance"},
        {"revived", "brought back", "recreated", "sounds like", "just like", "so close", "spot on", "surpass", "better", "can hear"},
        180)) score += 4;

    // Do not rescue ordinary cover-vs-original ranking comments.  They may be
    // positive but are often distracting unless there is a clear revival / mimic
    // context around them.
    const int hostile_or_cover_ranking = CountAnyLiteral(text, {
        "敵わない", "かなわない", "勝てない", "じゃないと", "じゃなきゃ", "の方が良い", "のほうが良い",
        "の方がいい", "のほうがいい", "ラップがなぁ", "RAPはやっぱり", "やっぱり本家", "本家には",
        "カバーなら", "原曲の方", "オリジナルの方", "本家の方", "本家のほう"
    }) + CountAnyLiteral(lower, {
        "can't beat", "cannot beat", "has to be", "not as good", "original is better", "cover should"
    });

    // If the comment has no revival/resemblance/unavailable signal, ranking
    // language should stay blocked.  If it has very strong mimic context, keep it.
    if (hostile_or_cover_ranking > 0 && (revival_terms + resemblance_terms + unavailable_or_loss_terms + era_terms) == 0) score -= 5;
    else if (hostile_or_cover_ranking > 0 && score < 9) score -= 3;

    return std::max(0, score);
}

bool LooksImitationRevivalPraiseComment(const std::string& text, const std::string& lower) {
    return ImitationRevivalPraiseScore(text, lower) >= 6;
}

int ImitationVideoContextHintScore(const std::string& context_text) {
    // Video-title / metadata hint for impersonation or revival videos.
    // Comment-only inference can miss true monomane videos when the displayed
    // comments are short or generic.  This hint is intentionally conservative:
    // it helps classify the video context, but individual comments still need
    // positive / revival / resemblance signals before being rescued.
    if (context_text.empty()) return 0;
    const std::string lower = LowerAscii(context_text);
    int score = 0;

    score += CountAnyLiteral(context_text, {
        "ものまね", "モノマネ", "物真似", "歌まね", "歌マネ", "声まね", "声マネ", "声真似",
        "そっくり", "激似", "本人そっくり", "本家そっくり", "本人降臨", "ご本人降臨",
        "本人みたい", "まるで本人", "ほぼ本人", "なりきり", "完コピ", "完全コピー",
        "再現", "完全再現", "再現度", "憑依", "寄せ", "寄せて", "似せ", "似てる",
        "本人ではございません", "本人ではありません", "本人じゃありません", "本人ではない", "本人じゃない",
        "大変似ています", "とても似ています", "かなり似ています", "似ていますが", "似ております",
        "カバーアーティスト", "カバー歌手", "トリビュート", "リスペクトアーティスト",
        "再現するシンガー", "再現シンガー", "時代を越えて蘇", "時代を越えて甦", "時代を超えて蘇",
        "過去の", "アイドル期", "現代まで", "昭和再現", "LIVE'", "LIVE’", "ツアー再現"
    }) * 2;

    score += CountAnyLiteral(lower, {
        "impersonation", "impersonator", "imitation", "imitates", "mimic", "mimics",
        "voice impression", "sound alike", "soundalike", "look alike", "lookalike",
        "tribute act", "tribute performance", "tribute artist", "recreation", "recreated", "recreate",
        "not the real", "not actually", "not本人", "cover artist", "revival singer", "reproduction singer"
    }) * 2;

    const int cover_terms = CountAnyLiteral(context_text, {"COVER", "Cover", "cover", "カバー", "歌ってみた"}) +
        CountAnyLiteral(lower, {"cover", "covered", "cover song", "cover version"});
    const int not_original_disclaimer = CountAnyLiteral(context_text, {
        "本人ではございません", "本人ではありません", "本人じゃありません", "本人ではない", "本人じゃない",
        "本家ではございません", "本家ではありません", "オリジナル本人では"
    }) + CountAnyLiteral(lower, {"not the real", "not actually", "not the original artist"});
    const int resemblance_disclaimer = CountAnyLiteral(context_text, {
        "似ています", "似ていますが", "似ております", "そっくり", "激似", "本人そっくり", "大変似", "かなり似",
        "再現する", "完全に再現", "完全再現", "時代を越えて", "時代を超えて", "蘇える", "蘇る", "甦える", "甦る"
    }) + CountAnyLiteral(lower, {"sound alike", "look alike", "tribute", "recreate", "recreated", "revival"});
    const int era_recreation_terms = CountAnyLiteral(context_text, {
        "LIVE'", "LIVE’", "LIVE `", "JAPAN'", "昭和再現", "平成再現", "ツアー再現", "当時再現", "時代再現",
        "あの頃", "若い頃", "全盛期", "過去の", "アイドル期", "デビュー当時"
    });

    // Hybrid cover + impersonation / revival context.  This catches generic
    // channels where the surface title says only "COVER", but the metadata or
    // description explains that it is a resemblance / revival act.  It is not
    // tied to one URL or channel name.
    if (cover_terms > 0 && (not_original_disclaimer > 0 || resemblance_disclaimer > 0 || era_recreation_terms >= 2)) {
        score += 5;
    } else if (cover_terms > 0 && (resemblance_disclaimer > 0 || era_recreation_terms > 0) && score > 0) {
        score += 2;
    }

    // These words alone are not enough, but they strengthen an already likely
    // monomane/revival title or metadata block.
    if (score > 0) {
        score += CountAnyLiteral(context_text, {
            "本人", "本家", "オリジナル", "原曲", "声", "歌声", "歌い方", "雰囲気",
            "あの頃", "若い頃", "全盛期", "復活", "蘇", "甦", "よみがえ", "昭和", "平成", "LIVE"
        });
    }

    // Avoid treating plain cover-song titles as monomane just because they
    // mention the original artist.  If the metadata explicitly says "not the
    // real person" or "reproduction / resemblance", keep the hint.
    if (score <= 2 && cover_terms > 0 && not_original_disclaimer == 0 && resemblance_disclaimer == 0 && era_recreation_terms == 0) return 0;
    return std::min(score, 16);
}


bool ContainsHardBlockEmoji(const std::string& text) {
    for (size_t i = 0; i < text.size();) {
        size_t len = 0;
        uint32_t cp = Utf8CodepointAt(text, i, &len);
        if (len == 0) break;
        // U+1F4A9 pile of poo, U+1F595 middle finger, plus common vomit/sick faces.
        // These are never useful as overlay comments, even when the rest of
        // the sentence contains a positive-looking word.
        if (cp == 0x1F4A9 || cp == 0x1F595 || cp == 0x1F92E || cp == 0x1F922) return true;
        i += len;
    }
    return false;
}

bool LooksHardBlockedDirtyOrAbusiveComment(const std::string& text, const std::string& lower) {
    // Absolute safety layer.  This runs before positive scoring, new-comment
    // mixing, and imitation/revival rescue.  A missing comment is much better
    // than showing dirty insults or money-grubbing accusations during playback.
    if (ContainsHardBlockEmoji(text)) return true;

    if (ContainsAnyLiteral(text, {
        // Dirty / scatological expressions.
        "うんこ", "ウンコ", "ｳﾝｺ", "う○こ", "ウン○", "ウンチ", "うんち", "糞", "クソ", "くそ",
        "排泄", "便所", "汚物", "下品", "ゲロ", "吐き気", "吐きそう",

        // Money-grubbing / exploitation accusations.  These may be phrased as
        // opinions rather than direct insults, but they are highly mood-killing.
        "銭ゲバ", "ぜにゲバ", "ゼニゲバ", "守銭奴", "金の亡者", "金亡者",
        "金儲け", "金もうけ", "金目当て", "金のため", "金に汚", "金に目が", "金しか", "金だけ",
        "集金", "搾取", "ぼったくり", "ボッタクリ", "金づる", "金ヅル",

        // Strong personal attack / abuse.  Keep these absolute even in
        // monomane/revival context; they should never be rescued.
        "死ね", "しね", "氏ね", "消えろ", "くたばれ", "黙れ", "黙ってろ",
        "詐欺師", "詐欺", "犯罪者", "反社", "老害", "乞食", "こじき", "基地外", "キチガイ"
    })) return true;

    if (ContainsAnyLiteral(lower, {
        "poop", "shit", "shitty", "bullshit", "crap", "cash grab", "money grab",
        "greedy", "money hungry", "sellout", "scammer", "scam", "fraud",
        "go die", "kill yourself", "kys", "shut up", "fuck", "fucking"
    })) return true;

    if (ContainsAnyPairLiteral(text,
        {"金", "銭", "儲", "集金", "搾取"},
        {"汚", "目当", "亡者", "ゲバ", "だけ", "しか", "ため", "商売", "稼ぎ", "稼ぐ"})) return true;

    return false;
}


bool LooksViewerGatekeepingOrAntiHaterComment(const std::string& text, const std::string& lower) {
    // Blocks comments that are technically replies to haters/antis rather than
    // direct insults.  They still drag the viewer into comment-section fighting
    // ("then don't watch", "go home", etc.), so they are bad overlay material.
    if (ContainsAnyLiteral(text, {
        "見なければいい", "観なければいい", "見なきゃいい", "観なきゃいい",
        "みなければいい", "みなきゃいい", "見なけりゃいい", "観なけりゃいい",
        "聞かなければいい", "聴かなければいい", "聞かなきゃいい", "聴かなきゃいい",
        "聞かなけりゃいい", "聴かなけりゃいい",
        "見るなよ", "観るなよ", "みるなよ", "見るな", "観るな", "みるな",
        "聞くなよ", "聴くなよ", "聞くな", "聴くな",
        "来なければいい", "来なきゃいい", "こなきゃいい", "来るなよ", "くるなよ", "来るな", "くるな",
        "わざわざ来な", "わざわざ見", "わざわざ観", "わざわざ聞", "わざわざ聴",
        "帰れ", "かえれ", "帰れば", "帰ったら", "ブラウザバック", "ブラバ",
        "嫌なら見るな", "嫌なら観るな", "嫌なら聞くな", "嫌なら聴くな", "嫌なら来るな",
        "アンチは帰", "アンチ帰", "アンチ来るな", "アンチ見るな"
    })) return true;

    if (ContainsAnyLiteral(lower, {
        "don't watch", "dont watch", "do not watch", "then don't watch", "then dont watch",
        "don't listen", "dont listen", "do not listen", "then don't listen", "then dont listen",
        "why are you here", "why did you come", "go away", "go home", "leave then",
        "just leave", "haters leave", "hater go", "stop watching", "stop listening"
    })) return true;

    if (ContainsAnyPairLiteral(text,
        {"嫌なら", "気に入らないなら", "文句あるなら", "文句言うなら", "アンチなら"},
        {"見るな", "観るな", "聞くな", "聴くな", "来るな", "帰れ", "かえれ"})) return true;

    if (ContainsAnyPairLiteral(text,
        {"見", "観", "み", "聞", "聴", "来", "こ"},
        {"なければいい", "なきゃいい", "なけりゃいい", "なくていい", "るなよ", "るな", "くなよ", "くな"})) return true;

    return false;
}

bool LooksNegativeNuanceComment(const std::string& text, const std::string& lower) {
    // Nuance layer: catches mood-killing criticism that is not just a single NG
    // word, e.g. "汚された気分", "台無しにされた", "好きだったのに".
    if (ContainsAnyLiteral(text, {
        "汚された", "汚され", "汚した", "汚して", "汚すな", "汚れる", "汚れた気分",
        "穢された", "穢され", "けがされた", "けがされ", "けがすな",
        "台無し", "台なし", "だいなし", "ぶち壊", "ぶっ壊", "壊された", "壊した", "壊してる", "壊すな",
        "傷つけられ", "傷ついた", "傷つけた", "傷つく", "踏みにじ", "踏み躙",
        "裏切られ", "裏切り", "裏切った", "裏切って", "許せない", "許せん", "許さん", "許されない",
        "冒涜", "侮辱", "リスペクトがない", "敬意がない", "原作破壊", "原作レイプ", "改悪",
        "別物", "誰得", "蛇足", "余計", "いらんこと", "やめてほしい", "やめて欲しい",
        "返して", "昔に戻", "前の方が", "原作の方が", "旧の方が", "昔の方が",
        "期待外れ", "期待してたのに", "楽しみにしてたのに", "好きだったのに", "良かったのに",
        "見てられ", "見るに堪え", "見たくない", "気分悪", "気分が悪", "気持ち悪く", "胸糞",
        "悲しくな", "つらくな", "辛くな", "しんどく", "冷めた", "冷める", "醒めた", "醒める"
    })) return true;

    if (ContainsAnyPairLiteral(text,
        {"汚", "穢", "けが", "壊", "傷", "踏み", "裏切", "冒涜", "侮辱", "台無", "改悪", "蛇足", "余計"},
        {"気分", "気持ち", "感じ", "思い", "された", "られた", "してる", "すな", "ないで", "許せ", "見たく"})) return true;

    if (ContainsAnyPairLiteral(text,
        {"好きだった", "期待して", "楽しみにして", "良かった", "昔は", "前は", "原作は"},
        {"のに", "けど", "でも", "なのに", "無理", "残念", "がっかり", "萎え", "冷め", "改悪", "台無し"})) return true;

    if (ContainsAnyPairLiteral(text,
        {"こんなの", "これじゃ", "なんで", "どうして", "何して", "どこが"},
        {"無理", "残念", "ひど", "酷", "汚", "台無し", "改悪", "冷め", "萎え", "許せ", "違う", "別物"})) return true;

    if (ContainsAnyLiteral(lower, {
        "ruined it", "ruined this", "ruined the", "it ruined", "tainted", "butchered",
        "destroyed", "messed up", "cheapened", "soulless", "cash grab", "mockery",
        "insult to", "disrespect", "disrespectful", "slap in the face", "betrayed",
        "feel dirty", "felt dirty", "made me feel dirty", "hard to watch", "painful to watch",
        "unwatchable", "this ain't it", "this aint it", "not it", "miss the old",
        "wish they didn't", "wish they had not", "shouldn't have", "should not have"
    })) return true;

    if (LooksDerivativeOrAttributionDoubtComment(text, lower)) return true;
    if (LooksCoverOriginalComparisonComment(text, lower)) return true;

    return LooksAmbivalentOrControversialComment(text, lower);
}

bool LooksNegativeOrHostileComment(const std::string& text) {
    const std::string lower = LowerAscii(text);
    if (LooksHardBlockedDirtyOrAbusiveComment(text, lower)) return true;
    if (LooksViewerGatekeepingOrAntiHaterComment(text, lower)) return true;
    if (ContainsAnyLiteral(lower, {
        "hate", "worst", "trash", "garbage", "boring", "annoying", "cringe", "sucks",
        "bad song", "bad video", "dislike", "unsub", "overrated", "terrible", "awful",
        "disappointed", "not good", "not worth", "waste of time", "ruined", "ugly",
        "lame", "meh", "mid", "cringey", "corny", "cheesy", "cheap-looking"
    })) return true;

    if (ContainsAnyLiteral(text, {
        "嫌い", "最悪", "最低", "つまら", "つまらない", "おもしろくない", "面白くない",
        "退屈", "うざ", "ウザ", "きも", "キモ", "気持ち悪", "下手", "酷い", "ひどい",
        "酷すぎ", "ひどすぎ", "ゴミ", "カス", "いらない", "消えろ", "劣化", "微妙",
        "萎え", "もう見ない", "低評価", "不快", "残念", "ダメ", "駄目", "無理",
        "がっかり", "ガッカリ", "見る価値", "嫌だ", "嫌な",

        // Casual / softened negative slang. These are intentionally broad because
        // for this tool a missing comment is better than showing a mood-killer.
        "しょぼ", "ショボ", "ｼｮﾎﾞ", "しょっぼ", "ショッボ",
        "さむ", "サム", "寒い", "寒っ", "寒すぎ", "サムい",
        "ださ", "ダサ", "だっさ", "ダッサ",
        "いたい", "イタい", "痛い", "痛々",
        "すべって", "滑って", "滑り", "スベ",
        "くどい", "しつこい", "きつい", "キツい",
        "薄い", "雑", "雑すぎ", "安っぽ", "安ぽ",
        "茶番", "冷め", "醒め", "醒める", "冷える"
    })) return true;

    return LooksNegativeNuanceComment(text, lower);
}

bool ContainsPositiveEmoji(const std::string& text) {
    for (size_t i = 0; i < text.size();) {
        size_t len = 0;
        uint32_t cp = Utf8CodepointAt(text, i, &len);
        if (len == 0) break;
        if (cp == 0x2764 || cp == 0x2665 || (0x1F493 <= cp && cp <= 0x1F49F) ||
            cp == 0x1F525 || cp == 0x2728 || cp == 0x2B50 || cp == 0x1F31F ||
            cp == 0x1F44D || cp == 0x1F44F || cp == 0x1F64C ||
            cp == 0x1F389 || cp == 0x1F38A ||
            cp == 0x1F60D || cp == 0x1F970 || cp == 0x1F929 ||
            cp == 0x1F602 || cp == 0x1F923) {
            return true;
        }
        i += len;
    }
    return false;
}

bool LooksPositiveOrSupportiveComment(const std::string& text) {
    const std::string lower = LowerAscii(text);
    if (ContainsAnyLiteral(lower, {
        "love", "loved", "like this", "great", "amazing", "awesome", "beautiful", "cute",
        "good", "nice", "cool", "perfect", "best", "thanks", "thank you", "wonderful",
        "fantastic", "excellent", "favorite", "favourite", "masterpiece", "goosebumps",
        "well done", "so good", "so cool", "so beautiful", "lol", "haha"
    })) return true;
    if (ContainsAnyLiteral(text, {
        "最高", "神", "好き", "すき", "良い", "いい", "よい", "凄い", "すごい", "すご",
        "かわいい", "可愛い", "かっこいい", "カッコいい", "格好いい", "かっけ", "カッケ",
        "美しい", "美し", "綺麗", "きれい", "素敵", "素晴", "素晴らしい", "名曲", "名作",
        "名演", "名場面", "名シーン", "上手い", "上手", "うまい", "巧い",
        "ありがとう", "感謝", "泣ける", "泣いた", "泣けた", "尊い", "癒", "エモ",
        "鳥肌", "胸熱", "熱い", "震える", "刺さる", "刺さった", "センス", "天才",
        "完璧", "優勝", "草", "笑", "ｗｗ", "www", "👏", "👍", "✨", "🔥", "❤", "♥"
    })) return true;
    return ContainsPositiveEmoji(text);
}

std::string NormalizeLanguageCode(std::string lang) {
    lang = LowerAscii(lang);
    size_t cut = lang.find_first_of("-_ .");
    if (cut != std::string::npos) lang = lang.substr(0, cut);
    if (lang == "jp") lang = "ja";
    if (lang == "cn") lang = "zh";
    if (lang.empty()) lang = "all";
    return lang;
}

std::string GetWindowsUserLanguageCode() {
    wchar_t locale_name[LOCALE_NAME_MAX_LENGTH]{};
    if (GetUserDefaultLocaleName(locale_name, LOCALE_NAME_MAX_LENGTH) > 0) {
        return NormalizeLanguageCode(WideToUtf8(locale_name));
    }
    LANGID lid = GetUserDefaultUILanguage();
    switch (PRIMARYLANGID(lid)) {
    case LANG_JAPANESE: return "ja";
    case LANG_KOREAN: return "ko";
    case LANG_CHINESE: return "zh";
    default: return "en";
    }
}

bool IsLatinLetterCp(uint32_t cp) {
    return (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z');
}

bool IsHiraganaOrKatakanaCp(uint32_t cp) {
    return (cp >= 0x3040 && cp <= 0x30FF) || (cp >= 0x31F0 && cp <= 0x31FF) || (cp >= 0xFF66 && cp <= 0xFF9D);
}

bool IsCjkCp(uint32_t cp) {
    return (cp >= 0x3400 && cp <= 0x4DBF) ||
           (cp >= 0x4E00 && cp <= 0x9FFF) ||
           (cp >= 0xF900 && cp <= 0xFAFF) ||
           (cp >= 0x20000 && cp <= 0x2EBEF);
}

bool IsHangulCp(uint32_t cp) {
    return (cp >= 0x1100 && cp <= 0x11FF) ||
           (cp >= 0x3130 && cp <= 0x318F) ||
           (cp >= 0xAC00 && cp <= 0xD7AF);
}

std::string DetectTextLanguage(const std::string& s) {
    int latin = 0;
    int kana = 0;
    int cjk = 0;
    int hangul = 0;
    int other = 0;
    for (size_t i = 0; i < s.size();) {
        size_t len = 0;
        uint32_t cp = Utf8CodepointAt(s, i, &len);
        if (len == 0) break;
        if (IsLatinLetterCp(cp)) ++latin;
        else if (IsHiraganaOrKatakanaCp(cp)) ++kana;
        else if (IsCjkCp(cp)) ++cjk;
        else if (IsHangulCp(cp)) ++hangul;
        else if (cp > 0x7F && !IsEmojiCodepoint(cp) && !IsEmojiContinuationCodepoint(cp)) ++other;
        i += len;
    }

    const bool has_latin = latin >= 3;
    if (kana > 0) {
        if (has_latin && latin > kana + cjk + 8) return "mixed";
        return "ja";
    }
    if (hangul > 0 && hangul >= cjk && hangul >= latin / 2) {
        if (has_latin && latin > hangul + 8) return "mixed";
        return "ko";
    }
    if (cjk > 0) {
        if (has_latin && latin > cjk + 8) return "mixed";
        return "zh";
    }
    if (has_latin) return "en";
    if (other > 0) return "other";
    return "other";
}

bool LanguageMatchesPreference(const std::string& preferred, const std::string& detected) {
    if (preferred.empty() || preferred == "all" || preferred == "auto") return true;
    if (detected == preferred) return true;
    if (detected == "mixed") return true;
    // Japanese comments often contain only kanji fragments. Give them a partial match.
    if (preferred == "ja" && detected == "zh") return true;
    return false;
}

double LanguagePriorityScore(const std::string& preferred, const Comment& c) {
    if (preferred.empty() || preferred == "all" || preferred == "auto") return 0.50;
    const std::string detected = c.lang.empty() ? DetectTextLanguage(c.text) : c.lang;
    if (detected == preferred) return 1.00;
    if (detected == "mixed") return 0.78;
    if (preferred == "ja" && detected == "zh") return 0.62;
    if (preferred == "en" && detected == "other" && Utf8CharCount(c.text) <= 12) return 0.45;
    return 0.18;
}

bool IsExcitedPunctuation(uint32_t cp) {
    return cp == '!' || cp == '?' || cp == 0xFF01 || cp == 0xFF1F || cp == 0x203C || cp == 0x2049;
}

bool IsJapaneseCommaOrPeriod(uint32_t cp) {
    return cp == 0x3001 || cp == 0x3002 || cp == ',' || cp == '.';
}

enum class CommentMood {
    Standard,
    Formal,
    Pop,
    Excited,
    Soft,
};

const char* MoodName(CommentMood mood) {
    switch (mood) {
    case CommentMood::Formal: return "formal";
    case CommentMood::Pop: return "pop";
    case CommentMood::Excited: return "excited";
    case CommentMood::Soft: return "soft";
    default: return "standard";
    }
}

struct FontPick {
    CommentMood mood = CommentMood::Standard;
    std::string face = "Yu Gothic UI";
    int size_adjust = 0;
    bool force_bold = false;
};

CommentMood DetectCommentMood(const Comment& c) {
    const std::string& t = c.text;
    const std::string lower = LowerAscii(t);
    const int chars = static_cast<int>(Utf8CharCount(t));
    const bool has_emoji = ContainsEmoji(t);
    const int excited_punc = CountCodepointIf(t, IsExcitedPunctuation);
    const int formal_punc = CountCodepointIf(t, IsJapaneseCommaOrPeriod);

    int casual_score = 0;
    if (has_emoji) casual_score += 3;
    if (excited_punc >= 2) casual_score += 2;
    if (ContainsAnyLiteral(lower, {"www", "w ", " lol", "haha", "kawaii"})) casual_score += 2;
    if (ContainsAnyLiteral(t, {"笑", "草", "最高", "神", "好き", "すき", "かわいい", "可愛い", "やば", "ヤバ", "！」", "！？", "!!", "??"})) casual_score += 2;
    if (c.tier >= 3 && excited_punc >= 1) casual_score += 1;

    int formal_score = 0;
    if (!has_emoji && formal_punc >= 2) formal_score += 2;
    if (!has_emoji && chars >= 55) formal_score += 1;
    if (ContainsAnyLiteral(t, {"です", "ます", "でした", "ました", "と思います", "について", "という", "つまり", "一方", "理由", "印象", "構成", "歌詞", "作曲", "演奏", "解釈"})) formal_score += 2;
    if (ContainsAnyLiteral(t, {"「", "」", "、", "。"}) && !has_emoji) formal_score += 1;

    int soft_score = 0;
    if (ContainsAnyLiteral(t, {"ありがとう", "感謝", "懐かしい", "泣", "涙", "尊い", "沁み", "綺麗", "素敵", "癒", "エモ"})) soft_score += 2;
    if (has_emoji && ContainsAnyLiteral(t, {"♡", "♥", "💕", "😭", "🥺", "😢"})) soft_score += 1;

    if (casual_score >= 4) return c.tier >= 3 ? CommentMood::Excited : CommentMood::Pop;
    if (soft_score >= 2 && formal_score < 3) return CommentMood::Soft;
    if (formal_score >= 3 && casual_score <= 1) return CommentMood::Formal;
    if (casual_score >= 2) return CommentMood::Pop;
    return CommentMood::Standard;
}

FontPick PickFont(const Comment& c) {
    FontPick fp;
    fp.mood = DetectCommentMood(c);
    switch (fp.mood) {
    case CommentMood::Formal:
        fp.face = "Meiryo";
        fp.size_adjust = -1;
        break;
    case CommentMood::Pop:
        fp.face = "Yu Gothic UI Semibold";
        fp.size_adjust = 1;
        fp.force_bold = true;
        break;
    case CommentMood::Excited:
        fp.face = "Yu Gothic UI Semibold";
        fp.size_adjust = 4;
        fp.force_bold = true;
        break;
    case CommentMood::Soft:
        fp.face = "Yu Gothic";
        fp.size_adjust = 0;
        break;
    default:
        fp.face = "Yu Gothic UI";
        break;
    }
    return fp;
}

void AppendAssEscapedUtf8(std::string& out, const std::string& s, size_t pos, size_t len) {
    for (size_t j = 0; j < len && pos + j < s.size(); ++j) {
        unsigned char c = static_cast<unsigned char>(s[pos + j]);
        switch (c) {
        case '{': out += "\\{"; break;
        case '}': out += "\\}"; break;
        case '\\': out += "\\\\"; break;
        case '\r': break;
        case '\n': out += "\\N"; break;
        default: out.push_back(static_cast<char>(c)); break;
        }
    }
}

std::string EmojiAssColor(const Comment& c) {
    if (c.tier >= 4) return "&H1E7DFF&";
    if (c.tier >= 3) return "&H35A4FF&";
    if (c.tier >= 2) return "&H7ADFFF&";
    if (c.from_new_sort) return "&HFFD38A&";
    return "&H9BE8FF&";
}

std::string EmojiColorForCodepoint(uint32_t cp, const std::string& fallback) {
    // ASS colors are BGR: &HBBGGRR&. Give emoji some playful variety.
    if (cp == 0x2764 || cp == 0x2665 || (0x1F493 <= cp && cp <= 0x1F49F)) return "&HCC66FF&"; // hearts: pink
    if (cp == 0x1F525) return "&H208BFF&"; // fire: orange
    if (cp == 0x2728 || cp == 0x2B50 || cp == 0x1F31F || cp == 0x1F4AB) return "&H00D7FF&"; // stars: gold
    if (cp == 0x1F389 || cp == 0x1F38A) return "&HFF70D0&"; // party: purple/magenta
    if (cp == 0x1F602 || cp == 0x1F923 || cp == 0x1F622 || cp == 0x1F62D || cp == 0x1F4A7) return "&HFFD880&"; // tears/water: cyan
    if (cp == 0x1F44D || cp == 0x1F44F || cp == 0x1F64C) return "&HA0E6FF&"; // hands: warm yellow
    if (cp == 0x1F331 || cp == 0x1F33F || cp == 0x1F340) return "&H60D070&"; // green
    if (0x1F600 <= cp && cp <= 0x1F64F) return "&H9BE8FF&"; // faces: soft yellow
    return fallback;
}

std::string ColorizeEmojiAssText(const std::string& t, const std::string& main_color, const std::string& emoji_color) {
    std::string out;
    std::string active_color = main_color;
    bool previous_was_emoji = false;
    for (size_t i = 0; i < t.size();) {
        size_t len = 0;
        uint32_t cp = Utf8CodepointAt(t, i, &len);
        if (len == 0) break;
        bool emojiish = IsEmojiCodepoint(cp) || (previous_was_emoji && IsEmojiContinuationCodepoint(cp));
        std::string want_color = emojiish ? EmojiColorForCodepoint(cp, emoji_color) : main_color;
        if (want_color != active_color) {
            out += "{\\c" + want_color + "}";
            active_color = want_color;
        }
        AppendAssEscapedUtf8(out, t, i, len);
        previous_was_emoji = emojiish;
        i += len;
    }
    if (active_color != main_color) out += "{\\c" + main_color + "}";
    return out;
}

enum class EffectMode {
    None = 0,
    SoftPulse = 1,
    BurstQuake = 2,
};

bool HasBurstPunctuation(const std::string& t) {
    return ContainsAnyLiteral(t, {"！！", "!!", "！？", "?!", "!?", "！", "!"});
}

bool IsBurstActionComment(const Comment& c, CommentMood mood) {
    const int chars = static_cast<int>(Utf8CharCount(c.text));
    if (chars > 70) return false;

    const bool burst_punc = HasBurstPunctuation(c.text);
    const bool hot_word = ContainsAnyLiteral(c.text, {"最高", "神", "やば", "ヤバ", "すご", "好き", "かわいい", "可愛い", "草", "笑"});
    const bool playful = (mood == CommentMood::Excited || mood == CommentMood::Pop);

    // "最高！！" / "神！！" / "好き！" のような短く勢いのあるコメントは、
    // 読ませる字幕ではなくリアクションとして強めに動かして短めに消す。
    if (burst_punc && chars <= 55 && (playful || hot_word || c.tier >= 1)) return true;
    if (chars <= 30 && c.tier >= 2 && (burst_punc || hot_word || ContainsEmoji(c.text))) return true;
    return false;
}

EffectMode DecideEffectMode(const Comment& c, CommentMood mood) {
    const int chars = static_cast<int>(Utf8CharCount(c.text));
    if (chars > 95) return EffectMode::None;

    if (IsBurstActionComment(c, mood)) return EffectMode::BurstQuake;

    const bool playful = (mood == CommentMood::Excited || mood == CommentMood::Pop);
    if (!playful) return EffectMode::None;

    const bool has_energy = ContainsEmoji(c.text) ||
        CountCodepointIf(c.text, IsExcitedPunctuation) >= 1 ||
        ContainsAnyLiteral(c.text, {"www", "ｗｗ", "笑", "草", "最高", "神", "かわいい", "可愛い", "すご", "好き"});

    if (c.tier >= 3) return EffectMode::SoftPulse;
    if (c.tier >= 2 && has_energy) return EffectMode::SoftPulse;
    if (c.tier >= 1 && chars <= 45 && has_energy) return EffectMode::SoftPulse;
    return EffectMode::None;
}

int EffectStrengthFor(const Comment& c, EffectMode mode) {
    if (mode == EffectMode::BurstQuake) {
        if (c.tier >= 3) return 14;
        if (c.tier >= 2) return 12;
        return 10;
    }
    if (c.tier >= 4) return 8;
    if (c.tier >= 3) return 7;
    if (c.tier >= 2) return 5;
    return 4;
}


std::string DisplayPrefix(const Comment& c) {
    if (c.tier >= 4) return "🔥🔥 ";
    if (c.tier >= 3) return "🔥 ";
    if (c.tier >= 2) return "✨ ";
    if (c.from_new_sort) return "🆕 ";
    return "💬 ";
}

std::string PrefixAssColor(const Comment& c) {
    // ASS color is BGR: &HBBGGRR&.
    if (c.tier >= 4) return "&H2B8CFF&"; // warm orange
    if (c.tier >= 3) return "&H3CAEFF&"; // soft amber
    if (c.tier >= 2) return "&H9ADFFF&"; // pale yellow
    if (c.from_new_sort) return "&HFFDFA8&"; // soft blue
    return "&HCCCCCC&"; // calm gray
}

std::string MainTextAssColor(const Comment& c) {
    if (c.tier >= 4) return "&HE8F0F4&"; // still softer than pure white
    if (c.tier >= 3) return "&HE2ECEF&";
    return "&HD6DEE3&";                  // calmer than pure white
}

std::string MakeDisplayText(const Comment& c) {
    return DisplayPrefix(c) + c.text;
}

bool IsRootComment(const Comment& c) {
    return c.parent.empty() || c.parent == "root" || c.parent == "0" || c.parent == c.id;
}

bool LooksLowMoodThreadReply(const std::string& text) {
    // Replies are shown as part of a conversation, so keep them a little
    // stricter than standalone comments.  These are not always hostile, but
    // they tend to feel mood-killing when they pop up during playback.
    const std::string lower = LowerAscii(text);
    if (ContainsAnyLiteral(text, {
        "うーん", "う〜ん", "う～ん", "うーーん", "うむ",
        "微妙", "惜しい", "もったいない", "勿体ない", "違和感", "解釈違い",
        "苦手", "好きじゃない", "好きではない", "好かん",
        "よくわから", "よく分から", "わからん", "分からん", "わかんない", "分かんない",
        "なんか違", "何か違", "ちょっと違", "別に", "普通だった", "普通かな", "普通かも", "普通すぎ",
        "もういい", "飽き", "長い", "重い", "引く", "引いた"
    })) return true;
    if (ContainsAnyLiteral(lower, {
        "not sure", "i don't know", "i dont know", "idk", "kinda", "kind of", "sort of",
        "weird", "awkward", "too much", "overdone", "could have", "should have",
        "wish it", "wish they", "not really", "not a fan", "meh"
    })) return true;
    return false;
}

bool LooksSupportiveThreadReply(const Comment& r) {
    if (r.text.empty()) return false;
    if (LooksNegativeOrHostileComment(r.text)) return false;
    if (LooksLowMoodThreadReply(r.text)) return false;
    if (LooksPositiveOrSupportiveComment(r.text)) return true;

    // Short agreement replies can be pleasant when attached to a positive
    // parent, even if they do not contain an obvious praise word.
    const std::string lower = LowerAscii(r.text);
    if (ContainsAnyLiteral(r.text, {
        "わかる", "分かる", "わかりみ", "それな", "ほんと", "本当",
        "確かに", "たしかに", "同意", "同じく", "ですよね", "だよね",
        "ほんそれ", "まじで", "マジで", "泣", "涙"
    })) return true;
    if (ContainsAnyLiteral(lower, {
        "agree", "same", "same here", "exactly", "true", "for real", "fr", "+1", "me too"
    })) return true;
    return false;
}

int ThreadReplyRankScore(const Comment& r) {
    int s = std::max(0, r.likes) * 10;
    if (LooksPositiveOrSupportiveComment(r.text)) s += 120;
    if (ContainsPositiveEmoji(r.text)) s += 60;
    if (r.from_new_sort) s += 20;
    const size_t chars = Utf8CharCount(r.text);
    if (chars >= 4 && chars <= 80) s += 25;
    if (chars > 120) s -= 40;
    return s;
}

std::vector<Comment> SelectSupportiveThreadReplies(const std::vector<Comment>& replies, int max_reply_count, int* filtered_count = nullptr) {
    std::vector<Comment> safe;
    int filtered = 0;
    for (const auto& r : replies) {
        if (LooksSupportiveThreadReply(r)) safe.push_back(r);
        else ++filtered;
    }
    std::stable_sort(safe.begin(), safe.end(), [](const Comment& a, const Comment& b) {
        const int sa = ThreadReplyRankScore(a);
        const int sb = ThreadReplyRankScore(b);
        if (sa != sb) return sa > sb;
        return a.likes > b.likes;
    });
    if (static_cast<int>(safe.size()) > max_reply_count) safe.resize(static_cast<size_t>(max_reply_count));
    if (filtered_count) *filtered_count = filtered;
    return safe;
}

std::string ThreadTextCandidate(const Comment& parent, const std::vector<Comment>& selected_replies) {
    std::string text = parent.text;
    for (const auto& r : selected_replies) {
        if (r.text.empty()) continue;
        text += "\n↳ " + r.text;
    }
    return text;
}

std::string TrimAsciiSpaces(std::string s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n')) s.erase(s.begin());
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) s.pop_back();
    return s;
}

std::string StripThreadMarker(std::string s) {
    s = TrimAsciiSpaces(std::move(s));
    const std::string arrow = "↳";
    if (s.rfind(arrow, 0) == 0) {
        s.erase(0, arrow.size());
        s = TrimAsciiSpaces(std::move(s));
    }
    if (!s.empty() && (s[0] == '>' || s[0] == '-')) {
        s.erase(s.begin());
        s = TrimAsciiSpaces(std::move(s));
    }
    return s;
}

std::vector<std::string> SplitThreadLines(const std::string& text) {
    std::vector<std::string> lines;
    std::string cur;
    for (char ch : text) {
        if (ch == '\r') continue;
        if (ch == '\n') {
            cur = TrimAsciiSpaces(std::move(cur));
            if (!cur.empty()) lines.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(ch);
        }
    }
    cur = TrimAsciiSpaces(std::move(cur));
    if (!cur.empty()) lines.push_back(cur);
    return lines;
}

std::vector<Comment> ExpandThreadCommentForConversation(const Comment& c) {
    std::vector<Comment> out;
    if (!c.threaded || c.text.find('\n') == std::string::npos) return out;
    auto lines = SplitThreadLines(c.text);
    if (lines.size() < 2) return out;

    for (size_t i = 0; i < lines.size(); ++i) {
        Comment part = c;
        std::string body = StripThreadMarker(lines[i]);
        if (body.empty()) continue;
        if (i == 0) {
            part.text = body;
        } else {
            part.text = "↳ " + body;
            part.likes = std::max(0, c.likes / 2);
            part.tier = std::max(0, std::min(c.tier, 2));
            part.score = std::max(0.0, c.score - 0.03 * static_cast<double>(i));
            part.from_new_sort = c.from_new_sort;
        }
        part.threaded = true;
        if (!part.id.empty()) part.id += "#threadpart" + std::to_string(i);
        out.push_back(std::move(part));
    }
    return out.size() >= 2 ? out : std::vector<Comment>{};
}

double CommentFreshnessScore(const Comment& c) {
    double s = c.from_new_sort ? 0.60 : 0.0;
    if (c.timestamp > 0) {
        std::time_t now = std::time(nullptr);
        double days = std::max(0.0, (static_cast<double>(now) - static_cast<double>(c.timestamp)) / 86400.0);
        double by_age = 0.0;
        if (days <= 2.0) by_age = 1.0;
        else if (days <= 7.0) by_age = 0.85;
        else if (days <= 30.0) by_age = 0.65;
        else if (days <= 120.0) by_age = 0.40;
        else if (days <= 365.0) by_age = 0.22;
        s = std::max(s, by_age);
    }
    return std::min(1.0, s);
}

int ComputeTier(int likes, int max_likes, bool from_new_sort) {
    if (max_likes <= 0) return from_new_sort ? 1 : 0;
    double ratio = static_cast<double>(likes) / static_cast<double>(std::max(1, max_likes));
    int tier = 0;
    if (likes >= 1000 || ratio >= 0.85) tier = 4;
    else if (likes >= 300 || ratio >= 0.60) tier = 3;
    else if (likes >= 80 || ratio >= 0.35) tier = 2;
    else if (likes >= 10 || ratio >= 0.15) tier = 1;
    if (from_new_sort && likes > 0) tier = std::max(tier, 1);

    // On videos with only a few likes overall, do not make the top comment look
    // like a mega-hit. Still reward it, but keep the visual scale reasonable.
    int cap = 4;
    if (max_likes < 10) cap = 2;
    else if (max_likes < 50) cap = 3;
    return std::min(tier, cap);
}


int ComputeAdaptiveMinLikesForDisplay(size_t item_count, int liked_count, int max_likes) {
    // YouTube exposes like_count but not dislike_count.  Use an adaptive minimum
    // like threshold as a practical "do not show poorly received comments" gate.
    // The threshold intentionally stays at 0 for small/quiet videos so that low
    // traffic videos do not become empty.  On busy videos, especially when the
    // top comments have many likes, new zero-like comments are often noisy or
    // negative, so require at least a small amount of positive reaction.
    if (max_likes <= 0) return 0;
    if (item_count < 16 || liked_count < 4) return 0;

    if (item_count >= 80 && liked_count >= 28 && max_likes >= 5000) return 5;
    if (item_count >= 60 && liked_count >= 22 && max_likes >= 2000) return 4;
    if (item_count >= 45 && liked_count >= 16 && max_likes >= 1000) return 3;
    if (item_count >= 35 && liked_count >= 12 && max_likes >= 300)  return 2;
    if (item_count >= 25 && liked_count >= 8  && max_likes >= 50)   return 1;
    if (item_count >= 40 && liked_count >= 10) return 1;
    return 0;
}

std::vector<Comment> BuildDisplayComments(std::vector<Comment> raw, int limit, int max_chars, const std::string& preferred_lang, const std::string& video_context_hint = std::string()) {
    std::vector<Comment> out;
    if (raw.empty()) return out;

    std::stable_sort(raw.begin(), raw.end(), [](const Comment& a, const Comment& b) {
        return a.likes > b.likes;
    });

    std::map<std::string, std::vector<Comment>> replies_by_parent;
    std::vector<Comment> parents;
    std::vector<Comment> fallback;

    for (const auto& c : raw) {
        if (c.text.empty()) continue;
        if (!IsWithinCharLimit(c.text, max_chars)) continue;
        fallback.push_back(c);
        if (!c.id.empty() && !IsRootComment(c)) {
            replies_by_parent[c.parent].push_back(c);
        } else {
            parents.push_back(c);
        }
    }

    for (auto& kv : replies_by_parent) {
        std::stable_sort(kv.second.begin(), kv.second.end(), [](const Comment& a, const Comment& b) {
            return a.likes > b.likes;
        });
    }

    std::vector<Comment> items;
    int thread_candidates_seen = 0;
    int thread_replies_used = 0;
    int thread_replies_filtered = 0;
    int thread_candidates_built = 0;
    std::set<std::string> seen_item_text;
    auto push_item = [&](Comment c) {
        if (c.text.empty()) return;
        if (!IsWithinCharLimit(c.text, max_chars)) return;
        if (!seen_item_text.insert(c.text).second) return;
        items.push_back(std::move(c));
    };

    for (const auto& p : parents) {
        Comment item = p;
        auto it = replies_by_parent.find(p.id);
        if (it != replies_by_parent.end() && !it->second.empty()) {
            ++thread_candidates_seen;
            int filtered = 0;
            std::vector<Comment> selected = SelectSupportiveThreadReplies(it->second, 3, &filtered);
            thread_replies_filtered += filtered;

            while (!selected.empty()) {
                std::string thread_text = ThreadTextCandidate(p, selected);
                if (IsWithinCharLimit(thread_text, max_chars)) {
                    item.text = thread_text;
                    item.threaded = true;
                    item.reply_count = std::max<int>(p.reply_count, static_cast<int>(it->second.size()));
                    for (const auto& r : selected) {
                        item.likes = std::max(item.likes, r.likes);
                        item.timestamp = std::max(item.timestamp, r.timestamp);
                        item.from_new_sort = item.from_new_sort || r.from_new_sort;
                    }
                    thread_replies_used += static_cast<int>(selected.size());
                    ++thread_candidates_built;
                    break;
                }
                selected.pop_back();
            }
        }
        push_item(item);
    }
    for (const auto& c : fallback) push_item(c);

    DebugLog("thread build parents=" + std::to_string(thread_candidates_seen) +
             " built=" + std::to_string(thread_candidates_built) +
             " replies_used=" + std::to_string(thread_replies_used) +
             " replies_filtered=" + std::to_string(thread_replies_filtered));

    if (items.empty()) return out;

    int max_likes = 0;
    int liked_count = 0;
    for (const auto& c : items) {
        max_likes = std::max(max_likes, c.likes);
        if (c.likes > 0) ++liked_count;
    }

    const bool abundant = items.size() >= 30 && liked_count >= 10 && max_likes >= 10;
    const int min_likes_for_display = ComputeAdaptiveMinLikesForDisplay(items.size(), liked_count, max_likes);
    DebugLog("quality gate items=" + std::to_string(items.size()) +
             " liked_count=" + std::to_string(liked_count) +
             " max_likes=" + std::to_string(max_likes) +
             " min_likes=" + std::to_string(min_likes_for_display));
    std::mt19937 rng(static_cast<unsigned int>(GetTickCount()) ^ 0x6D2B79F5u);
    std::uniform_real_distribution<double> jitter(0.0, 1.0);

    std::vector<Comment> candidates;
    int preferred_count = 0;
    int mixed_count = 0;
    for (auto& c : items) {
        c.lang = DetectTextLanguage(c.text);
        if (LanguageMatchesPreference(preferred_lang, c.lang)) ++preferred_count;
        if (c.lang == "mixed") ++mixed_count;
    }
    DebugLog("language priority resolved=" + preferred_lang +
             " preferred_or_mixed=" + std::to_string(preferred_count) +
             " mixed=" + std::to_string(mixed_count) +
             " total_items=" + std::to_string(items.size()));

    int skipped_negative = 0;
    int skipped_hard_block = 0;
    int skipped_audience_fightback = 0;
    int skipped_non_positive = 0;
    int skipped_cover_compare = 0;
    int skipped_low_language = 0;
    int rescued_imitation_praise = 0;
    const int preferred_ratio_pct = items.empty() ? 0 : static_cast<int>((static_cast<long long>(preferred_count) * 100) / static_cast<long long>(items.size()));
    const bool weak_preferred_language_pool = !(preferred_lang.empty() || preferred_lang == "all" || preferred_lang == "auto") &&
                                             items.size() >= 40 && preferred_count < std::max(24, static_cast<int>(items.size() * 45 / 100));
    const bool strong_preferred_pool = !(preferred_lang.empty() || preferred_lang == "all" || preferred_lang == "auto") &&
                                       items.size() >= 40 &&
                                       preferred_count >= 48 &&
                                       preferred_count >= static_cast<int>(items.size() * 55 / 100);
    DebugLog("language pool preferred_ratio=" + std::to_string(preferred_ratio_pct) +
             " weak_preferred_pool=" + std::to_string(weak_preferred_language_pool ? 1 : 0) +
             " strong_preferred_pool=" + std::to_string(strong_preferred_pool ? 1 : 0));

    int imitation_context_hits = 0;
    int cover_context_hits = 0;
    double imitation_context_weight = 0.0;
    double cover_context_weight = 0.0;
    for (const auto& it : items) {
        const std::string lower_it = LowerAscii(it.text);
        const int s = ImitationRevivalPraiseScore(it.text, lower_it);
        const int cover_s = CoverOriginalComparisonRiskScore(it.text, lower_it);
        const double like_heat = max_likes > 0
            ? std::log1p(static_cast<double>(std::max(0, it.likes))) / std::log1p(static_cast<double>(max_likes))
            : 0.0;
        if (s >= 5) ++imitation_context_hits;
        if (s >= 4) imitation_context_weight += std::min(5.0, s * 0.55 + like_heat * 2.0 + (it.threaded ? 0.8 : 0.0));
        if (cover_s >= 5) ++cover_context_hits;
        if (cover_s >= 4) cover_context_weight += std::min(4.0, cover_s * 0.45 + like_heat * 1.5);
    }
    const int external_imitation_hint = ImitationVideoContextHintScore(video_context_hint);
    const bool title_imitation_context = external_imitation_hint >= 4 ||
        (external_imitation_hint >= 2 && (imitation_context_hits >= 1 || imitation_context_weight >= 1.5));
    const bool imitation_context = items.size() >= 12 &&
        (imitation_context_hits >= std::max(2, static_cast<int>(items.size() / 24)) ||
         imitation_context_weight >= std::max(5.5, static_cast<double>(items.size()) / 12.0) ||
         title_imitation_context);
    const bool cover_context = !imitation_context && items.size() >= 12 &&
        (cover_context_hits >= std::max(2, static_cast<int>(items.size() / 28)) ||
         cover_context_weight >= std::max(4.5, static_cast<double>(items.size()) / 16.0));
    const char* inferred_context = imitation_context ? "imitation/revival" : (cover_context ? "cover/original-compare" : "general");
    DebugLog(std::string("comment context inferred=") + inferred_context +
             " imitation_context_hits=" + std::to_string(imitation_context_hits) +
             " imitation_context_weight=" + std::to_string(static_cast<int>(imitation_context_weight + 0.5)) +
             " external_imitation_hint=" + std::to_string(external_imitation_hint) +
             " cover_context_hits=" + std::to_string(cover_context_hits) +
             " cover_context_weight=" + std::to_string(static_cast<int>(cover_context_weight + 0.5)));
    for (auto c : items) {
        if (c.lang.empty()) c.lang = DetectTextLanguage(c.text);
        const size_t chars = Utf8CharCount(c.text);
        if (chars < 2) continue;

        // Strict positive-only gate. A high like count is not treated as proof
        // of positivity because divisive works often give many likes to sharp
        // criticism. Negative / ambivalent nuance is removed first, then the
        // comment must still contain an explicitly supportive signal.
        const bool standalone_reply = !c.threaded && !IsRootComment(c);
        const bool supportive_standalone_reply = standalone_reply && LooksSupportiveThreadReply(c);
        const std::string lower_text_for_risk = LowerAscii(c.text);
        if (LooksHardBlockedDirtyOrAbusiveComment(c.text, lower_text_for_risk)) {
            ++skipped_negative;
            ++skipped_hard_block;
            continue;
        }
        if (LooksViewerGatekeepingOrAntiHaterComment(c.text, lower_text_for_risk)) {
            ++skipped_negative;
            ++skipped_audience_fightback;
            continue;
        }
        const int attribution_doubt_risk = DerivativeOrAttributionDoubtRiskScore(c.text, lower_text_for_risk);
        const int cover_compare_risk = CoverOriginalComparisonRiskScore(c.text, lower_text_for_risk);
        const int imitation_praise_score = ImitationRevivalPraiseScore(c.text, lower_text_for_risk);
        const bool imitation_revival_praise = imitation_praise_score >= ((imitation_context && external_imitation_hint >= 4) ? 4 : (imitation_context ? 5 : 6));
        if ((cover_compare_risk >= 5 || (c.from_new_sort && c.likes <= 0 && cover_compare_risk >= 3)) && !imitation_revival_praise) {
            ++skipped_negative;
            ++skipped_cover_compare;
            continue;
        }
        if (cover_compare_risk >= 5 && imitation_revival_praise) {
            ++rescued_imitation_praise;
        }
        if (LooksNegativeOrHostileComment(c.text) ||
            (c.from_new_sort && c.likes <= 0 && attribution_doubt_risk >= 3) ||
            (standalone_reply && !supportive_standalone_reply && !imitation_revival_praise)) {
            ++skipped_negative;
            continue;
        }
        if (!LooksPositiveOrSupportiveComment(c.text) && !supportive_standalone_reply && !imitation_revival_praise) {
            ++skipped_non_positive;
            continue;
        }

        const double like_score = (max_likes > 0)
            ? (std::log1p(static_cast<double>(std::max(0, c.likes))) / std::log1p(static_cast<double>(max_likes)))
            : 0.0;
        const double fresh_score = CommentFreshnessScore(c);
        const double reply_heat = c.threaded ? std::min(1.0, std::log1p(static_cast<double>(std::max(0, c.reply_count))) / std::log(12.0)) : 0.0;
        const double threaded_score = c.threaded ? (0.18 + reply_heat * 0.12) : 0.0;
        const double readable_score = chars >= 8 ? 0.10 : 0.02;
        const double new_bonus = c.from_new_sort ? 0.12 : 0.0;
        const double lang_score = LanguagePriorityScore(preferred_lang, c);
        const double lang_bonus = (preferred_lang.empty() || preferred_lang == "all") ? 0.0 : (lang_score - 0.50) * 0.38;
        const double imitation_bonus = imitation_revival_praise ? std::min(0.28, 0.08 + imitation_praise_score * 0.025) : 0.0;

        // When the preferred-language pool is already healthy, avoid spending slots
        // on very low-signal foreign-language filler.  Keep high-like or threaded
        // foreign comments, because those can still add useful variety.
        // If the preferred-language pool is weak (for example, an overseas-popular
        // anime video with few Japanese comments), do not apply this filter;
        // otherwise the final pool becomes artificially tiny.
        if (strong_preferred_pool && lang_score < 0.50 && !c.threaded) {
            const double like_ratio = max_likes > 0 ? static_cast<double>(std::max(0, c.likes)) / static_cast<double>(max_likes) : 0.0;
            const bool low_signal_foreign = c.likes < std::max(12, max_likes / 12) && like_ratio < 0.10;
            if (low_signal_foreign) {
                ++skipped_low_language;
                continue;
            }
        }

        // If a video has enough positive reactions to judge comment quality,
        // do not show low-like comments even when they are new.  This keeps the
        // fresh-comment lane from becoming a stream of zero-like negative/noisy
        // remarks.  On quiet videos min_likes_for_display remains 0, so we still
        // show the few comments that exist.
        if (min_likes_for_display > 0 && c.likes < min_likes_for_display) continue;

        // For busy videos, also remove zero-like comments that are neither fresh
        // nor part of a useful thread.
        if (abundant && c.likes <= 0 && !c.from_new_sort && fresh_score < 0.50 && !c.threaded) continue;

        c.tier = ComputeTier(c.likes, max_likes, c.from_new_sort);
        const double fresh_like_guard = (c.from_new_sort && c.likes <= 0 && abundant) ? -0.35 : 0.0;
        c.score = like_score * 0.53 + fresh_score * 0.18 + new_bonus + threaded_score + readable_score + lang_bonus + imitation_bonus + fresh_like_guard + jitter(rng) * 0.06;
        candidates.push_back(std::move(c));
    }

    DebugLog("positive-only gate kept=" + std::to_string(candidates.size()) +
             " skipped_negative=" + std::to_string(skipped_negative) +
             " skipped_hard_block=" + std::to_string(skipped_hard_block) +
             " skipped_audience_fightback=" + std::to_string(skipped_audience_fightback) +
             " skipped_cover_compare=" + std::to_string(skipped_cover_compare) +
             " skipped_low_language=" + std::to_string(skipped_low_language) +
             " imitation_context_hits=" + std::to_string(imitation_context_hits) +
             " imitation_context_weight=" + std::to_string(static_cast<int>(imitation_context_weight + 0.5)) +
             " cover_context_hits=" + std::to_string(cover_context_hits) +
             " cover_context_weight=" + std::to_string(static_cast<int>(cover_context_weight + 0.5)) +
             " rescued_imitation_praise=" + std::to_string(rescued_imitation_praise) +
             " skipped_non_positive=" + std::to_string(skipped_non_positive));

    if (candidates.empty()) {
        // Do not backfill with neutral or negative comments. This tool is
        // intentionally allowed to show nothing when no clearly positive and
        // safe comments are available.
        return out;
    }

    auto by_score = [](const Comment& a, const Comment& b) {
        if (a.score != b.score) return a.score > b.score;
        return a.likes > b.likes;
    };

    auto like_ratio_of = [&](const Comment& c) -> double {
        return max_likes > 0 ? static_cast<double>(std::max(0, c.likes)) / static_cast<double>(max_likes) : 0.0;
    };

    std::vector<Comment> popular = candidates;
    std::stable_sort(popular.begin(), popular.end(), by_score);

    // Split candidates into quality lanes.  The final pool is intentionally not
    // all "top comments".  Hot comments remain favored, but good / moderate
    // comments are also kept so the display feels like a natural comment section.
    std::vector<Comment> hot;
    std::vector<Comment> good;
    std::vector<Comment> moderate;
    std::vector<Comment> fresh;
    std::vector<Comment> threaded;
    std::vector<Comment> localized;
    for (const auto& c : candidates) {
        const double r = like_ratio_of(c);
        const double fresh_score_lane = CommentFreshnessScore(c);

        if (c.tier >= 3 || c.likes >= 300 || r >= 0.60) {
            hot.push_back(c);
        } else if (c.tier >= 2 || c.likes >= 50 || r >= 0.28) {
            good.push_back(c);
        } else {
            // This lane is the "ほどよい評価" pool.  It still passes the
            // adaptive minimum-like gate above, so busy videos do not show raw
            // zero-like new comments, but it prevents the pool from being only
            // the same mega-liked classics.
            moderate.push_back(c);
        }

        // Fresh lane is allowed only after the same quality gate.  This keeps
        // the new-comment flavor without filling the screen with unliked remarks.
        if ((c.from_new_sort || fresh_score_lane >= 0.50) && (min_likes_for_display == 0 || c.likes >= min_likes_for_display)) fresh.push_back(c);
        if (c.threaded) threaded.push_back(c);
        if (LanguagePriorityScore(preferred_lang, c) >= 0.75) localized.push_back(c);
    }
    std::stable_sort(hot.begin(), hot.end(), by_score);
    std::stable_sort(good.begin(), good.end(), by_score);
    std::stable_sort(moderate.begin(), moderate.end(), by_score);
    std::stable_sort(fresh.begin(), fresh.end(), by_score);
    std::stable_sort(threaded.begin(), threaded.end(), by_score);
    std::stable_sort(localized.begin(), localized.end(), by_score);

    DebugLog("quality lanes hot=" + std::to_string(hot.size()) +
             " good=" + std::to_string(good.size()) +
             " moderate=" + std::to_string(moderate.size()) +
             " fresh=" + std::to_string(fresh.size()) +
             " threaded=" + std::to_string(threaded.size()) +
             " localized=" + std::to_string(localized.size()));

    std::set<std::string> seen;
    auto add_from = [&](const std::vector<Comment>& src, int quota) {
        for (const auto& c : src) {
            if (static_cast<int>(out.size()) >= limit || quota <= 0) break;
            std::string key = !c.id.empty() ? c.id : c.text;
            if (!seen.insert(key).second) continue;
            out.push_back(c);
            --quota;
        }
    };

    int localized_quota = (preferred_lang.empty() || preferred_lang == "all") ? 0 : std::max(1, limit * 25 / 100);
    int hot_quota       = std::max(1, limit * 35 / 100); // high-rated comments stay prioritized
    int good_quota      = std::max(1, limit * 30 / 100);
    int moderate_quota  = std::max(1, limit * 25 / 100); // avoid overfitting to only mega-liked comments
    int fresh_quota     = std::max(1, limit * 18 / 100); // only quality-gated fresh comments
    int threaded_quota  = std::max(1, limit * 18 / 100);

    if (items.size() < 25) {
        // For small videos, keep selection broad instead of over-filtering.
        localized_quota = (preferred_lang.empty() || preferred_lang == "all") ? 0 : std::max(1, limit * 20 / 100);
        hot_quota       = std::max(1, limit * 35 / 100);
        good_quota      = std::max(1, limit * 35 / 100);
        moderate_quota  = std::max(1, limit * 45 / 100);
        fresh_quota     = std::max(1, limit * 20 / 100);
        threaded_quota  = std::max(1, limit * 22 / 100);
    }

    // Build a mixed pool.  The order below limits the hot lane first, then
    // deliberately reserves room for good / moderate / fresh comments.
    add_from(localized, localized_quota);
    add_from(hot, hot_quota);
    add_from(good, good_quota);
    add_from(threaded, threaded_quota);
    add_from(moderate, moderate_quota);
    add_from(fresh, fresh_quota);

    // Fill any remaining slots, preferring broadly good comments before falling
    // back to hot/popular.  This keeps variety while preserving quality.
    add_from(good, limit);
    add_from(moderate, limit);
    add_from(fresh, limit / 2);
    add_from(threaded, limit / 2);
    add_from(hot, limit);
    add_from(popular, limit);

    return out;
}

std::vector<Comment> ParseRawComments(const std::string& json, bool from_new_sort) {
    std::vector<Comment> raw;
    std::set<std::string> seen_objects;

    size_t key = json.find("\"comments\"");
    if (key == std::string::npos) return raw;
    size_t arr = json.find('[', key);
    if (arr == std::string::npos) return raw;

    int depth = 0;
    bool in_str = false;
    bool esc = false;
    size_t obj_start = std::string::npos;

    for (size_t i = arr + 1; i < json.size(); ++i) {
        char ch = json[i];
        if (in_str) {
            if (esc) esc = false;
            else if (ch == '\\') esc = true;
            else if (ch == '"') in_str = false;
            continue;
        }
        if (ch == '"') { in_str = true; continue; }
        if (ch == '{') {
            if (depth == 0) obj_start = i;
            ++depth;
        } else if (ch == '}') {
            --depth;
            if (depth == 0 && obj_start != std::string::npos) {
                std::string obj = json.substr(obj_start, i - obj_start + 1);
                std::string text;
                if (JsonGetString(obj, "text", text)) {
                    text = CollapseWhitespace(text);
                    if (!text.empty()) {
                        Comment c;
                        c.text = text;
                        JsonGetString(obj, "author", c.author);
                        JsonGetString(obj, "id", c.id);
                        JsonGetString(obj, "parent", c.parent);
                        JsonGetInt(obj, "like_count", c.likes);
                        JsonGetInt(obj, "reply_count", c.reply_count);
                        JsonGetInt64(obj, "timestamp", c.timestamp);
                        c.from_new_sort = from_new_sort;
                        std::string key_text = c.id.empty() ? c.text : c.id;
                        if (seen_objects.insert(key_text).second) raw.push_back(c);
                    }
                }
                obj_start = std::string::npos;
            }
        } else if (ch == ']' && depth == 0) {
            break;
        }
    }
    return raw;
}

std::vector<Comment> ParseComments(const std::string& json, int limit, int max_chars) {
    return BuildDisplayComments(ParseRawComments(json, false), limit, max_chars, "all");
}

bool ReplySuccess(const std::string& reply) {
    std::string err;
    return JsonGetString(reply, "error", err) && err == "success";
}

class MpvIpc {
public:
    ~MpvIpc() { close(); }

    std::wstring pipeName() const { return pipe_name_; }

    bool connectAuto(const Options& opt) {
        close();
        std::vector<std::wstring> names;
        if (!opt.pipe_name.empty()) {
            names.push_back(opt.pipe_name);
        } else {
            names = {L"mpv-tool", L"mpv-pipe", L"mpv-comment-pipe", L"mpvsocket"};
        }
        for (const auto& name : names) {
            if (connectName(name)) {
                pipe_name_ = name;
                return true;
            }
        }
        return false;
    }

    bool valid() const { return h_ != INVALID_HANDLE_VALUE; }

    bool consumeOverlayMaybeLost() {
        bool v = overlay_maybe_lost_;
        overlay_maybe_lost_ = false;
        return v;
    }

    void markOverlayMaybeLost() {
        overlay_maybe_lost_ = true;
    }

    DWORD lastMediaStartTick() const {
        return last_media_start_tick_;
    }

    void close() {
        if (h_ != INVALID_HANDLE_VALUE) {
            CloseHandle(h_);
            h_ = INVALID_HANDLE_VALUE;
        }
        pending_.clear();
    }

    bool getPropertyString(const std::string& prop, std::string& value) {
        int id = next_id_++;
        std::string req = "{\"command\":[\"get_property\",\"" + JsonEscape(prop) + "\"],\"request_id\":" + std::to_string(id) + "}\n";
        std::string reply;
        if (!sendAndRead(req, reply, 1000)) {
            DebugLog("IPC get_property failed: " + prop);
            return false;
        }
        if (!ReplySuccess(reply)) {
            DebugLog("IPC get_property error: " + prop + " reply=" + OneLineForLog(reply));
            return false;
        }
        return JsonGetString(reply, "data", value);
    }

    bool getPropertyDouble(const std::string& prop, double& value, bool quiet = true) {
        int id = next_id_++;
        std::string req = "{\"command\":[\"get_property\",\"" + JsonEscape(prop) + "\"],\"request_id\":" + std::to_string(id) + "}\n";
        std::string reply;
        if (!sendAndRead(req, reply, 1000)) {
            if (!quiet) DebugLog("IPC get_property double failed: " + prop);
            return false;
        }
        if (!ReplySuccess(reply)) {
            if (!quiet) DebugLog("IPC get_property double error: " + prop + " reply=" + OneLineForLog(reply));
            return false;
        }
        return JsonGetDouble(reply, "data", value);
    }

    bool getPropertyBool(const std::string& prop, bool& value, bool quiet = true) {
        int id = next_id_++;
        std::string req = "{\"command\":[\"get_property\",\"" + JsonEscape(prop) + "\"],\"request_id\":" + std::to_string(id) + "}\n";
        std::string reply;
        if (!sendAndRead(req, reply, 1000)) {
            if (!quiet) DebugLog("IPC get_property bool failed: " + prop);
            return false;
        }
        if (!ReplySuccess(reply)) {
            if (!quiet) DebugLog("IPC get_property bool error: " + prop + " reply=" + OneLineForLog(reply));
            return false;
        }
        return JsonGetBool(reply, "data", value);
    }

    bool showText(const std::string& text, int duration_ms) {
        int id = next_id_++;
        std::string req = "{\"command\":[\"show-text\",\"" + JsonEscape(text) + "\"," + std::to_string(duration_ms) + "],\"request_id\":" + std::to_string(id) + "}\n";
        std::string reply;
        if (!sendAndRead(req, reply, 800)) {
            DebugLog("IPC show-text send/read failed");
            return false;
        }
        if (!ReplySuccess(reply)) {
            DebugLog("IPC show-text error reply=" + OneLineForLog(reply));
            return false;
        }
        return true;
    }

    bool setOsdOverlay(const std::string& ass_data, int z = 100) {
        int id = next_id_++;
        std::ostringstream oss;
        oss << "{\"command\":{"
            << "\"name\":\"osd-overlay\","
            << "\"id\":" << kOverlayId << ","
            << "\"format\":\"ass-events\","
            << "\"data\":\"" << JsonEscape(ass_data) << "\","
            << "\"res_x\":" << kAssResX << ","
            << "\"res_y\":" << kAssResY << ","
            << "\"z\":" << z
            << "},\"request_id\":" << id << "}\n";
        std::string reply;
        if (!sendAndRead(oss.str(), reply, 800)) {
            DebugLog("IPC osd-overlay send/read failed");
            return false;
        }
        if (!ReplySuccess(reply)) {
            DebugLog("IPC osd-overlay error reply=" + OneLineForLog(reply));
            return false;
        }
        return true;
    }

    bool removeOsdOverlay() {
        int id = next_id_++;
        std::ostringstream oss;
        oss << "{\"command\":{"
            << "\"name\":\"osd-overlay\","
            << "\"id\":" << kOverlayId << ","
            << "\"format\":\"none\","
            << "\"data\":\"\""
            << "},\"request_id\":" << id << "}\n";
        std::string reply;
        if (!sendAndRead(oss.str(), reply, 800)) {
            DebugLog("IPC remove overlay send/read failed");
            return false;
        }
        if (!ReplySuccess(reply)) {
            DebugLog("IPC remove overlay error reply=" + OneLineForLog(reply));
            return false;
        }
        return true;
    }

private:
    HANDLE h_ = INVALID_HANDLE_VALUE;
    int next_id_ = 1;
    std::wstring pipe_name_;
    std::string pending_;
    bool overlay_maybe_lost_ = false;
    DWORD last_media_start_tick_ = 0;

    bool connectName(const std::wstring& name) {
        std::wstring full = name;
        if (full.rfind(L"\\\\.\\pipe\\", 0) != 0) {
            full = L"\\\\.\\pipe\\" + full;
        }
        HANDLE h = CreateFileW(full.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            DebugLogW(L"IPC connect failed: " + full + L" error=" + std::to_wstring(GetLastError()));
            return false;
        }
        DebugLogW(L"IPC connected: " + full);
        DWORD mode = PIPE_READMODE_BYTE;
        SetNamedPipeHandleState(h, &mode, nullptr, nullptr);
        h_ = h;
        overlay_maybe_lost_ = true;
        last_media_start_tick_ = GetTickCount();
        return true;
    }

    bool sendAndRead(const std::string& req, std::string& line, DWORD timeout_ms) {
        line.clear();
        if (h_ == INVALID_HANDLE_VALUE) return false;

        int request_id = -1;
        JsonGetInt(req, "request_id", request_id);

        DWORD written = 0;
        if (!WriteFile(h_, req.data(), static_cast<DWORD>(req.size()), &written, nullptr) || written != req.size()) {
            DebugLog("IPC WriteFile failed error=" + std::to_string(GetLastError()));
            close();
            return false;
        }

        // mpv IPC can emit asynchronous events such as audio-reconfig/video-reconfig
        // at any time. Do not treat those events as replies to our commands.
        return readMatchingReply(line, timeout_ms, request_id);
    }

    bool readOneLine(std::string& line, DWORD timeout_ms, DWORD start_tick) {
        for (;;) {
            size_t nl = pending_.find('\n');
            if (nl != std::string::npos) {
                line = pending_.substr(0, nl);
                pending_.erase(0, nl + 1);
                return true;
            }
            DWORD avail = 0;
            if (!PeekNamedPipe(h_, nullptr, 0, nullptr, &avail, nullptr)) {
                DebugLog("IPC PeekNamedPipe failed error=" + std::to_string(GetLastError()));
                close();
                return false;
            }
            if (avail > 0) {
                char buf[4096];
                DWORD to_read = std::min<DWORD>(avail, sizeof(buf));
                DWORD read = 0;
                if (!ReadFile(h_, buf, to_read, &read, nullptr)) {
                    DebugLog("IPC ReadFile failed error=" + std::to_string(GetLastError()));
                    close();
                    return false;
                }
                if (read > 0) pending_.append(buf, buf + read);
                continue;
            }
            if (GetTickCount() - start_tick >= timeout_ms) return false;
            Sleep(10);
        }
    }

    bool readMatchingReply(std::string& line, DWORD timeout_ms, int request_id) {
        DWORD start = GetTickCount();
        for (;;) {
            std::string candidate;
            if (!readOneLine(candidate, timeout_ms, start)) return false;

            if (request_id < 0) {
                line = candidate;
                return true;
            }

            int reply_id = -1;
            if (JsonGetInt(candidate, "request_id", reply_id) && reply_id == request_id) {
                line = candidate;
                return true;
            }

            // Unsolicited mpv events are normal during playback changes.
            // vf/glsl/AI filter changes often trigger video-reconfig. mpv can
            // recreate the OSD plane at that moment, so remember to re-send our
            // ASS overlay instead of waiting for the next comment.
            std::string event_name;
            if (JsonGetString(candidate, "event", event_name)) {
                if (event_name == "video-reconfig" || event_name == "file-loaded" ||
                    event_name == "playback-restart" || event_name == "start-file") {
                    overlay_maybe_lost_ = true;
                    last_media_start_tick_ = GetTickCount();
                }
            }

            static int skipped_event_logs = 0;
            if (skipped_event_logs < 20) {
                DebugLog("IPC skip async event/reply=" + OneLineForLog(candidate, 180));
                ++skipped_event_logs;
            }
        }
    }
};

std::wstring FindYtDlp(const Options& opt) {
    if (!opt.yt_dlp_path.empty()) return opt.yt_dlp_path;
    std::wstring local = GetExeDir() + L"\\yt-dlp.exe";
    if (FileExistsW(local)) return local;
    return L"yt-dlp.exe";
}

bool RunProcessCaptureUtf8(const std::wstring& command_line, std::string& output, DWORD timeout_ms, const std::atomic<bool>* cancel_flag = nullptr) {
    output.clear();

    std::unique_lock<std::mutex> yt_lock(g_yt_dlp_mutex);


    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    HANDLE read_pipe = nullptr;
    HANDLE write_pipe = nullptr;
    if (!CreatePipe(&read_pipe, &write_pipe, &sa, 0)) {
        DebugLog("CreatePipe failed error=" + std::to_string(GetLastError()));
        return false;
    }
    SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = write_pipe;
    si.hStdError = write_pipe;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi{};
    std::wstring cmd = command_line;
    BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(write_pipe);
    if (!ok) {
        DebugLog("CreateProcessW failed error=" + std::to_string(GetLastError()));
        CloseHandle(read_pipe);
        return false;
    }

    DWORD start = GetTickCount();
    bool timed_out = false;
    bool canceled = false;
    for (;;) {
        DWORD avail = 0;
        if (PeekNamedPipe(read_pipe, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
            char buf[8192];
            DWORD to_read = std::min<DWORD>(avail, sizeof(buf));
            DWORD read = 0;
            if (ReadFile(read_pipe, buf, to_read, &read, nullptr) && read > 0) {
                output.append(buf, buf + read);
            }
            continue;
        }

        DWORD wait = WaitForSingleObject(pi.hProcess, 25);
        if (wait == WAIT_OBJECT_0) {
            for (;;) {
                DWORD more = 0;
                if (!PeekNamedPipe(read_pipe, nullptr, 0, nullptr, &more, nullptr) || more == 0) break;
                char buf[8192];
                DWORD to_read = std::min<DWORD>(more, sizeof(buf));
                DWORD read = 0;
                if (ReadFile(read_pipe, buf, to_read, &read, nullptr) && read > 0) output.append(buf, buf + read);
                else break;
            }
            break;
        }

        if (cancel_flag && cancel_flag->load()) {
            canceled = true;
            DebugLog("yt-dlp canceled");
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 1000);
            break;
        }

        if (GetTickCount() - start >= timeout_ms) {
            timed_out = true;
            DebugLog("yt-dlp timed out");
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 1000);
            break;
        }
    }

    DWORD exit_code = 1;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(read_pipe);

    DebugLog("process exit_code=" + std::to_string(exit_code) +
             " timed_out=" + std::to_string(timed_out ? 1 : 0) +
             " canceled=" + std::to_string(canceled ? 1 : 0) +
             " output_bytes=" + std::to_string(output.size()));
    return !timed_out && !canceled && exit_code == 0 && !output.empty();
}

std::string StripYtdlPrefix(std::string url) {
    const std::string p = "ytdl://";
    if (url.rfind(p, 0) == 0) url.erase(0, p.size());
    return url;
}

bool IsYouTubeUrl(const std::string& url) {
    std::string u = url;
    std::transform(u.begin(), u.end(), u.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return u.find("youtube.com/") != std::string::npos ||
           u.find("youtu.be/") != std::string::npos ||
           u.find("music.youtube.com/") != std::string::npos;
}

std::string ExtractQueryParam(const std::string& url, const std::string& key) {
    std::string pat1 = "?" + key + "=";
    std::string pat2 = "&" + key + "=";
    size_t p = url.find(pat1);
    size_t off = pat1.size();
    if (p == std::string::npos) {
        p = url.find(pat2);
        off = pat2.size();
    }
    if (p == std::string::npos) return "";
    p += off;
    size_t e = url.find_first_of("&#?", p);
    if (e == std::string::npos) e = url.size();
    return url.substr(p, e - p);
}

std::string ExtractYoutuBeId(const std::string& url) {
    std::string lower = url;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    size_t p = lower.find("youtu.be/");
    if (p == std::string::npos) return "";
    p += 9;
    size_t e = url.find_first_of("/?&#", p);
    if (e == std::string::npos) e = url.size();
    return url.substr(p, e - p);
}

std::string NormalizeYouTubeWatchUrl(const std::string& url) {
    std::string id = ExtractQueryParam(url, "v");
    if (id.empty()) id = ExtractYoutuBeId(url);
    if (id.empty()) return url;

    // YouTube Mix / playlist URL such as watch?v=ID&list=RD... can make yt-dlp
    // spend a long time processing the playlist. For comments, always query
    // the single video watch URL only.
    return "https://www.youtube.com/watch?v=" + id;
}

std::vector<Comment> MergeRawComments(std::vector<Comment> a, const std::vector<Comment>& b) {
    std::map<std::string, size_t> index;
    std::vector<Comment> out;
    auto add = [&](const Comment& c) {
        std::string key = !c.id.empty() ? c.id : c.text;
        auto it = index.find(key);
        if (it == index.end()) {
            index[key] = out.size();
            out.push_back(c);
            return;
        }
        Comment& dst = out[it->second];
        dst.likes = std::max(dst.likes, c.likes);
        dst.timestamp = std::max(dst.timestamp, c.timestamp);
        dst.reply_count = std::max(dst.reply_count, c.reply_count);
        dst.from_new_sort = dst.from_new_sort || c.from_new_sort;
        if (dst.author.empty()) dst.author = c.author;
        if (dst.parent.empty()) dst.parent = c.parent;
    };
    for (const auto& c : a) add(c);
    for (const auto& c : b) add(c);
    return out;
}

std::string BuildVideoContextHintFromJson(const std::string& json) {
    // Build a compact context string from video metadata.  This is deliberately
    // generic: it helps detect hybrid "cover + impersonation/revival" channels
    // without hard-coding a specific URL, channel, or artist name.
    std::vector<std::string> parts;
    auto add_field = [&](const char* key, size_t limit = 900) {
        std::string v;
        if (!JsonGetString(json, key, v)) return;
        v = CollapseWhitespace(v);
        if (v.empty()) return;
        if (v.size() > limit) v = v.substr(0, limit);
        parts.push_back(v);
    };
    add_field("title", 300);
    add_field("fulltitle", 300);
    add_field("alt_title", 300);
    add_field("channel", 300);
    add_field("uploader", 300);
    add_field("uploader_id", 180);
    add_field("channel_id", 180);
    add_field("description", 1600);

    std::string out;
    for (const auto& part : parts) {
        if (!out.empty()) out += " | ";
        out += part;
        if (out.size() > 2600) {
            out.resize(2600);
            break;
        }
    }
    return out;
}

std::vector<Comment> FetchRawCommentsWithYtDlpSort(const Options& opt, const std::string& url, const wchar_t* sort_name, int parents, std::string* video_title_out = nullptr, const std::atomic<bool>* cancel_flag = nullptr) {
    std::wstring yt = FindYtDlp(opt);
    std::wstring wurl = Utf8ToWide(url);

    parents = std::max(1, std::min(parents, 300));
    int replies_per_thread = 2;
    int max_replies = parents * replies_per_thread;
    int max_total = parents + max_replies;

    std::wstring args = L" --skip-download --no-cache-dir --no-playlist --dump-single-json --write-comments --extractor-args ";
    args += QuoteArg(std::wstring(L"youtube:comment_sort=") + sort_name + L";max_comments=" +
                     std::to_wstring(max_total) + L"," +
                     std::to_wstring(parents) + L"," +
                     std::to_wstring(max_replies) + L"," +
                     std::to_wstring(replies_per_thread) + L",2");
    args += L" ";
    args += QuoteArg(wurl);

    std::wstring cmd = QuoteArg(yt) + args;
    DebugLogW(std::wstring(L"yt-dlp path: ") + yt);
    DebugLogW(std::wstring(L"yt-dlp target URL: ") + wurl);
    DebugLog("yt-dlp start sort=" + WideToUtf8(sort_name) +
             " parents=" + std::to_string(parents) +
             " max_chars=" + std::to_string(opt.max_chars));
    std::string json;
    bool ok = RunProcessCaptureUtf8(cmd, json, 90000, cancel_flag);
    DebugLog("yt-dlp finished sort=" + WideToUtf8(sort_name) +
             " ok=" + std::to_string(ok ? 1 : 0) +
             " bytes=" + std::to_string(json.size()));
    if (!ok) {
        if (!json.empty()) DebugLog("yt-dlp output head: " + OneLineForLog(json, 4000));
        return {};
    }
    if (video_title_out && video_title_out->empty()) {
        *video_title_out = BuildVideoContextHintFromJson(json);
    }
    auto raw = ParseRawComments(json, std::wstring(sort_name) == L"new");
    DebugLog("raw comments sort=" + WideToUtf8(sort_name) + " count=" + std::to_string(raw.size()));
    if (raw.empty()) DebugLog("json head: " + OneLineForLog(json, 4000));
    return raw;
}

std::vector<Comment> FetchCommentsWithYtDlp(const Options& opt, const std::string& url, const std::atomic<bool>* cancel_flag = nullptr) {
    int max = std::max(1, std::min(opt.max_comments, 300));

    // Two-pass strategy:
    //  - top: stable high-quality comments
    //  - new: fresh comments that would never win by like_count alone
    // The later adaptive scorer mixes them according to the video's comment volume.
    std::string video_title;
    auto top_raw = FetchRawCommentsWithYtDlpSort(opt, url, L"top", max, &video_title, cancel_flag);
    if (cancel_flag && cancel_flag->load()) return {};
    int new_parents = std::max(12, std::min(max, max / 3 + 8));
    auto new_raw = FetchRawCommentsWithYtDlpSort(opt, url, L"new", new_parents, &video_title, cancel_flag);

    auto merged = MergeRawComments(std::move(top_raw), new_raw);
    auto comments = BuildDisplayComments(std::move(merged), max, opt.max_chars, opt.language_priority ? opt.resolved_lang : std::string("all"), video_title);
    DebugLog("parsed comments=" + std::to_string(comments.size()) + " after adaptive mix");
    return comments;
}

std::vector<Comment> FetchQuickSeedCommentsWithYtDlp(const Options& opt, const std::string& url) {
    // Fast seed path for startup: fetch a small top-comment set without replies.
    // This appears much sooner than the full adaptive pass; the full pass runs in
    // the background and replaces the seed list when ready.
    std::wstring yt = FindYtDlp(opt);
    std::wstring wurl = Utf8ToWide(url);
    int parents = std::max(4, std::min(22, opt.max_comments));

    std::wstring args = L" --skip-download --no-cache-dir --no-playlist --dump-single-json --write-comments --extractor-args ";
    args += QuoteArg(std::wstring(L"youtube:comment_sort=top;max_comments=") +
                     std::to_wstring(parents) + L"," +
                     std::to_wstring(parents) + L",0,0,1");
    args += L" ";
    args += QuoteArg(wurl);

    std::wstring cmd = QuoteArg(yt) + args;
    DebugLogW(std::wstring(L"yt-dlp quick seed path: ") + yt);
    DebugLogW(std::wstring(L"yt-dlp quick seed target URL: ") + wurl);
    DebugLog("yt-dlp quick seed start parents=" + std::to_string(parents));

    std::string json;
    bool ok = RunProcessCaptureUtf8(cmd, json, 14000);
    DebugLog("yt-dlp quick seed finished ok=" + std::to_string(ok ? 1 : 0) +
             " bytes=" + std::to_string(json.size()));
    if (!ok) return {};

    std::string video_title = BuildVideoContextHintFromJson(json);
    auto raw = ParseRawComments(json, false);
    DebugLog("quick seed raw comments count=" + std::to_string(raw.size()));
    auto comments = BuildDisplayComments(std::move(raw), std::min(parents, opt.max_comments), opt.max_chars,
                                         opt.language_priority ? opt.resolved_lang : std::string("all"), video_title);
    DebugLog("quick seed display comments=" + std::to_string(comments.size()));
    return comments;
}

struct BackgroundFetchState {
    std::mutex m;
    std::atomic<bool> cancel{false};
    bool running = false;
    bool ready = false;
    std::string url;
    std::vector<Comment> comments;
};

int ComputeDisplayDurationMs(const Comment& c, int line_count, const Options& opt) {
    // Practical subtitle pacing: short comments should not linger, while longer
    // Japanese/CJK comments need enough time to scan. This uses roughly 10-12
    // Japanese characters per second, then clamps to a sane upper bound.
    int chars = static_cast<int>(Utf8CharCount(c.text));
    int ms = 2600 + chars * 90 + line_count * 450;
    if (c.threaded) ms += 900;
    if (c.tier >= 3) ms += 700;
    if (c.tier >= 4) ms += 700;
    ms = std::max(3200, ms);
    ms = std::max(opt.duration_ms, ms);
    ms = std::min(std::max(opt.duration_ms, opt.max_duration_ms), ms);
    return ms;
}


struct AssRect {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
};

int ClampInt(int v, int lo, int hi) {
    return std::max(lo, std::min(hi, v));
}

void PickVisualPositionAnchored(int visual_slot, std::mt19937& rng, int& x, int& y, int& ass_anchor) {
    // Placement base:
    //   an4 = left edge of the text box is x
    //   an6 = right edge of the text box is x
    //   an5 = centered text box
    // Keep a strong edge lane, but mix in top/bottom off-center lanes so the
    // screen does not look mechanically organized.
    std::uniform_int_distribution<int> jy(-28, 28);
    std::uniform_int_distribution<int> small_j(0, 8);
    std::uniform_int_distribution<int> off_j(-34, 34);

    auto left_edge = [&](int inset, int yy) {
        ass_anchor = 4;
        x = inset + small_j(rng);
        y = yy + jy(rng);
    };
    auto right_edge = [&](int inset, int yy) {
        ass_anchor = 6;
        x = kAssResX - inset - small_j(rng);
        y = yy + jy(rng);
    };
    auto centerish = [&](int xx, int yy) {
        ass_anchor = 5;
        x = xx + off_j(rng);
        y = yy + jy(rng);
    };

    if (visual_slot < 0) {
        std::uniform_int_distribution<int> lane(0, 9);
        std::uniform_int_distribution<int> yy(105, 615);
        std::uniform_int_distribution<int> top_y(86, 185);
        std::uniform_int_distribution<int> bottom_y(535, 635);
        std::uniform_int_distribution<int> inset_near(6, 22);
        std::uniform_int_distribution<int> inset_soft(24, 58);
        int l = lane(rng);
        if (l <= 2) left_edge((l == 0) ? inset_near(rng) : inset_soft(rng), yy(rng));
        else if (l <= 5) right_edge((l == 3) ? inset_near(rng) : inset_soft(rng), yy(rng));
        else if (l == 6) centerish(410, top_y(rng));
        else if (l == 7) centerish(870, top_y(rng));
        else if (l == 8) centerish(395, bottom_y(rng));
        else centerish(885, bottom_y(rng));
    } else {
        // 32-slot cycle. Roughly half are true left/right edge anchors, the rest
        // are corners or upper/lower off-center lanes. Avoid the exact middle.
        switch (visual_slot % 32) {
        case 0:  left_edge(6,   138); break;
        case 1:  right_edge(6,  138); break;
        case 2:  left_edge(14,  635); break;
        case 3:  right_edge(14, 635); break;
        case 4:  centerish(390, 108); break;
        case 5:  centerish(890, 112); break;
        case 6:  left_edge(22,  300); break;
        case 7:  right_edge(22, 300); break;
        case 8:  centerish(410, 604); break;
        case 9:  centerish(870, 604); break;
        case 10: left_edge(38,  510); break;
        case 11: right_edge(38, 510); break;
        case 12: left_edge(8,   220); break;
        case 13: right_edge(8,  220); break;
        case 14: centerish(315, 410); break;
        case 15: centerish(965, 410); break;
        case 16: left_edge(55,  112); break;
        case 17: right_edge(55, 112); break;
        case 18: left_edge(12,  585); break;
        case 19: right_edge(12, 585); break;
        case 20: centerish(470, 188); break;
        case 21: centerish(810, 188); break;
        case 22: left_edge(28,  395); break;
        case 23: right_edge(28, 395); break;
        case 24: centerish(330, 548); break;
        case 25: centerish(950, 548); break;
        case 26: left_edge(70,  260); break;
        case 27: right_edge(70, 260); break;
        case 28: centerish(430, 300); break;
        case 29: centerish(850, 300); break;
        case 30: left_edge(18,  470); break;
        default: right_edge(18, 470); break;
        }
    }

    // Keep middle-anchor lanes away from the exact center of the video. Edge
    // anchors are allowed to stay at the edge.
    if (ass_anchor == 5 && x > 520 && x < 760 && y > 250 && y < 470) {
        x += (x < 640) ? -130 : 130;
    }
    x = ClampInt(x, 4, kAssResX - 4);
    y = ClampInt(y, 76, 642);
}

void PickVisualPosition(int visual_slot, std::mt19937& rng, int& x, int& y) {
    int ass_anchor = 5;
    PickVisualPositionAnchored(visual_slot, rng, x, y, ass_anchor);
}

void PickThreadReplyPosition(int anchor_x, int anchor_y, int reply_index, int attempt, std::mt19937& rng, int& x, int& y) {
    // Keep replies visually attached to the parent comment. Replies should
    // read as a continuation of the parent, so every fallback stays below the
    // parent anchor. Horizontal offset may vary, but never jump above it.
    static const int dxs[] = {  42,  72, -42, -72,  116, -116,   48,  -48,  128, -128,   0,   88,  -88,  152, -152, 0 };
    static const int dys[] = {  86, 112,  92, 122,  104,  104,  138,  138,  150,  150,   96, 126, 126, 164, 164, 178 };
    const int n = static_cast<int>(sizeof(dxs) / sizeof(dxs[0]));
    int idx = (reply_index * 3 + attempt) % n;
    std::uniform_int_distribution<int> j(-10, 10);
    x = anchor_x + dxs[idx] + j(rng);
    y = anchor_y + dys[idx] + j(rng);
    x = ClampInt(x, 160, 1120);

    // Clamp after applying the offset, but keep the lower-side relationship.
    // If the parent is near the bottom edge, place the reply at the lowest
    // usable lane instead of allowing the retry logic to move it above.
    const int min_reply_y = std::min(666, anchor_y + 70 + reply_index * 18);
    y = ClampInt(y, min_reply_y, 666);
}

AssRect EstimateAssRectAnchored(int x, int y, int fs, int border, const std::vector<std::string>& lines, int ass_anchor) {
    int max_w_units = 0;
    for (const auto& line : lines) {
        max_w_units = std::max(max_w_units, Utf8DisplayWidth(line));
    }
    // Utf8DisplayWidth counts CJK as 2. Approximate half-em per unit.
    int w = static_cast<int>(max_w_units * fs * 0.58) + border * 6 + 30;
    int h = static_cast<int>(lines.size() * fs * 1.28) + border * 4 + 24;
    w = ClampInt(w, 90, 860);
    h = ClampInt(h, 42, 360);

    AssRect r;
    if (ass_anchor == 4) {          // middle-left: x is text left edge
        r.left = x;
        r.right = x + w;
    } else if (ass_anchor == 6) {   // middle-right: x is text right edge
        r.left = x - w;
        r.right = x;
    } else {                        // middle-center
        r.left = x - w / 2;
        r.right = x + w / 2;
    }
    r.top = y - h / 2;
    r.bottom = y + h / 2;
    return r;
}

AssRect EstimateAssRect(int x, int y, int fs, int border, const std::vector<std::string>& lines) {
    return EstimateAssRectAnchored(x, y, fs, border, lines, 5);
}

void ClampAssPositionToScreenAnchored(int& x, int& y, int fs, int border, const std::vector<std::string>& lines, int ass_anchor) {
    // Do not destroy edge placement.  For an4/an6, x itself is the visible text
    // edge, so only push inward when the normal-size rectangle would clip.
    constexpr int margin = 8;
    AssRect r = EstimateAssRectAnchored(x, y, fs, border, lines, ass_anchor);
    const int w = r.right - r.left;

    if (w > kAssResX - margin * 2) {
        ass_anchor = 5;
        x = kAssResX / 2;
    } else if (ass_anchor == 4) {
        // Preserve left-edge feel.  The left side may be exactly near margin;
        // do not convert it into a center coordinate.
        if (r.left < margin) x += margin - r.left;
        if (r.right > kAssResX - margin) x -= r.right - (kAssResX - margin);
    } else if (ass_anchor == 6) {
        // Preserve right-edge feel.  x remains the right edge of the text box.
        if (r.right > kAssResX - margin) x -= r.right - (kAssResX - margin);
        if (r.left < margin) x += margin - r.left;
    } else {
        if (r.left < margin) x += margin - r.left;
        else if (r.right > kAssResX - margin) x -= r.right - (kAssResX - margin);
    }

    r = EstimateAssRectAnchored(x, y, fs, border, lines, ass_anchor);
    if (r.bottom - r.top > kAssResY - margin * 2) {
        y = kAssResY / 2;
    } else if (r.top < margin) {
        y += margin - r.top;
    } else if (r.bottom > kAssResY - margin) {
        y -= r.bottom - (kAssResY - margin);
    }
}

void ClampAssPositionToScreen(int& x, int& y, int fs, int border, const std::vector<std::string>& lines) {
    ClampAssPositionToScreenAnchored(x, y, fs, border, lines, 5);
}

bool RectsOverlap(const AssRect& a, const AssRect& b, int padding) {
    return !(a.right + padding < b.left || b.right + padding < a.left ||
             a.bottom + padding < b.top || b.bottom + padding < a.top);
}

bool OverlapsAnyRect(const AssRect& r, const std::vector<AssRect>& active_rects, int padding) {
    for (const auto& a : active_rects) {
        if (RectsOverlap(r, a, padding)) return true;
    }
    return false;
}

bool BuildAssOverlay(const Comment& c, const Options& opt, std::mt19937& rng, std::string& ass_out, std::string& display_out, int& display_ms, int visual_slot = -1, const std::vector<AssRect>* active_rects = nullptr, AssRect* rect_out = nullptr, bool* effect_out = nullptr, int* effect_mode_out = nullptr, int* effect_strength_out = nullptr, int* x_out = nullptr, int* y_out = nullptr, bool prefer_thread_anchor = false, int thread_anchor_x = 0, int thread_anchor_y = 0, int thread_reply_index = 0) {
    ass_out.clear();
    display_out = MakeDisplayText(c);
    display_ms = opt.duration_ms;

    if (!IsWithinCharLimit(c.text, opt.max_chars)) {
        DebugLog("skip overlong comment chars=" + std::to_string(Utf8CharCount(c.text)));
        return false;
    }

    // Keep comments away from the very edge so that outline is not clipped.
    int x = 640;
    int y = 360;
    int ass_anchor = 5;
    if (prefer_thread_anchor) {
        ass_anchor = 5;
        PickThreadReplyPosition(thread_anchor_x, thread_anchor_y, thread_reply_index, 0, rng, x, y);
    } else {
        PickVisualPositionAnchored(visual_slot, rng, x, y, ass_anchor);
    }

    const FontPick font = PickFont(c);
    const CommentMood mood = font.mood;

    int fs = 27;
    int border = 2;
    std::string bold = "0";
    int wrap_cols = 42;
    int max_lines = 5;

    if (c.tier >= 4) { fs = 54; border = 4; bold = "1"; wrap_cols = 19; max_lines = 4; }
    else if (c.tier >= 3) { fs = 47; border = 4; bold = "1"; wrap_cols = 23; max_lines = 4; }
    else if (c.tier >= 2) { fs = 38; border = 3; bold = "1"; wrap_cols = 31; max_lines = 4; }
    else if (c.tier >= 1) { fs = 31; border = 3; bold = "0"; wrap_cols = 38; max_lines = 4; }

    fs += font.size_adjust;
    if (font.force_bold) bold = "1";

    int chars = static_cast<int>(Utf8CharCount(c.text));
    if (chars >= 130 && fs > 34) { fs -= 10; wrap_cols += 7; }
    else if (chars >= 95 && fs > 36) { fs -= 6; wrap_cols += 5; }
    else if (chars >= 65 && fs > 44) { fs -= 4; wrap_cols += 3; }

    fs = std::max(24, std::min(fs, 58));

    bool overflow = false;
    auto lines = SplitIntoDisplayLines(display_out, wrap_cols, max_lines, &overflow);
    if (overflow || lines.empty()) {
        // Some highly-liked comments use a large font.  If such a comment is
        // medium-length, the old code rejected it, and the screen could go
        // almost silent when the remaining pool was mostly similar comments.
        // Keep the same placement lanes, but fall back to compact typography.
        const int orig_fs = fs;
        const int orig_border = border;
        const int orig_wrap_cols = wrap_cols;
        const int orig_max_lines = max_lines;
        bool compact_ok = false;

        for (int pass = 0; pass < 4; ++pass) {
            int try_fs = fs;
            int try_border = border;
            int try_wrap = wrap_cols;
            int try_lines = max_lines;

            if (pass == 0) {
                try_fs = std::max(28, orig_fs - 8);
                try_border = std::max(2, orig_border - 1);
                try_wrap = orig_wrap_cols + 6;
                try_lines = std::max(orig_max_lines, 5);
            } else if (pass == 1) {
                try_fs = std::max(26, orig_fs - 13);
                try_border = std::max(2, orig_border - 1);
                try_wrap = orig_wrap_cols + 10;
                try_lines = 5;
            } else if (pass == 2) {
                try_fs = 25;
                try_border = 2;
                try_wrap = std::max(orig_wrap_cols + 14, 42);
                try_lines = 5;
            } else {
                try_fs = 24;
                try_border = 2;
                try_wrap = std::max(orig_wrap_cols + 18, 48);
                try_lines = 6;
            }

            bool compact_overflow = false;
            auto compact_lines = SplitIntoDisplayLines(display_out, try_wrap, try_lines, &compact_overflow);
            if (!compact_overflow && !compact_lines.empty()) {
                fs = try_fs;
                border = try_border;
                wrap_cols = try_wrap;
                max_lines = try_lines;
                lines = std::move(compact_lines);
                overflow = false;
                compact_ok = true;
                DebugLogEvery("compact-fit", "compact-fit comment chars=" + std::to_string(Utf8CharCount(c.text)) +
                              " fs=" + std::to_string(fs) +
                              " wrap=" + std::to_string(wrap_cols) +
                              " lines=" + std::to_string(static_cast<int>(lines.size())), 10000);
                break;
            }
        }

        if (!compact_ok) {
            DebugLogEvery("skip-lines", "skip comment: would exceed overlay lines chars=" + std::to_string(Utf8CharCount(c.text)), 5000);
            return false;
        }
    }

    ClampAssPositionToScreenAnchored(x, y, fs, border, lines, ass_anchor);
    AssRect chosen_rect = EstimateAssRectAnchored(x, y, fs, border, lines, ass_anchor);
    if (active_rects && !active_rects->empty() && OverlapsAnyRect(chosen_rect, *active_rects, 10)) {
        bool placed = false;
        for (int attempt = 1; attempt <= 36; ++attempt) {
            int tx = x;
            int ty = y;
            int trial_anchor = ass_anchor;
            if (prefer_thread_anchor) {
                trial_anchor = 5;
                PickThreadReplyPosition(thread_anchor_x, thread_anchor_y, thread_reply_index, attempt, rng, tx, ty);
            } else if (attempt <= 32) {
                PickVisualPositionAnchored((visual_slot < 0 ? attempt : visual_slot + attempt), rng, tx, ty, trial_anchor);
            } else {
                PickVisualPositionAnchored(-1, rng, tx, ty, trial_anchor);
            }
            ClampAssPositionToScreenAnchored(tx, ty, fs, border, lines, trial_anchor);
            AssRect trial = EstimateAssRectAnchored(tx, ty, fs, border, lines, trial_anchor);
            int pad = (attempt <= 12) ? 14 : (attempt <= 24 ? 8 : 2);
            if (!OverlapsAnyRect(trial, *active_rects, pad)) {
                x = tx;
                y = ty;
                ass_anchor = trial_anchor;
                chosen_rect = trial;
                placed = true;
                break;
            }
        }

        if (!placed) {
            // Dense-scene fallback: when several long-lived comments already occupy
            // the screen, the normal rectangle sometimes cannot find a clean lane.
            // Do not allow overlap, but retry once with slightly smaller typography
            // and looser wrapping so the scheduler does not fall into a silent loop.
            int dense_fs = std::max(23, fs - (fs >= 44 ? 8 : 5));
            int dense_border = std::max(2, border - 1);
            int dense_wrap = wrap_cols + (fs >= 44 ? 10 : 7);
            int dense_max_lines = std::min(6, max_lines + 1);
            bool dense_overflow = false;
            auto dense_lines = SplitIntoDisplayLines(display_out, dense_wrap, dense_max_lines, &dense_overflow);
            if (!dense_overflow && !dense_lines.empty()) {
                for (int attempt = 1; attempt <= 56; ++attempt) {
                    int tx = x;
                    int ty = y;
                    int trial_anchor = ass_anchor;
                    if (prefer_thread_anchor) {
                        trial_anchor = 5;
                        PickThreadReplyPosition(thread_anchor_x, thread_anchor_y, thread_reply_index, attempt + 36, rng, tx, ty);
                    } else if (attempt <= 48) {
                        PickVisualPositionAnchored((visual_slot < 0 ? attempt + 36 : visual_slot + attempt + 36), rng, tx, ty, trial_anchor);
                    } else {
                        PickVisualPositionAnchored(-1, rng, tx, ty, trial_anchor);
                    }
                    ClampAssPositionToScreenAnchored(tx, ty, dense_fs, dense_border, dense_lines, trial_anchor);
                    AssRect trial = EstimateAssRectAnchored(tx, ty, dense_fs, dense_border, dense_lines, trial_anchor);
                    if (!OverlapsAnyRect(trial, *active_rects, 0)) {
                        x = tx;
                        y = ty;
                        ass_anchor = trial_anchor;
                        fs = dense_fs;
                        border = dense_border;
                        wrap_cols = dense_wrap;
                        max_lines = dense_max_lines;
                        lines = std::move(dense_lines);
                        chosen_rect = trial;
                        placed = true;
                        DebugLogEvery("compact-place", "compact-place comment chars=" + std::to_string(Utf8CharCount(c.text)) +
                                      " fs=" + std::to_string(fs) +
                                      " wrap=" + std::to_string(wrap_cols) +
                                      " lines=" + std::to_string(static_cast<int>(lines.size())), 10000);
                        break;
                    }
                }
            }
        }

        if (!placed) {
            DebugLogEvery("skip-overlap", "skip comment: no non-overlap area chars=" + std::to_string(Utf8CharCount(c.text)), 3000);
            return false;
        }
    }

    const std::string prefix = DisplayPrefix(c);
    const std::string main_color = MainTextAssColor(c);
    const std::string prefix_color = PrefixAssColor(c);
    const std::string emoji_color = EmojiAssColor(c);

    std::string ass_text;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i) ass_text += "\\N";
        const std::string& line = lines[i];
        if (i == 0 && line.rfind(prefix, 0) == 0) {
            ass_text += "{\\c" + prefix_color + "}" + ColorizeEmojiAssText(prefix, prefix_color, emoji_color);
            ass_text += "{\\c" + main_color + "}" + ColorizeEmojiAssText(line.substr(prefix.size()), main_color, emoji_color);
        } else {
            ass_text += ColorizeEmojiAssText(line, main_color, emoji_color);
        }
    }

    display_ms = ComputeDisplayDurationMs(c, static_cast<int>(lines.size()), opt);

    EffectMode effect_mode = opt.enable_shake ? DecideEffectMode(c, mood) : EffectMode::None;
    int effect_strength = EffectStrengthFor(c, effect_mode);
    bool use_effect = (effect_mode != EffectMode::None);

    // Burst reactions such as "最高！！" should feel like a quick pop/reaction,
    // not a long readable subtitle. Keep them short and punchy.
    if (effect_mode == EffectMode::BurstQuake) {
        int burst_ms = 1700 + std::min(chars, 40) * 35;
        display_ms = std::min(display_ms, std::max(1900, std::min(3300, burst_ms)));
    }

    if (effect_out) *effect_out = use_effect;
    if (effect_mode_out) *effect_mode_out = static_cast<int>(effect_mode);
    if (effect_strength_out) *effect_strength_out = effect_strength;
    if (x_out) *x_out = (chosen_rect.left + chosen_rect.right) / 2;
    if (y_out) *y_out = (chosen_rect.top + chosen_rect.bottom) / 2;

    std::ostringstream ass;
    ass << "{\\an" << ass_anchor << "\\pos(" << x << "," << y << ")"
        << "\\fn" << font.face
        << "\\fs" << fs
        << "\\b" << bold
        << "\\bord" << border
        << "\\shad1"
        << "\\c" << main_color
        << "\\3c&H202020&"
        << "\\4c&H000000&"
        << "\\3a&H38&"
        << "\\4a&HA8&"
        << "}"
        << ass_text;
    ass_out = ass.str();
    if (rect_out) *rect_out = chosen_rect;
    return true;
}


Options ParseOptions() {
    Options opt;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return opt;
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        auto starts = [&](const wchar_t* p) { return a.rfind(p, 0) == 0; };
        if (starts(L"--pipe=")) opt.pipe_name = a.substr(7);
        else if (starts(L"--yt-dlp=")) opt.yt_dlp_path = a.substr(9);
        else if (starts(L"--interval=")) opt.interval_ms = std::max(2000, _wtoi(a.substr(11).c_str()) * 1000);
        else if (starts(L"--duration=")) opt.duration_ms = std::max(1000, _wtoi(a.substr(11).c_str()));
        else if (starts(L"--max-duration=")) opt.max_duration_ms = std::max(3000, _wtoi(a.substr(15).c_str()));
        else if (starts(L"--max-comments=")) opt.max_comments = std::max(1, _wtoi(a.substr(15).c_str()));
        else if (starts(L"--max-chars=")) opt.max_chars = std::max(20, _wtoi(a.substr(12).c_str()));
        else if (starts(L"--url=")) opt.url_override = WideToUtf8(a.substr(6));
        else if (a == L"--debug") opt.debug = true;
        else if (a == L"--no-debug") opt.debug = false;
        else if (starts(L"--lang=")) opt.preferred_lang = NormalizeLanguageCode(WideToUtf8(a.substr(7)));
        else if (starts(L"--language=")) opt.preferred_lang = NormalizeLanguageCode(WideToUtf8(a.substr(11)));
        else if (a == L"--no-lang-priority") opt.language_priority = false;
        else if (a == L"--shake") opt.enable_shake = true;
        else if (a == L"--no-shake") opt.enable_shake = false;
        else if (a == L"--rhythm") opt.rhythm_mode = true;
        else if (a == L"--no-rhythm") opt.rhythm_mode = false;
        else if (starts(L"--max-concurrent=")) opt.max_concurrent = std::max(1, std::min(10, _wtoi(a.substr(17).c_str())));
        else if (a == L"--duration-pacing") opt.duration_pacing = true;
        else if (a == L"--no-duration-pacing") opt.duration_pacing = false;
        else if (a == L"--time-comment-sync") opt.time_comment_sync = true;
        else if (a == L"--no-time-comment-sync") opt.time_comment_sync = false;
        else if (starts(L"--time-comment-lead=")) opt.time_comment_lead_sec = std::max(1, std::min(15, _wtoi(a.substr(20).c_str())));
    }
    LocalFree(argv);
    if (!opt.language_priority) {
        opt.resolved_lang = "all";
    } else if (opt.preferred_lang.empty() || opt.preferred_lang == "auto") {
        opt.resolved_lang = GetWindowsUserLanguageCode();
    } else {
        opt.resolved_lang = NormalizeLanguageCode(opt.preferred_lang);
    }
    if (opt.resolved_lang.empty()) opt.resolved_lang = "all";
    return opt;
}

void ToggleThread(HANDLE toggle_event, MpvIpc* ipc) {
    while (!g_quit.load()) {
        DWORD r = WaitForSingleObject(toggle_event, 200);
        if (r == WAIT_OBJECT_0) {
            bool now = !g_enabled.load();
            g_enabled.store(now);
            if (ipc && ipc->valid()) {
                if (!now) ipc->removeOsdOverlay();
                ipc->showText(now ? "YouTube comments: ON" : "YouTube comments: OFF", 2200);
            }
        }
    }
}

bool WaitWithToggle(HANDLE toggle_event, DWORD total_ms, MpvIpc& ipc) {
    DWORD remain = total_ms;
    while (remain > 0 && !g_quit.load()) {
        DWORD step = std::min<DWORD>(remain, 200);
        DWORD r = WaitForSingleObject(toggle_event, step);
        if (r == WAIT_OBJECT_0) {
            bool now = !g_enabled.load();
            g_enabled.store(now);
            if (!now) ipc.removeOsdOverlay();
            ipc.showText(now ? "YouTube comments: ON" : "YouTube comments: OFF", 2200);
            return true;
        }
        remain -= step;
    }
    return false;
}


struct ActiveAssLine {
    std::string ass;
    DWORD start_tick = 0;
    DWORD expire_tick = 0;
    AssRect rect;
    bool effect = false;
    int effect_mode = 0;
    int effect_strength = 0;
    int x = 0;
    int y = 0;
};

struct PendingThreadComment {
    Comment comment;
    DWORD due_tick = 0;
    int anchor_x = 0;
    int anchor_y = 0;
    int reply_index = 0;
    int parent_slot = 0;
};

std::vector<AssRect> ActiveRects(const std::vector<ActiveAssLine>& active) {
    std::vector<AssRect> rects;
    rects.reserve(active.size());
    for (const auto& a : active) rects.push_back(a.rect);
    return rects;
}

bool TickReached(DWORD now, DWORD target) {
    return static_cast<LONG>(now - target) >= 0;
}

const char* EffectModeName(int mode) {
    switch (static_cast<EffectMode>(mode)) {
    case EffectMode::BurstQuake: return "burst";
    case EffectMode::SoftPulse: return "pulse";
    default: return "none";
    }
}

std::string BuildRealtimeEffectTags(int mode_int, int strength, DWORD age_ms, int& dx, int& dy) {
    dx = 0;
    dy = 0;
    EffectMode mode = static_cast<EffectMode>(mode_int);
    if (mode == EffectMode::None) return "";

    if (mode == EffectMode::BurstQuake) {
        if (age_ms > 1800) return "";
        static const int dxs[] = {0, 12, -10, 15, -14, 10, -8, 6, -5, 4, 0};
        static const int dys[] = {0, -7,  8, -6,   7, -5,  4, -3,  3, 0, 0};
        static const int rots[] = {-11, 9, -13, 12, -9, 8, -6, 5, -3, 0, 0};
        static const int scs[] = {126, 92, 118, 96, 122, 94, 112, 98, 106, 100, 100};
        const int n = static_cast<int>(sizeof(dxs) / sizeof(dxs[0]));
        int i = static_cast<int>((age_ms / 55) % n);
        int amp = std::max(8, std::min(18, strength));
        dx = dxs[i] * amp / 10;
        dy = dys[i] * amp / 10;
        int rot = rots[i] * amp / 10;
        int scale = scs[i];
        std::ostringstream ss;
        ss << "\\frz" << rot << "\\fscx" << scale << "\\fscy" << scale;
        return ss.str();
    }

    if (age_ms > 1500) return "";

    // Softer pulse for playful comments that are not pure burst reactions.
    static const int dxs[] = {0, 3, -2, 2, -1, 0};
    static const int dys[] = {0, -2, 2, -1, 1, 0};
    static const int scs[] = {105, 98, 103, 99, 102, 100};
    const int n = static_cast<int>(sizeof(dxs) / sizeof(dxs[0]));
    int i = static_cast<int>((age_ms / 130) % n);
    int amp = std::max(3, std::min(8, strength));
    dx = dxs[i] * amp / 4;
    dy = dys[i] * amp / 4;
    int scale = scs[i];
    int rot = (i % 2 == 0 ? 2 : -2) * amp / 4;
    std::ostringstream ss;
    ss << "\\frz" << rot << "\\fscx" << scale << "\\fscy" << scale;
    return ss.str();
}

std::string ApplyRealtimeEffectToAss(const ActiveAssLine& line, DWORD now) {
    if (!line.effect || line.effect_mode == 0) return line.ass;

    DWORD age = now - line.start_tick;
    int dx = 0;
    int dy = 0;
    std::string fx_tags = BuildRealtimeEffectTags(line.effect_mode, line.effect_strength, age, dx, dy);

    std::string out = line.ass;
    const std::string pos_key = "\\pos(";
    size_t p = out.find(pos_key);
    if (p != std::string::npos) {
        size_t e = out.find(')', p);
        if (e != std::string::npos) {
            std::ostringstream pos;
            pos << "\\pos(" << (line.x + dx) << "," << (line.y + dy) << ")";
            out.replace(p, e - p + 1, pos.str());
        }
    }

    size_t close = out.find('}');
    if (close != std::string::npos && !fx_tags.empty()) {
        out.insert(close, fx_tags);
    }
    return out;
}

bool HasAnyRealtimeEffect(const std::vector<ActiveAssLine>& active, DWORD now) {
    for (const auto& a : active) {
        if (!a.effect || a.effect_mode == 0) continue;
        DWORD age = now - a.start_tick;
        if (static_cast<EffectMode>(a.effect_mode) == EffectMode::BurstQuake) {
            if (age <= 1850) return true;
        } else if (age <= 1550) {
            return true;
        }
    }
    return false;
}

std::string JoinActiveAssLines(const std::vector<ActiveAssLine>& active, DWORD now) {
    std::string out;
    for (const auto& a : active) {
        if (a.ass.empty()) continue;
        if (!out.empty()) out += "\n";
        out += ApplyRealtimeEffectToAss(a, now);
    }
    return out;
}

bool PruneExpiredAssLines(std::vector<ActiveAssLine>& active, DWORD now) {
    const size_t before = active.size();
    active.erase(std::remove_if(active.begin(), active.end(), [&](const ActiveAssLine& a) {
        return TickReached(now, a.expire_tick);
    }), active.end());
    return active.size() != before;
}

void ClearAssLines(std::vector<ActiveAssLine>& active, MpvIpc& ipc) {
    active.clear();
    ipc.removeOsdOverlay();
}

int AutoMaxConcurrent(size_t comment_count, const Options& opt) {
    if (opt.max_concurrent > 0) return opt.max_concurrent;
    if (comment_count >= 80) return 6;
    if (comment_count >= 55) return 5;
    if (comment_count >= 28) return 4;
    if (comment_count >= 12) return 3;
    return 2;
}

int RandomBetween(std::mt19937& rng, int lo, int hi) {
    if (hi < lo) std::swap(lo, hi);
    std::uniform_int_distribution<int> dist(lo, hi);
    return dist(rng);
}

struct VideoPacingInfo {
    double duration_sec = 0.0;
    double time_pos_sec = 0.0;
    DWORD last_update_tick = 0;
};

bool RefreshVideoPacingInfo(MpvIpc& ipc, VideoPacingInfo& pacing, DWORD now, bool force = false) {
    if (!force && pacing.last_update_tick != 0 && now - pacing.last_update_tick < 5000) return pacing.duration_sec > 0.0;
    pacing.last_update_tick = now;

    double duration = 0.0;
    double pos = 0.0;
    bool got_duration = ipc.getPropertyDouble("duration", duration, true);
    bool got_pos = ipc.getPropertyDouble("time-pos", pos, true);
    if (got_duration && duration > 1.0) pacing.duration_sec = duration;
    if (got_pos && pos >= 0.0) pacing.time_pos_sec = pos;
    return pacing.duration_sec > 0.0;
}

bool IsPlaybackEnded(MpvIpc& ipc, VideoPacingInfo& pacing, DWORD now) {
    // Refresh first. After EOF, mpv can keep eof-reached/idle-active true for
    // a short time even after the user seeks back or repeat playback restarts.
    // If time-pos has clearly moved away from the tail, treat playback as
    // resumed and allow the comment scheduler to draw again.
    RefreshVideoPacingInfo(ipc, pacing, now, true);
    const bool have_time = pacing.duration_sec > 1.0;
    const bool at_tail = have_time && pacing.time_pos_sec >= pacing.duration_sec - 0.35;
    const bool away_from_tail = have_time && pacing.time_pos_sec < pacing.duration_sec - 1.25;

    bool eof = false;
    if (ipc.getPropertyBool("eof-reached", eof, true) && eof) {
        if (away_from_tail) {
            DebugLog("playback resumed after eof flag: duration=" + std::to_string(pacing.duration_sec) +
                     " pos=" + std::to_string(pacing.time_pos_sec));
            return false;
        }
        DebugLog("playback end detected: eof-reached");
        return true;
    }

    bool idle = false;
    if (ipc.getPropertyBool("idle-active", idle, true) && idle) {
        if (away_from_tail) {
            DebugLog("playback resumed after idle flag: duration=" + std::to_string(pacing.duration_sec) +
                     " pos=" + std::to_string(pacing.time_pos_sec));
            return false;
        }
        DebugLog("playback end detected: idle-active");
        return true;
    }

    if (at_tail) {
        DebugLog("playback end detected by time-pos duration=" + std::to_string(pacing.duration_sec) +
                 " pos=" + std::to_string(pacing.time_pos_sec));
        return true;
    }
    return false;
}

bool IsPlaybackPaused(MpvIpc& ipc) {
    bool paused = false;
    return ipc.getPropertyBool("pause", paused, true) && paused;
}

bool IsVideoStartedForCommentDisplay(MpvIpc& ipc, VideoPacingInfo& pacing, DWORD now) {
    // New YouTube URLs can be visible to mpv before the first video frame is
    // actually drawn.  In that loading window the screen may still be black,
    // so allow yt-dlp fetching to continue but hold comment display until mpv
    // has passed its file-loaded/playback-restart burst and time-pos is safely
    // past the black/loading head.
    bool flag = false;
    if (ipc.getPropertyBool("idle-active", flag, true) && flag) return false;
    if (ipc.getPropertyBool("core-idle", flag, true) && flag) return false;
    if (ipc.getPropertyBool("paused-for-cache", flag, true) && flag) return false;

    bool vo_configured = false;
    if (ipc.getPropertyBool("vo-configured", vo_configured, true) && !vo_configured) return false;

    if (!RefreshVideoPacingInfo(ipc, pacing, now, true)) return false;

    DWORD media_start_tick = ipc.lastMediaStartTick();
    if (media_start_tick != 0 && !TickReached(now, media_start_tick + 1200)) return false;

    // Some ytdl/streaming videos report time-pos around 0-1s while the window is
    // still black.  Hold the first comment slightly longer; later comments keep
    // the existing timing unchanged.
    return pacing.time_pos_sec >= 1.60;
}

bool RefreshActiveAssOverlay(MpvIpc& ipc, const std::vector<ActiveAssLine>& active);

bool StopCommentsForPlaybackEnd(std::vector<ActiveAssLine>& active, std::deque<PendingThreadComment>& pending, MpvIpc& ipc, DWORD now) {
    // EOF / idle: stop new comments and delayed replies, but let already
    // displayed comments finish naturally instead of cutting them off.
    pending.clear();

    bool changed = PruneExpiredAssLines(active, now);
    if (!active.empty()) {
        RefreshActiveAssOverlay(ipc, active);
        if (changed) {
            DebugLog("playback end drain: expired overlays remain=" + std::to_string(active.size()));
        } else {
            DebugLog("playback end drain: keeping active overlays=" + std::to_string(active.size()));
        }
        return true;
    }

    ipc.removeOsdOverlay();
    return false;
}

bool HasEnoughUniqueCommentsForStrictNoRepeat(size_t comment_count, const VideoPacingInfo& pacing) {
    // When the pool is rich enough for the video length, repeats are more
    // annoying than useful.  Use a conservative estimate: roughly one unique
    // comment every 2.5 seconds plus a small buffer.  If duration is unknown,
    // treat large pools as sufficient.
    if (comment_count >= 220) return true;
    if (pacing.duration_sec <= 1.0) return comment_count >= 80;

    const double need = std::max(24.0, std::min(360.0, pacing.duration_sec / 2.5 + 12.0));
    return static_cast<double>(comment_count) >= need;
}

double ClampDouble(double v, double lo, double hi) {
    return std::max(lo, std::min(v, hi));
}

double PlaybackProgressRatioForPacing(const VideoPacingInfo& pacing) {
    if (pacing.duration_sec <= 1.0 || pacing.time_pos_sec < 0.0) return 0.0;
    return ClampDouble(pacing.time_pos_sec / pacing.duration_sec, 0.0, 1.0);
}

size_t OpeningWarmupLimitForPacing(size_t comment_count, bool sparse_long_video) {
    if (comment_count == 0) return 0;
    // Sparse quick-seed pools are the most stall-prone: before the full yt-dlp
    // pass finishes, 6-10 good comments on a long video can otherwise schedule
    // a 30-80s gap after only a few bubbles.  Keep just a few more opening
    // comments lively; once the full pool arrives, normal duration pacing wins.
    if (sparse_long_video) {
        if (comment_count <= 10) return std::max<size_t>(3, std::min<size_t>(5, comment_count));
        return std::max<size_t>(2, std::min<size_t>(3, comment_count));
    }
    if (comment_count >= 60) return 4;
    if (comment_count >= 30) return 3;
    return std::max<size_t>(2, std::min<size_t>(3, comment_count));
}

int DurationPacedDelayMs(size_t comment_count, size_t used_count, int active_count, const Options& opt, std::mt19937& rng, const VideoPacingInfo& pacing) {
    if (!opt.duration_pacing || pacing.duration_sec <= 20.0 || comment_count == 0) return -1;

    // Keep the display moving, but avoid spending all comments early.
    // If the video is much longer than the unique positive pool can cover,
    // do not recycle comments.  Stretch the gaps instead so sparse videos stay
    // quiet rather than showing the same comment again and again.
    const double remaining_sec = (pacing.time_pos_sec >= 0.0)
        ? std::max(0.0, pacing.duration_sec - pacing.time_pos_sec)
        : pacing.duration_sec;
    const size_t clamped_used = std::min(used_count, comment_count);
    const size_t remaining_unique = std::max<size_t>(1, comment_count - clamped_used);

    const double whole_video_base = (pacing.duration_sec * 1000.0) / std::max<size_t>(1, comment_count);
    const double remaining_base = (remaining_sec * 1000.0) / (static_cast<double>(remaining_unique) + 1.0);

    // Use remaining-base as a gentle tail guard.  For sparse pools, allow it to
    // become large; that is preferable to duplicate comments.
    double base = whole_video_base * 0.70 + std::min(remaining_base, whole_video_base * 1.65) * 0.30;
    const bool sparse_long_video = whole_video_base > 18000.0;
    if (sparse_long_video) {
        base = whole_video_base * 0.55 + remaining_base * 0.45;
    }

    int min_delay = 900;
    int max_delay = 9000;
    if (comment_count >= 120) { min_delay = 650;  max_delay = 4200; }
    else if (comment_count >= 70) { min_delay = 750;  max_delay = 6500; }
    else if (comment_count >= 30) { min_delay = 1100; max_delay = 9500; }
    else if (comment_count >= 12) { min_delay = 1800; max_delay = 13000; }
    else { min_delay = 2800; max_delay = 18000; }

    // Very sparse long videos should feel quiet, not repetitive.  Permit long
    // gaps so each unique comment can be saved for later in the video, but avoid
    // the 50-60 second silence spikes seen when a small quick-seed pool is used
    // before full extraction finishes.  Keep the cap near the average spacing
    // instead of allowing rare random factors to stretch far beyond it.
    if (sparse_long_video) {
        double sparse_mul = 1.25;
        double sparse_floor = 18000.0;
        if (comment_count >= 30) { sparse_mul = 1.12; sparse_floor = 12000.0; }
        else if (comment_count >= 12) { sparse_mul = 1.18; sparse_floor = 14000.0; }
        else if (comment_count >= 6) { sparse_mul = 1.12; sparse_floor = 14000.0; }
        const int sparse_cap = static_cast<int>(std::min(120000.0, std::max(sparse_floor, whole_video_base * sparse_mul)));
        max_delay = std::max(max_delay, sparse_cap);
    }

    // Near the end, avoid a long final blank section by modestly speeding up.
    // Do not do this for very sparse pools, or the remaining unique comments
    // will be spent too aggressively.
    if (!sparse_long_video && pacing.duration_sec > 60.0 && remaining_sec < std::max(35.0, pacing.duration_sec * 0.18)) {
        base *= 0.72;
        max_delay = std::min(max_delay, comment_count >= 30 ? 6500 : 9500);
    }

    int dice = RandomBetween(rng, 1, 100);
    double factor = 1.0;
    if (dice <= 18) factor = RandomBetween(rng, 70, 88) / 100.0;
    else if (dice <= 78) factor = RandomBetween(rng, 90, 118) / 100.0;
    else if (dice <= 94) factor = RandomBetween(rng, 125, 155) / 100.0;
    else factor = RandomBetween(rng, 165, 205) / 100.0;

    int delay = static_cast<int>(base * factor);
    delay = ClampInt(delay, min_delay, max_delay);

    const int maxc = AutoMaxConcurrent(comment_count, opt);
    if (active_count >= maxc - 1) delay = std::max(delay, 1500);
    else if (active_count == 0 && !sparse_long_video) {
        if (comment_count >= 30) delay = std::min(delay, RandomBetween(rng, 650, 1400));
        else delay = std::min(delay, RandomBetween(rng, min_delay, std::max(min_delay, 2600)));
    }

    // Opening warm-up: the quick seed pass can contain only a few high-quality
    // comments. Duration pacing would otherwise calculate a very long first
    // interval after the first bubble, making the beginning feel empty. Keep
    // only the first few comments lively, then let normal pacing take over.
    // This touches timing only; placement/layout and comment selection stay unchanged.
    const size_t opening_warm_limit = OpeningWarmupLimitForPacing(comment_count, sparse_long_video);
    const double warm_pos_limit = (comment_count >= 30) ? 26.0 : 34.0;
    const bool early_warmup = comment_count >= 3 &&
                              pacing.duration_sec > 40.0 &&
                              pacing.time_pos_sec >= 0.0 && pacing.time_pos_sec < warm_pos_limit &&
                              used_count < opening_warm_limit;
    if (early_warmup && active_count < maxc) {
        int warm_cap = 2800;
        if (comment_count >= 30) warm_cap = 1700;
        else if (comment_count >= 12) warm_cap = 2200;
        else if (sparse_long_video) warm_cap = 4200;
        else warm_cap = 3200;
        if (active_count == 0) warm_cap = std::min(warm_cap, sparse_long_video ? 2200 : 1400);
        const int warm_delay = RandomBetween(rng, 650, warm_cap);
        if (delay > warm_delay) {
            DebugLog("opening warmup gap cap old=" + std::to_string(delay) +
                     " new=" + std::to_string(warm_delay) +
                     " used=" + std::to_string(used_count) +
                     " comments=" + std::to_string(comment_count) +
                     " pos=" + std::to_string(pacing.time_pos_sec) +
                     " sparse_seed=" + std::to_string(sparse_long_video ? 1 : 0));
            delay = warm_delay;
        }
    }

    // Budget guard: keep ordinary comments roughly aligned with playback progress.
    // The first few bubbles are allowed to appear early so the opening does not
    // feel empty, but after that, if the displayed-parent count gets ahead of
    // the video position, stretch the next ordinary gap. Thread replies are
    // counted separately by the caller and therefore do not burn this budget.
    if (!early_warmup && pacing.duration_sec > 60.0 && pacing.time_pos_sec >= 0.0 && comment_count >= 8) {
        const double progress = PlaybackProgressRatioForPacing(pacing);
        double allowance = static_cast<double>(opening_warm_limit);
        if (pacing.time_pos_sec > warm_pos_limit) {
            const double fade = ClampDouble((pacing.time_pos_sec - warm_pos_limit) / 45.0, 0.0, 1.0);
            allowance *= (1.0 - fade);
        }

        const double target_used = progress * static_cast<double>(comment_count) + allowance;
        const double over_budget = static_cast<double>(used_count) - target_used;
        if (over_budget > 0.35) {
            const double avg_ms = (pacing.duration_sec * 1000.0) / std::max<size_t>(1, comment_count);
            double guard_ms = avg_ms * (0.82 + over_budget * 0.72);
            if (comment_count < 40) guard_ms *= 1.16;
            if (remaining_sec < std::max(40.0, pacing.duration_sec * 0.16)) guard_ms *= 0.72;

            double guard_cap = std::max(6500.0, std::min(26000.0, avg_ms * (comment_count < 40 ? 2.35 : 1.85)));

            // Sparse-but-not-empty pools around 30-50 comments have enough material to
            // keep a gentle rhythm, but the old budget guard could still stretch a
            // 6-minute video into 18-25s gaps.  That technically preserves comments,
            // yet it feels like an unintended silence.  Keep only this sparse-mid
            // range from becoming too quiet; larger pools and genuinely tiny pools
            // keep the existing pacing behavior.
            if (comment_count >= 16 && comment_count < 24) {
                // Very sparse-but-usable pools still should not create 18s+
                // dead air on short clips.  With ~16-23 approved comments on a
                // 2-3 minute video there is enough material for a calm rhythm;
                // the old uncapped guard treated this like a nearly empty long
                // video and visibly paused.  Keep long sparse videos quiet, but
                // cap short/mid clips to one gentle beat.
                const double upper = (pacing.duration_sec <= 180.0) ? 10500.0 : 14500.0;
                const double floor = (pacing.duration_sec <= 180.0) ? 7200.0 : 9500.0;
                const double visible_cap = std::max(floor, std::min(upper, avg_ms * 1.32));
                guard_cap = std::min(guard_cap, visible_cap);
            } else if (comment_count >= 24 && comment_count < 30) {
                // 24-29 good comments on a 5-6 minute video is sparse, but not
                // empty.  The previous guard could still create 22-26s gaps,
                // which feels like accidental silence.  Keep the calm rhythm,
                // but cap the visible blank to roughly one quiet beat.
                const double visible_cap = std::max(11000.0, std::min(14500.0, avg_ms * 1.20));
                guard_cap = std::min(guard_cap, visible_cap);
            } else if (comment_count >= 30 && comment_count < 40) {
                // Around 30 comments on a 7-minute video is enough to avoid huge
                // empty zones, but only if we do not spend the pool too early.
                // The previous 14.5s cap fixed mid-video silence but could leave
                // only a held timestamp cue at the end.  Allow a slightly calmer
                // rhythm on long sparse videos so the tail stays populated.
                const double upper = (pacing.duration_sec >= 390.0) ? 17000.0 : 14500.0;
                const double visible_cap = std::max(10500.0, std::min(upper, avg_ms * 1.38));
                guard_cap = std::min(guard_cap, visible_cap);
            } else if (comment_count >= 40 && comment_count < 55) {
                const double visible_cap = std::max(8500.0, std::min(12500.0, avg_ms * 1.35));
                guard_cap = std::min(guard_cap, visible_cap);
            }

            int guarded_delay = static_cast<int>(std::min(guard_cap, std::max<double>(delay, guard_ms)));

            // Do not let the budget guard create a totally empty opening.
            // After the opening window it is allowed to stretch the rhythm, but
            // the sparse-mid cap above prevents visible 20s+ dead air.
            if (guarded_delay > delay) {
                DebugLog("duration budget gap guard old=" + std::to_string(delay) +
                         " new=" + std::to_string(guarded_delay) +
                         " used=" + std::to_string(used_count) +
                         " comments=" + std::to_string(comment_count) +
                         " pos=" + std::to_string(pacing.time_pos_sec) +
                         " duration=" + std::to_string(pacing.duration_sec));
                delay = guarded_delay;
            }
        }
    }

    return delay;
}

int NextRhythmDelayMs(size_t comment_count, size_t used_count, int active_count, const Options& opt, std::mt19937& rng, const VideoPacingInfo& pacing) {
    if (!opt.rhythm_mode) return std::max(500, std::min(opt.interval_ms, 3000));

    int paced = DurationPacedDelayMs(comment_count, used_count, active_count, opt, rng, pacing);
    if (paced > 0) {
        // In duration-pacing mode the interval option is the ordinary rhythm
        // fallback, not a hard ceiling.  Let sparse long videos use longer gaps
        // so comments last until the end instead of being spent early.
        return std::max(300, paced);
    }

    const int maxc = AutoMaxConcurrent(comment_count, opt);
    int dice = RandomBetween(rng, 1, 100);
    int delay = 1400;

    if (comment_count >= 70) {
        if (dice <= 48) delay = RandomBetween(rng, 320, 850);
        else if (dice <= 82) delay = RandomBetween(rng, 900, 1700);
        else if (dice <= 94) delay = RandomBetween(rng, 1800, 3200);
        else delay = RandomBetween(rng, 3300, 5600);
    } else if (comment_count >= 30) {
        if (dice <= 38) delay = RandomBetween(rng, 550, 1200);
        else if (dice <= 78) delay = RandomBetween(rng, 1300, 2400);
        else if (dice <= 94) delay = RandomBetween(rng, 2500, 4300);
        else delay = RandomBetween(rng, 4400, 6500);
    } else if (comment_count >= 12) {
        if (dice <= 32) delay = RandomBetween(rng, 900, 1700);
        else if (dice <= 78) delay = RandomBetween(rng, 1900, 3600);
        else delay = RandomBetween(rng, 3800, 7000);
    } else {
        if (dice <= 25) delay = RandomBetween(rng, 1500, 2800);
        else if (dice <= 82) delay = RandomBetween(rng, 3000, 5600);
        else delay = RandomBetween(rng, 5800, 8500);
    }

    if (active_count >= maxc) delay = std::max(delay, 900);
    else if (active_count == 0 && comment_count >= 30) delay = std::min(delay, RandomBetween(rng, 350, 900));

    if (opt.interval_ms > 0) delay = std::min(delay, std::max(300, opt.interval_ms));
    return std::max(250, delay);
}

bool RefreshActiveAssOverlay(MpvIpc& ipc, const std::vector<ActiveAssLine>& active) {
    if (active.empty()) return ipc.removeOsdOverlay();
    return ipc.setOsdOverlay(JoinActiveAssLines(active, GetTickCount()), 10000);
}

bool ShouldSkipTimestampCommentForNormalDisplay(Comment& c, const VideoPacingInfo& pacing, const Options& opt, std::set<std::string>& timed_used_keys) {
    if (!opt.time_comment_sync || pacing.duration_sec <= 1.0 || pacing.time_pos_sec < 0.0) return false;
    const int cue = EnsureCommentVideoTime(c, pacing.duration_sec);
    if (cue < 0) return false;

    // Timestamp comments are reserved for the dedicated cue scheduler only.
    // This prevents them from appearing late as ordinary random comments.
    std::string key = CommentRepeatKey(c);
    const double pos = pacing.time_pos_sec;
    const double lead = static_cast<double>(std::max(1, opt.time_comment_lead_sec));
    const double target = std::max(0.0, static_cast<double>(cue) - lead);
    const double hard_late = std::max(0.0, static_cast<double>(cue) - 0.75);
    if (pos > hard_late && !key.empty() && timed_used_keys.find(key) == timed_used_keys.end()) {
        timed_used_keys.insert(key);
        DebugLog("drop late timestamp cue cue_sec=" + std::to_string(cue) +
                 " target=" + std::to_string(target) +
                 " pos=" + std::to_string(pos) +
                 " late_by=" + std::to_string(pos - target) +
                 " text=" + OneLineForLog(c.text, 120));
    }
    return true;
}

int FindDueTimestampCommentIndex(std::vector<Comment>& comments, const std::set<std::string>& timed_used_keys, const VideoPacingInfo& pacing, const Options& opt) {
    if (!opt.time_comment_sync || comments.empty() || pacing.duration_sec <= 1.0 || pacing.time_pos_sec < 0.0) return -1;
    const double pos = pacing.time_pos_sec;
    const double lead = static_cast<double>(std::max(1, opt.time_comment_lead_sec));
    int best_index = -1;
    double best_abs = 999999.0;
    for (size_t i = 0; i < comments.size(); ++i) {
        Comment& c = comments[i];
        const int cue = EnsureCommentVideoTime(c, pacing.duration_sec);
        if (cue < 0) continue;
        std::string key = CommentRepeatKey(c);
        if (!key.empty() && timed_used_keys.find(key) != timed_used_keys.end()) continue;
        const double cue_d = static_cast<double>(cue);
        const double target = std::max(0.0, cue_d - lead);
        // Prefer early display.  The window still ends before the referenced
        // playback time, so this cannot appear after the moment it points to.
        const double window_start = std::max(0.0, target - 1.20);
        const double window_end = std::min(cue_d - 0.75, target + 1.00);
        if (window_end < 0.0) continue;
        if (pos >= window_start && pos <= window_end) {
            const double d = std::fabs(pos - target);
            if (d < best_abs) {
                best_abs = d;
                best_index = static_cast<int>(i);
            }
        }
    }
    return best_index;
}


int RunMain(const Options& opt, HANDLE toggle_event) {
    DebugLog("RunMain start interval_ms=" + std::to_string(opt.interval_ms) +
             " duration_ms=" + std::to_string(opt.duration_ms) +
             " max_duration_ms=" + std::to_string(opt.max_duration_ms) +
             " max_comments=" + std::to_string(opt.max_comments) +
             " max_chars=" + std::to_string(opt.max_chars) +
             " shake=" + std::to_string(opt.enable_shake ? 1 : 0) +
             " lang=" + opt.resolved_lang +
             " lang_priority=" + std::to_string(opt.language_priority ? 1 : 0) +
             " rhythm=" + std::to_string(opt.rhythm_mode ? 1 : 0) +
             " max_concurrent=" + std::to_string(opt.max_concurrent) +
             " duration_pacing=" + std::to_string(opt.duration_pacing ? 1 : 0) +
             " time_comment_sync=" + std::to_string(opt.time_comment_sync ? 1 : 0) +
             " time_comment_lead_sec=" + std::to_string(opt.time_comment_lead_sec));
    MpvIpc ipc;
    std::mt19937 rng(static_cast<unsigned int>(GetTickCount()) ^ static_cast<unsigned int>(GetCurrentProcessId()));
    std::vector<Comment> comments;
    std::string current_url;
    size_t next_index = 0;
    int ipc_fail_count = 0;
    std::vector<ActiveAssLine> active_ass;
    std::set<std::string> displayed_comment_keys;
    std::deque<std::string> displayed_comment_order;
    std::set<std::string> used_comment_keys;
    std::set<std::string> timed_comment_used_keys;
    std::deque<PendingThreadComment> pending_thread_comments;
    VideoPacingInfo pacing;
    DWORD next_emit_tick = GetTickCount() + 350;
    DWORD next_anim_tick = GetTickCount() + 160;
    DWORD next_overlay_refresh_tick = GetTickCount() + 1200;
    int visual_slot_counter = 0;
    size_t paced_parent_count = 0; // ordinary/timestamp parent comments; thread replies do not spend pacing budget
    std::shared_ptr<BackgroundFetchState> fetch_state;
    bool full_comments_applied = false;
    bool playback_end_mode = false;
    bool pause_mode = false;
    bool full_fetch_pending = false;
    bool seed_fetch_pending = false;
    bool wait_video_start_mode = false;
    DWORD wait_video_start_log_tick = 0;


    auto cancel_background_fetch = [&]() {
        if (fetch_state && fetch_state->running) {
            fetch_state->cancel.store(true);
            DebugLog("background full comments cancel requested url=" + fetch_state->url);
        }
    };

    auto start_background_fetch = [&](const std::string& url) {
        fetch_state = std::make_shared<BackgroundFetchState>();
        fetch_state->url = url;
        fetch_state->running = true;
        std::shared_ptr<BackgroundFetchState> state = fetch_state;
        Options opt_copy = opt;
        std::string url_copy = url;
        std::thread([state, opt_copy, url_copy]() {
            auto full = FetchCommentsWithYtDlp(opt_copy, url_copy, &state->cancel);
            std::lock_guard<std::mutex> lock(state->m);
            if (!state->cancel.load()) state->comments = std::move(full);
            state->ready = true;
            state->running = false;
        }).detach();
    };

    for (;;) {
        if (g_quit.load()) break;

        if (!ipc.valid()) {
            if (!ipc.connectAuto(opt)) {
                DebugLog("IPC auto connect failed count=" + std::to_string(ipc_fail_count + 1));
                if (++ipc_fail_count > 20) {
                    DebugLog("IPC connect failed too many times; exiting");
                    break;
                }
                WaitForSingleObject(toggle_event, 500);
                continue;
            }
            ipc_fail_count = 0;
        }

        // Toggle handling at top of loop.
        if (WaitForSingleObject(toggle_event, 0) == WAIT_OBJECT_0) {
            bool now = !g_enabled.load();
            g_enabled.store(now);
            if (!now) {
                cancel_background_fetch();
                pending_thread_comments.clear();
                ClearAssLines(active_ass, ipc);
            }
            ipc.showText(now ? "YouTube comments: ON" : "YouTube comments: OFF", 2200);
        }

        std::string path;
        if (!opt.url_override.empty()) {
            path = opt.url_override;
        } else if (!ipc.getPropertyString("path", path)) {
            // During vf/glsl/AI-filter reconfiguration mpv can temporarily report
            // property unavailable even though playback is still alive. Do not
            // treat that as EOF and do not clear or drain the comments. Keep the
            // last known YouTube URL and let the overlay watchdog re-send the OSD.
            DebugLog("failed to get mpv path; checking EOF/reconfig state");
            DWORD now_reconfig = GetTickCount();
            if (!ipc.valid()) {
                WaitWithToggle(toggle_event, 500, ipc);
                continue;
            }
            if (IsPlaybackEnded(ipc, pacing, now_reconfig)) {
                playback_end_mode = true;
                bool draining = StopCommentsForPlaybackEnd(active_ass, pending_thread_comments, ipc, now_reconfig);
                WaitWithToggle(toggle_event, draining ? 90 : 700, ipc);
                continue;
            }
            if (!current_url.empty()) {
                path = current_url;
                ipc.markOverlayMaybeLost();
                DebugLog("path unavailable during filter/video reconfig; keep current YouTube URL");
            } else {
                WaitWithToggle(toggle_event, 500, ipc);
                continue;
            }
        }

        path = StripYtdlPrefix(path);
        std::string raw_path = path;
        path = NormalizeYouTubeWatchUrl(path);
        if (raw_path != path) {
            // The mpv path can remain in a ytdl/playlist form for many polling cycles.
            // Log the normalization only when the normalized destination changes;
            // otherwise debug logs become dominated by identical lines every ~200ms.
            static std::string last_normalized_log_url;
            if (path != last_normalized_log_url) {
                DebugLog("normalized YouTube URL: " + OneLineForLog(path, 500));
                last_normalized_log_url = path;
            }
        }
        if (!IsYouTubeUrl(path)) {
            static std::string last_non_youtube;
            if (path != last_non_youtube) {
                DebugLog("not YouTube path: " + OneLineForLog(path, 500));
                last_non_youtube = path;
            }
            cancel_background_fetch();
            comments.clear();
            pending_thread_comments.clear();
            displayed_comment_keys.clear();
            displayed_comment_order.clear();
            used_comment_keys.clear();
            timed_comment_used_keys.clear();
            current_url.clear();
            paced_parent_count = 0;
            fetch_state.reset();
            full_comments_applied = false;
            full_fetch_pending = false;
            seed_fetch_pending = false;
            wait_video_start_mode = false;
            wait_video_start_log_tick = 0;
            ClearAssLines(active_ass, ipc);
            WaitWithToggle(toggle_event, 3000, ipc);
            continue;
        }

        if (IsPlaybackPaused(ipc) && opt.url_override.empty()) {
            DWORD pause_tick = GetTickCount();
            if (!pause_mode) {
                DebugLog("playback paused; suspend comment fetch/display");
            }
            pause_mode = true;
            pending_thread_comments.clear();
            if (PruneExpiredAssLines(active_ass, pause_tick)) {
                if (!active_ass.empty()) {
                    RefreshActiveAssOverlay(ipc, active_ass);
                } else {
                    ipc.removeOsdOverlay();
                }
                next_overlay_refresh_tick = pause_tick + 600;
            }
            next_emit_tick = pause_tick + 350;
            WaitWithToggle(toggle_event, active_ass.empty() ? 350 : 90, ipc);
            continue;
        }

        if (pause_mode) {
            pause_mode = false;
            next_emit_tick = GetTickCount() + 250;
            next_overlay_refresh_tick = GetTickCount() + 500;
            DebugLog("playback resumed; resume comment fetch/display");
        }

        if (path != current_url) {
            cancel_background_fetch();
            DebugLog("new YouTube URL detected: " + OneLineForLog(path, 500));
            current_url = path;
            comments.clear();
            pending_thread_comments.clear();
            displayed_comment_keys.clear();
            displayed_comment_order.clear();
            used_comment_keys.clear();
            timed_comment_used_keys.clear();
            pacing = VideoPacingInfo{};
            next_index = 0;
            ClearAssLines(active_ass, ipc);
            next_emit_tick = GetTickCount() + 350;
            next_overlay_refresh_tick = GetTickCount() + 600;
            visual_slot_counter = 0;
            paced_parent_count = 0;
            fetch_state.reset();
            full_comments_applied = false;
            playback_end_mode = false;
            full_fetch_pending = false;
            seed_fetch_pending = opt.url_override.empty();
            wait_video_start_mode = opt.url_override.empty();
            wait_video_start_log_tick = 0;

            // For normal mpv playback, do not start our yt-dlp comment extraction
            // while mpv/ytdl_hook is still resolving the YouTube URL or expanding
            // a Mix/playlist.  The comments tool only needs the current single
            // watch URL, so delay fetching until the first frames are actually
            // playing.  This keeps Mix continuous playback usable while avoiding
            // a heavy yt-dlp overlap during loading.
            if (seed_fetch_pending) {
                DebugLog("defer quick seed until video playback start url=" + OneLineForLog(current_url, 500));
                WaitWithToggle(toggle_event, 120, ipc);
                continue;
            }

            comments = FetchQuickSeedCommentsWithYtDlp(opt, current_url);
            ResetCommentVideoTimeScan(comments);
            if (!comments.empty()) {
                std::shuffle(comments.begin(), comments.end(), rng);
                DebugLog("quick seed comments loaded=" + std::to_string(comments.size()));
            } else {
                DebugLog("quick seed comments empty; waiting for background full extraction");
            }

            // Start the heavier adaptive pass only after the quick seed returns.
            // This avoids running two yt-dlp comment extractors at the exact same time.
            start_background_fetch(current_url);
        }

        if (seed_fetch_pending && !current_url.empty()) {
            DWORD seed_tick = GetTickCount();
            if (!IsVideoStartedForCommentDisplay(ipc, pacing, seed_tick)) {
                pending_thread_comments.clear();
                if (!active_ass.empty()) ClearAssLines(active_ass, ipc);
                next_emit_tick = seed_tick + 250;
                if (wait_video_start_log_tick == 0 || TickReached(seed_tick, wait_video_start_log_tick + 2000)) {
                    DebugLog("waiting for video playback start before comment fetch pos=" +
                             std::to_string(pacing.time_pos_sec) +
                             " duration=" + std::to_string(pacing.duration_sec));
                    wait_video_start_log_tick = seed_tick;
                }
                WaitWithToggle(toggle_event, 120, ipc);
                continue;
            }

            seed_fetch_pending = false;
            wait_video_start_mode = false;
            wait_video_start_log_tick = 0;
            next_emit_tick = seed_tick + 220;
            next_overlay_refresh_tick = seed_tick + 500;
            DebugLog("video playback started; begin comment fetch pos=" +
                     std::to_string(pacing.time_pos_sec) +
                     " duration=" + std::to_string(pacing.duration_sec));

            comments = FetchQuickSeedCommentsWithYtDlp(opt, current_url);
            ResetCommentVideoTimeScan(comments);
            if (!comments.empty()) {
                std::shuffle(comments.begin(), comments.end(), rng);
                DebugLog("quick seed comments loaded=" + std::to_string(comments.size()));
            } else {
                DebugLog("quick seed comments empty; waiting for background full extraction");
            }

            if (IsPlaybackPaused(ipc) && opt.url_override.empty()) {
                full_fetch_pending = true;
                next_emit_tick = GetTickCount() + 350;
                DebugLog("paused after quick seed; defer background full extraction");
                WaitWithToggle(toggle_event, 250, ipc);
                continue;
            }
            start_background_fetch(current_url);
        }

        if (full_fetch_pending && !fetch_state && !full_comments_applied && !current_url.empty()) {
            full_fetch_pending = false;
            DebugLog("resume deferred background full extraction");
            start_background_fetch(current_url);
        }

        DWORD playback_check_tick = GetTickCount();
        if (IsPlaybackEnded(ipc, pacing, playback_check_tick)) {
            playback_end_mode = true;
            bool draining = StopCommentsForPlaybackEnd(active_ass, pending_thread_comments, ipc, playback_check_tick);
            WaitWithToggle(toggle_event, draining ? 90 : 500, ipc);
            continue;
        }

        if (playback_end_mode) {
            // The user sought back from EOF or repeat playback restarted on the
            // same URL. The comment pool is still valid, but per-playthrough
            // display state must be reset; otherwise strict no-repeat / drained
            // overlay state can make comments stop appearing until a new URL is
            // loaded.
            playback_end_mode = false;
            pending_thread_comments.clear();
            displayed_comment_keys.clear();
            displayed_comment_order.clear();
            used_comment_keys.clear();
            timed_comment_used_keys.clear();
            ClearAssLines(active_ass, ipc);
            if (!comments.empty()) std::shuffle(comments.begin(), comments.end(), rng);
            next_index = 0;
            paced_parent_count = 0;
            next_emit_tick = GetTickCount() + 250;
            next_overlay_refresh_tick = GetTickCount() + 500;
            wait_video_start_mode = opt.url_override.empty();
            wait_video_start_log_tick = 0;
            DebugLog("playback resumed after end; reset per-playthrough comment display state");
        }

        if (fetch_state && !full_comments_applied && fetch_state->url == current_url) {
            std::vector<Comment> full;
            bool ready = false;
            {
                std::lock_guard<std::mutex> lock(fetch_state->m);
                ready = fetch_state->ready;
                if (ready) full = fetch_state->comments;
            }
            if (ready) {
                bool canceled_fetch = fetch_state->cancel.load();
                if (canceled_fetch) {
                    full_comments_applied = true;
                    DebugLog("background full comments discarded after cancel url=" + fetch_state->url);
                } else if (!full.empty()) {
                    const size_t old_comment_count = comments.size();
                    const size_t old_used_count = used_comment_keys.size();
                    comments = std::move(full);
                    ResetCommentVideoTimeScan(comments);
                    std::shuffle(comments.begin(), comments.end(), rng);
                    next_index = 0;
                    // Do not clear used_comment_keys here.  Comments shown by
                    // the quick seed pass must not reappear immediately after
                    // the full/background extraction replaces the pool.
                    // Keep recent keys as well so the transition is smooth.
                    full_comments_applied = true;

                    // If the quick seed produced only a small handful of displayable
                    // comments, duration pacing may have scheduled a long quiet gap before
                    // the full/background pool becomes available.  Once the full pool
                    // arrives, wake the ordinary rhythm promptly instead of inheriting
                    // that sparse-seed delay.
                    //
                    // Earlier this only handled 1-2 comment quick seeds.  In practice a
                    // 5-10 comment quick seed can still be half-spent while the full
                    // fetch is finishing, leaving a 30-80s gap even though dozens of comments
                    // have just arrived.  Keep this limited to the handoff case only:
                    // small old pool, much larger new pool, and a long pending wait.
                    DWORD now_apply_tick = GetTickCount();
                    const bool small_seed_handoff = old_comment_count > 0 && old_comment_count <= 12 &&
                                                    comments.size() >= old_comment_count + 8;
                    const bool much_larger_full_pool = old_comment_count > 0 &&
                                                       comments.size() >= std::max<size_t>(old_comment_count + 16, old_comment_count * 3);
                    const bool seed_already_started = old_used_count >= 2;
                    const bool nearly_spent_seed = old_comment_count > 0 &&
                                                   (old_used_count >= old_comment_count ||
                                                    (old_used_count + 1 >= old_comment_count) ||
                                                    (small_seed_handoff && old_used_count * 2 + 1 >= old_comment_count));
                    const bool sparse_seed_handoff = old_comment_count > 0 &&
                                                     (old_comment_count <= 2 || old_used_count >= old_comment_count ||
                                                      (small_seed_handoff && nearly_spent_seed) ||
                                                      (small_seed_handoff && much_larger_full_pool && seed_already_started));
                    if (sparse_seed_handoff && !TickReached(now_apply_tick, next_emit_tick)) {
                        DWORD remaining_wait = next_emit_tick - now_apply_tick;
                        if (remaining_wait > 1800) {
                            next_emit_tick = now_apply_tick + 650;
                            DebugLog("background full comments wake emit old_wait_ms=" + std::to_string(remaining_wait) +
                                     " old_comments=" + std::to_string(old_comment_count) +
                                     " new_comments=" + std::to_string(comments.size()) +
                                     " old_used=" + std::to_string(old_used_count) +
                                     " used_no_repeat=" + std::to_string(used_comment_keys.size()));
                        }
                    }

                    DebugLog("background full comments applied=" + std::to_string(comments.size()) +
                             " used_no_repeat=" + std::to_string(used_comment_keys.size()));
                } else {
                    full_comments_applied = true;
                    DebugLog("background full comments empty");
                    if (comments.empty()) {
                    }
                }
            }
        }

        if (comments.empty()) {
            // Full extraction is still running. Keep mpv responsive and do not block playback.
            WaitWithToggle(toggle_event, 250, ipc);
            continue;
        }

        if (!g_enabled.load()) {
            pending_thread_comments.clear();
            ClearAssLines(active_ass, ipc);
            WaitWithToggle(toggle_event, 500, ipc);
            continue;
        }

        DWORD now_tick = GetTickCount();
        if (wait_video_start_mode && opt.url_override.empty()) {
            if (!IsVideoStartedForCommentDisplay(ipc, pacing, now_tick)) {
                pending_thread_comments.clear();
                if (!active_ass.empty()) ClearAssLines(active_ass, ipc);
                next_emit_tick = now_tick + 250;
                if (wait_video_start_log_tick == 0 || TickReached(now_tick, wait_video_start_log_tick + 2000)) {
                    DebugLog("waiting for video playback start before comment display pos=" +
                             std::to_string(pacing.time_pos_sec) +
                             " duration=" + std::to_string(pacing.duration_sec));
                    wait_video_start_log_tick = now_tick;
                }
                WaitWithToggle(toggle_event, 120, ipc);
                continue;
            }
            wait_video_start_mode = false;
            wait_video_start_log_tick = 0;
            next_emit_tick = now_tick + 220;
            next_overlay_refresh_tick = now_tick + 500;
            DebugLog("video playback started; enable comment display pos=" +
                     std::to_string(pacing.time_pos_sec) +
                     " duration=" + std::to_string(pacing.duration_sec));
        }

        RefreshVideoPacingInfo(ipc, pacing, now_tick, opt.time_comment_sync);
        if (PruneExpiredAssLines(active_ass, now_tick)) {
            RefreshActiveAssOverlay(ipc, active_ass);
            next_overlay_refresh_tick = GetTickCount() + 1200;
        }

        if (HasAnyRealtimeEffect(active_ass, now_tick) && TickReached(now_tick, next_anim_tick)) {
            RefreshActiveAssOverlay(ipc, active_ass);
            next_anim_tick = GetTickCount() + 140;
            next_overlay_refresh_tick = GetTickCount() + 1200;
        }

        bool overlay_maybe_lost = ipc.consumeOverlayMaybeLost();
        if (!active_ass.empty() && (overlay_maybe_lost || TickReached(now_tick, next_overlay_refresh_tick))) {
            if (RefreshActiveAssOverlay(ipc, active_ass)) {
                if (overlay_maybe_lost) DebugLog("overlay watchdog: re-sent ASS overlay after video/filter reconfig");
                next_overlay_refresh_tick = GetTickCount() + (overlay_maybe_lost ? 550 : 1200);
            } else {
                DebugLog("overlay watchdog: re-send failed; will retry");
                next_overlay_refresh_tick = GetTickCount() + 500;
            }
        }

        // Keep the delayed reply queue tidy. Reply bubbles that missed their
        // window for a long time are dropped so they do not appear unrelated.
        while (!pending_thread_comments.empty() && TickReached(now_tick, pending_thread_comments.front().due_tick + 12000)) {
            DebugLog("drop stale threaded reply");
            pending_thread_comments.pop_front();
        }

        if (!comments.empty()) {
            const int max_active = AutoMaxConcurrent(comments.size(), opt);
            const int due_timestamp_index = FindDueTimestampCommentIndex(comments, timed_comment_used_keys, pacing, opt);
            const bool timestamp_due_now = due_timestamp_index >= 0;
            const bool pending_reply_due_now = !pending_thread_comments.empty() && TickReached(now_tick, pending_thread_comments.front().due_tick);
            const int active_limit_for_emit = max_active + (timestamp_due_now ? 1 : 0) + (pending_reply_due_now ? 1 : 0);
            if ((timestamp_due_now || pending_reply_due_now || TickReached(now_tick, next_emit_tick)) && static_cast<int>(active_ass.size()) < active_limit_for_emit) {
                if (next_index >= comments.size()) {
                    std::shuffle(comments.begin(), comments.end(), rng);
                    next_index = 0;
                }

                bool displayed = false;
                bool timestamp_attempted = false;
                bool displayed_timestamp_comment = false;
                bool timestamp_mode = timestamp_due_now;
                size_t attempts = 0;
                int held_timestamp_skips = 0;

                // When the screen is already crowded, trying every remaining
                // candidate just burns CPU and may spam placement attempts.
                // Keep normal behavior on an empty screen, but cap per-tick
                // search while existing comments occupy the overlay area.
                const bool screen_crowded_for_emit =
                    !active_ass.empty() &&
                    static_cast<int>(active_ass.size()) >= std::max(1, max_active - 1);
                const size_t placement_attempt_limit = active_ass.empty()
                    ? comments.size()
                    : std::min<size_t>(comments.size(), screen_crowded_for_emit ? 12 : 24);
                const int failed_build_limit = active_ass.empty()
                    ? 18
                    : (screen_crowded_for_emit ? 6 : 10);
                int failed_build_count = 0;

                bool pending_due = pending_reply_due_now;
                while (pending_due || timestamp_mode || attempts < placement_attempt_limit) {
                    Comment c;
                    bool from_thread_queue = false;
                    bool candidate_is_timestamp = false;
                    int thread_anchor_x = 0;
                    int thread_anchor_y = 0;
                    int thread_reply_index = 0;
                    int thread_parent_slot = 0;
                    std::vector<Comment> thread_sequence;

                    if (pending_due) {
                        PendingThreadComment pc = pending_thread_comments.front();
                        pending_thread_comments.pop_front();
                        c = pc.comment;
                        from_thread_queue = true;
                        thread_anchor_x = pc.anchor_x;
                        thread_anchor_y = pc.anchor_y;
                        thread_reply_index = pc.reply_index;
                        thread_parent_slot = pc.parent_slot;
                        pending_due = false;
                    } else if (timestamp_mode) {
                        c = comments[static_cast<size_t>(due_timestamp_index)];
                        timestamp_mode = false;
                        timestamp_attempted = true;
                        candidate_is_timestamp = true;
                        // If a timed cue cannot be placed because the screen is full,
                        // fall back to ordinary comments in the same emit pass.
                        // Keep the crowded-screen cap above, so one blocked cue does not
                        // trigger dozens of placement retries in a single tick.
                        attempts = 0;

                        // Timestamp comments can also be a parent+reply thread.
                        // Keep the cue timing strict, but still split replies so
                        // they appear after the parent and below it like normal
                        // conversation comments.
                        thread_sequence = ExpandThreadCommentForConversation(c);
                        if (thread_sequence.size() >= 2) {
                            c = thread_sequence.front();
                        }
                    } else {
                        // Crowded-screen optimization:
                        // When the overlay is nearly full, short comments are much easier
                        // to fit without overlap.  Keep the normal shuffled order when the
                        // screen is open, but in crowded scenes peek a small window ahead
                        // and pull a compact candidate forward if one is available.
                        if (screen_crowded_for_emit && !comments.empty()) {
                            const size_t base = next_index % comments.size();
                            const size_t scan_limit = std::min<size_t>(comments.size(), placement_attempt_limit + 12);
                            size_t best_short_idx = base;
                            size_t best_any_idx = base;
                            int best_short_chars = 0;
                            int best_any_chars = 0;
                            double best_short_score = -1.0e9;
                            double best_any_score = -1.0e9;
                            bool have_short = false;
                            bool have_any = false;

                            for (size_t scan = 0; scan < scan_limit; ++scan) {
                                const size_t idx = (base + scan) % comments.size();
                                Comment& cand = comments[idx];

                                if (ShouldSkipTimestampCommentForNormalDisplay(cand, pacing, opt, timed_comment_used_keys)) {
                                    continue;
                                }

                                const std::string cand_key = CommentRepeatKey(cand);
                                if (!cand_key.empty() &&
                                    (used_comment_keys.find(cand_key) != used_comment_keys.end() ||
                                     displayed_comment_keys.find(cand_key) != displayed_comment_keys.end())) {
                                    continue;
                                }

                                const int cand_chars = static_cast<int>(Utf8CharCount(cand.text));
                                double compact_score = cand.score + static_cast<double>(cand.tier) * 0.08;
                                if (cand.from_new_sort) compact_score += 0.02;
                                if (cand.threaded) compact_score -= 0.12;

                                if (cand_chars <= 12) compact_score += 0.24;
                                else if (cand_chars <= 24) compact_score += 0.18;
                                else if (cand_chars <= 36) compact_score += 0.10;
                                else if (cand_chars >= 80) compact_score -= 0.22;
                                else if (cand_chars >= 56) compact_score -= 0.10;

                                if (!have_any || compact_score > best_any_score) {
                                    have_any = true;
                                    best_any_score = compact_score;
                                    best_any_idx = idx;
                                    best_any_chars = cand_chars;
                                }
                                if (cand_chars >= 2 && cand_chars <= 36 && (!have_short || compact_score > best_short_score)) {
                                    have_short = true;
                                    best_short_score = compact_score;
                                    best_short_idx = idx;
                                    best_short_chars = cand_chars;
                                }
                            }

                            if (have_short || have_any) {
                                const size_t pick_idx = have_short ? best_short_idx : best_any_idx;
                                const int pick_chars = have_short ? best_short_chars : best_any_chars;
                                if (pick_idx != base) {
                                    std::swap(comments[base], comments[pick_idx]);
                                    DebugLogEvery("crowded-short-pick",
                                                  "crowded screen; prefer compact comment chars=" +
                                                  std::to_string(pick_chars) +
                                                  " active=" + std::to_string(active_ass.size()) +
                                                  "/" + std::to_string(max_active),
                                                  5000);
                                }
                                next_index = base;
                            }
                        }

                        c = comments[next_index++];
                        if (next_index >= comments.size()) {
                            std::shuffle(comments.begin(), comments.end(), rng);
                            next_index = 0;
                        }
                        ++attempts;

                        if (ShouldSkipTimestampCommentForNormalDisplay(c, pacing, opt, timed_comment_used_keys)) {
                            ++held_timestamp_skips;
                            if (held_timestamp_skips <= 8) {
                                DebugLog("hold/skip timestamp comment for pre-cue only text=" + OneLineForLog(c.text, 120));
                            } else if (held_timestamp_skips == 9) {
                                DebugLog("hold/skip timestamp comments suppressed in this emit");
                            }
                            continue;
                        }

                        thread_sequence = ExpandThreadCommentForConversation(c);
                        if (thread_sequence.size() >= 2) {
                            c = thread_sequence.front();
                        }
                    }

                    std::string display_key = CommentRepeatKey(c);
                    if (!from_thread_queue && !display_key.empty()) {
                        // No duplicate visible comments within the current playthrough.
                        // Sparse videos should simply become quieter instead of recycling
                        // the same text.
                        if (used_comment_keys.find(display_key) != used_comment_keys.end()) {
                            if (candidate_is_timestamp) {
                                timed_comment_used_keys.insert(display_key);
                                DebugLog("drop timestamp cue already displayed text=" + OneLineForLog(c.text, 120));
                            }
                            continue;
                        }
                        if (displayed_comment_keys.find(display_key) != displayed_comment_keys.end()) {
                            if (candidate_is_timestamp) {
                                timed_comment_used_keys.insert(display_key);
                                DebugLog("drop timestamp cue already visible/recent text=" + OneLineForLog(c.text, 120));
                            }
                            continue;
                        }
                    }

                    std::string ass;
                    std::string display;
                    int display_ms = opt.duration_ms;
                    int slot = from_thread_queue ? thread_parent_slot : visual_slot_counter++;
                    auto rects = ActiveRects(active_ass);
                    AssRect rect;
                    bool effect_used = false;
                    int effect_mode = 0;
                    int effect_strength = 0;
                    int base_x = 0;
                    int base_y = 0;
                    if (!BuildAssOverlay(c, opt, rng, ass, display, display_ms, slot, &rects, &rect, &effect_used, &effect_mode, &effect_strength, &base_x, &base_y, from_thread_queue, thread_anchor_x, thread_anchor_y, thread_reply_index)) {
                        if (candidate_is_timestamp && !display_key.empty()) {
                            timed_comment_used_keys.insert(display_key);
                            DebugLog("drop timestamp cue after placement fail text=" + OneLineForLog(c.text, 120));
                        }
                        ++failed_build_count;
                        if (!active_ass.empty() && failed_build_count >= failed_build_limit) {
                            DebugLogEvery("crowded-skip",
                                          "screen crowded; postpone comment search active=" +
                                          std::to_string(active_ass.size()) + "/" +
                                          std::to_string(max_active) +
                                          " failed=" + std::to_string(failed_build_count),
                                          3000);
                            break;
                        }
                        continue;
                    }

                    ActiveAssLine line;
                    line.ass = ass;
                    line.start_tick = GetTickCount();
                    line.expire_tick = line.start_tick + static_cast<DWORD>(display_ms);
                    line.rect = rect;
                    line.effect = effect_used;
                    line.effect_mode = effect_mode;
                    line.effect_strength = effect_strength;
                    line.x = base_x;
                    line.y = base_y;
                    active_ass.push_back(line);
                    if (!thread_sequence.empty() && thread_sequence.size() >= 2) {
                        DWORD base_due = line.start_tick;
                        for (size_t ti = 1; ti < thread_sequence.size(); ++ti) {
                            PendingThreadComment pc;
                            pc.comment = thread_sequence[ti];
                            pc.due_tick = base_due + static_cast<DWORD>(900 + ti * 1150 + RandomBetween(rng, 0, 450));
                            pc.anchor_x = base_x;
                            pc.anchor_y = base_y;
                            pc.reply_index = static_cast<int>(ti);
                            pc.parent_slot = slot;
                            pending_thread_comments.push_back(std::move(pc));
                        }
                        DebugLog("thread conversation scheduled replies=" + std::to_string(thread_sequence.size() - 1));
                    }
                    if (!display_key.empty()) {
                        if (candidate_is_timestamp) timed_comment_used_keys.insert(display_key);
                        used_comment_keys.insert(display_key);
                        displayed_comment_keys.insert(display_key);
                        displayed_comment_order.push_back(display_key);
                        const bool strict_recent = HasEnoughUniqueCommentsForStrictNoRepeat(comments.size(), pacing);
                        size_t recent_limit2 = 16;
                        if (strict_recent) {
                            recent_limit2 = std::max<size_t>(8, std::min<size_t>(96, std::max<size_t>(comments.size() / 2, 16)));
                        } else {
                            // Recent-key window is now only a short transition guard;
                            // used_comment_keys above enforces no duplicate display.
                            recent_limit2 = std::max<size_t>(3, std::min<size_t>(32, std::max<size_t>(comments.size() / 3, 4)));
                            if (comments.size() > 2) recent_limit2 = std::min<size_t>(recent_limit2, comments.size() - 1);
                        }
                        while (displayed_comment_order.size() > recent_limit2) {
                            displayed_comment_keys.erase(displayed_comment_order.front());
                            displayed_comment_order.pop_front();
                        }
                    }

                    if (candidate_is_timestamp && c.video_time_sec >= 0) {
                        displayed_timestamp_comment = true;
                        const double target_show_time = std::max(0.0, static_cast<double>(c.video_time_sec) - static_cast<double>(std::max(1, opt.time_comment_lead_sec)));
                        DebugLog("display pre-cue timestamp comment cue_sec=" + std::to_string(c.video_time_sec) +
                                 " target=" + std::to_string(target_show_time) +
                                 " pos=" + std::to_string(pacing.time_pos_sec) +
                                 " diff=" + std::to_string(pacing.time_pos_sec - target_show_time) +
                                 " text=" + OneLineForLog(c.text, 120));
                    }

                    DebugLog("display rhythm comment likes=" + std::to_string(c.likes) +
                             " tier=" + std::to_string(c.tier) +
                             " score=" + std::to_string(c.score) +
                             " chars=" + std::to_string(Utf8CharCount(c.text)) +
                             " ms=" + std::to_string(display_ms) +
                             " active=" + std::to_string(active_ass.size()) + "/" + std::to_string(max_active) +
                             " used=" + std::to_string(used_comment_keys.size()) +
                             " paced=" + std::to_string(paced_parent_count + (from_thread_queue ? 0 : 1)) +
                             " strict_nr=" + std::to_string(HasEnoughUniqueCommentsForStrictNoRepeat(comments.size(), pacing) ? 1 : 0) +
                             " slot=" + std::to_string(slot % 16) +
                             " fresh=" + std::to_string(c.from_new_sort ? 1 : 0) +
                             " threaded=" + std::to_string(c.threaded ? 1 : 0) +
                             " grouped_reply=" + std::to_string(from_thread_queue ? 1 : 0) +
                             " lang=" + c.lang +
                             " mood=" + std::string(MoodName(DetectCommentMood(c))) +
                             " effect=" + std::string(EffectModeName(effect_mode)) +
                             " rect=" + std::to_string(rect.left) + "," + std::to_string(rect.top) + "-" + std::to_string(rect.right) + "," + std::to_string(rect.bottom));

                    if (!RefreshActiveAssOverlay(ipc, active_ass)) {
                        DebugLog("osd-overlay failed; fallback to show-text");
                        ipc.showText(display, display_ms);
                        next_overlay_refresh_tick = GetTickCount() + 500;
                    } else {
                        next_overlay_refresh_tick = GetTickCount() + 1200;
                    }
                    if (!from_thread_queue) ++paced_parent_count;
                    displayed = true;
                    break;
                }

                if (!displayed) {
                    if (HasEnoughUniqueCommentsForStrictNoRepeat(comments.size(), pacing) && !used_comment_keys.empty()) {
                        DebugLogEvery("no-unique", "no displayable unique comments available without repeat used=" +
                                      std::to_string(used_comment_keys.size()) +
                                      " comments=" + std::to_string(comments.size()), 5000);
                    } else {
                        DebugLogEvery("no-displayable", "no displayable comments after max_chars/line filtering", 5000);
                    }
                    DWORD retry_delay = timestamp_attempted ? 120 : 900;
                    if (!timestamp_attempted && !comments.empty() && used_comment_keys.size() >= comments.size()) {
                        retry_delay = 6000;
                    }
                    if (!active_ass.empty()) {
                        DWORD soonest = active_ass.front().expire_tick;
                        for (const auto& a : active_ass) {
                            if (TickReached(soonest, a.expire_tick)) soonest = a.expire_tick;
                        }
                        DWORD now2 = GetTickCount();
                        if (!TickReached(now2, soonest)) {
                            retry_delay = std::min<DWORD>(1200, std::max<DWORD>(300, soonest - now2 + 80));
                        }
                    }
                    if (!pending_thread_comments.empty()) {
                        DWORD now3 = GetTickCount();
                        if (!TickReached(now3, pending_thread_comments.front().due_tick)) {
                            DWORD until_reply = pending_thread_comments.front().due_tick - now3;
                            retry_delay = std::max<DWORD>(retry_delay, std::min<DWORD>(1200, std::max<DWORD>(250, until_reply)));
                        } else {
                            retry_delay = std::min<DWORD>(retry_delay, 250);
                        }
                    }
                    next_emit_tick = GetTickCount() + retry_delay;
                } else {
                    int gap = NextRhythmDelayMs(comments.size(), paced_parent_count, static_cast<int>(active_ass.size()), opt, rng, pacing);

                    // Startup bridge: before the full background extraction is applied,
                    // the quick seed can be extremely small after quality / safety filters
                    // (for example 1-2 displayable comments).  Duration pacing then may
                    // schedule a 30s+ wait even though the full pool is still being fetched,
                    // causing the familiar opening silence.  Keep only this handoff window
                    // lively; once the full pool is applied, normal pacing takes over.
                    if (!full_comments_applied && fetch_state && !comments.empty() &&
                        comments.size() <= 10 && pacing.time_pos_sec >= 0.0 &&
                        pacing.time_pos_sec < 45.0 && paced_parent_count < std::min<size_t>(6, comments.size())) {
                        int bridge_cap = 3800;
                        if (comments.size() <= 2) bridge_cap = 2200;
                        else if (comments.size() <= 4) bridge_cap = 3000;
                        if (gap > bridge_cap) {
                            DebugLog("startup quick-seed bridge gap cap old=" + std::to_string(gap) +
                                     " new=" + std::to_string(bridge_cap) +
                                     " comments=" + std::to_string(comments.size()) +
                                     " used=" + std::to_string(paced_parent_count) +
                                     " pos=" + std::to_string(pacing.time_pos_sec));
                            gap = bridge_cap;
                        }
                    }
                    if (displayed_timestamp_comment) {
                        // A pre-cue timestamp comment is an extra timed event, not
                        // part of the ordinary rhythm. Do not let it push the next
                        // normal comment several seconds later, or overall comment
                        // frequency appears to drop.
                        gap = std::min(gap, 650);
                    }
                    if (!pending_thread_comments.empty()) {
                        DWORD now3 = GetTickCount();
                        if (!TickReached(now3, pending_thread_comments.front().due_tick)) {
                            DWORD until_reply = pending_thread_comments.front().due_tick - now3;
                            gap = std::min(gap, static_cast<int>(std::max<DWORD>(250, until_reply)));
                        } else {
                            gap = std::min(gap, 280);
                        }
                    }
                    next_emit_tick = GetTickCount() + static_cast<DWORD>(gap);
                    DebugLog("next rhythm gap_ms=" + std::to_string(gap));
                }
            } else if (static_cast<int>(active_ass.size()) >= max_active) {
                // The screen is full enough. Re-check soon so that a new comment can enter
                // as soon as one of the current overlays expires.
                next_emit_tick = std::min(next_emit_tick, GetTickCount() + 350);
            }
            WaitWithToggle(toggle_event, 90, ipc);
        } else {
            WaitWithToggle(toggle_event, 500, ipc);
        }
    }

    DebugLog("RunMain exiting");
    ipc.removeOsdOverlay();
    return 0;
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    Options opt = ParseOptions();
    g_debug_enabled.store(opt.debug);
    DebugLog("==== mpv_yt_comments start ====");

    HANDLE mutex = CreateMutexW(nullptr, TRUE, kMutexName);
    bool already_running = (GetLastError() == ERROR_ALREADY_EXISTS);

    if (already_running) {
        DebugLog("already running; sending toggle event");
        HANDLE ev = OpenEventW(EVENT_MODIFY_STATE, FALSE, kToggleEventName);
        if (ev) {
            SetEvent(ev);
            CloseHandle(ev);
            DebugLog("toggle event sent");
        } else {
            DebugLog("OpenEvent failed error=" + std::to_string(GetLastError()));
        }
        if (mutex) CloseHandle(mutex);
        return 0;
    }

    HANDLE toggle_event = CreateEventW(nullptr, FALSE, FALSE, kToggleEventName);
    if (!toggle_event) {
        DebugLog("CreateEvent failed error=" + std::to_string(GetLastError()));
        if (mutex) CloseHandle(mutex);
        return 1;
    }

    int ret = RunMain(opt, toggle_event);

    g_quit.store(true);
    CloseHandle(toggle_event);
    if (mutex) {
        ReleaseMutex(mutex);
        CloseHandle(mutex);
    }
    DebugLog("==== mpv_yt_comments end ret=" + std::to_string(ret) + " ====");
    return ret;
}
