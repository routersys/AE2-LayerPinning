#include "InputRemap.h"

#include <windowsx.h>

#include "CoverWindow.h"
#include "DrawHook.h"
#include "HostContext.h"
#include "LayerGeometry.h"
#include "Log.h"
#include "PinningStatus.h"
#include "RefreshRequest.h"
#include "ScrollAnchor.h"

namespace lp {

static bool g_remapping = false;

static int       g_userV     = 0;
static ULONGLONG g_holdUntil = 0;
static const ULONGLONG kHoldMs = 400;
static ULONGLONG g_coverUntil = 0;
static bool      g_menuRemap = false;
static bool      g_menuCovered = false;
static int       g_menuRemapV = 0;
static ULONGLONG g_menuRemapUntil = 0;
static const ULONGLONG kMenuRemapMs = 4000;
static const ULONGLONG kCoverMs = 150;

bool Remapping() { return g_remapping; }
bool CoverHeld() { return g_coverUntil != 0; }
void CancelScrollHold() { g_holdUntil = 0; }

LRESULT HandleContextMenu(HWND hwnd, WPARAM wp, LPARAM lp, bool* handled) {
    *handled = false;
    if (!PinningActive()) return 0;
    int v;
    if (!ReadStart(&v) || v <= 0) return 0;

    POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
    if (lp == (LPARAM)-1 || (pt.x == -1 && pt.y == -1)) GetCursorPos(&pt);
    ScreenToClient(hwnd, &pt);
    if (!PointInPinnedStrip(pt)) return 0;

    g_menuCovered = Cover().CoverLowerRegion();
    g_remapping = true;
    g_menuRemap = true;
    g_menuRemapV = v;
    g_menuRemapUntil = GetTickCount64() + kMenuRemapMs;
    WriteStart(0);
    LRESULT r = CallHostWindowProc(hwnd, WM_CONTEXTMENU, wp, lp);
    *handled = true;
    return r;
}

void EndMenuRemap() {
    if (!g_menuRemap) return;
    g_menuRemap = false;
    g_menuRemapUntil = 0;
    g_remapping = false;
    WriteStart(g_menuRemapV);
    g_userV = g_menuRemapV;
    g_holdUntil = GetTickCount64() + kHoldMs;
    if (g_menuCovered) g_coverUntil = GetTickCount64() + kCoverMs;
    g_menuCovered = false;
    RequestRefresh();
}

void ExpireMenuRemap() {
    if (g_menuRemap && GetTickCount64() >= g_menuRemapUntil) EndMenuRemap();
}

void ExpireCover() {
    if (g_coverUntil && GetTickCount64() >= g_coverUntil) {
        g_coverUntil = 0;
        Cover().UncoverLowerRegion();
    }
}

LRESULT HandleMouse(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, bool* handled) {
    *handled = false;
#ifdef LP_NO_REMAP
    return 0;
#endif
    if (!PinningActive()) return 0;
    int v;
    if (!ReadStart(&v)) return 0;
    if (v <= 0) return 0;
    if (!CursorInPinnedStrip(lp)) return 0;

#ifdef LP_DEBUG_LOG
    if (msg != WM_MOUSEMOVE)
        LogF(L"  REMAP msg=0x%04X at (%d,%d) v=%d", msg,
             GET_X_LPARAM(lp), GET_Y_LPARAM(lp), v);
#endif

    const bool button = (msg != WM_MOUSEMOVE);
    bool covered = button && !DrawHookActive() && Cover().CoverLowerRegion();

    g_remapping = true;
    SetRemapBase(v);
    WriteStart(0);
    LRESULT r = CallHostWindowProc(hwnd, msg, wp, lp);
    WriteStart(v);
    ClearRemapBase();
    g_remapping = false;

    g_userV = v;
    g_holdUntil = GetTickCount64() + kHoldMs;

    if (covered) g_coverUntil = GetTickCount64() + kCoverMs;
    RequestRefresh();
    *handled = true;
    return r;
}

int ApplyScrollHold(int v) {
    if (g_holdUntil) {
        if (GetTickCount64() >= g_holdUntil) {
            g_holdUntil = 0;
        } else if (v != g_userV) {
#ifdef LP_DEBUG_LOG
            LogF(L"  hold: host moved %d -> restoring %d", v, g_userV);
#endif
            WriteStart(g_userV);
            v = g_userV;
            RECT area;
            if (LayerAreaRect(&area)) RedrawWindow(HostWindow(), &area, nullptr, RDW_INVALIDATE);
        }
    }
    return v;
}

void ReassertScrollHold() {
    if (g_holdUntil && HasStartPointer()) {
        if (GetTickCount64() >= g_holdUntil) {
            g_holdUntil = 0;
        } else {
            int cur;
            if (ReadStart(&cur) && cur != g_userV) WriteStart(g_userV);
        }
    }
}

}
