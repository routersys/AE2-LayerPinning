#include "LayerGeometry.h"

#include <windowsx.h>

#include "Log.h"
#include "PinState.h"
#include "ScrollAnchor.h"

namespace lp {

struct Geometry {
    bool valid = false;
    RECT area = {};
    int  rowHeight = 0;
    int  row0Top = 0;
    int  contentRight = 0;
    int  displayLayerNum = 0;
};
static Geometry g_geo;

bool GeometryValid() { return g_geo.valid; }
int ContentRight() { return g_geo.contentRight; }
int DisplayLayerNum() { return g_geo.displayLayerNum; }
void SetDisplayLayerNum(int layerNum) { g_geo.displayLayerNum = layerNum; }

void ProbeGeometry(EDIT_SECTION* edit) {
    if (!HostWindow() || !IsWindowVisible(HostWindow()) || IsIconic(HostWindow())) return;
    RECT wr = {}; GetClientRect(HostWindow(), &wr);
    POINT org = { 0, 0 }; ClientToScreen(HostWindow(), &org);
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
    int sbLogical  = LayoutSize("ScrollBarSize");
    int rowLogical = LayoutSize("LayerHeight");
    double scale = (rowLogical > 0) ? (double)rowHeight / rowLogical : 1.0;
    int contentRight = area.right - (int)(sbLogical * scale + 0.5);
    if (contentRight <= area.left) contentRight = area.right;

    const EDIT_INFO info = EditInfo();

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

void RequestProbeGeometry() {
    CallEditSection([](EDIT_SECTION* e) { ProbeGeometry(e); });
}

bool LayerAreaRect(RECT* out) {
    if (!g_geo.valid) return false;
    RECT cli = {}; GetClientRect(HostWindow(), &cli);
    RECT rc = g_geo.area;
    if (rc.right  > cli.right)  rc.right  = cli.right;
    if (rc.bottom > cli.bottom) rc.bottom = cli.bottom;
    if (rc.right <= rc.left || rc.bottom <= rc.top) return false;
    *out = rc;
    return true;
}

static int VisiblePinRows() {
    if (PinCount() <= 0) return 0;
    int rows = PinCount();
    if (g_geo.displayLayerNum > 1 && rows > g_geo.displayLayerNum - 1)
        rows = g_geo.displayLayerNum - 1;
    return rows > 0 ? rows : 0;
}

bool PinnedStripRect(RECT* out) {
    const int rows = VisiblePinRows();
    if (!g_geo.valid || rows <= 0) return false;
    RECT cli = {}; GetClientRect(HostWindow(), &cli);
    RECT rc = { g_geo.area.left, g_geo.row0Top,
                g_geo.contentRight, g_geo.row0Top + rows * g_geo.rowHeight };
    if (rc.right  > cli.right)  rc.right  = cli.right;
    if (rc.bottom > cli.bottom) rc.bottom = cli.bottom;
    if (rc.right <= rc.left || rc.bottom <= rc.top) return false;
    *out = rc;
    return true;
}


RECT LowerRegionRect(const RECT& area, const RECT& strip) {
    return { area.left, strip.bottom, area.right, area.bottom };
}

bool LowerRegionRect(RECT* out) {
    RECT area, strip;
    if (!LayerAreaRect(&area) || !PinnedStripRect(&strip)) return false;
    *out = LowerRegionRect(area, strip);
    return true;
}

bool PointInPinnedStrip(POINT ptClient) {
    RECT strip;
    if (!PinnedStripRect(&strip)) return false;
    return PtInRect(&strip, ptClient) != 0;
}
bool CursorInPinnedStrip(LPARAM lp) {
    POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
    return PointInPinnedStrip(pt);
}

int LayerUnderCursor(int screenX, int screenY) {
    if (!HostWindow()) return -1;
    if (!g_geo.valid) {
        if (!Edit()) return -1;
        return EditInfo().layer;
    }
    POINT pt = { screenX, screenY };
    ScreenToClient(HostWindow(), &pt);
    if (pt.x < g_geo.area.left || pt.x >= g_geo.contentRight) return -1;
    if (pt.y < g_geo.row0Top || pt.y >= g_geo.area.bottom) return -1;

    int row = (pt.y - g_geo.row0Top) / g_geo.rowHeight;
    if (PinCount() > 0 && row < PinCount()) return row;

    int start = 0;
    if (HasStartPointer()) { if (!ReadStart(&start)) start = ReadStartThroughSdk(); }
    else start = ReadStartThroughSdk();
    return start + row;
}

}
