// mpvCapGC_60_YUV.cpp
// Capture a window using Windows.Graphics.Capture and write YUV444 frames
// to a file, stdout, or directly into mpv.exe via stdin.
// 60fps 相当出力版（TryGetNextFrame ポーリング）
// - 新しいフレームが来たら GPU→CPU コピーして即 YUV444 変換して fwrite（ペースメーカーなし → 低遅延）
// - mpv 自体（クラス "mpv" / タイトル "mpv" / "-"）はキャプチャ対象から除外
// - 自動ウィンドウ選択:
//     1. Zオーダーで一番上にある「Picture in Picture / ピクチャ / 画中画」系タイトルのウィンドウ
//     2. 見つからない場合は「普通のアプリっぽい」ウィンドウのうち一番上
// - 透明オーバーレイっぽいウィンドウは極力除外
// - outW/outH が 1x1 など異常に小さい場合は、キャプチャサイズに自動で置き換え
// - 起動時に「自分以外の mpvCapGC_60_YUV.exe プロセス」を全て Terminate → 黄色枠の多重発生を防止
// - ★ 子 mpv のウィンドウタイトルが "mpv" / "-" 以外（＝ファイル名 等）になったら
//       「ローカル動画再生開始」とみなしてキャプチャ終了（黄色枠を消す）
// - ★ キャプチャ元ウィンドウが閉じても mpv プロセスは kill しない（mpv は起動したまま）
// - ★ ONNX 対策：キャプチャ停止中は最後のフレームを 1fps で送り続ける
//
// ★タイトルバー除去（超安全ガード）
//   - WindowRect と ClientRect(スクリーン座標) の差分から
//     「クライアント領域推定クロップ」を行う
//   - outW/outH 自動時はクライアントサイズ優先
//   - ContentSize へ比率マッピングして安全にクロップ
//   - ★重要：WindowとClientの差がほぼ無い（<=3px程度）場合は
//     ボーダレス/タイトル無しとみなしてクロップを無効化
//
// ★最小差分の追加改善
//   1) FramePool buffer count: 2 -> 1
//   2) no-frame wait: sleep(1ms) -> yield()

#include <windows.h>
#include <inspectable.h>
#include <cstdio>
#include <cstdint>
#include <chrono>
#include <thread>
#include <io.h>
#include <fcntl.h>
#include <cstdlib>
#include <vector>
#include <cstring>
#include <wchar.h>

#include <d3d11.h>
#include <dxgi1_2.h>
#include <dwmapi.h>
#include <tlhelp32.h>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

#include <string>
#include <cwctype>
#include <cmath>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "windowsapp.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "dwmapi.lib")

// 公式の IGraphicsCaptureItemInterop 定義
#include <windows.graphics.capture.interop.h>

using namespace winrt;
namespace WF  = winrt::Windows::Foundation;
namespace WGC = winrt::Windows::Graphics::Capture;
namespace WGD = winrt::Windows::Graphics::DirectX;

// --- minimal interop definitions (no GetDXGIInterfaceFromObject) ----------

// GUID: A9B3D012-3DF2-4EE3-B8D1-8695F457D3C1
struct __declspec(uuid("A9B3D012-3DF2-4EE3-B8D1-8695F457D3C1"))
IDirect3DDxgiInterfaceAccess : IUnknown
{
    virtual HRESULT __stdcall GetInterface(REFIID iid, void** p) = 0;
};

// from windowsapp.lib
extern "C" HRESULT __stdcall CreateDirect3D11DeviceFromDXGIDevice(
    IDXGIDevice* dxgiDevice,
    IInspectable** graphicsDevice);

// -------------------------------------------------------------------------

// 名目上の fps（mpv の demuxer-rawvideo-fps と合わせる）
constexpr int kCaptureFps = 60;

struct Options {
    HWND hwnd = nullptr;
    int  outW = 0;                 // 0 => auto (client/capture size)
    int  outH = 0;                 // 0 => auto (client/capture size)
    char outPath[MAX_PATH] = {};   // if empty -> stdout or mpv pipe
    char mpvPath[MAX_PATH] = {};   // if non-empty -> launch mpv and pipe
    bool cursorCapture = false;    // default off (legacy behavior)
};

// ---- 起動時に「自分以外の mpvCapGC_60_YUV.exe」を全部キルする ---------------

static void kill_other_mpvCapGC_instances()
{
    DWORD selfPid = GetCurrentProcessId();

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        return;
    }

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);

    if (Process32FirstW(snap, &pe)) {
        do {
            if (pe.th32ProcessID == selfPid) {
                continue;
            }

            // 実行ファイル名が "mpvCapGC_60_YUV.exe" のプロセスだけ対象にする
            if (_wcsicmp(pe.szExeFile, L"mpvCapGC_60_YUV.exe") == 0) {
                HANDLE hProc = OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
                if (hProc) {
                    std::fprintf(stderr,
                        "[mpvCapGC_60_YUV] terminating old instance PID=%lu\n",
                        (unsigned long)pe.th32ProcessID);
                    TerminateProcess(hProc, 0);
                    CloseHandle(hProc);
                }
            }
        } while (Process32NextW(snap, &pe));
    }

    CloseHandle(snap);
}

// ---- command line parsing ------------------------------------------------
// 60fps 固定版なので --fps はありません
bool parse_args(int argc, char* argv[], Options& opt)
{
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (std::strcmp(a, "--hwnd") == 0 && i + 1 < argc) {
            const char* v = argv[++i];
            unsigned long long hwndVal = _strtoui64(v, nullptr, 0); // 0x... also ok
            opt.hwnd = reinterpret_cast<HWND>(static_cast<uintptr_t>(hwndVal));
        } else if (std::strcmp(a, "--w") == 0 && i + 1 < argc) {
            opt.outW = std::atoi(argv[++i]);  // 0 allowed (auto)
        } else if (std::strcmp(a, "--h") == 0 && i + 1 < argc) {
            opt.outH = std::atoi(argv[++i]);  // 0 allowed (auto)
        } else if (std::strcmp(a, "--out") == 0 && i + 1 < argc) {
            const char* p = argv[++i];
            std::strncpy(opt.outPath, p, MAX_PATH - 1);
            opt.outPath[MAX_PATH - 1] = '\0';
        } else if (std::strcmp(a, "--mpv") == 0 && i + 1 < argc) {
            const char* p = argv[++i];
            std::strncpy(opt.mpvPath, p, MAX_PATH - 1);
            opt.mpvPath[MAX_PATH - 1] = '\0';
        } else if (std::strcmp(a, "--cursor") == 0) {
            opt.cursorCapture = true;
        } else if (std::strcmp(a, "--no-cursor") == 0) {
            opt.cursorCapture = false;
        } else if (std::strcmp(a, "--help") == 0 || std::strcmp(a, "-h") == 0) {
            std::fprintf(stderr,
                "Usage: mpvCapGC_60_YUV.exe [--hwnd 0x123456]\n"
                "                           [--w 1280 --h 720]   (0 or omit => client/capture size)\n"
                "                           [--out path]        (write raw YUV444 to file)\n"
                "                           [--mpv \"C:/path/to/mpv.exe\"] (pipe into mpv)\n"
                "                           [--cursor|--no-cursor]\n"
                "\n"
                "FPS is nominally 60 in this build (for mpv's rawvideo demuxer).\n"
                "If --hwnd is omitted, the topmost non-mpv app window after 1 second will be captured,\n"
                "preferring Picture-in-Picture windows if present.\n"
                "If --out is omitted and --mpv is omitted: frames are written to stdout.\n"
                "If --mpv is specified: mpv is launched and frames are piped to its stdin.\n"
                "If --mpv is omitted: mpv.exe next to mpvCapGC_60_YUV.exe is used (portable).\n");
            return false;
        } else {
            // ignore unknown
        }
    }

    // あり得ない小さい値は「指定なし」とみなす（自動決定に任せる）
    if (opt.outW > 0 && opt.outW < 64) opt.outW = 0;
    if (opt.outH > 0 && opt.outH < 64) opt.outH = 0;

    return true;
}

// ---- convert Direct3D11CaptureFrame.Surface() -> ID3D11Texture2D ----------

winrt::com_ptr<ID3D11Texture2D> GetTextureFromSurface(
    WGC::Direct3D11CaptureFrame const& frame)
{
    auto surface = frame.Surface(); // WinRT IDirect3DSurface

    WF::IInspectable insp = surface.as<WF::IInspectable>();
    IUnknown* unk = reinterpret_cast<IUnknown*>(get_abi(insp));

    IDirect3DDxgiInterfaceAccess* access = nullptr;
    HRESULT hr = unk->QueryInterface(
        __uuidof(IDirect3DDxgiInterfaceAccess),
        reinterpret_cast<void**>(&access));

    if (FAILED(hr) || !access) {
        std::fprintf(stderr,
            "[mpvCapGC_60_YUV] QI(IDirect3DDxgiInterfaceAccess) failed: hr=0x%08X\n",
            (unsigned int)hr);
        return nullptr;
    }

    ID3D11Texture2D* texRaw = nullptr;
    hr = access->GetInterface(
        __uuidof(ID3D11Texture2D),
        reinterpret_cast<void**>(&texRaw));
    access->Release();

    if (FAILED(hr) || !texRaw) {
        std::fprintf(stderr,
            "[mpvCapGC_60_YUV] IDirect3DDxgiInterfaceAccess::GetInterface(ID3D11Texture2D) failed: hr=0x%08X\n",
            (unsigned int)hr);
        return nullptr;
    }

    winrt::com_ptr<ID3D11Texture2D> tex;
    tex.attach(texRaw); // take ownership
    return tex;
}

// ---- mpv / "-" ウィンドウ判定ヘルパ -------------------------------------

static void trim_spaces(wchar_t* s)
{
    if (!s) return;
    wchar_t* p = s;
    while (*p == L' ' || *p == L'\t' || *p == 0x3000) ++p;

    wchar_t* end = p + wcslen(p);
    while (end > p && (end[-1] == L' ' || end[-1] == 0x3000 || end[-1] == L'\t')) {
        --end;
    }

    size_t len = static_cast<size_t>(end - p);
    if (p != s) {
        memmove(s, p, (len + 1) * sizeof(wchar_t));
    } else {
        s[len] = L'\0';
    }
}

static bool is_mpv_like_window(HWND hwnd)
{
    if (!IsWindow(hwnd)) return false;

    wchar_t cls[256]   = {};
    wchar_t title[256] = {};

    GetClassNameW(hwnd, cls, 256);
    GetWindowTextW(hwnd, title, 256);

    if (_wcsicmp(cls, L"mpv") == 0) {
        return true;
    }

    trim_spaces(title);

    if (_wcsicmp(title, L"mpv") == 0) {
        return true;
    }

    if (wcscmp(title, L"-") == 0) {
        return true;
    }

    return false;
}

static bool is_pip_title(const wchar_t* rawTitle)
{
    if (!rawTitle) return false;

    wchar_t buf[256];
    wcsncpy_s(buf, rawTitle, _TRUNCATE);
    trim_spaces(buf);
    if (buf[0] == L'\0') return false;

    if (wcsstr(buf, L"ピクチャ") != nullptr) {
        return true;
    }
    if (wcsstr(buf, L"画中画") != nullptr) {
        return true;
    }

    std::wstring t(buf);
    for (auto& ch : t) {
        ch = static_cast<wchar_t>(towlower(ch));
    }

    if (t.find(L"picture in picture") != std::wstring::npos) {
        return true;
    }
    if (t.find(L"picture-in-picture") != std::wstring::npos) {
        return true;
    }

    return false;
}

// ---- Topmost window search ------------------------------------------------

struct TopmostCaptureSearch {
    HWND pipResult;
    HWND generalResult;
};

static BOOL CALLBACK EnumTopmostCaptureProc(HWND hwnd, LPARAM lParam)
{
    auto* ctx = reinterpret_cast<TopmostCaptureSearch*>(lParam);

    if (!IsWindow(hwnd) || !IsWindowVisible(hwnd)) {
        return TRUE;
    }

    if (is_mpv_like_window(hwnd)) {
        return TRUE;
    }

    wchar_t cls[256] = {};
    GetClassNameW(hwnd, cls, 256);
    if (_wcsicmp(cls, L"Progman") == 0 ||
        _wcsicmp(cls, L"Shell_TrayWnd") == 0 ||
        _wcsicmp(cls, L"WorkerW") == 0) {
        return TRUE;
    }

    BOOL cloaked = FALSE;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked)))) {
        if (cloaked) {
            return TRUE;
        }
    }

    LONG_PTR style = GetWindowLongPtr(hwnd, GWL_STYLE);
    if (style & WS_DISABLED) {
        return TRUE;
    }

    RECT rc{};
    if (!GetWindowRect(hwnd, &rc)) {
        return TRUE;
    }
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    if (w <= 64 || h <= 64) {
        return TRUE;
    }

    wchar_t title[256] = {};
    GetWindowTextW(hwnd, title, 256);
    trim_spaces(title);
    if (title[0] == L'\0') {
        return TRUE;
    }

    if (is_pip_title(title)) {
        ctx->pipResult = hwnd;
        return FALSE;
    }

    if (!ctx->generalResult) {
        ctx->generalResult = hwnd;
    }

    return TRUE;
}

// ---- mpv window search by pid --------------------------------------------

struct MpvWindowSearch {
    DWORD targetPid;
    HWND  found;
};

static BOOL CALLBACK EnumMpvWindowProc(HWND hwnd, LPARAM lParam)
{
    auto* s = reinterpret_cast<MpvWindowSearch*>(lParam);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == s->targetPid && is_mpv_like_window(hwnd)) {
        s->found = hwnd;
        return FALSE;
    }
    return TRUE;
}

static HWND find_mpv_window_for_pid(DWORD pid, DWORD timeoutMs = 3000)
{
    MpvWindowSearch search{};
    search.targetPid = pid;
    search.found = nullptr;

    DWORD start = GetTickCount();

    while (GetTickCount() - start < timeoutMs) {
        search.found = nullptr;
        EnumWindows(EnumMpvWindowProc, reinterpret_cast<LPARAM>(&search));
        if (search.found) {
            return search.found;
        }
        Sleep(50);
    }

    return nullptr;
}

// ---- Client-area based crop helper -------------------

struct ClientCropWS {
    int left = 0;
    int top = 0;
    int width = 0;
    int height = 0;
    int winW = 0;
    int winH = 0;
    bool enabled = false;
};

static ClientCropWS calc_client_crop_ws(HWND hwnd)
{
    ClientCropWS c{};

    RECT win{};
    if (!GetWindowRect(hwnd, &win))
        return c;

    RECT ext = win;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &ext, sizeof(ext)))) {
        win = ext;
    }

    c.winW = win.right - win.left;
    c.winH = win.bottom - win.top;
    if (c.winW <= 0 || c.winH <= 0)
        return c;

    RECT cr{};
    if (!GetClientRect(hwnd, &cr))
        return c;

    POINT tl{ cr.left, cr.top };
    POINT br{ cr.right, cr.bottom };
    if (!ClientToScreen(hwnd, &tl) || !ClientToScreen(hwnd, &br))
        return c;

    c.left   = tl.x - win.left;
    c.top    = tl.y - win.top;
    c.width  = br.x - tl.x;
    c.height = br.y - tl.y;

    if (c.width <= 0 || c.height <= 0 || c.left < 0 || c.top < 0)
        return c;

    // ★超安全ガード：
    // Window と Client の差がほぼ無い場合は
    // ボーダレス/タイトルバー無しとみなしてクロップ無効化
    int dw = std::abs(c.winW - c.width);
    int dh = std::abs(c.winH - c.height);
    if (dw <= 3 && dh <= 3) {
        c.enabled = false;
        return c;
    }

    c.enabled = true;
    return c;
}

struct ClientCropCS {
    int cropL = 0;
    int cropT = 0;
    int effW = 0;
    int effH = 0;
    bool enabled = false;
};

static ClientCropCS map_client_crop_to_contentsize(
    const ClientCropWS& ws, int srcW, int srcH)
{
    ClientCropCS cs{};
    cs.effW = srcW;
    cs.effH = srcH;

    if (!ws.enabled || ws.winW <= 0 || ws.winH <= 0 || srcW <= 0 || srcH <= 0)
        return cs;

    double sx = (double)srcW / (double)ws.winW;
    double sy = (double)srcH / (double)ws.winH;

    int L = (int)std::lround(ws.left * sx);
    int T = (int)std::lround(ws.top * sy);
    int W = (int)std::lround(ws.width * sx);
    int H = (int)std::lround(ws.height * sy);

    if (L < 0) L = 0;
    if (T < 0) T = 0;
    if (W <= 0) W = srcW;
    if (H <= 0) H = srcH;

    if (L + W > srcW) W = srcW - L;
    if (T + H > srcH) H = srcH - T;

    if (W < 64 || H < 64)
        return cs;

    cs.cropL = L;
    cs.cropT = T;
    cs.effW = W;
    cs.effH = H;
    cs.enabled = true;
    return cs;
}

// ---- BGRA -> YUV444 conversion -------------------------------------------

static inline void bgra_to_yuv601_limited(
    uint8_t B, uint8_t G, uint8_t R,
    uint8_t& Y, uint8_t& U, uint8_t& V)
{
    int y = (66 * R + 129 * G + 25 * B + 128) >> 8;
    int u = (-38 * R - 74 * G + 112 * B + 128) >> 8;
    int v = (112 * R - 94 * G - 18 * B + 128) >> 8;

    y += 16;
    u += 128;
    v += 128;

    if (y < 0) y = 0; else if (y > 255) y = 255;
    if (u < 0) u = 0; else if (u > 255) u = 255;
    if (v < 0) v = 0; else if (v > 255) v = 255;

    Y = (uint8_t)y;
    U = (uint8_t)u;
    V = (uint8_t)v;
}

static inline void bgra_to_yuv709_limited(
    uint8_t B, uint8_t G, uint8_t R,
    uint8_t& Y, uint8_t& U, uint8_t& V)
{
    int y = (47 * R + 157 * G + 16 * B + 128) >> 8;
    int u = (-26 * R - 87 * G + 112 * B + 128) >> 8;
    int v = (112 * R - 102 * G - 10 * B + 128) >> 8;

    y += 16;
    u += 128;
    v += 128;

    if (y < 0) y = 0; else if (y > 255) y = 255;
    if (u < 0) u = 0; else if (u > 255) u = 255;
    if (v < 0) v = 0; else if (v > 255) v = 255;

    Y = (uint8_t)y;
    U = (uint8_t)u;
    V = (uint8_t)v;
}

// -------------------------------------------------------------------------

int main(int argc, char* argv[])
{
    std::fprintf(stderr, "[mpvCapGC_60_YUV] main start (nominal %d fps)\n", kCaptureFps);

    kill_other_mpvCapGC_instances();

    try {
        winrt::init_apartment(winrt::apartment_type::single_threaded);
    } catch (std::exception const& e) {
        std::fprintf(stderr, "[mpvCapGC_60_YUV] init_apartment exception: %s\n", e.what());
        winrt::uninit_apartment();
        return 1;
    } catch (...) {
        std::fprintf(stderr, "[mpvCapGC_60_YUV] init_apartment unknown exception\n");
        winrt::uninit_apartment();
        return 1;
    }

    std::fprintf(stderr, "[mpvCapGC_60_YUV] init_apartment OK\n");

    auto finally_uninit = [is_uninit = false]() mutable {
        if (!is_uninit) {
            is_uninit = true;
            winrt::uninit_apartment();
        }
    };

    PROCESS_INFORMATION pi{};
    bool mpv_launched = false;
    HWND mpvWindow = nullptr;

    auto cleanup_handles = [&]() {
        if (mpv_launched) {
            if (pi.hThread) CloseHandle(pi.hThread);
            if (pi.hProcess) CloseHandle(pi.hProcess);
            mpv_launched = false;
        }
    };

    Options opt;
    if (!parse_args(argc, argv, opt)) {
        cleanup_handles();
        finally_uninit();
        return 1;
    }

    // --- ポータブル mpv 用: --mpv が指定されていない場合は
    //     「自分と同じフォルダの mpv.exe」を自動で使う -----------------
    if (opt.mpvPath[0] == '\0') {
        char exePath[MAX_PATH];
        DWORD len = GetModuleFileNameA(nullptr, exePath, MAX_PATH);
        if (len > 0 && len < MAX_PATH) {
            char* lastSlash = std::strrchr(exePath, '\\');
            if (lastSlash) {
                size_t dirLen = (size_t)(lastSlash - exePath + 1);
                if (dirLen + std::strlen("mpv.exe") < MAX_PATH) {
                    std::memcpy(opt.mpvPath, exePath, dirLen);
                    std::strcpy(opt.mpvPath + dirLen, "mpv.exe");
                }
            }
        }
        // 失敗した場合は mpvPath は空のまま → stdout モード
    }

    std::fprintf(stderr,
        "[mpvCapGC_60_YUV] options: hwnd=%p, out=%dx%d, fps=%d(nominal), outPath='%s', mpvPath='%s', cursor=%s\n",
        opt.hwnd, opt.outW, opt.outH, kCaptureFps,
        opt.outPath[0] ? opt.outPath : "(stdout or mpv)",
        opt.mpvPath[0] ? opt.mpvPath : "(none)",
        opt.cursorCapture ? "on" : "off");

    if (!WGC::GraphicsCaptureSession::IsSupported()) {
        std::fprintf(stderr,
            "[mpvCapGC_60_YUV] Windows.Graphics.Capture is not supported on this system.\n");
        cleanup_handles();
        finally_uninit();
        return 2;
    }

    // ---- resolve hwnd if not specified ------------------------------------
    if (!opt.hwnd) {
        std::fprintf(stderr,
            "[mpvCapGC_60_YUV] will auto-select topmost non-mpv window after 1 second...\n");
        std::this_thread::sleep_for(std::chrono::seconds(1));

        TopmostCaptureSearch search{};
        search.pipResult = nullptr;
        search.generalResult = nullptr;

        EnumWindows(EnumTopmostCaptureProc, (LPARAM)&search);

        HWND selected = search.pipResult ? search.pipResult : search.generalResult;

        if (!selected) {
            std::fprintf(stderr,
                "[mpvCapGC_60_YUV] EnumWindows could not find suitable capture window. fallback to foreground.\n");
            HWND fg = GetForegroundWindow();
            if (fg && !is_mpv_like_window(fg) && IsWindowVisible(fg)) {
                selected = fg;
            }
        }

        opt.hwnd = selected;
        std::fprintf(stderr, "[mpvCapGC_60_YUV] auto-selected hwnd -> %p\n", opt.hwnd);

        if (!opt.hwnd) {
            std::fprintf(stderr,
                "[mpvCapGC_60_YUV] failed to find non-mpv window for capture.\n");
            cleanup_handles();
            finally_uninit();
            return 4;
        }
    }

    if (!IsWindow(opt.hwnd)) {
        std::fprintf(stderr,
            "[mpvCapGC_60_YUV] specified hwnd is not a valid window.\n");
        cleanup_handles();
        finally_uninit();
        return 5;
    }

    // ---- create D3D11 device ----------------------------------------------
    winrt::com_ptr<ID3D11Device> d3dDevice;
    winrt::com_ptr<ID3D11DeviceContext> d3dContext;

    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    };
    D3D_FEATURE_LEVEL usedLevel{};

    HRESULT hr = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        flags,
        levels,
        ARRAYSIZE(levels),
        D3D11_SDK_VERSION,
        d3dDevice.put(),
        &usedLevel,
        d3dContext.put());

    if (FAILED(hr)) {
        std::fprintf(stderr,
            "[mpvCapGC_60_YUV] D3D11CreateDevice failed: hr=0x%08X\n",
            (unsigned int)hr);
        cleanup_handles();
        finally_uninit();
        return 6;
    }

    winrt::com_ptr<IDXGIDevice> dxgiDevice;
    dxgiDevice = d3dDevice.as<IDXGIDevice>();

    // DXGI Device -> WinRT IDirect3DDevice
    winrt::com_ptr<IInspectable> inspectableDevice;
    hr = CreateDirect3D11DeviceFromDXGIDevice(
        dxgiDevice.get(),
        inspectableDevice.put());

    if (FAILED(hr) || !inspectableDevice) {
        std::fprintf(stderr,
            "[mpvCapGC_60_YUV] CreateDirect3D11DeviceFromDXGIDevice failed: hr=0x%08X\n",
            (unsigned int)hr);
        cleanup_handles();
        finally_uninit();
        return 7;
    }

    auto winrtDevice =
        inspectableDevice.as<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();

    // ---- create GraphicsCaptureItem for window -----------------------------
    WGC::GraphicsCaptureItem item{ nullptr };
    {
        auto factory = winrt::get_activation_factory<WGC::GraphicsCaptureItem>();

        auto factoryInsp = factory.as<WF::IInspectable>();
        IUnknown* factoryUnk = (IUnknown*)get_abi(factoryInsp);

        IGraphicsCaptureItemInterop* interop = nullptr;
        hr = factoryUnk->QueryInterface(
            __uuidof(IGraphicsCaptureItemInterop),
            (void**)&interop);

        if (FAILED(hr) || !interop) {
            std::fprintf(stderr,
                "[mpvCapGC_60_YUV] QI(IGraphicsCaptureItemInterop) failed: hr=0x%08X\n",
                (unsigned int)hr);
            cleanup_handles();
            finally_uninit();
            return 8;
        }

        hr = interop->CreateForWindow(
            opt.hwnd,
            winrt::guid_of<WGC::GraphicsCaptureItem>(),
            (void**)winrt::put_abi(item));
        interop->Release();

        if (FAILED(hr) || !item) {
            std::fprintf(stderr,
                "[mpvCapGC_60_YUV] IGraphicsCaptureItemInterop::CreateForWindow failed: hr=0x%08X\n",
                (unsigned int)hr);
            cleanup_handles();
            finally_uninit();
            return 9;
        }
    }

    auto size = item.Size();
    std::fprintf(stderr,
        "[mpvCapGC_60_YUV] capture target size: %d x %d (logical)\n",
        size.Width, size.Height);

    // ---- client-crop (window-space) ----
    ClientCropWS clientWS = calc_client_crop_ws(opt.hwnd);
    if (clientWS.enabled) {
        std::fprintf(stderr,
            "[mpvCapGC_60_YUV] client crop WS: L=%d T=%d W=%d H=%d (win=%dx%d)\n",
            clientWS.left, clientWS.top, clientWS.width, clientWS.height,
            clientWS.winW, clientWS.winH);
    } else {
        std::fprintf(stderr,
            "[mpvCapGC_60_YUV] client crop WS disabled/unavailable -> fallback full window.\n");
    }

    int outW = opt.outW;
    int outH = opt.outH;

    // outW/outH 決定ロジック（小さすぎる指定は自動へ）
    if (outW <= 0 || outH <= 0) {
        if (clientWS.enabled) {
            outW = clientWS.width;
            outH = clientWS.height;
            std::fprintf(stderr,
                "[mpvCapGC_60_YUV] out size auto-selected to CLIENT size: %d x %d\n",
                outW, outH);
        } else {
            outW = size.Width;
            outH = size.Height;
            std::fprintf(stderr,
                "[mpvCapGC_60_YUV] out size auto-selected to capture size: %d x %d\n",
                outW, outH);
        }
    } else if (outW < 64 || outH < 64) {
        std::fprintf(stderr,
            "[mpvCapGC_60_YUV] out size too small (%dx%d) -> using auto size instead\n",
            outW, outH);
        if (clientWS.enabled) {
            outW = clientWS.width;
            outH = clientWS.height;
        } else {
            outW = size.Width;
            outH = size.Height;
        }
    } else {
        std::fprintf(stderr,
            "[mpvCapGC_60_YUV] out size overridden by options: %d x %d\n",
            outW, outH);
    }

    // ---- 色空間マトリクス選択 ------------------------------------------
    bool useBt709 = (outW >= 1280 || outH > 576);

    std::fprintf(stderr,
        "[mpvCapGC_60_YUV] colorspace matrix: %s (out=%dx%d)\n",
        useBt709 ? "BT.709 limited" : "BT.601 limited",
        outW, outH);

    const int fps = kCaptureFps; // 名目上

    // ---- setup output (file / stdout / mpv pipe) --------------------------
    FILE*  outFp      = nullptr;
    bool   useStdout  = false;
    HANDLE hPipeRead  = nullptr;
    HANDLE hPipeWrite = nullptr;

    if (opt.outPath[0]) {
        outFp = std::fopen(opt.outPath, "wb");
        if (!outFp) {
            std::fprintf(stderr,
                "[mpvCapGC_60_YUV] failed to open output file '%s'\n",
                opt.outPath);
            cleanup_handles();
            finally_uninit();
            return 3;
        }
        setvbuf(outFp, nullptr, _IONBF, 0);
        std::fprintf(stderr,
            "[mpvCapGC_60_YUV] output file opened: %s\n",
            opt.outPath);

    } else if (opt.mpvPath[0]) {
        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;
        sa.lpSecurityDescriptor = nullptr;

        if (!CreatePipe(&hPipeRead, &hPipeWrite, &sa, 0)) {
            std::fprintf(stderr, "[mpvCapGC_60_YUV] CreatePipe failed: %lu\n", GetLastError());
            cleanup_handles();
            finally_uninit();
            return 12;
        }

        if (!SetHandleInformation(hPipeWrite, HANDLE_FLAG_INHERIT, 0)) {
            std::fprintf(stderr,
                "[mpvCapGC_60_YUV] SetHandleInformation failed: %lu\n",
                GetLastError());
            CloseHandle(hPipeRead);
            CloseHandle(hPipeWrite);
            cleanup_handles();
            finally_uninit();
            return 13;
        }

        wchar_t wMpvPath[MAX_PATH];
        int wlen = MultiByteToWideChar(
            CP_ACP, 0,
            opt.mpvPath, -1,
            wMpvPath, MAX_PATH);
        if (wlen <= 0) {
            std::fprintf(stderr,
                "[mpvCapGC_60_YUV] MultiByteToWideChar(mpvPath) failed.\n");
            CloseHandle(hPipeRead);
            CloseHandle(hPipeWrite);
            cleanup_handles();
            finally_uninit();
            return 14;
        }

        wchar_t wCmdLine[1024];
        _snwprintf_s(
            wCmdLine, _TRUNCATE,
            L"mpv.exe"
            L" --profile=low-latency"
            L" --no-cache"
            L" --demuxer-readahead-secs=0"
            L" --force-window=yes"
            L" --keep-open=yes"
            L" --ontop"
            L" --msg-level=all=v"
            L" --icc-profile=no"
            L" --target-prim=auto"
            L" --target-trc=auto"
            L" --untimed=yes"
            L" --interpolation=no"

            // ★ キャプチャ時だけスケーラを固定（普段の mpv.conf には影響しない）
            L" --scale=bilinear"
            L" --cscale=bilinear"
            L" --dscale=bilinear"
            L" --correct-downscaling=yes"

            L" --{"
            L" --demuxer=rawvideo"
            L" --demuxer-rawvideo-w=%d"
            L" --demuxer-rawvideo-h=%d"
            L" --demuxer-rawvideo-fps=%d"
            L" --demuxer-rawvideo-mp-format=yuv444p"
            L" -"
            L" --}",
            outW, outH, fps);

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput  = hPipeRead;
        si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
        si.hStdError  = GetStdHandle(STD_ERROR_HANDLE);

        BOOL ok = CreateProcessW(
            wMpvPath,
            wCmdLine,
            nullptr,
            nullptr,
            TRUE,
            0,
            nullptr,
            nullptr,
            &si,
            &pi);

        if (!ok) {
            DWORD err = GetLastError();
            std::fprintf(stderr,
                "[mpvCapGC_60_YUV] CreateProcessW(mpv) failed: %lu\n",
                err);
            CloseHandle(hPipeRead);
            CloseHandle(hPipeWrite);
            cleanup_handles();
            finally_uninit();
            return 15;
        }
        mpv_launched = true;

        CloseHandle(hPipeRead);
        hPipeRead = nullptr;

        int fd = _open_osfhandle((intptr_t)hPipeWrite, _O_BINARY);
        if (fd == -1) {
            std::fprintf(stderr,
                "[mpvCapGC_60_YUV] _open_osfhandle failed.\n");
            CloseHandle(hPipeWrite);
            cleanup_handles();
            finally_uninit();
            return 16;
        }

        outFp = _fdopen(fd, "wb");
        if (!outFp) {
            std::fprintf(stderr,
                "[mpvCapGC_60_YUV] _fdopen failed.\n");
            _close(fd);
            cleanup_handles();
            finally_uninit();
            return 17;
        }

        setvbuf(outFp, nullptr, _IONBF, 0);

        std::fprintf(stderr, "[mpvCapGC_60_YUV] mpv pipe opened.\n");

        mpvWindow = find_mpv_window_for_pid(pi.dwProcessId);
        if (mpvWindow) {
            std::fprintf(stderr,
                "[mpvCapGC_60_YUV] found mpv window: %p for pid=%lu\n",
                mpvWindow, (unsigned long)pi.dwProcessId);
        } else {
            std::fprintf(stderr,
                "[mpvCapGC_60_YUV] WARNING: mpv window for pid=%lu not found.\n",
                (unsigned long)pi.dwProcessId);
        }

    } else {
        _setmode(_fileno(stdout), _O_BINARY);
        setvbuf(stdout, nullptr, _IONBF, 0);
        outFp = stdout;
        useStdout = true;
        std::fprintf(stderr, "[mpvCapGC_60_YUV] using stdout for frames\n");
    }

    // ---- FramePool & Session -----------------------------------------------
    WGC::Direct3D11CaptureFramePool framePool{ nullptr };

    try {
        framePool = WGC::Direct3D11CaptureFramePool::Create(
            winrtDevice,
            WGD::DirectXPixelFormat::B8G8R8A8UIntNormalized,
            1,      // buffer count (低遅延優先)
            size);
    } catch (...) {
        std::fprintf(stderr,
            "[mpvCapGC_60_YUV] Direct3D11CaptureFramePool::Create threw.\n");
        if (!useStdout && outFp) {
            std::fclose(outFp);
        }
        cleanup_handles();
        finally_uninit();
        return 10;
    }

    WGC::GraphicsCaptureSession session = framePool.CreateCaptureSession(item);
    session.IsCursorCaptureEnabled(opt.cursorCapture);

    try {
        session.StartCapture();
    } catch (...) {
        std::fprintf(stderr,
            "[mpvCapGC_60_YUV] GraphicsCaptureSession::StartCapture threw.\n");
        if (!useStdout && outFp) {
            std::fclose(outFp);
        }
        cleanup_handles();
        finally_uninit();
        return 11;
    }

    std::fprintf(stderr,
        "[mpvCapGC_60_YUV] capture started: hwnd=%p, out=%dx%d, fps=%d(nominal)\n",
        opt.hwnd, outW, outH, fps);

    // ---- output buffer (YUV444 planar: Y, U, V) ----------------------------
    const size_t outPixelCount = (size_t)outW * (size_t)outH;
    const size_t outFrameSize  = outPixelCount * 3; // YUV444 planar
    std::vector<uint8_t> outBuffer(outFrameSize);

    uint8_t* Yplane = outBuffer.data();
    uint8_t* Uplane = Yplane + outPixelCount;
    uint8_t* Vplane = Uplane + outPixelCount;

    // STAGING テクスチャを使い回す
    winrt::com_ptr<ID3D11Texture2D> stagingTex;
    D3D11_TEXTURE2D_DESC stagingDesc{};
    ZeroMemory(&stagingDesc, sizeof(stagingDesc));

    const int maxFrames = -1;
    int frameCount = 0;

    // ★ ONNX 対策用：最後のフレーム送出間隔
    using clock = std::chrono::steady_clock;
    bool hasLastFrame = false;
    auto lastKeepAliveSentTime = clock::now();
    const std::chrono::milliseconds keepAliveInterval(1000);

    // --- メインループ ---
    while (maxFrames < 0 || frameCount < maxFrames) {
        if (!IsWindow(opt.hwnd)) {
            std::fprintf(stderr,
                "[mpvCapGC_60_YUV] target window is no longer valid. stopping capture.\n");
            break;
        }

        if (mpv_launched) {
            DWORD exitCode = 0;
            if (GetExitCodeProcess(pi.hProcess, &exitCode) && exitCode != STILL_ACTIVE) {
                std::fprintf(stderr,
                    "[mpvCapGC_60_YUV] mpv process terminated (exit code: %lu). stopping capture.\n",
                    exitCode);
                break;
            }
            if (mpvWindow && !IsWindow(mpvWindow)) {
                std::fprintf(stderr,
                    "[mpvCapGC_60_YUV] mpv window destroyed. stopping capture.\n");
                break;
            }
            if (mpvWindow && IsWindow(mpvWindow)) {
                wchar_t title[256] = {};
                GetWindowTextW(mpvWindow, title, 256);
                trim_spaces(title);
                if (title[0] != L'\0' &&
                    wcscmp(title, L"-") != 0 &&
                    _wcsicmp(title, L"mpv") != 0)
                {
                    std::fprintf(stderr,
                        "[mpvCapGC_60_YUV] mpv window title changed to '%S'. "
                        "assuming local file playback, stopping capture.\n",
                        title);
                    break;
                }
            }
        }

        WGC::Direct3D11CaptureFrame frame{ nullptr };
        try {
            for (;;) {
                auto f = framePool.TryGetNextFrame();
                if (!f) break;
                frame = f;
            }
        } catch (...) {
            std::fprintf(stderr,
                "[mpvCapGC_60_YUV] TryGetNextFrame threw. exiting.\n");
            break;
        }

        if (!frame) {
            if (hasLastFrame) {
                auto now = clock::now();
                if (now - lastKeepAliveSentTime >= keepAliveInterval) {
                    size_t written = std::fwrite(outBuffer.data(), 1, outFrameSize, outFp);
                    if (written != outFrameSize) {
                        std::fprintf(stderr,
                            "[mpvCapGC_60_YUV] fwrite(keepalive) failed. exiting.\n");
                        break;
                    }
                    ++frameCount;
                    lastKeepAliveSentTime = now;
                }
            }
            std::this_thread::yield();
            continue;
        }

        auto contentSize = frame.ContentSize();
        int srcW = contentSize.Width;
        int srcH = contentSize.Height;
        if (srcW <= 0 || srcH <= 0) {
            std::fprintf(stderr,
                "[mpvCapGC_60_YUV] invalid ContentSize. exiting.\n");
            break;
        }

        // クライアント領域推定を ContentSize にマッピング
        ClientCropCS clientCS = map_client_crop_to_contentsize(clientWS, srcW, srcH);

        int cropL = 0, cropT = 0, effW = srcW, effH = srcH;
        if (clientCS.enabled) {
            cropL = clientCS.cropL;
            cropT = clientCS.cropT;
            effW  = clientCS.effW;
            effH  = clientCS.effH;
        }

        auto srcTex = GetTextureFromSurface(frame);
        if (!srcTex) {
            std::fprintf(stderr, "[mpvCapGC_60_YUV] GetTextureFromSurface returned null.\n");
            break;
        }

        D3D11_TEXTURE2D_DESC srcDesc{};
        srcTex->GetDesc(&srcDesc);

        if (!stagingTex ||
            stagingDesc.Width  != srcDesc.Width ||
            stagingDesc.Height != srcDesc.Height ||
            stagingDesc.Format != srcDesc.Format)
        {
            srcDesc.Usage = D3D11_USAGE_STAGING;
            srcDesc.BindFlags = 0;
            srcDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            srcDesc.MiscFlags = 0;

            stagingTex = nullptr;
            hr = d3dDevice->CreateTexture2D(&srcDesc, nullptr, stagingTex.put());
            if (FAILED(hr) || !stagingTex) {
                std::fprintf(stderr,
                    "[mpvCapGC_60_YUV] CreateTexture2D(STAGING) failed.\n");
                break;
            }
            stagingDesc = srcDesc;
        }

        d3dContext->CopyResource(stagingTex.get(), srcTex.get());

        D3D11_MAPPED_SUBRESOURCE mapped{};
        hr = d3dContext->Map(stagingTex.get(), 0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(hr)) {
            std::fprintf(stderr,
                "[mpvCapGC_60_YUV] Map failed.\n");
            break;
        }

        const uint8_t* srcBase = (const uint8_t*)mapped.pData;
        int srcStride = (int)mapped.RowPitch;

        if (!useBt709) {
            if (outW == effW && outH == effH) {
                size_t idx = 0;
                for (int y = 0; y < outH; ++y) {
                    const uint8_t* srcRow = srcBase + (y + cropT) * srcStride;
                    for (int x = 0; x < outW; ++x, ++idx) {
                        const uint8_t* srcPx = srcRow + (x + cropL) * 4;
                        uint8_t b = srcPx[0], g = srcPx[1], r = srcPx[2];
                        uint8_t Yv, Uv, Vv;
                        bgra_to_yuv601_limited(b, g, r, Yv, Uv, Vv);
                        Yplane[idx] = Yv; Uplane[idx] = Uv; Vplane[idx] = Vv;
                    }
                }
            } else {
                for (int y = 0; y < outH; ++y) {
                    int sy = cropT + (y * effH / outH);
                    if (sy < cropT) sy = cropT;
                    if (sy >= cropT + effH) sy = cropT + effH - 1;

                    const uint8_t* srcRow = srcBase + sy * srcStride;

                    for (int x = 0; x < outW; ++x) {
                        int sx = cropL + (x * effW / outW);
                        if (sx < cropL) sx = cropL;
                        if (sx >= cropL + effW) sx = cropL + effW - 1;

                        const uint8_t* srcPx = srcRow + sx * 4;
                        uint8_t b = srcPx[0], g = srcPx[1], r = srcPx[2];
                        uint8_t Yv, Uv, Vv;
                        bgra_to_yuv601_limited(b, g, r, Yv, Uv, Vv);

                        size_t idx = (size_t)y * outW + (size_t)x;
                        Yplane[idx] = Yv; Uplane[idx] = Uv; Vplane[idx] = Vv;
                    }
                }
            }
        } else {
            if (outW == effW && outH == effH) {
                size_t idx = 0;
                for (int y = 0; y < outH; ++y) {
                    const uint8_t* srcRow = srcBase + (y + cropT) * srcStride;
                    for (int x = 0; x < outW; ++x, ++idx) {
                        const uint8_t* srcPx = srcRow + (x + cropL) * 4;
                        uint8_t b = srcPx[0], g = srcPx[1], r = srcPx[2];
                        uint8_t Yv, Uv, Vv;
                        bgra_to_yuv709_limited(b, g, r, Yv, Uv, Vv);
                        Yplane[idx] = Yv; Uplane[idx] = Uv; Vplane[idx] = Vv;
                    }
                }
            } else {
                for (int y = 0; y < outH; ++y) {
                    int sy = cropT + (y * effH / outH);
                    if (sy < cropT) sy = cropT;
                    if (sy >= cropT + effH) sy = cropT + effH - 1;

                    const uint8_t* srcRow = srcBase + sy * srcStride;

                    for (int x = 0; x < outW; ++x) {
                        int sx = cropL + (x * effW / outW);
                        if (sx < cropL) sx = cropL;
                        if (sx >= cropL + effW) sx = cropL + effW - 1;

                        const uint8_t* srcPx = srcRow + sx * 4;
                        uint8_t b = srcPx[0], g = srcPx[1], r = srcPx[2];
                        uint8_t Yv, Uv, Vv;
                        bgra_to_yuv709_limited(b, g, r, Yv, Uv, Vv);

                        size_t idx = (size_t)y * outW + (size_t)x;
                        Yplane[idx] = Yv; Uplane[idx] = Uv; Vplane[idx] = Vv;
                    }
                }
            }
        }

        d3dContext->Unmap(stagingTex.get(), 0);

        {
            size_t written = std::fwrite(outBuffer.data(), 1, outFrameSize, outFp);
            if (written != outFrameSize) {
                std::fprintf(stderr,
                    "[mpvCapGC_60_YUV] fwrite failed. exiting.\n");
                break;
            }
        }

        ++frameCount;

        hasLastFrame = true;
        lastKeepAliveSentTime = clock::now();
    }

    std::fprintf(stderr,
        "[mpvCapGC_60_YUV] finished. total frames=%d\n",
        frameCount);

    try { if (session) session.Close(); } catch (...) {}
    try { if (framePool) framePool.Close(); } catch (...) {}

    stagingTex   = nullptr;
    d3dContext   = nullptr;
    d3dDevice    = nullptr;
    dxgiDevice   = nullptr;
    winrtDevice  = nullptr;
    item         = nullptr;

    if (!useStdout && outFp) {
        std::fclose(outFp);
        outFp = nullptr;
    }

    cleanup_handles();
    finally_uninit();

    return 0;
}
