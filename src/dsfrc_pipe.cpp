// dsfrc_pipe.cpp
// DirectShow -> rawvideo pipe for mpv (portable-friendly)
// Key change vs v23:
//   - DO NOT rely on "Color Space Converter" connection (it may hang/fail on some graphs).
//   - Connect finalOut -> RawSink directly, accepting ANY uncompressed video subtype.
//   - If --out=bgra, RawSink converts common YUV/RGB formats to BGRA on CPU and writes tight frames to stdout.
//   - If --out=nv12, RawSink passes NV12 or converts I420/YV12 to NV12 (limited support); otherwise warns.
//
// Recommended first test:
//   .\dsfrc_pipe.exe "C:\dsfrc_pipe\test.mp4" --frc=on --out=bgra --size=1920x1080 2> dsfrc.log ^
//   | mpv --no-config --demuxer=lavf --demuxer-lavf-format=rawvideo ^
//       --demuxer-lavf-o-add=video_size=1920x1080 ^
//       --demuxer-lavf-o-add=pixel_format=bgra ^
//       --demuxer-lavf-o-add=framerate=48000/1001 -
//
// Build (VS2022 x64):
// Build example:
//   cl /utf-8 /std:c++17 /EHsc dsfrc_pipe.cpp /Fe:dsfrc_pipe.exe ^
//     strmiids.lib ole32.lib oleaut32.lib uuid.lib

#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <tlhelp32.h>

#include <dshow.h>
#include <dvdmedia.h>
#include <amvideo.h>
#include <uuids.h>
#include <cmath>
#include <chrono>
#include <cwctype>
#include <cwchar>
#include <atomic>
#include <cstdio>
#include <cstdarg>
#include <cstdint>
#include <string>
#include <vector>
#include <io.h>
#include <fcntl.h>
#include <algorithm>
#include <cwctype>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

static FILE* g_log = nullptr;
static bool  g_log_enabled = false; // default OFF; enable with log=yes
static std::atomic<bool> g_stop_requested{false};

// Kill other running instances of dsfrc_pipe.exe so that
// 1) we never accumulate many helper processes, and
// 2) the newest invocation is always the active one.
// Similar strategy is used in mpvCapGC_60_YUV to avoid multiple
// capture helpers running at once.
static void kill_other_dsfrc_instances()
{
    DWORD selfPid = GetCurrentProcessId();
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap == INVALID_HANDLE_VALUE)
        return;

    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);
    if (!Process32FirstW(hSnap, &pe)) {
        CloseHandle(hSnap);
        return;
    }

    const wchar_t* target = L"dsfrc_pipe.exe";

    do {
        if (pe.th32ProcessID == selfPid)
            continue;
        if (_wcsicmp(pe.szExeFile, target) == 0) {
            HANDLE hProc = OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_INFORMATION, FALSE, pe.th32ProcessID);
            if (hProc) {
                // Try not to be too rude: only kill clearly orphaned helpers if possible.
                // But in practice this will also clean up any still-running older helpers.
                TerminateProcess(hProc, 0);
                CloseHandle(hProc);
            }
        }
    } while (Process32NextW(hSnap, &pe));

    CloseHandle(hSnap);
}


static BOOL WINAPI console_ctrl_handler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
        g_stop_requested.store(true);
        return TRUE;
    }
    return FALSE;
}


static void log_line(const char* fmt, ...) {
    if (!g_log) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    fputc('\n', g_log);
    fflush(g_log);
    va_end(ap);
}

static void log_line(const wchar_t* s) {
    if (!g_log) return;
    fwprintf(g_log, L"%s\n", s);
    fflush(g_log);
}

static void logf(const wchar_t* fmt, ...) {
    if (!g_log) return;
    va_list ap;
    va_start(ap, fmt);
    vfwprintf(g_log, fmt, ap);
    va_end(ap);
    fflush(g_log);
}

static void log_hr(const wchar_t* what, HRESULT hr) {
    if (!g_log) return;
    fwprintf(g_log, L"[hr] %s = 0x%08X\n", what, (unsigned)hr);
    fflush(g_log);
}

// Always-visible console printing (for usage / fatal errors even when log is off)
static void print_err(const wchar_t* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfwprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}
// --- TS audio timestamp normalization (for --audio-file with rawvideo stdin) ---
// Some MPEG-TS captures start at a very large PTS (hours), and when mpv is fed
// rawvideo via stdin and audio via --audio-file="input.ts", mpv may keep silent
// while printing: "delaying audio start ... diff=XXXX".
// To avoid that, for TS-like inputs we remux ONLY the audio stream(s) to a temp
// Matroska audio file whose timestamps are shifted to start at 0, and pass that
// temp file to mpv via --audio-file.
//
// This requires ffmpeg.exe to be available either next to dsfrc_pipe.exe (portable),
// or in PATH. If ffmpeg is not found, behavior falls back to the original file.

static bool file_existsW(const std::wstring& p)
{
    if (p.empty()) return false;
    DWORD a = GetFileAttributesW(p.c_str());
    return (a != INVALID_FILE_ATTRIBUTES) && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static bool ends_with_i(const std::wstring& s, const wchar_t* suf)
{
    if (!suf) return false;
    size_t sl = s.size();
    size_t tl = wcslen(suf);
    if (tl == 0 || sl < tl) return false;
    for (size_t i = 0; i < tl; ++i) {
        wchar_t a = s[sl - tl + i];
        wchar_t b = suf[i];
        if (towlower(a) != towlower(b)) return false;
    }
    return true;
}

static bool is_ts_like_path(const std::wstring& p)
{
    return ends_with_i(p, L".ts") || ends_with_i(p, L".m2ts") || ends_with_i(p, L".mts");
}

static std::wstring find_ffmpeg_nearby(const std::wstring& exeDir)
{
    std::vector<std::wstring> cands;
    if (!exeDir.empty()) {
        cands.push_back(exeDir + L"ffmpeg.exe");
        cands.push_back(exeDir + L"tools\\ffmpeg.exe");
        cands.push_back(exeDir + L"bin\\ffmpeg.exe");
        cands.push_back(exeDir + L"ffmpeg\\bin\\ffmpeg.exe");
    }
    cands.push_back(L"ffmpeg.exe"); // PATH

    for (const auto& c : cands) {
        if (file_existsW(c)) return c;
    }
    return L"";
}

static std::wstring make_temp_audio_path()
{
    wchar_t tmpDir[MAX_PATH] = {0};
    DWORD n = GetTempPathW(MAX_PATH, tmpDir);
    std::wstring dir = (n > 0 && n < MAX_PATH) ? std::wstring(tmpDir, tmpDir + n) : L".\\";
    if (!dir.empty() && (dir.back() != L'\\' && dir.back() != L'/')) dir += L"\\";
    wchar_t name[128];
    DWORD pid = GetCurrentProcessId();
    _snwprintf_s(name, _TRUNCATE, L"dsfrc_audio_%lu.mka", (unsigned long)pid);
    return dir + name;
}

static std::wstring make_temp_audio_path_wav()
{
    wchar_t tmpDir[MAX_PATH] = {0};
    DWORD n = GetTempPathW(MAX_PATH, tmpDir);
    std::wstring dir = (n > 0 && n < MAX_PATH) ? std::wstring(tmpDir, tmpDir + n) : L".\\";
    if (!dir.empty() && (dir.back() != L'\\' && dir.back() != L'/')) dir += L"\\";
    wchar_t name[128];
    DWORD pid = GetCurrentProcessId();
    _snwprintf_s(name, _TRUNCATE, L"dsfrc_audio_%lu.wav", (unsigned long)pid);
    return dir + name;
}

static bool run_ffmpeg_remux_audio_zero(const std::wstring& ffmpegPath, const std::wstring& inPath, std::wstring& outPath)
{
    outPath = make_temp_audio_path();

    // -copyts + -start_at_zero shifts timestamps to start at 0 when stream-copying.
    // -avoid_negative_ts make_zero keeps muxer happy if anything goes negative.
    std::wstring cmd = L"\"";
    cmd += ffmpegPath;
    cmd += L"\" -hide_banner -loglevel error -y -copyts -start_at_zero -i \"";
    cmd += inPath;
    cmd += L"\" -map 0:a -c copy -avoid_negative_ts make_zero \"";
    cmd += outPath;
    cmd += L"\"";

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    HANDLE hWatch = nullptr;

    std::wstring cmdBuf = cmd;
    BOOL ok = CreateProcessW(
        nullptr,
        cmdBuf.data(),
        nullptr,
        nullptr,
        FALSE,
        CREATE_NO_WINDOW,
        nullptr,
        nullptr,
        &si,
        &pi
    );
    if (!ok) {
        log_hr(L"[ts] CreateProcessW(ffmpeg)", HRESULT_FROM_WIN32(GetLastError()));
        return false;
    }

    WaitForSingleObject(pi.hProcess, INFINITE);

    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    if (exitCode != 0 || !file_existsW(outPath)) {
        logf(L"[ts] ffmpeg remux failed (exit=%lu)\n", (unsigned long)exitCode);
        // best-effort cleanup
        if (file_existsW(outPath)) DeleteFileW(outPath.c_str());

        // Fallback: decode audio and re-mux to WAV with timestamps starting at 0.
        // This is larger than stream-copy, but works even when TS timestamps are wild.
        outPath = make_temp_audio_path_wav();

        std::wstring cmd2 = L"\"";
        cmd2 += ffmpegPath;
        cmd2 += L"\" -hide_banner -loglevel error -y -i \"";
        cmd2 += inPath;
        cmd2 += L"\" -map 0:a:0 -vn -sn -dn -af asetpts=PTS-STARTPTS -c:a pcm_s16le -ar 48000 \"";
        cmd2 += outPath;
        cmd2 += L"\"";

        STARTUPINFOW si2{};
        si2.cb = sizeof(si2);
        PROCESS_INFORMATION pi2{};

        std::wstring cmdBuf2 = cmd2;
        BOOL ok2 = CreateProcessW(nullptr, cmdBuf2.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si2, &pi2);
        if (!ok2) {
            log_hr(L"[ts] CreateProcessW(ffmpeg wav)", HRESULT_FROM_WIN32(GetLastError()));
            outPath.clear();
            return false;
        }

        WaitForSingleObject(pi2.hProcess, INFINITE);

        DWORD exit2 = 1;
        GetExitCodeProcess(pi2.hProcess, &exit2);
        CloseHandle(pi2.hThread);
        CloseHandle(pi2.hProcess);

        if (exit2 != 0 || !file_existsW(outPath)) {
            logf(L"[ts] ffmpeg wav fallback failed (exit=%lu)\n", (unsigned long)exit2);
            if (file_existsW(outPath)) DeleteFileW(outPath.c_str());
            outPath.clear();
            return false;
        }

        logf(L"[ts] wav fallback OK: %s\n", outPath.c_str());
        return true;
    }

    return true;
}


/// --- mpv IPC for audio-delay auto tuning ---
static std::wstring g_mpv_ipc_name;
static bool g_mpv_ipc_enabled = false;
static bool g_mpv_audio_delay_sent = false;
static std::chrono::high_resolution_clock::time_point g_t_mpv_launch;

// extra tweak (seconds) applied on top of automatically measured delay.
// can be configured via command line, e.g. --audio-delay-extra=-0.05
static double g_audio_delay_extra = 0.0;

// Remember when input path looks like TS/ISDB/BD transport streams (.ts/.m2ts/.mts).
// Used to apply special decoder policy for terrestrial/BS MPEG-2 recordings.
static bool g_is_ts_like_input = false;

// If we can probe BlueskyFRC's negotiated output fps in a pre-check graph,
// store it here so that mpv's rawvideo framerate can be aligned to it.
static bool g_frc_output_fps_detected = false;
static int  g_frc_output_fps_num = 0;
static int  g_frc_output_fps_den = 1;


// Aspect ratio signal (DAR) from source (4:3 / 16:9 etc.)
// If present, we pass it to mpv via --video-aspect-override for the rawvideo stdin entry.
static bool g_has_aspect_signal = false;
static int  g_aspect_x = 0;
static int  g_aspect_y = 0;
static std::wstring g_mpv_aspect_override;

static bool make_mpv_aspect_override_from_signal(int x, int y, std::wstring& out)
{
    out.clear();
    if (x <= 0 || y <= 0) return false;

    // Many filters report slightly "inflated" ratios for BT.601-coded 720-wide SD
    // (e.g. 15:11 for 4:3 and 20:11 for 16:9). So we match with tolerance and
    // special-case those common SD ratios.
    const double r = (double)x / (double)y;

    auto rel_match = [&](double target) -> bool {
        if (target <= 0.0) return false;
        const double rel = std::fabs(r - target) / target;
        return rel <= 0.03; // ~3% relative tolerance
    };

    // Explicit common SD cases
    if (x == 15 && y == 11) { out = L"4:3";  return true; }
    if (x == 20 && y == 11) { out = L"16:9"; return true; }

    if (rel_match(4.0 / 3.0))  { out = L"4:3";  return true; }
    if (rel_match(16.0 / 9.0)) { out = L"16:9"; return true; }

    return false;
}

static void update_aspect_signal(int x, int y)
{
    g_aspect_x = x;
    g_aspect_y = y;
    g_has_aspect_signal = false;
    g_mpv_aspect_override.clear();

    if (make_mpv_aspect_override_from_signal(x, y, g_mpv_aspect_override)) {
        g_has_aspect_signal = true;
        logf(L"[info] aspect signal detected: %d:%d -> mpv --video-aspect-override=%s\n",
             x, y, g_mpv_aspect_override.c_str());
    } else if (x > 0 && y > 0) {
        // Keep running; just don't force mpv aspect for unknown ratios.
        logf(L"[info] aspect signal detected: %d:%d (unsupported; ignore)\n", x, y);
    }
}



// Send audio-delay to mpv via JSON IPC
static void send_audio_delay_to_mpv(double delay_sec)
{
    if (!g_mpv_ipc_enabled || g_mpv_ipc_name.empty())
        return;
    if (g_mpv_audio_delay_sent)
        return;

    HANDLE hPipe = CreateFileW(
        g_mpv_ipc_name.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        OPEN_EXISTING,
        0,
        nullptr);
    if (hPipe == INVALID_HANDLE_VALUE)
    {
        log_hr(L"[ipc] CreateFileW(pipe) failed", HRESULT_FROM_WIN32(GetLastError()));
        return;
    }

    wchar_t wbuf[256];
    _snwprintf_s(wbuf, _TRUNCATE, L"{\"command\":[\"set\",\"audio-delay\",%.3f]}\n", delay_sec);
    // convert to UTF-8
    int needed = WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 0)
    {
        CloseHandle(hPipe);
        return;
    }
    std::string json;
    json.resize((size_t)needed);
    WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, &json[0], needed, nullptr, nullptr);

    DWORD written = 0;
    if (!WriteFile(hPipe, json.c_str(), (DWORD)json.size(), &written, nullptr))
    {
        log_hr(L"[ipc] WriteFile(pipe) failed", HRESULT_FROM_WIN32(GetLastError()));
        CloseHandle(hPipe);
        return;
    }
    CloseHandle(hPipe);
    g_mpv_audio_delay_sent = true;
    logf(L"[ipc] sent audio-delay=%.3f sec via mpv IPC\n", delay_sec);
}


static HANDLE g_raw_out = INVALID_HANDLE_VALUE;
static bool   g_raw_out_overlapped = false; // true when using an overlapped named pipe
static DWORD  g_raw_write_timeout_ms = 2000; // if mpv stops reading for this long, abort to avoid orphaned dsfrc_pipe

// Write to mpv pipe with a timeout so dsfrc_pipe will not hang forever if mpv
// stops reading (e.g. mpv failed to restart / switched files / got stuck).
// When using an overlapped named pipe, we can time out and abort cleanly.
static bool write_pipe_with_timeout(HANDLE h, const uint8_t* p, DWORD len, DWORD timeoutMs, DWORD& writtenOut)
{
    writtenOut = 0;
    if (!h || h == INVALID_HANDLE_VALUE)
        return false;

    if (!g_raw_out_overlapped)
    {
        DWORD w = 0;
        if (!WriteFile(h, p, len, &w, nullptr))
            return false;
        writtenOut = w;
        return (w == len);
    }

    // Overlapped path (named pipe with FILE_FLAG_OVERLAPPED).
    thread_local HANDLE ev = nullptr;
    thread_local OVERLAPPED ov{};
    if (!ev)
    {
        ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        ZeroMemory(&ov, sizeof(ov));
        ov.hEvent = ev;
    }
    if (!ev)
        return false;

    ResetEvent(ev);

    DWORD w = 0;
    BOOL ok = WriteFile(h, p, len, &w, &ov);
    if (!ok)
    {
        DWORD err = GetLastError();
        if (err != ERROR_IO_PENDING)
            return false;

        DWORD wait = WaitForSingleObject(ev, timeoutMs);
        if (wait != WAIT_OBJECT_0)
        {
            // mpv is not consuming; cancel the I/O and bail out WITHOUT an infinite wait.
            CancelIoEx(h, &ov);
            // Give it a brief moment to complete cancellation, but never block forever.
            WaitForSingleObject(ev, 200);
            // Best effort to drain result (may still be incomplete).
            GetOverlappedResult(h, &ov, &w, FALSE);
            SetLastError(ERROR_TIMEOUT);
            writtenOut = w;
            return false;
        }

        if (!GetOverlappedResult(h, &ov, &w, FALSE))
            return false;
    }

    writtenOut = w;
    return (w == len);
}

// fwrite wrapper: if writing to stdout and g_raw_out is a valid handle,
// forward data to that handle (mpv pipe). Otherwise, fall back to C stdio.
static size_t dsfrc_raw_fwrite(const void* ptr, size_t size, size_t nmemb, FILE* stream)
{
    size_t total = size * nmemb;
    if (!total)
        return 0;

    if (stream == stdout && g_raw_out && g_raw_out != INVALID_HANDLE_VALUE)
    {
        const uint8_t* p = static_cast<const uint8_t*>(ptr);
        size_t remaining = total;
        while (remaining > 0)
        {
            DWORD chunk = (DWORD)(remaining > 0x40000000 ? 0x40000000 : remaining);
            DWORD written = 0;
            if (!write_pipe_with_timeout(g_raw_out, p, chunk, g_raw_write_timeout_ms, written))
            {
                DWORD err = GetLastError();
                if (err == ERROR_TIMEOUT)
                    log_line(L"[err] WriteFile to mpv pipe timed out (mpv not consuming). Requesting graph stop.");
                else
                    log_line(L"[err] WriteFile to mpv pipe failed; requesting graph stop.");
                g_stop_requested.store(true);
                return total - remaining;
            }
            p += written;
            remaining -= written;
        }
        return total;
    }
    return fwrite(ptr, size, nmemb, stream);
}

static bool g_diag_connect = true;// print QueryAccept etc

template<typename T>
struct ComPtr {
    T* p = nullptr;
    ~ComPtr(){ if(p) p->Release(); p=nullptr; }
    T* get() const { return p; }
    T** put(){ if(p){ p->Release(); p=nullptr; } return &p; }
    T* operator->() const { return p; }
    operator bool() const { return p!=nullptr; }
    void reset(T* np=nullptr){ if(p) p->Release(); p=np; }
};

static void FreeMediaType(AM_MEDIA_TYPE& mt) {
    if (mt.cbFormat) {
        CoTaskMemFree(mt.pbFormat);
        mt.cbFormat = 0;
        mt.pbFormat = nullptr;
    }
    if (mt.pUnk) {
        mt.pUnk->Release();
        mt.pUnk = nullptr;
    }
}

static void DeleteMediaType(AM_MEDIA_TYPE* pmt) {
    if (!pmt) return;
    FreeMediaType(*pmt);
    CoTaskMemFree(pmt);
}

static std::wstring guid_to_wstr(const GUID& g){
    wchar_t buf[64]{};
    StringFromGUID2(g, buf, 64);
    return buf;
}
static std::wstring guid_to_string(const GUID& g){ return guid_to_wstr(g); }



static const wchar_t* wcsistr(const wchar_t* hay, const wchar_t* needle){
    if(!hay || !needle) return nullptr;
    size_t nlen = wcslen(needle);
    if(nlen==0) return hay;
    for(const wchar_t* p=hay; *p; ++p){
        size_t i=0;
        while(i<nlen){
            wchar_t c1 = p[i];
            if(!c1) break;
            wchar_t c2 = needle[i];
            if(towlower(c1) != towlower(c2)) break;
            ++i;
        }
        if(i==nlen) return p;
    }
    return nullptr;
}

// ---------- output format ----------
enum class OutFmt { NV12, BGRA };
static const wchar_t* outfmt_name(OutFmt f){ return (f==OutFmt::NV12) ? L"nv12" : L"bgra"; }

// common subtypes
static const GUID GUID_SUBTYPE_I420 = {0x30323449, 0x0000,0x0010,{0x80,0x00,0x00,0xAA,0x00,0x38,0x9B,0x71}}; // 'I420'
static const GUID GUID_SUBTYPE_YV12 = {0x32315659, 0x0000,0x0010,{0x80,0x00,0x00,0xAA,0x00,0x38,0x9B,0x71}}; // 'YV12'
static const GUID MEDIASUBTYPE_YUY2_ = MEDIASUBTYPE_YUY2; // YUY2
static const GUID MEDIASUBTYPE_UYVY_ = MEDIASUBTYPE_UYVY; // UYVY
static const GUID MEDIASUBTYPE_RGB24_ = MEDIASUBTYPE_RGB24;
static const GUID MEDIASUBTYPE_RGB32_ = MEDIASUBTYPE_RGB32;
static const GUID MEDIASUBTYPE_NV12_ = MEDIASUBTYPE_NV12;

// ---------- YUV->RGB helper (BT.601 limited-ish) ----------
static inline uint8_t clip_u8(int v){ return (uint8_t)(v<0?0:(v>255?255:v)); }
static inline void yuv_to_rgb(int y, int u, int v, uint8_t& r, uint8_t& g, uint8_t& b){
    int c = y - 16;
    int d = u - 128;
    int e = v - 128;
    if(c < 0) c = 0;
    int rr = (298*c + 409*e + 128) >> 8;
    int gg = (298*c - 100*d - 208*e + 128) >> 8;
    int bb = (298*c + 516*d + 128) >> 8;
    r = clip_u8(rr);
    g = clip_u8(gg);
    b = clip_u8(bb);
}

// ---------- custom sink filter ----------
class SinkFilter;

class SinkPin final : public IPin, public IMemInputPin {
public:
    SinkPin(SinkFilter* parent, OutFmt outFmt, int forcedW, int forcedH)
        : m_parent(parent), m_outFmt(outFmt), m_forcedW(forcedW), m_forcedH(forcedH) {}

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override { return (ULONG)InterlockedIncrement(&m_ref); }
    STDMETHODIMP_(ULONG) Release() override { ULONG v=(ULONG)InterlockedDecrement(&m_ref); if(!v) delete this; return v; }

    // IPin
    STDMETHODIMP Connect(IPin*, const AM_MEDIA_TYPE*) override { return E_NOTIMPL; }
    STDMETHODIMP ReceiveConnection(IPin* pConnector, const AM_MEDIA_TYPE* pmt) override;
    STDMETHODIMP Disconnect() override;
    STDMETHODIMP ConnectedTo(IPin** ppPin) override;
    STDMETHODIMP ConnectionMediaType(AM_MEDIA_TYPE* pmt) override;
    STDMETHODIMP QueryPinInfo(PIN_INFO* pInfo) override;
    STDMETHODIMP QueryDirection(PIN_DIRECTION* pPinDir) override { if(!pPinDir) return E_POINTER; *pPinDir=PINDIR_INPUT; return S_OK; }
    STDMETHODIMP QueryId(LPWSTR* Id) override;
    STDMETHODIMP QueryAccept(const AM_MEDIA_TYPE* pmt) override;
    STDMETHODIMP EnumMediaTypes(IEnumMediaTypes** ppEnum) override;
    STDMETHODIMP QueryInternalConnections(IPin**, ULONG*) override { return E_NOTIMPL; }
    STDMETHODIMP EndOfStream() override { return S_OK; }
    STDMETHODIMP BeginFlush() override { return S_OK; }
    STDMETHODIMP EndFlush() override { return S_OK; }
    STDMETHODIMP NewSegment(REFERENCE_TIME, REFERENCE_TIME, double) override { return S_OK; }

    // IMemInputPin
    STDMETHODIMP GetAllocator(IMemAllocator** ppAllocator) override;
    STDMETHODIMP NotifyAllocator(IMemAllocator* pAllocator, BOOL bReadOnly) override;
    STDMETHODIMP GetAllocatorRequirements(ALLOCATOR_PROPERTIES* pProps) override { if(pProps) ZeroMemory(pProps,sizeof(*pProps)); return E_NOTIMPL; }
    STDMETHODIMP Receive(IMediaSample* pSample) override;
    STDMETHODIMP ReceiveMultiple(IMediaSample** pSamples, long nSamples, long* nSamplesProcessed) override;
    STDMETHODIMP ReceiveCanBlock() override { return S_FALSE; }

    bool format_ok() const { return m_format_ok; }
    int width() const { return m_w; }
    int height() const { return m_h; }
    GUID subtype() const { return m_mt.subtype; }
    OutFmt outfmt() const { return m_outFmt; }

    HRESULT commit_allocator(){
        if(m_alloc){
            return m_alloc->Commit();
        }
        return S_OK;
    }
    HRESULT decommit_allocator(){
        if(m_alloc){
            return m_alloc->Decommit();
        }
        return S_OK;
    }


private:
    volatile LONG m_ref = 1;
    SinkFilter* m_parent = nullptr;
    OutFmt m_outFmt{OutFmt::BGRA};

    ComPtr<IPin> m_connected;
    AM_MEDIA_TYPE m_mt{};
    bool m_has_mt=false;

    ComPtr<IMemAllocator> m_alloc;
    bool m_readonly=false;

    int m_w=0, m_h=0;
    bool m_format_ok=false;

    int m_forcedW=0, m_forcedH=0;

    void parse_format(const AM_MEDIA_TYPE* pmt);
    void apply_forced_if_needed();
};

class SinkFilter final : public IBaseFilter {
public:
    SinkFilter(OutFmt outFmt, int forcedW, int forcedH): m_pin(new SinkPin(this, outFmt, forcedW, forcedH)) {
        // rawvideo must be binary on Windows
        _setmode(_fileno(stdout), _O_BINARY);
    SetConsoleCtrlHandler(console_ctrl_handler, TRUE);
        // For piping into mpv, avoid stdio buffering delays.
        if(!_isatty(_fileno(stdout))) {
            setvbuf(stdout, nullptr, _IONBF, 0);
        } else {
            setvbuf(stdout, nullptr, _IOFBF, 1<<20);
        }
}

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if(!ppv) return E_POINTER;
        if(riid==IID_IUnknown || riid==IID_IBaseFilter || riid==IID_IMediaFilter || riid==IID_IPersist){
            *ppv = static_cast<IBaseFilter*>(this); AddRef(); return S_OK;
        }
        *ppv=nullptr; return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return (ULONG)InterlockedIncrement(&m_ref); }
    STDMETHODIMP_(ULONG) Release() override { ULONG v=(ULONG)InterlockedDecrement(&m_ref); if(!v) delete this; return v; }

    // IPersist
    STDMETHODIMP GetClassID(CLSID* pClassID) override { if(!pClassID) return E_POINTER; *pClassID=CLSID_NULL; return S_OK; }

    // IMediaFilter
    STDMETHODIMP Stop() override {
        if(m_pin) m_pin->decommit_allocator();
        m_state=State_Stopped;
        return S_OK;
    }
    STDMETHODIMP Pause() override {
        if(m_pin) m_pin->commit_allocator();
        m_state=State_Paused;
        return S_OK;
    }
    STDMETHODIMP Run(REFERENCE_TIME) override {
        if(m_pin) m_pin->commit_allocator();
        m_state=State_Running;
        return S_OK;
    }
    STDMETHODIMP GetState(DWORD, FILTER_STATE* pState) override { if(!pState) return E_POINTER; *pState=m_state; return S_OK; }
    STDMETHODIMP SetSyncSource(IReferenceClock* pClock) override { m_clock.reset(pClock); if(pClock) pClock->AddRef(); return S_OK; }
    STDMETHODIMP GetSyncSource(IReferenceClock** ppClock) override { if(!ppClock) return E_POINTER; *ppClock=m_clock.get(); if(*ppClock) (*ppClock)->AddRef(); return S_OK; }

    // IBaseFilter
    STDMETHODIMP EnumPins(IEnumPins** ppEnum) override;
    STDMETHODIMP FindPin(LPCWSTR Id, IPin** ppPin) override;
    STDMETHODIMP QueryFilterInfo(FILTER_INFO* pInfo) override;
    STDMETHODIMP JoinFilterGraph(IFilterGraph* pGraph, LPCWSTR pName) override {
        m_graph.reset(pGraph); if(pGraph) pGraph->AddRef();
        m_name = pName ? pName : L"RawSink";
        return S_OK;
    }
    STDMETHODIMP QueryVendorInfo(LPWSTR* pVendorInfo) override { if(!pVendorInfo) return E_POINTER; *pVendorInfo=nullptr; return E_NOTIMPL; }

    SinkPin* pin() const { return m_pin; }

    void inc_frame(){
        uint64_t n = ++m_frameCount;
        if(n==1){
            log_line(L"[info] first frame delivered to stdout");
            fflush(stdout);
        }
        if((n % 120) == 0){
            logf(L"[info] frames=%llu\n", (unsigned long long)n);
        }
    }

    HRESULT on_sample(const uint8_t* data, long len, int w, int h, const GUID& subtype){
        if(!data || len<=0 || w<=0 || h<=0) return S_OK;

        // One-time audio-delay configuration.
        // We no longer try to auto-measure wall-clock latency between mpv launch
        // and the first video frame, because that could easily overshoot (especially
        // when ffmpeg remuxing or decoder latency is large) and cause huge A/V drift.
        // Instead, we only send a *fixed* offset if the user explicitly requested it
        // via --audio-delay-extra. The default (0.0) means "do not touch mpv's
        // audio-delay at all" and let mpv handle sync based solely on timestamps.
        if (!m_auto_delay_done && g_mpv_ipc_enabled) {
            m_auto_delay_done = true;
            if (std::abs(g_audio_delay_extra) > 1e-4) {
                send_audio_delay_to_mpv(g_audio_delay_extra);
                logf(L"[ipc] sent fixed audio-delay=%.3f sec via mpv IPC\n", g_audio_delay_extra);
            } else {
                log_line(L"[ipc] audio-delay auto-tuning disabled (no offset applied)");
            }
        }

        if(m_pin->outfmt() == OutFmt::NV12){
            // NV12 pass-through only (tight or with stride repack)
            if(subtype == MEDIASUBTYPE_NV12_){
                const long tight = (long)(w*h*3/2);
                if(len == tight){
                    if(dsfrc_raw_fwrite(data,1,(size_t)len,stdout)!=(size_t)len) return E_FAIL;
                    return S_OK;
                }
                long stride = (long)(((__int64)len*2) / ((__int64)h*3));
                if(stride < w) stride = w;
                long expect = (long)((__int64)stride*h*3/2);
                if (expect > (long)len) {
                log_line("on_sample: expect>len (w=%d h=%d expect=%ld got=%zu) -> drop", w, h, expect, len);
                return S_OK;
            }
                const uint8_t* y = data;
                const uint8_t* uv = data + (size_t)stride * (size_t)h;
                for(int row=0; row<h; ++row){
                    if(dsfrc_raw_fwrite(y + (size_t)row*stride, 1, (size_t)w, stdout)!=(size_t)w) return E_FAIL;
                }
                for(int row=0; row<h/2; ++row){
                    if(dsfrc_raw_fwrite(uv + (size_t)row*stride, 1, (size_t)w, stdout)!=(size_t)w) return E_FAIL;
                }
                return S_OK;
            }
            logf(L"[warn] --out=nv12 but incoming subtype=%s not supported; try --out=bgra\n", guid_to_wstr(subtype).c_str());
            return S_OK;
        }

        // BGRA output: convert common formats to tight BGRA
        const size_t outSize = (size_t)w * (size_t)h * 4;
        if(m_tmp.size() != outSize) m_tmp.assign(outSize, 0);

        uint8_t* out = m_tmp.data();

        // RGB32/BGRA pass or repack
        if(subtype == MEDIASUBTYPE_RGB32_){
            const long tight = (long)outSize;
            if(len == tight){
                if(dsfrc_raw_fwrite(data,1,(size_t)len,stdout)!=(size_t)len) return E_FAIL;
                return S_OK;
            }
            long stride = len / h;
            if(stride < w*4) stride = w*4;
            for(int y=0;y<h;y++){
                memcpy(out + (size_t)y*w*4, data + (size_t)y*stride, (size_t)w*4);
            }
            if(dsfrc_raw_fwrite(out,1,outSize,stdout)!=outSize) return E_FAIL;
            return S_OK;
        }

        // RGB24 (BGR)
        if(subtype == MEDIASUBTYPE_RGB24_){
            long stride = len / h;
            if(stride < w*3) stride = w*3;
            for(int y=0;y<h;y++){
                const uint8_t* s = data + (size_t)y*stride;
                uint8_t* d = out + (size_t)y*w*4;
                for(int x=0;x<w;x++){
                    d[x*4+0]=s[x*3+0];
                    d[x*4+1]=s[x*3+1];
                    d[x*4+2]=s[x*3+2];
                    d[x*4+3]=255;
                }
            }
            if(dsfrc_raw_fwrite(out,1,outSize,stdout)!=outSize) return E_FAIL;
            return S_OK;
        }

        // YUY2
        if(subtype == MEDIASUBTYPE_YUY2_){
            long stride = len / h;
            if(stride < w*2) stride = w*2;
            for(int y=0;y<h;y++){
                const uint8_t* s = data + (size_t)y*stride;
                uint8_t* d = out + (size_t)y*w*4;
                for(int x=0;x<w; x+=2){
                    int y0 = s[x*2+0];
                    int u  = s[x*2+1];
                    int y1 = s[x*2+2];
                    int v  = s[x*2+3];
                    uint8_t r,g,b;
                    yuv_to_rgb(y0,u,v,r,g,b);
                    d[(x+0)*4+0]=b; d[(x+0)*4+1]=g; d[(x+0)*4+2]=r; d[(x+0)*4+3]=255;
                    if(x+1<w){
                        yuv_to_rgb(y1,u,v,r,g,b);
                        d[(x+1)*4+0]=b; d[(x+1)*4+1]=g; d[(x+1)*4+2]=r; d[(x+1)*4+3]=255;
                    }
                }
            }
            if(dsfrc_raw_fwrite(out,1,outSize,stdout)!=outSize) return E_FAIL;
            return S_OK;
        }

        // UYVY
        if(subtype == MEDIASUBTYPE_UYVY_){
            long stride = len / h;
            if(stride < w*2) stride = w*2;
            for(int y=0;y<h;y++){
                const uint8_t* s = data + (size_t)y*stride;
                uint8_t* d = out + (size_t)y*w*4;
                for(int x=0;x<w; x+=2){
                    int u  = s[x*2+0];
                    int y0 = s[x*2+1];
                    int v  = s[x*2+2];
                    int y1 = s[x*2+3];
                    uint8_t r,g,b;
                    yuv_to_rgb(y0,u,v,r,g,b);
                    d[(x+0)*4+0]=b; d[(x+0)*4+1]=g; d[(x+0)*4+2]=r; d[(x+0)*4+3]=255;
                    if(x+1<w){
                        yuv_to_rgb(y1,u,v,r,g,b);
                        d[(x+1)*4+0]=b; d[(x+1)*4+1]=g; d[(x+1)*4+2]=r; d[(x+1)*4+3]=255;
                    }
                }
            }
            if(dsfrc_raw_fwrite(out,1,outSize,stdout)!=outSize) return E_FAIL;
            return S_OK;
        }

        // NV12 -> BGRA
        if(subtype == MEDIASUBTYPE_NV12_){
            long stride = (long)(((__int64)len*2) / ((__int64)h*3));
            if(stride < w) stride = w;
            long expect = (long)((__int64)stride*h*3/2);
            if (expect > (long)len) {
                log_line("on_sample: expect>len (w=%d h=%d expect=%ld got=%zu) -> drop", w, h, expect, len);
                return S_OK;
            }
            const uint8_t* yplane = data;
            const uint8_t* uvplane = data + (size_t)stride * (size_t)h;
            for(int yy=0; yy<h; yy++){
                const uint8_t* yrow = yplane + (size_t)yy*stride;
                const uint8_t* uvrow = uvplane + (size_t)(yy/2)*stride;
                uint8_t* d = out + (size_t)yy*w*4;
                for(int x=0; x<w; x++){
                    int yv = yrow[x];
                    int u = uvrow[(x/2)*2 + 0];
                    int v = uvrow[(x/2)*2 + 1];
                    uint8_t r,g,b;
                    yuv_to_rgb(yv,u,v,r,g,b);
                    d[x*4+0]=b; d[x*4+1]=g; d[x*4+2]=r; d[x*4+3]=255;
                }
            }
            if(dsfrc_raw_fwrite(out,1,outSize,stdout)!=outSize) return E_FAIL;
            return S_OK;
        }

        // I420/IYUV/YV12 -> BGRA (planar 4:2:0)
        if(subtype == GUID_SUBTYPE_I420 || subtype == MEDIASUBTYPE_IYUV || subtype == GUID_SUBTYPE_YV12){
            // infer strides assuming tightly packed planes; if not, we still try using len heuristics
            // layout: Y (w*h) then U (w/2*h/2) then V (w/2*h/2) for I420/IYUV; swapped for YV12
            const size_t ySize = (size_t)w*h;
            const size_t cSize = (size_t)(w/2)*(size_t)(h/2);
            if((size_t)len < ySize + 2*cSize){
                logf(L"[warn] planar sample too small len=%ld\n", len);
                return S_OK;
            }
            const uint8_t* Y = data;
            const uint8_t* U = data + ySize;
            const uint8_t* V = data + ySize + cSize;
            if(subtype == GUID_SUBTYPE_YV12){
                std::swap(U, V);
            }
            for(int yy=0; yy<h; yy++){
                uint8_t* d = out + (size_t)yy*w*4;
                for(int x=0; x<w; x++){
                    int yv = Y[(size_t)yy*w + x];
                    int u = U[(size_t)(yy/2)*(w/2) + (x/2)];
                    int v = V[(size_t)(yy/2)*(w/2) + (x/2)];
                    uint8_t r,g,b;
                    yuv_to_rgb(yv,u,v,r,g,b);
                    d[x*4+0]=b; d[x*4+1]=g; d[x*4+2]=r; d[x*4+3]=255;
                }
            }
            if(dsfrc_raw_fwrite(out,1,outSize,stdout)!=outSize) return E_FAIL;
            return S_OK;
        }

        logf(L"[warn] Unsupported incoming subtype=%s. Try forcing decoder output, or use CSC externally.\n", guid_to_wstr(subtype).c_str());
        return S_OK;
    }

private:
    ~SinkFilter(){ if(m_pin) m_pin->Release(); m_pin=nullptr; }
    volatile LONG m_ref = 1;
    SinkPin* m_pin=nullptr;
    FILTER_STATE m_state=State_Stopped;
    ComPtr<IReferenceClock> m_clock;
    ComPtr<IFilterGraph> m_graph;
    std::wstring m_name=L"RawSink";
    uint64_t m_frameCount = 0;
    std::vector<uint8_t> m_tmp;
    bool m_auto_delay_done = false;

public:
    uint64_t frame_count() const { return m_frameCount; }

private:
    friend class SinkPin;
};

// --- pin impl ---
STDMETHODIMP SinkPin::QueryInterface(REFIID riid, void** ppv){
    if(!ppv) return E_POINTER;
    if(riid==IID_IUnknown || riid==IID_IPin){ *ppv=static_cast<IPin*>(this); AddRef(); return S_OK; }
    if(riid==IID_IMemInputPin){ *ppv=static_cast<IMemInputPin*>(this); AddRef(); return S_OK; }
    *ppv=nullptr; return E_NOINTERFACE;
}

static bool parse_vih2(const VIDEOINFOHEADER2* vih2, int& w, int& h){
    if (!vih2) return false;

    // Prefer rcSource if it is set. Some decoders (especially for
    // broadcast TS) report a padded stride (e.g. 1472x1080) in
    // bmiHeader.biWidth / biHeight but the actual intended image
    // area is 1440x1080 in rcSource. Using rcSource here keeps our
    // "logical" size consistent with what players and FRC expect.
    int srcW = vih2->rcSource.right  - vih2->rcSource.left;
    int srcH = vih2->rcSource.bottom - vih2->rcSource.top;
    if (srcW > 0 && srcH > 0) {
        w = srcW;
        h = srcH;
    } else {
        w = vih2->bmiHeader.biWidth;
        int hh = vih2->bmiHeader.biHeight;
        h = (hh < 0) ? -hh : hh;
    }
    return (w > 0 && h > 0);
}

static bool parse_vih(const VIDEOINFOHEADER* vih, int& w, int& h){
    if (!vih) return false;

    int srcW = vih->rcSource.right  - vih->rcSource.left;
    int srcH = vih->rcSource.bottom - vih->rcSource.top;
    if (srcW > 0 && srcH > 0) {
        w = srcW;
        h = srcH;
    } else {
        w = vih->bmiHeader.biWidth;
        int hh = vih->bmiHeader.biHeight;
        h = (hh < 0) ? -hh : hh;
    }
    return (w > 0 && h > 0);
}

void SinkPin::parse_format(const AM_MEDIA_TYPE* pmt){
    m_format_ok=false; m_w=0; m_h=0;
    if(!pmt) return;

    if(pmt->formattype==FORMAT_VideoInfo2 && pmt->pbFormat && pmt->cbFormat>=sizeof(VIDEOINFOHEADER2)){
        auto* vih2 = reinterpret_cast<const VIDEOINFOHEADER2*>(pmt->pbFormat);
        m_format_ok = parse_vih2(vih2, m_w, m_h);
        return;
    }
    if(pmt->formattype==FORMAT_VideoInfo && pmt->pbFormat && pmt->cbFormat>=sizeof(VIDEOINFOHEADER)){
        auto* vih = reinterpret_cast<const VIDEOINFOHEADER*>(pmt->pbFormat);
        m_format_ok = parse_vih(vih, m_w, m_h);
        return;
    }
    if(pmt->formattype==FORMAT_MPEG2Video && pmt->pbFormat && pmt->cbFormat>=sizeof(MPEG2VIDEOINFO)){
        auto* m2 = reinterpret_cast<const MPEG2VIDEOINFO*>(pmt->pbFormat);
        m_format_ok = parse_vih2(&m2->hdr, m_w, m_h);
        return;
    }
}

void SinkPin::apply_forced_if_needed(){
    if (m_format_ok) {
        // 2025-12: allow "logical" width override when the source is slightly wider
        // (e.g. 1472x1080 treated as 1440x1080 active area for broadcast 1440x1080 TS).
        if (m_forcedW > 0 && m_forcedH > 0 &&
            m_h == m_forcedH &&
            m_w > m_forcedW &&
            (m_w - m_forcedW) <= 128)
        {
            logf(L"[info] Sink logical width override %d -> %d (h=%d)\n", m_w, m_forcedW, m_h);
            m_w = m_forcedW;
        }

        // Keep the stored media type's "logical" size in sync so that
        // downstream stats (e.g. BlueskyFRC RawSink) see 1440x1080 instead
        // of padded widths like 1472x1080 when we intentionally crop.
        if (m_mt.pbFormat && m_mt.cbFormat >= sizeof(VIDEOINFOHEADER2) &&
            m_mt.formattype == FORMAT_VideoInfo2)
        {
            auto* vih2 = reinterpret_cast<VIDEOINFOHEADER2*>(m_mt.pbFormat);
            if (m_forcedW > 0) {
                vih2->bmiHeader.biWidth = m_forcedW;
                vih2->rcSource.right    = vih2->rcSource.left + m_forcedW;
            }
            if (m_forcedH > 0) {
                LONG sign = (vih2->bmiHeader.biHeight < 0) ? -1 : 1;
                vih2->bmiHeader.biHeight = sign * m_forcedH;
                vih2->rcSource.bottom    = vih2->rcSource.top + m_forcedH;
            }
        } else if (m_mt.pbFormat && m_mt.cbFormat >= sizeof(VIDEOINFOHEADER) &&
                   (m_mt.formattype == FORMAT_VideoInfo || m_mt.formattype == FORMAT_MPEG2Video))
        {
            auto* vih = reinterpret_cast<VIDEOINFOHEADER*>(m_mt.pbFormat);
            if (m_forcedW > 0) {
                vih->bmiHeader.biWidth = m_forcedW;
                vih->rcSource.right    = vih->rcSource.left + m_forcedW;
            }
            if (m_forcedH > 0) {
                LONG sign = (vih->bmiHeader.biHeight < 0) ? -1 : 1;
                vih->bmiHeader.biHeight = sign * m_forcedH;
                vih->rcSource.bottom    = vih->rcSource.top + m_forcedH;
            }
        }

        return;
    }

    if (m_forcedW > 0 && m_forcedH > 0) {
        m_w = m_forcedW;
        m_h = m_forcedH;
        m_format_ok = true;
        logf(L"[info] Sink using forced size %dx%d\n", m_w, m_h);

        if (m_mt.pbFormat && m_mt.cbFormat >= sizeof(VIDEOINFOHEADER2) &&
            m_mt.formattype == FORMAT_VideoInfo2)
        {
            auto* vih2 = reinterpret_cast<VIDEOINFOHEADER2*>(m_mt.pbFormat);
            LONG sign = (vih2->bmiHeader.biHeight < 0) ? -1 : 1;
            vih2->bmiHeader.biWidth  = m_forcedW;
            vih2->bmiHeader.biHeight = sign * m_forcedH;
            vih2->rcSource.right     = vih2->rcSource.left + m_forcedW;
            vih2->rcSource.bottom    = vih2->rcSource.top  + m_forcedH;
        } else if (m_mt.pbFormat && m_mt.cbFormat >= sizeof(VIDEOINFOHEADER) &&
                   (m_mt.formattype == FORMAT_VideoInfo || m_mt.formattype == FORMAT_MPEG2Video))
        {
            auto* vih = reinterpret_cast<VIDEOINFOHEADER*>(m_mt.pbFormat);
            LONG sign = (vih->bmiHeader.biHeight < 0) ? -1 : 1;
            vih->bmiHeader.biWidth  = m_forcedW;
            vih->bmiHeader.biHeight = sign * m_forcedH;
            vih->rcSource.right     = vih->rcSource.left + m_forcedW;
            vih->rcSource.bottom    = vih->rcSource.top  + m_forcedH;
        }
    }
}

STDMETHODIMP SinkPin::QueryAccept(const AM_MEDIA_TYPE* pmt)
{
    if (!pmt) return E_POINTER;
    if (pmt->majortype != MEDIATYPE_Video) return S_FALSE;

    const GUID& sub = pmt->subtype;
    const DWORD fcc = sub.Data1;

    auto is_fcc = [&](DWORD want) -> bool { return fcc == want; };

    bool ok = false;
    if (m_outFmt == OutFmt::NV12) {
        ok = (sub == MEDIASUBTYPE_NV12_) || is_fcc(MAKEFOURCC('N','V','1','2'));
    } else {
        // We can convert these to BGRA internally.
        ok =
            (sub == MEDIASUBTYPE_RGB32_) ||
            (sub == MEDIASUBTYPE_RGB24_) ||
            is_fcc(MAKEFOURCC('N','V','1','2')) ||
            is_fcc(MAKEFOURCC('Y','V','1','2')) ||
            is_fcc(MAKEFOURCC('I','4','2','0')) ||
            is_fcc(MAKEFOURCC('I','Y','U','V')) ||
            is_fcc(MAKEFOURCC('Y','U','Y','2')) ||
            is_fcc(MAKEFOURCC('U','Y','V','Y'));
    }

    if (g_diag_connect) {
        char fourcc_s[5] = { (char)(fcc & 0xFF), (char)((fcc >> 8) & 0xFF), (char)((fcc >> 16) & 0xFF), (char)((fcc >> 24) & 0xFF), 0 };
        log_line("[diag] Sink: QueryAccept out=%s subtype=%s fourcc=%s -> %s",
            (m_outFmt == OutFmt::NV12 ? "nv12" : "bgra"),
            guid_to_string(sub).c_str(),
            fourcc_s,
            ok ? "OK" : "NO");
    }
    return ok ? S_OK : S_FALSE;
}



// Minimal EnumMediaTypes:
// Provide a preferred target media type so IGraphBuilder::Connect can insert transforms
// (e.g., decoders / color converters) and land on a system-memory format we can write to stdout.

static AM_MEDIA_TYPE make_vih2_mt(const GUID& subtype, int w, int h){
    AM_MEDIA_TYPE mt{};
    mt.majortype = MEDIATYPE_Video;
    mt.subtype   = subtype;
    mt.formattype= FORMAT_VideoInfo2;
    mt.bFixedSizeSamples = TRUE;
    mt.bTemporalCompression = FALSE;
    mt.lSampleSize = 0;

    auto* vih2 = (VIDEOINFOHEADER2*)CoTaskMemAlloc(sizeof(VIDEOINFOHEADER2));
    ZeroMemory(vih2, sizeof(VIDEOINFOHEADER2));
    vih2->bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    vih2->bmiHeader.biWidth = w;
    vih2->bmiHeader.biHeight = h; // positive = bottom-up; OK for negotiation
    vih2->bmiHeader.biPlanes = 1;
    vih2->bmiHeader.biBitCount = (subtype == MEDIASUBTYPE_RGB32_) ? 32 : 12;
    vih2->bmiHeader.biCompression = (subtype == MEDIASUBTYPE_RGB32_) ? BI_RGB : MAKEFOURCC('N','V','1','2');
    vih2->bmiHeader.biSizeImage = 0;

    mt.cbFormat = sizeof(VIDEOINFOHEADER2);
    mt.pbFormat = (BYTE*)vih2;
    return mt;
}

class EnumMediaTypesPreferred final : public IEnumMediaTypes {
public:
    EnumMediaTypesPreferred(const std::vector<AM_MEDIA_TYPE>& mts) : m_mts(mts) {}

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if(!ppv) return E_POINTER;
        if(riid==IID_IUnknown || riid==IID_IEnumMediaTypes){ *ppv=(IEnumMediaTypes*)this; AddRef(); return S_OK; }
        *ppv=nullptr; return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return (ULONG)InterlockedIncrement(&m_ref); }
    STDMETHODIMP_(ULONG) Release() override { ULONG v=(ULONG)InterlockedDecrement(&m_ref); if(!v) delete this; return v; }

    STDMETHODIMP Next(ULONG c, AM_MEDIA_TYPE** ppmt, ULONG* fetched) override {
        if(fetched) *fetched=0;
        if(!ppmt) return E_POINTER;
        if(c==0) return S_OK;
        if(m_pos >= (LONG)m_mts.size()) return S_FALSE;

        // Allocate and deep-copy one MT.
        const AM_MEDIA_TYPE& src = m_mts[m_pos++];
        AM_MEDIA_TYPE* dst = (AM_MEDIA_TYPE*)CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE));
        if(!dst) return E_OUTOFMEMORY;
        *dst = src;
        if(src.cbFormat && src.pbFormat){
            dst->pbFormat = (BYTE*)CoTaskMemAlloc(src.cbFormat);
            if(!dst->pbFormat){ CoTaskMemFree(dst); return E_OUTOFMEMORY; }
            memcpy(dst->pbFormat, src.pbFormat, src.cbFormat);
        }
        if(dst->pUnk) dst->pUnk->AddRef();
        *ppmt = dst;
        if(fetched) *fetched=1;
        return S_OK;
    }
    STDMETHODIMP Skip(ULONG c) override { m_pos += (LONG)c; return (m_pos <= (LONG)m_mts.size()) ? S_OK : S_FALSE; }
    STDMETHODIMP Reset() override { m_pos=0; return S_OK; }
    STDMETHODIMP Clone(IEnumMediaTypes** pp) override { if(!pp) return E_POINTER; *pp=new EnumMediaTypesPreferred(m_mts); return *pp?S_OK:E_OUTOFMEMORY; }

private:
    ~EnumMediaTypesPreferred(){
        for(auto& mt : m_mts){
            if(mt.pbFormat){ CoTaskMemFree(mt.pbFormat); }
        }
    }
    volatile LONG m_ref = 1;
    std::vector<AM_MEDIA_TYPE> m_mts;
    LONG m_pos = 0;
};

STDMETHODIMP SinkPin::EnumMediaTypes(IEnumMediaTypes** ppEnum){
    if(!ppEnum) return E_POINTER;

    int ww = (m_forcedW>0)? m_forcedW : 16;
    int hh = (m_forcedH>0)? m_forcedH : 16;

    std::vector<AM_MEDIA_TYPE> mts;
    if(m_outFmt == OutFmt::NV12){
        mts.push_back(make_vih2_mt(MEDIASUBTYPE_NV12_, ww, hh));
    } else {
        mts.push_back(make_vih2_mt(MEDIASUBTYPE_RGB32_, ww, hh));
    }

    *ppEnum = new EnumMediaTypesPreferred(mts);
    return *ppEnum ? S_OK : E_OUTOFMEMORY;
}


STDMETHODIMP SinkPin::ReceiveConnection(IPin* pConnector, const AM_MEDIA_TYPE* pmt){
    if(!pConnector || !pmt) return E_POINTER;
    if(m_connected) return VFW_E_ALREADY_CONNECTED;
    HRESULT qhr = QueryAccept(pmt);
    if(qhr!=S_OK){
        logf(L"[err] ReceiveConnection: QueryAccept rejected hr=0x%08X subtype=%s formattype=%s bTemp=%d\n",
             (unsigned)qhr,
             guid_to_wstr(pmt->subtype).c_str(),
             guid_to_wstr(pmt->formattype).c_str(),
             (int)pmt->bTemporalCompression);
        return VFW_E_TYPE_NOT_ACCEPTED;
    }

    m_connected.reset(pConnector); pConnector->AddRef();

    ZeroMemory(&m_mt,sizeof(m_mt));
    m_mt = *pmt;
    if(pmt->cbFormat && pmt->pbFormat){
        m_mt.pbFormat=(BYTE*)CoTaskMemAlloc(pmt->cbFormat);
        if(!m_mt.pbFormat) return E_OUTOFMEMORY;
        memcpy(m_mt.pbFormat,pmt->pbFormat,pmt->cbFormat);
    }
    if(pmt->pUnk){ m_mt.pUnk=pmt->pUnk; m_mt.pUnk->AddRef(); }
    m_has_mt=true;

    parse_format(pmt);
    apply_forced_if_needed();

    logf(L"[ok] Sink: ReceiveConnection accepted (incoming subtype=%s, out=%s)\n",
         guid_to_wstr(pmt->subtype).c_str(), outfmt_name(m_outFmt));
    if(m_format_ok){
        logf(L"[info] Sink format %dx%d formattype=%s\n", m_w, m_h, guid_to_wstr(pmt->formattype).c_str());
    } else {
        logf(L"[warn] Sink could not parse size; provide --size=WxH\n");
    }
    return S_OK;
}

STDMETHODIMP SinkPin::Disconnect(){
    m_connected.reset();
    if(m_has_mt){ FreeMediaType(m_mt); ZeroMemory(&m_mt,sizeof(m_mt)); m_has_mt=false; }
    m_alloc.reset();
    return S_OK;
}
STDMETHODIMP SinkPin::ConnectedTo(IPin** ppPin){
    if(!ppPin) return E_POINTER;
    *ppPin = m_connected.get();
    if(*ppPin) (*ppPin)->AddRef();
    return m_connected ? S_OK : VFW_E_NOT_CONNECTED;
}
STDMETHODIMP SinkPin::ConnectionMediaType(AM_MEDIA_TYPE* pmt){
    if(!pmt) return E_POINTER;
    if(!m_has_mt) return VFW_E_NOT_CONNECTED;
    *pmt = m_mt;
    if(m_mt.cbFormat && m_mt.pbFormat){
        pmt->pbFormat=(BYTE*)CoTaskMemAlloc(m_mt.cbFormat);
        if(!pmt->pbFormat) return E_OUTOFMEMORY;
        memcpy(pmt->pbFormat,m_mt.pbFormat,m_mt.cbFormat);
    }
    if(m_mt.pUnk) m_mt.pUnk->AddRef();
    return S_OK;
}
STDMETHODIMP SinkPin::QueryPinInfo(PIN_INFO* pInfo){
    if(!pInfo) return E_POINTER;
    pInfo->pFilter = (IBaseFilter*)m_parent;
    if(pInfo->pFilter) pInfo->pFilter->AddRef();
    pInfo->dir = PINDIR_INPUT;
    wcsncpy_s(pInfo->achName, L"In", _TRUNCATE);
    return S_OK;
}
STDMETHODIMP SinkPin::QueryId(LPWSTR* Id){
    if(!Id) return E_POINTER;
    const wchar_t* s=L"In";
    size_t bytes=(wcslen(s)+1)*sizeof(wchar_t);
    *Id=(LPWSTR)CoTaskMemAlloc(bytes);
    if(!*Id) return E_OUTOFMEMORY;
    memcpy(*Id,s,bytes);
    return S_OK;
}
STDMETHODIMP SinkPin::GetAllocator(IMemAllocator** ppAllocator){
    if(!ppAllocator) return E_POINTER;
    if(m_alloc){
        *ppAllocator=m_alloc.get(); (*ppAllocator)->AddRef(); return S_OK;
    }
    IMemAllocator* p=nullptr;
    HRESULT hr=CoCreateInstance(CLSID_MemoryAllocator,nullptr,CLSCTX_INPROC_SERVER,IID_IMemAllocator,(void**)&p);
    if(SUCCEEDED(hr)){
        m_alloc.reset(p);
        *ppAllocator=p;
        p->AddRef();
    }
    return hr;
}
STDMETHODIMP SinkPin::NotifyAllocator(IMemAllocator* pAllocator, BOOL bReadOnly){
    m_alloc.reset(pAllocator);
    if(pAllocator) pAllocator->AddRef();
    m_readonly = (bReadOnly!=FALSE);
    logf(L"[info] Sink NotifyAllocator: allocator=%p readonly=%d\n", pAllocator, (int)m_readonly);
    return S_OK;
}
STDMETHODIMP SinkPin::Receive(IMediaSample* pSample){
    if(!pSample) return E_POINTER;
    BYTE* pData=nullptr;
    HRESULT hr=pSample->GetPointer(&pData);
    if(FAILED(hr) || !pData) return hr;
    long len=pSample->GetActualDataLength();

    if(!m_format_ok){
        // can't repack; but still dump raw (mainly for debugging)
        logf(L"[warn] Receive without size; len=%ld (raw dump)\n", len);
        if (dsfrc_raw_fwrite(pData,1,(size_t)len,stdout)!=(size_t)len) {
            logf(L"[err] stdout write failed (raw dump) len=%ld -> drop\n", len);
            return S_OK;
        }
        m_parent->inc_frame();
        return S_OK;
    }
    GUID sub = m_mt.subtype;
    hr = m_parent->on_sample((const uint8_t*)pData, len, m_w, m_h, sub);
    if (SUCCEEDED(hr)) {
        m_parent->inc_frame();
        return S_OK;
    }
    logf(L"[warn] on_sample failed hr=0x%08X len=%ld w=%d h=%d -> drop\n", (unsigned)hr, len, m_w, m_h);
    return S_OK;
}
STDMETHODIMP SinkPin::ReceiveMultiple(IMediaSample** pSamples, long nSamples, long* nProcessed){
    if(nProcessed) *nProcessed=0;
    if(!pSamples) return E_POINTER;
    for(long i=0;i<nSamples;i++){
        HRESULT hr=Receive(pSamples[i]);
        if(FAILED(hr)) return hr;
        if(nProcessed) (*nProcessed)++;
    }
    return S_OK;
}

// EnumPins (single pin)
class EnumPinsOne final : public IEnumPins {
public:
    explicit EnumPinsOne(IPin* pin): m_pin(pin){ if(m_pin) m_pin->AddRef(); }
    ~EnumPinsOne(){ if(m_pin) m_pin->Release(); m_pin=nullptr; }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if(!ppv) return E_POINTER;
        if(riid==IID_IUnknown || riid==IID_IEnumPins){ *ppv=(IEnumPins*)this; AddRef(); return S_OK; }
        *ppv=nullptr; return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return (ULONG)InterlockedIncrement(&m_ref); }
    STDMETHODIMP_(ULONG) Release() override { ULONG v=(ULONG)InterlockedDecrement(&m_ref); if(!v) delete this; return v; }

    STDMETHODIMP Next(ULONG cPins, IPin** ppPins, ULONG* fetched) override {
        if(!ppPins) return E_POINTER;
        if(fetched) *fetched=0;
        if(m_done) return S_FALSE;
        if(cPins==0) return S_OK;
        ppPins[0]=m_pin;
        if(ppPins[0]) ppPins[0]->AddRef();
        if(fetched) *fetched=1;
        m_done=true;
        return S_OK;
    }
    STDMETHODIMP Skip(ULONG) override { m_done=true; return S_FALSE; }
    STDMETHODIMP Reset() override { m_done=false; return S_OK; }
    STDMETHODIMP Clone(IEnumPins** ppEnum) override { if(!ppEnum) return E_POINTER; *ppEnum=new EnumPinsOne(m_pin); return *ppEnum?S_OK:E_OUTOFMEMORY; }

private:
    volatile LONG m_ref = 1;
    IPin* m_pin=nullptr;
    bool m_done=false;
};

STDMETHODIMP SinkFilter::EnumPins(IEnumPins** ppEnum){
    if(!ppEnum) return E_POINTER;
    *ppEnum = new EnumPinsOne(m_pin);
    return *ppEnum?S_OK:E_OUTOFMEMORY;
}
STDMETHODIMP SinkFilter::FindPin(LPCWSTR Id, IPin** ppPin){
    if(!ppPin) return E_POINTER;
    *ppPin=nullptr;
    if(!Id) return E_POINTER;
    if(wcscmp(Id,L"In")==0){
        *ppPin=m_pin; if(*ppPin) (*ppPin)->AddRef(); return S_OK;
    }
    return VFW_E_NOT_FOUND;
}
STDMETHODIMP SinkFilter::QueryFilterInfo(FILTER_INFO* pInfo){
    if(!pInfo) return E_POINTER;
    ZeroMemory(pInfo,sizeof(*pInfo));
    wcsncpy_s(pInfo->achName, L"RawSink", _TRUNCATE);
    pInfo->pGraph = nullptr;
    return S_OK;
}


// Dump current DirectShow graph topology (filters, pins, connections) for diagnostics.
static void log_graph_topology(IGraphBuilder* graph)
{
    if (!graph) {
        log_line(L"[graph] (null graph)");
        return;
    }

    ComPtr<IEnumFilters> ef;
    HRESULT hr = graph->EnumFilters(ef.put());
    if (FAILED(hr) || !ef) {
        log_hr(L"graph->EnumFilters", hr);
        return;
    }

    log_line(L"[graph] ---- Filter graph topology ----");
    ULONG fetched = 0;
    IBaseFilter* f = nullptr;
    int fiIndex = 0;
    while (ef->Next(1, &f, &fetched) == S_OK && fetched == 1) {
        FILTER_INFO fi{};
        std::wstring fname = L"(unknown)";
        if (SUCCEEDED(f->QueryFilterInfo(&fi))) {
            fname = fi.achName;
            if (fi.pGraph) fi.pGraph->Release();
        }
        logf(L"[graph] Filter #%d: %s\n", fiIndex, fname.c_str());

        ComPtr<IEnumPins> ep;
        if (SUCCEEDED(f->EnumPins(ep.put())) && ep) {
            IPin* p = nullptr;
            ULONG got = 0;
            int piIndex = 0;
            while (ep->Next(1, &p, &got) == S_OK && got == 1) {
                PIN_INFO pi{};
                std::wstring pinName = L"(pin)";
                std::wstring dirStr  = L"?";
                if (SUCCEEDED(p->QueryPinInfo(&pi))) {
                    if (pi.achName) pinName = pi.achName;
                    if (pi.dir == PINDIR_INPUT)  dirStr = L"In";
                    if (pi.dir == PINDIR_OUTPUT) dirStr = L"Out";
                    if (pi.pFilter) pi.pFilter->Release();
                }
                logf(L"[graph]   Pin #%d (%s) dir=%s\n",
                     piIndex, pinName.c_str(), dirStr.c_str());

                IPin* connected = nullptr;
                if (SUCCEEDED(p->ConnectedTo(&connected)) && connected) {
                    PIN_INFO pi2{};
                    std::wstring pinName2 = L"(pin)";
                    std::wstring filtName2 = L"(unknown)";
                    if (SUCCEEDED(connected->QueryPinInfo(&pi2))) {
                        if (pi2.achName) pinName2 = pi2.achName;
                        if (pi2.pFilter) {
                            FILTER_INFO fi2{};
                            if (SUCCEEDED(pi2.pFilter->QueryFilterInfo(&fi2))) {
                                filtName2 = fi2.achName;
                                if (fi2.pGraph) fi2.pGraph->Release();
                            }
                            pi2.pFilter->Release();
                        }
                    }
                    logf(L"[graph]     -> connected to: %s / %s\n",
                         filtName2.c_str(), pinName2.c_str());
                    connected->Release();
                }

                p->Release();
                p = nullptr;
                ++piIndex;
            }
        }

        f->Release();
        f = nullptr;
        ++fiIndex;
    }
    log_line(L"[graph] ---- end of graph ----");
}

// ---------- graph helpers ----------
static HRESULT add_filter(IGraphBuilder* g, REFCLSID clsid, const wchar_t* name, IBaseFilter** out){
    if(!g || !out) return E_POINTER;
    *out=nullptr;
    ComPtr<IBaseFilter> f;
    HRESULT hr=CoCreateInstance(clsid,nullptr,CLSCTX_INPROC_SERVER,IID_IBaseFilter,(void**)f.put());
    if(FAILED(hr)) return hr;
    hr=g->AddFilter(f.get(), name);
    if(FAILED(hr)) return hr;
    *out=f.get(); (*out)->AddRef();
    return S_OK;
}

static HRESULT create_filter_by_clsid_string(IGraphBuilder* graph, const wchar_t* clsidStr, const wchar_t* name, IBaseFilter** out){
    if(!graph||!clsidStr||!out) return E_POINTER;
    *out=nullptr;
    CLSID cls{};
    HRESULT hr = CLSIDFromString(clsidStr, &cls);
    if(FAILED(hr)) return hr;
    return add_filter(graph, cls, name, out);
}

static IPin* find_pin(IBaseFilter* f, PIN_DIRECTION dir, int idx=0){
    if(!f) return nullptr;
    ComPtr<IEnumPins> e;
    if(FAILED(f->EnumPins(e.put())) || !e) return nullptr;
    ULONG got=0; IPin* p=nullptr;
    int n=0;
    while(e->Next(1,&p,&got)==S_OK && got==1){
        PIN_DIRECTION d{};
        if(SUCCEEDED(p->QueryDirection(&d)) && d==dir){
            if(n==idx) return p; // caller owns ref
            n++;
        }
        p->Release(); p=nullptr;
    }
    return nullptr;
}

// Find a pin by (partial) pin name. More robust than relying on index order.
static IPin* find_pin_by_name(IBaseFilter* f, PIN_DIRECTION dir, const wchar_t* nameSubstr){
    if(!f || !nameSubstr || !*nameSubstr) return nullptr;
    ComPtr<IEnumPins> e;
    if(FAILED(f->EnumPins(e.put())) || !e) return nullptr;
    ULONG got=0; IPin* p=nullptr;
    while(e->Next(1,&p,&got)==S_OK && got==1){
        PIN_DIRECTION d{};
        if(SUCCEEDED(p->QueryDirection(&d)) && d==dir){
            PIN_INFO info{};
            if(SUCCEEDED(p->QueryPinInfo(&info))){
                bool hit = (wcsistr(info.achName, nameSubstr) != nullptr);
                if(info.pFilter) info.pFilter->Release();
                if(hit){
                    return p; // caller owns ref
                }
            }
        }
        p->Release(); p=nullptr;
    }
    return nullptr;
}

static HRESULT connect(IGraphBuilder* g, IPin* outPin, IPin* inPin){
    if(!g||!outPin||!inPin) return E_POINTER;
    return g->Connect(outPin,inPin);
}
static HRESULT connect_direct(IGraphBuilder* g, IPin* outPin, IPin* inPin){
    if(!g||!outPin||!inPin) return E_POINTER;
    return g->ConnectDirect(outPin,inPin,nullptr);
}

// Manual connection: enumerate media types on the OUTPUT pin and try direct IPin::Connect.
// This avoids GraphBuilder's media-type negotiation which can fail with custom sinks.
static HRESULT connect_by_out_mediatypes(IPin* outPin, IPin* inPin){
    if(!outPin||!inPin) return E_POINTER;
    IEnumMediaTypes* en = nullptr;
    HRESULT hr = outPin->EnumMediaTypes(&en);
    if(FAILED(hr) || !en){
        log_hr(L"EnumMediaTypes(outPin)", hr);
        return FAILED(hr) ? hr : E_FAIL;
    }
    AM_MEDIA_TYPE* pmt = nullptr;
    ULONG fetched = 0;
    int idx = 0;
    HRESULT last = VFW_E_NO_ACCEPTABLE_TYPES;
    while(en->Next(1, &pmt, &fetched) == S_OK && fetched == 1){
        if(pmt){
            logf(L"[try] outMT #%d subtype=%s formattype=%s cbFormat=%lu\n",
                 idx,
                 guid_to_wstr(pmt->subtype).c_str(),
                 guid_to_wstr(pmt->formattype).c_str(),
                 (unsigned long)pmt->cbFormat);

            hr = outPin->Connect(inPin, pmt);
            if(SUCCEEDED(hr)){
                logf(L"[ok] outPin->Connect succeeded with MT #%d\n", idx);
                DeleteMediaType(pmt);
                en->Release();
                return S_OK;
            }
            log_hr(L"outPin->Connect", hr);
            last = hr;
            DeleteMediaType(pmt);
        }
        idx++;
    }
    en->Release();
    return last;
}

static HRESULT find_connected_video_out_pin(IGraphBuilder* graph, IBaseFilter** outFilter, IPin** outPin){
    if(!graph||!outFilter||!outPin) return E_POINTER;
    *outFilter=nullptr; *outPin=nullptr;

    ComPtr<IEnumFilters> ef;
    HRESULT hr = graph->EnumFilters(ef.put());
    if(FAILED(hr)||!ef) return hr;

    ULONG got=0; IBaseFilter* f=nullptr;
    while(ef->Next(1,&f,&got)==S_OK && got==1){
        ComPtr<IEnumPins> ep;
        if(SUCCEEDED(f->EnumPins(ep.put())) && ep){
            IPin* p=nullptr; ULONG pg=0;
            while(ep->Next(1,&p,&pg)==S_OK && pg==1){
                PIN_DIRECTION d{};
                if(SUCCEEDED(p->QueryDirection(&d)) && d==PINDIR_OUTPUT){
                    ComPtr<IPin> to;
                    if(SUCCEEDED(p->ConnectedTo(to.put())) && to){
                        AM_MEDIA_TYPE mt{};
                        if(SUCCEEDED(p->ConnectionMediaType(&mt))){
                            bool isVideo = (mt.majortype==MEDIATYPE_Video);
                            FreeMediaType(mt);
                            if(isVideo){
                                *outFilter=f; (*outFilter)->AddRef();
                                *outPin=p; // keep ref
                                f->Release();
                                return S_OK;
                            }
                        }
                    }
                }
                p->Release(); p=nullptr;
            }
        }
        f->Release(); f=nullptr;
    }
    return E_FAIL;
}

// Probe input video size and fps from the DirectShow graph before we build
// the actual BlueskyFRC+RawSink graph. This lets us auto-fill
// --demuxer-rawvideo-w/h and --demuxer-rawvideo-fps for mpv.
static bool extract_video_info_from_mt(const AM_MEDIA_TYPE& mt, int& w, int& h, int& fpsNum, int& fpsDen)
{
    w = h = 0;
    fpsNum = 0;
    fpsDen = 1;

    if (!mt.pbFormat || mt.cbFormat == 0)
        return false;

    REFERENCE_TIME avg = 0;

    if (mt.formattype == FORMAT_VideoInfo2 && mt.cbFormat >= sizeof(VIDEOINFOHEADER2)) {
        auto* vih2 = reinterpret_cast<const VIDEOINFOHEADER2*>(mt.pbFormat);
        parse_vih2(vih2, w, h);
        avg = vih2->AvgTimePerFrame;
    } else if (mt.formattype == FORMAT_VideoInfo && mt.cbFormat >= sizeof(VIDEOINFOHEADER)) {
        auto* vih = reinterpret_cast<const VIDEOINFOHEADER*>(mt.pbFormat);
        parse_vih(vih, w, h);
        avg = vih->AvgTimePerFrame;
    } else if (mt.formattype == FORMAT_MPEG2Video && mt.cbFormat >= sizeof(MPEG2VIDEOINFO)) {
        auto* m2 = reinterpret_cast<const MPEG2VIDEOINFO*>(mt.pbFormat);
        parse_vih2(&m2->hdr, w, h);
        avg = m2->hdr.AvgTimePerFrame;
    }

    if (w <= 0 || h <= 0)
        return false;

    if (avg > 0) {
        // DirectShow REFERENCE_TIME is 100ns units -> 10,000,000 ticks per second.
        const LONGLONG oneSec = 10000000;
        if (avg <= 0)
            avg = 1;
        if (avg > 2147483647LL)
            avg = 2147483647LL;
        fpsNum = (int)oneSec;         // numerator
        fpsDen = (int)avg;            // denominator
    }

    return true;
}

static bool probe_input_video_info(const std::wstring& inputPath, int& w, int& h, int& fpsNum, int& fpsDen)
{
    w = h = 0;
    fpsNum = 0;
    fpsDen = 1;

    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) {
        log_hr(L"CoInitializeEx(probe)", hr);
        return false;
    }

    ComPtr<IGraphBuilder> graph;
    hr = CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER,
                          IID_IGraphBuilder, (void**)graph.put());
    if (FAILED(hr) || !graph) {
        log_hr(L"CoCreate(FilterGraph probe)", hr);
        CoUninitialize();
        return false;
    }

    hr = graph->RenderFile(inputPath.c_str(), nullptr);
    if (FAILED(hr)) {
        log_hr(L"RenderFile(probe)", hr);
        CoUninitialize();
        return false;
    }

    IBaseFilter* srcFilter = nullptr;
    IPin* srcOut = nullptr;
    hr = find_connected_video_out_pin(graph.get(), &srcFilter, &srcOut);
    if (FAILED(hr) || !srcOut) {
        log_hr(L"find_connected_video_out_pin(probe)", hr);
        if (srcOut) srcOut->Release();
        if (srcFilter) srcFilter->Release();
        CoUninitialize();
        return false;
    }

    AM_MEDIA_TYPE mt{};
    bool ok = false;
    if (SUCCEEDED(srcOut->ConnectionMediaType(&mt))) {
        int n = 0, d = 1;
        ok = extract_video_info_from_mt(mt, w, h, n, d);
        // Probe aspect ratio signal (if present) so we can apply it to mpv before launch.
        if (mt.formattype == FORMAT_VideoInfo2 && mt.cbFormat >= sizeof(VIDEOINFOHEADER2) && mt.pbFormat) {
            const auto* vih2 = reinterpret_cast<const VIDEOINFOHEADER2*>(mt.pbFormat);
            update_aspect_signal((int)vih2->dwPictAspectRatioX, (int)vih2->dwPictAspectRatioY);
        } else if (mt.formattype == FORMAT_MPEG2Video && mt.cbFormat >= sizeof(MPEG2VIDEOINFO) && mt.pbFormat) {
            const auto* m2 = reinterpret_cast<const MPEG2VIDEOINFO*>(mt.pbFormat);
            update_aspect_signal((int)m2->hdr.dwPictAspectRatioX, (int)m2->hdr.dwPictAspectRatioY);
        }
        if (ok) {
            fpsNum = n;
            fpsDen = d;
            logf(L"[probe] src video %dx%d, fps ~ %d/%d\n",
                 w, h, fpsNum, fpsDen);
        } else {
            log_line(L"[probe] extract_video_info_from_mt failed");
        }
        FreeMediaType(mt);
    } else {
        log_line(L"[probe] ConnectionMediaType(srcOut) failed");
    }

    if (srcOut) srcOut->Release();
    if (srcFilter) srcFilter->Release();
    CoUninitialize();
    return ok;
}


static HRESULT add_registered_filter_by_name(IGraphBuilder* graph, const std::wstring& nameSubstr, IBaseFilter** outFilter){
    if(!graph||!outFilter) return E_POINTER;
    *outFilter=nullptr;

    ComPtr<ICreateDevEnum> dev;
    HRESULT hr = CoCreateInstance(CLSID_SystemDeviceEnum,nullptr,CLSCTX_INPROC_SERVER,IID_ICreateDevEnum,(void**)dev.put());
    if(FAILED(hr)||!dev) return hr;

    ComPtr<IEnumMoniker> em;
    hr = dev->CreateClassEnumerator(CLSID_LegacyAmFilterCategory, em.put(), 0);
    if(hr != S_OK || !em) return E_FAIL;

    IMoniker* mk=nullptr; ULONG got=0;
    while(em->Next(1,&mk,&got)==S_OK && got==1){
        ComPtr<IPropertyBag> bag;
        if(SUCCEEDED(mk->BindToStorage(nullptr,nullptr,IID_IPropertyBag,(void**)bag.put())) && bag){
            VARIANT v; VariantInit(&v);
            if(SUCCEEDED(bag->Read(L"FriendlyName",&v,nullptr)) && v.vt==VT_BSTR){
                std::wstring fname = v.bstrVal ? v.bstrVal : L"";
                if(!nameSubstr.empty() && fname.find(nameSubstr)!=std::wstring::npos){
                    IBaseFilter* tmp=nullptr;
                    HRESULT bhr = mk->BindToObject(nullptr,nullptr,IID_IBaseFilter,(void**)&tmp);
                    if(SUCCEEDED(bhr) && tmp){
                        HRESULT ahr = graph->AddFilter(tmp, fname.c_str());
                        if(SUCCEEDED(ahr)){
                            *outFilter = tmp;
                            VariantClear(&v);
                            mk->Release();
                            return S_OK;
                        }
                        tmp->Release();
                    }
                }
            }
            VariantClear(&v);
        }
        mk->Release(); mk=nullptr;
    }
    return E_FAIL;
}

static void dump_pin_types(IPin* p, const wchar_t* tag){
    if(!p) return;
    ComPtr<IEnumMediaTypes> emt;
    if(FAILED(p->EnumMediaTypes(emt.put())) || !emt) return;
    AM_MEDIA_TYPE* mt=nullptr;
    ULONG got=0;
    int n=0;
    while(emt->Next(1,&mt,&got)==S_OK && got==1){
        if(mt->majortype==MEDIATYPE_Video){
            logf(L"[type:%s] #%d subtype=%s formattype=%s cbFormat=%lu\n",
                 tag, n, guid_to_wstr(mt->subtype).c_str(), guid_to_wstr(mt->formattype).c_str(), (unsigned long)mt->cbFormat);
            n++;
            if(n>=30){
                CoTaskMemFree(mt);
                break;
            }
        }
        FreeMediaType(*mt);
        CoTaskMemFree(mt);
        mt=nullptr;
    }

}


// Mute audio on all filters that expose IBasicAudio (to avoid DS audio double-play)
static void mute_graph_audio(IGraphBuilder* graph)
{
    if (!graph) return;
    ComPtr<IEnumFilters> enumFilters;
    if (FAILED(graph->EnumFilters(enumFilters.put()))) {
        log_line(L"[audio] EnumFilters failed");
        return;
    }
    while (true) {
        IBaseFilter* f = nullptr;
        if (enumFilters->Next(1, &f, nullptr) != S_OK)
            break;
        ComPtr<IBaseFilter> holder;
        holder.reset(f); // will Release automatically

        FILTER_INFO fi = {};
        if (SUCCEEDED(f->QueryFilterInfo(&fi))) {
            if (fi.pGraph) fi.pGraph->Release();
        }

        ComPtr<IBasicAudio> ba;
        if (SUCCEEDED(f->QueryInterface(IID_IBasicAudio, (void**)ba.put()))) {
            ba->put_Volume(-10000); // -10000 = silence
            log_line(L"[audio] IBasicAudio found on filter -> volume=-10000");
        }
    }
}

// Try to replace "Microsoft DTV-DVD Video Decoder" with "LAV Video Decoder"
// in an already-built RenderFile graph, keeping upstream and downstream pins
// connected as before. If no DTV-DVD video decoder is found, this is a no-op.
static HRESULT try_force_lav_video_decoder(IGraphBuilder* graph)
{
    if (!graph) return E_POINTER;

    ComPtr<IEnumFilters> efs;
    HRESULT hr = graph->EnumFilters(efs.put());
    if (FAILED(hr)) {
        log_hr(L"EnumFilters(try_force_lav_video_decoder)", hr);
        return hr;
    }

    IBaseFilter* target = nullptr;
    FILTER_INFO finfo{};
    while (true) {
        IBaseFilter* f = nullptr;
        if (efs->Next(1, &f, nullptr) != S_OK)
            break;

        ZeroMemory(&finfo, sizeof(finfo));
        if (SUCCEEDED(f->QueryFilterInfo(&finfo))) {
            if (finfo.achName[0]) {
                std::wstring name(finfo.achName);
                if (name.find(L"Microsoft DTV-DVD Video Decoder") != std::wstring::npos) {
                    if (finfo.pGraph) finfo.pGraph->Release();
                    target = f; // keep this one
                    break;
                }
            }
            if (finfo.pGraph) finfo.pGraph->Release();
        }
        f->Release();
    }

    if (!target) {
        log_line(L"[lavfix] no Microsoft DTV-DVD Video Decoder found in graph");
        return S_FALSE;
    }

    log_line(L"[lavfix] found Microsoft DTV-DVD Video Decoder; trying to insert LAV Video Decoder instead");

    IPin* decIn = find_pin(target, PINDIR_INPUT, 0);
    IPin* decOut = find_pin(target, PINDIR_OUTPUT, 0);
    if (!decIn || !decOut) {
        log_line(L"[lavfix] decoder has no input/output pins");
        if (decIn) decIn->Release();
        if (decOut) decOut->Release();
        target->Release();
        return E_FAIL;
    }

    IPin* upOut = nullptr;
    hr = decIn->ConnectedTo(&upOut);
    if (FAILED(hr) || !upOut) {
        log_hr(L"[lavfix] decoder input not connected", hr);
        decIn->Release();
        decOut->Release();
        target->Release();
        return S_FALSE;
    }

    IPin* downIn = nullptr;
    hr = decOut->ConnectedTo(&downIn);
    if (FAILED(hr) || !downIn) {
        log_hr(L"[lavfix] decoder output not connected", hr);
        upOut->Release();
        decIn->Release();
        decOut->Release();
        target->Release();
        return S_FALSE;
    }

    // Disconnect old paths
    graph->Disconnect(upOut);
    graph->Disconnect(decIn);
    graph->Disconnect(decOut);
    graph->Disconnect(downIn);

    // Add LAV Video Decoder
    IBaseFilter* lav = nullptr;
    hr = add_registered_filter_by_name(graph, L"LAV Video Decoder", &lav);
    if (FAILED(hr) || !lav) {
        log_hr(L"[lavfix] LAV Video Decoder not found/failed to add", hr);
        // Try to restore original connections
        graph->ConnectDirect(upOut, decIn, nullptr);
        graph->ConnectDirect(decOut, downIn, nullptr);
        if (lav) lav->Release();
        upOut->Release();
        downIn->Release();
        decIn->Release();
        decOut->Release();
        target->Release();
        return FAILED(hr) ? hr : E_FAIL;
    }

    IPin* lavIn = find_pin(lav, PINDIR_INPUT, 0);
    IPin* lavOut = find_pin(lav, PINDIR_OUTPUT, 0);
    if (!lavIn || !lavOut) {
        log_line(L"[lavfix] LAV Video Decoder pins missing");
        // Restore original connections
        graph->RemoveFilter(lav);
        graph->ConnectDirect(upOut, decIn, nullptr);
        graph->ConnectDirect(decOut, downIn, nullptr);
        if (lavIn) lavIn->Release();
        if (lavOut) lavOut->Release();
        lav->Release();
        upOut->Release();
        downIn->Release();
        decIn->Release();
        decOut->Release();
        target->Release();
        return E_FAIL;
    }

    hr = graph->ConnectDirect(upOut, lavIn, nullptr);
    if (FAILED(hr)) {
        log_hr(L"[lavfix] ConnectDirect(upOut->lavIn) failed", hr);
        graph->RemoveFilter(lav);
        graph->ConnectDirect(upOut, decIn, nullptr);
        graph->ConnectDirect(decOut, downIn, nullptr);
        lavIn->Release();
        lavOut->Release();
        lav->Release();
        upOut->Release();
        downIn->Release();
        decIn->Release();
        decOut->Release();
        target->Release();
        return hr;
    }

    hr = graph->ConnectDirect(lavOut, downIn, nullptr);
    if (FAILED(hr)) {
        log_hr(L"[lavfix] ConnectDirect(lavOut->downIn) failed", hr);
        // undo
        graph->Disconnect(lavIn);
        graph->Disconnect(lavOut);
        graph->RemoveFilter(lav);
        graph->ConnectDirect(upOut, decIn, nullptr);
        graph->ConnectDirect(decOut, downIn, nullptr);
        lavIn->Release();
        lavOut->Release();
        lav->Release();
        upOut->Release();
        downIn->Release();
        decIn->Release();
        decOut->Release();
        target->Release();
        return hr;
    }

    log_line(L"[lavfix] replaced Microsoft DTV-DVD Video Decoder with LAV Video Decoder");

    // Remove the old decoder filter from the graph
    graph->RemoveFilter(target);

    // Release local references
    lavIn->Release();
    lavOut->Release();
    lav->Release();
    upOut->Release();
    downIn->Release();
    decIn->Release();
    decOut->Release();
    target->Release();

    return S_OK;
}
static HRESULT try_force_dtv_for_interlaced_mpeg2(IGraphBuilder* graph)
{
    if (!graph) return E_POINTER;

    log_line(L"[dtvfix] scanning graph for LAV Video Decoder (interlaced MPEG-2)");

    IEnumFilters* efs = nullptr;
    HRESULT hr = graph->EnumFilters(&efs);
    if (FAILED(hr) || !efs) {
        log_hr(L"[dtvfix] EnumFilters", hr);
        return hr;
    }

    IBaseFilter* lav = nullptr;
    FILTER_INFO finfo{};
    while (true) {
        IBaseFilter* f = nullptr;
        if (efs->Next(1, &f, nullptr) != S_OK)
            break;

        ZeroMemory(&finfo, sizeof(finfo));
        if (SUCCEEDED(f->QueryFilterInfo(&finfo))) {
            if (finfo.achName[0]) {
                std::wstring name(finfo.achName);
                if (name.find(L"LAV Video Decoder") != std::wstring::npos) {
                    lav = f;
                    if (finfo.pGraph) finfo.pGraph->Release();
                    break;
                }
            }
            if (finfo.pGraph) finfo.pGraph->Release();
        }
        f->Release();
    }
    efs->Release();

    if (!lav) {
        log_line(L"[dtvfix] no LAV Video Decoder found in graph; nothing to do");
        return S_FALSE;
    }

    IPin* decIn  = find_pin(lav, PINDIR_INPUT,  0);
    IPin* decOut = find_pin(lav, PINDIR_OUTPUT, 0);
    if (!decIn || !decOut) {
        log_line(L"[dtvfix] LAV Video Decoder pins missing");
        if (decIn)  decIn->Release();
        if (decOut) decOut->Release();
        lav->Release();
        return S_FALSE;
    }

    // Check decoder *output* interlace flags
    AM_MEDIA_TYPE decOutMT{};
    bool isInterlaced = false;
    bool isMpeg2      = false;

    hr = decOut->ConnectionMediaType(&decOutMT);
    if (SUCCEEDED(hr)) {
        // Only treat this as our "DTV MPEG-2 case" when the decoder output
        // is actually MPEG-2 video. We *do not* want to touch H.264/AVC TS.
        if (decOutMT.subtype == MEDIASUBTYPE_MPEG2_VIDEO ||
            decOutMT.formattype == FORMAT_MPEG2Video)
        {
            isMpeg2 = true;
        }

        if (decOutMT.formattype == FORMAT_VideoInfo2 &&
            decOutMT.cbFormat   >= sizeof(VIDEOINFOHEADER2) &&
            decOutMT.pbFormat)
        {
            const VIDEOINFOHEADER2* vih2 =
                reinterpret_cast<const VIDEOINFOHEADER2*>(decOutMT.pbFormat);
            int  w = 0, h = 0;
            parse_vih2(vih2, w, h);

            DWORD flags = vih2->dwInterlaceFlags;
            if (flags != 0) {
                isInterlaced = true;
                logf(L"[dtvfix] LAV out interlaced: %dx%d flags=0x%08lX\n",
                     w, h, (unsigned long)flags);
            } else {
                logf(L"[dtvfix] LAV out progressive: %dx%d flags=0x%08lX\n",
                     w, h, (unsigned long)flags);
            }
        } else {
            log_line(L"[dtvfix] LAV out is not VIDEOINFOHEADER2; cannot inspect interlace flags");
        }
        FreeMediaType(decOutMT);
    } else {
        log_hr(L"[dtvfix] decOut->ConnectionMediaType failed", hr);
    }

    // If the stream is not both "interlaced" and "MPEG-2", do NOT
    // try to swap LAV for Microsoft DTV-DVD. This keeps progressive
    // and H.264 TS files on LAV where they behave well with FRC.
    if (!isInterlaced || !isMpeg2) {
        logf(L"[dtvfix] keep LAV (isInterlaced=%d, isMpeg2=%d)\n",
             isInterlaced ? 1 : 0, isMpeg2 ? 1 : 0);
        decIn->Release();
        decOut->Release();
        lav->Release();
        return S_FALSE;
    }

    // For ISDB-T / BS/CS transport streams we may prefer Microsoft
    // DTV-DVD decoder because LAV can mis-report flags or sizes for
    // 1440x1080 interlaced material. Only apply this path when the
    // user is actually feeding a TS-like input.
    if (!g_is_ts_like_input) {
        log_line(L"[dtvfix] MPEG-2 but not TS-like input; keep LAV");
        decIn->Release();
        decOut->Release();
        lav->Release();
        return S_FALSE;
    }

log_line(L"[dtvfix] trying to replace LAV Video Decoder with Microsoft DTV-DVD Video Decoder for interlaced stream");

    IPin* upOut = nullptr;
    hr = decIn->ConnectedTo(&upOut);
    if (FAILED(hr) || !upOut) {
        log_hr(L"[dtvfix] LAV input not connected", hr);
        if (upOut) upOut->Release();
        decIn->Release();
        decOut->Release();
        lav->Release();
        return S_FALSE;
    }

    IPin* downIn = nullptr;
    hr = decOut->ConnectedTo(&downIn);
    if (FAILED(hr) || !downIn) {
        log_hr(L"[dtvfix] LAV output not connected", hr);
        upOut->Release();
        decIn->Release();
        decOut->Release();
        lav->Release();
        return S_FALSE;
    }

    IBaseFilter* dtv = nullptr;
    hr = add_registered_filter_by_name(graph, L"Microsoft DTV-DVD Video Decoder", &dtv);
    if (FAILED(hr) || !dtv) {
        log_hr(L"[dtvfix] failed to add Microsoft DTV-DVD Video Decoder", hr);
        upOut->Release();
        downIn->Release();
        decIn->Release();
        decOut->Release();
        lav->Release();
        return S_FALSE;
    }

    IPin* dtvIn  = find_pin(dtv, PINDIR_INPUT,  0);
    IPin* dtvOut = find_pin(dtv, PINDIR_OUTPUT, 0);
    if (!dtvIn || !dtvOut) {
        log_line(L"[dtvfix] Microsoft DTV-DVD Video Decoder pins missing");
        if (dtvIn)  dtvIn->Release();
        if (dtvOut) dtvOut->Release();
        graph->RemoveFilter(dtv);
        upOut->Release();
        downIn->Release();
        decIn->Release();
        decOut->Release();
        lav->Release();
        return E_FAIL;
    }

    // Disconnect LAV path
    graph->Disconnect(upOut);
    graph->Disconnect(decIn);
    graph->Disconnect(decOut);
    graph->Disconnect(downIn);

    // Remove LAV filter from graph
    graph->RemoveFilter(lav);

    // Connect DTV-DVD in its place
    hr = graph->ConnectDirect(upOut, dtvIn, nullptr);
    if (FAILED(hr)) {
        log_hr(L"[dtvfix] ConnectDirect(upOut->dtvIn) failed", hr);

        graph->Disconnect(dtvIn);
        graph->Disconnect(dtvOut);
        graph->RemoveFilter(dtv);

        // Best-effort rollback: re-add LAV and reconnect
        IBaseFilter* lav2 = nullptr;
        HRESULT hr2 = add_registered_filter_by_name(graph, L"LAV Video Decoder", &lav2);
        if (SUCCEEDED(hr2) && lav2) {
            IPin* lav2In  = find_pin(lav2, PINDIR_INPUT,  0);
            IPin* lav2Out = find_pin(lav2, PINDIR_OUTPUT, 0);
            if (lav2In && lav2Out) {
                graph->ConnectDirect(upOut, lav2In, nullptr);
                graph->ConnectDirect(lav2Out, downIn, nullptr);
            }
            if (lav2In)  lav2In->Release();
            if (lav2Out) lav2Out->Release();
            lav2->Release();
        }

        upOut->Release();
        downIn->Release();
        decIn->Release();
        decOut->Release();
        lav->Release();
        dtvIn->Release();
        dtvOut->Release();
        return hr;
    }

    hr = graph->ConnectDirect(dtvOut, downIn, nullptr);
    if (FAILED(hr)) {
        log_hr(L"[dtvfix] ConnectDirect(dtvOut->downIn) failed", hr);

        graph->Disconnect(dtvIn);
        graph->Disconnect(dtvOut);
        graph->RemoveFilter(dtv);

        // Rollback: try to restore LAV path
        IBaseFilter* lav2 = nullptr;
        HRESULT hr2 = add_registered_filter_by_name(graph, L"LAV Video Decoder", &lav2);
        if (SUCCEEDED(hr2) && lav2) {
            IPin* lav2In  = find_pin(lav2, PINDIR_INPUT,  0);
            IPin* lav2Out = find_pin(lav2, PINDIR_OUTPUT, 0);
            if (lav2In && lav2Out) {
                graph->ConnectDirect(upOut, lav2In, nullptr);
                graph->ConnectDirect(lav2Out, downIn, nullptr);
            }
            if (lav2In)  lav2In->Release();
            if (lav2Out) lav2Out->Release();
            lav2->Release();
        }

        upOut->Release();
        downIn->Release();
        decIn->Release();
        decOut->Release();
        lav->Release();
        dtvIn->Release();
        dtvOut->Release();
        return hr;
    }

    log_line(L"[dtvfix] replaced LAV Video Decoder with Microsoft DTV-DVD Video Decoder for interlaced stream");

    upOut->Release();
    downIn->Release();
    decIn->Release();
    decOut->Release();
    lav->Release();
    dtvIn->Release();
    dtvOut->Release();
    return S_OK;
}



static 
// Try to find an existing filter in the current graph whose friendly name
// contains the given substring. This is useful to avoid inserting a second
// instance of BlueskyFRC if the auto-built graph already contains it.
HRESULT find_filter_in_graph_by_name_substring(IGraphBuilder* graph, const std::wstring& nameSubstr, IBaseFilter** outFilter)
{
    if (!graph || !outFilter) return E_POINTER;
    *outFilter = nullptr;

    ComPtr<IEnumFilters> ef;
    HRESULT hr = graph->EnumFilters(ef.put());
    if (FAILED(hr) || !ef) return hr;

    ULONG got = 0;
    IBaseFilter* f = nullptr;
    while (ef->Next(1, &f, &got) == S_OK && got == 1) {
        FILTER_INFO fi{};
        if (SUCCEEDED(f->QueryFilterInfo(&fi))) {
            std::wstring fname = fi.achName;
            if (!nameSubstr.empty() && fname.find(nameSubstr) != std::wstring::npos) {
                *outFilter = f;
                if (fi.pGraph) fi.pGraph->Release();
                return S_OK;
            }
            if (fi.pGraph) fi.pGraph->Release();
        }
        f->Release();
        f = nullptr;
    }
    return E_FAIL;
}

HRESULT build_and_run(const std::wstring& inputPath, bool frcOn, const std::wstring& frcName,
                            OutFmt outFmt, int forcedW, int forcedH){
  logf(L"dsfrc_pipe_lav2 (DirectShow -> rawvideo) out=%s\n", outfmt_name(outFmt));

    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if(FAILED(hr)){ log_hr(L"CoInitializeEx", hr); return hr; }

    ComPtr<IGraphBuilder> graph;
    hr = CoCreateInstance(CLSID_FilterGraph,nullptr,CLSCTX_INPROC_SERVER,IID_IGraphBuilder,(void**)graph.put());
    if(FAILED(hr)){ log_hr(L"CoCreate(FilterGraph)", hr); CoUninitialize(); return hr; }

    ComPtr<IMediaControl> mc;
    graph->QueryInterface(IID_IMediaControl,(void**)mc.put());

    ComPtr<IMediaEventEx> me;
    graph->QueryInterface(IID_IMediaEventEx,(void**)me.put());

    log_line(L"[try] RenderFile()");
    hr = graph->RenderFile(inputPath.c_str(), nullptr);
    if(FAILED(hr)){ log_hr(L"RenderFile", hr); CoUninitialize(); return hr; }
    log_line(L"[ok] RenderFile done");

// For non-TS-like inputs (e.g. progressive H.264 MP4), prefer the LAV path.
// For TS-like inputs (.ts/.m2ts/.mts), we may want Microsoft DTV-DVD for
// interlaced MPEG-2, so we *do not* blindly swap to LAV here.
    if (!g_is_ts_like_input) {
        log_line(L"[dtvfix] non-TS input -> try Force LAV Video Decoder");
        try_force_lav_video_decoder(graph.get());
    } else {
        log_line(L"[dtvfix] TS-like input -> allow DTV-DVD swap only for interlaced MPEG-2");
        try_force_dtv_for_interlaced_mpeg2(graph.get());
    }

// After the graph is auto-built (and possibly patched), mute any audio-renderer branches
    mute_graph_audio(graph.get());

    IBaseFilter* srcVideoFilter=nullptr;
    IPin* srcOut=nullptr;
    hr = find_connected_video_out_pin(graph.get(), &srcVideoFilter, &srcOut);
    if(FAILED(hr)){ log_line(L"[err] couldn't find connected video output"); CoUninitialize(); return hr; }
    log_line(L"[ok] Found connected video output pin");

    ComPtr<IPin> downstream;
    if(SUCCEEDED(srcOut->ConnectedTo(downstream.put())) && downstream){
        graph->Disconnect(srcOut);
        graph->Disconnect(downstream.get());
        log_line(L"[ok] Disconnected src video out from downstream");
    }

    IBaseFilter* frc = nullptr;
    IPin* upOut = srcOut; // keep ref (will be replaced by FRC out when connected)

    if (frcOn)
    {
        // First, try to reuse an existing BlueskyFRC instance in the graph
        // (for environments where the system already inserted it as renderer).
        log_line(L"[try] Search existing BlueskyFRC in graph");
        HRESULT hrF = find_filter_in_graph_by_name_substring(graph.get(), frcName, &frc);
        if (SUCCEEDED(hrF) && frc) {
            log_line(L"[ok] Reusing existing BlueskyFRC filter from auto-built graph");
        } else {
            log_line(L"[try] Add BlueskyFRC by friendly-name substring");
            hrF = add_registered_filter_by_name(graph.get(), frcName, &frc);
            if (FAILED(hrF) || !frc)
            {
                log_hr(L"add_registered_filter_by_name(BlueskyFRC)", FAILED(hrF) ? hrF : HRESULT_FROM_WIN32(ERROR_NOT_FOUND));
                log_line(L"[err] --frc=on was requested, but BlueskyFRC filter could not be created. Aborting FRC mode.");
                // Fail the whole DirectShow run so that caller can fall back to normal mpv playback.
                CoUninitialize();
                return E_FAIL;
            }
            log_line(L"[ok] BlueskyFRC added");
        }
        IPin* frcIn  = find_pin(frc, PINDIR_INPUT, 0);
        IPin* frcOut = find_pin(frc, PINDIR_OUTPUT, 0);
        if (!frcIn || !frcOut)
        {
            log_line(L"[err] BlueskyFRC pin(s) not found; aborting FRC mode.");
            if (frcIn)  frcIn->Release();
            if (frcOut) frcOut->Release();
            frc->Release();
            CoUninitialize();
            return E_FAIL;
        }

        log_line(L"[try] Connect src->FRC (direct mediatypes)");
        hr = connect_by_out_mediatypes(upOut, frcIn);
        frcIn->Release();

        if (FAILED(hr))
        {
            log_hr(L"src->FRC", hr);
            log_line(L"[err] Failed to connect source video to FRC; aborting FRC mode.");
            frcOut->Release();
            frc->Release();
            CoUninitialize();
            return hr;
        }

        log_line(L"[ok] src->FRC connected");
        upOut = frcOut; // keep ref as new upstream output
    }

    // Insert Infinite Pin Tee between FRC (if any) and RawSink.
// This tries to more closely mimic BlueskyFRCUtil's graph, where
// "Infinite Pin Tee Filter" often appears as the renderer.
// If anything fails, we gracefully fall back to direct FRC->RawSink.
IBaseFilter* tee = nullptr;
IPin* teeIn = nullptr;
IPin* teePreviewOut = nullptr;
IPin* teeCaptureOut = nullptr;

IPin* finalOut = upOut;

// Only bother with the tee chain when FRC is actually enabled.
if (frcOn && finalOut) {
    log_line(L"[tee] trying to insert Infinite Pin Tee after FRC");

    HRESULT hrT = CoCreateInstance(CLSID_InfTee, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_IBaseFilter, (void**)&tee);
    if (SUCCEEDED(hrT) && tee) {
        hrT = graph->AddFilter(tee, L"Infinite Pin Tee Filter");
        if (SUCCEEDED(hrT)) {
            teeIn         = find_pin(tee, PINDIR_INPUT, 0);
            teePreviewOut = find_pin(tee, PINDIR_OUTPUT, 0);
            teeCaptureOut = find_pin(tee, PINDIR_OUTPUT, 1);
        }
    }

    if (tee && teeIn && teePreviewOut && teeCaptureOut) {
        HRESULT hrC = graph->ConnectDirect(finalOut, teeIn, nullptr);
        if (SUCCEEDED(hrC)) {
            log_line(L"[tee] FRC->InfTee connected");

            // We ignore the first (preview) output here; BlueskyFRC will still
            // see the tee as its immediate downstream filter (renderer).
            finalOut = teeCaptureOut;
            log_line(L"[tee] Using InfTee capture output as finalOut for RawSink");
        } else {
            log_hr(L"[tee] connect FRC->InfTee", hrC);
            log_line(L"[tee] Falling back to direct FRC->RawSink");
        }
    } else {
        log_line(L"[tee] InfTee unavailable or missing pins; using direct FRC->RawSink");
    }
}


    log_line(L"[info] finalOut media types (up to 30):");
    dump_pin_types(finalOut, L"finalOut");

    // Add our sink
    SinkFilter* sinkObj = new SinkFilter(outFmt, forcedW, forcedH);
    IBaseFilter* sink = sinkObj;
    hr = graph->AddFilter(sink, L"RawSink");
    if(FAILED(hr)){ log_hr(L"AddFilter(RawSink)", hr); sink->Release(); CoUninitialize(); return hr; }
    IPin* sinkIn = sinkObj->pin(); sinkIn->AddRef();

    log_line(L"[try] Connect finalOut -> RawSink (IGraphBuilder::Connect)");
    hr = connect(graph.get(), finalOut, sinkIn);
    if (FAILED(hr)) {
        log_hr(L"Connect(final->sink) Connect()", hr);

        // Fallback 1: try all advertised media types on finalOut via outPin->Connect
        log_line(L"[try] Connect finalOut -> RawSink via outPin->Connect (EnumMediaTypes)");
        HRESULT hr2 = connect_by_out_mediatypes(finalOut, sinkIn);
        if (SUCCEEDED(hr2)) {
            log_line(L"[ok] finalOut -> RawSink connected via manual outPin->Connect");
            hr = S_OK;
        } else {
            log_hr(L"Connect(final->sink) outPin->Connect", hr2);

            // Fallback 2: try inserting Color Space Converter (quartz) for BGRA output.
            if (outFmt == OutFmt::BGRA) {
                IBaseFilter* csc = nullptr;
                // CLSID_Colour (Color Space Converter): {1643E180-90F5-11CE-97D5-00AA0055595A}
                HRESULT hrC = create_filter_by_clsid_string(
                    graph.get(),
                    L"{1643E180-90F5-11CE-97D5-00AA0055595A}",
                    L"Color Space Converter",
                    &csc
                );
                if (SUCCEEDED(hrC) && csc) {
                    log_line(L"[ok] Color Space Converter added");
                    IPin* cscIn  = find_pin(csc, PINDIR_INPUT, 0);
                    IPin* cscOut = find_pin(csc, PINDIR_OUTPUT, 0);
                    if (cscIn && cscOut) {
                        log_line(L"[try] Connect finalOut -> CSC");
                        HRESULT hr1 = connect(graph.get(), finalOut, cscIn);
                        if (FAILED(hr1)) {
                            log_hr(L"final->csc", hr1);
                        } else {
                            log_line(L"[ok] final->csc connected");

                            log_line(L"[try] Connect CSC -> RawSink");
                            HRESULT hr3 = connect(graph.get(), cscOut, sinkIn);
                            if (FAILED(hr3)) {
                                log_hr(L"csc->sink", hr3);
                            } else {
                                log_line(L"[ok] CSC -> RawSink connected");
                                hr = S_OK;
                            }
                        }
                    }
                    if (cscIn)  cscIn->Release();
                    if (cscOut) cscOut->Release();
                    if (FAILED(hr)) {
                        csc->Release();
                        csc = nullptr;
                    }
                }
            }
        }
    }
    if (FAILED(hr)) {
        log_hr(L"Connect(final->sink) manual", hr);
        log_line(L"[err] Could not connect finalOut to RawSink.");
        sinkIn->Release();
        sink->Release();
        CoUninitialize();
        return hr;
    }
    log_line(L"[ok] finalOut -> RawSink connected");
    log_line(L"[ok] finalOut -> RawSink connected");

    log_graph_topology(graph.get());
    log_line(L"[run] graph Run()");
    hr = mc->Run();
    if(FAILED(hr)) log_hr(L"IMediaControl::Run", hr);

    DWORD startTick = GetTickCount();
    uint64_t lastFrames = 0;

    if(SUCCEEDED(hr) && me){
        long ev=0; LONG_PTR p1=0,p2=0;
        while(true){
            if(g_stop_requested.load()){
                logf(L"[ctrl] stop requested -> stopping graph\n");
                if(mc) mc->Stop();
                break;
            }

            DWORD now = GetTickCount();
            uint64_t frames = sinkObj->frame_count();
            if (frames > lastFrames) {
                lastFrames = frames;
            } else if (frcOn && frames == 0 && now - startTick > 5000) {
                log_line(L"[err] Timeout: no frames received from FRC; aborting graph.");
                hr = E_FAIL;
                if (mc) mc->Stop();
                break;
            }

            HRESULT erh = me->GetEvent(&ev,&p1,&p2,200);
            if(erh==S_OK){
                me->FreeEventParams(ev,p1,p2);
                if(ev==EC_COMPLETE || ev==EC_ERRORABORT || ev==EC_USERABORT) break;
            }
        }
    }

    // One more check after stopping: if FRC is ON and no frames were ever delivered,
    // treat this as a failure so that the caller can fall back to normal playback.
    uint64_t totalFrames = sinkObj->frame_count();
    logf(L"[info] total frames delivered by RawSink: %llu\n", (unsigned long long)totalFrames);
    if (frcOn && totalFrames == 0 && SUCCEEDED(hr)) {
        log_line(L"[err] FRC mode ended with zero frames; marking as failure.");
        hr = E_FAIL;
    }

    mc->Stop();

    sinkIn->Release(); sink->Release();
    if(teeIn) teeIn->Release();
    if(teePreviewOut) teePreviewOut->Release();
    if(teeCaptureOut) teeCaptureOut->Release();
    if(tee) tee->Release();
    if(frc) frc->Release();
    if(srcVideoFilter) srcVideoFilter->Release();
    if(srcOut) srcOut->Release();

    CoUninitialize();
    return hr;
}

static void usage(){
    // Print to console even when log is off.
    print_err(L"Usage: dsfrc_pipe.exe <input> [--frc=on|off] [--out=bgra|nv12] [--size=WxH] [--frc-name=SUBSTR] [log=yes|log=no]");
    // Also write to dsfrc.log when enabled.
    log_line(L"Usage: dsfrc_pipe.exe <input> [--frc=on|off] [--out=bgra|nv12] [--size=WxH] [--frc-name=SUBSTR] [log=yes|log=no]");
}

static bool parse_size(const std::wstring& s, int& w, int& h){
    size_t x = s.find(L'x');
    if(x==std::wstring::npos) x = s.find(L'X');
    if(x==std::wstring::npos) return false;
    w = _wtoi(s.substr(0,x).c_str());
    h = _wtoi(s.substr(x+1).c_str());
    return (w>0 && h>0);
}



// Parse log option.
// Accepts: log=yes|no|on|off|1|0|true|false
// Also accepts --log=yes style.
// Returns true if the argument was a log switch (and sets enabled accordingly).
static bool parse_log_arg(const std::wstring& arg, bool& enabled)
{
    std::wstring a = arg;
    // strip leading dashes
    while (!a.empty() && a[0] == L'-') a.erase(a.begin());
    if (a.rfind(L"log=", 0) != 0)
        return false;
    std::wstring v = a.substr(4);
    // to lower
    for (auto& ch : v) ch = (wchar_t)towlower(ch);

    if (v == L"yes" || v == L"on" || v == L"1" || v == L"true") { enabled = true; return true; }
    if (v == L"no"  || v == L"off"|| v == L"0" || v == L"false") { enabled = false; return true; }

    // unknown value: treat as enable so user can collect logs
    enabled = true;
    return true;
}


static int launch_normal_mpv(const std::wstring& mpvPath,
                             const std::wstring& workDir,
                             const std::wstring& input,
                             const std::wstring& aspectOverride)
{
    if (mpvPath.empty() || input.empty())
        return 1;

    std::wstring normalCmd = L"\"";
    normalCmd += mpvPath;
    normalCmd += L"\" ";
    if (!aspectOverride.empty()) {
        normalCmd += L"--video-aspect-override=";
        normalCmd += aspectOverride;
        normalCmd += L" ";
    }
    normalCmd += L"\"";
    normalCmd += input;
    normalCmd += L"\"";

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::wstring buf = normalCmd;

    BOOL ok = CreateProcessW(
        nullptr,
        buf.data(),
        nullptr,
        nullptr,
        FALSE,
        0,
        nullptr,
        workDir.empty() ? nullptr : workDir.c_str(),
        &si,
        &pi
    );
    if (!ok)
    {
        log_hr(L"CreateProcessW(mpv.exe normal)", HRESULT_FROM_WIN32(GetLastError()));
        return 1;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
}

// Create a named pipe pair for rawvideo stdin so we can use OVERLAPPED writes with timeout.
// This prevents dsfrc_pipe.exe from getting stuck if mpv stops reading (restart failure, file switch, etc.).
static bool create_rawvideo_pipe_for_mpv(HANDLE& hReadInheritable, HANDLE& hWriteOverlapped, std::wstring& pipeNameOut)
{
    hReadInheritable = INVALID_HANDLE_VALUE;
    hWriteOverlapped = INVALID_HANDLE_VALUE;
    pipeNameOut.clear();

    DWORD pid = GetCurrentProcessId();
    DWORD tick = GetTickCount();
    wchar_t nameBuf[256];
    _snwprintf_s(nameBuf, _TRUNCATE, L"\\\\.\\pipe\\dsfrc-raw-%lu-%lu", (unsigned long)pid, (unsigned long)tick);
    pipeNameOut.assign(nameBuf);

    SECURITY_ATTRIBUTES saNoInherit{};
    saNoInherit.nLength = sizeof(saNoInherit);
    saNoInherit.bInheritHandle = FALSE;
    saNoInherit.lpSecurityDescriptor = nullptr;

    SECURITY_ATTRIBUTES saInherit{};
    saInherit.nLength = sizeof(saInherit);
    saInherit.bInheritHandle = TRUE;
    saInherit.lpSecurityDescriptor = nullptr;

    // Server (writer) end
    hWriteOverlapped = CreateNamedPipeW(
        pipeNameOut.c_str(),
        PIPE_ACCESS_OUTBOUND | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1,
        4 * 1024 * 1024, // out buffer
        4 * 1024 * 1024, // in buffer (unused)
        0,
        &saNoInherit
    );
    if (hWriteOverlapped == INVALID_HANDLE_VALUE)
    {
        log_hr(L"CreateNamedPipeW(rawvideo)", HRESULT_FROM_WIN32(GetLastError()));
        return false;
    }

    // Start an overlapped ConnectNamedPipe (so CreateFile won't block awkwardly).
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent)
    {
        CloseHandle(hWriteOverlapped);
        hWriteOverlapped = INVALID_HANDLE_VALUE;
        return false;
    }

    BOOL connOk = ConnectNamedPipe(hWriteOverlapped, &ov);
    DWORD connErr = connOk ? ERROR_SUCCESS : GetLastError();
    if (!connOk && connErr != ERROR_IO_PENDING && connErr != ERROR_PIPE_CONNECTED)
    {
        log_hr(L"ConnectNamedPipe(rawvideo)", HRESULT_FROM_WIN32(connErr));
        CloseHandle(ov.hEvent);
        CloseHandle(hWriteOverlapped);
        hWriteOverlapped = INVALID_HANDLE_VALUE;
        return false;
    }

    // Client (reader) end - inheritable for mpv stdin
    // Retry briefly in case pipe is "busy" during connect timing.
    for (int i = 0; i < 20; ++i)
    {
        hReadInheritable = CreateFileW(
            pipeNameOut.c_str(),
            GENERIC_READ,
            0,
            &saInherit,
            OPEN_EXISTING,
            0,
            nullptr
        );
        if (hReadInheritable != INVALID_HANDLE_VALUE)
            break;

        DWORD e = GetLastError();
        if (e == ERROR_PIPE_BUSY)
        {
            WaitNamedPipeW(pipeNameOut.c_str(), 50);
            continue;
        }
        log_hr(L"CreateFileW(rawvideo client)", HRESULT_FROM_WIN32(e));
        break;
    }

    if (hReadInheritable == INVALID_HANDLE_VALUE)
    {
        // cancel connect and cleanup
        CancelIoEx(hWriteOverlapped, &ov);
        CloseHandle(ov.hEvent);
        CloseHandle(hWriteOverlapped);
        hWriteOverlapped = INVALID_HANDLE_VALUE;
        return false;
    }

    // Finish connect if it was pending
    if (!connOk && connErr == ERROR_IO_PENDING)
    {
        DWORD wait = WaitForSingleObject(ov.hEvent, 2000);
        if (wait != WAIT_OBJECT_0)
        {
            // Never hang here: if mpv doesn't connect in time, cancel and fail cleanly.
            CancelIoEx(hWriteOverlapped, &ov);
            WaitForSingleObject(ov.hEvent, 200); // bounded
            SetLastError(ERROR_TIMEOUT);
            CloseHandle(ov.hEvent);
            CloseHandle(hReadInheritable);
            CloseHandle(hWriteOverlapped);
            hReadInheritable = INVALID_HANDLE_VALUE;
            hWriteOverlapped = INVALID_HANDLE_VALUE;
            return false;
        }
        DWORD dummy = 0;
        if (!GetOverlappedResult(hWriteOverlapped, &ov, &dummy, FALSE))
        {
            log_hr(L"GetOverlappedResult(ConnectNamedPipe)", HRESULT_FROM_WIN32(GetLastError()));
            CloseHandle(ov.hEvent);
            CloseHandle(hReadInheritable);
            CloseHandle(hWriteOverlapped);
            hReadInheritable = INVALID_HANDLE_VALUE;
            hWriteOverlapped = INVALID_HANDLE_VALUE;
            return false;
        }
    }

    CloseHandle(ov.hEvent);
    return true;
}

struct WatchParam {
    HANDLE hProc = nullptr;
    bool closeHandle = false;
};

static DWORD WINAPI mpv_watch_thread(LPVOID param)
{
    WatchParam* wp = (WatchParam*)param;
    HANDLE hProc = wp ? wp->hProc : nullptr;
    bool closeIt = wp ? wp->closeHandle : false;
    if (wp) delete wp;

    if (hProc) {
        WaitForSingleObject(hProc, INFINITE);
        if (closeIt) {
            CloseHandle(hProc);
        }
    }
    g_stop_requested.store(true);
    return 0;
}
// Pre-check FRC connectivity: build a temporary graph with the file source and BlueskyFRC,
// and attempt to connect src video out -> FRC input using direct IPin::Connect on all
// advertised media types. No RawSink, no mpv, no graph Run().
// If this fails, we know FRC is unusable for this file and can skip starting rawvideo mpv.
static HRESULT test_frc_connect(const std::wstring& inputPath, const std::wstring& frcName)
{
    log_line(L"[pre] testing FRC connectivity before starting mpv...");

    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) {
        log_hr(L"CoInitializeEx(pre-frc)", hr);
        return hr;
    }

    ComPtr<IGraphBuilder> graph;
    hr = CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER,
                          IID_IGraphBuilder, (void**)graph.put());
    if (FAILED(hr) || !graph) {
        log_hr(L"CoCreate(FilterGraph pre-frc)", hr);
        CoUninitialize();
        return FAILED(hr) ? hr : E_FAIL;
    }

    hr = graph->RenderFile(inputPath.c_str(), nullptr);
    if (FAILED(hr)) {
        log_hr(L"RenderFile(pre-frc)", hr);
        CoUninitialize();
        return hr;
    }
    log_line(L"[pre] RenderFile(pre-frc) done");

    // Try to mimic decoder selection policy used in the real graph (LAV vs DTV-DVD).
    // This reduces false negatives where the initial auto graph picks a different decoder.
    if (!g_is_ts_like_input) {
        try_force_lav_video_decoder(graph.get());
    } else {
        try_force_dtv_for_interlaced_mpeg2(graph.get());
    }

    // Mute any audio renderer branches so the probe graph will not produce audible output
    mute_graph_audio(graph.get());

    IBaseFilter* srcVideoFilter = nullptr;
    IPin* srcOut = nullptr;
    hr = find_connected_video_out_pin(graph.get(), &srcVideoFilter, &srcOut);
    if (FAILED(hr) || !srcOut) {
        log_hr(L"find_connected_video_out_pin(pre-frc)", hr);
        if (srcOut) srcOut->Release();
        if (srcVideoFilter) srcVideoFilter->Release();
        CoUninitialize();
        return FAILED(hr) ? hr : E_FAIL;
    }
    log_line(L"[pre] Found connected video output pin (pre-frc)");

    // Detach from any downstream filters so we can insert FRC in between
    ComPtr<IPin> downstream;
    if (SUCCEEDED(srcOut->ConnectedTo(downstream.put())) && downstream) {
        graph->Disconnect(srcOut);
        graph->Disconnect(downstream.get());
        log_line(L"[pre] Disconnected src video out from downstream (pre-frc)");
    }

    IBaseFilter* frc = nullptr;
    log_line(L"[pre] Add BlueskyFRC by friendly-name substring (pre-frc)");
    HRESULT hrF = add_registered_filter_by_name(graph.get(), frcName, &frc);
    if (FAILED(hrF) || !frc) {
        log_hr(L"add_registered_filter_by_name(BlueskyFRC pre-frc)", FAILED(hrF) ? hrF : HRESULT_FROM_WIN32(ERROR_NOT_FOUND));
        if (srcOut) srcOut->Release();
        if (srcVideoFilter) srcVideoFilter->Release();
        CoUninitialize();
        return FAILED(hrF) ? hrF : HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    }

    IPin* frcIn  = find_pin(frc, PINDIR_INPUT, 0);
    if (!frcIn) {
        log_line(L"[pre][err] BlueskyFRC input pin not found (pre-frc)");
        if (frc) frc->Release();
        if (srcOut) srcOut->Release();
        if (srcVideoFilter) srcVideoFilter->Release();
        CoUninitialize();
        return E_FAIL;
    }

    log_line(L"[pre] try Connect src->FRC (direct mediatypes, pre-frc)");
    hr = connect_by_out_mediatypes(srcOut, frcIn);
    frcIn->Release();

    if (FAILED(hr)) {
        log_hr(L"[pre] src->FRC connect(pre-frc)", hr);
    } else {
        log_line(L"[pre] src->FRC connect OK (pre-frc)");

        // Try to inspect BlueskyFRC's negotiated *output* media type so that we can
        // later match mpv's rawvideo framerate to it (2x, 3x, etc.).
        IPin* frcOut = find_pin(frc, PINDIR_OUTPUT, 0);
        if (frcOut) {
            // Ensure the FRC output pin is actually connected so that
            // ConnectionMediaType() returns a valid VIDEOINFOHEADER/2.
            ComPtr<IBaseFilter> nullR;
            HRESULT hrNR = create_filter_by_clsid_string(
                graph.get(),
                L"{C1F400A4-3F08-11D3-9F0B-006008039E37}", // Null Renderer
                L"NullRenderer(pre-frc)",
                nullR.put()
            );
            if (SUCCEEDED(hrNR) && nullR) {
                IPin* nullIn = find_pin(nullR.get(), PINDIR_INPUT, 0);
                if (nullIn) {
                    HRESULT hrConn = graph->Connect(frcOut, nullIn);
                    if (SUCCEEDED(hrConn)) {
                        log_line(L"[pre] FRC->NullRenderer connect OK (pre-frc)");
                    } else {
                        log_hr(L"[pre] FRC->NullRenderer connect failed (pre-frc)", hrConn);
                    }
                    nullIn->Release();
                } else {
                    log_line(L"[pre] NullRenderer input pin not found (pre-frc)");
                }
            } else {
                log_hr(L"[pre] create NullRenderer(pre-frc)", hrNR);
            }

            AM_MEDIA_TYPE frcMT{};
            HRESULT hrMT = frcOut->ConnectionMediaType(&frcMT);
            if (SUCCEEDED(hrMT) && frcMT.pbFormat && frcMT.cbFormat) {
                int w = 0, h = 0;
                int fpsNum = 0, fpsDen = 1;
                REFERENCE_TIME avg = 0;

                if (frcMT.formattype == FORMAT_VideoInfo2 &&
                    frcMT.cbFormat   >= sizeof(VIDEOINFOHEADER2))
                {
                    const VIDEOINFOHEADER2* vih2 =
                        reinterpret_cast<const VIDEOINFOHEADER2*>(frcMT.pbFormat);
                    parse_vih2(vih2, w, h);
                    avg = vih2->AvgTimePerFrame;
                } else if (frcMT.formattype == FORMAT_VideoInfo &&
                           frcMT.cbFormat   >= sizeof(VIDEOINFOHEADER))
                {
                    const VIDEOINFOHEADER* vih =
                        reinterpret_cast<const VIDEOINFOHEADER*>(frcMT.pbFormat);
                    parse_vih(vih, w, h);
                    avg = vih->AvgTimePerFrame;
                }

if (avg > 0) {
                    long long num = 10000000;         // 1 second in REFERENCE_TIME units
                    long long den = (long long)avg;   // AvgTimePerFrame
                    if (den <= 0) den = 1;

                    auto gcdll = [](long long a, long long b) -> long long {
                        if (a < 0) a = -a;
                        if (b < 0) b = -b;
                        while (b) {
                            long long t = a % b;
                            a = b;
                            b = t;
                        }
                        return (a == 0) ? 1 : a;
                    };

                    long long g = gcdll(num, den);
                    num /= g;
                    den /= g;

                    if (num > 0 && den > 0) {
                        g_frc_output_fps_detected = true;
                        g_frc_output_fps_num = (int)num;
                        g_frc_output_fps_den = (int)den;
                        logf(L"[pre] detected FRC output fps = %lld/%lld (approx %.3f fps) size=%dx%d\n",
                             num, den, (double)num / (double)den, w, h);
                    }
                }

                FreeMediaType(frcMT);
            } else {
                log_hr(L"[pre] frcOut->ConnectionMediaType failed", hrMT);
            }
            frcOut->Release();
        }
    }

    if (frc) frc->Release();
    if (srcOut) srcOut->Release();
    if (srcVideoFilter) srcVideoFilter->Release();
    CoUninitialize();
    return hr;
}

int wmain(int argc, wchar_t** argv)
{
    // Logging: default OFF.
    // Enable by adding: log=yes  (or log=on / log=1). Disable explicitly with: log=no.
    // If log is explicitly disabled (log=no), we also delete old log files so users don't get confused.
    bool logEnabled = false;
    bool logExplicit = false;
    for (int i = 1; i < argc; ++i) {
        bool tmp = logEnabled;
        if (parse_log_arg(argv[i], tmp)) {
            logEnabled = tmp;
            logExplicit = true;
        }
    }
    g_log_enabled = logEnabled;
    if (!g_log_enabled && logExplicit) {
        DeleteFileW(L"dsfrc.log");
        DeleteFileW(L"mpv-dsfrc.log");
    }
    if (g_log_enabled) {
        // Write logs to current working directory for portability (same folder you run from).
        FILE* f = _wfopen(L"dsfrc.log", L"w");
        if (f) {
            g_log = f;
        } else {
            // If file cannot be created, fall back to stderr.
            g_log = stderr;
        }
    } else {
        g_log = nullptr;
    }

kill_other_dsfrc_instances();

    std::wstring input;
    bool frcOn = false;
    OutFmt outFmt = OutFmt::BGRA; // default
    std::wstring frcName = L"Bluesky";
    int forcedW = 0, forcedH = 0;

    for (int i = 1; i < argc; ++i)
    {
        std::wstring a = argv[i];
        if (a.rfind(L"--frc=", 0) == 0)
        {
            std::wstring v = a.substr(6);
            frcOn = (v == L"on" || v == L"1" || v == L"true");
        }
        else if (a.rfind(L"--out=", 0) == 0)
        {
            std::wstring v = a.substr(6);
            outFmt = (v == L"nv12" || v == L"NV12") ? OutFmt::NV12 : OutFmt::BGRA;
        }
        else if (a.rfind(L"--frc-name=", 0) == 0)
        {
            frcName = a.substr(11);
        }
        else if (a.rfind(L"--size=", 0) == 0)
        {
            std::wstring sz = a.substr(7);
            if (!parse_size(sz, forcedW, forcedH))
            {
                logf(L"[warn] bad --size=%s\n", sz.c_str());
                forcedW = forcedH = 0;
            }
        }
        else if (a.rfind(L"--audio-delay-extra=", 0) == 0)
        {
            std::wstring v = a.substr(20);
            double val = 0.0;
            if (!v.empty())
            {
                wchar_t* endp = nullptr;
                val = wcstod(v.c_str(), &endp);
            }
            g_audio_delay_extra = val;
            logf(L"[opts] audio-delay-extra=%.3f sec\n", g_audio_delay_extra);
        }
        else if (parse_log_arg(a, logEnabled))
        {
            // already handled above; do not treat as input
            g_log_enabled = logEnabled;
            // Note: dsfrc.log is opened only once at startup.
        }
        else if (!a.empty() && a[0] != L'-' && input.empty())
        {
            input = a;
        }
    }

    if (input.empty())
    {
        usage();
        return 2;
    }

    // Remember whether this looks like a TS/ISDB transport stream so that
    // decoder selection logic (LAV vs Microsoft DTV-DVD) can make a
    // per-file decision without command-line switches.
    g_is_ts_like_input = is_ts_like_path(input);
    if (g_is_ts_like_input) {
        log_line(L"[info] input is TS-like (.ts/.m2ts/.mts); enabling TS-specific decoder policy");
    }

    int probeW = 0, probeH = 0;
    int fpsNum = 0, fpsDen = 1;
    if (probe_input_video_info(input, probeW, probeH, fpsNum, fpsDen)) {
        logf(L"[probe] input video %dx%d, avg-fps ~ %d/%d\n",
             probeW, probeH, fpsNum, fpsDen);
    } else {
        log_line(L"[probe] could not detect input video size/fps; using defaults if needed.");
    }
    // Decide output size.
    // If --size=WxH is not specified, try to use the probed input size.
    if (forcedW <= 0 || forcedH <= 0)
    {
        if (probeW > 0 && probeH > 0) {
            forcedW = probeW;
            forcedH = probeH;
            logf(L"[info] auto size %dx%d from DirectShow probe\n", forcedW, forcedH);
        } else {
            // fallback to 1920x1080 (typical FHD output)
            forcedW = 1920;
            forcedH = 1080;
            log_line(L"[warn] --size=WxH not provided and auto-detect failed. Using default 1920x1080.");
        }
    }
    else
    {
        logf(L"[info] using forced size %dx%d\n", forcedW, forcedH);
    }

    // 2025-12: special-case MPEG2 TS that reports 1472x1080 but is logically 1440x1080.
    // We keep the upstream DirectShow graph as-is and only crop to 1440x1080 on our side
    // when writing rawvideo to mpv.
    if (forcedW == 1472 && forcedH == 1080) {
        log_line(L"[info] treating 1472x1080 as 1440x1080 active area (crop 16px left/right)");
        forcedW = 1440;
        // forcedH stays 1080
    }

    // Decide fps string for mpv (supports FRC on/off).
    // Decide fps string for mpv (supports FRC on/off).
// Decide fps string for mpv (supports FRC on/off).
    // Pre-check FRC connectivity to avoid audio glitches / hangs when FRC cannot be used.
// If the pre-check fails, we immediately fall back to normal mpv playback (no rawvideo pipe).
    bool frc_pre_ok = true;
    if (frcOn) {
        HRESULT pre = test_frc_connect(input, frcName);
        frc_pre_ok = SUCCEEDED(pre);
        if (!frc_pre_ok) {
            log_hr(L"[pre] test_frc_connect", pre);
            log_line(L"[pre] FRC pre-check failed; will fall back to normal mpv playback.");
        } else {
            log_line(L"[pre] FRC pre-check OK; proceeding with FRC graph.");
        }
    }



    std::wstring fpsOpt;
    if (!frcOn) {
        // No FRC: just use the source fps if known, otherwise fall back to 24p-ish.
        if (fpsNum > 0 && fpsDen > 0) {
            logf(L"[info] using source fps %d/%d (FRC off)\n", fpsNum, fpsDen);
            fpsOpt = std::to_wstring(fpsNum) + L"/" + std::to_wstring(fpsDen);
        } else {
            fpsOpt = L"24000/1001";
            log_line(L"[warn] auto fps detect failed; fallback to 24000/1001 for FRC-off.");
        }
    } else {
        // FRC ON: if we were able to probe BlueskyFRC's output fps in test_frc_connect,
        // prefer that so mpv exactly matches the FRC output (48/50/60/etc).
        if (g_frc_output_fps_detected && g_frc_output_fps_num > 0 && g_frc_output_fps_den > 0) {
            // Optionally sanity-check against source fps. Even if it differs, trusting
            // FRC is almost always what we want.
            if (fpsNum > 0 && fpsDen > 0) {
                double base_src = (double)fpsNum / (double)fpsDen;
                double base_frc = (double)g_frc_output_fps_num / (double)g_frc_output_fps_den;
                double ratio    = (base_src > 0.0) ? (base_frc / base_src) : 0.0;

                // If FRC is roughly 2x or 3x (common patterns), log that fact for debugging.
                if (base_src > 0.0) {
                    double k2 = ratio;
                    logf(L"[info] FRC output vs source: src=%.3f, frc=%.3f (~%.2fx)\n",
                         base_src, base_frc, k2);
                }
            } else {
                logf(L"[info] FRC output fps detected = %d/%d (source fps unknown)\n",
                     g_frc_output_fps_num, g_frc_output_fps_den);
            }

            {
                // Decide mpv's fps based on FRC output, source fps, and resolution (SD vs HD).
                double base_src = 0.0;
                if (fpsNum > 0 && fpsDen > 0) {
                    base_src = (double)fpsNum / (double)fpsDen;
                }
                double base_frc = 0.0;
                if (g_frc_output_fps_den != 0) {
                    base_frc = (double)g_frc_output_fps_num / (double)g_frc_output_fps_den;
                }

                bool isSD = false;
                // Prefer probed input size if available, otherwise fall back to forced size.
                if (probeW > 0 && probeH > 0) {
                    isSD = (probeW <= 736 && probeH <= 576);
                } else if (forcedW > 0 && forcedH > 0) {
                    isSD = (forcedW <= 736 && forcedH <= 576);
                }

                int outNum = g_frc_output_fps_num;
                int outDen = g_frc_output_fps_den;

                if (base_src > 29.0 && base_src < 31.0 &&
                    base_frc > 47.0 && base_frc < 50.0) {
                    if (isSD) {
                        // Typical DVD/SD telecine case: keep 48fps.
                        outNum = 48000;
                        outDen = 1001;
                        logf(L"[info] FRC: 29.97fps SD input with ~48fps output → using 48000/1001 for mpv.\n");
                    } else {
                        // HD 29.97p case: treat as 60fps FRC output.
                        outNum = 60000;
                        outDen = 1001;
                        logf(L"[info] FRC: 29.97fps HD input with ~48fps output → using 60000/1001 for mpv.\n");
                    }
                } else {
                    logf(L"[info] using FRC output fps as-is: %d/%d (~ %.3f)\n",
                         g_frc_output_fps_num, g_frc_output_fps_den, base_frc);
                }

                fpsOpt = std::to_wstring(outNum) + L"/" + std::to_wstring(outDen);
            }
        } else if (fpsNum > 0 && fpsDen > 0) {
            int num = fpsNum * 2; // fallback assumption: FRC is 2x
            int den = fpsDen;
            logf(L"[info] assuming 2x FRC output fps -> %d/%d (no direct probe)\n", num, den);
            fpsOpt = std::to_wstring(num) + L"/" + std::to_wstring(den);
        } else {
            fpsOpt = L"48000/1001";
            log_line(L"[warn] auto fps detect failed; fallback to 48000/1001 for FRC-on.");
        }
    }

    // Determine mpv.exe path in the same directory as this exe
    wchar_t exePath[MAX_PATH];
    DWORD len = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring exeDir;
    if (len > 0)
    {
        exeDir.assign(exePath, exePath + len);
        size_t pos = exeDir.find_last_of(L"\\/");
        if (pos != std::wstring::npos)
            exeDir.erase(pos + 1);
    }

    std::wstring mpvPath = exeDir + L"mpv.exe";

    // If FRC was requested but the pre-check failed, do NOT start rawvideo mpv.
    // This restores the "fallback to normal playback" behavior and avoids leaving dsfrc_pipe.exe stuck.
    if (frcOn && !frc_pre_ok) {
        std::wstring aspectOverride = (g_has_aspect_signal && !g_mpv_aspect_override.empty()) ? g_mpv_aspect_override : L"";
        return launch_normal_mpv(mpvPath, exeDir, input, aspectOverride);
    }

    // reset IPC flags per run
    g_mpv_ipc_enabled = false;
    g_mpv_audio_delay_sent = false;
    g_mpv_ipc_name.clear();

    if (frcOn) {
        wchar_t ipcBuf[128];
        DWORD pid = GetCurrentProcessId();
        _snwprintf_s(ipcBuf, _TRUNCATE, L"\\\\.\\pipe\\mpv-dsfrc-%lu", (unsigned long)pid);
        g_mpv_ipc_name.assign(ipcBuf);
        g_mpv_ipc_enabled = true;
        logf(L"[ipc] using mpv IPC server %s\n", g_mpv_ipc_name.c_str());
    }

    // For TS-like inputs, mpv may stay silent because audio PTS starts far from 0.
    // When we feed video as rawvideo via stdin, we therefore normalize TS audio by remuxing.
    std::wstring mpvAudioPath = input;
    std::wstring mpvAudioTempPath;
    if (frcOn && is_ts_like_path(input)) {
        std::wstring ffmpegPath = find_ffmpeg_nearby(exeDir);
        if (!ffmpegPath.empty()) {
            log_line(L"[ts] TS-like input detected; remuxing audio timestamps to start at 0...");
            if (run_ffmpeg_remux_audio_zero(ffmpegPath, input, mpvAudioTempPath)) {
                mpvAudioPath = mpvAudioTempPath;
                logf(L"[ts] using remuxed audio-file for mpv: %s\n", mpvAudioPath.c_str());
            } else {
                log_line(L"[ts][warn] remux failed; using original input as --audio-file (may be silent).");
            }
        } else {
            log_line(L"[ts][warn] ffmpeg.exe not found (portable or PATH). Using original input as --audio-file (may be silent).");
        }
    }

    // Build mpv command line
    // IMPORTANT: Always close the initial quote around mpvPath.
    // If we forget to close it (e.g. when log is OFF), CreateProcessW can fail and we end up
    // falling back to normal playback (which looks like "FRC stopped working").
    std::wstring cmd = L"\"";
    cmd += mpvPath;
    cmd += L"\" ";
    if (g_log_enabled) {
        cmd += L"--log-file=\"mpv-dsfrc.log\" ";
    }
    if (frcOn) {
        // Low-latency / sync-focused options when FRC is active
        cmd += L"--profile=low-latency ";
        // cmd += L"--untimed=yes "; // disabled for tear-free vsync
        cmd += L"--cache=no ";
        cmd += L"--demuxer-readahead-secs=0 ";
        cmd += L"--video-sync=audio ";
        cmd += L"--d3d11-sync-interval=1 ";
        cmd += L"--swapchain-depth=3 ";
        cmd += L"--video-timing-offset=0.12 ";
        cmd += L"--autosync=30 ";
        cmd += L"--audio-buffer=0 ";
        cmd += L"--audio-samplerate=48000 ";
        cmd += L"--audio-channels=auto-safe ";
        cmd += L"--interpolation=no ";
        cmd += L"--vd-queue-enable=yes ";
        cmd += L"--vd-queue-max-secs=0.12 ";
        cmd += L"--vd-queue-max-samples=8 ";
        if (g_mpv_ipc_enabled && !g_mpv_ipc_name.empty()) {
            cmd += L"--input-ipc-server=\"";
            cmd += g_mpv_ipc_name;
            cmd += L"\" ";
        }
    }

    // Local option block so that audio-file + rawvideo demuxer apply
    // only to the first stdin entry ("-"). This avoids later dropped-in
    // files in the same mpv window inheriting rawvideo + audio-file,
    // which caused sandstorm/green video and wrong audio.
    cmd += L"--{" L" ";
    if (g_has_aspect_signal && !g_mpv_aspect_override.empty()) {
        cmd += L"--video-aspect-override=";
        cmd += g_mpv_aspect_override;
        cmd += L" ";
    }
    cmd += L"--audio-file=\"";
    cmd += mpvAudioPath;
    cmd += L"\" ";
    cmd += L"--demuxer=rawvideo ";
    cmd += L"--demuxer-rawvideo-w=" + std::to_wstring(forcedW) + L" ";
    cmd += L"--demuxer-rawvideo-h=" + std::to_wstring(forcedH) + L" ";
    cmd += L"--demuxer-rawvideo-mp-format=";
    cmd += (outFmt == OutFmt::NV12 ? L"nv12 " : L"bgra ");
    cmd += L"--demuxer-rawvideo-fps=";
    cmd += fpsOpt;
    cmd += L" ";
    cmd += L"- ";
    cmd += L"--}";

    logf(L"[info] launching mpv: %s\n", cmd.c_str());

    // Create rawvideo pipe for mpv stdin (named pipe + OVERLAPPED writes with timeout)
    HANDLE hRead = INVALID_HANDLE_VALUE;
    HANDLE hWrite = INVALID_HANDLE_VALUE;
    std::wstring rawPipeName;
    if (!create_rawvideo_pipe_for_mpv(hRead, hWrite, rawPipeName))
    {
        log_line(L"[err] create_rawvideo_pipe_for_mpv failed");
        return 1;
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = hRead;
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);

    PROCESS_INFORMATION pi{};
    HANDLE hWatch = nullptr;
    g_t_mpv_launch = std::chrono::high_resolution_clock::now();
    std::wstring cmdBuf = cmd;
    BOOL ok = CreateProcessW(
        nullptr,
        cmdBuf.data(),   // lpCommandLine (modifiable buffer)
        nullptr,
        nullptr,
        TRUE,            // inherit handles
        0,
        nullptr,
        exeDir.empty() ? nullptr : exeDir.c_str(),
        &si,
        &pi
    );
    if (!ok)
    {
        HRESULT hrCP = HRESULT_FROM_WIN32(GetLastError());
        log_hr(L"CreateProcessW(mpv.exe)", hrCP);
        CloseHandle(hRead);
        CloseHandle(hWrite);

        // If FRC was requested but we couldn't even start rawvideo mpv,
        // try normal playback as a fallback.
        if (frcOn) {
            std::wstring aspectOverride = (g_has_aspect_signal && !g_mpv_aspect_override.empty()) ? g_mpv_aspect_override : L"";
            return launch_normal_mpv(mpvPath, exeDir, input, aspectOverride);
        }
        return 1;
    }

    // Parent no longer needs read-end
    CloseHandle(hRead);

    // Watch mpv: if it exits, request graph stop so dsfrc_pipe doesn't linger.
    HANDLE hProcDup = nullptr;
    bool closeDup = false;
    if (DuplicateHandle(GetCurrentProcess(), pi.hProcess, GetCurrentProcess(), &hProcDup, SYNCHRONIZE, FALSE, 0)) {
        closeDup = true;
    }
    WatchParam* wp = new WatchParam();
    wp->hProc = closeDup ? hProcDup : pi.hProcess;
    wp->closeHandle = closeDup;

    hWatch = CreateThread(nullptr, 0, mpv_watch_thread, wp, 0, nullptr);
    if (!hWatch) {
        log_hr(L"CreateThread(mpv_watch_thread)", HRESULT_FROM_WIN32(GetLastError()));
        if (closeDup && hProcDup) CloseHandle(hProcDup);
        delete wp;
        wp = nullptr;
    }

    // Use write-end of pipe for RawSink
    g_raw_out = hWrite;
    g_raw_out_overlapped = true;
    HRESULT hr = build_and_run(input, frcOn, frcName, outFmt, forcedW, forcedH);

    // Close write-end to signal EOF to mpv
    CloseHandle(hWrite);
    g_raw_out = INVALID_HANDLE_VALUE;
    g_raw_out_overlapped = false;
    if (hWatch) { CloseHandle(hWatch); hWatch = nullptr; }


    // Wait a bit for mpv to exit.
    DWORD wait = WaitForSingleObject(pi.hProcess, 1000);
    if (wait == WAIT_TIMEOUT && FAILED(hr) && frcOn)
    {
        // FRC モードで失敗しているのに mpv がまだ生きている場合、
        // ここで rawvideo 用 mpv を強制終了して「音だけ残る」状態を防ぐ。
        log_line(L"[warn] FRC mode failed and mpv still running; terminating rawvideo mpv.");
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 500);
    }
    else if (wait == WAIT_TIMEOUT)
    {
        // 正常に動いているケースでは detach してユーザーに mpv を使い続けてもらう
        log_line(L"[warn] mpv still running; not waiting further (detaching).");
    }

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    // Cleanup TS-remuxed temp audio if mpv has already exited (or we terminated it).
    if (!mpvAudioTempPath.empty()) {
        if (wait != WAIT_TIMEOUT || (wait == WAIT_TIMEOUT && FAILED(hr) && frcOn)) {
            if (DeleteFileW(mpvAudioTempPath.c_str())) {
                log_line(L"[ts] deleted temp audio file.");
            } else {
                log_hr(L"[ts] DeleteFileW(temp audio)", HRESULT_FROM_WIN32(GetLastError()));
            }
        } else {
            log_line(L"[ts] keeping temp audio file because mpv was detached.");
        }
    }
    // FRC モードで失敗した場合は、「素の」mpv で元ファイルを再生し直す
    if (FAILED(hr) && frcOn && !input.empty())
    {
        log_line(L"[info] FRC playback failed. Launching normal mpv for the original file...");
        std::wstring aspectOverride = (g_has_aspect_signal && !g_mpv_aspect_override.empty()) ? g_mpv_aspect_override : L"";
        launch_normal_mpv(mpvPath, exeDir, input, aspectOverride);
    }

    int ret = FAILED(hr) ? 1 : 0;
    if (g_log_enabled && g_log && g_log != stderr) { fclose(g_log); g_log = nullptr; }
    return ret;
}