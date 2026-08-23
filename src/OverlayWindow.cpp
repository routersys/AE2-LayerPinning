#include "OverlayWindow.h"

#include "HostContext.h"
#include "LayerGeometry.h"

namespace lp {

static const UINT_PTR kTimerId = 0x4C500001;

static OverlayWindow g_overlay;

OverlayWindow& Overlay() { return g_overlay; }

unsigned long long StripImage::Hash() {
    if (!cache_.dc || !cache_.bmp || !cache_.old) return 0;
    const int w = cache_.w, h = cache_.h;
    BITMAPINFO bi = {}; bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    hashBuf_.resize((size_t)w * h * 4);

    SelectObject(cache_.dc, cache_.old);
    int got = GetDIBits(cache_.dc, cache_.bmp, 0, h,
                        hashBuf_.data(), &bi, DIB_RGB_COLORS);
    SelectObject(cache_.dc, cache_.bmp);
    if (!got) return 0;

    unsigned long long v = 1469598103934665603ULL;
    for (size_t i = 0; i < hashBuf_.size(); i += 8) { v ^= hashBuf_[i]; v *= 1099511628211ULL; }
    return v;
}

bool StripImage::Refresh(HDC hostDc, const RECT& strip) {
    changed_ = false;
    const int sw = strip.right - strip.left, sh = strip.bottom - strip.top;
    bool ok = false;
    if (cache_.Ensure(hostDc, sw, sh))
        ok = BitBlt(cache_.dc, 0, 0, sw, sh, hostDc, strip.left, strip.top, SRCCOPY) != 0;

    if (ok) {
        const unsigned long long hash = Hash();
        if (valid_ && hash == hash_) {
            if (idleStreak_ < 8) idleStreak_++;
        } else {
            hash_ = hash;
            idleStreak_ = 0;
            changed_ = true;
        }
        valid_ = true;
    }
    return ok;
}

void StripImage::Paint(HDC dc, const RECT& rc) {
    if (valid_ && cache_.dc)
        BitBlt(dc, 0, 0, rc.right, rc.bottom, cache_.dc, 0, 0, SRCCOPY);
}

void StripImage::Release() {
    cache_.Release();
}

void OverlayWindow::Create(HWND parent) {
    if (hwnd_ || !parent) return;
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = { sizeof(wc) };
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor   = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpfnWndProc   = Proc;
        wc.lpszClassName = L"LayerPinningStrip";
        RegisterClassExW(&wc);
        registered = true;
    }
    hwnd_ = CreateWindowExW(WS_EX_NOACTIVATE, L"LayerPinningStrip", L"",
                            WS_CHILD, 0, 0, 1, 1,
                            parent, nullptr, GetModuleHandleW(nullptr), nullptr);
}

void OverlayWindow::StartTicking(void (*proc)()) {
    tick_ = proc;
    if (hwnd_) SetTimer(hwnd_, kTimerId, kTickIntervalMs, nullptr);
}

void OverlayWindow::Destroy() {
    if (hwnd_) { KillTimer(hwnd_, kTimerId); DestroyWindow(hwnd_); hwnd_ = nullptr; }
}

bool OverlayWindow::Visible() const {
    return hwnd_ && IsWindowVisible(hwnd_);
}

void OverlayWindow::Hide() {
    if (Visible()) ShowWindow(hwnd_, SW_HIDE);
}

void OverlayWindow::ShowRepainted() {
    if (!hwnd_) return;
    InvalidateRect(hwnd_, nullptr, FALSE);
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    UpdateWindow(hwnd_);
}

void OverlayWindow::RepaintIfVisible() {
    if (!Visible()) return;
    InvalidateRect(hwnd_, nullptr, FALSE);
    UpdateWindow(hwnd_);
}

void OverlayWindow::UpdatePlacement(bool shouldShow) {
    if (!hwnd_) return;
    RECT strip;
    if (!shouldShow || !PinnedStripRect(&strip)) {
        Hide();
        return;
    }
    RECT cur = {}; GetWindowRect(hwnd_, &cur);
    POINT tl = { strip.left, strip.top }; ClientToScreen(HostWindow(), &tl);
    int w = strip.right - strip.left, h = strip.bottom - strip.top;
    if (cur.left != tl.x || cur.top != tl.y ||
        (cur.right - cur.left) != w || (cur.bottom - cur.top) != h) {
        SetWindowPos(hwnd_, HWND_TOP, strip.left, strip.top, w, h,
                     SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    }
    if (!IsWindowVisible(hwnd_)) ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
}

LRESULT CALLBACK OverlayWindow::Proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
        case WM_NCHITTEST:
            return HTTRANSPARENT;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
            RECT rc; GetClientRect(h, &rc);
            g_overlay.image_.Paint(dc, rc);
            EndPaint(h, &ps);
            return 0;
        }
        case WM_TIMER:
            if (wp == kTimerId) { if (g_overlay.tick_) g_overlay.tick_(); return 0; }
            break;
    }
    return DefWindowProcW(h, m, wp, lp);
}

}
