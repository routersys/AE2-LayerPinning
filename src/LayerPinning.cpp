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
static int g_pinCount = 0;

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

    LogF(L"LayerPinning registered (host=%p)", (void*)g_host);
}

EXTERN_C __declspec(dllexport) void UninitializePlugin() {
}

BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID) { return TRUE; }
