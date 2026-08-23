#include "StripCapture.h"

#include <vector>

#include "CoverWindow.h"
#include "HostContext.h"
#include "LayerGeometry.h"
#include "Log.h"
#include "OverlayWindow.h"
#include "PinningStatus.h"
#include "ScrollAnchor.h"

namespace lp {

static volatile LONG g_refreshing   = 0;
static ULONGLONG g_lastRefresh = 0;

ULONGLONG LastRefreshTick() { return g_lastRefresh; }

static void ForceHostRepaint(const RECT& rc) {
    RedrawWindow(HostWindow(), &rc, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
}

static void WaitForHostToFinishDrawing(const RECT& rc) {
    const int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;
    HDC wdc = GetDC(HostWindow());
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
    DeleteObject(bmp); DeleteDC(mdc); ReleaseDC(HostWindow(), wdc);
}

bool RefreshStrip() {
    if (!PinningActive()) return false;
    if (InterlockedCompareExchange(&g_refreshing, 1, 0) != 0) return false;
#ifdef LP_DEBUG_LOG
    LARGE_INTEGER t_begin; QueryPerformanceCounter(&t_begin);
#endif

    bool ok = false;
    RECT area, strip;
    int v = 0;
    if (LayerAreaRect(&area) && PinnedStripRect(&strip) && ReadStart(&v)) {
        HDC wdc = GetDC(HostWindow());
        if (wdc) {
            const bool needSwap = (v != 0);
            const RECT lower = LowerRegionRect(area, strip);
            bool covered = false;

            if (needSwap && Cover().Capture(wdc, lower)) {
                Cover().Place(lower);
                covered = true;
            }

            const bool hideOverlay = needSwap && Overlay().Visible();
            if (hideOverlay) Overlay().Hide();

            if (needSwap) {
                WriteStart(0);
                ForceHostRepaint(strip);
                WaitForHostToFinishDrawing(strip);
            }

            ok = Overlay().Image().Refresh(wdc, strip);

            if (needSwap) {
                WriteStart(v);
                if (covered) Cover().Restore(wdc, lower);
                else ForceHostRepaint(area);
            }
            ReleaseDC(HostWindow(), wdc);

            if (hideOverlay) {
                Overlay().ShowRepainted();
            } else {
                Overlay().UpdatePlacement(OverlayShouldShow(v));
                if (Overlay().Image().Changed()) Overlay().RepaintIfVisible();
            }
            if (covered) Cover().Hide();
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

}
