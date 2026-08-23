#include "PinningController.h"

#include "CoverWindow.h"
#include "DrawHook.h"
#include "HostContext.h"
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

namespace lp {

static int   g_lastV      = -1;

struct WatchedState {
    int frameStart = -1;
    int frameNum   = -1;
    int frame      = -1;
    int layerNum   = -1;
};
static WatchedState g_watch;

static void OnTick();

static void WakeTick() {
    Overlay().RequestTick();
}

static void EnsureOverlay() {
    if (Overlay().Hwnd() || !HostWindow()) return;
    Overlay().Create(HostWindow());
    Cover().Create(HostWindow());
    Overlay().StartTicking(OnTick);
    SetRefreshWaker(WakeTick);
    LogF(L"overlay=%p cover=%p", (void*)Overlay().Hwnd(), (void*)Cover().Hwnd());
}

static void OnTick() {
    ExpireMenuRemap();
    if (Remapping()) return;
    KeepDrawHookArmed();
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
    if (DrawHookActive()) {
        ExpireCover();
        Overlay().Hide();
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

    const bool urgent = TakeUrgentRefreshRequest();
    bool dirty = TakeRefreshRequest() || urgent;

    if (dirty && !urgent && Overlay().Image().Valid()) {
        const double minGap = (double)kTickIntervalMs * Overlay().Image().IdleStreak();
        if (MillisecondsSinceLastRefresh() < minGap) {
            DeferRefreshRequest();
            dirty = false;
        }
    }
    if (!Overlay().Image().Valid()) dirty = true;

    if (dirty && !Locating() && !CoverHeld()) RefreshStrip();
    else if (dirty) DeferRefreshRequest();
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
    KeepDrawHookArmed();

    if (HostWindow()) InvalidateRect(HostWindow(), nullptr, FALSE);
}

void DestroyOverlay() {
    Overlay().Destroy();
}

void ReleaseCaches() {
    Overlay().Image().Release();
    Cover().ReleaseCache();
}

void OnPinMenu(EDIT_SECTION* edit)   { ApplyPinChange(edit, true); }
void OnUnpinMenu(EDIT_SECTION* edit) { ApplyPinChange(edit, false); }

void OnChangeScene(EDIT_SECTION* edit) {
    SelectScene(edit->info->scene_id);
    ReleaseDrawHook();
    ResetStartPointer();
    Overlay().Image().Invalidate();
    RequestRefresh();
    ProbeGeometry(edit);
    if (PinCount() > 0) EnsureStartPointerAsync();
}

void OnAnyEvent(void*) {
    Overlay().Image().ResetIdleStreak();
    RequestUrgentRefresh();
    if (!Edit()) return;
    const EDIT_INFO info = EditInfo();
    if (info.display_layer_num != DisplayLayerNum()) {
        SetDisplayLayerNum(info.display_layer_num);
        Overlay().Image().Invalidate();
        RequestProbeGeometry();
    }
}

}
