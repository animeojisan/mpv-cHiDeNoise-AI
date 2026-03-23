// dvdmenu.cpp - Hybrid v89-stable VMR9 path with v84-style repaint-only menu refresh
// Goal: make menu video visible in mpv, robust logging, avoid named-pipe race/ERROR_NO_DATA.
//
// Build:
//   cl /utf-8 /std:c++17 /EHsc dvdmenu.cpp /Fe:dvdmenu.exe ^
//     ole32.lib oleaut32.lib uuid.lib strmiids.lib user32.lib gdi32.lib d3d9.lib
//
// Run example:
//   dvdmenu.exe --dvd-device "H:\" --start-menu root --fps 60 ^
//     --log "C:\DVDmenu\dvdmenu.log" --mpv-log "C:\DVDmenu\mpv.log" ^
//     --dump-first-bmp "C:\DVDmenu\first.bmp" --verbose
//
// Notes:
// - Uses mpv stdin ("-") instead of named pipes to avoid GLE=232 (ERROR_NO_DATA) / open-probe races.
// - Forces rendering of any unconnected DVD Navigator output pin to complete the video path.
// - Connects video decoder output -> VMR9 windowless.
// - Streams fixed-size 720x480 bgr0 to mpv. Sends black frames until capture succeeds.

#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#include <mmsystem.h>
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>
#include <dshow.h>
#include <dvdmedia.h>
#include <d3d9.h>
#include <vmr9.h>
#include <commdlg.h>

#include <string>
#include <atomic>

// Forward declarations for globals used before later definitions.
extern std::atomic<long> g_curVideoAspectX;
extern std::atomic<long> g_curVideoAspectY;

// Forward declarations for logging helpers used below.
static void logi(const char* fmt, ...);
static void logw(const char* fmt, ...);

// Forward declarations used before later definitions.
static bool send_ipc_menu_command_to_running_instance(const std::wstring& cmd);

// Strict mpv window identification:
// Avoid closing unrelated windows (e.g., Explorer/Browser tabs) whose title happens to contain "mpv".
static bool is_mpv_process_for_hwnd(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid) return false;

    HANDLE hp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hp) return false;

    wchar_t path[MAX_PATH * 2]{};
    DWORD sz = (DWORD)(sizeof(path) / sizeof(path[0]));
    BOOL ok = QueryFullProcessImageNameW(hp, 0, path, &sz);
    CloseHandle(hp);
    if (!ok || sz == 0) return false;

    const wchar_t* base = wcsrchr(path, L'\\');
    base = base ? (base + 1) : path;

    return (_wcsicmp(base, L"mpv.exe") == 0) || (_wcsicmp(base, L"mpv.com") == 0);
}

static bool try_close_foreground_mpv_window_for_command_or_launch();
static int close_all_mpv_windows_for_command_or_launch();

// Cached DVD title count (as reported by IDvdInfo2 / GetDVDVolumeInfo).
// Used for logging and (optionally) to decide how many titles to expose to mpv
// without having to parse VIDEO_TS.IFO manually.
static ULONG g_lastKnownDvdTitleCount = 0;

static ULONG dvd_get_title_count(IDvdInfo2* info) {
    if (!info) return 0;
    ULONG numVolumes = 0;
    ULONG volume = 0;
    ULONG numTitles = 0;
    DVD_DISC_SIDE side = (DVD_DISC_SIDE)0;
    HRESULT hr = info->GetDVDVolumeInfo(&numVolumes, &volume, &side, &numTitles);
    if (FAILED(hr)) {
        logw("GetDVDVolumeInfo failed: 0x%08lx", (unsigned long)hr);
        return 0;
    }
    logi("GetDVDVolumeInfo: volumes=%lu, volume=%lu, side=%d, titles=%lu",
         (unsigned long)numVolumes,
         (unsigned long)volume,
         (int)side,
         (unsigned long)numTitles);
    return numTitles;
}


// qedit.h is unavailable in modern SDKs; define the minimal Sample Grabber COM interfaces/CLSIDs we use.
#ifndef __QEDIT_MINIMAL_SAMPLE_GRABBER_DEFS__
#define __QEDIT_MINIMAL_SAMPLE_GRABBER_DEFS__
struct __declspec(uuid("0579154A-2B53-4994-B0D0-E773148EFF85")) ISampleGrabberCB;
struct __declspec(uuid("6B652FFF-11FE-4fce-92AD-0266B5D7C78F")) ISampleGrabber;
EXTERN_C const CLSID CLSID_SampleGrabber;
EXTERN_C const IID IID_ISampleGrabber;
EXTERN_C const IID IID_ISampleGrabberCB;

MIDL_INTERFACE("0579154A-2B53-4994-B0D0-E773148EFF85")
ISampleGrabberCB : public IUnknown {
public:
    virtual STDMETHODIMP SampleCB(double SampleTime, IMediaSample* pSample) = 0;
    virtual STDMETHODIMP BufferCB(double SampleTime, BYTE* pBuffer, long BufferLen) = 0;
};

MIDL_INTERFACE("6B652FFF-11FE-4fce-92AD-0266B5D7C78F")
ISampleGrabber : public IUnknown {
public:
    virtual STDMETHODIMP SetOneShot(BOOL OneShot) = 0;
    virtual STDMETHODIMP SetMediaType(const AM_MEDIA_TYPE* pType) = 0;
    virtual STDMETHODIMP GetConnectedMediaType(AM_MEDIA_TYPE* pType) = 0;
    virtual STDMETHODIMP SetBufferSamples(BOOL BufferThem) = 0;
    virtual STDMETHODIMP GetCurrentBuffer(long* pBufferSize, long* pBuffer) = 0;
    virtual STDMETHODIMP GetCurrentSample(IMediaSample** ppSample) = 0;
    virtual STDMETHODIMP SetCallback(ISampleGrabberCB* pCallback, long WhichMethodToCallback) = 0;
};

// qedit.dll / Sample Grabber CLSID and IID values
#endif // __QEDIT_MINIMAL_SAMPLE_GRABBER_DEFS__


#include <string>
#include <vector>
#include <cstdio>
#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <atomic>
#include <algorithm>
#include <mutex>
#include <thread>


// ---------------- Handoff watchdog (optional) ----------------
// Opt-in: --handoff-watchdog-sec N
// After launching mpv for dvd:// playback, dvdmenu spawns a detached watchdog child (same exe).
// The child waits N seconds, then:
//   - if mpv has a visible top-level window, it assumes healthy and exits;
//   - otherwise, it terminates mpv and the original dvdmenu process.
// This targets the "mpv stuck headless in background, still accessing DVD" case.
static DWORD g_handoffWatchdogSec = 0;

static BOOL CALLBACK enum_find_visible_window_for_pid(HWND hwnd, LPARAM lp) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    DWORD target = (DWORD)lp;
    if (pid != target) return TRUE;
    if (IsWindowVisible(hwnd) && !IsIconic(hwnd)) {
        SetLastError(ERROR_SUCCESS);
        return FALSE;
    }
    return TRUE;
}

static bool process_has_visible_window(DWORD pid) {
    SetLastError(ERROR_NOT_FOUND);
    EnumWindows(enum_find_visible_window_for_pid, (LPARAM)pid);
    return GetLastError() == ERROR_SUCCESS;
}

static bool is_process_alive(DWORD pid) {
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!h) return false;
    DWORD w = WaitForSingleObject(h, 0);
    CloseHandle(h);
    return w == WAIT_TIMEOUT;
}

static void terminate_pid(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);
    if (!h) return;
    TerminateProcess(h, 1);
    WaitForSingleObject(h, 2000);
    CloseHandle(h);
}

static int watchdog_child_main(DWORD mpvPid, DWORD parentPid, DWORD timeoutSec) {
    const DWORD totalMs = timeoutSec * 1000;
    DWORD slept = 0;
    while (slept < totalMs) {
        if (!is_process_alive(mpvPid)) return 0;
        Sleep(250);
        slept += 250;
    }
    if (!is_process_alive(mpvPid)) return 0;
    if (process_has_visible_window(mpvPid)) return 0;

    terminate_pid(mpvPid);
    if (parentPid && is_process_alive(parentPid)) terminate_pid(parentPid);
    return 0;
}

static bool spawn_watchdog_child(DWORD mpvPid, DWORD parentPid, DWORD timeoutSec) {
    if (!mpvPid || !timeoutSec) return false;

    wchar_t exePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);

    wchar_t cmd[1024]{};
    _snwprintf(cmd, 1024, L"\"%ls\" --watchdog-child %lu %lu %lu",
              exePath,
              (unsigned long)mpvPid,
              (unsigned long)parentPid,
              (unsigned long)timeoutSec);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    wchar_t* mutableCmd = _wcsdup(cmd);
    BOOL ok = CreateProcessW(nullptr, mutableCmd, nullptr, nullptr, FALSE,
                            CREATE_NO_WINDOW | DETACHED_PROCESS,
                            nullptr, nullptr, &si, &pi);
    free(mutableCmd);
    if (!ok) return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}



static double qpc_now_ms() {
    static LARGE_INTEGER freq = [](){ LARGE_INTEGER f{}; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER c{}; QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)freq.QuadPart;
}

static void precise_sleep_until_ms(double targetMs) {
    // Hybrid sleep: coarse Sleep() while far away, then yield/spin near deadline.
    for (;;) {
        double nowMs = qpc_now_ms();
        double remain = targetMs - nowMs;
        if (remain <= 0.0) break;
        if (remain > 3.0) {
            DWORD ms = (DWORD)(remain - 1.0);
            if (ms > 0) Sleep(ms); else Sleep(0);
        } else if (remain > 0.8) {
            Sleep(0);
        } else {
            SwitchToThread();
        }
    }
}

#pragma comment(lib, "strmiids.lib")

static std::string w2utf8(const std::wstring& ws); // forward decl for scaffold logs

// ---------------- logging ----------------
static FILE* g_log = nullptr;
static bool g_verbose = false;
static bool g_debug_overlay = false; // draw on-video debug info (default off)

static void vlogf(const char* lvl, const char* fmt, va_list ap) {
    char buf[4096];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    if (g_log) { fprintf(g_log, "[%s] %s\n", lvl, buf); fflush(g_log); }

    fprintf(stdout, "[%s] %s\n", lvl, buf);
    fflush(stdout);
}

#ifndef VFW_E_DVD_INVALID_DISC
#define VFW_E_DVD_INVALID_DISC ((HRESULT)0x80040291L)
#endif
#ifndef VFW_E_DVD_MENU_DOES_NOT_EXIST
#define VFW_E_DVD_MENU_DOES_NOT_EXIST ((HRESULT)0x80040292L)
#endif
#ifndef VFW_E_DVD_INVALID_DOMAIN
#define VFW_E_DVD_INVALID_DOMAIN ((HRESULT)0x80040293L)
#endif

static bool hr_is_menu_missing(HRESULT hr) {
    // "No menu" / not available / invalid domain.
    // Some discs/driver stacks return a different code (observed: 0x80040282) for menu-less / unavailable menu requests.
    const unsigned long u = (unsigned long)hr;
    return hr == VFW_E_DVD_MENU_DOES_NOT_EXIST || hr == VFW_E_DVD_INVALID_DOMAIN || hr == VFW_E_DVD_INVALID_DISC ||
           u == 0x80040282UL;
}

static void logi(const char* fmt, ...) { va_list ap; va_start(ap, fmt); vlogf("I", fmt, ap); va_end(ap); }
static void logw(const char* fmt, ...) { va_list ap; va_start(ap, fmt); vlogf("W", fmt, ap); va_end(ap); }
static void loge(const char* fmt, ...) { va_list ap; va_start(ap, fmt); vlogf("E", fmt, ap); va_end(ap); }

static DWORD gle() { return GetLastError(); }

// Create parent directories for a *file path* (i.e. mkdir -p for dirname(path)).
static bool ensure_parent_dir_recursive(std::wstring path) {
    if (path.empty()) return true;
    // If this is just a filename (no directory component), nothing to create.
    size_t pos = path.find_last_of(L"\\/");
    if (pos == std::wstring::npos) return true;
    // Strip file name and create the parent directories.
    path = path.substr(0, pos);
    if (path.empty()) return true;

    // If already exists
    DWORD attr = GetFileAttributesW(path.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) return true;

    // Build progressively
    std::wstring cur;
    cur.reserve(path.size());
    for (size_t i = 0; i < path.size(); i++) {
        wchar_t c = path[i];
        cur.push_back(c);
        if (c == L'\\' || c == L'/') {
            if (cur.size() <= 3) continue; // "C:\"
            CreateDirectoryW(cur.c_str(), nullptr);
        }
    }
    return CreateDirectoryW(path.c_str(), nullptr) || gle() == ERROR_ALREADY_EXISTS;
}



static std::wstring adjust_path_if_directory(std::wstring path, const wchar_t* default_name) {
    if (path.empty()) return path;

    // If the user passes a directory (ends with slash), append default filename.
    if (!path.empty()) {
        wchar_t last = path.back();
        if (last == L'\\' || last == L'/') {
            path += default_name;
            return path;
        }
    }

    DWORD attr = GetFileAttributesW(path.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
        // A directory exists with this name (often created accidentally). Write inside it.
        if (path.back() != L'\\' && path.back() != L'/') path += L'\\';
        path += default_name;
    }
    return path;
}


static bool open_iso_file_dialog(std::wstring& outPath) {
    wchar_t buf[MAX_PATH * 4] = {};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = L"ISO image (*.iso)\0*.iso\0All files (*.*)\0*.*\0\0";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = (DWORD)(sizeof(buf) / sizeof(buf[0]));
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    ofn.lpstrDefExt = L"iso";
    if (!GetOpenFileNameW(&ofn)) {
        DWORD e = CommDlgExtendedError();
        if (e != 0) logw("GetOpenFileNameW(.iso) failed/cancelled, CommDlgExtendedError=%lu", (unsigned long)e);
        return false;
    }
    outPath.assign(buf);
    return !outPath.empty();
}

static std::wstring ps_quote_single(const std::wstring& s) {
    std::wstring o; o.reserve(s.size() + 8);
    for (wchar_t ch : s) { if (ch == L'\'') o += L"''"; else o.push_back(ch); }
    return o;
}

static bool run_process_capture_stdout(const std::wstring& cmdLine, std::wstring& outText, DWORD timeoutMs = 30000) {
    outText.clear();
    SECURITY_ATTRIBUTES sa{}; sa.nLength = sizeof(sa); sa.bInheritHandle = TRUE;
    HANDLE hRead = nullptr, hWrite = nullptr;
    if (!CreatePipe(&hRead, &hWrite, &sa, 0)) { loge("CreatePipe failed: %lu", (unsigned long)gle()); return false; }
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{}; si.cb = sizeof(si); si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE); si.hStdOutput = hWrite; si.hStdError = hWrite;
    PROCESS_INFORMATION pi{};
    std::wstring mutableCmd = cmdLine;
    BOOL ok = CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(hWrite); hWrite = nullptr;
    if (!ok) { loge("CreateProcessW failed: %lu for cmd: %ls", (unsigned long)gle(), cmdLine.c_str()); CloseHandle(hRead); return false; }

    std::string bytes; char tmp[4096];
    for (;;) {
        DWORD avail = 0;
        if (!PeekNamedPipe(hRead, nullptr, 0, nullptr, &avail, nullptr)) break;
        if (avail > 0) {
            DWORD got = 0;
            DWORD want = (avail < sizeof(tmp)) ? avail : (DWORD)sizeof(tmp);
            if (ReadFile(hRead, tmp, want, &got, nullptr) && got > 0) bytes.append(tmp, tmp + got);
        } else {
            DWORD wr = WaitForSingleObject(pi.hProcess, 30);
            if (wr == WAIT_OBJECT_0) {
                for (;;) { DWORD got = 0; if (!ReadFile(hRead, tmp, sizeof(tmp), &got, nullptr) || got == 0) break; bytes.append(tmp, tmp + got); }
                break;
            }
        }
    }
    DWORD wr = WaitForSingleObject(pi.hProcess, timeoutMs);
    if (wr == WAIT_TIMEOUT) { logw("Process timeout; terminating helper process."); TerminateProcess(pi.hProcess, 1); }
    DWORD exitCode = 0; GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess); CloseHandle(hRead);

    int wlen = MultiByteToWideChar(CP_UTF8, 0, bytes.c_str(), (int)bytes.size(), nullptr, 0);
    if (wlen > 0) {
        outText.resize(wlen);
        MultiByteToWideChar(CP_UTF8, 0, bytes.c_str(), (int)bytes.size(), &outText[0], wlen);
    } else {
        wlen = MultiByteToWideChar(CP_ACP, 0, bytes.c_str(), (int)bytes.size(), nullptr, 0);
        if (wlen > 0) { outText.resize(wlen); MultiByteToWideChar(CP_ACP, 0, bytes.c_str(), (int)bytes.size(), &outText[0], wlen); }
    }
    logi("Helper process exit code: %lu", (unsigned long)exitCode);
    return (wr != WAIT_TIMEOUT) && (exitCode == 0);
}

static bool run_process_wait_hidden(const std::wstring& cmdLine, DWORD timeoutMs = 30000) {
    STARTUPINFOW si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::wstring mutableCmd = cmdLine;
    BOOL ok = CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    if (!ok) { loge("CreateProcessW(wait hidden) failed: %lu for cmd: %ls", (unsigned long)gle(), cmdLine.c_str()); return false; }
    CloseHandle(pi.hThread);
    DWORD wr = WaitForSingleObject(pi.hProcess, timeoutMs);
    if (wr == WAIT_TIMEOUT) {
        logw("Helper process timeout; terminating (no-capture helper).");
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 2000);
    }
    DWORD ec = 0; GetExitCodeProcess(pi.hProcess, &ec);
    CloseHandle(pi.hProcess);
    logi("Helper(no-capture) exit code: %lu", (unsigned long)ec);
    return (wr != WAIT_TIMEOUT) && (ec == 0);
}

static bool dismount_iso_image_best_effort(const std::wstring& isoPath) {
    std::wstring q = ps_quote_single(isoPath);
    std::wstring ps =
        L"$p='" + q + L"';"
        L"$img=Get-DiskImage -ImagePath $p -ErrorAction SilentlyContinue;"
        L"try { if ($img) { Dismount-DiskImage -ImagePath $p -ErrorAction Stop | Out-Null }; exit 0 } catch { exit 1 }";
    std::wstring cmd = L"powershell.exe -NoProfile -ExecutionPolicy Bypass -Command \"" + ps + L"\"";
    bool ok = run_process_wait_hidden(cmd, 15000);
    logi("Dismount ISO(best-effort): %ls -> helper_ok=%d", isoPath.c_str(), ok ? 1 : 0);
    return ok;
}

static bool dismount_all_iso_images_best_effort() {
    std::wstring ps =
        L"$imgs=Get-DiskImage | Where-Object { $_.Attached -eq $true -and $_.ImagePath -and $_.ImagePath.ToLower().EndsWith('.iso') };"
        L"try { if ($imgs) { foreach($i in $imgs){ Dismount-DiskImage -ImagePath $i.ImagePath -ErrorAction Stop | Out-Null } }; exit 0 } catch { exit 1 }";
    std::wstring cmd = L"powershell.exe -NoProfile -ExecutionPolicy Bypass -Command \"" + ps + L"\"";
    bool ok = run_process_wait_hidden(cmd, 20000);
    logi("Dismount all mounted ISOs(best-effort): helper_ok=%d", ok ? 1 : 0);
    return ok;
}

static void trim_ws_inplace(std::wstring& s) {
    auto isw = [](wchar_t c)->bool { return c==L' '||c==L'\t'||c==L'\r'||c==L'\n'; };
    size_t a = 0, b = s.size();
    while (a < b && isw(s[a])) ++a;
    while (b > a && isw(s[b-1])) --b;
    s = s.substr(a, b - a);
}

static bool mount_iso_and_get_drive_root(const std::wstring& isoPath, std::wstring& outDriveRoot) {
    outDriveRoot.clear();
    std::wstring q = ps_quote_single(isoPath);

    // Multi-mount is allowed. Resolve the drive for THIS image path after mounting,
    // so we never accidentally read an older still-mounted ISO.
    std::wstring ps =
        L"[Console]::OutputEncoding=[System.Text.UTF8Encoding]::new();"
        L"$p='" + q + L"';"
        L"$img0=Get-DiskImage -ImagePath $p -ErrorAction SilentlyContinue;"
        L"if (-not $img0 -or $img0.Attached -ne $true) { Mount-DiskImage -ImagePath $p -ErrorAction Stop | Out-Null; }"
        L"$img = Get-DiskImage -ImagePath $p -ErrorAction Stop; "
        L"$vols = @($img | Get-Volume -ErrorAction SilentlyContinue); "
        L"$letters = @(); "
        L"foreach($v in $vols){ if($v -and $v.DriveLetter){ $letters += [string]$v.DriveLetter } } "
        L"if($letters.Count -gt 0){ $dl = $letters | Sort-Object | Select-Object -Last 1; Write-Output ($dl + ':\\'); exit 0; } "
        L"throw 'No drive letter from mounted ISO';";

    std::wstring cmd = L"powershell.exe -NoLogo -NonInteractive -NoProfile -ExecutionPolicy Bypass -Command \"" + ps + L"\"";
    std::wstring out;
    if (!run_process_capture_stdout(cmd, out, 30000)) { loge("Failed to mount ISO via PowerShell."); return false; }
    trim_ws_inplace(out);
    if (out.size() >= 2 && out[1] == L':') {
        if (out.size() == 2) out += L"\\";
        outDriveRoot = out;
        logi("Mounted ISO -> drive root (by image path): %ls", outDriveRoot.c_str());
        return true;
    }
    loge("Unexpected mount helper output: %ls", out.c_str());
    return false;
}


static bool g_isoMountedByThisRun = false;
static std::wstring g_isoMountedPathForCleanup;


static void dismount_current_run_iso_with_retry_and_clear() {
    if (!g_isoMountedByThisRun || g_isoMountedPathForCleanup.empty()) return;
    std::wstring path = g_isoMountedPathForCleanup;
    logi("ISO cleanup begin: %ls", path.c_str());

    // Run retry logic inside ONE PowerShell process to avoid helper proliferation.
    std::wstring q = ps_quote_single(path);
    std::wstring ps =
        L"$p='" + q + L"';"
        L"$ok=$false;"
        L"for($i=0;$i -lt 8;$i++){"
          L"try { $img=Get-DiskImage -ImagePath $p -ErrorAction SilentlyContinue; if(-not $img -or $img.Attached -ne $true){ $ok=$true; break }; Dismount-DiskImage -ImagePath $p -ErrorAction Stop | Out-Null } catch {} ;"
          L"Start-Sleep -Milliseconds 250;"
        L"}"
        L"try { $img2=Get-DiskImage -ImagePath $p -ErrorAction SilentlyContinue; if(-not $img2 -or $img2.Attached -ne $true){ $ok=$true } } catch { $ok=$true }"
        L"if($ok){ exit 0 } else { exit 1 }";
    std::wstring cmd = L"powershell.exe -NoLogo -NonInteractive -NoProfile -ExecutionPolicy Bypass -Command \"" + ps + L"\"";
    bool okAny = run_process_wait_hidden(cmd, 30000);

    logi("ISO cleanup end: %ls (attempted=%d, helper_ok_any=%d)", path.c_str(), 8, okAny ? 1 : 0);
    g_isoMountedByThisRun = false;
    g_isoMountedPathForCleanup.clear();
}

struct IsoAutoUnmountGuard {
    bool active = false;
    std::wstring isoPath;
    void arm(const std::wstring& p) { active = true; isoPath = p; g_isoMountedByThisRun = true; g_isoMountedPathForCleanup = p; }
    void disarm() { active = false; isoPath.clear(); g_isoMountedByThisRun = false; g_isoMountedPathForCleanup.clear(); }
    ~IsoAutoUnmountGuard() {
        if (!active || isoPath.empty()) return;
        // IMPORTANT: do not wait indefinitely here. This guard runs during shutdown,
        // and hanging here forces the user to kill dvdmenu.exe from Task Manager.
        // Prefer the shared cleanup helper so global tracking is cleared consistently.
        dismount_current_run_iso_with_retry_and_clear();
        active = false;
        isoPath.clear();
    }
};

static std::wstring alternate_path_same_dir(const std::wstring& original, const wchar_t* alt_name) {
    size_t pos = original.find_last_of(L"\\/");
    if (pos == std::wstring::npos) return std::wstring(alt_name);
    return original.substr(0, pos + 1) + alt_name;
}
static void open_log(const std::wstring& path) {
    if (path.empty()) return;

    std::wstring p = adjust_path_if_directory(path, L"dvdmenu.log");
    ensure_parent_dir_recursive(p);

    g_log = _wfopen(p.c_str(), L"ab");
    if (!g_log) {
        DWORD e = gle();
        logw("Failed to open log file: %ls (GLE=%lu).", p.c_str(), (unsigned long)e);

        // If access denied because the target is a directory or read-only, try an alternate name.
        std::wstring alt = alternate_path_same_dir(p, L"dvdmenu_log.txt");
        ensure_parent_dir_recursive(alt);
        g_log = _wfopen(alt.c_str(), L"ab");
        if (g_log) {
            logw("Using alternate log path: %ls", alt.c_str());
        } else {
            logw("Falling back to ./dvdmenu.log");
            g_log = _wfopen(L"dvdmenu.log", L"ab");
        }
    }
    if (g_log) {
        logi("Logging to: %ls", p.c_str());
        fprintf(g_log, "---- dvdmenu session ----\n");
        fflush(g_log);
    }
}

// ---------------- minimal ComPtr ----------------
template <class T>
struct ComPtr {
    T* p = nullptr;
    ComPtr() = default;
    explicit ComPtr(T* v) : p(v) {}
    ~ComPtr() { reset(); }

    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;

    ComPtr(ComPtr&& o) noexcept : p(o.p) { o.p = nullptr; }
    ComPtr& operator=(ComPtr&& o) noexcept { if (this != &o) { reset(); p = o.p; o.p = nullptr; } return *this; }

    void reset(T* v = nullptr) { if (p) p->Release(); p = v; }
    // Provide both get() and Get() to avoid mismatches with various snippets / SDK samples.
    T* get() const { return p; }
    T* Get() const { return p; }
    T** put() { reset(); return &p; }
    // A few extra helpers to ease porting (not strictly required, but harmless).
    T** GetAddressOf() { return &p; }
    T** ReleaseAndGetAddressOf() { reset(); return &p; }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
    // Allow implicit use where a raw pointer is expected.
    operator T*() const { return p; }
    void attach(T* v) { reset(); p = v; }
};

// ---------------- hidden owner window ----------------
static LRESULT CALLBACK HiddenWndProc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }

static HWND create_offscreen_owner_window() {
    const wchar_t* cls = L"dvdmenu_owner_vmr9";
    static std::atomic<bool> reg{false};
    if (!reg.exchange(true)) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = HiddenWndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = cls;
        RegisterClassW(&wc);
    }
    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, cls, L"", WS_POPUP,
                               -32000, -32000, 64, 64,
                               nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    UpdateWindow(hwnd);
    return hwnd;
}

// ---------------- graph diagnostics ----------------
static void dump_graph(IGraphBuilder* gb) {
    if (!gb) return;
    ComPtr<IEnumFilters> ef;
    HRESULT hr = gb->EnumFilters(ef.put());
    if (FAILED(hr) || !ef) { logw("EnumFilters failed: 0x%08lx", (unsigned long)hr); return; }
    logi("---- Filter graph dump ----");
    ULONG fetched = 0;
    while (true) {
        IBaseFilter* f = nullptr;
        hr = ef->Next(1, &f, &fetched);
        if (hr != S_OK || fetched == 0) break;
        FILTER_INFO fi{};
        if (SUCCEEDED(f->QueryFilterInfo(&fi))) {
            logi("  Filter: %ls", fi.achName);
            if (fi.pGraph) fi.pGraph->Release();
        }
        f->Release();
    }
    logi("---- End graph dump ----");
}

// ---------------- renderer cleanup ----------------
static void disconnect_all_pins(IGraphBuilder* gb, IBaseFilter* f) {
    if (!gb || !f) return;
    ComPtr<IEnumPins> ep;
    if (FAILED(f->EnumPins(ep.put())) || !ep) return;
    while (true) {
        IPin* p = nullptr;
        ULONG got = 0;
        HRESULT hr = ep->Next(1, &p, &got);
        if (hr != S_OK || got == 0) break;
        gb->Disconnect(p);
        // also disconnect the connected peer, if any
        ComPtr<IPin> peer;
        if (SUCCEEDED(p->ConnectedTo(peer.put())) && peer) gb->Disconnect(peer.get());
        p->Release();
    }
}

// Remove the default "Video Renderer" that creates the extra "ActiveMovie Window".
// We want the graph to render only into our VMR9(windowless) so mpv is the only visible window.
static void remove_default_video_renderers(IGraphBuilder* gb) {
    if (!gb) return;
    ComPtr<IEnumFilters> ef;
    if (FAILED(gb->EnumFilters(ef.put())) || !ef) return;

    std::vector<IBaseFilter*> toRemove;
    while (true) {
        IBaseFilter* f = nullptr;
        ULONG fetched = 0;
        HRESULT hr = ef->Next(1, &f, &fetched);
        if (hr != S_OK || fetched == 0) break;

        FILTER_INFO fi{};
        if (SUCCEEDED(f->QueryFilterInfo(&fi))) {
            std::wstring name = fi.achName ? fi.achName : L"";
            if (fi.pGraph) fi.pGraph->Release();

            // Typical culprit is literally named "Video Renderer" (shows "ActiveMovie Window").
            // Also remove other auto-inserted video renderers (VMR/EVR) before we add our own VMR9.
            auto has = [&](const wchar_t* s) -> bool {
                return !s ? false : (name.find(s) != std::wstring::npos);
            };
            if (has(L"Video Renderer") || has(L"VMR") || has(L"Enhanced Video Renderer") || has(L"EVR")) {
                toRemove.push_back(f); // keep ref
                continue;
            }
        }
        f->Release();
    }

    for (auto* f : toRemove) {
        FILTER_INFO fi{};
        std::wstring name;
        if (SUCCEEDED(f->QueryFilterInfo(&fi))) {
            name = fi.achName ? fi.achName : L"";
            if (fi.pGraph) fi.pGraph->Release();
        }
        disconnect_all_pins(gb, f);
        HRESULT hr = gb->RemoveFilter(f);
        logi("RemoveFilter(%ls) -> 0x%08lx", name.c_str(), (unsigned long)hr);
        f->Release();
    }
}

static ComPtr<IBaseFilter> find_filter_by_name_substr(IGraphBuilder* gb, const wchar_t* needle) {
    ComPtr<IBaseFilter> found;
    if (!gb || !needle) return found;
    ComPtr<IEnumFilters> ef;
    if (FAILED(gb->EnumFilters(ef.put())) || !ef) return found;
    ULONG fetched = 0;
    while (true) {
        IBaseFilter* f = nullptr;
        HRESULT hr = ef->Next(1, &f, &fetched);
        if (hr != S_OK || fetched == 0) break;
        FILTER_INFO fi{};
        if (SUCCEEDED(f->QueryFilterInfo(&fi))) {
            std::wstring name = fi.achName;
            if (fi.pGraph) fi.pGraph->Release();
            if (name.find(needle) != std::wstring::npos) { found.attach(f); return found; }
        }
        f->Release();
    }
    return found;
}

// pin helpers
static bool is_pin_connected(IPin* p) {
    ComPtr<IPin> c;
    return p && SUCCEEDED(p->ConnectedTo(c.put())) && c;
}
static bool enum_pins(IBaseFilter* f, std::vector<ComPtr<IPin>>& pins) {
    pins.clear();
    if (!f) return false;
    ComPtr<IEnumPins> ep;
    if (FAILED(f->EnumPins(ep.put())) || !ep) return false;
    ULONG fetched = 0;
    while (true) {
        IPin* p = nullptr;
        HRESULT hr = ep->Next(1, &p, &fetched);
        if (hr != S_OK || fetched == 0) break;
        pins.emplace_back(ComPtr<IPin>(p));
    }
    return true;
}
static IPin* first_unconnected_pin(IBaseFilter* f, PIN_DIRECTION dir) {
    std::vector<ComPtr<IPin>> pins;
    if (!enum_pins(f, pins)) return nullptr;
    for (auto& pp : pins) {
        PIN_DIRECTION d{};
        if (SUCCEEDED(pp->QueryDirection(&d)) && d == dir) {
            if (!is_pin_connected(pp.get())) {
                pp.get()->AddRef();
                return pp.get();
            }
        }
    }
    return nullptr;
}

static std::wstring get_pin_name(IPin* p) {
    if (!p) return L"";
    PIN_INFO pi{};
    if (SUCCEEDED(p->QueryPinInfo(&pi))) {
        std::wstring n = pi.achName;
        if (pi.pFilter) pi.pFilter->Release();
        return n;
    }
    return L"";
}
static bool wcontains_i(const std::wstring& s, const wchar_t* sub) {
    if (!sub || !*sub) return false;
    std::wstring a=s, b=sub;
    std::transform(a.begin(), a.end(), a.begin(), ::towlower);
    std::transform(b.begin(), b.end(), b.begin(), ::towlower);
    return a.find(b) != std::wstring::npos;
}
static IPin* find_pin_by_name_substr(IBaseFilter* f, PIN_DIRECTION dir, const wchar_t* nameSubstr) {
    if (!f) return nullptr;
    std::vector<ComPtr<IPin>> pins;
    if (!enum_pins(f, pins)) return nullptr;
    for (auto& pp : pins) {
        PIN_DIRECTION d{};
        if (SUCCEEDED(pp->QueryDirection(&d)) && d == dir) {
            std::wstring n = get_pin_name(pp.get());
            if (wcontains_i(n, nameSubstr)) { pp->AddRef(); return pp.get(); }
        }
    }
    return nullptr;
}

static HRESULT try_connect_subpicture_nav_to_decoder(IGraphBuilder* g, IBaseFilter* nav, IBaseFilter* vdec) {
    if (!g || !nav || !vdec) return E_POINTER;
    ComPtr<IPin> navSp; navSp.attach(find_pin_by_name_substr(nav, PINDIR_OUTPUT, L"Sub"));
    ComPtr<IPin> decSpIn; decSpIn.attach(find_pin_by_name_substr(vdec, PINDIR_INPUT, L"Sub"));
    if (!navSp || !decSpIn) return S_FALSE;

    g->Disconnect(navSp.get());
    g->Disconnect(decSpIn.get());
    HRESULT hr = g->Connect(navSp.get(), decSpIn.get());
    if (SUCCEEDED(hr)) {
        logi("Connected DVD Navigator SubPicture -> Video Decoder SubPicture In.");
    } else {
        logw("Connect(Navigator SubPicture -> Decoder SubPicture In) failed: 0x%08lx", (unsigned long)hr);
    }
    return hr;
}

static HRESULT try_connect_decoder_subpicture_to_vmr9(IGraphBuilder* g, IBaseFilter* vdec, IBaseFilter* vmr9) {
    if (!g || !vdec || !vmr9) return E_POINTER;
    // Common decoder pin names include "SubPicture" / "SubPicture Out".
    ComPtr<IPin> decSpOut; decSpOut.attach(find_pin_by_name_substr(vdec, PINDIR_OUTPUT, L"Sub"));
    if (!decSpOut) return S_FALSE;

    IPin* in2 = first_unconnected_pin(vmr9, PINDIR_INPUT);
    if (!in2) return S_FALSE;
    ComPtr<IPin> vmrIn; vmrIn.attach(in2);

    g->Disconnect(decSpOut.get());
    g->Disconnect(vmrIn.get());
    HRESULT hr = g->Connect(decSpOut.get(), vmrIn.get());
    if (SUCCEEDED(hr)) {
        logi("Connected Video Decoder SubPicture -> VMR9 secondary stream.");
    } else {
        logw("Connect(Decoder SubPicture -> VMR9) failed: 0x%08lx", (unsigned long)hr);
    }
    return hr;
}


// ---------------- DVD state ----------------
struct DvdState {
    ComPtr<IGraphBuilder> graph;
    ComPtr<IDvdGraphBuilder> dvdgb;
    ComPtr<IMediaControl> mc;
    ComPtr<IDvdControl2> dvdctl;
    ComPtr<IDvdInfo2> dvdinfo;
    ComPtr<IBasicAudio> basicAudio;

    ComPtr<IBaseFilter> vmr9;
    ComPtr<IVMRWindowlessControl9> wl;
    HWND ownerHwnd = nullptr;
    // Native DVD video size as reported by VMR9 (can be 720x480, 720x540, 720x576, etc.)
    int nativeW = 0;
    int nativeH = 0;
};

struct AudioDelayGate {
    bool enabled = false;
    LONG savedVolume = 0;
    bool haveSavedVolume = false;
    bool muted = false;
    DWORD unmuteAtTick = 0;
};

static void audio_delay_gate_schedule(DvdState& st, AudioDelayGate& gate, DWORD delayMs) {
    if (!st.basicAudio || delayMs == 0) return;
    if (!gate.haveSavedVolume) {
        LONG v = 0;
        if (SUCCEEDED(st.basicAudio->get_Volume(&v))) {
            gate.savedVolume = v;
            gate.haveSavedVolume = true;
        } else {
            gate.savedVolume = 0; // DirectShow default nominal volume
        }
    }
    HRESULT hrMute = st.basicAudio->put_Volume(-10000); // silence
    if (SUCCEEDED(hrMute)) {
        gate.enabled = true;
        gate.muted = true;
        gate.unmuteAtTick = GetTickCount() + delayMs;
        logi("Audio delay gate: muting DirectShow audio for %lu ms", (unsigned long)delayMs);
    } else {
        logw("Audio delay gate: put_Volume(mute) failed: 0x%08lx", (unsigned long)hrMute);
    }
}

static void audio_delay_gate_poll(DvdState& st, AudioDelayGate& gate) {
    if (!gate.enabled || !gate.muted || !st.basicAudio) return;
    DWORD now = GetTickCount();
    if ((LONG)(now - gate.unmuteAtTick) >= 0) {
        LONG restoreVol = gate.haveSavedVolume ? gate.savedVolume : 0;
        HRESULT hr = st.basicAudio->put_Volume(restoreVol);
        if (SUCCEEDED(hr)) {
            gate.muted = false;
            gate.enabled = false;
            logi("Audio delay gate: unmuted DirectShow audio (volume=%ld)", (long)restoreVol);
        } else {
            logw("Audio delay gate: put_Volume(restore) failed: 0x%08lx", (unsigned long)hr);
        }
    }
}

static void audio_delay_gate_force_restore(DvdState& st, AudioDelayGate& gate) {
    if (!gate.muted || !st.basicAudio) return;
    LONG restoreVol = gate.haveSavedVolume ? gate.savedVolume : 0;
    (void)st.basicAudio->put_Volume(restoreVol);
    gate.muted = false;
    gate.enabled = false;
}




static void free_media_type_local(AM_MEDIA_TYPE& mt) {
    if (mt.cbFormat && mt.pbFormat) {
        CoTaskMemFree(mt.pbFormat);
        mt.pbFormat = nullptr;
        mt.cbFormat = 0;
    }
    if (mt.pUnk) {
        mt.pUnk->Release();
        mt.pUnk = nullptr;
    }
}

static bool find_connected_audio_decoder_renderer_edge(const DvdState& st,
                                                       ComPtr<IPin>& outDecOut,
                                                       ComPtr<IPin>& outRendIn,
                                                       AM_MEDIA_TYPE* outMtOpt) {
    if (outMtOpt) ZeroMemory(outMtOpt, sizeof(*outMtOpt));
    if (!st.graph) return false;

    ComPtr<IBaseFilter> aDec = find_filter_by_name_substr(st.graph.get(), L"LAV Audio Decoder");
    if (!aDec) aDec = find_filter_by_name_substr(st.graph.get(), L"Audio Decoder");
    ComPtr<IBaseFilter> aRend = find_filter_by_name_substr(st.graph.get(), L"DSound Renderer");
    if (!aRend) aRend = find_filter_by_name_substr(st.graph.get(), L"Audio Renderer");
    if (!aDec || !aRend) return false;

    std::vector<ComPtr<IPin>> dpins;
    if (!enum_pins(aDec.get(), dpins)) return false;

    for (auto& p : dpins) {
        if (!p) continue;
        PIN_DIRECTION d = PINDIR_INPUT;
        if (FAILED(p->QueryDirection(&d)) || d != PINDIR_OUTPUT) continue;

        IPin* connRaw = nullptr;
        if (FAILED(p->ConnectedTo(&connRaw)) || !connRaw) continue;
        ComPtr<IPin> conn; conn.attach(connRaw);

        PIN_INFO pi{}; HRESULT hpi = conn->QueryPinInfo(&pi);
        bool isRendererIn = false;
        if (SUCCEEDED(hpi) && pi.pFilter) {
            PIN_DIRECTION rd = PINDIR_OUTPUT;
            (void)conn->QueryDirection(&rd);
            isRendererIn = (pi.pFilter == aRend.get() && rd == PINDIR_INPUT);
            pi.pFilter->Release();
        }
        if (!isRendererIn) continue;

        outDecOut.reset(p.get()); if (outDecOut.get()) outDecOut.get()->AddRef();
        outRendIn.reset(conn.get()); if (outRendIn.get()) outRendIn.get()->AddRef();

        if (outMtOpt) {
            HRESULT hrmt = outDecOut->ConnectionMediaType(outMtOpt);
            if (FAILED(hrmt)) {
                outDecOut.reset();
                outRendIn.reset();
                ZeroMemory(outMtOpt, sizeof(*outMtOpt));
                return false;
            }
        }
        return true;
    }
    return false;
}

static void resync_audio_renderer_on_menu_return(DvdState& st) {
    if (!st.graph) return;

    ComPtr<IPin> decOut;
    ComPtr<IPin> rendIn;
    if (!find_connected_audio_decoder_renderer_edge(st, decOut, rendIn, nullptr) || !rendIn) {
        logw("Menu audio resync: audio decoder->renderer edge not found.");
        return;
    }

    PIN_INFO pi{};
    HRESULT hpi = rendIn->QueryPinInfo(&pi);
    if (FAILED(hpi) || !pi.pFilter) {
        if (SUCCEEDED(hpi) && pi.pFilter) pi.pFilter->Release();
        logw("Menu audio resync: QueryPinInfo(renderer pin) failed: 0x%08lx", (unsigned long)hpi);
        return;
    }

    ComPtr<IBaseFilter> aRend;
    aRend.attach(pi.pFilter); // QueryPinInfo returns +1 ref

    FILTER_INFO fi{};
    if (SUCCEEDED(aRend->QueryFilterInfo(&fi))) {
        logi("Menu audio resync: target renderer = %ls", fi.achName);
        if (fi.pGraph) fi.pGraph->Release();
    }

    ComPtr<IMediaFilter> mf;
    HRESULT hrq = aRend->QueryInterface(IID_IMediaFilter, (void**)mf.put());
    if (FAILED(hrq) || !mf) {
        logw("Menu audio resync: IMediaFilter unavailable on renderer: 0x%08lx", (unsigned long)hrq);
        return;
    }

    FILTER_STATE fs = State_Running;
    (void)mf->GetState(0, &fs);

    HRESULT hr = mf->Pause();
    logi("Menu audio resync: Audio Renderer Pause() -> 0x%08lx", (unsigned long)hr);
    Sleep(10);

    hr = mf->Stop();
    logi("Menu audio resync: Audio Renderer Stop() -> 0x%08lx", (unsigned long)hr);
    Sleep(10);

    hr = mf->Run(0);
    logi("Menu audio resync: Audio Renderer Run() -> 0x%08lx", (unsigned long)hr);

    // If the renderer had been paused before our nudge, restore paused state (best-effort).
    if (fs == State_Paused) {
        HRESULT hrp = mf->Pause();
        logi("Menu audio resync: restoring renderer paused state -> 0x%08lx", (unsigned long)hrp);
    }
}

static void exec_audio_rewire_same_edge_trial_phase_a(DvdState& st, bool enabledByUser) {
    if (!enabledByUser) return;
    logi("Audio rewire trial (Phase A): opt-in ON -> executing reversible same-edge trial (no PCM tap insertion yet).");

    if (!st.graph || !st.mc) {
        logw("Audio rewire trial (Phase A): graph/IMediaControl unavailable.");
        return;
    }

    ComPtr<IPin> decOut;
    ComPtr<IPin> rendIn;
    AM_MEDIA_TYPE mt{};
    if (!find_connected_audio_decoder_renderer_edge(st, decOut, rendIn, &mt)) {
        logw("Audio rewire trial (Phase A): target decoder->renderer edge not found or media type unavailable.");
        return;
    }

    OAFilterState before = State_Stopped;
    HRESULT hgs = st.mc->GetState(0, &before);
    if (FAILED(hgs)) {
        logw("Audio rewire trial (Phase A): GetState failed before trial: 0x%08lx", (unsigned long)hgs);
        before = State_Running;
    }

    HRESULT hr = st.mc->Pause();
    if (SUCCEEDED(hr)) logi("Audio rewire trial (Phase A): Pause() before trial -> OK");
    else logw("Audio rewire trial (Phase A): Pause() before trial failed: 0x%08lx", (unsigned long)hr);

    HRESULT hrd1 = st.graph->Disconnect(decOut.get());
    HRESULT hrd2 = st.graph->Disconnect(rendIn.get());
    logi("Audio rewire trial (Phase A): Disconnect(decoder output) -> 0x%08lx", (unsigned long)hrd1);
    logi("Audio rewire trial (Phase A): Disconnect(renderer input)  -> 0x%08lx", (unsigned long)hrd2);

    bool reconnected = false;
    HRESULT hrcd = st.graph->ConnectDirect(decOut.get(), rendIn.get(), &mt);
    if (SUCCEEDED(hrcd)) {
        reconnected = true;
        logi("Audio rewire trial (Phase A): ConnectDirect(same-edge) -> OK");
    } else {
        logw("Audio rewire trial (Phase A): ConnectDirect(same-edge) failed: 0x%08lx", (unsigned long)hrcd);
        HRESULT hrc = st.graph->Connect(decOut.get(), rendIn.get());
        if (SUCCEEDED(hrc)) {
            reconnected = true;
            logi("Audio rewire trial (Phase A): fallback Connect(same-edge) -> OK");
        } else {
            loge("Audio rewire trial (Phase A): fallback Connect(same-edge) failed: 0x%08lx", (unsigned long)hrc);
        }
    }

    if (reconnected) {
        IPin* chk = nullptr;
        HRESULT hchk = decOut->ConnectedTo(&chk);
        if (SUCCEEDED(hchk) && chk) {
            ComPtr<IPin> chkPin; chkPin.attach(chk);
            logi("Audio rewire trial (Phase A): post-trial edge restore check -> OK");
        } else {
            logw("Audio rewire trial (Phase A): post-trial edge restore check failed: 0x%08lx", (unsigned long)hchk);
        }
    }

    HRESULT hrs = S_OK;
    if (before == State_Running) {
        hrs = st.mc->Run();
        logi("Audio rewire trial (Phase A): state restore -> Run() hr=0x%08lx", (unsigned long)hrs);
    } else if (before == State_Paused) {
        hrs = st.mc->Pause();
        logi("Audio rewire trial (Phase A): state restore -> Pause() hr=0x%08lx", (unsigned long)hrs);
    } else {
        hrs = st.mc->Stop();
        logi("Audio rewire trial (Phase A): state restore -> Stop() hr=0x%08lx", (unsigned long)hrs);
    }

    free_media_type_local(mt);
    logi("Audio rewire trial (Phase A): completed. Next step is PCM tap graph insertion (Phase B2).");
}


// Step 2 scaffold (safe/log-only): prepare for future shared A/V timeline queueing.
// IMPORTANT: This does not change audio routing, timing, or rendering behavior.
static void log_audio_timeline_scaffold_step2(const DvdState& st, DWORD delayMs) {
    logi("Audio scaffold: timeline/queue planning placeholder only (delay=%lu ms).",
         (unsigned long)delayMs);
    if (!st.basicAudio) {
        logw("Audio scaffold: IBasicAudio is unavailable (placeholder only).");
    }
}

// Step 3-A scaffold (safe/log-only): identify the current audio route and estimate a future PCM hook point.
// IMPORTANT: This does not modify the graph or audio routing yet.
static void log_audio_capture_hook_scaffold_step3a(const DvdState& st) {
    logi("Audio scaffold: capture-hook reconnaissance only (no graph changes yet).");
    if (!st.graph) {
        logw("Audio scaffold: graph unavailable.");
        return;
    }

    auto log_filter_name = [](IBaseFilter* f, const char* tag) {
        if (!f) { logw("%s: (null)", tag); return; }
        FILTER_INFO fi{};
        if (SUCCEEDED(f->QueryFilterInfo(&fi))) {
            std::wstring ws = fi.achName;
            std::string n = w2utf8(ws);
            logi("%s: %s", tag, n.c_str());
            if (fi.pGraph) fi.pGraph->Release();
        } else {
            logw("%s: QueryFilterInfo failed", tag);
        }
    };

    ComPtr<IBaseFilter> aDec = find_filter_by_name_substr(st.graph.get(), L"LAV Audio Decoder");
    if (!aDec) {
        // Fallback: any filter with "Audio Decoder" in the name.
        aDec = find_filter_by_name_substr(st.graph.get(), L"Audio Decoder");
    }
    ComPtr<IBaseFilter> aRend = find_filter_by_name_substr(st.graph.get(), L"DSound Renderer");
    if (!aRend) {
        aRend = find_filter_by_name_substr(st.graph.get(), L"Audio Renderer");
    }

    log_filter_name(aDec.get(), "Audio scaffold: decoder");
    log_filter_name(aRend.get(), "Audio scaffold: renderer");

    if (!aDec || !aRend) {
        logw("Audio scaffold: decoder/renderer not both found; deferring hook insertion.");
        return;
    }

    std::vector<ComPtr<IPin>> decPins, rendPins;
    (void)enum_pins(aDec.get(), decPins);
    (void)enum_pins(aRend.get(), rendPins);

    ComPtr<IPin> decOut;
    ComPtr<IPin> rendIn;
    for (auto& p : decPins) {
        PIN_DIRECTION d{};
        if (FAILED(p->QueryDirection(&d)) || d != PINDIR_OUTPUT) continue;
        if (!is_pin_connected(p.get())) continue;
        ComPtr<IPin> to;
        if (SUCCEEDED(p->ConnectedTo(to.put())) && to) {
            PIN_INFO pi{};
            if (SUCCEEDED(to->QueryPinInfo(&pi))) {
                bool isRenderer = (pi.pFilter == aRend.get());
                if (pi.pFilter) pi.pFilter->Release();
                if (isRenderer) { decOut.reset(p.get()); if (decOut) decOut->AddRef(); break; }
            }
        }
    }
    for (auto& p : rendPins) {
        PIN_DIRECTION d{};
        if (FAILED(p->QueryDirection(&d)) || d != PINDIR_INPUT) continue;
        if (!is_pin_connected(p.get())) continue;
        ComPtr<IPin> from;
        if (SUCCEEDED(p->ConnectedTo(from.put())) && from) {
            PIN_INFO pi{};
            if (SUCCEEDED(from->QueryPinInfo(&pi))) {
                bool isDecoder = (pi.pFilter == aDec.get());
                if (pi.pFilter) pi.pFilter->Release();
                if (isDecoder) { rendIn.reset(p.get()); if (rendIn) rendIn->AddRef(); break; }
            }
        }
    }

    if (!decOut || !rendIn) {
        logw("Audio scaffold: direct decoder->renderer connection not identified.");
        return;
    }

    auto log_pin_name = [](IPin* p, const char* tag) {
        PIN_INFO pi{};
        if (p && SUCCEEDED(p->QueryPinInfo(&pi))) {
            std::wstring ws = pi.achName;
            std::string n = w2utf8(ws);
            logi("%s: %s", tag, n.c_str());
            if (pi.pFilter) pi.pFilter->Release();
        } else {
            logw("%s: (unavailable)", tag);
        }
    };
    log_pin_name(decOut.get(), "Audio scaffold: hook candidate upstream pin");
    log_pin_name(rendIn.get(), "Audio scaffold: hook candidate downstream pin");

    AM_MEDIA_TYPE mt{};
    HRESULT hrmt = decOut->ConnectionMediaType(&mt);
    if (SUCCEEDED(hrmt)) {
        if (mt.majortype == MEDIATYPE_Audio && mt.pbFormat && mt.cbFormat >= sizeof(WAVEFORMATEX)) {
            const WAVEFORMATEX* w = reinterpret_cast<const WAVEFORMATEX*>(mt.pbFormat);
            logi("Audio scaffold: connection media type confirms audio. tag=0x%04x ch=%u rate=%lu bits=%u align=%u avgBps=%lu cbSize=%u",
                 (unsigned)w->wFormatTag, (unsigned)w->nChannels, (unsigned long)w->nSamplesPerSec,
                 (unsigned)w->wBitsPerSample, (unsigned)w->nBlockAlign, (unsigned long)w->nAvgBytesPerSec,
                 (unsigned)w->cbSize);
        } else {
            logw("Audio scaffold: connection media type is not audio/PCM-inspectable.");
        }
        if (mt.cbFormat && mt.pbFormat) CoTaskMemFree(mt.pbFormat);
        if (mt.pUnk) mt.pUnk->Release();
    } else {
        logw("Audio scaffold: ConnectionMediaType failed: 0x%08lx", (unsigned long)hrmt);
    }

    logi("Audio scaffold: next step will replace this decoder->renderer edge with a PCM tap/queue path (same timeline design).");
}




// Step 3-B scaffold (safe/log-only): allocate a PCM queue "shape" using the currently connected decoder->renderer format.
// IMPORTANT: This does not capture PCM and does not change graph/audio routing.
struct AudioPcmQueueScaffold {
    DWORD delayMs = 0;
    DWORD sampleRate = 0;
    WORD channels = 0;
    WORD bitsPerSample = 0;
    WORD blockAlign = 1;
    DWORD avgBytesPerSec = 0;
    unsigned long long delayBytesExact = 0;
    unsigned long long delayBytesAligned = 0;
    unsigned long long capacityBytes = 0;
    double msPerSampleFrame = 0.0;
    std::vector<unsigned char> backing;
};

static void log_audio_pcm_queue_scaffold_step3b(const DvdState& st, DWORD delayMs) {
    logi("Audio scaffold: PCM queue scaffold only (no PCM push/pop yet). delay=%lu ms",
         (unsigned long)delayMs);
    if (!st.graph) {
        logw("Audio scaffold: graph unavailable.");
        return;
    }

    ComPtr<IBaseFilter> aDec = find_filter_by_name_substr(st.graph.get(), L"LAV Audio Decoder");
    if (!aDec) aDec = find_filter_by_name_substr(st.graph.get(), L"Audio Decoder");
    ComPtr<IBaseFilter> aRend = find_filter_by_name_substr(st.graph.get(), L"DSound Renderer");
    if (!aRend) aRend = find_filter_by_name_substr(st.graph.get(), L"Audio Renderer");
    if (!aDec || !aRend) {
        logw("Audio scaffold: decoder/renderer not both found.");
        return;
    }

    std::vector<ComPtr<IPin>> dpins, rpins;
    enum_pins(aDec.get(), dpins);
    enum_pins(aRend.get(), rpins);

    ComPtr<IPin> decOut;
    for (auto& p : dpins) {
        if (!p) continue;
        PIN_DIRECTION d{};
        if (FAILED(p->QueryDirection(&d)) || d != PINDIR_OUTPUT) continue;
        ComPtr<IPin> to;
        if (FAILED(p->ConnectedTo(to.put())) || !to) continue;
        PIN_INFO pi{};
        if (SUCCEEDED(to->QueryPinInfo(&pi))) {
            bool isRenderer = (pi.pFilter == aRend.get());
            if (pi.pFilter) pi.pFilter->Release();
            if (isRenderer) { decOut.reset(p.get()); if (decOut) decOut->AddRef(); break; }
        }
    }
    if (!decOut) {
        logw("Audio scaffold: decoder->renderer edge not found.");
        return;
    }

    AM_MEDIA_TYPE mt{};
    HRESULT hrmt = decOut->ConnectionMediaType(&mt);
    if (FAILED(hrmt)) {
        logw("Audio scaffold: ConnectionMediaType failed: 0x%08lx", (unsigned long)hrmt);
        return;
    }

    if (mt.majortype != MEDIATYPE_Audio || !mt.pbFormat || mt.cbFormat < sizeof(WAVEFORMATEX)) {
        logw("Audio scaffold: media type not audio/WaveFormatEx-inspectable.");
        if (mt.cbFormat && mt.pbFormat) CoTaskMemFree(mt.pbFormat);
        if (mt.pUnk) mt.pUnk->Release();
        return;
    }

    const WAVEFORMATEX* wf = reinterpret_cast<const WAVEFORMATEX*>(mt.pbFormat);
    AudioPcmQueueScaffold q{};
    q.delayMs = delayMs;
    q.sampleRate = wf->nSamplesPerSec;
    q.channels = wf->nChannels;
    q.bitsPerSample = wf->wBitsPerSample;
    q.blockAlign = wf->nBlockAlign ? wf->nBlockAlign : 1;
    q.avgBytesPerSec = wf->nAvgBytesPerSec;
    q.delayBytesExact = (q.avgBytesPerSec > 0)
        ? (unsigned long long(q.avgBytesPerSec) * (unsigned long long)delayMs) / 1000ull
        : 0ull;
    q.delayBytesAligned = q.delayBytesExact;
    if (q.blockAlign > 1) q.delayBytesAligned = (q.delayBytesAligned / q.blockAlign) * q.blockAlign;
    unsigned long long headroom = (q.avgBytesPerSec > 0)
        ? (unsigned long long(q.avgBytesPerSec) * 250ull) / 1000ull
        : 0ull;
    if (q.blockAlign > 1) headroom = (headroom / q.blockAlign) * q.blockAlign;
    q.capacityBytes = q.delayBytesAligned + headroom;
    if (q.blockAlign > 0 && q.sampleRate > 0) q.msPerSampleFrame = 1000.0 / double(q.sampleRate);

    try {
        if (q.capacityBytes > 0 && q.capacityBytes <= (128ull * 1024ull * 1024ull)) {
            q.backing.resize((size_t)q.capacityBytes);
        }
    } catch (...) {
        logw("Audio scaffold: queue backing allocation threw; continuing with metadata only.");
    }

    logi("Audio scaffold: fmt tag=0x%04x ch=%u rate=%lu bits=%u align=%u avgBps=%lu",
         (unsigned)wf->wFormatTag, (unsigned)wf->nChannels, (unsigned long)wf->nSamplesPerSec,
         (unsigned)wf->wBitsPerSample, (unsigned)q.blockAlign, (unsigned long)q.avgBytesPerSec);
    logi("Audio scaffold: queue target delay=%lu ms -> exact=%llu bytes aligned=%llu bytes",
         (unsigned long)delayMs, q.delayBytesExact, q.delayBytesAligned);
    logi("Audio scaffold: queue capacity=%llu bytes (~%lu ms incl. headroom), blockAlign=%u, ms/sample-frame~=%0.6f",
         q.capacityBytes,
         (unsigned long)((q.avgBytesPerSec > 0) ? ((q.capacityBytes * 1000ull) / q.avgBytesPerSec) : 0ull),
         (unsigned)q.blockAlign, q.msPerSampleFrame);
    logi("Audio scaffold: queue object initialized%s. No PCM is pushed yet.",
         q.backing.empty() ? " (metadata-only/no backing alloc)" : "");

    if (mt.cbFormat && mt.pbFormat) CoTaskMemFree(mt.pbFormat);
    if (mt.pUnk) mt.pUnk->Release();
}

static HRESULT force_render_any_dvd_outputs(DvdState& st) {
    ComPtr<IBaseFilter> nav = find_filter_by_name_substr(st.graph.get(), L"DVD Navigator");
    if (!nav) { logw("DVD Navigator filter not found by name."); return S_FALSE; }

    std::vector<ComPtr<IPin>> pins;
    if (!enum_pins(nav.get(), pins)) return E_FAIL;

    bool did = false;
    for (auto& pp : pins) {
        PIN_DIRECTION dir{};
        if (FAILED(pp->QueryDirection(&dir)) || dir != PINDIR_OUTPUT) continue;
        if (is_pin_connected(pp.get())) continue;

        logi("Force-render: rendering an unconnected DVD Navigator OUTPUT pin...");
        HRESULT hr = st.graph->Render(pp.get());
        logi("Render(pin) returned: 0x%08lx", (unsigned long)hr);
        did = true;
    }
    return did ? S_OK : S_FALSE;
}


// Step 3-C1 scaffold (safe/log-only): prepare the future PCM tap insertion plan without modifying the graph.
// Goal: confirm we can collect all info needed to break and replace the decoder->renderer edge later.
static void log_audio_pcm_tap_insertion_prep_step3c1(const DvdState& st, DWORD delayMs) {
    logi("Audio scaffold: PCM tap insertion prep only (no graph changes yet). delay=%lu ms",
         (unsigned long)delayMs);

    if (!st.graph) {
        logw("Audio scaffold: graph unavailable.");
        return;
    }

    ComPtr<IBaseFilter> aDec = find_filter_by_name_substr(st.graph.get(), L"LAV Audio Decoder");
    if (!aDec) aDec = find_filter_by_name_substr(st.graph.get(), L"Audio Decoder");
    ComPtr<IBaseFilter> aRend = find_filter_by_name_substr(st.graph.get(), L"DSound Renderer");
    if (!aRend) aRend = find_filter_by_name_substr(st.graph.get(), L"Audio Renderer");

    if (!aDec || !aRend) {
        logw("Audio scaffold: decoder/renderer not both found; tap prep deferred.");
        return;
    }

    std::vector<ComPtr<IPin>> dpins, rpins;
    enum_pins(aDec.get(), dpins);
    enum_pins(aRend.get(), rpins);

    ComPtr<IPin> decOut;
    ComPtr<IPin> rendIn;
    for (auto& p : dpins) {
        if (!p) continue;
        PIN_DIRECTION d{};
        if (FAILED(p->QueryDirection(&d)) || d != PINDIR_OUTPUT) continue;
        ComPtr<IPin> c;
        if (FAILED(p->ConnectedTo(c.put())) || !c) continue;
        for (auto& r : rpins) {
            if (!r) continue;
            if (r.get() == c.get()) {
                decOut.reset(p.get());
                if (decOut) decOut->AddRef();
                rendIn.reset(r.get());
                if (rendIn) rendIn->AddRef();
                break;
            }
        }
        if (decOut && rendIn) break;
    }

    if (!decOut || !rendIn) {
        logw("Audio scaffold: could not resolve connected decoder->renderer pins.");
        return;
    }

    ComPtr<IPin> back;
    HRESULT hrBack = decOut->ConnectedTo(back.put());
    if (FAILED(hrBack) || !back || back.get() != rendIn.get()) {
        logw("Audio scaffold: connection changed during inspection (hr=0x%08lx).", (unsigned long)hrBack);
        return;
    }

    AM_MEDIA_TYPE mt{};
    HRESULT hrmt = decOut->ConnectionMediaType(&mt);
    if (SUCCEEDED(hrmt)) {
        bool inspectable = (mt.formattype == FORMAT_WaveFormatEx && mt.cbFormat >= sizeof(WAVEFORMATEX) && mt.pbFormat);
        if (inspectable) {
            const WAVEFORMATEX* wf = reinterpret_cast<const WAVEFORMATEX*>(mt.pbFormat);
            const DWORD avgBps = wf->nAvgBytesPerSec;
            const WORD  blockAlign = wf->nBlockAlign ? wf->nBlockAlign : 1;
            unsigned long long exact = (avgBps > 0)
                ? (unsigned long long(avgBps) * (unsigned long long)delayMs) / 1000ull
                : 0ull;
            unsigned long long aligned = exact;
            if (blockAlign > 1) aligned = (aligned / blockAlign) * blockAlign;

            logi("Audio scaffold: tap target edge confirmed. avgBps=%lu blockAlign=%u delayMs=%lu delayBytesAligned=%llu",
                 (unsigned long)avgBps, (unsigned)blockAlign, (unsigned long)delayMs, aligned);
            logi("Audio scaffold: next step will insert a PCM tap/queue path between decoder output and renderer input while preserving menu control path.");
        } else {
            logw("Audio scaffold: media type not inspectable as WaveFormatEx.");
        }
        if (mt.cbFormat && mt.pbFormat) { CoTaskMemFree(mt.pbFormat); mt.pbFormat = nullptr; mt.cbFormat = 0; } if (mt.pUnk) { mt.pUnk->Release(); mt.pUnk = nullptr; }
    } else {
        logw("Audio scaffold: ConnectionMediaType failed: 0x%08lx", (unsigned long)hrmt);
    }
}



// Step 3-C2 scaffold (safe/log-only): build the "tap insertion transaction" plan and validate prerequisites.
// IMPORTANT: This still does NOT disconnect/reconnect pins.
static void log_audio_pcm_tap_component_scaffold_step3c2(const DvdState& st, DWORD delayMs) {
    logi("Audio scaffold: PCM tap component scaffolding only (no graph changes yet). delay=%lu ms",
         (unsigned long)delayMs);

    if (!st.graph) {
        logw("Audio scaffold: graph unavailable.");
        return;
    }

    ComPtr<IBaseFilter> aDec = find_filter_by_name_substr(st.graph.get(), L"LAV Audio Decoder");
    if (!aDec) aDec = find_filter_by_name_substr(st.graph.get(), L"Audio Decoder");
    ComPtr<IBaseFilter> aRend = find_filter_by_name_substr(st.graph.get(), L"DSound Renderer");
    if (!aRend) aRend = find_filter_by_name_substr(st.graph.get(), L"Audio Renderer");

    if (!aDec || !aRend) {
        logw("Audio scaffold: decoder/renderer not both found.");
        return;
    }

    std::vector<ComPtr<IPin>> dpins, rpins;
    enum_pins(aDec.get(), dpins);
    enum_pins(aRend.get(), rpins);

    ComPtr<IPin> decOut, rendIn;
    for (auto& p : dpins) {
        if (!p) continue;
        PIN_DIRECTION d{};
        if (FAILED(p->QueryDirection(&d)) || d != PINDIR_OUTPUT) continue;
        ComPtr<IPin> c;
        if (FAILED(p->ConnectedTo(c.put())) || !c) continue;
        PIN_INFO pi{};
        if (SUCCEEDED(c->QueryPinInfo(&pi))) {
            bool isRenderer = (pi.pFilter == aRend.get());
            if (pi.pFilter) pi.pFilter->Release();
            if (isRenderer) { decOut.reset(p.get()); if (decOut) decOut->AddRef(); rendIn.reset(c.get()); if (rendIn) rendIn->AddRef(); break; }
        }
    }
    if (!decOut || !rendIn) {
        logw("Audio scaffold: tap target edge not currently connected.");
        return;
    }

    AM_MEDIA_TYPE mt{};
    HRESULT hrmt = decOut->ConnectionMediaType(&mt);
    if (FAILED(hrmt)) {
        logw("Audio scaffold: ConnectionMediaType failed: 0x%08lx", (unsigned long)hrmt);
        return;
    }

    bool inspectable = (mt.majortype == MEDIATYPE_Audio && mt.formattype == FORMAT_WaveFormatEx &&
                        mt.pbFormat && mt.cbFormat >= sizeof(WAVEFORMATEX));
    if (!inspectable) {
        logw("Audio scaffold: target media type is not inspectable PCM/WaveFormatEx.");
        if (mt.cbFormat && mt.pbFormat) CoTaskMemFree(mt.pbFormat);
        if (mt.pUnk) mt.pUnk->Release();
        return;
    }

    const WAVEFORMATEX* wf = reinterpret_cast<const WAVEFORMATEX*>(mt.pbFormat);
    DWORD avgBps = wf->nAvgBytesPerSec;
    WORD blockAlign = wf->nBlockAlign ? wf->nBlockAlign : 1;
    unsigned long long delayBytes = (avgBps > 0)
        ? (unsigned long long(avgBps) * (unsigned long long)delayMs) / 1000ull
        : 0ull;
    if (blockAlign > 1) delayBytes = (delayBytes / blockAlign) * blockAlign;

    PIN_INFO outInfo{}, inInfo{};
    std::string outName = "(unknown)", inName = "(unknown)";
    if (SUCCEEDED(decOut->QueryPinInfo(&outInfo))) {
        outName = w2utf8(std::wstring(outInfo.achName));
        if (outInfo.pFilter) outInfo.pFilter->Release();
    }
    if (SUCCEEDED(rendIn->QueryPinInfo(&inInfo))) {
        inName = w2utf8(std::wstring(inInfo.achName));
        if (inInfo.pFilter) inInfo.pFilter->Release();
    }

    logi("Audio scaffold: tap transaction plan validated on edge [%s] -> [%s]", outName.c_str(), inName.c_str());
    logi("Audio scaffold: planned queue params ch=%u rate=%lu bits=%u align=%u avgBps=%lu delayBytes=%llu",
         (unsigned)wf->nChannels, (unsigned long)wf->nSamplesPerSec, (unsigned)wf->wBitsPerSample,
         (unsigned)blockAlign, (unsigned long)avgBps, delayBytes);
    logi("Audio scaffold: next step will introduce a PCM tap component and only then attempt decoder->renderer edge replacement.");

    if (mt.cbFormat && mt.pbFormat) CoTaskMemFree(mt.pbFormat);
    if (mt.pUnk) mt.pUnk->Release();
}


// Step 3-C3 (incremental actual component implementation): real PCM queue utilities + runtime preflight/self-test.
// IMPORTANT: Still no graph edge replacement in this step. We only validate queue behavior with the currently connected format.
struct PcmAudioFormat {
    WORD formatTag = 0;          // WAVE_FORMAT_PCM or WAVE_FORMAT_IEEE_FLOAT
    WORD channels = 0;
    DWORD sampleRate = 0;
    WORD bitsPerSample = 0;
    WORD blockAlign = 0;
    DWORD avgBytesPerSec = 0;
    bool isFloat = false;
    bool isSupported = false;
};

static bool extract_pcm_audio_format_from_mt(const AM_MEDIA_TYPE& mt, PcmAudioFormat& outFmt) {
    outFmt = PcmAudioFormat{};
    if (mt.majortype != MEDIATYPE_Audio) return false;
    if (!mt.pbFormat || mt.cbFormat < sizeof(WAVEFORMATEX)) return false;
    if (mt.formattype != FORMAT_WaveFormatEx) return false;

    const WAVEFORMATEX* wf = reinterpret_cast<const WAVEFORMATEX*>(mt.pbFormat);
    WORD tag = wf->wFormatTag;

    // Handle extensible headers by looking at SubFormat when available.
    if (tag == WAVE_FORMAT_EXTENSIBLE && mt.cbFormat >= sizeof(WAVEFORMATEXTENSIBLE)) {
        const WAVEFORMATEXTENSIBLE* wfxe = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(mt.pbFormat);
        if (wfxe->SubFormat == KSDATAFORMAT_SUBTYPE_PCM) tag = WAVE_FORMAT_PCM;
        else if (wfxe->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) tag = WAVE_FORMAT_IEEE_FLOAT;
    }

    outFmt.formatTag = tag;
    outFmt.channels = wf->nChannels;
    outFmt.sampleRate = wf->nSamplesPerSec;
    outFmt.bitsPerSample = wf->wBitsPerSample;
    outFmt.blockAlign = wf->nBlockAlign;
    outFmt.avgBytesPerSec = wf->nAvgBytesPerSec;
    outFmt.isFloat = (tag == WAVE_FORMAT_IEEE_FLOAT);

    bool supportedTag = (tag == WAVE_FORMAT_PCM || tag == WAVE_FORMAT_IEEE_FLOAT);
    bool sane = (outFmt.channels > 0 && outFmt.sampleRate > 0 &&
                 outFmt.blockAlign > 0 && outFmt.avgBytesPerSec > 0);
    outFmt.isSupported = (supportedTag && sane);
    return outFmt.isSupported;
}

struct AudioByteRingQueue {
    std::vector<unsigned char> buf;
    size_t r = 0;
    size_t w = 0;
    size_t used = 0;
    size_t blockAlign = 1;

    bool init(size_t capacityBytes, size_t alignBytes) {
        clear();
        blockAlign = (alignBytes > 0) ? alignBytes : 1;
        if (capacityBytes == 0) return true;
        if (blockAlign > 1) capacityBytes = (capacityBytes / blockAlign) * blockAlign;
        if (capacityBytes == 0) capacityBytes = blockAlign;
        try { buf.assign(capacityBytes, 0); }
        catch (...) { return false; }
        return true;
    }

    void clear() {
        buf.clear();
        r = w = used = 0;
        blockAlign = 1;
    }

    size_t capacity() const { return buf.size(); }
    size_t size_bytes() const { return used; }
    size_t free_bytes() const { return buf.size() - used; }

    size_t push_bytes(const unsigned char* p, size_t n) {
        if (!p || n == 0 || buf.empty()) return 0;
        if (blockAlign > 1) n = (n / blockAlign) * blockAlign;
        if (n == 0) return 0;
        if (n > free_bytes()) n = free_bytes();
        if (blockAlign > 1) n = (n / blockAlign) * blockAlign;
        if (n == 0) return 0;

        size_t first = std::min(n, buf.size() - w);
        memcpy(buf.data() + w, p, first);
        size_t rem = n - first;
        if (rem) memcpy(buf.data(), p + first, rem);
        w = (w + n) % buf.size();
        used += n;
        return n;
    }

    size_t pop_bytes(unsigned char* out, size_t n) {
        if (n == 0 || buf.empty()) return 0;
        if (blockAlign > 1) n = (n / blockAlign) * blockAlign;
        if (n == 0) return 0;
        if (n > used) n = used;
        if (blockAlign > 1) n = (n / blockAlign) * blockAlign;
        if (n == 0) return 0;

        size_t first = std::min(n, buf.size() - r);
        if (out) memcpy(out, buf.data() + r, first);
        size_t rem = n - first;
        if (rem && out) memcpy(out + first, buf.data(), rem);
        r = (r + n) % buf.size();
        used -= n;
        return n;
    }

    size_t push_silence(size_t nBytes) {
        if (buf.empty() || nBytes == 0) return 0;
        if (blockAlign > 1) nBytes = (nBytes / blockAlign) * blockAlign;
        if (nBytes == 0) return 0;
        if (nBytes > free_bytes()) nBytes = free_bytes();
        if (blockAlign > 1) nBytes = (nBytes / blockAlign) * blockAlign;
        if (nBytes == 0) return 0;

        size_t remaining = nBytes;
        while (remaining) {
            size_t chunk = std::min(remaining, buf.size() - w);
            memset(buf.data() + w, 0, chunk);
            w = (w + chunk) % buf.size();
            used += chunk;
            remaining -= chunk;
        }
        return nBytes;
    }
};


struct PcmDelayPlaybackState {
    std::mutex mtx;
    AudioByteRingQueue q;
    PcmAudioFormat fmt{};
    DWORD delayMs = 0;
    size_t delayBytes = 0;
    bool configured = false;
    bool released = false;
    bool stopReq = false;
    bool rendererMuted = false;
    LONG savedVolume = 0;
    bool haveSavedVolume = false;
    HWAVEOUT hwo = nullptr;
    HANDLE thread = nullptr;
    HANDLE wakeEvent = nullptr;
    size_t bytesPerChunk = 0;
    std::atomic<unsigned long long> cbCount{0};
    std::atomic<unsigned long long> bytesIn{0};
    std::atomic<unsigned long long> bytesOut{0};
};
static PcmDelayPlaybackState g_pcmDelay;

static void pcm_delay_restore_renderer_volume(DvdState& st) {
    if (!g_pcmDelay.rendererMuted || !st.basicAudio) return;
    LONG v = g_pcmDelay.haveSavedVolume ? g_pcmDelay.savedVolume : 0;
    (void)st.basicAudio->put_Volume(v);
    g_pcmDelay.rendererMuted = false;
    logi("Audio PCM delay: restored DirectShow renderer volume=%ld", (long)v);
}

static void pcm_delay_mute_renderer_if_needed(DvdState& st) {
    if (g_pcmDelay.rendererMuted || !st.basicAudio) return;
    LONG v = 0;
    if (SUCCEEDED(st.basicAudio->get_Volume(&v))) {
        g_pcmDelay.savedVolume = v;
        g_pcmDelay.haveSavedVolume = true;
    }
    if (SUCCEEDED(st.basicAudio->put_Volume(-10000))) {
        g_pcmDelay.rendererMuted = true;
        logi("Audio PCM delay: muted DirectShow renderer (tapped playback path active)");
    } else {
        logw("Audio PCM delay: failed to mute DirectShow renderer; duplicate audio may occur.");
    }
}

class AudioPcmDelayGrabberCB final : public ISampleGrabberCB {
    std::atomic<long> refcnt{1};
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (riid == IID_IUnknown) { *ppv = static_cast<ISampleGrabberCB*>(this); AddRef(); return S_OK; }
        *ppv = static_cast<ISampleGrabberCB*>(this); AddRef(); return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return (ULONG)++refcnt; }
    STDMETHODIMP_(ULONG) Release() override { long r = --refcnt; if (r == 0) delete this; return (ULONG)r; }
    STDMETHODIMP SampleCB(double, IMediaSample*) override { return E_NOTIMPL; }
    STDMETHODIMP BufferCB(double, BYTE* pBuffer, long BufferLen) override {
        if (!pBuffer || BufferLen <= 0) return S_OK;
        auto& s = g_pcmDelay;
        std::lock_guard<std::mutex> lock(s.mtx);
        if (!s.configured || s.stopReq) return S_OK;
        size_t pushed = s.q.push_bytes((const unsigned char*)pBuffer, (size_t)BufferLen);
        s.cbCount.fetch_add(1, std::memory_order_relaxed);
        s.bytesIn.fetch_add((unsigned long long)pushed, std::memory_order_relaxed);
        if (!s.released && s.q.size_bytes() >= s.delayBytes) {
            s.released = true;
            logi("Audio PCM delay: release threshold reached (queued=%zu target=%zu delayMs=%lu)",
                 s.q.size_bytes(), s.delayBytes, (unsigned long)s.delayMs);
        }
        if (s.wakeEvent) SetEvent(s.wakeEvent);
        return S_OK;
    }
};
static AudioPcmDelayGrabberCB* g_pcmDelayCb = nullptr;

struct WaveOutChunk { WAVEHDR hdr{}; std::vector<unsigned char> data; bool prepared = false; };

static DWORD WINAPI pcm_delay_waveout_thread_proc(LPVOID) {
    auto& s = g_pcmDelay;
    std::vector<WaveOutChunk> chunks(6);
    for (auto& c : chunks) {
        c.data.assign(s.bytesPerChunk ? s.bytesPerChunk : 4096, 0);
        c.hdr.lpData = (LPSTR)c.data.data();
        c.hdr.dwBufferLength = (DWORD)c.data.size();
    }
    while (!s.stopReq) {
        bool wrote = false;
        for (auto& c : chunks) {
            if (c.hdr.dwFlags & WHDR_INQUEUE) continue;
            {
                std::lock_guard<std::mutex> lock(s.mtx);
                size_t n = 0;
                if (s.configured && s.released) n = s.q.pop_bytes(c.data.data(), c.data.size());
                if (n < c.data.size()) memset(c.data.data() + n, 0, c.data.size() - n);
                s.bytesOut.fetch_add((unsigned long long)n, std::memory_order_relaxed);
            }
            c.hdr.dwBufferLength = (DWORD)c.data.size();
            if (!c.prepared) { if (waveOutPrepareHeader(s.hwo, &c.hdr, sizeof(c.hdr)) != MMSYSERR_NOERROR) continue; c.prepared = true; }
            if (waveOutWrite(s.hwo, &c.hdr, sizeof(c.hdr)) == MMSYSERR_NOERROR) wrote = true;
        }
        if (s.wakeEvent) { WaitForSingleObject(s.wakeEvent, wrote ? 5 : 20); ResetEvent(s.wakeEvent); }
        else Sleep(wrote ? 5 : 20);
    }
    if (s.hwo) {
        waveOutReset(s.hwo);
        for (auto& c : chunks) if (c.prepared) waveOutUnprepareHeader(s.hwo, &c.hdr, sizeof(c.hdr));
    }
    return 0;
}

static bool pcm_delay_start_from_format(DvdState& st, const PcmAudioFormat& fmt, DWORD delayMs) {
    auto& s = g_pcmDelay;
    if (!fmt.isSupported || fmt.isFloat || fmt.formatTag != WAVE_FORMAT_PCM) {
        logw("Audio PCM delay: unsupported waveOut format (tag=0x%04x float=%d)", (unsigned)fmt.formatTag, fmt.isFloat ? 1 : 0);
        return false;
    }
    // reset previous
    s.stopReq = true;
    if (s.wakeEvent) SetEvent(s.wakeEvent);
    if (s.thread) { WaitForSingleObject(s.thread, 2000); CloseHandle(s.thread); s.thread = nullptr; }
    if (s.hwo) { waveOutClose(s.hwo); s.hwo = nullptr; }
    if (s.wakeEvent) { CloseHandle(s.wakeEvent); s.wakeEvent = nullptr; }
    { std::lock_guard<std::mutex> lock(s.mtx); s.q.clear(); }
    s.fmt = fmt; s.delayMs = delayMs; s.configured = false; s.released = false; s.stopReq = false; s.bytesPerChunk = 0;
    s.cbCount.store(0); s.bytesIn.store(0); s.bytesOut.store(0);

    unsigned long long d = ((unsigned long long)fmt.avgBytesPerSec * (unsigned long long)delayMs) / 1000ull;
    if (fmt.blockAlign > 1) d = (d / fmt.blockAlign) * fmt.blockAlign;
    s.delayBytes = (size_t)d;
    unsigned long long head = ((unsigned long long)fmt.avgBytesPerSec * 800ull) / 1000ull;
    if (fmt.blockAlign > 1) head = (head / fmt.blockAlign) * fmt.blockAlign;
    size_t cap = (size_t)std::min<unsigned long long>(64ull * 1024ull * 1024ull, d + head + 4096ull);
    if (cap < (size_t)(fmt.blockAlign ? fmt.blockAlign : 1)) cap = (size_t)(fmt.blockAlign ? fmt.blockAlign : 1);
    if (!s.q.init(cap, fmt.blockAlign ? fmt.blockAlign : 1)) { logw("Audio PCM delay: queue init failed"); return false; }

    WAVEFORMATEX wfx{};
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = fmt.channels;
    wfx.nSamplesPerSec = fmt.sampleRate;
    wfx.wBitsPerSample = fmt.bitsPerSample;
    wfx.nBlockAlign = fmt.blockAlign;
    wfx.nAvgBytesPerSec = fmt.avgBytesPerSec;
    MMRESULT mm = waveOutOpen(&s.hwo, WAVE_MAPPER, &wfx, 0, 0, CALLBACK_NULL);
    if (mm != MMSYSERR_NOERROR || !s.hwo) { logw("Audio PCM delay: waveOutOpen failed mm=%u", (unsigned)mm); s.hwo = nullptr; return false; }
    s.bytesPerChunk = std::max<size_t>(fmt.blockAlign ? fmt.blockAlign : 1, (size_t)(fmt.avgBytesPerSec / 50));
    if (fmt.blockAlign > 1) s.bytesPerChunk = (s.bytesPerChunk / fmt.blockAlign) * fmt.blockAlign;
    if (s.bytesPerChunk == 0) s.bytesPerChunk = fmt.blockAlign ? fmt.blockAlign : 1;
    s.wakeEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    s.released = (s.delayBytes == 0);
    s.configured = true;
    pcm_delay_mute_renderer_if_needed(st);
    s.thread = CreateThread(nullptr, 0, pcm_delay_waveout_thread_proc, nullptr, 0, nullptr);
    if (!s.thread) {
        logw("Audio PCM delay: CreateThread failed");
        s.stopReq = true;
        if (s.hwo) { waveOutClose(s.hwo); s.hwo = nullptr; }
        if (s.wakeEvent) { CloseHandle(s.wakeEvent); s.wakeEvent = nullptr; }
        s.configured = false;
        pcm_delay_restore_renderer_volume(st);
        return false;
    }
    logi("Audio PCM delay: START ch=%u rate=%lu bits=%u align=%u avgBps=%lu delayMs=%lu delayBytes=%zu queueCap=%zu chunk=%zu",
         (unsigned)fmt.channels, (unsigned long)fmt.sampleRate, (unsigned)fmt.bitsPerSample,
         (unsigned)fmt.blockAlign, (unsigned long)fmt.avgBytesPerSec, (unsigned long)delayMs,
         s.delayBytes, s.q.capacity(), s.bytesPerChunk);
    return true;
}

static void pcm_delay_stop(DvdState& st) {
    auto& s = g_pcmDelay;
    if (!s.thread && !s.hwo && !s.rendererMuted) return;
    s.stopReq = true;
    if (s.wakeEvent) SetEvent(s.wakeEvent);
    if (s.thread) { WaitForSingleObject(s.thread, 3000); CloseHandle(s.thread); s.thread = nullptr; }
    if (s.hwo) { waveOutClose(s.hwo); s.hwo = nullptr; }
    if (s.wakeEvent) { CloseHandle(s.wakeEvent); s.wakeEvent = nullptr; }
    { std::lock_guard<std::mutex> lock(s.mtx); s.q.clear(); s.configured = false; s.released = false; }
    logi("Audio PCM delay: STOP cb=%llu in=%llu out=%llu", (unsigned long long)s.cbCount.load(), (unsigned long long)s.bytesIn.load(), (unsigned long long)s.bytesOut.load());
    pcm_delay_restore_renderer_volume(st);
}

static void run_audio_pcm_component_selftest_step3c3(const DvdState& st, DWORD delayMs) {
    logi("Audio Step 3-C3: PCM queue component self-test (real queue ops, no graph changes). delay=%lu ms",
         (unsigned long)delayMs);

    if (!st.graph) {
        logw("Audio Step 3-C3: graph unavailable.");
        return;
    }

    ComPtr<IBaseFilter> aDec = find_filter_by_name_substr(st.graph.get(), L"LAV Audio Decoder");
    if (!aDec) aDec = find_filter_by_name_substr(st.graph.get(), L"Audio Decoder");
    ComPtr<IBaseFilter> aRend = find_filter_by_name_substr(st.graph.get(), L"DSound Renderer");
    if (!aRend) aRend = find_filter_by_name_substr(st.graph.get(), L"Audio Renderer");
    if (!aDec || !aRend) {
        logw("Audio Step 3-C3: decoder/renderer not both found.");
        return;
    }

    ComPtr<IPin> decOut, rendIn;
    AM_MEDIA_TYPE mt{};
    if (!find_connected_audio_decoder_renderer_edge(st, decOut, rendIn, &mt)) {
        logw("Audio Step 3-C3: connected decoder->renderer edge not found.");
        return;
    }

    PcmAudioFormat fmt{};
    bool okFmt = extract_pcm_audio_format_from_mt(mt, fmt);
    if (!okFmt) {
        const WAVEFORMATEX* wf = (mt.pbFormat && mt.cbFormat >= sizeof(WAVEFORMATEX))
            ? reinterpret_cast<const WAVEFORMATEX*>(mt.pbFormat) : nullptr;
        if (wf) {
            logw("Audio Step 3-C3: audio edge format not supported for PCM queue yet (tag=0x%04x formattype!=PCM/float?).",
                 (unsigned)wf->wFormatTag);
        } else {
            logw("Audio Step 3-C3: media type is not WaveFormatEx-inspectable PCM.");
        }
        if (mt.cbFormat && mt.pbFormat) CoTaskMemFree(mt.pbFormat);
        if (mt.pUnk) mt.pUnk->Release();
        return;
    }

    unsigned long long delayBytes = (unsigned long long(fmt.avgBytesPerSec) * (unsigned long long)delayMs) / 1000ull;
    if (fmt.blockAlign > 1) delayBytes = (delayBytes / fmt.blockAlign) * fmt.blockAlign;
    unsigned long long headroom = (unsigned long long(fmt.avgBytesPerSec) * 250ull) / 1000ull;
    if (fmt.blockAlign > 1) headroom = (headroom / fmt.blockAlign) * fmt.blockAlign;
    unsigned long long cap64 = delayBytes + headroom;
    if (cap64 == 0) cap64 = fmt.blockAlign ? fmt.blockAlign : 1;
    if (cap64 > 64ull * 1024ull * 1024ull) cap64 = 64ull * 1024ull * 1024ull;

    AudioByteRingQueue q;
    if (!q.init((size_t)cap64, (size_t)(fmt.blockAlign ? fmt.blockAlign : 1))) {
        logw("Audio Step 3-C3: queue allocation failed (cap=%llu).", cap64);
        if (mt.cbFormat && mt.pbFormat) CoTaskMemFree(mt.pbFormat);
        if (mt.pUnk) mt.pUnk->Release();
        return;
    }

    // Pre-fill delay with silence (what the future tap path will do conceptually).
    size_t silencePushed = q.push_silence((size_t)delayBytes);

    // Push one block of synthetic PCM-ish bytes and pop them back to verify alignment-safe queue mechanics.
    const size_t oneBlock = (size_t)(fmt.blockAlign ? fmt.blockAlign : 1);
    std::vector<unsigned char> sample(oneBlock, 0x5A);
    std::vector<unsigned char> out(oneBlock, 0x00);
    size_t nPush = q.push_bytes(sample.data(), sample.size());
    size_t nPop  = q.pop_bytes(out.data(), out.size());

    bool patternOk = (nPop == oneBlock);
    for (size_t i = 0; patternOk && i < oneBlock; ++i) {
        if (out[i] != 0x00) patternOk = false; // first pop should be silence from delay prefill (if any)
    }

    logi("Audio Step 3-C3: format=%s tag=0x%04x ch=%u rate=%lu bits=%u align=%u avgBps=%lu",
         fmt.isFloat ? "floatPCM" : "PCM",
         (unsigned)fmt.formatTag, (unsigned)fmt.channels, (unsigned long)fmt.sampleRate,
         (unsigned)fmt.bitsPerSample, (unsigned)fmt.blockAlign, (unsigned long)fmt.avgBytesPerSec);
    logi("Audio Step 3-C3: queue cap=%zu delayFill=%zu pushTest=%zu popTest=%zu usedAfter=%zu pattern=%s",
         q.capacity(), silencePushed, nPush, nPop, q.size_bytes(), patternOk ? "OK(silence-prefill)" : "WARN");

    if (mt.cbFormat && mt.pbFormat) CoTaskMemFree(mt.pbFormat);
    if (mt.pUnk) mt.pUnk->Release();
}

static HRESULT ensure_vmr9_and_connect(DvdState& st) {
    HRESULT hr = CoCreateInstance(CLSID_VideoMixingRenderer9, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_IBaseFilter, (void**)st.vmr9.put());
    if (FAILED(hr) || !st.vmr9) { loge("Failed to create VMR9: 0x%08lx", (unsigned long)hr); return hr; }

    hr = st.graph->AddFilter(st.vmr9.get(), L"VMR9 (dvdmenu)");
    if (FAILED(hr)) { loge("AddFilter(VMR9) failed: 0x%08lx", (unsigned long)hr); return hr; }

    ComPtr<IVMRFilterConfig9> cfg;
    hr = st.vmr9->QueryInterface(IID_IVMRFilterConfig9, (void**)cfg.put());
    if (FAILED(hr) || !cfg) return FAILED(hr) ? hr : E_NOINTERFACE;

    hr = cfg->SetRenderingMode(VMR9Mode_Windowless);
    if (FAILED(hr)) { loge("VMR9 SetRenderingMode(Windowless) failed: 0x%08lx", (unsigned long)hr); return hr; }

    // Enable mixer with two streams (video + subpicture/highlight) so DVD menu highlights can be composed.
    HRESULT hrStreams = cfg->SetNumberOfStreams(2);
    if (FAILED(hrStreams)) {
        logw("VMR9 SetNumberOfStreams(2) failed: 0x%08lx (continuing)", (unsigned long)hrStreams);
    }

    hr = st.vmr9->QueryInterface(IID_IVMRWindowlessControl9, (void**)st.wl.put());
    if (FAILED(hr) || !st.wl) return FAILED(hr) ? hr : E_NOINTERFACE;

    st.ownerHwnd = create_offscreen_owner_window();
    if (!st.ownerHwnd) return E_FAIL;

    hr = st.wl->SetVideoClippingWindow(st.ownerHwnd);
    if (FAILED(hr)) { loge("SetVideoClippingWindow failed: 0x%08lx", (unsigned long)hr); return hr; }

    // Find a video decoder filter (prefer LAV)
    ComPtr<IBaseFilter> vdec = find_filter_by_name_substr(st.graph.get(), L"LAV Video Decoder");
    if (!vdec) vdec = find_filter_by_name_substr(st.graph.get(), L"Video Decoder");
    if (!vdec) { loge("Could not find video decoder filter in graph."); return E_FAIL; }

    // Try to connect SubPicture path so DVD menu button highlights can be generated/composited.
    ComPtr<IBaseFilter> nav = find_filter_by_name_substr(st.graph.get(), L"DVD Navigator");
    if (nav) {
        try_connect_subpicture_nav_to_decoder(st.graph.get(), nav.get(), vdec.get());
    } else {
        logw("DVD Navigator filter not found by name when connecting SubPicture.");
    }

    // Find decoder OUTPUT (can be connected to some renderer - we'll disconnect)
    IPin* out = first_unconnected_pin(vdec.get(), PINDIR_OUTPUT);
    if (!out) {
        // take first output pin even if connected
        std::vector<ComPtr<IPin>> pins;
        enum_pins(vdec.get(), pins);
        for (auto& pp : pins) {
            PIN_DIRECTION d{};
            if (SUCCEEDED(pp->QueryDirection(&d)) && d == PINDIR_OUTPUT) { out = pp.get(); out->AddRef(); break; }
        }
    }
    if (!out) { loge("Could not get video decoder output pin."); return E_FAIL; }
    ComPtr<IPin> outPin; outPin.attach(out);

    // VMR9 input pin (unconnected)
    IPin* in = first_unconnected_pin(st.vmr9.get(), PINDIR_INPUT);
    if (!in) { loge("Could not get VMR9 input pin."); return E_FAIL; }
    ComPtr<IPin> inPin; inPin.attach(in);

    // Disconnect any existing connections
    st.graph->Disconnect(outPin.get());
    st.graph->Disconnect(inPin.get());

    hr = st.graph->Connect(outPin.get(), inPin.get());
    if (FAILED(hr)) { loge("Connect(VideoDecoder->VMR9) failed: 0x%08lx", (unsigned long)hr); return hr; }

    logi("Connected video decoder output -> VMR9.");

    // If the decoder exposes a SubPicture output, connect it to the second VMR9 stream.
    try_connect_decoder_subpicture_to_vmr9(st.graph.get(), vdec.get(), st.vmr9.get());

    return S_OK;
}

static HRESULT build_dvd_graph(DvdState& st, const std::wstring& dvdRoot) {
    HRESULT hr = CoCreateInstance(CLSID_DvdGraphBuilder, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_IDvdGraphBuilder, (void**)st.dvdgb.put());
    if (FAILED(hr)) { loge("CoCreateInstance(CLSID_DvdGraphBuilder) failed: 0x%08lx", (unsigned long)hr); return hr; }

    hr = st.dvdgb->GetFiltergraph(st.graph.put());
    if (FAILED(hr)) { loge("GetFiltergraph failed: 0x%08lx", (unsigned long)hr); return hr; }

    AM_DVD_RENDERSTATUS rs{};
    hr = st.dvdgb->RenderDvdVideoVolume(dvdRoot.c_str(), 0, &rs);
    logi("RenderDvdVideoVolume returned: 0x%08lx", (unsigned long)hr);
    if (hr == S_FALSE) logw("NOTE: S_FALSE means partial render. Continuing anyway.");
    if (FAILED(hr)) return hr;

    // Remove auto-inserted video renderers (the ones that pop up an extra "ActiveMovie Window").
    remove_default_video_renderers(st.graph.get());

    dump_graph(st.graph.get());

    st.graph->QueryInterface(IID_IMediaControl, (void**)st.mc.put());
    st.graph->QueryInterface(IID_IBasicAudio, (void**)st.basicAudio.put());
    st.dvdgb->GetDvdInterface(IID_IDvdControl2, (void**)st.dvdctl.put());
    st.dvdgb->GetDvdInterface(IID_IDvdInfo2, (void**)st.dvdinfo.put());
    if (st.dvdinfo) {
        g_lastKnownDvdTitleCount = dvd_get_title_count(st.dvdinfo.get());
    }
    return S_OK;
}
struct Frame {
    int w=0, h=0;
    std::vector<uint8_t> bgr0; // w*h*4
};

static size_t dib_pixel_offset(const BITMAPINFOHEADER* bih) {
    if (!bih) return 0;
    size_t off = bih->biSize;
    if (bih->biCompression == BI_BITFIELDS) off += 12;
    if (bih->biBitCount <= 8) {
        DWORD colors = bih->biClrUsed;
        if (colors == 0) colors = 1u << bih->biBitCount;
        off += (size_t)colors * 4;
    }
    return off;
}


static bool capture_vmr9(DvdState& st, Frame& out, HRESULT* outHr=nullptr) {
    if (!st.wl) return false;
    // PAL DVD menus are often still-image + subpicture overlays. In that state,
    // VMR9 may not refresh the composited frame unless we explicitly repaint.
    if (st.ownerHwnd) {
        (void)st.wl->RepaintVideo(st.ownerHwnd, nullptr);
    }
    BYTE* dib = nullptr;
    HRESULT hr = st.wl->GetCurrentImage(&dib);
    if (outHr) *outHr = hr;
    if (FAILED(hr) || !dib) return false;

    auto* bih = (BITMAPINFOHEADER*)dib;
    int w = (int)bih->biWidth;
    int h = (int)abs(bih->biHeight);
    int bpp = (int)bih->biBitCount;
    int bppBytes = (bpp + 7) / 8;
    if (w <= 0 || h <= 0 || (bpp != 24 && bpp != 32)) { CoTaskMemFree(dib); return false; }

    size_t off = dib_pixel_offset(bih);
    const uint8_t* src = (const uint8_t*)dib + off;
    int stride = ((w * bppBytes + 3) / 4) * 4;
    bool bottomUp = (bih->biHeight > 0);

    out.w = w; out.h = h;
    const size_t need = (size_t)w * (size_t)h * 4;
    if (out.bgr0.size() != need) out.bgr0.resize(need);

    for (int y=0; y<h; y++) {
        int sy = bottomUp ? (h-1-y) : y;
        const uint8_t* srow = src + (size_t)sy * (size_t)stride;
        uint8_t* drow = out.bgr0.data() + (size_t)y * (size_t)w * 4;
        if (bpp == 32) {
            for (int x=0; x<w; x++) {
                drow[x*4+0]=srow[x*4+0];
                drow[x*4+1]=srow[x*4+1];
                drow[x*4+2]=srow[x*4+2];
                drow[x*4+3]=0;
            }
        } else {
            for (int x=0; x<w; x++) {
                drow[x*4+0]=srow[x*3+0];
                drow[x*4+1]=srow[x*3+1];
                drow[x*4+2]=srow[x*3+2];
                drow[x*4+3]=0;
            }
        }
    }

    CoTaskMemFree(dib);
    return true;
}

static void scale_nearest_bgr0(const Frame& src, int outW, int outH, std::vector<uint8_t>& out) {
    const size_t need = (size_t)outW * (size_t)outH * 4;
    if (out.size() != need) out.resize(need);
    if (src.w <= 0 || src.h <= 0 || src.bgr0.empty()) {
        if (!out.empty()) memset(out.data(), 0, out.size());
        return;
    }
    if (src.w == outW && src.h == outH && src.bgr0.size() == need) {
        memcpy(out.data(), src.bgr0.data(), need);
        return;
    }

    for (int y=0; y<outH; y++) {
        int sy = (int)((long long)y * src.h / outH);
        const uint8_t* srow = src.bgr0.data() + (size_t)sy * (size_t)src.w * 4;
        uint8_t* drow = out.data() + (size_t)y * (size_t)outW * 4;
        for (int x=0; x<outW; x++) {
            int sx = (int)((long long)x * src.w / outW);
            const uint8_t* sp = srow + (size_t)sx * 4;
            uint8_t* dp = drow + (size_t)x * 4;
            dp[0]=sp[0]; dp[1]=sp[1]; dp[2]=sp[2]; dp[3]=0;
        }
    }
}

static void compose_aspect_correct_bgr0(const Frame& src,
                                        int canvasW, int canvasH,
                                        int darX, int darY,
                                        std::vector<uint8_t>& out) {
    const size_t need = (size_t)canvasW * (size_t)canvasH * 4;
    if (out.size() != need) out.resize(need);
    if (out.empty()) return;
    std::memset(out.data(), 0, out.size());

    if (src.w <= 0 || src.h <= 0 || src.bgr0.empty() || canvasW <= 0 || canvasH <= 0) return;

    int dstW = canvasW;
    int dstH = canvasH;

    if (darX > 0 && darY > 0) {
        double dar = (double)darX / (double)darY;
        int wantW = (int)((double)canvasH * dar + 0.5);
        int wantH = canvasH;

        if (wantW > canvasW) {
            wantW = canvasW;
            wantH = (int)((double)canvasW / dar + 0.5);
        }
        if (wantW > 0 && wantH > 0) {
            if (wantW & 1) wantW -= 1;
            if (wantH & 1) wantH -= 1;
            if (wantW > 0) dstW = wantW;
            if (wantH > 0) dstH = wantH;
        }
    }

    int offX = (canvasW - dstW) / 2;
    int offY = (canvasH - dstH) / 2;
    if (offX < 0) offX = 0;
    if (offY < 0) offY = 0;

    for (int y = 0; y < dstH; ++y) {
        int sy = (int)((long long)y * src.h / dstH);
        const uint8_t* srow = src.bgr0.data() + (size_t)sy * (size_t)src.w * 4;
        uint8_t* drow = out.data() + (size_t)(y + offY) * (size_t)canvasW * 4 + (size_t)offX * 4;
        for (int x = 0; x < dstW; ++x) {
            int sx = (int)((long long)x * src.w / dstW);
            const uint8_t* sp = srow + (size_t)sx * 4;
            uint8_t* dp = drow + (size_t)x * 4;
            dp[0] = sp[0];
            dp[1] = sp[1];
            dp[2] = sp[2];
            dp[3] = 0;
        }
    }
}


static inline uint8_t clip_u8_int(int v) {
    return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
}
static inline uint8_t rgb_to_y_bt601_limited(int r, int g, int b) {
    return clip_u8_int(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
}
static inline uint8_t rgb_to_u_bt601_limited(int r, int g, int b) {
    return clip_u8_int(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
}
static inline uint8_t rgb_to_v_bt601_limited(int r, int g, int b) {
    return clip_u8_int(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);
}
static void bgr0_to_nv12(const std::vector<uint8_t>& bgr0, int w, int h, std::vector<uint8_t>& outNv12) {
    const size_t yBytes = (size_t)w * (size_t)h;
    const size_t uvBytes = yBytes / 2;
    const size_t need = yBytes + uvBytes;
    if (outNv12.size() != need) outNv12.resize(need);
    if (bgr0.size() < (size_t)w * (size_t)h * 4 || w <= 0 || h <= 0) {
        if (!outNv12.empty()) memset(outNv12.data(), 0, outNv12.size());
        return;
    }

    uint8_t* yPlane = outNv12.data();
    uint8_t* uvPlane = outNv12.data() + yBytes;

    for (int y = 0; y < h; ++y) {
        const uint8_t* srow = bgr0.data() + (size_t)y * (size_t)w * 4;
        uint8_t* yrow = yPlane + (size_t)y * (size_t)w;
        for (int x = 0; x < w; ++x) {
            int b = srow[x * 4 + 0];
            int g = srow[x * 4 + 1];
            int r = srow[x * 4 + 2];
            yrow[x] = rgb_to_y_bt601_limited(r, g, b);
        }
    }

    for (int y = 0; y < h; y += 2) {
        const uint8_t* row0 = bgr0.data() + (size_t)y * (size_t)w * 4;
        const uint8_t* row1 = bgr0.data() + (size_t)std::min(y + 1, h - 1) * (size_t)w * 4;
        uint8_t* uvRow = uvPlane + (size_t)(y / 2) * (size_t)w;
        for (int x = 0; x < w; x += 2) {
            int x1 = std::min(x + 1, w - 1);
            const uint8_t* p00 = row0 + (size_t)x * 4;
            const uint8_t* p01 = row0 + (size_t)x1 * 4;
            const uint8_t* p10 = row1 + (size_t)x * 4;
            const uint8_t* p11 = row1 + (size_t)x1 * 4;

            int b = (int(p00[0]) + int(p01[0]) + int(p10[0]) + int(p11[0]) + 2) >> 2;
            int g = (int(p00[1]) + int(p01[1]) + int(p10[1]) + int(p11[1]) + 2) >> 2;
            int r = (int(p00[2]) + int(p01[2]) + int(p10[2]) + int(p11[2]) + 2) >> 2;

            uvRow[x + 0] = rgb_to_u_bt601_limited(r, g, b);
            uvRow[x + 1] = rgb_to_v_bt601_limited(r, g, b);
        }
    }
}

static void bgr0_to_yuv444p(const std::vector<uint8_t>& bgr0, int w, int h, std::vector<uint8_t>& outYuv444p) {
    const size_t planeBytes = (size_t)w * (size_t)h;
    const size_t need = planeBytes * 3;
    if (outYuv444p.size() != need) outYuv444p.resize(need);
    if (bgr0.size() < (size_t)w * (size_t)h * 4 || w <= 0 || h <= 0) {
        if (!outYuv444p.empty()) memset(outYuv444p.data(), 0, outYuv444p.size());
        return;
    }

    uint8_t* yPlane = outYuv444p.data();
    uint8_t* uPlane = outYuv444p.data() + planeBytes;
    uint8_t* vPlane = outYuv444p.data() + planeBytes * 2;

    for (int y = 0; y < h; ++y) {
        const uint8_t* srow = bgr0.data() + (size_t)y * (size_t)w * 4;
        uint8_t* yrow = yPlane + (size_t)y * (size_t)w;
        uint8_t* urow = uPlane + (size_t)y * (size_t)w;
        uint8_t* vrow = vPlane + (size_t)y * (size_t)w;
        for (int x = 0; x < w; ++x) {
            int b = srow[x * 4 + 0];
            int g = srow[x * 4 + 1];
            int r = srow[x * 4 + 2];
            yrow[x] = rgb_to_y_bt601_limited(r, g, b);
            urow[x] = rgb_to_u_bt601_limited(r, g, b);
            vrow[x] = rgb_to_v_bt601_limited(r, g, b);
        }
    }
}

static bool write_bmp32_bgr0(const std::wstring& path, int w, int h, const uint8_t* bgr0) {
    std::wstring p = adjust_path_if_directory(path, L"first.bmp");
    ensure_parent_dir_recursive(p);

    FILE* f = _wfopen(p.c_str(), L"wb");
    if (!f) {
        // If the target is a directory (or permission issue), try alternate name in same dir.
        std::wstring alt = alternate_path_same_dir(p, L"first_dump.bmp");
        ensure_parent_dir_recursive(alt);
        f = _wfopen(alt.c_str(), L"wb");
        if (!f) return false;
        p = alt;
    }

    BITMAPFILEHEADER bfh{};
    BITMAPINFOHEADER bih{};
    bih.biSize = sizeof(BITMAPINFOHEADER);
    bih.biWidth = w;
    bih.biHeight = -h; // top-down
    bih.biPlanes = 1;
    bih.biBitCount = 32;
    bih.biCompression = BI_RGB;
    bih.biSizeImage = (DWORD)((size_t)w * (size_t)h * 4);

    bfh.bfType = 0x4D42;
    bfh.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    bfh.bfSize = bfh.bfOffBits + bih.biSizeImage;

    fwrite(&bfh, sizeof(bfh), 1, f);
    fwrite(&bih, sizeof(bih), 1, f);
    fwrite(bgr0, bih.biSizeImage, 1, f);
    fclose(f);
    return true;
}

static bool is_all_black(const std::vector<uint8_t>& bgr0) {
    // check some samples for speed
    if (bgr0.empty()) return true;
    size_t n = bgr0.size();
    size_t step = std::max<size_t>(4, n / 4096);
    for (size_t i=0; i<n; i+=step) {
        if (bgr0[i] || bgr0[i+1] || bgr0[i+2]) return false;
    }
    return true;
}


// ---------------- simple menu highlight overlay ----------------
// Many DVDs draw menu highlights via subpicture (SPU). Depending on renderer/capture path,
// GetCurrentImage may not include the highlight. To make navigation visible and debuggable,
// we draw a simple rectangle over the currently-selected button if the DVD Navigator reports it.
static void draw_rect_outline_bgr0(std::vector<uint8_t>& bgr0, int w, int h, const RECT& rc, int thick=2) {
    if (bgr0.empty() || w<=0 || h<=0) return;
    int l = (int)std::max<long>(0, rc.left);
    int t = (int)std::max<long>(0, rc.top);
    int r = (int)std::min<long>(w-1, rc.right);
    int b = (int)std::min<long>(h-1, rc.bottom);
    if (r<=l || b<=t) return;

    auto put = [&](int x,int y){
        if ((unsigned)x >= (unsigned)w || (unsigned)y >= (unsigned)h) return;
        size_t off = ((size_t)y*(size_t)w + (size_t)x)*4;
        // High-contrast outline: invert underlying pixel so it is visible on any background.
        bgr0[off+0] = (uint8_t)(255 - bgr0[off+0]);
        bgr0[off+1] = (uint8_t)(255 - bgr0[off+1]);
        bgr0[off+2] = (uint8_t)(255 - bgr0[off+2]);
    };

    for (int k=0;k<thick;k++){
        int yy1=t+k, yy2=b-k;
        for(int x=l;x<=r;x++){ put(x,yy1); put(x,yy2); }
        int xx1=l+k, xx2=r-k;
        for(int y=t;y<=b;y++){ put(xx1,y); put(xx2,y); }
    }
}


static void fill_rect_bgr0(std::vector<uint8_t>& bgr0, int w, int h, int x0, int y0, int x1, int y1, uint8_t b, uint8_t g, uint8_t r) {
    if (bgr0.empty() || w<=0 || h<=0) return;
    x0 = std::max(0, std::min(w, x0));
    x1 = std::max(0, std::min(w, x1));
    y0 = std::max(0, std::min(h, y0));
    y1 = std::max(0, std::min(h, y1));
    if (x1<=x0 || y1<=y0) return;
    for (int y=y0; y<y1; ++y) {
        size_t row = (size_t)y*(size_t)w*4;
        for (int x=x0; x<x1; ++x) {
            size_t off = row + (size_t)x*4;
            bgr0[off+0]=b; bgr0[off+1]=g; bgr0[off+2]=r;
        }
    }
}

static void draw_seg(std::vector<uint8_t>& bgr0, int w, int h, int x, int y, int ww, int hh, int t, uint8_t b, uint8_t g, uint8_t r) {
    (void)t;
    fill_rect_bgr0(bgr0, w, h, x, y, x+ww, y+hh, b, g, r);
}

// 7-seg digit renderer (very simple, chunky). Good enough to show button index changes.
static void draw_digit7(std::vector<uint8_t>& bgr0, int w, int h, int x, int y, int s, int digit, uint8_t b, uint8_t g, uint8_t r) {
    // Segment layout within a 3*s by 5*s box.
    // a: top, b: upper-right, c: lower-right, d: bottom, e: lower-left, f: upper-left, g: middle
    bool seg[7] = {};
    switch (digit) {
        case 0: seg[0]=seg[1]=seg[2]=seg[3]=seg[4]=seg[5]=true; break;
        case 1: seg[1]=seg[2]=true; break;
        case 2: seg[0]=seg[1]=seg[6]=seg[4]=seg[3]=true; break;
        case 3: seg[0]=seg[1]=seg[6]=seg[2]=seg[3]=true; break;
        case 4: seg[5]=seg[6]=seg[1]=seg[2]=true; break;
        case 5: seg[0]=seg[5]=seg[6]=seg[2]=seg[3]=true; break;
        case 6: seg[0]=seg[5]=seg[6]=seg[2]=seg[3]=seg[4]=true; break;
        case 7: seg[0]=seg[1]=seg[2]=true; break;
        case 8: seg[0]=seg[1]=seg[2]=seg[3]=seg[4]=seg[5]=seg[6]=true; break;
        case 9: seg[0]=seg[1]=seg[2]=seg[3]=seg[5]=seg[6]=true; break;
        default: break;
    }
    int t = std::max(1, s/2);
    int W = 3*s, H = 5*s;

    // a
    if (seg[0]) draw_seg(bgr0,w,h, x+t, y, W-2*t, t, t, b,g,r);
    // d
    if (seg[3]) draw_seg(bgr0,w,h, x+t, y+H-t, W-2*t, t, t, b,g,r);
    // g
    if (seg[6]) draw_seg(bgr0,w,h, x+t, y+(H/2)-(t/2), W-2*t, t, t, b,g,r);
    // f
    if (seg[5]) draw_seg(bgr0,w,h, x, y+t, t, (H/2)-t, t, b,g,r);
    // e
    if (seg[4]) draw_seg(bgr0,w,h, x, y+(H/2), t, (H/2)-t, t, b,g,r);
    // b
    if (seg[1]) draw_seg(bgr0,w,h, x+W-t, y+t, t, (H/2)-t, t, b,g,r);
    // c
    if (seg[2]) draw_seg(bgr0,w,h, x+W-t, y+(H/2), t, (H/2)-t, t, b,g,r);
}

static void draw_button_index_overlay(std::vector<uint8_t>& bgr0, int w, int h, ULONG btn, ULONG avail) {
    if (bgr0.empty() || w<=0 || h<=0) return;
    // Draw a black box top-left and render "btn" as 3 digits.
    int s = std::max(3, std::min(w, h) / 90); // scales with resolution
    int pad = s;
    int boxW = pad*2 + (3*s+pad)*3; // up to 3 digits
    int boxH = pad*2 + 5*s;
    int x0 = 8, y0 = 8;
    fill_rect_bgr0(bgr0, w, h, x0, y0, x0+boxW, y0+boxH, 0,0,0);

    // Use bright yellow digits.
    int v = (int)btn;
    if (v < 0) v = 0;
    if (v > 999) v = 999;
    int d2 = (v/100)%10;
    int d1 = (v/10)%10;
    int d0 = v%10;

    int x = x0 + pad;
    int y = y0 + pad;
    draw_digit7(bgr0, w, h, x, y, s, d2, 0,255,255);
    x += 3*s + pad;
    draw_digit7(bgr0, w, h, x, y, s, d1, 0,255,255);
    x += 3*s + pad;
    draw_digit7(bgr0, w, h, x, y, s, d0, 0,255,255);

    // Small availability marker on the right (a vertical bar that scales with avail).
    int barX0 = x0 + boxW - pad;
    int barY0 = y0 + pad;
    int barY1 = y0 + boxH - pad;
    int barH = barY1 - barY0;
    int filled = (avail > 0) ? std::min(barH, (int)(barH * std::min<ULONG>(avail, 16) / 16)) : 0;
    fill_rect_bgr0(bgr0, w, h, barX0, barY1-filled, barX0 + std::max(1,s/2), barY1, 0,255,0);
}

struct OverlayButtonCache {
    DWORD tick = 0;
    ULONG buttonsAvail = 0;
    ULONG btn = 0;
    RECT rc{};
    bool hasRc = false;
};

static void overlay_selected_button(DvdState& st, std::vector<uint8_t>& outBgr0, int outW, int outH) {
    if (!st.dvdinfo) return;

    static OverlayButtonCache c;
    const DWORD nowTick = GetTickCount();
    // Query DVD button state at ~30Hz to reduce COM overhead/jitter in the capture loop.
    if (c.tick == 0 || (DWORD)(nowTick - c.tick) >= 33) {
        c.tick = nowTick;
        c.buttonsAvail = 0;
        c.btn = 0;
        c.hasRc = false;
        HRESULT hrBtnQ = st.dvdinfo->GetCurrentButton(&c.buttonsAvail, &c.btn);
        if (SUCCEEDED(hrBtnQ) && c.buttonsAvail > 0 && c.btn > 0) {
            RECT rcq{};
            if (SUCCEEDED(st.dvdinfo->GetButtonRect(c.btn, &rcq))) {
                c.rc = rcq;
                c.hasRc = true;
            }
        }
    }

    ULONG buttonsAvail = c.buttonsAvail;
    ULONG btn = c.btn;
    if (buttonsAvail == 0 || btn == 0) return;

    if (g_debug_overlay) {
        draw_button_index_overlay(outBgr0, outW, outH, btn, buttonsAvail);
    }

    RECT rc{};
    if (!c.hasRc) {
        static bool s_logged = false;
        if (!s_logged) {
            s_logged = true;
            logw("GetButtonRect failed (disc may rely on SPU highlight). Using only DVD commands.");
        }
        return;
    }
    rc = c.rc;

    static ULONG s_prevBtn = 0;
    if (btn != s_prevBtn) {
        s_prevBtn = btn;
        if (g_verbose) {
            logi("selected button: %lu / avail=%lu rect=(%ld,%ld)-(%ld,%ld)",
                 (unsigned long)btn, (unsigned long)buttonsAvail,
                 (long)rc.left, (long)rc.top, (long)rc.right, (long)rc.bottom);
        }
    }

    if (rc.right < rc.left || rc.bottom < rc.top) return;

    int baseW = 720;
    int baseH = (st.nativeH >= 560) ? 576 : 480;
    if (st.nativeW > 0 && st.nativeH > 0 && rc.right <= st.nativeW && rc.bottom <= st.nativeH && rc.left >= 0 && rc.top >= 0) {
        baseW = st.nativeW;
        baseH = st.nativeH;
    }

if (!(outW==baseW && outH==baseH)) {
    int activeW = outW, activeH = outH, offX = 0, offY = 0;
    int darX = (int)g_curVideoAspectX.load(std::memory_order_relaxed);
    int darY = (int)g_curVideoAspectY.load(std::memory_order_relaxed);
    if (darX > 0 && darY > 0) {
        double dar = (double)darX / (double)darY;
        int wantW = (int)((double)outH * dar + 0.5);
        int wantH = outH;
        if (wantW > outW) {
            wantW = outW;
            wantH = (int)((double)outW / dar + 0.5);
        }
        if (wantW > 0 && wantH > 0) {
            activeW = wantW;
            activeH = wantH;
            offX = (outW - activeW) / 2;
            offY = (outH - activeH) / 2;
        }
    }

    rc.left   = (LONG)(offX + (long long)rc.left   * activeW / baseW);
    rc.right  = (LONG)(offX + (long long)rc.right  * activeW / baseW);
    rc.top    = (LONG)(offY + (long long)rc.top    * activeH / baseH);
    rc.bottom = (LONG)(offY + (long long)rc.bottom * activeH / baseH);
}

    draw_rect_outline_bgr0(outBgr0, outW, outH, rc, 4);
}

// ---------------- path helpers (mpv portable config) ----------------
static bool file_exists_w(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
static bool dir_exists_w(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
static std::wstring dir_of_path(const std::wstring& p) {
    size_t pos = p.find_last_of(L"\\/");
    if (pos == std::wstring::npos) return L"";
    return p.substr(0, pos);
}
static std::wstring join_path2(const std::wstring& a, const std::wstring& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    wchar_t last = a.back();
    if (last == L'\\' || last == L'/') return a + b;
    return a + L"\\" + b;
}
static std::wstring fullpath_w(const std::wstring& p) {
    if (p.empty()) return p;
    DWORD need = GetFullPathNameW(p.c_str(), 0, nullptr, nullptr);
    if (!need) return p;
    std::wstring out;
    out.resize(need + 4);
    DWORD got = GetFullPathNameW(p.c_str(), (DWORD)out.size(), out.data(), nullptr);
    if (!got) return p;
    out.resize(got);
    return out;
}
static std::wstring resolve_exe_path(const std::wstring& exe) {
    if (exe.empty()) return exe;
    // If exe already looks like a path, just fullpath it.
    if (exe.find(L'\\') != std::wstring::npos || exe.find(L'/') != std::wstring::npos || (exe.size() >= 2 && exe[1] == L':')) {
        std::wstring fp = fullpath_w(exe);
        return fp;
    }
    // Search PATH / current dir.
    wchar_t buf[MAX_PATH * 4]{};
    DWORD got = SearchPathW(nullptr, exe.c_str(), nullptr, (DWORD)(sizeof(buf)/sizeof(buf[0])), buf, nullptr);
    if (got && got < (DWORD)(sizeof(buf)/sizeof(buf[0]))) {
        return std::wstring(buf);
    }
    return exe;
}

// ---------------- mpv return-to-menu script injection ----------------
static std::string w2utf8(const std::wstring& ws) {
    if (ws.empty()) return std::string();
    int need = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), (int)ws.size(), nullptr, 0, nullptr, nullptr);
    if (need <= 0) return std::string();
    std::string out;
    out.resize((size_t)need);
    int got = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), (int)ws.size(), out.data(), need, nullptr, nullptr);
    if (got <= 0) return std::string();
    return out;
}

static std::wstring get_module_path() {
    wchar_t buf[MAX_PATH * 4]{};
    DWORD n = GetModuleFileNameW(nullptr, buf, (DWORD)(sizeof(buf) / sizeof(buf[0])));
    if (!n || n >= (DWORD)(sizeof(buf) / sizeof(buf[0]))) return L"";
    return std::wstring(buf, buf + n);
}

static bool ensure_dir_recursive(const std::wstring& dir) {
    if (dir.empty()) return false;
    if (dir_exists_w(dir)) return true;

    std::wstring cur;
    cur.reserve(dir.size());
    for (size_t i = 0; i < dir.size(); ++i) {
        wchar_t c = dir[i];
        cur.push_back(c);
        if (c == L'\\' || c == L'/') {
            if (cur.size() == 3 && cur[1] == L':' && (cur[2] == L'\\' || cur[2] == L'/')) continue;
            CreateDirectoryW(cur.c_str(), nullptr);
        }
    }
    if (!CreateDirectoryW(dir.c_str(), nullptr)) {
        DWORD e = gle();
        if (e != ERROR_ALREADY_EXISTS) return false;
    }
    return dir_exists_w(dir);
}

static bool write_text_utf8(const std::wstring& pathW, const std::string& text) {
    HANDLE h = CreateFileW(pathW.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    BOOL ok = WriteFile(h, text.data(), (DWORD)text.size(), &written, nullptr);
    CloseHandle(h);
    return ok && written == (DWORD)text.size();
}

// Ensure a tiny Lua script exists that binds Ctrl+F1/F2 in mpv to re-launch dvdmenu.exe.
// Returns an mpv argument string like:  L" --script=\"...lua\""
static std::wstring ensure_mpv_return_script_arg(const std::wstring& mpvDir, const std::wstring& dvdDeviceRoot) {
    std::wstring scriptsDir;
    std::wstring pc = join_path2(mpvDir, L"portable_config");
    if (!pc.empty() && dir_exists_w(pc)) {
        scriptsDir = join_path2(pc, L"scripts");
    } else {
        scriptsDir = mpvDir;
    }
    if (scriptsDir.empty()) return L"";

    if (!dir_exists_w(scriptsDir)) {
        if (!ensure_dir_recursive(scriptsDir)) return L"";
    }

    std::wstring luaPath = join_path2(scriptsDir, L"dvdmenu_return.lua");

    std::wstring selfExe = get_module_path();
    if (selfExe.empty()) selfExe = L"dvdmenu.exe";

    const std::string exe8 = w2utf8(selfExe);
    const std::string dvd8 = w2utf8(dvdDeviceRoot);

    std::string lua;
    lua.reserve(4096 + exe8.size() + dvd8.size());
    lua += "local mp = require 'mp'\n";
    lua += "local exe = [["; lua += (exe8.empty() ? "dvdmenu.exe" : exe8); lua += "]]\n";
    lua += "local startup_delay = '2200'\n";
    lua += "local triggered = false\n";
    lua += "local pending_menu = nil\n";
    lua += "local pending_dvd = nil\n";
    lua += "local function log(s) mp.msg.info('[dvdmenu_return] ' .. tostring(s)) end\n";
    lua += "local function normalize_root(path)\n";
    lua += "  if type(path) ~= 'string' then return '' end\n";
    lua += "  local m = path:match('([A-Za-z]:)')\n";
    lua += "  if not m then return '' end\n";
    lua += "  return m .. '\\\\\\\\'\n";
    lua += "end\n";
    lua += "local function guess_dvd()\n";
    lua += "  local d = mp.get_property('options/dvd-device') or ''\n";
    lua += "  local nd = normalize_root(d)\n";
    lua += "  if nd ~= '' then if d ~= nd then log('normalized options/dvd-device: ' .. d .. ' -> ' .. nd) end return nd end\n";
    lua += "  local p = mp.get_property('path') or ''\n";
    lua += "  local m = p:match('^dvd://([A-Za-z]:)')\n";
    lua += "  if m then return m .. '\\\\\\\\' end\n";
    lua += "  local fn = mp.get_property('stream-open-filename') or ''\n";
    lua += "  local nf = normalize_root(fn)\n";
    lua += "  if nf ~= '' then return nf end\n";
    lua += "  return ''\n";
    lua += "end\n";
    lua += "local function spawn(menu, dvdroot)\n";
    lua += "  local args = { exe, '--startup-delay-ms', startup_delay, '--fps', 'auto' }\n";
    lua += "  if dvdroot and dvdroot ~= '' then args[#args+1] = '--dvd-device'; args[#args+1] = dvdroot end\n";
    lua += "  args[#args+1] = '--start-menu'; args[#args+1] = menu\n";
    lua += "  log('spawn: menu=' .. tostring(menu) .. ' dvd=' .. tostring((dvdroot and dvdroot ~= '') and dvdroot or '(auto)'))\n";
    lua += "  local ok, res = pcall(mp.command_native, {name='subprocess', detach=true, playback_only=false, capture_stdout=false, capture_stderr=false, args=args})\n";
    lua += "  if not ok then log('subprocess failed: ' .. tostring(res)) end\n";
    lua += "end\n";
    lua += "local function begin(menu)\n";
    lua += "  if triggered then log('ignored duplicate trigger'); return end\n";
    lua += "  triggered = true\n";
    lua += "  pending_menu = (menu == 'root') and 'root' or 'title'\n";
    lua += "  pending_dvd = guess_dvd()\n";
    lua += "  log('begin: menu=' .. pending_menu .. ' dvd=' .. ((pending_dvd ~= '') and pending_dvd or '(auto)'))\n";
    lua += "  mp.commandv('quit')\n";
    lua += "end\n";
    lua += "mp.register_event('shutdown', function() if pending_menu then spawn(pending_menu, pending_dvd or '') end end)\n";
    lua += "mp.register_script_message('go', function(menu) begin(menu) end)\n";
    lua += "mp.add_forced_key_binding('Ctrl+F1','dvdmenu_title', function() begin('title') end)\n";
    lua += "mp.add_forced_key_binding('Ctrl+F2','dvdmenu_root', function() begin('root') end)\n";
    if (!write_text_utf8(luaPath, lua)) return L"";
    // mpv expects: --script="path".  Keep quoting for paths with spaces.
    return L" --script=\"" + luaPath + L"\"";
}


static void cleanup_stale_preview_guard_scripts(const std::wstring& mpvDir) {
    if (mpvDir.empty()) return;
    std::wstring pc = join_path2(mpvDir, L"portable_config");
    std::wstring scriptsDir;
    if (!pc.empty() && dir_exists_w(pc)) {
        scriptsDir = join_path2(pc, L"scripts");
    } else {
        scriptsDir = mpvDir;
    }
    if (scriptsDir.empty()) return;
    std::wstring luaPath = join_path2(scriptsDir, L"dvdmenu_preview_guard.lua");
    DWORD attr = GetFileAttributesW(luaPath.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
        if (DeleteFileW(luaPath.c_str())) {
            logi("Removed stale preview-guard script: %ls", luaPath.c_str());
        } else {
            logw("Failed to remove stale preview-guard script: %ls (GLE=%lu)", luaPath.c_str(), (unsigned long)gle());
        }
    }
}

// ---------------- mpv launch (stdin pipe) ----------------
struct MpvStdin {
    HANDLE hWrite = INVALID_HANDLE_VALUE;
    HANDLE hProcess = nullptr;
    std::wstring ipcPipeName;
    bool ipcReadySeen = false;
    ~MpvStdin() {
        if (hWrite != INVALID_HANDLE_VALUE) CloseHandle(hWrite);
        if (hProcess) CloseHandle(hProcess);
    }
};

static HWND g_mpvHwnd = nullptr;
static DWORD g_mpvPid = 0;
static std::atomic<bool> g_requestQuit{false};

static std::wstring make_preview_ipc_pipe_name() {
    wchar_t buf[256]{};
    swprintf(buf, 256, L"\\\\.\\pipe\\dvdmenu_preview_%lu_%lu", (unsigned long)GetCurrentProcessId(), (unsigned long)GetTickCount());
    return buf;
}

static bool mpv_ipc_get_property_string(const std::wstring& pipeName, const char* prop, std::string& out) {
    out.clear();
    if (pipeName.empty()) return false;
    if (!WaitNamedPipeW(pipeName.c_str(), 50)) return false;
    HANDLE h = CreateFileW(pipeName.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD mode = PIPE_READMODE_BYTE;
    SetNamedPipeHandleState(h, &mode, nullptr, nullptr);

    char cmd[256];
    snprintf(cmd, sizeof(cmd), "{\"command\":[\"get_property\",\"%s\"]}\n", prop);
    DWORD written = 0;
    if (!WriteFile(h, cmd, (DWORD)strlen(cmd), &written, nullptr) || written == 0) {
        CloseHandle(h);
        return false;
    }

    char buf[512];
    DWORD got = 0;
    std::string resp;
    ULONGLONG deadline = GetTickCount64() + 120;
    while (GetTickCount64() < deadline) {
        if (ReadFile(h, buf, sizeof(buf) - 1, &got, nullptr) && got > 0) {
            buf[got] = 0;
            resp.append(buf, buf + got);
            if (resp.find('\n') != std::string::npos) break;
        } else {
            Sleep(5);
        }
    }
    CloseHandle(h);
    if (resp.empty()) return false;

    const std::string key = "\"data\":";
    size_t pos = resp.find(key);
    if (pos == std::string::npos) return false;
    pos += key.size();
    while (pos < resp.size() && resp[pos] == ' ') ++pos;
    if (pos >= resp.size()) return false;

    if (resp[pos] == '"') {
        ++pos;
        std::string val;
        while (pos < resp.size()) {
            char c = resp[pos++];
            if (c == '\\' && pos < resp.size()) {
                char n = resp[pos++];
                if (n == '"' || n == '\\' || n == '/') val.push_back(n);
                else if (n == 'n') val.push_back('\n');
                else if (n == 'r') val.push_back('\r');
                else if (n == 't') val.push_back('\t');
                else val.push_back(n);
            } else if (c == '"') {
                out = val;
                return true;
            } else {
                val.push_back(c);
            }
        }
        return false;
    }
    if (resp.compare(pos, 4, "null") == 0) {
        out.clear();
        return true;
    }
    return false;
}

static bool launch_mpv_stdin(MpvStdin& mpv, const std::wstring& mpvExe, int w, int h, double fps, const std::wstring& mpvLog) {
    // Resolve mpv.exe so we can set a stable working directory (important for portable_config / input.conf).
    std::wstring mpvExeAbs = resolve_exe_path(mpvExe);
    std::wstring mpvDir = dir_of_path(mpvExeAbs);
    cleanup_stale_preview_guard_scripts(mpvDir);
    g_mpvHwnd = nullptr;
    g_mpvPid = 0;
    if (!mpvDir.empty()) {
        // If mpv is portable (portable_config next to mpv.exe), point mpv at it explicitly.
        // This makes mpv.conf / input.conf work even when dvdmenu.exe is run from another directory.
    }

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE hRead = INVALID_HANDLE_VALUE;
    HANDLE hWrite = INVALID_HANDLE_VALUE;
    if (!CreatePipe(&hRead, &hWrite, &sa, 1 << 20)) {
        loge("CreatePipe failed (GLE=%lu)", (unsigned long)gle());
        return false;
    }
    // Parent must not inherit write end; child must inherit read end
    SetHandleInformation(hWrite, HANDLE_FLAG_INHERIT, 0);

    // mpv rawvideo from stdin is sensitive to demuxer queue limits.
    // Avoid low-latency tweaks like demuxer-max-bytes=0 which can make mpv instantly hit EOF.
    if (!(fps > 0.0)) {
        // Auto fps guess: NTSC DVDs are typically 29.970, PAL DVDs 25.000.
        fps = (h >= 570) ? 25.000 : 29.970;
    }

    wchar_t cmd[8192];

    std::wstring mpvLog2;
    if (!mpvLog.empty()) mpvLog2 = adjust_path_if_directory(mpvLog, L"mpv.log");

    mpv.ipcPipeName = make_preview_ipc_pipe_name();
    std::wstring ipcArg = L" --input-ipc-server=\"" + mpv.ipcPipeName + L"\"";

    std::wstring configDirArg;
    if (!mpvDir.empty()) {
        std::wstring pc = join_path2(mpvDir, L"portable_config");
        if (dir_exists_w(pc)) {
            configDirArg = L" --config-dir=\"" + pc + L"\"";
        }
    }
if (!mpvLog2.empty()) {
        swprintf(cmd, 8192,
                 L"\"%s\"%ls%ls --force-window=yes --keep-open=yes --idle=yes --no-audio --cache=no --demuxer-cache-wait=no --cache-pause=no --cache-secs=0 --demuxer-max-bytes=4MiB --demuxer-max-back-bytes=0 "
                 L"--input-terminal=no "
                 L"--title=dvdmenu "
                 L"--demuxer=rawvideo --demuxer-rawvideo-w=%d --demuxer-rawvideo-h=%d "
                 L"--video-sync=display-desync --interpolation=no --demuxer-rawvideo-mp-format=yuv444p --demuxer-rawvideo-fps=%.3f "
                 L"--log-file=\"%s\" -",
                 mpvExeAbs.c_str(), configDirArg.c_str(), ipcArg.c_str(), w, h, fps, mpvLog2.c_str());
    } else {
        swprintf(cmd, 8192,
                 L"\"%s\"%ls%ls --force-window=yes --keep-open=yes --idle=yes --no-audio --cache=no --demuxer-cache-wait=no --cache-pause=no --cache-secs=0 --demuxer-max-bytes=4MiB --demuxer-max-back-bytes=0 "
                 L"--input-terminal=no "
                 L"--title=dvdmenu "
                 L"--demuxer=rawvideo --demuxer-rawvideo-w=%d --demuxer-rawvideo-h=%d "
                 L"--video-sync=display-desync --interpolation=no --demuxer-rawvideo-mp-format=yuv444p --demuxer-rawvideo-fps=%.3f -",
                 mpvExeAbs.c_str(), configDirArg.c_str(), ipcArg.c_str(), w, h, fps);
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = hRead;
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError  = GetStdHandle(STD_ERROR_HANDLE);

    PROCESS_INFORMATION pi{};
    std::wstring cmdline = cmd;
    BOOL ok = CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr, TRUE,
                             0, nullptr, (mpvDir.empty() ? nullptr : mpvDir.c_str()), &si, &pi);
    CloseHandle(hRead);
    if (g_verbose) {
        logi("Launch mpv dvd handoff cmd: %ls", cmdline.c_str());
    }

    if (!ok) {
        CloseHandle(hWrite);
        loge("Failed to launch mpv (GLE=%lu). Cmd: %ls", (unsigned long)gle(), cmdline.c_str());
        return false;
    }

    CloseHandle(pi.hThread);
    mpv.hWrite = hWrite;
    mpv.hProcess = pi.hProcess;
    g_mpvPid = pi.dwProcessId;
    g_requestQuit.store(false, std::memory_order_relaxed);
    logi("Launched mpv (stdin): %ls", cmdline.c_str());
    return true;
}

// Launch mpv to play the DVD directly (dvd://) using mpv's normal disc path.
// This is used when the disc has no DVD menus (ShowMenu returns MENU_DOES_NOT_EXIST),
// or when the user explicitly wants to skip this tool's rawvideo pipeline.
static bool launch_mpv_dvd_direct(const std::wstring& mpvExe,
                                 const std::wstring& dvdDeviceRoot,
                                 bool startAtMenu,
                                 const std::wstring& mpvLog) {
    std::wstring mpvExeAbs = resolve_exe_path(mpvExe);
    std::wstring mpvDir = dir_of_path(mpvExeAbs);

    std::wstring configDirArg;
    if (!mpvDir.empty()) {
        std::wstring pc = join_path2(mpvDir, L"portable_config");
        if (dir_exists_w(pc)) {
            configDirArg = L" --config-dir=\"" + pc + L"\"";
        }
    }

        std::wstring scriptArg = ensure_mpv_return_script_arg(mpvDir, dvdDeviceRoot);

    std::wstring mpvLog2 = mpvLog;
    wchar_t cmd[8192];

    // Build mpv URL list. For menu handoff we still use dvd://menu.
    // Otherwise, if we know the title count, queue dvd://0 .. dvd://N-1
    // so mpv's internal playlist has all titles. If we don't know the
    // count yet, fall back to plain dvd:// (libdvdnav picks main title).
    std::wstring urlList;
    if (startAtMenu) {
        urlList = L"dvd://menu";
    } else if (g_lastKnownDvdTitleCount > 0) {
        for (ULONG i = 0; i < g_lastKnownDvdTitleCount; ++i) {
            wchar_t buf[32];
            _snwprintf(buf, 32, L" dvd://%lu", (unsigned long)i);
            urlList += buf;
        }
    } else {
        urlList = L"dvd://";
    }

    const wchar_t* url = urlList.c_str();

    if (!mpvLog2.empty()) {
        swprintf(cmd, 8192,
                 L"\"%s\"%s --player-operation-mode=pseudo-gui --force-window=yes --mute=no --dvd-device=%s%s --log-file=\"%s\" %s",
                 mpvExeAbs.c_str(), configDirArg.c_str(), dvdDeviceRoot.c_str(), scriptArg.c_str(), mpvLog2.c_str(), url);
    } else {
        swprintf(cmd, 8192,
                 L"\"%s\"%s --player-operation-mode=pseudo-gui --force-window=yes --mute=no --dvd-device=%s%s %s",
                 mpvExeAbs.c_str(), configDirArg.c_str(), dvdDeviceRoot.c_str(), scriptArg.c_str(), url);
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::wstring cmdline = cmd;

    // Use mpv dir as working directory so portable_config works.
    wchar_t* mutableCmd = _wcsdup(cmdline.c_str());
    BOOL ok = CreateProcessW(nullptr, mutableCmd, nullptr, nullptr, FALSE,
                            0, nullptr, mpvDir.empty() ? nullptr : mpvDir.c_str(),
                            &si, &pi);
    free(mutableCmd);

    if (!ok) {
        loge("CreateProcessW(mpv dvd://) failed (GLE=%lu)", (unsigned long)gle());
        return false;
    }

    
    DWORD mpvPid = pi.dwProcessId;
    if (g_handoffWatchdogSec > 0 && mpvPid) {
        DWORD selfPid = GetCurrentProcessId();
        if (spawn_watchdog_child(mpvPid, selfPid, g_handoffWatchdogSec)) {
            logi("Armed handoff watchdog: mpv pid=%lu, dvdmenu pid=%lu, timeout=%lu s",
                 (unsigned long)mpvPid, (unsigned long)selfPid, (unsigned long)g_handoffWatchdogSec);
        } else {
            logw("Failed to arm handoff watchdog (GLE=%lu).", (unsigned long)gle());
        }
    }

CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    logi("Handed off to mpv direct DVD playback: %ls", url);
    return true;
}
// Launch mpv to play a specific DVD title/chapter (dvd://<title> + --start='#<chapter>').
// Used for menu handoff: this tool handles dvdnav menus, then mpv handles normal playback.
static bool launch_mpv_dvd_title(const std::wstring& mpvExe,
                                const std::wstring& dvdDeviceRoot,
                                int titleNum,
                                int chapterNum,
                                const std::wstring& mpvLog) {
    std::wstring mpvExeAbs = resolve_exe_path(mpvExe);
    std::wstring mpvDir = dir_of_path(mpvExeAbs);

    std::wstring configDirArg;
    if (!mpvDir.empty()) {
        std::wstring pc = join_path2(mpvDir, L"portable_config");
        if (dir_exists_w(pc)) {
            configDirArg = L" --config-dir=\"" + pc + L"\"";
        }
    }

    // mpv URL syntax: dvd://[title] (title is optional; if omitted, mpv selects the longest title).
    wchar_t url[64]{};
    if (titleNum > 0) swprintf(url, 64, L"dvd://%d", titleNum);
    else swprintf(url, 64, L"dvd://");

    // Chapters in mpv seek syntax are 1-based: --start='#2' starts at chapter 2. (See mpv manpage.)
    std::wstring startArg;
    if (chapterNum > 1) {
        wchar_t tmp[64];
        swprintf(tmp, 64, L" --start=\"#%d\"", chapterNum);
        startArg = tmp;
    }

    std::wstring scriptArg = ensure_mpv_return_script_arg(mpvDir, dvdDeviceRoot);

    wchar_t cmd[8192];
    if (!mpvLog.empty()) {
        swprintf(cmd, 8192,
                 L"\"%s\"%s --player-operation-mode=pseudo-gui --force-window=yes --mute=no --dvd-device=%s%s --log-file=\"%s\"%s %s",
                 mpvExeAbs.c_str(), configDirArg.c_str(), dvdDeviceRoot.c_str(), scriptArg.c_str(), mpvLog.c_str(), startArg.c_str(), url);
    } else {
        swprintf(cmd, 8192,
                 L"\"%s\"%s --player-operation-mode=pseudo-gui --force-window=yes --mute=no --dvd-device=%s%s%s %s",
                 mpvExeAbs.c_str(), configDirArg.c_str(), dvdDeviceRoot.c_str(), scriptArg.c_str(), startArg.c_str(), url);
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::wstring cmdline = cmd;

    wchar_t* mutableCmd = _wcsdup(cmdline.c_str());
    BOOL ok = CreateProcessW(nullptr, mutableCmd, nullptr, nullptr, FALSE,
                             0, nullptr, mpvDir.empty() ? nullptr : mpvDir.c_str(),
                             &si, &pi);
    free(mutableCmd);

    if (!ok) {
        loge("CreateProcessW(mpv dvd://title) failed (GLE=%lu)", (unsigned long)gle());
        return false;
    }

    
    DWORD mpvPid = pi.dwProcessId;
    if (g_handoffWatchdogSec > 0 && mpvPid) {
        DWORD selfPid = GetCurrentProcessId();
        if (spawn_watchdog_child(mpvPid, selfPid, g_handoffWatchdogSec)) {
            logi("Armed handoff watchdog: mpv pid=%lu, dvdmenu pid=%lu, timeout=%lu s",
                 (unsigned long)mpvPid, (unsigned long)selfPid, (unsigned long)g_handoffWatchdogSec);
        } else {
            logw("Failed to arm handoff watchdog (GLE=%lu).", (unsigned long)gle());
        }
    }

CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    logi("Handed off to mpv DVD playback: %ls%s", url, (chapterNum > 1 ? L" (with chapter start)" : L""));
    return true;
}




static bool write_all(HANDLE h, const uint8_t* data, size_t sz) {
    size_t left = sz;
    const uint8_t* p = data;
    while (left) {
        DWORD w = 0;
        DWORD chunk = (left > (1u << 20)) ? (1u << 20) : (DWORD)left;
        if (!WriteFile(h, p, chunk, &w, nullptr)) return false;
        if (w == 0) return false;
        p += w;
        left -= w;
    }
    return true;
}

// ---------------- DVD commands (optional future) ----------------
using DvdMenuId = decltype(DVD_MENU_Root);
static HRESULT dvd_ShowMenu(IDvdControl2* c, DvdMenuId m, DVD_CMD_FLAGS f);


// ---------------- shutdown coordination ----------------

// ---------------- DVD command hang watchdog ----------------
// If a DVD Navigator COM call deadlocks, mpv stops receiving frames and appears frozen.
// We can't reliably cancel the COM call, so we detect "in-flight too long" and terminate THIS process.
// This is preferable to an indefinite OS-wide input stall and orphaned dvdmenu.exe.
static std::atomic<bool>   g_dvdCmdInFlight{false};
static std::atomic<ULONGLONG> g_dvdCmdStartTick{0};
static DWORD g_dvdCmdHangKillMs = 1500; // default; can be tuned if needed

static void start_dvd_cmd_watchdog_thread() {
    static std::atomic<bool> started{false};
    bool expected = false;
    if (!started.compare_exchange_strong(expected, true)) return;
    std::thread([](){
        for (;;) {
            if (g_requestQuit.load(std::memory_order_relaxed)) return;
            if (g_dvdCmdInFlight.load(std::memory_order_acquire)) {
                ULONGLONG st = g_dvdCmdStartTick.load(std::memory_order_relaxed);
                ULONGLONG now = GetTickCount64();
                if (st != 0 && (now - st) > (ULONGLONG)g_dvdCmdHangKillMs) {
                    // Last resort: exit hard to avoid orphaned processes.
                    // (Logging may not flush if the process is wedged.)
                    ExitProcess(3);
                }
            }
            Sleep(50);
        }
    }).detach();
}
;

static BOOL WINAPI ConsoleCtrlHandler(DWORD type) {
    switch (type) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
        case CTRL_LOGOFF_EVENT:
        case CTRL_SHUTDOWN_EVENT:
            g_requestQuit.store(true, std::memory_order_relaxed);
            return TRUE;
        default:
            return FALSE;
    }
}
// ---------------- menu input hook (keyboard/mouse -> DVD Navigator) ----------------
// Rationale: When the DVD menu is shown inside mpv (rawvideo), input focus is on the mpv window.
// DirectShow's DVD Navigator won't receive those keys automatically, so we forward them.
// We ONLY swallow keys while the DVD is in a menu domain (VMGM/VTSMenu) and the mpv window is foreground.

static std::atomic<bool> g_inMenu{false};
static std::atomic<unsigned long> g_buttonsAvail{0};
static std::atomic<unsigned long> g_curButton{0};
static std::atomic<DWORD> g_allowKeysUntil{0}; // GetTickCount deadline

// Current DVD playback state (for mpv handoff)
static std::atomic<long> g_curDomain{ (long)DVD_DOMAIN_FirstPlay };
std::atomic<long> g_curVideoAspectX{ 0 };
std::atomic<long> g_curVideoAspectY{ 0 };
static std::atomic<long> g_curVideoFrameH{ 0 };
static std::atomic<unsigned long> g_curTitleNum{0};
static std::atomic<unsigned long> g_curChapterNum{0};
static std::atomic<DWORD> g_lastMenuSeenTick{0}; // last time we observed a menu domain or buttons

static std::atomic<bool> g_vkDown[256]{}; // track physical key down
static std::atomic<DWORD> g_vkLastTick[256]{}; // rate-limit repeats while held
static DvdState* g_st = nullptr;
static HHOOK g_kbdHook = nullptr;
static HHOOK g_mouseHook = nullptr;

static void start_menu(DvdState& st, const std::wstring& which) {
    if (!st.dvdctl) return;
    if (_wcsicmp(which.c_str(), L"root") == 0) {
        HRESULT hr = dvd_ShowMenu(st.dvdctl, DVD_MENU_Root, DVD_CMD_FLAG_None);
        g_allowKeysUntil.store(GetTickCount() + 5000, std::memory_order_relaxed);
        logi("ShowMenu(root) -> 0x%08lx", (unsigned long)hr);
    } else if (_wcsicmp(which.c_str(), L"title") == 0) {
        HRESULT hr = dvd_ShowMenu(st.dvdctl, DVD_MENU_Title, DVD_CMD_FLAG_None);
        g_allowKeysUntil.store(GetTickCount() + 5000, std::memory_order_relaxed);
        logi("ShowMenu(title) -> 0x%08lx", (unsigned long)hr);
    }
}
// Action queue: hooks only enqueue, main loop executes DVD COM calls.
// IMPORTANT: Low-level hooks must NEVER block or allocate; otherwise the entire OS input stack can stall.
// Use a fixed-size lock-free ring buffer. If full, we drop the newest action.
enum class MenuActionType : int { Up, Down, Left, Right, Activate, Back, Root, Title, MouseSelect, MouseActivate };

struct MenuAction {
    MenuActionType type;
    POINT pt{}; // for mouse actions: DVD pixel coords (0..w-1, 0..h-1)
};

struct ActionRing {
    static constexpr uint32_t kCap = 256; // power of two recommended
    std::atomic<uint32_t> w{0};
    std::atomic<uint32_t> r{0};
    MenuAction buf[kCap]{};

    bool push(const MenuAction& a) {
        uint32_t w0 = w.load(std::memory_order_relaxed);
        uint32_t r0 = r.load(std::memory_order_acquire);
        if ((uint32_t)(w0 - r0) >= kCap) return false; // full
        buf[w0 & (kCap - 1)] = a;
        w.store(w0 + 1, std::memory_order_release);
        return true;
    }
    uint32_t pop_many(MenuAction* out, uint32_t maxN) {
        uint32_t r0 = r.load(std::memory_order_relaxed);
        uint32_t w0 = w.load(std::memory_order_acquire);
        uint32_t avail = (uint32_t)(w0 - r0);
        if (avail == 0) return 0;
        uint32_t n = (avail < maxN) ? avail : maxN;
        for (uint32_t i = 0; i < n; ++i) {
            out[i] = buf[(r0 + i) & (kCap - 1)];
        }
        r.store(r0 + n, std::memory_order_release);
        return n;
    }
};

static ActionRing g_actRing;

static void enqueue_action(MenuActionType t, POINT pt = POINT{0,0}) {
    (void)g_actRing.push(MenuAction{t, pt}); // drop if full
}

static std::vector<MenuAction> take_actions() {
    MenuAction tmp[ActionRing::kCap]{};
    uint32_t n = g_actRing.pop_many(tmp, ActionRing::kCap);
    std::vector<MenuAction> out;
    out.reserve(n);
    for (uint32_t i = 0; i < n; ++i) out.push_back(tmp[i]);
    return out;
}


static void log_menu_state_snapshot(DvdState& st, const char* tag) {
    if (!g_verbose || !st.dvdinfo) return;
    DVD_DOMAIN dom{};
    DVD_PLAYBACK_LOCATION2 loc{};
    ULONG avail = 0, btn = 0;
    HRESULT hrDom = st.dvdinfo->GetCurrentDomain(&dom);
    HRESULT hrLoc = st.dvdinfo->GetCurrentLocation(&loc);
    HRESULT hrBtn = st.dvdinfo->GetCurrentButton(&avail, &btn);
    logi("menu-state[%s]: domHr=0x%08lx dom=%ld locHr=0x%08lx T=%lu C=%lu btnHr=0x%08lx avail=%lu btn=%lu",
         tag, (unsigned long)hrDom, SUCCEEDED(hrDom) ? (long)dom : -1L,
         (unsigned long)hrLoc, SUCCEEDED(hrLoc) ? (unsigned long)loc.TitleNum : 0UL,
         SUCCEEDED(hrLoc) ? (unsigned long)loc.ChapterNum : 0UL,
         (unsigned long)hrBtn, SUCCEEDED(hrBtn) ? (unsigned long)avail : 0UL,
         SUCCEEDED(hrBtn) ? (unsigned long)btn : 0UL);
}

struct WinPick {
    HWND hwnd = nullptr;
    int score = -1;
};
struct WinPickCtx {
    DWORD pid = 0;
    WinPick best;
};

static BOOL CALLBACK enum_windows_pick_best(HWND hwnd, LPARAM lParam) {
    WinPickCtx* ctx = reinterpret_cast<WinPickCtx*>(lParam);

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != ctx->pid) return TRUE;

    wchar_t cls[128]{};
    GetClassNameW(hwnd, cls, 128);

    bool visible = IsWindowVisible(hwnd) != FALSE;
    bool owned   = GetWindow(hwnd, GW_OWNER) != nullptr;

    // mpv window class is usually "mpv" (but don't rely on it too hard)
    bool isMpvClass = (_wcsicmp(cls, L"mpv") == 0) || (wcsstr(cls, L"mpv") != nullptr);

    // Prefer: mpv-class > visible > not-owned
    int score = (isMpvClass ? 4 : 0) + (visible ? 2 : 0) + (!owned ? 1 : 0);

    if (!ctx->best.hwnd || score > ctx->best.score) {
        ctx->best.hwnd = hwnd;
        ctx->best.score = score;
        // If we got the ideal candidate, we can stop enumeration early.
        if (score >= 7) return FALSE;
    }
    return TRUE;
}

static HWND find_main_window_for_pid(DWORD pid, DWORD waitMs = 5000) {
    // Always perform at least one EnumWindows pass, even when waitMs == 0.
    DWORD start = GetTickCount();

    for (;;) {
        WinPickCtx ctx;
        ctx.pid = pid;
        EnumWindows(enum_windows_pick_best, (LPARAM)&ctx);
        if (ctx.best.hwnd) return ctx.best.hwnd;

        if (waitMs == 0) break;
        if (GetTickCount() - start >= waitMs) break;
        Sleep(50);
    }
    return nullptr;
}

// --- IDvdControl2 signature adaptation (SDKs differ; avoid hardcoding signatures) ---
// Different Windows SDKs declare IDvdControl2 methods with varying parameter lists.
// We use C++17 SFINAE (overload ranking) to call whichever form exists.

using DvdRelDir = decltype(DVD_Relative_Upper);

template <class C>
static auto _dvd_selrel(C* c, DvdRelDir dir, DVD_CMD_FLAGS f, int)
    -> decltype(c->SelectRelativeButton(dir, f, (IDvdCmd**)nullptr)) {
    return c->SelectRelativeButton(dir, f, nullptr);
}
template <class C>
static auto _dvd_selrel(C* c, DvdRelDir dir, DVD_CMD_FLAGS f, long)
    -> decltype(c->SelectRelativeButton(dir, f)) {
    return c->SelectRelativeButton(dir, f);
}
template <class C>
static auto _dvd_selrel(C* c, DvdRelDir dir, DVD_CMD_FLAGS, short)
    -> decltype(c->SelectRelativeButton(dir, (IDvdCmd**)nullptr)) {
    return c->SelectRelativeButton(dir, nullptr);
}
template <class C>
static auto _dvd_selrel(C* c, DvdRelDir dir, DVD_CMD_FLAGS, ...)
    -> decltype(c->SelectRelativeButton(dir)) {
    return c->SelectRelativeButton(dir);
}

static HRESULT dvd_SelectRelative(IDvdControl2* c, DvdRelDir dir, DVD_CMD_FLAGS f) {
    if (!c) return E_POINTER;
    return _dvd_selrel(c, dir, f, 0);
}

template <class C>
static auto _dvd_act(C* c, DVD_CMD_FLAGS f, int)
    -> decltype(c->ActivateButton(f, (IDvdCmd**)nullptr)) {
    return c->ActivateButton(f, nullptr);
}
template <class C>
static auto _dvd_act(C* c, DVD_CMD_FLAGS f, long)
    -> decltype(c->ActivateButton(f)) {
    return c->ActivateButton(f);
}
template <class C>
static auto _dvd_act(C* c, DVD_CMD_FLAGS, short)
    -> decltype(c->ActivateButton((IDvdCmd**)nullptr)) {
    return c->ActivateButton(nullptr);
}
template <class C>
static auto _dvd_act(C* c, DVD_CMD_FLAGS, ...)
    -> decltype(c->ActivateButton()) {
    return c->ActivateButton();
}

static HRESULT dvd_Activate(IDvdControl2* c, DVD_CMD_FLAGS f) {
    if (!c) return E_POINTER;
    return _dvd_act(c, f, 0);
}

template <class C>
static auto _dvd_back(C* c, DVD_CMD_FLAGS f, int)
    -> decltype(c->ReturnFromSubmenu(f, (IDvdCmd**)nullptr)) {
    return c->ReturnFromSubmenu(f, nullptr);
}
template <class C>
static auto _dvd_back(C* c, DVD_CMD_FLAGS f, long)
    -> decltype(c->ReturnFromSubmenu(f)) {
    return c->ReturnFromSubmenu(f);
}
template <class C>
static auto _dvd_back(C* c, DVD_CMD_FLAGS, short)
    -> decltype(c->ReturnFromSubmenu((IDvdCmd**)nullptr)) {
    return c->ReturnFromSubmenu(nullptr);
}
template <class C>
static auto _dvd_back(C* c, DVD_CMD_FLAGS, ...)
    -> decltype(c->ReturnFromSubmenu()) {
    return c->ReturnFromSubmenu();
}

static HRESULT dvd_ReturnFromSubmenu(IDvdControl2* c, DVD_CMD_FLAGS f) {
    if (!c) return E_POINTER;
    return _dvd_back(c, f, 0);
}

template <class C>
static auto _dvd_show(C* c, DvdMenuId m, DVD_CMD_FLAGS f, int)
    -> decltype(c->ShowMenu(m, f, (IDvdCmd**)nullptr)) {
    return c->ShowMenu(m, f, nullptr);
}
template <class C>
static auto _dvd_show(C* c, DvdMenuId m, DVD_CMD_FLAGS f, long)
    -> decltype(c->ShowMenu(m, f)) {
    return c->ShowMenu(m, f);
}
template <class C>
static auto _dvd_show(C* c, DvdMenuId m, DVD_CMD_FLAGS, ...)
    -> decltype(c->ShowMenu(m)) {
    return c->ShowMenu(m);
}

static HRESULT dvd_ShowMenu(IDvdControl2* c, DvdMenuId m, DVD_CMD_FLAGS f) {
    if (!c) return E_POINTER;
    return _dvd_show(c, m, f, 0);
}


static bool is_ctrl_pressed() {
    return (GetAsyncKeyState(VK_LCONTROL) & 0x8000) || (GetAsyncKeyState(VK_RCONTROL) & 0x8000) || (GetAsyncKeyState(VK_CONTROL) & 0x8000);
}

static bool dvd_is_handled_menu_vk(UINT vk) {
    switch (vk) {
        case VK_UP: case VK_DOWN: case VK_LEFT: case VK_RIGHT:
        case VK_RETURN: case VK_SPACE:
        case VK_BACK: case VK_ESCAPE:
        case VK_F1: case VK_F2:
            return true;
        default:
            return false;
    }
}

static bool dvd_send_menu_key(WPARAM vk) {
    // Enqueue only; main loop executes to avoid COM reentrancy and to keep the hook fast.
    MenuActionType t;
    switch (vk) {
        case VK_UP:    t = MenuActionType::Up; break;
        case VK_DOWN:  t = MenuActionType::Down; break;
        case VK_LEFT:  t = MenuActionType::Left; break;
        case VK_RIGHT: t = MenuActionType::Right; break;
        case VK_RETURN:
        case VK_SPACE: t = MenuActionType::Activate; break;
        case VK_BACK:
        case VK_ESCAPE:t = MenuActionType::Back; break;
        case VK_F1:    t = MenuActionType::Root; break;
        case VK_F2:    t = MenuActionType::Title; break;
        default:       return false;
    }
    enqueue_action(t);
    return true;
}


static bool is_foreground_mpv() {
    HWND fg = GetForegroundWindow();
    if (!fg) return false;

    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);

    const DWORD selfPid = GetCurrentProcessId();

    // Allow when mpv is foreground, or when this console app is foreground (so arrows still work).
    if (pid == selfPid) return true;
    if (g_mpvPid && pid == g_mpvPid) return true;

    // Fallback: compare against discovered main hwnd if available
    if (g_mpvHwnd) {
        if (fg == g_mpvHwnd) return true;
        HWND root = GetAncestor(fg, GA_ROOT);
        if (root == g_mpvHwnd) return true;
    }

    return false;
}


static void clear_vk_down_state() {
    for (int i = 0; i < 256; ++i) {
        g_vkDown[i].store(false, std::memory_order_relaxed);
        g_vkLastTick[i].store(0, std::memory_order_relaxed);
    }
}
static bool menu_keys_allowed() {
    // Allow key forwarding while we're in a menu domain, or if the navigator reports buttons,
    // or for a short grace period after we explicitly requested a menu.
    const DWORD now = GetTickCount();
    if (g_inMenu.load(std::memory_order_relaxed)) return true;
    if (g_buttonsAvail.load(std::memory_order_relaxed) > 0) return true;
    const DWORD until = g_allowKeysUntil.load(std::memory_order_relaxed);
    // Unsigned wrap-safe comparison: now is considered before until if (int32)(now-until) < 0
    return (int32_t)(now - until) < 0;
}

static LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {
        const KBDLLHOOKSTRUCT* k = (const KBDLLHOOKSTRUCT*)lParam;
        const UINT vk = (UINT)k->vkCode;

        const bool isDownMsg = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
        const bool isUpMsg   = (wParam == WM_KEYUP   || wParam == WM_SYSKEYUP);

        if (vk < 256 && isUpMsg) {
            g_vkDown[vk].store(false, std::memory_order_relaxed);
            g_vkLastTick[vk].store(0, std::memory_order_relaxed);
        }

        if (isDownMsg && is_foreground_mpv()) {
            const bool ctrl = is_ctrl_pressed();

            
// Handle first keydown immediately; while held, allow repeats at a modest rate.
bool already = false;
DWORD now = GetTickCount();
DWORD last = 0;
if (vk < 256) {
    already = g_vkDown[vk].exchange(true, std::memory_order_relaxed);
    last = g_vkLastTick[vk].load(std::memory_order_relaxed);
}
const DWORD REPEAT_MS = 120; // allow held-key repeats ~8 Hz
const bool allowRepeat = already && (last == 0 || (DWORD)(now - last) >= REPEAT_MS);

if (!already || allowRepeat) {
    if (vk < 256) g_vkLastTick[vk].store(now, std::memory_order_relaxed);
                // Playback-safe hotkeys: F1=Root menu, F2=Title menu (works both during playback and in menus).
                // We swallow these so mpv's input.conf does not also trigger.
                if (vk == VK_F1 || vk == VK_F2) {
                    g_allowKeysUntil.store(GetTickCount() + 5000, std::memory_order_relaxed);
                    if (dvd_send_menu_key(vk)) return 1;
                }
// Menu-only keys: only forward while we are in a menu (or shortly after requesting a menu).
                if (menu_keys_allowed()) {
                    if (dvd_send_menu_key(vk)) return 1;
                }
            } else {
                // Key auto-repeat: swallow handled keys, but do NOT enqueue again.
                if (vk == VK_F1 || vk == VK_F2) return 1;
                if (menu_keys_allowed() && dvd_is_handled_menu_vk(vk)) return 1;
            }
        }
    }
    return CallNextHookEx(g_kbdHook, nCode, wParam, lParam);
}


static LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {
        (void)lParam;
        // Let mpv's own input.conf mouse bindings (including right-click menu) continue to work.
        // We still keep a short grace period + reset key repeat state so DVD keyboard navigation stays responsive
        // after mouse interaction, but we do NOT swallow right-click anymore.
        if (menu_keys_allowed() && is_foreground_mpv()) {
            switch (wParam) {
                case WM_RBUTTONDOWN: case WM_RBUTTONUP:
                case WM_MBUTTONDOWN: case WM_MBUTTONUP:
                case WM_MOUSEWHEEL:  case WM_MOUSEHWHEEL:
                    g_allowKeysUntil.store(GetTickCount() + 3000, std::memory_order_relaxed);
                    clear_vk_down_state();
                    if (g_mpvHwnd && IsWindow(g_mpvHwnd)) {
                        SetForegroundWindow(g_mpvHwnd);
                        SetFocus(g_mpvHwnd);
                    }
                    // IMPORTANT: pass through so mpv input.conf / context menu can handle it.
                    break;
                default:
                    break;
            }
        }
    }
    return CallNextHookEx(g_mouseHook, nCode, wParam, lParam);
}

static void install_menu_hooks_if_possible(DvdState& st, HANDLE mpvProcess, bool enable) {
    if (!enable) return;
    if (!mpvProcess) return;
    if (g_kbdHook) return; // already installed

    DWORD pid = GetProcessId(mpvProcess);
    HWND hwnd = find_main_window_for_pid(pid, 0);
    if (!hwnd) {
        // mpv window not ready yet; caller can retry later without blocking startup.
        return;
    }

    g_st = &st;
    g_mpvHwnd = hwnd;

    g_kbdHook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc, GetModuleHandleW(nullptr), 0);
    // Mouse hook disabled by default (no DVD menu mouse control for now) to avoid system-wide cursor lag.
    // g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, LowLevelMouseProc, GetModuleHandleW(nullptr), 0);

    logi("Menu keyboard forwarding enabled (mpv hwnd=0x%p). Reserved keys: arrows/Enter/Space/Esc/Backspace, F1(root), F2(title). Mouse hook is DISABLED (DVD menu mouse control not implemented) so mpv's mouse input.conf and right-click menu work normally without any global cursor overhead.", hwnd);
}

static void uninstall_menu_hooks() {
    if (g_kbdHook) { UnhookWindowsHookEx(g_kbdHook); g_kbdHook = nullptr; }
    if (g_mouseHook) { UnhookWindowsHookEx(g_mouseHook); g_mouseHook = nullptr; }
    g_mpvHwnd = nullptr;
}

static void update_menu_state(DvdState& st) {
    if (!st.dvdinfo) return;

    bool inMenu = false;

    // Primary signal: domain (spec-correct for VM/VTS menus)
    DVD_DOMAIN d{};
    if (SUCCEEDED(st.dvdinfo->GetCurrentDomain(&d))) {
        g_curDomain.store((long)d, std::memory_order_relaxed);
        inMenu = (d == DVD_DOMAIN_VideoTitleSetMenu) || (d == DVD_DOMAIN_VideoManagerMenu);
    }

    // Current playback location (title/chapter) - used for mpv handoff.
    DVD_PLAYBACK_LOCATION2 loc{};
    if (SUCCEEDED(st.dvdinfo->GetCurrentLocation(&loc))) {
        g_curTitleNum.store((unsigned long)loc.TitleNum, std::memory_order_relaxed);
        g_curChapterNum.store((unsigned long)loc.ChapterNum, std::memory_order_relaxed);
    }

    // Secondary signal: interactive buttons (more robust on some discs/graphs)
    ULONG buttonsAvail = 0, btn = 0;
    HRESULT hrBtn = st.dvdinfo->GetCurrentButton(&buttonsAvail, &btn);
    if (SUCCEEDED(hrBtn)) {
        g_buttonsAvail.store(buttonsAvail, std::memory_order_relaxed);
        g_curButton.store(btn, std::memory_order_relaxed);
        if (buttonsAvail > 0) inMenu = true;
    } else {
        g_buttonsAvail.store(0, std::memory_order_relaxed);
        g_curButton.store(0, std::memory_order_relaxed);
    }

// Current video attributes (DAR can change between first-play/menu/title on authored DVDs).
if (st.dvdinfo) {
    DVD_VideoAttributes va{};
    HRESULT hrVa = st.dvdinfo->GetCurrentVideoAttributes(&va);
    if (SUCCEEDED(hrVa)) {
        if ((long)va.ulAspectX > 0 && (long)va.ulAspectY > 0) {
            g_curVideoAspectX.store((long)va.ulAspectX, std::memory_order_relaxed);
            g_curVideoAspectY.store((long)va.ulAspectY, std::memory_order_relaxed);
        }
        if ((long)va.ulFrameHeight > 0) {
            g_curVideoFrameH.store((long)va.ulFrameHeight, std::memory_order_relaxed);
        }
    }
}

    g_inMenu.store(inMenu, std::memory_order_relaxed);

    // Grace period: once we detect menu/buttons, keep forwarding keys for a bit even if the domain temporarily flips.
    if (inMenu) {
        const DWORD now = GetTickCount();
        g_allowKeysUntil.store(now + 3000, std::memory_order_relaxed);
        g_lastMenuSeenTick.store(now, std::memory_order_relaxed);
    }
}

static void process_menu_actions(DvdState& st) {
    if (!st.dvdctl) {
        // drain anyway
        (void)take_actions();
        return;
    }
    auto acts = take_actions();
    if (acts.empty()) return;
    // Most actions should only run while a menu is active (to avoid interfering with normal playback).
    // However, Root/Title menu requests should be allowed at any time (Ctrl+F1/Ctrl+F2).
    const bool allowMenu = menu_keys_allowed();

    const DVD_CMD_FLAGS FMove = (DVD_CMD_FLAGS)(DVD_CMD_FLAG_Flush);
    const DVD_CMD_FLAGS FAct  = (DVD_CMD_FLAGS)(DVD_CMD_FLAG_Flush);
    const DVD_CMD_FLAGS FNav  = (DVD_CMD_FLAGS)(DVD_CMD_FLAG_Flush);
    const DVD_CMD_FLAGS FMenu = (DVD_CMD_FLAGS)(DVD_CMD_FLAG_Flush);

    for (const auto& a : acts) {
        // Only Root/Title requests are allowed outside menus.
        if (!allowMenu && !(a.type == MenuActionType::Root || a.type == MenuActionType::Title)) {
            continue;
        }
        // Snapshot before action for diagnostics/retry verification.
        ULONG preAvail = 0, preBtn = 0;
        HRESULT hrPreBtn = E_FAIL;
        if (st.dvdinfo) hrPreBtn = st.dvdinfo->GetCurrentButton(&preAvail, &preBtn);
        if (g_verbose) log_menu_state_snapshot(st, "pre");

        // Mark in-flight (watchdog will hard-exit if a COM call deadlocks).
        g_dvdCmdInFlight.store(true, std::memory_order_release);
        g_dvdCmdStartTick.store(GetTickCount64(), std::memory_order_relaxed);
        HRESULT hr = S_OK;
        switch (a.type) {
            case MenuActionType::Up:       hr = dvd_SelectRelative(st.dvdctl, DVD_Relative_Upper, FMove); break;
            case MenuActionType::Down:     hr = dvd_SelectRelative(st.dvdctl, DVD_Relative_Lower, FMove); break;
            case MenuActionType::Left:     hr = dvd_SelectRelative(st.dvdctl, DVD_Relative_Left,  FMove); break;
            case MenuActionType::Right:    hr = dvd_SelectRelative(st.dvdctl, DVD_Relative_Right, FMove); break;
            case MenuActionType::Activate: hr = dvd_Activate(st.dvdctl, FAct); break;
            case MenuActionType::Back:     hr = dvd_ReturnFromSubmenu(st.dvdctl, FNav); break;
            case MenuActionType::Root:     hr = dvd_ShowMenu(st.dvdctl, DVD_MENU_Root,  FMenu); break;
            case MenuActionType::Title:    hr = dvd_ShowMenu(st.dvdctl, DVD_MENU_Title, FMenu); break;
            case MenuActionType::MouseSelect:
            case MenuActionType::MouseActivate:
                // not implemented in this minimal patch
                hr = S_FALSE;
                break;
            default: break;
        }

// Some DVDs occasionally ignore a navigation/activate command transiently (S_FALSE),
// especially during state transitions. Retry once to reduce "skipped" selections.
if (hr == S_FALSE) {
    const bool isMove = (a.type == MenuActionType::Up || a.type == MenuActionType::Down ||
                         a.type == MenuActionType::Left || a.type == MenuActionType::Right);
    const bool isAct  = (a.type == MenuActionType::Activate);
    const bool isBack = (a.type == MenuActionType::Back);
    if (isMove || isAct || isBack) {
        Sleep(15);
        switch (a.type) {
            case MenuActionType::Up:       hr = dvd_SelectRelative(st.dvdctl, DVD_Relative_Upper, FMove); break;
            case MenuActionType::Down:     hr = dvd_SelectRelative(st.dvdctl, DVD_Relative_Lower, FMove); break;
            case MenuActionType::Left:     hr = dvd_SelectRelative(st.dvdctl, DVD_Relative_Left,  FMove); break;
            case MenuActionType::Right:    hr = dvd_SelectRelative(st.dvdctl, DVD_Relative_Right, FMove); break;
            case MenuActionType::Activate: hr = dvd_Activate(st.dvdctl, FAct); break;
            case MenuActionType::Back:     hr = dvd_ReturnFromSubmenu(st.dvdctl, FNav); break;
            default: break;
        }
    }
}
        // Additional verification for directional moves: some discs return S_OK but keep the same button
        // during highlight/SPU init right after menu entry. Retry a few times if the button did not move.
        if (SUCCEEDED(hr)) {
            const bool isMove = (a.type == MenuActionType::Up || a.type == MenuActionType::Down ||
                                 a.type == MenuActionType::Left || a.type == MenuActionType::Right);
            if (isMove && st.dvdinfo && SUCCEEDED(hrPreBtn) && preAvail > 1) {
                for (int attempt = 0; attempt < 3; ++attempt) {
                    Sleep(12);
                    update_menu_state(st);
                    ULONG postAvail = 0, postBtn = 0;
                    HRESULT hrPostBtn = st.dvdinfo->GetCurrentButton(&postAvail, &postBtn);
                    if (SUCCEEDED(hrPostBtn) && postAvail > 0 && postBtn != 0 && postBtn != preBtn) break;
                    if (attempt == 2) break;
                    if (g_verbose) {
                        logi("menu move verify: button unchanged after action %d (pre=%lu), retry %d",
                             (int)a.type, (unsigned long)preBtn, attempt + 1);
                    }
                    switch (a.type) {
                        case MenuActionType::Up:    hr = dvd_SelectRelative(st.dvdctl, DVD_Relative_Upper, FMove); break;
                        case MenuActionType::Down:  hr = dvd_SelectRelative(st.dvdctl, DVD_Relative_Lower, FMove); break;
                        case MenuActionType::Left:  hr = dvd_SelectRelative(st.dvdctl, DVD_Relative_Left,  FMove); break;
                        case MenuActionType::Right: hr = dvd_SelectRelative(st.dvdctl, DVD_Relative_Right, FMove); break;
                        default: break;
                    }
                    if (FAILED(hr)) break;
                }
            }
        }

        if (g_verbose) log_menu_state_snapshot(st, "post");
        g_dvdCmdInFlight.store(false, std::memory_order_release);
        g_dvdCmdStartTick.store(0, std::memory_order_relaxed);

        if ((a.type == MenuActionType::Root || a.type == MenuActionType::Title) && SUCCEEDED(hr)) {
            // Give the domain a moment to switch; allow menu key forwarding during the transition.
            g_allowKeysUntil.store(GetTickCount() + 5000, std::memory_order_relaxed);
        }
        if (FAILED(hr)) {
            logw("menu action %d failed: hr=0x%08lx", (int)a.type, (unsigned long)hr);
        } else if (hr == S_FALSE) {
            // Some discs return S_FALSE when the move is not possible (edge) or temporarily ignored.
            if (g_verbose) logi("menu action %d -> S_FALSE", (int)a.type);
        } else if (g_verbose) {
            logi("menu action %d -> hr=0x%08lx", (int)a.type, (unsigned long)hr);
        }
    }
}





// ---------------- fast menu presence probe (no DirectShow) ----------------
// Goal: for menu-less discs, avoid building the DirectShow graph at all (faster startup).
// We detect menus by checking the IFO sector pointers:
//   VIDEO_TS.IFO @ 0x00C8 -> sector pointer to VMGM_PGCI_UT (VMG menu PGC table)
//   VTS_nn_0.IFO @ 0x00D0 -> sector pointer to VTSM_PGCI_UT (VTS menu PGC table)
// If all are zero / invalid, we consider it "no menus" and hand off to mpv directly.
// Offsets and semantics: see dvd.sourceforge.net/dvdinfo/ifo.html (IFO header pointers).

static bool read_file_word_at(HANDLE h, uint32_t off, uint16_t& outLE) {
    LARGE_INTEGER li; li.QuadPart = off;
    if (!SetFilePointerEx(h, li, nullptr, FILE_BEGIN)) return false;
    DWORD got = 0;
    uint8_t b[2] = {0,0};
    if (!ReadFile(h, b, 2, &got, nullptr) || got != 2) return false;
    outLE = (uint16_t)b[0] | ((uint16_t)b[1] << 8);
    return true;
}
static bool read_file_dword_at(HANDLE h, uint32_t off, uint32_t& outLE) {
    LARGE_INTEGER li; li.QuadPart = off;
    if (!SetFilePointerEx(h, li, nullptr, FILE_BEGIN)) return false;
    DWORD got = 0;
    uint8_t b[4] = {0,0,0,0};
    if (!ReadFile(h, b, 4, &got, nullptr) || got != 4) return false;
    outLE = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
    return true;
}
static uint32_t bswap32_u(uint32_t v) { return (v>>24) | ((v>>8)&0x0000FF00u) | ((v<<8)&0x00FF0000u) | (v<<24); }

static bool get_file_size_u64(const std::wstring& path, uint64_t& sz) {
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) return false;
    if (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return false;
    sz = ((uint64_t)fad.nFileSizeHigh << 32) | (uint64_t)fad.nFileSizeLow;
    return true;
}

static bool ifo_sector_ptr_nonzero_plausible(const std::wstring& ifoPath, uint32_t off) {
    uint64_t fsz = 0;
    if (!get_file_size_u64(ifoPath, fsz) || fsz < (off + 4)) return false;

    HANDLE h = CreateFileW(ifoPath.c_str(), GENERIC_READ, FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    uint32_t le = 0;
    bool ok = read_file_dword_at(h, off, le);
    CloseHandle(h);
    if (!ok) return false;

    // Try interpret as big-endian and little-endian; pick whichever yields a plausible byte offset.
    // Sector size is 2048 bytes.
    const uint64_t SECTOR = 2048;
    uint32_t be = bswap32_u(le);

    auto plausible = [&](uint32_t sp)->bool {
        if (sp == 0) return false;
        uint64_t byteOff = (uint64_t)sp * SECTOR;
        // Within file, and not pointing into the header (usually tables start >= 1 sector).
        return byteOff >= SECTOR && byteOff < fsz;
    };

    return plausible(be) || plausible(le);
}

static bool fast_disc_has_menu_tables(const std::wstring& dvdRoot) {
    // Accept both "X:\" and "... \VIDEO_TS\" roots.
    std::wstring root = dvdRoot;
    if (root.size() == 2 && root[1] == L':') root += L"\\";
    // If user passed the VIDEO_TS directory itself, keep it; else assume VIDEO_TS under root.
    std::wstring videoTsDir = root;
    std::wstring upper = root; 
    std::transform(upper.begin(), upper.end(), upper.begin(), ::towupper);
    if (upper.find(L"\\VIDEO_TS\\") == std::wstring::npos && upper.rfind(L"\\VIDEO_TS") != upper.size()-8) {
        if (!videoTsDir.empty() && videoTsDir.back() != L'\\') videoTsDir += L"\\";
        videoTsDir += L"VIDEO_TS\\";
    } else {
        if (!videoTsDir.empty() && videoTsDir.back() != L'\\') videoTsDir += L"\\";
    }

    // VMG menu PGC table pointer in VIDEO_TS.IFO (offset 0xC8).
    std::wstring vmgIfo = videoTsDir + L"VIDEO_TS.IFO";
    if (ifo_sector_ptr_nonzero_plausible(vmgIfo, 0x00C8)) return true;

    // Any VTS menu PGC table pointer in VTS_nn_0.IFO (offset 0xD0).
    int maxVts = 99;
    // VIDEO_TS.IFO @ 0x003E stores "number of title sets" (2 bytes). If readable, use it to limit disk I/O.
    {
        uint64_t fsz = 0;
        if (get_file_size_u64(vmgIfo, fsz) && fsz >= 0x003E + 2) {
            HANDLE h = CreateFileW(vmgIfo.c_str(), GENERIC_READ, FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                                   nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h != INVALID_HANDLE_VALUE) {
                uint16_t le16 = 0;
                if (read_file_word_at(h, 0x003E, le16)) {
                    uint16_t be16 = (uint16_t)((le16 >> 8) | (le16 << 8));
                    auto plaus = [](uint16_t v)->bool { return v >= 1 && v <= 99; };
                    uint16_t v = plaus(be16) ? be16 : (plaus(le16) ? le16 : 0);
                    if (v) maxVts = (int)v;
                }
                CloseHandle(h);
            }
        }
    }


    for (int n = 1; n <= maxVts; n++) {
        wchar_t name[64];
        swprintf(name, 64, L"VTS_%02d_0.IFO", n);
        std::wstring vtsIfo = videoTsDir + name;
        if (ifo_sector_ptr_nonzero_plausible(vtsIfo, 0x00D0)) return true;
    }
    return false;
}


static bool dir_exists_simple(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static std::wstring detect_dvd_drive_root_auto() {
    wchar_t buf[512]{};
    DWORD n = GetLogicalDriveStringsW((DWORD)(sizeof(buf)/sizeof(buf[0])), buf);
    if (!n || n >= (DWORD)(sizeof(buf)/sizeof(buf[0]))) return L"";

    std::wstring fallbackCdrom;
    for (const wchar_t* p = buf; *p; p += wcslen(p) + 1) {
        std::wstring root = p; // e.g. "D:\\"
        UINT dt = GetDriveTypeW(root.c_str());
        if (dt != DRIVE_CDROM) continue;
        if (fallbackCdrom.empty()) fallbackCdrom = root;

        std::wstring videoTs = root;
        if (!videoTs.empty() && videoTs.back() != L'\\') videoTs += L'\\';
        videoTs += L"VIDEO_TS";
        if (dir_exists_simple(videoTs)) return root;

        std::wstring videoTs2 = root;
        if (!videoTs2.empty() && videoTs2.back() != L'\\') videoTs2 += L'\\';
        videoTs2 += L"video_ts";
        if (dir_exists_simple(videoTs2)) return root;
    }
    return fallbackCdrom;
}

// ---------------- main ----------------
static void usage() {
    logi("Usage: dvdmenu.exe [--dvd-device \"H:\\\"] [--start-menu root|title|none] [--startup-delay-ms 1500] [--audio-delay-ms 0..5000] [--audio-tap-trial on|off] [--handoff on|off] [--handoff-wait-ms 300] [--handoff-watchdog-sec 45] [--fps 60|auto] [--log path] [--mpv-log path] [--dump-first-bmp path] [--mpv mpv.exe] [--verbose] [--no-input-hook] [--open-iso-dialog] [--iso-file path]");
}


static void execute_audio_pcm_phase_b2_passthrough_trial_optin(DvdState& st, bool enableAudioTapTrial, int audioDelayMs) {
    (void)audioDelayMs;
    // Incremental step: run real PCM queue component self-test (no graph changes yet),
    // then keep the reversible Phase-A same-edge transaction as the opt-in trial.
    run_audio_pcm_component_selftest_step3c3(st, (DWORD)audioDelayMs);
    exec_audio_rewire_same_edge_trial_phase_a(st, enableAudioTapTrial);
}

int wmain(int argc, wchar_t** argv) {
    std::wstring dvdDevice;
    std::wstring mpvPath = L"mpv.exe";
    std::wstring logPath;
    std::wstring mpvLogPath;
    std::wstring dumpBmpPath;
    std::wstring startMenuWhich = L"none";
    std::wstring oneShotCommand;
    std::wstring commandOrLaunch;
    bool deferredCloseExistingMpvForOpenIso = false;
    std::wstring isoFilePath;
    IsoAutoUnmountGuard isoUnmountGuard;
    bool openIsoDialogMode = false;
    double fps = 60.0;
    bool enableInputHook = true;
    bool enableHandoff = false; // DirectShow継続固定（メニュー選択後も mpv handoff しない）
    DWORD handoffWaitMs = 300;
    DWORD startupDelayMs = 0;
    DWORD audioDelayMs = 0; // Delay hidden DirectShow audio output by this many milliseconds
    bool enableAudioTapTrial = false; // Phase B2 opt-in (guarded same-edge rewire trial)

    int outW = 720, outH = 480;

    // Watchdog child mode: dvdmenu.exe --watchdog-child <mpvPid> <dvdmenuPid> <timeoutSec>
    if (argc >= 5 && wcscmp(argv[1], L"--watchdog-child") == 0) {
        DWORD mpvPid = (DWORD)wcstoul(argv[2], nullptr, 10);
        DWORD parentPid = (DWORD)wcstoul(argv[3], nullptr, 10);
        DWORD sec = (DWORD)wcstoul(argv[4], nullptr, 10);
        return watchdog_child_main(mpvPid, parentPid, sec);
    }


    for (int i=1; i<argc; i++) {
        std::wstring a = argv[i];
        auto next = [&](std::wstring& out)->bool { if (i+1 >= argc) return false; out = argv[++i]; return true; };

        if (a == L"--dvd-device") next(dvdDevice);
        else if (a == L"--mpv") next(mpvPath);
        else if (a == L"--log") next(logPath);
        else if (a == L"--mpv-log") next(mpvLogPath);
        else if (a == L"--dump-first-bmp") next(dumpBmpPath);
        else if (a == L"--start-menu") next(startMenuWhich);
        else if (a == L"--command") next(oneShotCommand);
        else if (a == L"--command-or-launch") next(commandOrLaunch);
        else if (a == L"--iso-file") next(isoFilePath);
        else if (a == L"--open-iso-dialog") openIsoDialogMode = true;
        else if (a == L"--fps") {
            std::wstring s; if (next(s)) {
                if (_wcsicmp(s.c_str(), L"auto") == 0) fps = 0.0;
                else fps = wcstod(s.c_str(), nullptr);
            }
        }
        else if (a == L"--startup-delay-ms") {
            std::wstring s; if (next(s)) startupDelayMs = (DWORD)wcstoul(s.c_str(), nullptr, 10);
        }
        else if (a == L"--audio-delay-ms") {
            std::wstring s; if (next(s)) {
                wchar_t* endp = nullptr;
                unsigned long v = wcstoul(s.c_str(), &endp, 10);
                if (endp == s.c_str() || (endp && *endp != L'\0')) {
                    logw("Invalid --audio-delay-ms value: %ls (expected integer ms). Using 0.", s.c_str());
                    audioDelayMs = 0;
                } else {
                    if (v > 5000UL) {
                        logw("--audio-delay-ms=%lu is too large. Clamping to 5000 ms for safety.", v);
                        v = 5000UL;
                    }
                    audioDelayMs = (DWORD)v;
                }
            }
        }
        else if (a == L"--audio-tap-trial") {
            std::wstring v; next(v);
            if (_wcsicmp(v.c_str(), L"on") == 0 || _wcsicmp(v.c_str(), L"1") == 0 || _wcsicmp(v.c_str(), L"true") == 0) enableAudioTapTrial = true;
            else if (_wcsicmp(v.c_str(), L"off") == 0 || _wcsicmp(v.c_str(), L"0") == 0 || _wcsicmp(v.c_str(), L"false") == 0) enableAudioTapTrial = false;
            else { logw("Unknown --audio-tap-trial value: %ls (expected on/off). Using off.", v.c_str()); enableAudioTapTrial = false; }
        }
        else if (a == L"--handoff") {
            std::wstring s; if (next(s)) {
                if (_wcsicmp(s.c_str(), L"on") == 0 || _wcsicmp(s.c_str(), L"yes") == 0) enableHandoff = true;
                else if (_wcsicmp(s.c_str(), L"off") == 0 || _wcsicmp(s.c_str(), L"no") == 0) enableHandoff = false;
            }
        }
        else if (a == L"--handoff-watchdog-sec") {
            std::wstring s; if (next(s)) g_handoffWatchdogSec = (DWORD)wcstoul(s.c_str(), nullptr, 10);
        }

        else if (a == L"--verbose") g_verbose = true;
        else if (a == L"--debug-overlay") g_debug_overlay = true;
        else if (a == L"--no-input-hook") enableInputHook = false;
        else if (a == L"--help" || a == L"-h" || a == L"/?") { usage(); return 0; }
        else {
            // ignore unknown to keep compatibility with your earlier options
        }
    }

    open_log(logPath);
    start_dvd_cmd_watchdog_thread();
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);

    if (!oneShotCommand.empty()) {
        bool ok = send_ipc_menu_command_to_running_instance(oneShotCommand);
        return ok ? 0 : 11;
    }

    if (!commandOrLaunch.empty()) {
        const bool isOpenIsoCmd =
            (_wcsicmp(commandOrLaunch.c_str(), L"open-iso") == 0 ||
             _wcsicmp(commandOrLaunch.c_str(), L"iso") == 0 ||
             _wcsicmp(commandOrLaunch.c_str(), L"menu-iso") == 0);
        if (!isOpenIsoCmd) {
            bool ok = send_ipc_menu_command_to_running_instance(commandOrLaunch);
            if (ok) return 0;
            logw("command-or-launch: IPC target not found for '%ls'; falling back to fresh launch path.", commandOrLaunch.c_str());
        } else {
            logi("command-or-launch '%ls' -> local ISO open path (no IPC).", commandOrLaunch.c_str());
        }
        if (_wcsicmp(commandOrLaunch.c_str(), L"menu-root") == 0 || _wcsicmp(commandOrLaunch.c_str(), L"root") == 0) {
            startMenuWhich = L"root";
        } else if (_wcsicmp(commandOrLaunch.c_str(), L"menu-title") == 0 || _wcsicmp(commandOrLaunch.c_str(), L"title") == 0) {
            startMenuWhich = L"title";
        } else if (isOpenIsoCmd) {
            openIsoDialogMode = true;
            // Default to root menu unless explicitly overridden by --start-menu.
            if (_wcsicmp(startMenuWhich.c_str(), L"none") == 0) startMenuWhich = L"root";
        } else {
            loge("Unknown --command-or-launch value: %ls (expected menu-root/menu-title/open-iso)", commandOrLaunch.c_str());
            return 12;
        }
        if (!isOpenIsoCmd) {
            if (try_close_foreground_mpv_window_for_command_or_launch()) {
                Sleep(250);
            }
        } else {
            logi("command-or-launch: open-iso flow -> defer foreground mpv close until just before new mpv launch.");
            deferredCloseExistingMpvForOpenIso = true;
        }
    }


    logi("Mode: DirectShow continuous playback (menu handoff to mpv is DISABLED)");
    logi("Audio delay setting (DirectShow hidden audio): %lu ms", (unsigned long)audioDelayMs);
    logi("Audio PCM tap trial switch (Phase B2 passthrough): %s", enableAudioTapTrial ? "ON (opt-in)" : "OFF (default)");

    if (openIsoDialogMode && isoFilePath.empty()) {
        logi("Opening ISO file dialog...");
        if (!open_iso_file_dialog(isoFilePath)) {
            logw("ISO selection cancelled.");
            return 0;
        }
        logi("Selected ISO: %ls", isoFilePath.c_str());
    }

    if (!isoFilePath.empty()) {
        // Single-ISO policy is enforced inside mount_iso_and_get_drive_root() in one helper call.
        std::wstring mountedRoot;
        if (!mount_iso_and_get_drive_root(isoFilePath, mountedRoot)) {
            loge("Failed to mount/resolve ISO drive root for: %ls", isoFilePath.c_str());
            return 13;
        }
        dvdDevice = mountedRoot;
        isoUnmountGuard.arm(isoFilePath);
        logi("Using mounted ISO as dvd-device: %ls", dvdDevice.c_str());
    }

    if (dvdDevice.empty()) {
        dvdDevice = detect_dvd_drive_root_auto();
        if (!dvdDevice.empty()) {
            logi("Auto-detected DVD drive: %ls", dvdDevice.c_str());
        } else {
            loge("--dvd-device is required (auto-detect failed).");
            usage();
            return 2;
        }
    }
    if (startupDelayMs > 0) {
        logi("Startup delay: %lu ms", (unsigned long)startupDelayMs);
        Sleep(startupDelayMs);
    }

    // Normalize dvd root early (before COM) and do a fast "menu present" probe.
    std::wstring dvdRoot = dvdDevice;
    if (dvdRoot.size() == 2 && dvdRoot[1] == L':') dvdRoot += L"\\";
    logi("DVD device root: %ls", dvdRoot.c_str());

    // This tool is for DVD menus. If the user didn't request menus, hand off immediately.
    bool wantMenu = (_wcsicmp(startMenuWhich.c_str(), L"none") != 0);
    if (!wantMenu) {
        logi("start-menu=none -> handing off to mpv direct DVD playback immediately.");
        launch_mpv_dvd_direct(mpvPath, dvdDevice, false, mpvLogPath);
        dismount_current_run_iso_with_retry_and_clear();
        isoUnmountGuard.disarm();
        return 0;
    }

    // Fast IFO probe: if no menu tables exist, don't build DirectShow graph (much faster).
    if (!fast_disc_has_menu_tables(dvdRoot)) {
        logw("Fast probe: no DVD menu tables found. Handing off to mpv direct DVD playback (dvd://).");
        launch_mpv_dvd_direct(mpvPath, dvdDevice, false, mpvLogPath);
        dismount_current_run_iso_with_retry_and_clear();
        isoUnmountGuard.disarm();
        return 0;
    }

    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) { loge("CoInitializeEx failed: 0x%08lx", (unsigned long)hr); return 3; }
    CoInitializeSecurity(nullptr, -1, nullptr, nullptr,
                         RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IDENTIFY,
                         nullptr, EOAC_NONE, nullptr);


    DvdState st;
    AudioDelayGate audioGate{};
    const DWORD kMenuTransitionAudioMuteMs = 180; // mask short return-to-menu audio crackle/pops
    hr = build_dvd_graph(st, dvdRoot);
    if (FAILED(hr)) { loge("build_dvd_graph failed: 0x%08lx", (unsigned long)hr); CoUninitialize(); return 4; }

    hr = ensure_vmr9_and_connect(st);
    if (FAILED(hr)) { loge("ensure_vmr9_and_connect failed: 0x%08lx", (unsigned long)hr); dump_graph(st.graph.get()); CoUninitialize(); return 6; }

    log_audio_timeline_scaffold_step2(st, audioDelayMs);
    log_audio_capture_hook_scaffold_step3a(st);
    log_audio_pcm_queue_scaffold_step3b(st, audioDelayMs);
    log_audio_pcm_tap_insertion_prep_step3c1(st, audioDelayMs);
    log_audio_pcm_tap_component_scaffold_step3c2(st, audioDelayMs);
    execute_audio_pcm_phase_b2_passthrough_trial_optin(st, enableAudioTapTrial, audioDelayMs);

    if (!st.mc) { loge("IMediaControl unavailable."); CoUninitialize(); return 5; }

    hr = st.mc->Run();
    if (FAILED(hr)) { loge("IMediaControl::Run failed: 0x%08lx", (unsigned long)hr); CoUninitialize(); return 7; }
    if (audioDelayMs > 0) audio_delay_gate_schedule(st, audioGate, audioDelayMs);
    // If the user requested to start in a menu, try immediately.
    // For menu-less discs, DVD Navigator returns MENU_DOES_NOT_EXIST quickly.
    // In that case, release the DVD graph and let mpv open the disc normally (dvd://),
    // instead of staying in a black-frame loop.
    HRESULT hrMenu = S_FALSE;
    if (wantMenu) {
        if (_wcsicmp(startMenuWhich.c_str(), L"title") == 0) {
            hrMenu = dvd_ShowMenu(st.dvdctl, DVD_MENU_Title, DVD_CMD_FLAG_None);
            logi("ShowMenu(title) -> 0x%08lx", (unsigned long)hrMenu);
        } else {
            // default root
            hrMenu = dvd_ShowMenu(st.dvdctl, DVD_MENU_Root, DVD_CMD_FLAG_None);
            logi("ShowMenu(root) -> 0x%08lx", (unsigned long)hrMenu);
        }

        if (hr_is_menu_missing(hrMenu)) {
            logw("DVD menu not present (hr=0x%08lx). Releasing graph and handing off to mpv dvd:// playback.",
                 (unsigned long)hrMenu);
            audio_delay_gate_force_restore(st, audioGate);
    pcm_delay_stop(st);
            pcm_delay_stop(st);
            if (st.mc) st.mc->Stop();
            // Release COM objects before launching mpv, otherwise mpv can't open the DVD device.
            st = DvdState{};
            CoUninitialize();

            // Hand off: start at title (dvd://), not dvd://menu.
            launch_mpv_dvd_direct(mpvPath, dvdDevice, false, mpvLogPath);
            return 0;
        }
    }


    // Decide DVD standard (PAL/NTSC) from DVD Navigator attributes when possible,
    // then sanitize odd display-oriented native sizes (e.g. 720x540) to raster sizes.
    int expectedRasterH = 0; // 480 (NTSC) or 576 (PAL)
    int dvdAspectX = 0;      // display aspect from DVD Navigator (preferred)
    int dvdAspectY = 0;
    if (st.dvdinfo) {
        DVD_VideoAttributes va{};
        HRESULT hrVa = st.dvdinfo->GetCurrentVideoAttributes(&va);
        if (SUCCEEDED(hrVa)) {
            // PAL/NTSC (use frame height; some SDKs differ in exposed enums/fields)
            if ((long)va.ulFrameHeight >= 560) expectedRasterH = 576;
            else if ((long)va.ulFrameHeight > 0) expectedRasterH = 480;

            // Prefer DVD Navigator's recorded display aspect ratio over VMR9-reported aspect.
            // Typical values are 4:3 or 16:9.
            if ((int)va.ulAspectX > 0 && (int)va.ulAspectY > 0) {
                dvdAspectX = (int)va.ulAspectX;
                dvdAspectY = (int)va.ulAspectY;
            }

            if (expectedRasterH == 480 || expectedRasterH == 576) {
                logi("DVD video attributes: frameHeight=%lu aspect=%lux%lu -> expected raster 720x%d",
                     (unsigned long)va.ulFrameHeight,
                     (unsigned long)va.ulAspectX, (unsigned long)va.ulAspectY,
                     expectedRasterH);
            } else {
                logw("DVD video attributes returned unusual frameHeight=%lu aspect=%lux%lu; leaving raster undecided",
                     (unsigned long)va.ulFrameHeight,
                     (unsigned long)va.ulAspectX, (unsigned long)va.ulAspectY);
            }
        } else {
            logw("GetCurrentVideoAttributes failed: 0x%08lx", (unsigned long)hrVa);
        }
    }

    // Try to use the native DVD video size for rawvideo output.
    // 1) Normalize to DVD raster (720x480/576) so field cadence stays faithful.
    // 2) Convert to a square-pixel frame size using the reported display aspect (aw:ah),
    //    so mpv rawvideo displays menus/titles at the same DAR as the DVD.
    long nw=0, nh=0, aw=0, ah=0;
    if (st.wl && SUCCEEDED(st.wl->GetNativeVideoSize(&nw, &nh, &aw, &ah)) && nw > 0 && nh > 0) {
        st.nativeW = (int)nw;
        st.nativeH = (int)nh;

        int rasterW = 720;
        int rasterH = (int)nh;
        if (rasterH <= 0) rasterH = (outH > 0 ? outH : 480);

        // Prefer DVD Navigator's PAL/NTSC answer when available.
        if (expectedRasterH == 480 || expectedRasterH == 576) {
            rasterH = expectedRasterH;
        } else {
            // Fallback heuristic if attributes are unavailable.
            if (nh >= 560 && nh <= 590) rasterH = 576;
            else if (nh >= 460 && nh <= 500) rasterH = 480;
            else if (nh >= 520 && nh <= 555) rasterH = (outH >= 570) ? 576 : 480; // 720x540-ish
        }

        int finalW = rasterW;
        int finalH = rasterH;

        // Convert to square-pixel output while preserving raster height.
        // IMPORTANT: Prefer DVD Navigator's recorded display aspect (ulAspectX:ulAspectY).
        // VMR9's aw:ah is used only as a fallback because it may reflect a transient/wrong state.
        int useAspectX = 0, useAspectY = 0;
        if (dvdAspectX > 0 && dvdAspectY > 0) {
            useAspectX = dvdAspectX;
            useAspectY = dvdAspectY;
        } else if (aw > 0 && ah > 0) {
            useAspectX = (int)aw;
            useAspectY = (int)ah;
        }
        if (useAspectX > 0 && useAspectY > 0 && rasterH > 0) {
            double dar = (double)useAspectX / (double)useAspectY;
            int squareW = (int)(((double)rasterH * dar) + 0.5);
            if (squareW > 0) {
                if (squareW & 1) squareW += 1; // keep even width
                finalW = squareW;
            }
            logi("AR source: %s (%dx%d)",
                 (dvdAspectX > 0 && dvdAspectY > 0) ? "DVD Navigator" : "VMR9 fallback",
                 useAspectX, useAspectY);
        }

        outW = finalW;
        outH = finalH;

        logi("Native video size: %ldx%ld (VMR9 aspect %ldx%ld). DVD raster=%dx%d, rawvideo(square-pixel)=%dx%d.",
             nw, nh, aw, ah, rasterW, rasterH, outW, outH);
    }

// Preview canvas is fixed-size rawvideo. Some authored DVDs switch DAR between first-play/menu (4:3)
// and title playback (16:9). Keep a 16:9-capable canvas and pillarbox 4:3 frames dynamically so both
// are displayed correctly without restarting mpv.
if (outH > 0) {
    int maxCanvasW = (int)(((double)outH * 16.0 / 9.0) + 0.5);
    if (maxCanvasW & 1) maxCanvasW += 1;
    if (maxCanvasW > outW) {
        logi("Aspect-switch-safe preview canvas: widening rawvideo from %dx%d to %dx%d (4:3/16:9 mixed DVD support).",
             outW, outH, maxCanvasW, outH);
        outW = maxCanvasW;
    }
}

    // Defer the initial ShowMenu until after mpv has opened its window and started consuming frames.
    // This makes startup *feel* much faster (mpv window appears immediately) even if the DVD drive takes time.
    bool pendingStartMenu = false; // ShowMenu handled immediately above
    DWORD pendingMenuAt = GetTickCount() + 200; // allow mpv to pop up first
    bool pendingForceRender = true;
    DWORD forceRenderAt = GetTickCount() + 50; // after mpv window shows

    // Launch mpv and stream to stdin
    MpvStdin mpv;
    // If --fps auto (or invalid), decide now based on video height.
    if (!(fps > 0.0)) fps = (outH >= 570) ? 25.000 : 29.970;

    logi("Rawvideo output setup: %dx%d yuv444p @ %.3f fps", outW, outH, fps);

    if (deferredCloseExistingMpvForOpenIso) {
        // open-iso can be triggered while older dvdmenu/mpv preview windows still exist.
        // Closing only the foreground mpv is not enough; a stale background mpv may remain and
        // keep showing/receiving input for an older mounted ISO. Close all visible mpv windows first.
        int closedMpvCount = close_all_mpv_windows_for_command_or_launch();
        if (closedMpvCount <= 0) {
            if (try_close_foreground_mpv_window_for_command_or_launch()) {
                for (int k = 0; k < 20; ++k) { Sleep(50); }
            }
        } else {
            logi("command-or-launch open-iso: requested close for %d mpv window(s) before launching fresh preview.", closedMpvCount);
        }
        deferredCloseExistingMpvForOpenIso = false;
    }

    if (!launch_mpv_stdin(mpv, mpvPath, outW, outH, fps, mpvLogPath)) {
        CoUninitialize(); 
        return 10;
    }

    // Enable keyboard/mouse forwarding to DVD Navigator while the DVD is in menu domain.
    install_menu_hooks_if_possible(st, mpv.hProcess, enableInputHook);

    // Stream loop: send black frames until capture produces non-black (or at least valid)
    const size_t outBytes = (size_t)outW * (size_t)outH * 4;
    const size_t outYuv444pBytes = ((size_t)outW * (size_t)outH * 3);
    std::vector<uint8_t> black(outYuv444pBytes, 0);
    if (outYuv444pBytes > 0) {
        const size_t planeBytes = (size_t)outW * (size_t)outH;
        std::fill(black.begin(), black.begin() + planeBytes, 16);
        std::fill(black.begin() + planeBytes, black.begin() + planeBytes * 2, 128);
        std::fill(black.begin() + planeBytes * 2, black.end(), 128);
    }

    uint64_t capCount=0, sentCount=0, blackCount=0, failCount=0;
    bool mpvWriteBroken = false; // mpv stopped consuming stdin (e.g., user loaded another file)
    DWORD lastStat = GetTickCount();
    bool dumped = false;
    Frame fr;
    std::vector<uint8_t> outBgr0;
    outBgr0.resize(outBytes);
    std::vector<uint8_t> outYuv444p;
    outYuv444p.resize(outYuv444pBytes);
    std::vector<uint8_t> lastGoodYuv444p;
    lastGoodYuv444p.resize(outYuv444pBytes);
    bool hasLastGood = false;
    // Stable pacing: use QPC-based absolute scheduling (higher precision than GetTickCount/Sleep alone).
    double nextFrameAtMs = qpc_now_ms();
    const double frameIntervalMs = (fps > 1.0) ? (1000.0 / fps) : 15.0;

    DWORD handoffCandidateSince = 0;
    bool didHandoff = false;
    bool prevInMenuForKick = false;
    DWORD lastMenuKickTick = 0;

    // If ShowMenu() succeeded but the rendered menu stays black for too long, treat it as a non-usable menu
    // and fall back to mpv's normal DVD playback. This avoids black-screen stalls on discs that technically have
    // menu domains/assets but no practical visible/interactive menu for our preview path.
    const bool menuVisualProbeArmed = (wantMenu && SUCCEEDED(hrMenu));
    const DWORD menuVisualProbeStart = GetTickCount();
    const DWORD menuVisualProbeTimeoutMs = 4000; // conservative: let slow drives settle first

    while (true) {
        // Pump messages (required for hooks and helps VMR9 draw)
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        if (g_requestQuit.load(std::memory_order_relaxed)) {
            logw("Quit requested (console signal).");
            break;
        }
        // If mpv window is gone, request quit (helps avoid hanging when the user closes mpv).
        if (g_mpvHwnd && !IsWindow(g_mpvHwnd)) {
            logw("mpv window closed.");
            break;
        }
        static DWORD s_nextIpcPoll = 0;
        if (!mpv.ipcPipeName.empty() && GetTickCount() >= s_nextIpcPoll) {
            s_nextIpcPoll = GetTickCount() + 250;
            std::string curPath;
            if (mpv_ipc_get_property_string(mpv.ipcPipeName, "path", curPath)) {
                mpv.ipcReadySeen = true;
                if (!curPath.empty() && curPath != "-" && curPath != "fd://0") {
                    logw("Preview mpv switched away from stdin path to '%s'; shutting down DVD graph/audio.", curPath.c_str());
                    mpvWriteBroken = true;
                    if (st.mc) st.mc->Stop();
                    break;
                }
            }
        }

        // Non-blocking hook install retry (do not slow down startup)
        static DWORD s_nextHookTry = 0;
        DWORD nowTick = GetTickCount();
        if (enableInputHook && !g_kbdHook && nowTick >= s_nextHookTry) {
            install_menu_hooks_if_possible(st, mpv.hProcess, true);
            s_nextHookTry = nowTick + 100;
        }
        static DWORD s_nextMenuPoll = 0;
        if (nowTick >= s_nextMenuPoll) {
            update_menu_state(st);
            const bool nowMenuKick = g_inMenu.load(std::memory_order_relaxed);
            if (nowMenuKick && !prevInMenuForKick) {
                if (st.wl && st.ownerHwnd) {
                    // Hybrid build: keep v89's stable VMR9/BGR0 capture path, but adopt
                    // v84's low-impact menu refresh so we do not disturb the audio path.
                    logi("Menu transition detected; repaint-only refresh (hybrid v89 video path, no Pause/Run nudge).");
                    (void)st.wl->RepaintVideo(st.ownerHwnd, nullptr);
                }
                lastMenuKickTick = nowTick;
            }
            prevInMenuForKick = nowMenuKick;
            s_nextMenuPoll = nowTick + 15; // ~66Hz is enough for menu navigation state
        }
        process_menu_actions(st);
        audio_delay_gate_poll(st, audioGate);

        // Menu -> title handoff:
        // Once the DVD leaves a menu domain and enters Title domain, we can stop the rawvideo menu preview
        // and let mpv open the disc normally (dvd://<title>) starting at the selected title/chapter.
        if (enableHandoff) {
            const DWORD now = GetTickCount();
            const bool inMenu = g_inMenu.load(std::memory_order_relaxed);
            const long dom = g_curDomain.load(std::memory_order_relaxed);
            const unsigned long titleNum = g_curTitleNum.load(std::memory_order_relaxed);
            const unsigned long chapNum  = g_curChapterNum.load(std::memory_order_relaxed);
            const DWORD lastMenu = g_lastMenuSeenTick.load(std::memory_order_relaxed);

            const bool cameFromMenuRecently = (lastMenu != 0) && (now - lastMenu < 15000);
            const bool inTitleDomain = ((DVD_DOMAIN)dom == DVD_DOMAIN_Title);

            static unsigned long handoffStableTitle = 0;
            static unsigned long handoffStableChap = 0;
            static DWORD handoffStableSince = 0;

            if (!inMenu && inTitleDomain && titleNum > 0 && cameFromMenuRecently) {
                if (handoffCandidateSince == 0) handoffCandidateSince = now;

                if (handoffStableTitle != titleNum || handoffStableChap != chapNum) {
                    handoffStableTitle = titleNum;
                    handoffStableChap = chapNum;
                    handoffStableSince = now;
                    if (g_verbose) {
                        logi("handoff candidate update: T=%lu C=%lu dom=%ld (stabilizing)", titleNum, chapNum, dom);
                    }
                }

                const bool titleStable = (handoffStableSince != 0) && (now - handoffStableSince >= 350);
                if (now - handoffCandidateSince >= handoffWaitMs && titleStable) {
                    logi("Menu selection resolved to Title=%lu Chapter=%lu. Handing off to mpv dvd:// playback...",
                         titleNum, chapNum);

                    // Stop streaming mpv (rawvideo) so we can launch a new mpv instance for dvd://.
                    uninstall_menu_hooks();
                    if (g_mpvHwnd && IsWindow(g_mpvHwnd)) PostMessageW(g_mpvHwnd, WM_CLOSE, 0, 0);
                    if (mpv.hWrite) { CloseHandle(mpv.hWrite); mpv.hWrite = nullptr; }

                    if (mpv.hProcess) {
                        DWORD r = WaitForSingleObject(mpv.hProcess, 1500);
                        if (r == WAIT_TIMEOUT) {
                            // If the user is still interacting, try a bit harder but don't hang.
                            TerminateProcess(mpv.hProcess, 0);
                        }
                        CloseHandle(mpv.hProcess);
                        mpv.hProcess = nullptr;
                    }

                    // Release DVD graph before launching mpv, otherwise mpv can't open the DVD device.
                    audio_delay_gate_force_restore(st, audioGate);
    pcm_delay_stop(st);
            pcm_delay_stop(st);
                    if (st.mc) st.mc->Stop();
                    st = DvdState{};
                    CoUninitialize();

                    // Launch mpv in normal DVD mode at the selected title/chapter.
                    launch_mpv_dvd_title(mpvPath, dvdDevice, (int)titleNum, (int)chapNum, mpvLogPath);
                    didHandoff = true;
                    dismount_current_run_iso_with_retry_and_clear();
                    isoUnmountGuard.disarm();
                    return 0;
                }
            } else {
                handoffCandidateSince = 0;
                handoffStableTitle = 0;
                handoffStableChap = 0;
                handoffStableSince = 0;
            }
        }


        if (pendingStartMenu && GetTickCount() >= pendingMenuAt) {
            start_menu(st, startMenuWhich);
            pendingStartMenu = false;
        }

        if (pendingForceRender && GetTickCount() >= forceRenderAt) {
            force_render_any_dvd_outputs(st);
            pendingForceRender = false;
        }

        HRESULT hrGet = E_FAIL;
        bool got = capture_vmr9(st, fr, &hrGet);

        if (got) {
            capCount++;
int curAspectX = (int)g_curVideoAspectX.load(std::memory_order_relaxed);
int curAspectY = (int)g_curVideoAspectY.load(std::memory_order_relaxed);
compose_aspect_correct_bgr0(fr, outW, outH, curAspectX, curAspectY, outBgr0);
overlay_selected_button(st, outBgr0, outW, outH);
            if (!dumped && !dumpBmpPath.empty()) {
                if (write_bmp32_bgr0(dumpBmpPath, outW, outH, outBgr0.data()))
                    logi("Wrote first BMP: %ls", dumpBmpPath.c_str());
                else
                    logw("Failed to write first BMP: %ls (GLE=%lu)", dumpBmpPath.c_str(), (unsigned long)gle());
                dumped = true;
            }

            bool allBlack = is_all_black(outBgr0);
            if (allBlack) {
                // Prefer re-sending the last valid frame instead of injecting black (reduces visible stutter/flicker).
                const uint8_t* srcFrame = hasLastGood ? lastGoodYuv444p.data() : black.data();
                const size_t srcLen = hasLastGood ? lastGoodYuv444p.size() : black.size();
                if (!write_all(mpv.hWrite, srcFrame, srcLen)) { loge("Write to mpv stdin failed (GLE=%lu)", (unsigned long)gle()); mpvWriteBroken = true; if (st.mc) st.mc->Stop(); break; }
                blackCount++;
            } else {
                bgr0_to_yuv444p(outBgr0, outW, outH, outYuv444p);
                std::memcpy(lastGoodYuv444p.data(), outYuv444p.data(), outYuv444p.size());
                hasLastGood = true;
                if (!write_all(mpv.hWrite, outYuv444p.data(), outYuv444p.size())) { loge("Write to mpv stdin failed (GLE=%lu)", (unsigned long)gle()); mpvWriteBroken = true; if (st.mc) st.mc->Stop(); break; }
                sentCount++;
            }
        } else {
            failCount++;
            const uint8_t* srcFrame = hasLastGood ? lastGoodYuv444p.data() : black.data();
            const size_t srcLen = hasLastGood ? lastGoodYuv444p.size() : black.size();
            if (!write_all(mpv.hWrite, srcFrame, srcLen)) { loge("Write to mpv stdin failed (GLE=%lu)", (unsigned long)gle()); mpvWriteBroken = true; if (st.mc) st.mc->Stop(); break; }
            blackCount++;
            Sleep(2);
        }

        DWORD now = GetTickCount();
        if (now - lastStat >= 1000) {
            lastStat = now;
            logi("stats: cap=%llu sent=%llu black=%llu fails=%llu lastGetHr=0x%08lx menu=%d",
                 (unsigned long long)capCount, (unsigned long long)sentCount, (unsigned long long)blackCount,
                 (unsigned long long)failCount, (unsigned long)hrGet, g_inMenu.load() ? 1 : 0);
        }

        // Early fallback: ShowMenu() succeeded, but we never obtained a visible frame within timeout.
        // This catches discs that appear to have a menu domain but produce a black/non-usable preview here.
        if (menuVisualProbeArmed && !didHandoff && sentCount == 0) {
            const DWORD probeNow = GetTickCount();
            const bool timedOut = (probeNow - menuVisualProbeStart >= menuVisualProbeTimeoutMs);
            const bool enoughSamples = (capCount + failCount) >= 90; // ~3s at 29.97fps, ~3.6s at 25fps
            if (timedOut && enoughSamples) {
                const bool stillMostlyBlack = (blackCount > 0);
                if (stillMostlyBlack) {
                    logw("Menu preview stayed black (cap=%llu sent=%llu black=%llu fails=%llu, %lu ms). Fallback to mpv dvd:// playback.",
                         (unsigned long long)capCount, (unsigned long long)sentCount,
                         (unsigned long long)blackCount, (unsigned long long)failCount,
                         (unsigned long)(probeNow - menuVisualProbeStart));

                    uninstall_menu_hooks();
                    if (g_mpvHwnd && IsWindow(g_mpvHwnd)) PostMessageW(g_mpvHwnd, WM_CLOSE, 0, 0);
                    if (mpv.hWrite) { CloseHandle(mpv.hWrite); mpv.hWrite = nullptr; }
                    if (mpv.hProcess) {
                        DWORD r = WaitForSingleObject(mpv.hProcess, 1500);
                        if (r == WAIT_TIMEOUT) TerminateProcess(mpv.hProcess, 0);
                        CloseHandle(mpv.hProcess);
                        mpv.hProcess = nullptr;
                    }

                    audio_delay_gate_force_restore(st, audioGate);
                    pcm_delay_stop(st);
                    pcm_delay_stop(st);
                    if (st.mc) st.mc->Stop();
                    st = DvdState{};
                    CoUninitialize();

                    launch_mpv_dvd_direct(mpvPath, dvdDevice, false, mpvLogPath);
                    dismount_current_run_iso_with_retry_and_clear();
                    isoUnmountGuard.disarm();
                    return 0;
                }
            }
        }

        // Detect mpv exit
        if (mpv.hProcess) {
            DWORD code = STILL_ACTIVE;
            if (GetExitCodeProcess(mpv.hProcess, &code) && code != STILL_ACTIVE) {
                logw("mpv exited.");
                g_mpvHwnd = nullptr;
                g_mpvPid = 0;
                break;
            }
        }

        // Pace to fps against an absolute schedule (QPC-based for steadier 29.97/25.00 frame pacing).
        nextFrameAtMs += frameIntervalMs;
        double nowMs = qpc_now_ms();
        if (nowMs + 1000.0 < nextFrameAtMs || nowMs - nextFrameAtMs > 1000.0) {
            // Large clock jump guard (suspend/resume, debugger break, etc.).
            nextFrameAtMs = nowMs + frameIntervalMs;
        }
        if (nextFrameAtMs > nowMs) {
            precise_sleep_until_ms(nextFrameAtMs);
        } else if (nowMs - nextFrameAtMs > frameIntervalMs * 3.0) {
            // If we fell behind badly, resync to avoid endless catch-up and erratic UI.
            nextFrameAtMs = nowMs;
        }
    }

    logi("Shutting down...");
    uninstall_menu_hooks();
    // Make sure the rawvideo stdin pipe is closed first so mpv can observe EOF and exit cleanly.
    if (mpv.hWrite && mpv.hWrite != INVALID_HANDLE_VALUE) {
        CloseHandle(mpv.hWrite);
        mpv.hWrite = INVALID_HANDLE_VALUE;
    }
    // Best-effort: ask mpv to close if we're exiting (especially on Ctrl+C / console close).
    if (g_mpvHwnd && IsWindow(g_mpvHwnd)) {
        PostMessageW(g_mpvHwnd, WM_CLOSE, 0, 0);
    }
    if (mpv.hProcess) {
        DWORD waitMs = 1500;
        DWORD r = WaitForSingleObject(mpv.hProcess, waitMs);
        if (r == WAIT_TIMEOUT) {
            logw("mpv did not exit in time during shutdown; terminating.");
            TerminateProcess(mpv.hProcess, 0);
            (void)WaitForSingleObject(mpv.hProcess, 1000);
        }
    }
    g_mpvHwnd = nullptr;
    g_mpvPid = 0;
    audio_delay_gate_force_restore(st, audioGate);
    pcm_delay_stop(st);
            pcm_delay_stop(st);
    if (st.mc) st.mc->Stop();
    // Release graph-owned COM objects before ISO cleanup so the mounted volume is no longer in use.
    st = DvdState{};
    CoUninitialize();
    dismount_current_run_iso_with_retry_and_clear();
    return 0;
}

// ---------------- command helper (pragmatic fallback) ----------------
// WM_COPYDATA receiver may not exist in some transitional builds. To keep
// --command / --command-or-launch useful, detect another running dvdmenu.exe
// and synthesize the same global hotkeys this tool already handles.
static bool has_other_running_dvdmenu_process() {
    DWORD selfPid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    bool found = false;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (pe.th32ProcessID == 0 || pe.th32ProcessID == selfPid) continue;
            if (_wcsicmp(pe.szExeFile, L"dvdmenu.exe") == 0) { found = true; break; }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

static bool send_ctrl_function_key(WORD vkFn) {
    INPUT in[4]{};
    in[0].type = INPUT_KEYBOARD; in[0].ki.wVk = VK_CONTROL;
    in[1].type = INPUT_KEYBOARD; in[1].ki.wVk = vkFn;
    in[2].type = INPUT_KEYBOARD; in[2].ki.wVk = vkFn; in[2].ki.dwFlags = KEYEVENTF_KEYUP;
    in[3].type = INPUT_KEYBOARD; in[3].ki.wVk = VK_CONTROL; in[3].ki.dwFlags = KEYEVENTF_KEYUP;
    UINT sent = SendInput(4, in, sizeof(INPUT));
    if (sent != 4) {
        logw("SendInput failed for Ctrl+F%d (sent=%u, GLE=%lu)",
             (int)(vkFn - VK_F1 + 1), (unsigned)sent, (unsigned long)GetLastError());
        return false;
    }
    return true;
}


static bool try_close_foreground_mpv_window_for_command_or_launch() {
    HWND fg = GetForegroundWindow();
    if (!fg) return false;

    wchar_t cls[128] = {};
    wchar_t title[512] = {};
    GetClassNameW(fg, cls, (int)(sizeof(cls) / sizeof(cls[0])));
    GetWindowTextW(fg, title, (int)(sizeof(title) / sizeof(title[0])));

    // IMPORTANT: only close if the owning process is actually mpv.exe/mpv.com
    if (!is_mpv_process_for_hwnd(fg)) {
        logi("command-or-launch fallback: foreground window is not mpv process (class='%ls', title='%ls'); not closing it.", cls, title);
        return false;
    }

    logi("command-or-launch fallback: closing foreground mpv window before launching fresh dvdmenu (class='%ls', title='%ls').", cls, title);
    PostMessageW(fg, WM_CLOSE, 0, 0);
    return true;
}



struct CloseMpvEnumCtx {
    HWND hwnds[64]{};
    int count = 0;
};

static BOOL CALLBACK enum_collect_mpv_windows(HWND hwnd, LPARAM lParam) {
    CloseMpvEnumCtx* ctx = reinterpret_cast<CloseMpvEnumCtx*>(lParam);
    if (!ctx) return TRUE;
    if (!IsWindow(hwnd)) return TRUE;

    wchar_t cls[128] = {};
    wchar_t title[512] = {};
    GetClassNameW(hwnd, cls, (int)(sizeof(cls) / sizeof(cls[0])));
    GetWindowTextW(hwnd, title, (int)(sizeof(title) / sizeof(title[0])));

    bool visible = (IsWindowVisible(hwnd) != FALSE);
    bool owned   = (GetWindow(hwnd, GW_OWNER) != nullptr);
    bool isMpvClass = (_wcsicmp(cls, L"mpv") == 0) || (wcsstr(cls, L"mpv") != nullptr);
    bool looksMpv = isMpvClass || is_mpv_process_for_hwnd(hwnd);
    if (!looksMpv) return TRUE;
    if (!visible || owned) return TRUE;

    if (ctx->count < (int)(sizeof(ctx->hwnds) / sizeof(ctx->hwnds[0]))) {
        ctx->hwnds[ctx->count++] = hwnd;
    }
    return TRUE;
}

static int close_all_mpv_windows_for_command_or_launch() {
    CloseMpvEnumCtx ctx{};
    EnumWindows(enum_collect_mpv_windows, (LPARAM)&ctx);

    if (ctx.count <= 0) {
        logi("command-or-launch fallback: no top-level mpv windows found to close.");
        return 0;
    }

    int posted = 0;
    for (int i = 0; i < ctx.count; ++i) {
        HWND h = ctx.hwnds[i];
        if (!IsWindow(h)) continue;
        wchar_t cls[128] = {};
        wchar_t title[512] = {};
        GetClassNameW(h, cls, (int)(sizeof(cls) / sizeof(cls[0])));
        GetWindowTextW(h, title, (int)(sizeof(title) / sizeof(title[0])));
        logi("command-or-launch fallback: closing mpv window[%d/%d] (hwnd=0x%p class='%ls' title='%ls').",
             i + 1, ctx.count, h, cls, title);
        if (PostMessageW(h, WM_CLOSE, 0, 0)) ++posted;
    }

    for (int k = 0; k < 30; ++k) Sleep(50); // ~1.5s

    // If mpv did not exit (scripts/idle/confirmation can block WM_CLOSE), force-terminate remaining mpv processes.
    CloseMpvEnumCtx ctx2{};
    EnumWindows(enum_collect_mpv_windows, (LPARAM)&ctx2);
    int killed = 0;
    for (int i = 0; i < ctx2.count; ++i) {
        HWND h = ctx2.hwnds[i];
        if (!IsWindow(h)) continue;
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        if (!pid) continue;
        HANDLE hp = OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (hp) {
            logw("command-or-launch fallback: mpv still alive after WM_CLOSE; terminating pid=%lu.", (unsigned long)pid);
            TerminateProcess(hp, 0);
            CloseHandle(hp);
            ++killed;
        }
    }
    if (killed > 0) {
        for (int k = 0; k < 20; ++k) Sleep(50); // extra ~1s for teardown
    }
    return posted;
}

static bool send_ipc_menu_command_to_running_instance(const std::wstring& cmd) {
    if (!has_other_running_dvdmenu_process()) {
        logw("IPC send failed: running dvdmenu instance window not found.");
        return false;
    }

    if (_wcsicmp(cmd.c_str(), L"menu-root") == 0 || _wcsicmp(cmd.c_str(), L"root") == 0) {
        bool ok = send_ctrl_function_key(VK_F1);
        if (ok) logi("command fallback via SendInput: menu-root (Ctrl+F1)");
        return ok;
    }
    if (_wcsicmp(cmd.c_str(), L"menu-title") == 0 || _wcsicmp(cmd.c_str(), L"title") == 0) {
        bool ok = send_ctrl_function_key(VK_F2);
        if (ok) logi("command fallback via SendInput: menu-title (Ctrl+F2)");
        return ok;
    }

    logw("Unknown command value: %ls", cmd.c_str());
    return false;
}
