#include <windows.h>
#include <windowsx.h>
#include <vector>

#include "HostContext.h"
#include "LayerGeometry.h"
#include "Log.h"
#include "OverlayWindow.h"
#include "PinState.h"
#include "PixelCache.h"
#include "RefreshRequest.h"
#include "ScrollAnchor.h"

using namespace lp;

static HWND           g_cover   = nullptr;
static WNDPROC        g_origProc = nullptr;

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

struct WatchedState {
    int frameStart = -1;
    int frameNum   = -1;
    int frame      = -1;
    int layerNum   = -1;
};
static WatchedState g_watch;

static PixelCache g_lowerCache;
static bool PinningActive() {
    return PinCount() > 0 && GeometryValid() && HasStartPointer();
}

static bool OverlayShouldShow(int v);
static void OnTick();
static void UncoverLowerRegion();
static void EndMenuRemap();

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

static bool RefreshStrip() {
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

            const bool hideOverlay = needSwap && Overlay().Visible();
            if (hideOverlay) Overlay().Hide();

            if (needSwap) {
                WriteStart(0);
                RedrawWindow(HostWindow(), &strip, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
                WaitForHostToFinishDrawing(strip);
            }

            ok = Overlay().Image().Refresh(wdc, strip);

            if (needSwap) {
                WriteStart(v);
                if (covered) BitBlt(wdc, lower.left, lower.top, lw, lh, g_lowerCache.dc, 0, 0, SRCCOPY);
                else RedrawWindow(HostWindow(), &area, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
            }
            ReleaseDC(HostWindow(), wdc);

            if (hideOverlay) {
                Overlay().ShowRepainted();
            } else {
                Overlay().UpdatePlacement(OverlayShouldShow(v));
                if (Overlay().Image().Changed()) Overlay().RepaintIfVisible();
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

static LRESULT CALLBACK CoverProc(HWND h, UINT m, WPARAM wp, LPARAM lp);

static void EnsureCover() {
    if (g_cover || !HostWindow()) return;
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = { sizeof(wc) };
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor   = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpfnWndProc   = CoverProc;
        wc.lpszClassName = L"LayerPinningCover";
        RegisterClassExW(&wc);
        registered = true;
    }
    g_cover = CreateWindowExW(WS_EX_NOACTIVATE, L"LayerPinningCover", L"",
                              WS_CHILD, 0, 0, 1, 1,
                              HostWindow(), nullptr, GetModuleHandleW(nullptr), nullptr);
}

static void EnsureOverlay() {
    if (Overlay().Hwnd() || !HostWindow()) return;
    Overlay().Create(HostWindow());
    EnsureCover();
    Overlay().StartTicking(OnTick);
    LogF(L"overlay=%p cover=%p", (void*)Overlay().Hwnd(), (void*)g_cover);
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
    return PinningActive() && Overlay().Image().Valid() && v > 0;
}

static void OnTick() {
    if (g_menuRemap && GetTickCount64() >= g_menuRemapUntil) EndMenuRemap();
    if (g_remapping) return;
    if (PinCount() <= 0) {
        Overlay().Hide();
        return;
    }
    if (!HasStartPointer()) {
        Overlay().Hide();
        const EDIT_INFO probe = EditInfo();
        if (probe.layer_max + 1 > probe.display_layer_num) EnsureStartPointerAsync();
        return;
    }
    if (!PinningActive()) {
        Overlay().Hide();
        return;
    }
    int v = 0;
    if (!ReadStart(&v)) return;

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
            WriteStart(g_userV);
            v = g_userV;
            RECT area;
            if (LayerAreaRect(&area)) RedrawWindow(HostWindow(), &area, nullptr, RDW_INVALIDATE);
        }
    }

    const EDIT_INFO info = EditInfo();
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
            SetDisplayLayerNum(info.display_layer_num);
            Overlay().Image().Invalidate();
        }
        RequestRefresh();
    }

    bool dirty = TakeRefreshRequest();

    if (dirty && Overlay().Image().Valid()) {
        const ULONGLONG minGap = (ULONGLONG)kTickIntervalMs * (1 + Overlay().Image().IdleStreak());
        if (GetTickCount64() - g_lastRefresh < minGap) {
            RequestRefresh();
            dirty = false;
        }
    }
    if (!Overlay().Image().Valid()) dirty = true;

    if (dirty && !Locating() && !g_coverUntil) RefreshStrip();
    else if (dirty) RequestRefresh();
    if (v != g_lastV) g_lastV = v;
    Overlay().UpdatePlacement(OverlayShouldShow(v));
}

static bool CoverLowerRegion() {
    if (!g_cover || !GeometryValid()) return false;
    RECT lower;
    if (!LowerRegionRect(&lower)) return false;
    const int w = lower.right - lower.left, h = lower.bottom - lower.top;
    if (w <= 0 || h <= 0) return false;

    HDC wdc = GetDC(HostWindow());
    if (!wdc) return false;
    bool ok = g_lowerCache.Ensure(wdc, w, h) &&
              BitBlt(g_lowerCache.dc, 0, 0, w, h, wdc, lower.left, lower.top, SRCCOPY) != 0;
    ReleaseDC(HostWindow(), wdc);
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
    RECT lower;
    if (LowerRegionRect(&lower) && g_lowerCache.dc) {
        const int w = lower.right - lower.left, h = lower.bottom - lower.top;
        if (w == g_lowerCache.w && h == g_lowerCache.h) {
            HDC wdc = GetDC(HostWindow());
            if (wdc) {
                BitBlt(wdc, lower.left, lower.top, w, h, g_lowerCache.dc, 0, 0, SRCCOPY);
                ReleaseDC(HostWindow(), wdc);
            }
        }
    }
    ShowWindow(g_cover, SW_HIDE);
    RECT area;
    if (LayerAreaRect(&area)) RedrawWindow(HostWindow(), &area, nullptr, RDW_INVALIDATE);
}

static LRESULT HandleContextMenu(HWND hwnd, WPARAM wp, LPARAM lp, bool* handled) {
    *handled = false;
    if (!PinningActive()) return 0;
    int v;
    if (!ReadStart(&v) || v <= 0) return 0;

    POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
    if (lp == (LPARAM)-1 || (pt.x == -1 && pt.y == -1)) GetCursorPos(&pt);
    ScreenToClient(hwnd, &pt);
    if (!PointInPinnedStrip(pt)) return 0;

    CoverLowerRegion();
    g_remapping = true;
    g_menuRemap = true;
    g_menuRemapV = v;
    g_menuRemapUntil = GetTickCount64() + kMenuRemapMs;
    WriteStart(0);
    LRESULT r = CallWindowProcW(g_origProc, hwnd, WM_CONTEXTMENU, wp, lp);
    *handled = true;
    return r;
}

static void EndMenuRemap() {
    if (!g_menuRemap) return;
    g_menuRemap = false;
    g_menuRemapUntil = 0;
    g_remapping = false;
    WriteStart(g_menuRemapV);
    g_userV = g_menuRemapV;
    g_holdUntil = GetTickCount64() + kHoldMs;
    g_coverUntil = GetTickCount64() + kCoverMs;
    RequestRefresh();
}

static LRESULT HandleMouse(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, bool* handled) {
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
    bool covered = button && CoverLowerRegion();

    g_remapping = true;
    WriteStart(0);
    LRESULT r = CallWindowProcW(g_origProc, hwnd, msg, wp, lp);
    WriteStart(v);
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
            if (PaintSuppressed()) {
                PAINTSTRUCT ps; BeginPaint(hwnd, &ps); EndPaint(hwnd, &ps);
                return 0;
            }
            break;

        case WM_SIZE:
        case WM_DPICHANGED: {
            LRESULT r = CallWindowProcW(g_origProc, hwnd, msg, wp, lp);
            Overlay().Image().Release();
            g_lowerCache.Release();
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
            LRESULT r = CallWindowProcW(g_origProc, hwnd, msg, wp, lp);
            EndMenuRemap();
            RequestRefresh();
            return r;
        }

        case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
        case WM_KEYDOWN: case WM_KEYUP: case WM_CHAR:
            if (msg == WM_MOUSEWHEEL || msg == WM_MOUSEHWHEEL ||
                msg == WM_KEYDOWN || msg == WM_KEYUP)
                g_holdUntil = 0;
            RequestRefresh();
            break;

        case WM_DESTROY:
            Overlay().Destroy();
            if (g_cover) { DestroyWindow(g_cover); g_cover = nullptr; }
            Overlay().Image().Release();
            g_lowerCache.Release();
            break;
    }

    LRESULT r = CallWindowProcW(g_origProc, hwnd, msg, wp, lp);

    if (g_holdUntil && HasStartPointer()) {
        if (GetTickCount64() >= g_holdUntil) {
            g_holdUntil = 0;
        } else {
            int cur;
            if (ReadStart(&cur) && cur != g_userV) WriteStart(g_userV);
        }
    }
    return r;
}

static const wchar_t* MenuTextPin() {
    return Translate(L"レイヤーを固定");
}
static const wchar_t* MenuTextUnpin() {
    return Translate(L"レイヤーの固定を解除");
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
        const bool pinned   = eligible && layer < PinCount();
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
        int cur = -1; if (HasStartPointer()) ReadStart(&cur);
        const EDIT_INFO info = EditInfo();
        LogF(L"  MENU OPEN: pStart=%d hostLayer=%d ourLayer=%d",
             cur, info.layer, g_menuLayer);
    }
#endif
    BOOL r = g_origTPM(menu, flags, x, y, reserved, hwnd, rc);
#ifdef LP_DEBUG_LOG
    {
        int cur = -1; if (HasStartPointer()) ReadStart(&cur);
        const EDIT_INFO info = EditInfo();
        LogF(L"  MENU CLOSED: pStart=%d hostLayer=%d", cur, info.layer);
    }
#endif
    return r;
}

static void ApplyPinChange(EDIT_SECTION* edit, bool wantPin) {
    if (!GeometryValid()) ProbeGeometry(edit);

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

    const int before = PinCount();
    if (!ApplyPin(layer, wantPin)) return;

    Overlay().Image().Invalidate();
    RequestRefresh();
    LogF(L"pinCount %d -> %d (layer %d, scene %d)", before, PinCount(), layer, SceneId());

    EnsureOverlay();
    if (PinCount() > 0) EnsureStartPointerAsync();
    else Overlay().Hide();

    if (HostWindow()) InvalidateRect(HostWindow(), nullptr, FALSE);
}

static void OnPinMenu(EDIT_SECTION* edit)   { ApplyPinChange(edit, true); }
static void OnUnpinMenu(EDIT_SECTION* edit) { ApplyPinChange(edit, false); }

static void OnChangeScene(EDIT_SECTION* edit) {
    SelectScene(edit->info->scene_id);
    ResetStartPointer();
    Overlay().Image().Invalidate();
    RequestRefresh();
    ProbeGeometry(edit);
    if (PinCount() > 0) EnsureStartPointerAsync();
}

static void OnAnyEvent(void*) {
    Overlay().Image().ResetIdleStreak();
    RequestRefresh();
    if (!Edit()) return;
    const EDIT_INFO info = EditInfo();
    if (info.display_layer_num != DisplayLayerNum()) {
        SetDisplayLayerNum(info.display_layer_num);
        Overlay().Image().Invalidate();
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
EXTERN_C __declspec(dllexport) void InitializeLogger(LOG_HANDLE* h) { SetLogHandle(h); }
EXTERN_C __declspec(dllexport) void InitializeConfig(CONFIG_HANDLE* h) { SetConfigHandle(h); }
EXTERN_C __declspec(dllexport) bool InitializePlugin(DWORD) { return true; }

EXTERN_C __declspec(dllexport) void RegisterPlugin(HOST_APP_TABLE* host) {
    SetEditHandle(host->create_edit_handle());
    SetHostWindow(Edit()->get_host_app_window());

    host->register_layer_menu(MenuTextPin(), OnPinMenu);
    host->register_layer_menu(MenuTextUnpin(), OnUnpinMenu);
    host->register_change_scene_handler(OnChangeScene);
    host->register_event_listener(EVENT_TYPE::UPDATE_OBJECT,       nullptr, OnAnyEvent);
    host->register_event_listener(EVENT_TYPE::CHANGE_EDIT_FRAME,   nullptr, OnAnyEvent);
    host->register_event_listener(EVENT_TYPE::CHANGE_FOCUS_OBJECT, nullptr, OnAnyEvent);
    host->register_event_listener(EVENT_TYPE::CHANGE_EDIT_SCENE,   nullptr, OnAnyEvent);

    if (HostWindow())
        g_origProc = (WNDPROC)SetWindowLongPtrW(HostWindow(), GWLP_WNDPROC, (LONG_PTR)HostProc);

    g_tpmSlot = FindIatSlot(GetModuleHandleW(nullptr), "USER32.dll", "TrackPopupMenu");
    if (g_tpmSlot) PatchSlot(g_tpmSlot, (void*)&Hook_TrackPopupMenu, (void**)&g_origTPM);

    LogF(L"LayerPinning registered (host=%p tpmSlot=%p)", (void*)HostWindow(), (void*)g_tpmSlot);
}

EXTERN_C __declspec(dllexport) void UninitializePlugin() {
    if (g_tpmSlot && g_origTPM) PatchSlot(g_tpmSlot, (void*)g_origTPM, nullptr);
    Overlay().Destroy();
    if (HostWindow() && g_origProc) SetWindowLongPtrW(HostWindow(), GWLP_WNDPROC, (LONG_PTR)g_origProc);
    Overlay().Image().Release();
    g_lowerCache.Release();
}

BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID) { return TRUE; }
