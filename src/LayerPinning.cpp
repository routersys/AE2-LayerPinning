#include <windows.h>
#include <windowsx.h>
#include <stdio.h>
#include <vector>
#include <unordered_map>

#include "plugin2.h"
#include "logger2.h"
#include "config2.h"

static EDIT_HANDLE*   g_edit    = nullptr;
static LOG_HANDLE*    g_log     = nullptr;
static CONFIG_HANDLE* g_config  = nullptr;
static HWND           g_host    = nullptr;
static HWND           g_overlay = nullptr;
static HWND           g_cover   = nullptr;
static WNDPROC        g_origProc = nullptr;

static std::unordered_map<int, int> g_pinByScene;
static int g_sceneId  = 0;
static int g_pinCount = 0;

static int* g_pStart = nullptr;
static volatile LONG g_locating     = 0;
static volatile LONG g_suppressPaint = 0;
static volatile LONG g_dirty        = 1;
static volatile LONG g_refreshing   = 0;
static bool g_remapping = false;

static int   g_lastV      = -1;
static ULONGLONG g_lastRefresh = 0;

static int       g_userV     = 0;
static ULONGLONG g_holdUntil = 0;
static const ULONGLONG kHoldMs = 400;
static ULONGLONG g_coverUntil = 0;
static bool      g_menuRemap = false;
static int       g_menuRemapV = 0;
static ULONGLONG g_menuRemapUntil = 0;
static const ULONGLONG kMenuRemapMs = 4000;
static const ULONGLONG kCoverMs = 150;

static const UINT_PTR kTimerId = 0x4C500001;
static const UINT     kTimerMs = 33;

struct WatchedState {
    int frameStart = -1;
    int frameNum   = -1;
    int frame      = -1;
    int layerNum   = -1;
};
static WatchedState g_watch;

static void LogF(const wchar_t* fmt, ...) {
    wchar_t buf[1024];
    va_list ap; va_start(ap, fmt);
    _vsnwprintf_s(buf, _TRUNCATE, fmt, ap);
    va_end(ap);
    if (g_log) g_log->log(g_log, buf);
#ifdef LP_DEBUG_LOG
    static wchar_t logPath[MAX_PATH] = {};
    if (!logPath[0]) {
        wchar_t dir[MAX_PATH] = {};
        if (GetTempPathW(MAX_PATH, dir))
            _snwprintf_s(logPath, _TRUNCATE, L"%sLayerPinning.log", dir);
    }
    if (logPath[0]) {
        FILE* f = nullptr;
        if (_wfopen_s(&f, logPath, L"a+, ccs=UTF-8") == 0 && f) {
            fwprintf(f, L"%s\n", buf);
            fclose(f);
        }
    }
#endif
}

static size_t ScanBlockSafe(int* base, size_t count, int wanted, int** out, size_t outCap) {
    size_t n = 0;
    __try {
        for (size_t i = 0; i < count; i++) {
            if (base[i] != wanted) continue;
            if (n >= outCap) break;
            out[n++] = &base[i];
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return n;
}
static bool ReadIntSafe(const int* p, int* out) {
    __try { *out = *p; return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool WriteIntSafe(int* p, int v) {
    __try { *p = v; return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

struct Geometry {
    bool valid = false;
    RECT area = {};
    int  rowHeight = 0;
    int  row0Top = 0;
    int  contentRight = 0;
    int  displayLayerNum = 0;
};
static Geometry g_geo;

static void ProbeGeometry(EDIT_SECTION* edit) {
    if (!g_host || !IsWindowVisible(g_host) || IsIconic(g_host)) return;
    RECT wr = {}; GetClientRect(g_host, &wr);
    POINT org = { 0, 0 }; ClientToScreen(g_host, &org);
    const int W = wr.right, H = wr.bottom;
    if (W <= 0 || H <= 0) return;

    auto layerAt = [&](int x, int y) -> int {
        int layer = -1, frame = -1;
        if (!edit->pos_to_layer_frame(org.x + x, org.y + y, &layer, &frame)) return -1;
        return layer;
    };
    auto hit = [&](int x, int y) { return layerAt(x, y) >= 0; };

    int hx = -1, hy = -1;
    for (int y = 0; y < H && hx < 0; y += 24)
        for (int x = 0; x < W; x += 24)
            if (hit(x, y)) { hx = x; hy = y; break; }
    if (hx < 0) { g_geo.valid = false; return; }

    int x0 = hx; while (x0 > 0     && hit(x0 - 1, hy)) x0--;
    int x1 = hx; while (x1 < W - 1 && hit(x1 + 1, hy)) x1++;
    int y0 = hy; while (y0 > 0     && hit(hx, y0 - 1)) y0--;
    int y1 = hy; while (y1 < H - 1 && hit(hx, y1 + 1)) y1++;

    int b1 = -1, b2 = -1, prev = layerAt(hx, y0);
    for (int y = y0; y <= y1; y++) {
        int cur = layerAt(hx, y);
        if (cur == prev) continue;
        prev = cur;
        if (b1 < 0) b1 = y; else { b2 = y; break; }
    }
    if (b1 < 0 || b2 < 0) { g_geo.valid = false; return; }

    RECT area = { x0, y0, x1 + 1, y1 + 1 };
    if (area.right  > W) area.right  = W;
    if (area.bottom > H) area.bottom = H;

    int rowHeight = b2 - b1;
    int sbLogical  = g_config ? g_config->get_layout_size(g_config, "ScrollBarSize") : 0;
    int rowLogical = g_config ? g_config->get_layout_size(g_config, "LayerHeight") : 0;
    double scale = (rowLogical > 0) ? (double)rowHeight / rowLogical : 1.0;
    int contentRight = area.right - (int)(sbLogical * scale + 0.5);
    if (contentRight <= area.left) contentRight = area.right;

    EDIT_INFO info = {}; g_edit->get_edit_info(&info, sizeof(info));

    g_geo.area = area;
    g_geo.rowHeight = rowHeight;
    g_geo.row0Top = b1 - rowHeight;
    g_geo.contentRight = contentRight;
    g_geo.displayLayerNum = info.display_layer_num;
    g_geo.valid = rowHeight > 0 && g_geo.row0Top >= 0;

    LogF(L"geometry: area=(%d,%d)-(%d,%d) rowHeight=%d row0Top=%d contentRight=%d layers=%d",
         area.left, area.top, area.right, area.bottom,
         rowHeight, g_geo.row0Top, contentRight, info.display_layer_num);
}

static void RequestProbeGeometry() {
    if (g_edit) g_edit->call_edit_section([](EDIT_SECTION* e) { ProbeGeometry(e); });
}

static bool LayerAreaRect(RECT* out) {
    if (!g_geo.valid) return false;
    RECT cli = {}; GetClientRect(g_host, &cli);
    RECT rc = g_geo.area;
    if (rc.right  > cli.right)  rc.right  = cli.right;
    if (rc.bottom > cli.bottom) rc.bottom = cli.bottom;
    if (rc.right <= rc.left || rc.bottom <= rc.top) return false;
    *out = rc;
    return true;
}

static int VisiblePinRows() {
    if (g_pinCount <= 0) return 0;
    int rows = g_pinCount;
    if (g_geo.displayLayerNum > 1 && rows > g_geo.displayLayerNum - 1)
        rows = g_geo.displayLayerNum - 1;
    return rows > 0 ? rows : 0;
}

static bool PinnedStripRect(RECT* out) {
    const int rows = VisiblePinRows();
    if (!g_geo.valid || rows <= 0) return false;
    RECT cli = {}; GetClientRect(g_host, &cli);
    RECT rc = { g_geo.area.left, g_geo.row0Top,
                g_geo.contentRight, g_geo.row0Top + rows * g_geo.rowHeight };
    if (rc.right  > cli.right)  rc.right  = cli.right;
    if (rc.bottom > cli.bottom) rc.bottom = cli.bottom;
    if (rc.right <= rc.left || rc.bottom <= rc.top) return false;
    *out = rc;
    return true;
}

static void ScanFull(int wanted, std::vector<int*>& out) {
    out.clear();
    NT_TIB* tib = (NT_TIB*)NtCurrentTeb();
    BYTE* stackLo = (BYTE*)tib->StackLimit;
    BYTE* stackHi = (BYTE*)tib->StackBase;

    SYSTEM_INFO si; GetSystemInfo(&si);
    BYTE* p   = (BYTE*)si.lpMinimumApplicationAddress;
    BYTE* end = (BYTE*)si.lpMaximumApplicationAddress;
    MEMORY_BASIC_INFORMATION mbi;
    std::vector<int*> tmp(1 << 16);
    while (p < end && VirtualQuery(p, &mbi, sizeof(mbi))) {
        BYTE* next = (BYTE*)mbi.BaseAddress + mbi.RegionSize;
        bool rw = mbi.State == MEM_COMMIT &&
                  (mbi.Protect == PAGE_READWRITE || mbi.Protect == PAGE_WRITECOPY);
        bool ownStack = ((BYTE*)mbi.BaseAddress < stackHi) && (next > stackLo);
        if (rw && !ownStack && mbi.RegionSize <= (32u << 20)) {
            size_t found = ScanBlockSafe((int*)mbi.BaseAddress, mbi.RegionSize / sizeof(int),
                                         wanted, tmp.data(), tmp.size());
            size_t take = found < tmp.size() ? found : tmp.size();
            for (size_t i = 0; i < take; i++) out.push_back(tmp[i]);
        }
        if (out.size() > 1000000) break;
        if (next <= p) break;
        p = next;
    }
}

static void FilterCands(int wanted, std::vector<int*>& cands) {
    std::vector<int*> keep;
    keep.reserve(cands.size());
    for (int* a : cands) { int v; if (ReadIntSafe(a, &v) && v == wanted) keep.push_back(a); }
    cands.swap(keep);
}

static int g_setRequest = 0;
static int SetStartThroughSdk(int value) {
    g_setRequest = value;
    g_edit->call_edit_section([](EDIT_SECTION* e) {
        e->set_display_layer_frame(g_setRequest, e->info->display_frame_start);
    });
    EDIT_INFO chk = {}; g_edit->get_edit_info(&chk, sizeof(chk));
    return chk.display_layer_start;
}
static int ReadStartThroughSdk() {
    EDIT_INFO info = {}; g_edit->get_edit_info(&info, sizeof(info));
    return info.display_layer_start;
}
static bool StartPointerLooksValid() {
    if (!g_pStart) return false;
    int v;
    return ReadIntSafe(g_pStart, &v) && v == ReadStartThroughSdk();
}

static bool LocateStartPointer() {
    if (!g_edit) return false;
    if (InterlockedCompareExchange(&g_locating, 1, 0) != 0) return false;

    InterlockedExchange(&g_suppressPaint, 1);
    const int original = ReadStartThroughSdk();
    bool ok = false;

    const int maxScroll = SetStartThroughSdk(1 << 20);
    if (maxScroll >= 1) {
        std::vector<int> probes;
        if (maxScroll >= 3) {
            for (int p : { maxScroll, maxScroll / 2, 1, maxScroll / 3, 2, maxScroll - 1 }) {
                bool dup = false;
                for (int q : probes) if (q == p) { dup = true; break; }
                if (!dup && p >= 0 && p <= maxScroll) probes.push_back(p);
            }
        } else {
            for (int i = 0; i < 14; i++) probes.push_back((i % 2) ? 0 : maxScroll);
        }

        std::vector<int*> cands;
        bool first = true;
        for (int p : probes) {
            if (SetStartThroughSdk(p) != p) continue;
            if (first) { ScanFull(p, cands); first = false; }
            else FilterCands(p, cands);
            if (!first && cands.size() <= 8) break;
        }

        const int before = ReadStartThroughSdk();
        const int probe = (before == 1) ? 0 : 1;
        for (int* addr : cands) {
            int save;
            if (!ReadIntSafe(addr, &save)) continue;
            if (!WriteIntSafe(addr, probe)) continue;
            const bool match = (ReadStartThroughSdk() == probe);
            WriteIntSafe(addr, save);
            if (match) { g_pStart = addr; ok = true; break; }
        }
        if (ok)
            LogF(L"レイヤー固定: 表示開始レイヤー番号の位置を特定しました "
                 L"(maxScroll=%d candidates=%zu)", maxScroll, cands.size());
        else if (g_log)
            g_log->warn(g_log, L"レイヤー固定: 表示開始レイヤー番号を特定できませんでした。"
                               L"固定は無効のままになります。");
    } else {
        static bool reported = false;
        if (!reported) {
            reported = true;
            LogF(L"レイヤー固定: まだ縦スクロール出来ないので位置の特定を保留します。"
                 L"レイヤーが増えたら自動でやり直します。");
        }
    }

    if (g_pStart) WriteIntSafe(g_pStart, original);
    if (ReadStartThroughSdk() != original) SetStartThroughSdk(original);

    InterlockedExchange(&g_suppressPaint, 0);
    if (g_host) InvalidateRect(g_host, nullptr, FALSE);
    InterlockedExchange(&g_dirty, 1);
    InterlockedExchange(&g_locating, 0);
    return ok;
}

static ULONGLONG g_lastLocateTry = 0;
static void EnsureStartPointerAsync() {
    if (g_locating) return;
    if (g_pStart && StartPointerLooksValid()) return;
    const ULONGLONG now = GetTickCount64();
    if (g_lastLocateTry && now - g_lastLocateTry < 1000) return;
    g_lastLocateTry = now;
    g_pStart = nullptr;
    CreateThread(nullptr, 0, [](LPVOID) -> DWORD { LocateStartPointer(); return 0; },
                 nullptr, 0, nullptr);
}

struct PixelCache {
    HDC dc = nullptr; HBITMAP bmp = nullptr; HGDIOBJ old = nullptr;
    int w = 0, h = 0;
    bool Ensure(HDC ref, int cw, int ch) {
        if (cw <= 0 || ch <= 0) return false;
        if (dc && w == cw && h == ch) return true;
        Release();
        dc = CreateCompatibleDC(ref);
        if (!dc) return false;
        bmp = CreateCompatibleBitmap(ref, cw, ch);
        if (!bmp) { DeleteDC(dc); dc = nullptr; return false; }
        old = SelectObject(dc, bmp);
        w = cw; h = ch;
        return true;
    }
    void Release() {
        if (dc) { if (old) SelectObject(dc, old); DeleteDC(dc); dc = nullptr; }
        if (bmp) { DeleteObject(bmp); bmp = nullptr; }
        old = nullptr; w = h = 0;
    }
};
static PixelCache g_stripCache;
static PixelCache g_lowerCache;
static bool g_stripValid = false;

static unsigned long long g_stripHash = 0;
static int  g_idleStreak = 0;
static bool g_stripChanged = false;
static std::vector<BYTE> g_hashBuf;

static unsigned long long HashStrip() {
    if (!g_stripCache.dc || !g_stripCache.bmp || !g_stripCache.old) return 0;
    const int w = g_stripCache.w, h = g_stripCache.h;
    BITMAPINFO bi = {}; bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    g_hashBuf.resize((size_t)w * h * 4);

    SelectObject(g_stripCache.dc, g_stripCache.old);
    int got = GetDIBits(g_stripCache.dc, g_stripCache.bmp, 0, h,
                        g_hashBuf.data(), &bi, DIB_RGB_COLORS);
    SelectObject(g_stripCache.dc, g_stripCache.bmp);
    if (!got) return 0;

    unsigned long long v = 1469598103934665603ULL;
    for (size_t i = 0; i < g_hashBuf.size(); i += 8) { v ^= g_hashBuf[i]; v *= 1099511628211ULL; }
    return v;
}

static bool PinningActive() {
    return g_pinCount > 0 && g_geo.valid && g_pStart != nullptr;
}

static void UpdateOverlayPlacement(int v);
static void UncoverLowerRegion();
static void EndMenuRemap();

static void WaitForHostToFinishDrawing(const RECT& rc) {
    const int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;
    HDC wdc = GetDC(g_host);
    if (!wdc) return;
    HDC mdc = CreateCompatibleDC(wdc);
    HBITMAP bmp = CreateCompatibleBitmap(wdc, w, h);
    HGDIOBJ old = SelectObject(mdc, bmp);
    const int probeLines = 8;
    const int startLine = (h > probeLines) ? (h - probeLines) / 2 : 0;
    const int lines = (h < probeLines) ? h : probeLines;
    BITMAPINFO bi = {}; bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = h;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    std::vector<BYTE> prev((size_t)w * lines * 4), cur(prev.size());

    const ULONGLONG deadline = GetTickCount64() + 12;
    int stable = 0;
    bool first = true;
    while (GetTickCount64() < deadline && stable < 2) {
        BitBlt(mdc, 0, 0, w, h, wdc, rc.left, rc.top, SRCCOPY);
        SelectObject(mdc, old);
        GetDIBits(mdc, bmp, startLine, lines, cur.data(), &bi, DIB_RGB_COLORS);
        SelectObject(mdc, bmp);
        if (!first && memcmp(cur.data(), prev.data(), cur.size()) == 0) stable++;
        else stable = 0;
        prev.swap(cur);
        first = false;
        if (stable < 2) Sleep(1);
    }
    SelectObject(mdc, old);
    DeleteObject(bmp); DeleteDC(mdc); ReleaseDC(g_host, wdc);
}

static bool RefreshStrip() {
    if (!PinningActive()) return false;
    if (InterlockedCompareExchange(&g_refreshing, 1, 0) != 0) return false;
#ifdef LP_DEBUG_LOG
    LARGE_INTEGER t_begin; QueryPerformanceCounter(&t_begin);
#endif

    bool ok = false;
    g_stripChanged = false;
    RECT area, strip;
    int v = 0;
    if (LayerAreaRect(&area) && PinnedStripRect(&strip) && ReadIntSafe(g_pStart, &v)) {
        const int sw = strip.right - strip.left, sh = strip.bottom - strip.top;
        HDC wdc = GetDC(g_host);
        if (wdc) {
            const bool needSwap = (v != 0);
            RECT lower = { area.left, strip.bottom, area.right, area.bottom };
            const int lw = lower.right - lower.left, lh = lower.bottom - lower.top;
            bool covered = false;

            if (needSwap && g_cover && lw > 0 && lh > 0 &&
                g_lowerCache.Ensure(wdc, lw, lh) &&
                BitBlt(g_lowerCache.dc, 0, 0, lw, lh, wdc, lower.left, lower.top, SRCCOPY)) {
                SetWindowPos(g_cover, HWND_TOP, lower.left, lower.top, lw, lh,
                             SWP_NOACTIVATE | SWP_NOOWNERZORDER);
                ShowWindow(g_cover, SW_SHOWNOACTIVATE);
                UpdateWindow(g_cover);
                covered = true;
            }

            const bool hideOverlay = needSwap && g_overlay && IsWindowVisible(g_overlay);
            if (hideOverlay) ShowWindow(g_overlay, SW_HIDE);

            if (needSwap) {
                WriteIntSafe(g_pStart, 0);
                RedrawWindow(g_host, &strip, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
                WaitForHostToFinishDrawing(strip);
            }

            if (g_stripCache.Ensure(wdc, sw, sh))
                ok = BitBlt(g_stripCache.dc, 0, 0, sw, sh, wdc, strip.left, strip.top, SRCCOPY) != 0;

            if (ok) {
                const unsigned long long hash = HashStrip();
                if (g_stripValid && hash == g_stripHash) {
                    if (g_idleStreak < 8) g_idleStreak++;
                } else {
                    g_stripHash = hash;
                    g_idleStreak = 0;
                    g_stripChanged = true;
                }
                g_stripValid = true;
            }

            if (needSwap) {
                WriteIntSafe(g_pStart, v);
                if (covered) BitBlt(wdc, lower.left, lower.top, lw, lh, g_lowerCache.dc, 0, 0, SRCCOPY);
                else RedrawWindow(g_host, &area, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
            }
            ReleaseDC(g_host, wdc);

            if (hideOverlay) {
                InvalidateRect(g_overlay, nullptr, FALSE);
                ShowWindow(g_overlay, SW_SHOWNOACTIVATE);
                UpdateWindow(g_overlay);
            } else {
                UpdateOverlayPlacement(v);
                if (g_stripChanged && g_overlay && IsWindowVisible(g_overlay)) {
                    InvalidateRect(g_overlay, nullptr, FALSE);
                    UpdateWindow(g_overlay);
                }
            }
            if (covered) ShowWindow(g_cover, SW_HIDE);
        }
    }
#ifdef LP_DEBUG_LOG
    {
        static int n = 0;
        LARGE_INTEGER f, t1; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t1);
        if (v > 0 && ++n <= 200)
            LogF(L"RefreshStrip ok=%d v=%d took %.2f ms",
                 (int)ok, v, (double)(t1.QuadPart - t_begin.QuadPart) * 1000.0 / f.QuadPart);
    }
#endif
    g_lastRefresh = GetTickCount64();
    InterlockedExchange(&g_refreshing, 0);
    return ok;
}

static LRESULT CALLBACK OverlayProc(HWND h, UINT m, WPARAM wp, LPARAM lp);
static LRESULT CALLBACK CoverProc(HWND h, UINT m, WPARAM wp, LPARAM lp);

static void EnsureOverlay() {
    if (g_overlay || !g_host) return;
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = { sizeof(wc) };
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor   = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpfnWndProc   = OverlayProc;
        wc.lpszClassName = L"LayerPinningStrip";
        RegisterClassExW(&wc);
        wc.lpfnWndProc   = CoverProc;
        wc.lpszClassName = L"LayerPinningCover";
        RegisterClassExW(&wc);
        registered = true;
    }
    g_overlay = CreateWindowExW(WS_EX_NOACTIVATE, L"LayerPinningStrip", L"",
                                WS_CHILD, 0, 0, 1, 1,
                                g_host, nullptr, GetModuleHandleW(nullptr), nullptr);
    g_cover   = CreateWindowExW(WS_EX_NOACTIVATE, L"LayerPinningCover", L"",
                                WS_CHILD, 0, 0, 1, 1,
                                g_host, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (g_overlay) SetTimer(g_overlay, kTimerId, kTimerMs, nullptr);
    LogF(L"overlay=%p cover=%p", (void*)g_overlay, (void*)g_cover);
}

static LRESULT CALLBACK CoverProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
        case WM_NCHITTEST:  return HTTRANSPARENT;
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
            RECT rc; GetClientRect(h, &rc);
#ifdef LP_DEBUG_COVER
            HBRUSH br = CreateSolidBrush(RGB(255, 0, 255));
            FillRect(dc, &rc, br); DeleteObject(br);
#else
            if (g_lowerCache.dc)
                BitBlt(dc, 0, 0, rc.right, rc.bottom, g_lowerCache.dc, 0, 0, SRCCOPY);
#endif
            EndPaint(h, &ps);
            return 0;
        }
    }
    return DefWindowProcW(h, m, wp, lp);
}

static bool OverlayShouldShow(int v) {
    return PinningActive() && g_stripValid && v > 0;
}

static void UpdateOverlayPlacement(int v) {
    if (!g_overlay) return;
    RECT strip;
    if (!OverlayShouldShow(v) || !PinnedStripRect(&strip)) {
        if (IsWindowVisible(g_overlay)) ShowWindow(g_overlay, SW_HIDE);
        return;
    }
    RECT cur = {}; GetWindowRect(g_overlay, &cur);
    POINT tl = { strip.left, strip.top }; ClientToScreen(g_host, &tl);
    int w = strip.right - strip.left, h = strip.bottom - strip.top;
    if (cur.left != tl.x || cur.top != tl.y ||
        (cur.right - cur.left) != w || (cur.bottom - cur.top) != h) {
        SetWindowPos(g_overlay, HWND_TOP, strip.left, strip.top, w, h,
                     SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    }
    if (!IsWindowVisible(g_overlay)) ShowWindow(g_overlay, SW_SHOWNOACTIVATE);
}

static void OnTick() {
    if (g_menuRemap && GetTickCount64() >= g_menuRemapUntil) EndMenuRemap();
    if (g_remapping) return;
    if (g_pinCount <= 0) {
        if (g_overlay && IsWindowVisible(g_overlay)) ShowWindow(g_overlay, SW_HIDE);
        return;
    }
    if (!g_pStart) {
        if (g_overlay && IsWindowVisible(g_overlay)) ShowWindow(g_overlay, SW_HIDE);
        EDIT_INFO probe = {};
        g_edit->get_edit_info(&probe, sizeof(probe));
        if (probe.layer_max + 1 > probe.display_layer_num) EnsureStartPointerAsync();
        return;
    }
    if (!PinningActive()) {
        if (g_overlay && IsWindowVisible(g_overlay)) ShowWindow(g_overlay, SW_HIDE);
        return;
    }
    int v = 0;
    if (!ReadIntSafe(g_pStart, &v)) return;

    if (g_coverUntil && GetTickCount64() >= g_coverUntil) {
        g_coverUntil = 0;
        UncoverLowerRegion();
    }

    if (g_holdUntil) {
        if (GetTickCount64() >= g_holdUntil) {
            g_holdUntil = 0;
        } else if (v != g_userV) {
#ifdef LP_DEBUG_LOG
            LogF(L"  hold: host moved %d -> restoring %d", v, g_userV);
#endif
            WriteIntSafe(g_pStart, g_userV);
            v = g_userV;
            RECT area;
            if (LayerAreaRect(&area)) RedrawWindow(g_host, &area, nullptr, RDW_INVALIDATE);
        }
    }

    EDIT_INFO info = {};
    g_edit->get_edit_info(&info, sizeof(info));
    if (info.display_frame_start != g_watch.frameStart ||
        info.frame               != g_watch.frame      ||
        info.display_layer_num   != g_watch.layerNum) {
#ifdef LP_DEBUG_LOG
        LogF(L"  watch changed: frameStart %d->%d frame %d->%d layerNum %d->%d",
             g_watch.frameStart, info.display_frame_start,
             g_watch.frame, info.frame,
             g_watch.layerNum, info.display_layer_num);
#endif
        g_watch.frameStart = info.display_frame_start;
        g_watch.frame      = info.frame;
        if (info.display_layer_num != g_watch.layerNum) {
            g_watch.layerNum = info.display_layer_num;
            g_geo.displayLayerNum = info.display_layer_num;
            g_stripValid = false;
        }
        InterlockedExchange(&g_dirty, 1);
    }

    bool dirty = InterlockedExchange(&g_dirty, 0) != 0;

    if (dirty && g_stripValid) {
        const ULONGLONG minGap = (ULONGLONG)kTimerMs * (1 + g_idleStreak);
        if (GetTickCount64() - g_lastRefresh < minGap) {
            InterlockedExchange(&g_dirty, 1);
            dirty = false;
        }
    }
    if (!g_stripValid) dirty = true;

    if (dirty && !g_locating && !g_coverUntil) RefreshStrip();
    else if (dirty) InterlockedExchange(&g_dirty, 1);
    if (v != g_lastV) g_lastV = v;
    UpdateOverlayPlacement(v);
}

static LRESULT CALLBACK OverlayProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
        case WM_NCHITTEST:
            return HTTRANSPARENT;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
            RECT rc; GetClientRect(h, &rc);
            if (g_stripValid && g_stripCache.dc)
                BitBlt(dc, 0, 0, rc.right, rc.bottom, g_stripCache.dc, 0, 0, SRCCOPY);
            EndPaint(h, &ps);
            return 0;
        }
        case WM_TIMER:
            if (wp == kTimerId) { OnTick(); return 0; }
            break;
    }
    return DefWindowProcW(h, m, wp, lp);
}

static bool CoverLowerRegion() {
    if (!g_cover || !g_geo.valid) return false;
    RECT area, strip;
    if (!LayerAreaRect(&area) || !PinnedStripRect(&strip)) return false;
    RECT lower = { area.left, strip.bottom, area.right, area.bottom };
    const int w = lower.right - lower.left, h = lower.bottom - lower.top;
    if (w <= 0 || h <= 0) return false;

    HDC wdc = GetDC(g_host);
    if (!wdc) return false;
    bool ok = g_lowerCache.Ensure(wdc, w, h) &&
              BitBlt(g_lowerCache.dc, 0, 0, w, h, wdc, lower.left, lower.top, SRCCOPY) != 0;
    ReleaseDC(g_host, wdc);
    if (!ok) return false;

    SetWindowPos(g_cover, HWND_TOP, lower.left, lower.top, w, h,
                 SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    ShowWindow(g_cover, SW_SHOWNOACTIVATE);
    UpdateWindow(g_cover);
#ifdef LP_DEBUG_LOG
    LogF(L"  COVER on (%d,%d %dx%d) visible=%d", lower.left, lower.top, w, h,
         (int)IsWindowVisible(g_cover));
#endif
    return true;
}

static void UncoverLowerRegion() {
    if (!g_cover) return;
    RECT area, strip;
    if (LayerAreaRect(&area) && PinnedStripRect(&strip) && g_lowerCache.dc) {
        RECT lower = { area.left, strip.bottom, area.right, area.bottom };
        const int w = lower.right - lower.left, h = lower.bottom - lower.top;
        if (w == g_lowerCache.w && h == g_lowerCache.h) {
            HDC wdc = GetDC(g_host);
            if (wdc) {
                BitBlt(wdc, lower.left, lower.top, w, h, g_lowerCache.dc, 0, 0, SRCCOPY);
                ReleaseDC(g_host, wdc);
            }
        }
    }
    ShowWindow(g_cover, SW_HIDE);
    if (LayerAreaRect(&area)) RedrawWindow(g_host, &area, nullptr, RDW_INVALIDATE);
}

static bool PointInPinnedStrip(POINT ptClient) {
    RECT strip;
    if (!PinnedStripRect(&strip)) return false;
    return PtInRect(&strip, ptClient) != 0;
}
static bool CursorInPinnedStrip(LPARAM lp) {
    POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
    return PointInPinnedStrip(pt);
}

static LRESULT HandleContextMenu(HWND hwnd, WPARAM wp, LPARAM lp, bool* handled) {
    *handled = false;
    if (!PinningActive()) return 0;
    int v;
    if (!ReadIntSafe(g_pStart, &v) || v <= 0) return 0;

    POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
    if (lp == (LPARAM)-1 || (pt.x == -1 && pt.y == -1)) GetCursorPos(&pt);
    ScreenToClient(hwnd, &pt);
    if (!PointInPinnedStrip(pt)) return 0;

    CoverLowerRegion();
    g_remapping = true;
    g_menuRemap = true;
    g_menuRemapV = v;
    g_menuRemapUntil = GetTickCount64() + kMenuRemapMs;
    WriteIntSafe(g_pStart, 0);
    LRESULT r = CallWindowProcW(g_origProc, hwnd, WM_CONTEXTMENU, wp, lp);
    *handled = true;
    return r;
}

static void EndMenuRemap() {
    if (!g_menuRemap) return;
    g_menuRemap = false;
    g_menuRemapUntil = 0;
    g_remapping = false;
    WriteIntSafe(g_pStart, g_menuRemapV);
    g_userV = g_menuRemapV;
    g_holdUntil = GetTickCount64() + kHoldMs;
    g_coverUntil = GetTickCount64() + kCoverMs;
    InterlockedExchange(&g_dirty, 1);
}

static LRESULT HandleMouse(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, bool* handled) {
    *handled = false;
#ifdef LP_NO_REMAP
    return 0;
#endif
    if (!PinningActive()) return 0;
    int v;
    if (!ReadIntSafe(g_pStart, &v)) return 0;
    if (v <= 0) return 0;
    if (!CursorInPinnedStrip(lp)) return 0;

#ifdef LP_DEBUG_LOG
    if (msg != WM_MOUSEMOVE)
        LogF(L"  REMAP msg=0x%04X at (%d,%d) v=%d", msg,
             GET_X_LPARAM(lp), GET_Y_LPARAM(lp), v);
#endif

    const bool button = (msg != WM_MOUSEMOVE);
    bool covered = button && CoverLowerRegion();

    g_remapping = true;
    WriteIntSafe(g_pStart, 0);
    LRESULT r = CallWindowProcW(g_origProc, hwnd, msg, wp, lp);
    WriteIntSafe(g_pStart, v);
    g_remapping = false;

    g_userV = v;
    g_holdUntil = GetTickCount64() + kHoldMs;

    if (covered) g_coverUntil = GetTickCount64() + kCoverMs;
    *handled = true;
    return r;
}

static LRESULT CALLBACK HostProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_PAINT:
            if (g_suppressPaint) {
                PAINTSTRUCT ps; BeginPaint(hwnd, &ps); EndPaint(hwnd, &ps);
                return 0;
            }
            break;

        case WM_SIZE:
        case WM_DPICHANGED: {
            LRESULT r = CallWindowProcW(g_origProc, hwnd, msg, wp, lp);
            g_stripCache.Release();
            g_lowerCache.Release();
            g_stripValid = false;
            InterlockedExchange(&g_dirty, 1);
            RequestProbeGeometry();
            return r;
        }

        case WM_MOUSEMOVE:
        case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN: case WM_RBUTTONUP:
        case WM_MBUTTONDOWN: case WM_MBUTTONUP:
        case WM_CAPTURECHANGED: {
            const bool onScrollBar = g_geo.valid && GET_X_LPARAM(lp) >= g_geo.contentRight;
            const bool dragging = (wp & (MK_LBUTTON | MK_RBUTTON | MK_MBUTTON)) != 0;
            if (!onScrollBar && (msg != WM_MOUSEMOVE || dragging || CursorInPinnedStrip(lp))) {
                if (msg != WM_MOUSEMOVE) g_idleStreak = 0;
                InterlockedExchange(&g_dirty, 1);
            }
            bool handled = false;
            LRESULT r = HandleMouse(hwnd, msg, wp, lp, &handled);
            if (handled) return r;
            break;
        }

        case WM_CONTEXTMENU: {
            bool handled = false;
            LRESULT r = HandleContextMenu(hwnd, wp, lp, &handled);
            InterlockedExchange(&g_dirty, 1);
            if (handled) return r;
            break;
        }

        case WM_COMMAND: {
            LRESULT r = CallWindowProcW(g_origProc, hwnd, msg, wp, lp);
            EndMenuRemap();
            InterlockedExchange(&g_dirty, 1);
            return r;
        }

        case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
        case WM_KEYDOWN: case WM_KEYUP: case WM_CHAR:
            if (msg == WM_MOUSEWHEEL || msg == WM_MOUSEHWHEEL ||
                msg == WM_KEYDOWN || msg == WM_KEYUP)
                g_holdUntil = 0;
            InterlockedExchange(&g_dirty, 1);
            break;

        case WM_DESTROY:
            if (g_overlay) { KillTimer(g_overlay, kTimerId); DestroyWindow(g_overlay); g_overlay = nullptr; }
            if (g_cover) { DestroyWindow(g_cover); g_cover = nullptr; }
            g_stripCache.Release();
            g_lowerCache.Release();
            break;
    }

    LRESULT r = CallWindowProcW(g_origProc, hwnd, msg, wp, lp);

    if (g_holdUntil && g_pStart) {
        if (GetTickCount64() >= g_holdUntil) {
            g_holdUntil = 0;
        } else {
            int cur;
            if (ReadIntSafe(g_pStart, &cur) && cur != g_userV) WriteIntSafe(g_pStart, g_userV);
        }
    }
    return r;
}

static const wchar_t* MenuTextPin() {
    return g_config ? g_config->translate(g_config, L"レイヤーを固定") : L"レイヤーを固定";
}
static const wchar_t* MenuTextUnpin() {
    return g_config ? g_config->translate(g_config, L"レイヤーの固定を解除") : L"レイヤーの固定を解除";
}

static void** FindIatSlot(HMODULE mod, const char* dll, const char* fn) {
    auto base = (BYTE*)mod;
    auto dos = (IMAGE_DOS_HEADER*)base;
    if (!dos || dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    auto nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;
    auto dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return nullptr;
    for (auto imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); imp->Name; ++imp) {
        if (_stricmp((const char*)(base + imp->Name), dll) != 0) continue;
        auto oft = (IMAGE_THUNK_DATA*)(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk
                                                                       : imp->FirstThunk));
        auto ft = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
        for (; oft->u1.AddressOfData; ++oft, ++ft) {
            if (IMAGE_SNAP_BY_ORDINAL(oft->u1.Ordinal)) continue;
            auto ibn = (IMAGE_IMPORT_BY_NAME*)(base + oft->u1.AddressOfData);
            if (strcmp((const char*)ibn->Name, fn) == 0) return (void**)&ft->u1.Function;
        }
    }
    return nullptr;
}
static bool PatchSlot(void** slot, void* fn, void** old) {
    DWORD prot = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &prot)) return false;
    if (old) *old = *slot;
    *slot = fn;
    VirtualProtect(slot, sizeof(void*), prot, &prot);
    return true;
}

static bool FindItemByText(HMENU menu, const wchar_t* text, int depth,
                           HMENU* outMenu, int* outPos) {
    if (depth > 3) return false;
    int n = GetMenuItemCount(menu);
    for (int i = 0; i < n; i++) {
        wchar_t txt[256] = {};
        GetMenuStringW(menu, i, txt, 255, MF_BYPOSITION);
        if (wcscmp(txt, text) == 0) { *outMenu = menu; *outPos = i; return true; }
        MENUITEMINFOW mii = { sizeof(mii) }; mii.fMask = MIIM_SUBMENU;
        if (GetMenuItemInfoW(menu, i, TRUE, &mii) && mii.hSubMenu)
            if (FindItemByText(mii.hSubMenu, text, depth + 1, outMenu, outPos)) return true;
    }
    return false;
}

static void RemoveIfEmptySubmenu(HMENU root, HMENU sub) {
    if (!sub || GetMenuItemCount(sub) != 0) return;
    HMENU parent = nullptr; int ppos = -1;
    std::vector<HMENU> stack{ root };
    while (!stack.empty() && !parent) {
        HMENU m = stack.back(); stack.pop_back();
        int n = GetMenuItemCount(m);
        for (int i = 0; i < n; i++) {
            MENUITEMINFOW mii = { sizeof(mii) }; mii.fMask = MIIM_SUBMENU;
            if (!GetMenuItemInfoW(m, i, TRUE, &mii) || !mii.hSubMenu) continue;
            if (mii.hSubMenu == sub) { parent = m; ppos = i; break; }
            stack.push_back(mii.hSubMenu);
        }
    }
    if (parent) RemoveMenu(parent, ppos, MF_BYPOSITION);
}

static int LayerUnderCursor(int screenX, int screenY) {
    if (!g_host) return -1;
    if (!g_geo.valid) {
        if (!g_edit) return -1;
        EDIT_INFO info = {};
        g_edit->get_edit_info(&info, sizeof(info));
        return info.layer;
    }
    POINT pt = { screenX, screenY };
    ScreenToClient(g_host, &pt);
    if (pt.x < g_geo.area.left || pt.x >= g_geo.contentRight) return -1;
    if (pt.y < g_geo.row0Top || pt.y >= g_geo.area.bottom) return -1;

    int row = (pt.y - g_geo.row0Top) / g_geo.rowHeight;
    if (g_pinCount > 0 && row < g_pinCount) return row;

    int start = 0;
    if (g_pStart) { if (!ReadIntSafe(g_pStart, &start)) start = ReadStartThroughSdk(); }
    else start = ReadStartThroughSdk();
    return start + row;
}

static bool IsMenuEligible(int layer) {
    if (layer < 0 || !g_edit) return false;
    if (layer < g_pinCount) return true;
    if (layer != g_pinCount) return false;
    EDIT_INFO info = {}; g_edit->get_edit_info(&info, sizeof(info));
    return layer + 1 < info.display_layer_num;
}

typedef BOOL (WINAPI* TrackPopupMenu_t)(HMENU, UINT, int, int, int, HWND, const RECT*);
static TrackPopupMenu_t g_origTPM = nullptr;
static void** g_tpmSlot = nullptr;

static int g_menuLayer = -1;

static BOOL WINAPI Hook_TrackPopupMenu(HMENU menu, UINT flags, int x, int y,
                                       int reserved, HWND hwnd, const RECT* rc) {
    HMENU pinOwner = nullptr, unpinOwner = nullptr;
    int pinPos = -1, unpinPos = -1;
    const bool hasPin   = menu && FindItemByText(menu, MenuTextPin(), 0, &pinOwner, &pinPos);
    const bool hasUnpin = menu && FindItemByText(menu, MenuTextUnpin(), 0, &unpinOwner, &unpinPos);

    if (hasPin || hasUnpin) {
        const int layer = LayerUnderCursor(x, y);
        g_menuLayer = layer;
        const bool eligible = IsMenuEligible(layer);
        const bool pinned   = eligible && layer < g_pinCount;
        const bool dropPin   = !eligible || pinned;
        const bool dropUnpin = !eligible || !pinned;

        HMENU sub = nullptr;
        struct Item { bool drop; HMENU owner; int pos; };
        Item items[2] = { { dropPin, pinOwner, pinPos }, { dropUnpin, unpinOwner, unpinPos } };
        if (items[0].owner == items[1].owner && items[0].pos < items[1].pos) {
            Item t = items[0]; items[0] = items[1]; items[1] = t;
        }
        for (const Item& it : items) {
            if (!it.drop || !it.owner || it.pos < 0) continue;
            RemoveMenu(it.owner, it.pos, MF_BYPOSITION);
            sub = it.owner;
        }
        RemoveIfEmptySubmenu(menu, sub);
    }
#ifdef LP_DEBUG_LOG
    {
        int cur = -1; if (g_pStart) ReadIntSafe(g_pStart, &cur);
        EDIT_INFO info = {}; if (g_edit) g_edit->get_edit_info(&info, sizeof(info));
        LogF(L"  MENU OPEN: pStart=%d hostLayer=%d ourLayer=%d",
             cur, info.layer, g_menuLayer);
    }
#endif
    BOOL r = g_origTPM(menu, flags, x, y, reserved, hwnd, rc);
#ifdef LP_DEBUG_LOG
    {
        int cur = -1; if (g_pStart) ReadIntSafe(g_pStart, &cur);
        EDIT_INFO info = {}; if (g_edit) g_edit->get_edit_info(&info, sizeof(info));
        LogF(L"  MENU CLOSED: pStart=%d hostLayer=%d", cur, info.layer);
    }
#endif
    return r;
}

static void ApplyPinChange(EDIT_SECTION* edit, bool wantPin) {
    if (!g_geo.valid) ProbeGeometry(edit);

    int layer = g_menuLayer;
    if (layer < 0) {
        POINT pt; GetCursorPos(&pt);
        layer = LayerUnderCursor(pt.x, pt.y);
    }
    if (layer < 0) {
        int l = -1, f = -1;
        if (edit->get_mouse_layer_frame(&l, &f)) layer = l;
    }
    if (layer < 0) return;

    const int before = g_pinCount;
    if (wantPin) {
        if (layer != g_pinCount) return;
        g_pinCount = layer + 1;
    } else {
        if (layer >= g_pinCount) return;
        g_pinCount = layer;
    }

    g_pinByScene[g_sceneId] = g_pinCount;
    g_stripValid = false;
    InterlockedExchange(&g_dirty, 1);
    LogF(L"pinCount %d -> %d (layer %d, scene %d)", before, g_pinCount, layer, g_sceneId);

    EnsureOverlay();
    if (g_pinCount > 0) EnsureStartPointerAsync();
    else if (g_overlay && IsWindowVisible(g_overlay)) ShowWindow(g_overlay, SW_HIDE);

    if (g_host) InvalidateRect(g_host, nullptr, FALSE);
}

static void OnPinMenu(EDIT_SECTION* edit)   { ApplyPinChange(edit, true); }
static void OnUnpinMenu(EDIT_SECTION* edit) { ApplyPinChange(edit, false); }

static void OnChangeScene(EDIT_SECTION* edit) {
    g_sceneId = edit->info->scene_id;
    auto it = g_pinByScene.find(g_sceneId);
    g_pinCount = (it == g_pinByScene.end()) ? 0 : it->second;
    g_pStart = nullptr;
    g_stripValid = false;
    InterlockedExchange(&g_dirty, 1);
    ProbeGeometry(edit);
    if (g_pinCount > 0) EnsureStartPointerAsync();
}

static void OnAnyEvent(void*) {
    g_idleStreak = 0;
    InterlockedExchange(&g_dirty, 1);
    if (!g_edit) return;
    EDIT_INFO info = {};
    g_edit->get_edit_info(&info, sizeof(info));
    if (info.display_layer_num != g_geo.displayLayerNum) {
        g_geo.displayLayerNum = info.display_layer_num;
        g_stripValid = false;
        RequestProbeGeometry();
    }
}

COMMON_PLUGIN_TABLE common_plugin_table = {
    L"レイヤー固定",
    L"レイヤー固定 version 1.00",
};

EXTERN_C __declspec(dllexport) COMMON_PLUGIN_TABLE* GetCommonPluginTable(void) {
    return &common_plugin_table;
}
EXTERN_C __declspec(dllexport) DWORD RequiredVersion() { return 2010000; }
EXTERN_C __declspec(dllexport) void InitializeLogger(LOG_HANDLE* h) { g_log = h; }
EXTERN_C __declspec(dllexport) void InitializeConfig(CONFIG_HANDLE* h) { g_config = h; }
EXTERN_C __declspec(dllexport) bool InitializePlugin(DWORD) { return true; }

EXTERN_C __declspec(dllexport) void RegisterPlugin(HOST_APP_TABLE* host) {
    g_edit = host->create_edit_handle();
    g_host = g_edit->get_host_app_window();

    host->register_layer_menu(MenuTextPin(), OnPinMenu);
    host->register_layer_menu(MenuTextUnpin(), OnUnpinMenu);
    host->register_change_scene_handler(OnChangeScene);
    host->register_event_listener(EVENT_TYPE::UPDATE_OBJECT,       nullptr, OnAnyEvent);
    host->register_event_listener(EVENT_TYPE::CHANGE_EDIT_FRAME,   nullptr, OnAnyEvent);
    host->register_event_listener(EVENT_TYPE::CHANGE_FOCUS_OBJECT, nullptr, OnAnyEvent);
    host->register_event_listener(EVENT_TYPE::CHANGE_EDIT_SCENE,   nullptr, OnAnyEvent);

    if (g_host)
        g_origProc = (WNDPROC)SetWindowLongPtrW(g_host, GWLP_WNDPROC, (LONG_PTR)HostProc);

    g_tpmSlot = FindIatSlot(GetModuleHandleW(nullptr), "USER32.dll", "TrackPopupMenu");
    if (g_tpmSlot) PatchSlot(g_tpmSlot, (void*)&Hook_TrackPopupMenu, (void**)&g_origTPM);

    LogF(L"LayerPinning registered (host=%p tpmSlot=%p)", (void*)g_host, (void*)g_tpmSlot);
}

EXTERN_C __declspec(dllexport) void UninitializePlugin() {
    if (g_tpmSlot && g_origTPM) PatchSlot(g_tpmSlot, (void*)g_origTPM, nullptr);
    if (g_overlay) { KillTimer(g_overlay, kTimerId); DestroyWindow(g_overlay); g_overlay = nullptr; }
    if (g_host && g_origProc) SetWindowLongPtrW(g_host, GWLP_WNDPROC, (LONG_PTR)g_origProc);
    g_stripCache.Release();
    g_lowerCache.Release();
}

BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID) { return TRUE; }
