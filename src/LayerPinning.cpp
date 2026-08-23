#include <windows.h>

#include "CoverWindow.h"
#include "HostContext.h"
#include "HostWindowHook.h"
#include "InputRemap.h"
#include "LayerGeometry.h"
#include "LayerMenuHook.h"
#include "Log.h"
#include "OverlayWindow.h"
#include "PinState.h"
#include "PinningStatus.h"
#include "RefreshRequest.h"
#include "ScrollAnchor.h"
#include "StripCapture.h"

using namespace lp;

static int   g_lastV      = -1;

struct WatchedState {
    int frameStart = -1;
    int frameNum   = -1;
    int frame      = -1;
    int layerNum   = -1;
};
static WatchedState g_watch;

static void OnTick();

static void EnsureOverlay() {
    if (Overlay().Hwnd() || !HostWindow()) return;
    Overlay().Create(HostWindow());
    Cover().Create(HostWindow());
    Overlay().StartTicking(OnTick);
    LogF(L"overlay=%p cover=%p", (void*)Overlay().Hwnd(), (void*)Cover().Hwnd());
}

static void OnTick() {
    ExpireMenuRemap();
    if (Remapping()) return;
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

    ExpireCover();

    v = ApplyScrollHold(v);

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
        if (GetTickCount64() - LastRefreshTick() < minGap) {
            RequestRefresh();
            dirty = false;
        }
    }
    if (!Overlay().Image().Valid()) dirty = true;

    if (dirty && !Locating() && !CoverHeld()) RefreshStrip();
    else if (dirty) RequestRefresh();
    if (v != g_lastV) g_lastV = v;
    Overlay().UpdatePlacement(OverlayShouldShow(v));
}

static void ApplyPinChange(EDIT_SECTION* edit, bool wantPin) {
    if (!GeometryValid()) ProbeGeometry(edit);

    int layer = MenuTargetLayer();
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

    InstallHostWindowHook();

    void* tpmSlot = InstallLayerMenuHook();

    LogF(L"LayerPinning registered (host=%p tpmSlot=%p)", (void*)HostWindow(), tpmSlot);
}

EXTERN_C __declspec(dllexport) void UninitializePlugin() {
    UninstallLayerMenuHook();
    Overlay().Destroy();
    UninstallHostWindowHook();
    Overlay().Image().Release();
    Cover().ReleaseCache();
}

BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID) { return TRUE; }
