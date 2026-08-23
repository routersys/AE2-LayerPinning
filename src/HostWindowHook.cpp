#include "HostWindowHook.h"

#include <windows.h>
#include <windowsx.h>

#include "CoverWindow.h"
#include "HostContext.h"
#include "InputRemap.h"
#include "LayerGeometry.h"
#include "OverlayWindow.h"
#include "RefreshRequest.h"
#include "ScrollAnchor.h"

namespace lp {

static LRESULT CALLBACK HostProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_PAINT:
            if (PaintSuppressed()) {
                PAINTSTRUCT ps; BeginPaint(hwnd, &ps); EndPaint(hwnd, &ps);
                return 0;
            }
            break;

        case WM_SIZE:
        case WM_DPICHANGED: {
            LRESULT r = CallHostWindowProc(hwnd, msg, wp, lp);
            Overlay().Image().Release();
            Cover().ReleaseCache();
            Overlay().Image().Invalidate();
            RequestRefresh();
            RequestProbeGeometry();
            return r;
        }

        case WM_MOUSEMOVE:
        case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN: case WM_RBUTTONUP:
        case WM_MBUTTONDOWN: case WM_MBUTTONUP:
        case WM_CAPTURECHANGED: {
            const bool onScrollBar = GeometryValid() && GET_X_LPARAM(lp) >= ContentRight();
            const bool dragging = (wp & (MK_LBUTTON | MK_RBUTTON | MK_MBUTTON)) != 0;
            if (!onScrollBar && (msg != WM_MOUSEMOVE || dragging || CursorInPinnedStrip(lp))) {
                if (msg != WM_MOUSEMOVE) Overlay().Image().ResetIdleStreak();
                RequestRefresh();
            }
            bool handled = false;
            LRESULT r = HandleMouse(hwnd, msg, wp, lp, &handled);
            if (handled) return r;
            break;
        }

        case WM_CONTEXTMENU: {
            bool handled = false;
            LRESULT r = HandleContextMenu(hwnd, wp, lp, &handled);
            RequestRefresh();
            if (handled) return r;
            break;
        }

        case WM_COMMAND: {
            LRESULT r = CallHostWindowProc(hwnd, msg, wp, lp);
            EndMenuRemap();
            RequestRefresh();
            return r;
        }

        case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
        case WM_KEYDOWN: case WM_KEYUP: case WM_CHAR:
            if (msg == WM_MOUSEWHEEL || msg == WM_MOUSEHWHEEL ||
                msg == WM_KEYDOWN || msg == WM_KEYUP)
                CancelScrollHold();
            RequestRefresh();
            break;

        case WM_DESTROY:
            Overlay().Destroy();
            Cover().Destroy();
            Overlay().Image().Release();
            Cover().ReleaseCache();
            break;
    }

    LRESULT r = CallHostWindowProc(hwnd, msg, wp, lp);

    ReassertScrollHold();
    return r;
}

void InstallHostWindowHook() {
    if (HostWindow())
        SetHostWindowProc((WNDPROC)SetWindowLongPtrW(HostWindow(), GWLP_WNDPROC, (LONG_PTR)HostProc));
}

void UninstallHostWindowHook() {
    if (HostWindow() && HostWindowProc())
        SetWindowLongPtrW(HostWindow(), GWLP_WNDPROC, (LONG_PTR)HostWindowProc());
}

}
