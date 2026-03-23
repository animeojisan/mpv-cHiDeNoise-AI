// dshowCap.cpp
// DirectShow でキャプチャデバイスからフレームを取得して stdout に流すキャプチャツール。
// mpv 側は --demuxer=rawvideo で受ける想定。
//
// 特徴:
//   - --list / --list-devices でビデオ入力デバイス一覧を表示
//   - ログは stderr に出力（シェルの「2> dshowCap.log」で保存可能）
//   - stdout には生フレームのみを出すので、そのまま mpv へパイプ可能
//   - 画面は出さず、Ctrl+C でキャプチャ終了
//
// ビルド例 (VS x64 Native Tools コマンドプロンプト):
//   cl /utf-8 /std:c++17 /EHsc dshowCap.cpp ^
//       /Fe:dshowCap.exe ^
//       strmiids.lib ole32.lib uuid.lib

#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <dshow.h>

#include <cstdio>
#include <cstdarg>
#include <fcntl.h>
#include <io.h>

#include <iostream>
#include <string>
#include <vector>
#include <sstream>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <cmath>

#pragma comment(lib, "strmiids.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "user32.lib")

// -------------------------
// SampleGrabber / NullRenderer 用 CLSID/UUID 定義
// -------------------------

// {C1F400A0-3F08-11d3-9F0B-006008039E37}
static const CLSID CLSID_SampleGrabber =
{ 0xC1F400A0, 0x3F08, 0x11D3,{ 0x9F, 0x0B, 0x00, 0x60, 0x08, 0x03, 0x9E, 0x37 } };

// {6B652FFF-11FE-4fce-92AD-0266B5D7C78F}
static const IID IID_ISampleGrabber =
{ 0x6B652FFF, 0x11FE, 0x4FCE,{ 0x92, 0xAD, 0x02, 0x66, 0xB5, 0xD7, 0xC7, 0x8F } };

// {0579154A-2B53-4994-B0D0-E773148EFF85}
static const IID IID_ISampleGrabberCB =
{ 0x0579154A, 0x2B53, 0x4994,{ 0xB0, 0xD0, 0xE7, 0x73, 0x14, 0x8E, 0xFF, 0x85 } };

// Null Renderer CLSID（DirectShow BaseClasses / quartz.dll）
// {C1F400A4-3F08-11d3-9F0B-006008039E37}
static const CLSID CLSID_NullRenderer =
{ 0xC1F400A4, 0x3F08, 0x11D3,{ 0x9F, 0x0B, 0x00, 0x60, 0x08, 0x03, 0x9E, 0x37 } };

// -------------------------
// FOURCC media subtype GUIDs (I420/YV12)
// -------------------------
// DirectShow の FOURCC subtype GUID は Data1 に FOURCC を詰め、残りは固定。
// 例: 'I420' => 0x30323449
static const GUID kMEDIASUBTYPE_I420 =
{ 0x30323449, 0x0000, 0x0010,{ 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };

// 'YV12' => 0x32315659
static const GUID kMEDIASUBTYPE_YV12 =
{ 0x32315659, 0x0000, 0x0010,{ 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };


struct ISampleGrabberCB : public IUnknown {
    virtual STDMETHODIMP SampleCB(double, IMediaSample*) = 0;
    virtual STDMETHODIMP BufferCB(double, BYTE*, long) = 0;
};

struct ISampleGrabber : public IUnknown {
    virtual STDMETHODIMP SetOneShot(BOOL) = 0;
    virtual STDMETHODIMP SetMediaType(const AM_MEDIA_TYPE*) = 0;
    virtual STDMETHODIMP GetConnectedMediaType(AM_MEDIA_TYPE*) = 0;
    virtual STDMETHODIMP SetBufferSamples(BOOL) = 0;
    virtual STDMETHODIMP GetCurrentBuffer(long*, long*) = 0;
    virtual STDMETHODIMP GetCurrentSample(IMediaSample**) = 0;
    virtual STDMETHODIMP SetCallback(ISampleGrabberCB*, long) = 0;
};

// -------------------------
// 簡易ログユーティリティ（stderr に出す）
// -------------------------

void log_printf(const wchar_t* fmt, ...)
{
    // NOTE:
    //  - Console に出す場合は WriteConsoleW で Unicode をそのまま表示
    //  - リダイレクト (2> dshowCap.log) の場合は UTF-8 で書き出す
    //    （従来の vfwprintf は CP_ACP 変換になり、日本語が '????' になりがち）

    va_list args;
    va_start(args, fmt);

    // ワイド文字列へフォーマット（args は一度しか消費できないのでコピーを使う）
    va_list args2;
    va_copy(args2, args);
    int len = _vscwprintf(fmt, args2);
    va_end(args2);
    if (len <= 0) {
        va_end(args);
        return;
    }

    std::wstring wbuf;
    wbuf.resize((size_t)len + 1);
    vswprintf_s(wbuf.data(), wbuf.size(), fmt, args);
    va_end(args);

    HANDLE hErr = GetStdHandle(STD_ERROR_HANDLE);
    if (hErr && hErr != INVALID_HANDLE_VALUE) {
        // stderr が console なら Unicode のまま表示
        DWORD mode = 0;
        if (GetConsoleMode(hErr, &mode)) {
            DWORD written = 0;
            WriteConsoleW(hErr, wbuf.c_str(), (DWORD)wcslen(wbuf.c_str()), &written, nullptr);
            return;
        }

        // stderr がファイル/パイプなら UTF-8 で書き出す
        int need = WideCharToMultiByte(CP_UTF8, 0, wbuf.c_str(), -1, nullptr, 0, nullptr, nullptr);
        if (need > 1) {
            // need は終端NULを含むバイト数
            std::string u8;
            u8.resize((size_t)need);
            WideCharToMultiByte(CP_UTF8, 0, wbuf.c_str(), -1, u8.data(), need, nullptr, nullptr);
            // NUL 終端は書き出さない
            if (!u8.empty() && u8.back() == '\0') {
                u8.pop_back();
            }
            DWORD written = 0;
            WriteFile(hErr, u8.data(), (DWORD)u8.size(), &written, nullptr);
            return;
        }
    }

    // フォールバック（最悪の場合）
    fputws(wbuf.c_str(), stderr);
    fflush(stderr);
}

// -------------------------
// グローバル設定
// -------------------------

struct CaptureConfig {
    std::wstring deviceName;
    long width  = 1280;
    long height = 720;
    long fpsNum = 30;
    long fpsDen = 1;
    std::wstring audioDeviceName;  // 音声キャプチャ用デバイス名 (mpv 用)
    std::wstring mpvPath;          // mpv.exe のパス指定（省略時は "mpv.exe"）
    std::wstring pipeName;         // 出力先 Named Pipe 名 (空なら stdout を使用)

    // Listing modes
    bool listDevicesOnly = false; // legacy --list / --list-devices (video)
    bool listVideo = false;       // --list-video
    bool listAudio = false;       // --list-audio
    bool json = false;            // --json
    bool quiet = false;           // --quiet
    bool queryDefaultFormat = false; // --get-default-format
};

static std::atomic<bool> g_running{ true };
// -------------------------
// stdout 書き込みの非同期化（SampleGrabber のスレッドをブロックしない）
// -------------------------
struct FrameSlot {
    std::vector<unsigned char> buf;
    long size = 0;
    uint64_t id = 0;
    bool ready = false;
    bool in_use = false;
};

static std::mutex g_q_mtx;
static std::condition_variable g_q_cv;

// Connected media subtype and conversion flags (OBS Virtual Camera often outputs I420/YV12)
static GUID g_connectedSubtype = GUID_NULL;
static int g_vid_w = 0;
static int g_vid_h = 0;
static std::atomic<uint64_t> g_cb_frames{0};
static std::atomic<uint64_t> g_in_frames{0}; // callback total (before decimation)
static std::atomic<double>   g_connected_fps{0.0};
static int g_decimate = 1; // if >1, write every Nth frame
static bool g_needYUY2_to_nv12 = false;
static std::atomic<uint64_t> g_written_frames{0};
static std::atomic<uint64_t> g_dropped_frames{0};
static bool g_need420_to_nv12 = false;
static bool g_uvSwap = false; // YV12 needs U/V swap

static FrameSlot g_slots[4];
static uint64_t g_next_frame_id = 0;
static uint64_t g_latest_frame_id = 0;
static std::atomic<uint64_t> g_drop_frames{ 0 };

static std::thread g_writer_thread;
static std::atomic<bool> g_writer_started{ false };
static std::atomic<bool> g_use_async_writer{ false };

static void WriterThreadProc()
{
    uint64_t last_written = 0;
    while (g_running.load()) {
        FrameSlot* slot = nullptr;
        uint64_t sid = 0;
        long len = 0;
        const unsigned char* ptr = nullptr;

        {
            std::unique_lock<std::mutex> lk(g_q_mtx);
            // 新しいフレームが来るまで待つ（spurious wakeup もあるので while）
            while (g_running.load() && g_latest_frame_id <= last_written) {
                g_q_cv.wait(lk);
            }
            if (!g_running.load()) break;

            // 「最新」を優先して書く（古いものは捨てても良い）
            uint64_t best_id = 0;
            int best_i = -1;
            for (int i = 0; i < (int)(sizeof(g_slots) / sizeof(g_slots[0])); i++) {
                if (g_slots[i].ready && !g_slots[i].in_use && g_slots[i].id > best_id) {
                    best_id = g_slots[i].id;
                    best_i = i;
                }
            }
            if (best_i < 0) {
                // 何も用意できていない（race）場合は待ち直し
                continue;
            }

            g_slots[best_i].in_use = true;
            slot = &g_slots[best_i];
            sid = slot->id;
            len = slot->size;
            ptr = slot->buf.data();
        }

        // ここで stdout に書く（mpv 側が詰まっても SampleGrabber スレッドは止めない）
        long total = 0;
        while (g_running.load() && total < len) {
            int w = _write(_fileno(stdout), ptr + total, (unsigned int)(len - total));
            if (w <= 0) {
                // 受け側がいない / パイプ切断など
                g_running = false;
                break;
            }
            total += w;
        }

        {
            std::lock_guard<std::mutex> lk(g_q_mtx);
            if (slot) {
                slot->in_use = false;
                slot->ready = false;
            }
        }

        last_written = sid;
    }
}

static void StartWriterThread()
{
    bool expected = false;
    if (!g_writer_started.compare_exchange_strong(expected, true)) {
        return;
    }
    g_writer_thread = std::thread(WriterThreadProc);
}

static void StopWriterThread()
{
    // まず停止フラグ
    g_running = false;
    // 待機解除
    g_q_cv.notify_all();
    if (g_writer_started.load() && g_writer_thread.joinable()) {
        g_writer_thread.join();
    }
    g_writer_started = false;
}

// Ctrl+C で終了
BOOL WINAPI ConsoleCtrlHandler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT ||
        type == CTRL_CLOSE_EVENT || type == CTRL_SHUTDOWN_EVENT) {
        g_running = false;
        return TRUE;
    }
    return FALSE;
}

// -------------------------
// mpv(音声専用) 起動ヘルパー
// -------------------------

static bool LaunchAudioMpv(const CaptureConfig& cfg, PROCESS_INFORMATION* pOutProcess)
{
    if (cfg.audioDeviceName.empty()) {
        return false;
    }

    std::wstring mpvPath = cfg.mpvPath.empty() ? L"mpv.exe" : cfg.mpvPath;

    // デバイス名にクォートが含まれているとコマンドラインが壊れるので念のため置き換え
    std::wstring dev = cfg.audioDeviceName;
    for (auto& ch : dev) {
        if (ch == L'"') {
            ch = L'_';
        }
    }

    std::wstringstream ss;
    ss << L"\"" << mpvPath << L"\""
       << L" --vo=null --keep-open=yes "
       << L"\"av://dshow:audio=" << dev << L"\""
       << L" --profile=low-latency --no-cache --demuxer-readahead-secs=0"
       << L" --demuxer=lavf --demuxer-lavf-o-add=fflags=+nobuffer"
       << L" --demuxer-lavf-o-add=rtbufsize=15M --demuxer-lavf-o-add=audio_buffer_size=6"
       << L" --audio-buffer=0 --untimed=yes --video-sync=desync --interpolation=no"
       << L" --ao=wasapi --audio-exclusive=no --mute=no --volume=100";

    std::wstring cmd = ss.str();

    STARTUPINFOW si{};
    PROCESS_INFORMATION pi{};
    si.cb = sizeof(si);

    DWORD flags = CREATE_NO_WINDOW; // 黒いコンソールを出さない

    // CreateProcessW は書き換え可能なバッファを要求する
    std::wstring cmdBuf = cmd;
    if (!CreateProcessW(
            nullptr,              // lpApplicationName
            cmdBuf.data(),        // lpCommandLine
            nullptr, nullptr,     // セキュリティ属性
            FALSE,                // ハンドル継承
            flags,                // 作成フラグ
            nullptr,              // 環境変数
            nullptr,              // カレントディレクトリ
            &si,
            &pi))
    {
        DWORD err = GetLastError();
        log_printf(L"CreateProcessW for mpv audio failed (err=%lu)\n", err);
        return false;
    }

    log_printf(L"Launched mpv audio: %s\n", cmd.c_str());

    if (pOutProcess) {
        *pOutProcess = pi; // 呼び出し側で CloseHandle する
    } else {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }

    return true;
}


// Convert planar 4:2:0 (I420/YV12) to NV12.
// Input layout:
//  - I420: Y plane (W*H), then U (W*H/4), then V (W*H/4)
//  - YV12: Y plane, then V, then U
static void convert_420_planar_to_nv12(uint8_t* dstNV12, const uint8_t* src420, int w, int h, bool uvSwap)
{
    // Y
    const size_t ysz = (size_t)w * (size_t)h;
    memcpy(dstNV12, src420, ysz);

    const uint8_t* pU = src420 + ysz;
    const uint8_t* pV = src420 + ysz + (ysz / 4);
    if (uvSwap) { // YV12
        const uint8_t* tmp = pU; pU = pV; pV = tmp;
    }

    uint8_t* uv = dstNV12 + ysz;
    const size_t csz = ysz / 4; // number of U samples (same as V)
    for (size_t i = 0; i < csz; ++i) {
        uv[i * 2 + 0] = pU[i];
        uv[i * 2 + 1] = pV[i];
    }

}


// Convert packed YUY2 (YUYV 4:2:2) to NV12 (4:2:0).
// Output uses simple vertical averaging for chroma.
static void convert_yuy2_to_nv12(uint8_t* dstNV12, const uint8_t* srcYUY2, int w, int h)
{
    const int srcStride = w * 2;
    const size_t ysz = (size_t)w * (size_t)h;
    uint8_t* dstY = dstNV12;
    uint8_t* dstUV = dstNV12 + ysz;

    // Y plane
    for (int y = 0; y < h; ++y) {
        const uint8_t* s = srcYUY2 + (size_t)y * srcStride;
        uint8_t* d = dstY + (size_t)y * w;
        for (int x = 0; x < w; x += 2) {
            d[x + 0] = s[x * 2 + 0]; // Y0
            d[x + 1] = s[x * 2 + 2]; // Y1
        }
    }

    // UV plane (subsampled 2x2)
    for (int y = 0; y < h; y += 2) {
        const uint8_t* s0 = srcYUY2 + (size_t)y * srcStride;
        const uint8_t* s1 = (y + 1 < h) ? (srcYUY2 + (size_t)(y + 1) * srcStride) : s0;
        uint8_t* uv = dstUV + (size_t)(y / 2) * w;
        for (int x = 0; x < w; x += 2) {
            // For each pair, YUY2 is: Y0 U0 Y1 V0
            int u0 = s0[x * 2 + 1];
            int v0 = s0[x * 2 + 3];
            int u1 = s1[x * 2 + 1];
            int v1 = s1[x * 2 + 3];
            uv[x + 0] = (uint8_t)((u0 + u1) >> 1);
            uv[x + 1] = (uint8_t)((v0 + v1) >> 1);
        }
}
}

// -------------------------
// SampleGrabber コールバック
// -------------------------

class GrabberCallback : public ISampleGrabberCB {
public:
    GrabberCallback() : m_ref(1) {
        // stdout をバイナリモードに
        _setmode(_fileno(stdout), _O_BINARY);
    }

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_ISampleGrabberCB) {
            *ppv = static_cast<ISampleGrabberCB*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override {
        return (ULONG)InterlockedIncrement(&m_ref);
    }
    STDMETHODIMP_(ULONG) Release() override {
        LONG r = InterlockedDecrement(&m_ref);
        if (r == 0) delete this;
        return (ULONG)r;
    }

    // ISampleGrabberCB
    STDMETHODIMP SampleCB(double, IMediaSample*) override {
        return S_OK;
    }

    STDMETHODIMP BufferCB(double time, BYTE* pBuffer, long BufferLen) override {
        if (!pBuffer || BufferLen <= 0) return E_POINTER;

        static bool first = true;
        if (first) {
            first = false;
            log_printf(L"BufferCB: first frame, time=%0.3f, size=%ld bytes\n", time, BufferLen);
        }

        // OBS 以外のデバイスでは、従来どおり SampleGrabber のスレッドから
        // 直接 stdout へ書き出す（v21 と同じ動作）
        if (!g_use_async_writer.load()) {
            long total = 0;
            while (total < BufferLen && g_running.load()) {
                int written = _write(_fileno(stdout), pBuffer + total, BufferLen - total);
                if (written <= 0) {
                    // pipe/stdout side was closed (e.g. mpv capture window closed)
                    log_printf(L"BufferCB: _write failed (pipe closed?), written=%d\n", written);
                    // Stop the capture loop so dshowCap.exe and the audio mpv exit together.
                    g_running = false;
                    break;
                }
                total += written;
            }
            return S_OK;
        }

        // OBS Virtual Camera など、非同期 writer を使う経路では stats + 変換 + キュー投入を行う
        g_cb_frames.fetch_add(1, std::memory_order_relaxed);
        const uint64_t in_id = g_in_frames.fetch_add(1, std::memory_order_relaxed) + 1;
        if (g_decimate > 1 && (in_id % (uint64_t)g_decimate) != 0) {
            g_dropped_frames.fetch_add(1, std::memory_order_relaxed);
            return S_OK;
        }

        // If the graph outputs I420/YV12, convert to NV12 so mpv can stay on --demuxer-rawvideo-mp-format=nv12
        std::vector<uint8_t> converted;
        const uint8_t* src = reinterpret_cast<const uint8_t*>(pBuffer);
        long srcLen = BufferLen;

        if (g_need420_to_nv12 && g_vid_w > 0 && g_vid_h > 0) {
            const int w = g_vid_w;
            const int h = g_vid_h;
            // require even dims for 4:2:0
            if (((w & 1) == 0) && ((h & 1) == 0)) {
                const size_t need = (size_t)w * (size_t)h * 3 / 2;
                if ((size_t)BufferLen >= need) {
                    converted.resize(need);
                    convert_420_planar_to_nv12(converted.data(), src, w, h, g_uvSwap);
                    src = converted.data();
                    srcLen = (long)need;
                }
            }
        }



        
if (g_needYUY2_to_nv12 && g_vid_w > 0 && g_vid_h > 0) {
    const int w = g_vid_w;
    const int h = g_vid_h;
    if (((w & 1) == 0) && ((h & 1) == 0)) {
        const size_t outNeed = (size_t)w * (size_t)h * 3 / 2;
        const size_t inNeed  = (size_t)w * (size_t)h * 2; // YUY2
        if ((size_t)BufferLen >= inNeed) {
            converted.resize(outNeed);
            convert_yuy2_to_nv12(converted.data(), src, w, h);
            src = converted.data();
            srcLen = (long)outNeed;
        }
    }
}

// stdout への書き込みで DirectShow グラフが止まらないよう、キューに積んで別スレッドで書く
uint64_t my_id = 0;
int chosen = -1;
{
    std::lock_guard<std::mutex> lk(g_q_mtx);
    my_id = ++g_next_frame_id;

    // 書き込み中(in_use)でないスロットを探す。空きがなければ最古を上書き（=ドロップ）
    uint64_t oldest_id = UINT64_MAX;
    int oldest_i = -1;

    for (int i = 0; i < (int)(sizeof(g_slots) / sizeof(g_slots[0])); i++) {
        if (g_slots[i].in_use) continue;

        if (!g_slots[i].ready) {
            chosen = i;
            break;
        }
        if (g_slots[i].id < oldest_id) {
            oldest_id = g_slots[i].id;
            oldest_i = i;
        }
    }
    if (chosen < 0) chosen = oldest_i;

    if (chosen < 0) {
        // Queue is full; we'll overwrite the oldest frame (drop)
        g_dropped_frames.fetch_add(1, std::memory_order_relaxed);

        // 全部書き込み中で埋まっている（まれ）。このフレームは捨てる
        g_drop_frames++;
        return S_OK;
    }

    FrameSlot& s = g_slots[chosen];
    if ((long)s.buf.size() != srcLen) {
        s.buf.resize(srcLen);
    }
    memcpy(s.buf.data(), src, srcLen);
    s.size = srcLen;
    s.id = my_id;
    s.ready = true;

    g_latest_frame_id = my_id;
}
g_q_cv.notify_one();

    return S_OK;
    }

private:
    LONG m_ref;
};

// -------------------------
// ユーティリティ
// -------------------------

std::string Utf16ToUtf8(const std::wstring& w)
{
    if (w.empty()) return std::string();
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (len <= 0) return std::string();
    std::string s(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], len, nullptr, nullptr);
    return s;
}

std::string JsonEscapeUtf8(const std::wstring& w)
{
    std::string in = Utf16ToUtf8(w);
    std::string out;
    out.reserve(in.size() * 2);
    for (unsigned char c : in) {
        if (c == '\\' || c == '\"') {
            out.push_back('\\');
            out.push_back(c);
        } else if (c < 0x20) {
            char buf[7];
            sprintf_s(buf, "\\u%04X", c);
            out.append(buf);
        } else {
            out.push_back(c);
        }
    }
    return out;
}

// ビデオ/オーディオキャプチャデバイスの列挙（FriendlyName のみ）
std::vector<std::wstring> EnumerateDevices(const GUID& category)
{
    std::vector<std::wstring> result;

    ICreateDevEnum* pDevEnum = nullptr;
    IEnumMoniker* pEnum = nullptr;

    HRESULT hr = CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_ICreateDevEnum, (void**)&pDevEnum);
    if (FAILED(hr)) {
        log_printf(L"CoCreateInstance(CLSID_SystemDeviceEnum) failed: 0x%08X\n", hr);
        return result;
    }

    hr = pDevEnum->CreateClassEnumerator(category, &pEnum, 0);
    if (hr != S_OK || !pEnum) {
        log_printf(L"CreateClassEnumerator failed or no device: hr=0x%08X\n", hr);
        pDevEnum->Release();
        return result;
    }

    IMoniker* pMoniker = nullptr;
    while (pEnum->Next(1, &pMoniker, nullptr) == S_OK) {
        IPropertyBag* pBag = nullptr;
        VARIANT varName;
        VariantInit(&varName);

        hr = pMoniker->BindToStorage(nullptr, nullptr, IID_IPropertyBag, (void**)&pBag);
        if (SUCCEEDED(hr)) {
            hr = pBag->Read(L"FriendlyName", &varName, nullptr);
        }
        if (SUCCEEDED(hr) && varName.vt == VT_BSTR) {
            std::wstring fname = varName.bstrVal ? varName.bstrVal : L"";
            result.push_back(fname);
        }

        VariantClear(&varName);
        if (pBag) pBag->Release();
        pMoniker->Release();
    }

    pEnum->Release();
    pDevEnum->Release();

    return result;
}

void PrintJsonArray(const std::vector<std::wstring>& devs)
{
    std::string s = "[";
    bool first = true;
    for (auto& d : devs) {
        if (!first) s += ",";
        first = false;
        s += "\"";
        s += JsonEscapeUtf8(d);
        s += "\"";
    }
    s += "]\n";
    fwrite(s.data(), 1, s.size(), stdout);
            g_written_frames.fetch_add(1, std::memory_order_relaxed);
fflush(stdout);
}

// FriendlyName でビデオキャプチャデバイスを探す
HRESULT FindCaptureDevice(const std::wstring& targetName, IBaseFilter** ppFilter)
{
    if (!ppFilter) return E_POINTER;
    *ppFilter = nullptr;

    ICreateDevEnum* pDevEnum = nullptr;
    IEnumMoniker* pEnum = nullptr;

    HRESULT hr = CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_ICreateDevEnum, (void**)&pDevEnum);
    if (FAILED(hr)) {
        log_printf(L"CoCreateInstance(CLSID_SystemDeviceEnum) failed: 0x%08X\n", hr);
        return hr;
    }

    hr = pDevEnum->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &pEnum, 0);
    if (hr != S_OK || !pEnum) {
        log_printf(L"CreateClassEnumerator failed or no device: hr=0x%08X\n", hr);
        pDevEnum->Release();
        return E_FAIL;
    }

    IMoniker* pMoniker = nullptr;
    while (pEnum->Next(1, &pMoniker, nullptr) == S_OK) {
        IPropertyBag* pBag = nullptr;
        VARIANT varName;
        VariantInit(&varName);

        hr = pMoniker->BindToStorage(nullptr, nullptr, IID_IPropertyBag, (void**)&pBag);
        if (SUCCEEDED(hr)) {
            hr = pBag->Read(L"FriendlyName", &varName, nullptr);
        }
        if (SUCCEEDED(hr) && varName.vt == VT_BSTR) {
            std::wstring fname = varName.bstrVal ? varName.bstrVal : L"";
            if (fname == targetName) {
                log_printf(L"FindCaptureDevice: matched device \"%s\"\n", fname.c_str());
                hr = pMoniker->BindToObject(nullptr, nullptr, IID_IBaseFilter, (void**)ppFilter);
                VariantClear(&varName);
                pBag->Release();
                pMoniker->Release();
                break;
            }
        }

        VariantClear(&varName);
        if (pBag) pBag->Release();
        pMoniker->Release();
    }

    if (!*ppFilter) {
        log_printf(L"FindCaptureDevice: device \"%s\" not found\n", targetName.c_str());
        hr = E_FAIL;
    }

    if (pEnum) pEnum->Release();
    if (pDevEnum) pDevEnum->Release();

    return hr;
}

// 指定解像度/フレームレートに近いフォーマットを設定（ざっくり）
HRESULT SetVideoFormat(ICaptureGraphBuilder2* pBuilder, IBaseFilter* pCap, const CaptureConfig& cfg) {
    if (!pBuilder || !pCap) return E_POINTER;

    IAMStreamConfig* pConfig = nullptr;
    HRESULT hr = pBuilder->FindInterface(&PIN_CATEGORY_CAPTURE, &MEDIATYPE_Video, pCap,
                                         IID_IAMStreamConfig, (void**)&pConfig);
    if (FAILED(hr) || !pConfig) {
        log_printf(L"FindInterface(IAMStreamConfig) failed: 0x%08X\n", hr);
        return hr;
    }

    int count = 0, size = 0;
    hr = pConfig->GetNumberOfCapabilities(&count, &size);
    if (FAILED(hr) || size < (int)sizeof(VIDEO_STREAM_CONFIG_CAPS)) {
        log_printf(L"GetNumberOfCapabilities failed: 0x%08X (size=%d)\n", hr, size);
        pConfig->Release();
        return hr;
    }

    std::vector<BYTE> capsBuf(size);
    VIDEO_STREAM_CONFIG_CAPS* caps = reinterpret_cast<VIDEO_STREAM_CONFIG_CAPS*>(capsBuf.data());

    AM_MEDIA_TYPE* pBest = nullptr;
    long bestDiff = LONG_MAX;

    log_printf(L"Enumerating stream capabilities (count=%d, size=%d)\n", count, size);

    for (int i = 0; i < count; ++i) {
        AM_MEDIA_TYPE* pmt = nullptr;
        // 第3引数は BYTE* なので reinterpret_cast でキャスト
        hr = pConfig->GetStreamCaps(i, &pmt, reinterpret_cast<BYTE*>(caps));
        if (FAILED(hr) || !pmt) {
            log_printf(L"GetStreamCaps[%d] failed: 0x%08X\n", i, hr);
            continue;
        }

        if (pmt->formattype == FORMAT_VideoInfo && pmt->cbFormat >= sizeof(VIDEOINFOHEADER)) {
            auto vih = reinterpret_cast<VIDEOINFOHEADER*>(pmt->pbFormat);
            LONG w = vih->bmiHeader.biWidth;
            LONG h = vih->bmiHeader.biHeight; // 通常は正の値でも上下反転のことあり
            long cw = cfg.width;
            long ch = cfg.height;

            long diff = labs(w - cw) + labs(abs(h) - ch);
            log_printf(L"  caps[%d]: %ldx%ld, diff=%ld\n", i, w, h, diff);

            if (diff < bestDiff) {
                bestDiff = diff;
                if (pBest) {
                    if (pBest->cbFormat != 0) {
                        CoTaskMemFree(pBest->pbFormat);
                        pBest->cbFormat = 0;
                        pBest->pbFormat = nullptr;
                    }
                    if (pBest->pUnk) {
                        pBest->pUnk->Release();
                        pBest->pUnk = nullptr;
                    }
                    CoTaskMemFree(pBest);
                }
                pBest = pmt;
            } else {
                if (pmt->cbFormat != 0) {
                    CoTaskMemFree(pmt->pbFormat);
                    pmt->cbFormat = 0;
                    pmt->pbFormat = nullptr;
                }
                if (pmt->pUnk) {
                    pmt->pUnk->Release();
                    pmt->pUnk = nullptr;
                }
                CoTaskMemFree(pmt);
            }
        } else {
            log_printf(L"  caps[%d]: unsupported formattype or cbFormat\n", i);
            if (pmt->cbFormat != 0) {
                CoTaskMemFree(pmt->pbFormat);
                pmt->cbFormat = 0;
                pmt->pbFormat = nullptr;
            }
            if (pmt->pUnk) {
                pmt->pUnk->Release();
                pmt->pUnk = nullptr;
            }
            CoTaskMemFree(pmt);
        }
    }

    if (!pBest) {
        log_printf(L"SetVideoFormat: no suitable format found\n");
        pConfig->Release();
        return E_FAIL;
    }

    if (pBest->formattype == FORMAT_VideoInfo && pBest->cbFormat >= sizeof(VIDEOINFOHEADER)) {
        auto vih = reinterpret_cast<VIDEOINFOHEADER*>(pBest->pbFormat);
        LONG w = vih->bmiHeader.biWidth;
        LONG h = vih->bmiHeader.biHeight;
        log_printf(L"Selected format: %ldx%ld\n", w, h);
    }

    hr = pConfig->SetFormat(pBest);
    log_printf(L"SetFormat result: 0x%08X\n", hr);

    if (pBest->cbFormat != 0) {
        CoTaskMemFree(pBest->pbFormat);
        pBest->cbFormat = 0;
        pBest->pbFormat = nullptr;
    }
    if (pBest->pUnk) {
        pBest->pUnk->Release();
        pBest->pUnk = nullptr;
    }
    CoTaskMemFree(pBest);

    pConfig->Release();
    return hr;
}

// -------------------------
// Video subtype → fourcc / mpv format
// -------------------------
static const char* GetVideoSubtypeFourCC(const GUID& subtype)
{
    if (subtype == MEDIASUBTYPE_NV12) return "NV12";
    if (subtype == MEDIASUBTYPE_YUY2) return "YUY2";
    if (subtype == MEDIASUBTYPE_UYVY) return "UYVY";
    if (subtype == MEDIASUBTYPE_RGB32) return "RGB32";
    if (subtype == MEDIASUBTYPE_RGB24) return "RGB24";
    if (subtype == MEDIASUBTYPE_YV12) return "YV12";
    if (subtype == MEDIASUBTYPE_IYUV) return "IYUV";
    if (subtype == MEDIASUBTYPE_MJPG) return "MJPG";
    return "";
}

static const char* GetMpvFormatForSubtype(const GUID& subtype)
{
    if (subtype == MEDIASUBTYPE_NV12) return "nv12";
    if (subtype == MEDIASUBTYPE_YUY2) return "yuyv422";
    if (subtype == MEDIASUBTYPE_UYVY) return "uyvy422";
    if (subtype == MEDIASUBTYPE_RGB32) return "bgr0";
    if (subtype == MEDIASUBTYPE_RGB24) return "bgr24";
    if (subtype == MEDIASUBTYPE_YV12 || subtype == MEDIASUBTYPE_IYUV) return "yuv420p";
    return "";
}

// -------------------------
// デフォルトビデオフォーマット(JSON)出力
// -------------------------
HRESULT PrintDefaultVideoFormatAsJson(const CaptureConfig& cfg)
{
    if (cfg.deviceName.empty()) {
        log_printf(L"PrintDefaultVideoFormatAsJson: deviceName is empty\n");
        return E_INVALIDARG;
    }

    IGraphBuilder*         pGraph   = nullptr;
    ICaptureGraphBuilder2* pBuilder = nullptr;
    IBaseFilter*           pCap     = nullptr;
    IAMStreamConfig*       pConfig  = nullptr;
    AM_MEDIA_TYPE*         pmt      = nullptr;

    HRESULT hr = CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_IGraphBuilder, (void**)&pGraph);
    if (FAILED(hr)) goto cleanup;

    hr = CoCreateInstance(CLSID_CaptureGraphBuilder2, nullptr, CLSCTX_INPROC_SERVER,
                          IID_ICaptureGraphBuilder2, (void**)&pBuilder);
    if (FAILED(hr)) goto cleanup;

    hr = pBuilder->SetFiltergraph(pGraph);
    if (FAILED(hr)) goto cleanup;

    hr = FindCaptureDevice(cfg.deviceName, &pCap);
    if (FAILED(hr) || !pCap) goto cleanup;

    hr = pGraph->AddFilter(pCap, L"Video Capture");
    if (FAILED(hr)) goto cleanup;

    hr = pBuilder->FindInterface(&PIN_CATEGORY_CAPTURE, &MEDIATYPE_Video, pCap,
                                 IID_IAMStreamConfig, (void**)&pConfig);
    if (FAILED(hr) || !pConfig) goto cleanup;

    hr = pConfig->GetFormat(&pmt);
    if (FAILED(hr) || !pmt) goto cleanup;

    LONG   w   = 0;
    LONG   h   = 0;
    double fps = 0.0;

    if (pmt->formattype == FORMAT_VideoInfo && pmt->cbFormat >= sizeof(VIDEOINFOHEADER)) {
        auto vih = reinterpret_cast<VIDEOINFOHEADER*>(pmt->pbFormat);
        w = vih->bmiHeader.biWidth;
        h = vih->bmiHeader.biHeight;
        if (vih->AvgTimePerFrame > 0) {
            // AvgTimePerFrame: 100ns 単位
            fps = 1.0e7 / static_cast<double>(vih->AvgTimePerFrame);
        }
    }

    if (w <= 0 || h <= 0) {
        hr = E_FAIL;
        goto cleanup;
    }

    {
        std::ostringstream oss;
        oss.setf(std::ios::fixed);
        oss.precision(6);
        const char* subtypeStr = nullptr;
        const char* mpvFmt    = nullptr;
        if (pmt) {
            subtypeStr = GetVideoSubtypeFourCC(pmt->subtype);
            mpvFmt     = GetMpvFormatForSubtype(pmt->subtype);
        }

        oss << "{\"width\":" << w << ",\"height\":" << h;
        if (fps > 0.0) {
            oss << ",\"fps\":" << fps;
        }
        if (subtypeStr && subtypeStr[0] != '\0') {
            oss << ",\"subtype\":\"" << subtypeStr << "\"";
        }
        if (mpvFmt && mpvFmt[0] != '\0') {
            oss << ",\"mp_format\":\"" << mpvFmt << "\"";
        }
        oss << "}\\n";
        auto s = oss.str();
        fwrite(s.data(), 1, s.size(), stderr);
        fflush(stderr);
    }

cleanup:
    if (pmt) {
        if (pmt->cbFormat != 0) {
            CoTaskMemFree(pmt->pbFormat);
            pmt->cbFormat = 0;
            pmt->pbFormat = nullptr;
        }
        if (pmt->pUnk) {
            pmt->pUnk->Release();
            pmt->pUnk = nullptr;
        }
        CoTaskMemFree(pmt);
    }
    if (pConfig) pConfig->Release();
    if (pCap)    pCap->Release();
    if (pBuilder) pBuilder->Release();
    if (pGraph)   pGraph->Release();
    return hr;
}


// -------------------------
// Named Pipe 経由で stdout を差し替え（オプション）
// -------------------------
static bool SetupStdoutPipe(const CaptureConfig& cfg)
{
    if (cfg.pipeName.empty()) {
        // 何も指定されていない場合は従来どおり stdout をそのまま使う
        return true;
    }

    std::wstring fullName = L"\\\\.\\pipe\\" + cfg.pipeName;

    HANDLE hPipe = CreateNamedPipeW(
        fullName.c_str(),
        PIPE_ACCESS_OUTBOUND | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1,              // instances
        8 * 1024 * 1024,  // out buffer (8MB)
        8 * 1024 * 1024,  // in buffer (8MB)
        0,              // default timeout
        nullptr);       // security

    if (hPipe == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        log_printf(L"CreateNamedPipeW failed: 0x%08X\n", err);
        return false;
    }

    log_printf(L"Waiting for pipe client: %s\n", fullName.c_str());

    BOOL ok = ConnectNamedPipe(hPipe, nullptr);
    if (!ok) {
        DWORD err = GetLastError();
        if (err != ERROR_PIPE_CONNECTED) {
            log_printf(L"ConnectNamedPipe failed: 0x%08X\n", err);
            CloseHandle(hPipe);
            return false;
        }
    }

    // CRT の stdout にこのパイプハンドルを紐付ける
    int fd = _open_osfhandle((intptr_t)hPipe, _O_BINARY | _O_WRONLY);
    if (fd == -1) {
        log_printf(L"_open_osfhandle failed\n");
        CloseHandle(hPipe);
        return false;
    }

    if (_dup2(fd, _fileno(stdout)) == -1) {
        log_printf(L"_dup2 to stdout failed\n");
        _close(fd);
        return false;
    }

    _close(fd);
    // hPipe は CRT (stdout) に所有権を移したので CloseHandle しない

    return true;
}

// -------------------------
// メイン
// -------------------------

int wmain(int argc, wchar_t* argv[])
{
    CaptureConfig cfg;

    // rawvideo を stdout へ流すので、必ずバイナリモードにする（\n の変換などを防ぐ）
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stdin), _O_BINARY);

    // コマンドライン解析（超簡易）
    for (int i = 1; i < argc; ++i) {
        std::wstring arg = argv[i];

        // Listing
        if (arg == L"--list" || arg == L"--list-devices") {
            cfg.listDevicesOnly = true;
            cfg.listVideo = true; // legacy --list = video
        } else if (arg == L"--list-video") {
            cfg.listVideo = true;
        } else if (arg == L"--list-audio") {
            cfg.listAudio = true;
        } else if (arg == L"--json") {
            cfg.json = true;
        } else if (arg == L"--quiet") {
            cfg.quiet = true;
        } else if (arg == L"--get-default-format") {
            cfg.queryDefaultFormat = true;
        }
        // Capture options
        else if ((arg == L"--device" || arg == L"-d") && i + 1 < argc) {
            cfg.deviceName = argv[++i];
        } else if (arg == L"--w" && i + 1 < argc) {
            cfg.width = _wtoi(argv[++i]);
        } else if (arg == L"--h" && i + 1 < argc) {
            cfg.height = _wtoi(argv[++i]);
        } else if (arg == L"--fps" && i + 1 < argc) {
            cfg.fpsNum = _wtoi(argv[++i]);
            cfg.fpsDen = 1;
        } else if ((arg == L"--audio-device" || arg == L"--audio-dev") && i + 1 < argc) {
            cfg.audioDeviceName = argv[++i];
        } else if (arg == L"--mpv-path" && i + 1 < argc) {
            cfg.mpvPath = argv[++i];
        } else if ((arg == L"--pipe" || arg == L"--pipe-name") && i + 1 < argc) {
            cfg.pipeName = argv[++i];
        }
    }

    
const bool isObsVirtualCam =
    (cfg.deviceName.find(L"OBS Virtual Camera") != std::wstring::npos);

// OBS Virtual Camera のときだけ非同期 writer + STA + メッセージポンプを使う。
g_use_async_writer = isObsVirtualCam;

HRESULT hr = CoInitializeEx(nullptr, isObsVirtualCam ? COINIT_APARTMENTTHREADED
                                                    : COINIT_MULTITHREADED);
if (FAILED(hr)) {
        log_printf(L"CoInitializeEx failed: 0x%08X\n", hr);
        return 1;
    }

    // -------------------------
    // デバイス一覧モード
    // -------------------------
    if (cfg.listDevicesOnly || cfg.listVideo || cfg.listAudio) {
        if (!cfg.listVideo && !cfg.listAudio) cfg.listVideo = true; // default = video

        if (cfg.json) {
            if (cfg.listVideo && cfg.listAudio) {
                auto v = EnumerateDevices(CLSID_VideoInputDeviceCategory);
                auto a = EnumerateDevices(CLSID_AudioInputDeviceCategory);
                std::string outj = "{";
                outj += "\"video\":";

                auto buildArr = [](const std::vector<std::wstring>& devs) {
                    std::string s = "[";
                    bool first = true;
                    for (auto& d : devs) {
                        if (!first) s += ",";
                        first = false;
                        s += "\"";
                        s += JsonEscapeUtf8(d);
                        s += "\"";
                    }
                    s += "]";
                    return s;
                };
                outj += buildArr(v);
                outj += ",\"audio\":";
                outj += buildArr(a);
                outj += "}\n";
                fwrite(outj.data(), 1, outj.size(), stdout);
                fflush(stdout);
            } else if (cfg.listVideo) {
                auto v = EnumerateDevices(CLSID_VideoInputDeviceCategory);
                PrintJsonArray(v);
            } else if (cfg.listAudio) {
                auto a = EnumerateDevices(CLSID_AudioInputDeviceCategory);
                PrintJsonArray(a);
            }
        } else {
            if (cfg.listVideo) {
                auto devs = EnumerateDevices(CLSID_VideoInputDeviceCategory);
                std::wcout << L"=== Video Capture Devices ===\n";
                for (size_t i = 0; i < devs.size(); ++i) {
                    std::wcout << L"[" << (int)i << L"] " << devs[i] << L"\n";
                }
                if (devs.empty()) std::wcout << L"(デバイス無し)\n";
            }
            if (cfg.listAudio) {
                auto devs = EnumerateDevices(CLSID_AudioInputDeviceCategory);
                std::wcout << L"=== Audio Input Devices ===\n";
                for (size_t i = 0; i < devs.size(); ++i) {
                    std::wcout << L"[" << (int)i << L"] " << devs[i] << L"\n";
                }
                if (devs.empty()) std::wcout << L"(デバイス無し)\n";
            }
        }

        CoUninitialize();
        return 0;
    }

    // -------------------------
    // キャプチャーモード
    // -------------------------

    if (cfg.queryDefaultFormat) {
        if (cfg.deviceName.empty()) {
            log_printf(L"--get-default-format には --device が必要です\n");
            CoUninitialize();
            return 1;
        }
        HRESULT hr2 = PrintDefaultVideoFormatAsJson(cfg);
        CoUninitialize();
        return FAILED(hr2) ? 1 : 0;
    }

    if (cfg.deviceName.empty()) {
        log_printf(L"Usage:\n");
        log_printf(L"  dshowCap.exe --list              : ビデオ入力デバイス一覧を表示\n");
        log_printf(L"  dshowCap.exe --list-video --json : ビデオ入力デバイスをJSONで出力\n");
        log_printf(L"  dshowCap.exe --list-audio --json : オーディオ入力デバイスをJSONで出力\n");
        log_printf(L"  dshowCap.exe --device \"FriendlyName\" --get-default-format\n");
        log_printf(L"  dshowCap.exe --device \"FriendlyName\" --w 1280 --h 720 --fps 30 > out.raw 2> dshowCap.log\n");
        log_printf(L"  （音声同時起動）\n");
        log_printf(L"  dshowCap.exe --device \"Live Gamer Ultra-Video\" --w 1280 --h 720 --fps 30 \\\n");
        log_printf(L"             --audio-device \"HDMI (Live Gamer Ultra-Audio)\" \\\n");
        log_printf(L"             --mpv-path \"D:\\mpv-cHiDeNoise-AI\\mpv.exe\" \\\n");
        log_printf(L"             2> dshowCap.log | mpv ...\n");
        CoUninitialize();
        return 1;
    }

    // Named Pipe が指定されている場合はここで stdout を差し替える
    if (!SetupStdoutPipe(cfg)) {
        CoUninitialize();
        return 1;
    }

    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);

    IGraphBuilder*         pGraph       = nullptr;
    ICaptureGraphBuilder2* pBuilder     = nullptr;
    IMediaControl*         pControl     = nullptr;
    IBaseFilter*           pCap         = nullptr;
    IBaseFilter*           pGrabberBase = nullptr;
    ISampleGrabber*        pGrabber     = nullptr;
    IBaseFilter*           pNullRend    = nullptr;
    PROCESS_INFORMATION    piAudio{};
    ZeroMemory(&piAudio, sizeof(piAudio));

    do {
        hr = CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER,
                              IID_IGraphBuilder, (void**)&pGraph);
        if (FAILED(hr)) {
            log_printf(L"CoCreateInstance(FilterGraph) failed: 0x%08X\n", hr);
            break;
        }

        hr = CoCreateInstance(CLSID_CaptureGraphBuilder2, nullptr, CLSCTX_INPROC_SERVER,
                              IID_ICaptureGraphBuilder2, (void**)&pBuilder);
        if (FAILED(hr)) {
            log_printf(L"CoCreateInstance(CaptureGraphBuilder2) failed: 0x%08X\n", hr);
            break;
        }

        hr = pBuilder->SetFiltergraph(pGraph);
        if (FAILED(hr)) {
            log_printf(L"SetFiltergraph failed: 0x%08X\n", hr);
            break;
        }

        hr = FindCaptureDevice(cfg.deviceName, &pCap);
        if (FAILED(hr) || !pCap) {
            log_printf(L"FindCaptureDevice failed\n");
            break;
        }

        hr = pGraph->AddFilter(pCap, L"Video Capture");
        if (FAILED(hr)) {
            log_printf(L"AddFilter(Video Capture) failed: 0x%08X\n", hr);
            break;
        }

        hr = CoCreateInstance(CLSID_SampleGrabber, nullptr, CLSCTX_INPROC_SERVER,
                              IID_IBaseFilter, (void**)&pGrabberBase);
        if (FAILED(hr)) {
            log_printf(L"CoCreateInstance(SampleGrabber) failed: 0x%08X\n", hr);
            break;
        }

        hr = pGraph->AddFilter(pGrabberBase, L"Sample Grabber");
        if (FAILED(hr)) {
            log_printf(L"AddFilter(Sample Grabber) failed: 0x%08X\n", hr);
            break;
        }

        hr = pGrabberBase->QueryInterface(IID_ISampleGrabber, (void**)&pGrabber);
        if (FAILED(hr)) {
            log_printf(L"QueryInterface(ISampleGrabber) failed: 0x%08X\n", hr);
            break;
        }

        // ★ PixelFormat はデバイス側デフォルトを尊重するため、SetMediaType は行わない。
        //   （必要があれば GetConnectedMediaType で確認して mpv 側の demuxer に合わせる）

        // Null Renderer
        hr = CoCreateInstance(CLSID_NullRenderer, nullptr, CLSCTX_INPROC_SERVER,
                              IID_IBaseFilter, (void**)&pNullRend);
        if (FAILED(hr)) {
            log_printf(L"CLSID_NullRenderer failed: 0x%08X\n", hr);
            break;
        }

        hr = pGraph->AddFilter(pNullRend, L"Null Renderer");
        if (FAILED(hr)) {
            log_printf(L"AddFilter(Null Renderer) failed: 0x%08X\n", hr);
            break;
        }

        // 解像度/FPS 設定（できる範囲で）
        hr = SetVideoFormat(pBuilder, pCap, cfg);
        if (FAILED(hr)) {
            log_printf(L"SetVideoFormat failed (continue anyway), hr=0x%08X\n", hr);
            // 致命的ではないので続行
        }

        
// Capture -> SampleGrabber -> NullRenderer を構築
// OBS Virtual Camera は PIN_CATEGORY_CAPTURE が機能しない/不安定な場合があるため、
// まず PREVIEW を優先してつなぎ、失敗したら CAPTURE を試す。
HRESULT hrRender = E_FAIL;
if (isObsVirtualCam) {
    hrRender = pBuilder->RenderStream(&PIN_CATEGORY_PREVIEW, &MEDIATYPE_Video,
                                      pCap, pGrabberBase, pNullRend);
    if (FAILED(hrRender)) {
        hrRender = pBuilder->RenderStream(&PIN_CATEGORY_CAPTURE, &MEDIATYPE_Video,
                                          pCap, pGrabberBase, pNullRend);
    }
} else {
    hrRender = pBuilder->RenderStream(&PIN_CATEGORY_CAPTURE, &MEDIATYPE_Video,
                                      pCap, pGrabberBase, pNullRend);
    if (FAILED(hrRender)) {
        hrRender = pBuilder->RenderStream(&PIN_CATEGORY_PREVIEW, &MEDIATYPE_Video,
                                          pCap, pGrabberBase, pNullRend);
    }
}
hr = hrRender;
if (FAILED(hr)) {
    log_printf(L"RenderStream failed: 0x%08X\n", hr);
    break;
}

        // Grabber 設定
        pGrabber->SetBufferSamples(FALSE);
        pGrabber->SetOneShot(FALSE);

        GrabberCallback* cb = new GrabberCallback();
        pGrabber->SetCallback(cb, 1); // BufferCB モード
        cb->Release(); // pGrabber が保持するのでここでは Release

        // 実際に接続されたフォーマットをログに出す
        AM_MEDIA_TYPE mtConn;
        ZeroMemory(&mtConn, sizeof(mtConn));
        hr = pGrabber->GetConnectedMediaType(&mtConn);
        if (SUCCEEDED(hr)) {
            if (mtConn.formattype == FORMAT_VideoInfo && mtConn.cbFormat >= sizeof(VIDEOINFOHEADER)) {
                auto vih = reinterpret_cast<VIDEOINFOHEADER*>(mtConn.pbFormat);
                LONG w = vih->bmiHeader.biWidth;
                LONG h = vih->bmiHeader.biHeight;
                g_vid_w = (int)w;
                g_vid_h = (int)h;
                g_connectedSubtype = mtConn.subtype;

// Connected fps (for logging/decimation)
double cap_fps = 0.0;
if (vih->AvgTimePerFrame > 0) cap_fps = 10000000.0 / (double)vih->AvgTimePerFrame;
g_connected_fps.store(cap_fps, std::memory_order_relaxed);

// If the capture device outputs much higher fps than requested, decimate output.
// This is important for very high bandwidth sources (e.g. OBS Virtual Camera 1080p60 rawvideo).
g_decimate = 1;
if (cfg.fpsNum > 0 && cap_fps > 0.0) {
    const double ratio = cap_fps / (double)cfg.fpsNum;
    const int dec = (int)llround(ratio);
    if (dec >= 2 && dec <= 8 && ratio > 1.4) {
        g_decimate = dec;
        log_printf(L"Decimate: connected %0.3ffps -> output %ldfps (every %d frame)\n", cap_fps, cfg.fpsNum, g_decimate);
    }
}

                // Decide if we need planar 4:2:0 -> NV12 conversion
                g_need420_to_nv12 = false;
                g_needYUY2_to_nv12 = false;
                g_uvSwap = false;

                // OBS Virtual Camera などは I420/YV12 (planar 4:2:0) を返すことがある。
                // mpv 側は NV12 固定で受けるため、必要な場合は I420/YV12 -> NV12 へ変換する。
                if (IsEqualGUID(mtConn.subtype, kMEDIASUBTYPE_I420)) {
                    g_need420_to_nv12 = true;
                    g_uvSwap = false; // I420: Y + U + V
                    log_printf(L"Connected subtype: I420 (will convert to NV12)\n");
                } else if (IsEqualGUID(mtConn.subtype, kMEDIASUBTYPE_YV12)) {
                    g_need420_to_nv12 = true;
                    g_uvSwap = true;  // YV12: Y + V + U
                    log_printf(L"Connected subtype: YV12 (will convert to NV12)\n");
                } else if (IsEqualGUID(mtConn.subtype, MEDIASUBTYPE_NV12)) {
                    log_printf(L"Connected subtype: NV12\n");
                } else if (IsEqualGUID(mtConn.subtype, MEDIASUBTYPE_YUY2)) {
                    g_needYUY2_to_nv12 = true;
                    log_printf(L"Connected subtype: YUY2 (will convert to NV12)\n");
                } else if (IsEqualGUID(mtConn.subtype, MEDIASUBTYPE_RGB32)) {
                    log_printf(L"Connected subtype: RGB32\n");
                } else {
                    log_printf(L"Connected subtype: (other)\n");
                }

                log_printf(L"Connected format: %ldx%ld\n", w, h);
}
            if (mtConn.cbFormat != 0) {
                CoTaskMemFree(mtConn.pbFormat);
                mtConn.cbFormat = 0;
                mtConn.pbFormat = nullptr;
            }
            if (mtConn.pUnk) {
                mtConn.pUnk->Release();
                mtConn.pUnk = nullptr;
            }
        } else {
            log_printf(L"GetConnectedMediaType failed: 0x%08X\n", hr);
        }

        hr = pGraph->QueryInterface(IID_IMediaControl, (void**)&pControl);
        if (FAILED(hr)) {
            log_printf(L"QueryInterface(IMediaControl) failed: 0x%08X\n", hr);
            break;
        }

        const double cap_fps2 = g_connected_fps.load(std::memory_order_relaxed);
        log_printf(L"Start capturing from: %s (req %ldx%ld @%ldfps, connected %dx%d @%0.3ffps)\n",
                   cfg.deviceName.c_str(), cfg.width, cfg.height, cfg.fpsNum,
                   g_vid_w, g_vid_h, cap_fps2);
        log_printf(L"Ctrl + C でキャプチャを停止します。\n");

        // グラフ開始
        if (g_use_async_writer.load()) {
            StartWriterThread();
        }
        hr = pControl->Run();
        if (FAILED(hr)) {
            log_printf(L"MediaControl Run failed: 0x%08X\n", hr);
            break;
        }

        // 音声 mpv 起動（指定されている場合）
        if (!cfg.audioDeviceName.empty()) {
            LaunchAudioMpv(cfg, &piAudio);
        }

        
// ループ（Ctrl+C で抜ける）
// OBS Virtual Camera は STA + メッセージポンプがないとフレームが止まるケースがある。
ULONGLONG lastStatTick = GetTickCount64();
while (g_running.load()) {
    if (isObsVirtualCam) {
        // メッセージポンプ
        MsgWaitForMultipleObjectsEx(0, nullptr, 50, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        MSG msg;
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    } else {
        Sleep(50);
    }

    // 1秒に1回だけ簡易統計を出す（問題切り分け用）
    ULONGLONG now = GetTickCount64();
    if (now - lastStatTick >= 1000) {
        lastStatTick = now;
        log_printf(L"[stat] cb=%llu written=%llu dropped=%llu\n",
                   (unsigned long long)g_cb_frames.load(),
                   (unsigned long long)g_written_frames.load(),
                   (unsigned long long)g_dropped_frames.load());
    }
}

        pControl->Stop();

        StopWriterThread();

    } while (false);

    if (pNullRend)      pNullRend->Release();
    if (pGrabber)       pGrabber->Release();
    if (pGrabberBase)   pGrabberBase->Release();
    if (pCap)           pCap->Release();
    if (pControl)       pControl->Release();
    if (pBuilder)       pBuilder->Release();
    if (pGraph)         pGraph->Release();

    if (piAudio.hProcess) {
        TerminateProcess(piAudio.hProcess, 0);
        CloseHandle(piAudio.hThread);
        CloseHandle(piAudio.hProcess);
    }

    CoUninitialize();
    return 0;
}
