#include "CoverWindow.h"

#include "HostContext.h"
#include "LayerGeometry.h"
#include "Log.h"

namespace lp {

static CoverWindow g_cover;

CoverWindow& Cover() { return g_cover; }

void CoverWindow::Create(HWND parent) {
    if (hwnd_ || !parent) return;
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = { sizeof(wc) };
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor   = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpfnWndProc   = Proc;
        wc.lpszClassName = L"LayerPinningCover";
        RegisterClassExW(&wc);
        registered = true;
    }
    hwnd_ = CreateWindowExW(WS_EX_NOACTIVATE, L"LayerPinningCover", L"",
                            WS_CHILD, 0, 0, 1, 1,
                            parent, nullptr, GetModuleHandleW(nullptr), nullptr);
}

void CoverWindow::Destroy() {
    if (hwnd_) { DestroyWindow(hwnd_); hwnd_ = nullptr; }
}

bool CoverWindow::Capture(HDC hostDc, const RECT& lower) {
    const int w = lower.right - lower.left, h = lower.bottom - lower.top;
    return hwnd_ && w > 0 && h > 0 &&
           cache_.Ensure(hostDc, w, h) &&
           BitBlt(cache_.dc, 0, 0, w, h, hostDc, lower.left, lower.top, SRCCOPY) != 0;
}

void CoverWindow::Place(const RECT& lower) {
    SetWindowPos(hwnd_, HWND_TOP, lower.left, lower.top,
                 lower.right - lower.left, lower.bottom - lower.top,
                 SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    UpdateWindow(hwnd_);
}

void CoverWindow::Restore(HDC hostDc, const RECT& lower) {
    BitBlt(hostDc, lower.left, lower.top,
           lower.right - lower.left, lower.bottom - lower.top,
           cache_.dc, 0, 0, SRCCOPY);
}

void CoverWindow::Hide() {
    if (hwnd_) ShowWindow(hwnd_, SW_HIDE);
}

void CoverWindow::ReleaseCache() {
    cache_.Release();
}

bool CoverWindow::CoverLowerRegion() {
    if (!hwnd_ || !GeometryValid()) return false;
    RECT lower;
    if (!LowerRegionRect(&lower)) return false;
    const int w = lower.right - lower.left, h = lower.bottom - lower.top;
    if (w <= 0 || h <= 0) return false;

    HDC wdc = GetDC(HostWindow());
    if (!wdc) return false;
    const bool ok = Capture(wdc, lower);
    ReleaseDC(HostWindow(), wdc);
    if (!ok) return false;

    Place(lower);
#ifdef LP_DEBUG_LOG
    LogF(L"  COVER on (%d,%d %dx%d) visible=%d", lower.left, lower.top, w, h,
         (int)IsWindowVisible(hwnd_));
#endif
    return true;
}

void CoverWindow::UncoverLowerRegion() {
    if (!hwnd_) return;
    RECT lower;
    if (LowerRegionRect(&lower) && cache_.dc) {
        const int w = lower.right - lower.left, h = lower.bottom - lower.top;
        if (w == cache_.w && h == cache_.h) {
            HDC wdc = GetDC(HostWindow());
            if (wdc) {
                Restore(wdc, lower);
                ReleaseDC(HostWindow(), wdc);
            }
        }
    }
    ShowWindow(hwnd_, SW_HIDE);
    RECT area;
    if (LayerAreaRect(&area)) RedrawWindow(HostWindow(), &area, nullptr, RDW_INVALIDATE);
}

LRESULT CALLBACK CoverWindow::Proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
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
            if (g_cover.cache_.dc)
                BitBlt(dc, 0, 0, rc.right, rc.bottom, g_cover.cache_.dc, 0, 0, SRCCOPY);
#endif
            EndPaint(h, &ps);
            return 0;
        }
    }
    return DefWindowProcW(h, m, wp, lp);
}

}
