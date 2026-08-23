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

static int* g_pStart = nullptr;
static volatile LONG g_locating     = 0;
static volatile LONG g_suppressPaint = 0;
static volatile LONG g_dirty        = 1;

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

static size_t ScanBlockSafe(int* base, size_t count, int wanted, int** out, size_t outCap) {
    size_t n = 0;
    __try {
        for (size_t i = 0; i < count; i++) {
            if (base[i] != wanted) continue;
            if (n >= outCap) break;
            out[n++] = &base[i];
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return n;
}
static bool ReadIntSafe(const int* p, int* out) {
    __try { *out = *p; return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool WriteIntSafe(int* p, int v) {
    __try { *p = v; return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
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

static void ScanFull(int wanted, std::vector<int*>& out) {
    out.clear();
    NT_TIB* tib = (NT_TIB*)NtCurrentTeb();
    BYTE* stackLo = (BYTE*)tib->StackLimit;
    BYTE* stackHi = (BYTE*)tib->StackBase;

    SYSTEM_INFO si; GetSystemInfo(&si);
    BYTE* p   = (BYTE*)si.lpMinimumApplicationAddress;
    BYTE* end = (BYTE*)si.lpMaximumApplicationAddress;
    MEMORY_BASIC_INFORMATION mbi;
    std::vector<int*> tmp(1 << 16);
    while (p < end && VirtualQuery(p, &mbi, sizeof(mbi))) {
        BYTE* next = (BYTE*)mbi.BaseAddress + mbi.RegionSize;
        bool rw = mbi.State == MEM_COMMIT &&
                  (mbi.Protect == PAGE_READWRITE || mbi.Protect == PAGE_WRITECOPY);
        bool ownStack = ((BYTE*)mbi.BaseAddress < stackHi) && (next > stackLo);
        if (rw && !ownStack && mbi.RegionSize <= (32u << 20)) {
            size_t found = ScanBlockSafe((int*)mbi.BaseAddress, mbi.RegionSize / sizeof(int),
                                         wanted, tmp.data(), tmp.size());
            size_t take = found < tmp.size() ? found : tmp.size();
            for (size_t i = 0; i < take; i++) out.push_back(tmp[i]);
        }
        if (out.size() > 1000000) break;
        if (next <= p) break;
        p = next;
    }
}

static void FilterCands(int wanted, std::vector<int*>& cands) {
    std::vector<int*> keep;
    keep.reserve(cands.size());
    for (int* a : cands) { int v; if (ReadIntSafe(a, &v) && v == wanted) keep.push_back(a); }
    cands.swap(keep);
}

static int g_setRequest = 0;
static int SetStartThroughSdk(int value) {
    g_setRequest = value;
    g_edit->call_edit_section([](EDIT_SECTION* e) {
        e->set_display_layer_frame(g_setRequest, e->info->display_frame_start);
    });
    EDIT_INFO chk = {}; g_edit->get_edit_info(&chk, sizeof(chk));
    return chk.display_layer_start;
}
static int ReadStartThroughSdk() {
    EDIT_INFO info = {}; g_edit->get_edit_info(&info, sizeof(info));
    return info.display_layer_start;
}
static bool StartPointerLooksValid() {
    if (!g_pStart) return false;
    int v;
    return ReadIntSafe(g_pStart, &v) && v == ReadStartThroughSdk();
}

static bool LocateStartPointer() {
    if (!g_edit) return false;
    if (InterlockedCompareExchange(&g_locating, 1, 0) != 0) return false;

    InterlockedExchange(&g_suppressPaint, 1);
    const int original = ReadStartThroughSdk();
    bool ok = false;

    const int maxScroll = SetStartThroughSdk(1 << 20);
    if (maxScroll >= 1) {
        std::vector<int> probes;
        if (maxScroll >= 3) {
            for (int p : { maxScroll, maxScroll / 2, 1, maxScroll / 3, 2, maxScroll - 1 }) {
                bool dup = false;
                for (int q : probes) if (q == p) { dup = true; break; }
                if (!dup && p >= 0 && p <= maxScroll) probes.push_back(p);
            }
        } else {
            for (int i = 0; i < 14; i++) probes.push_back((i % 2) ? 0 : maxScroll);
        }

        std::vector<int*> cands;
        bool first = true;
        for (int p : probes) {
            if (SetStartThroughSdk(p) != p) continue;
            if (first) { ScanFull(p, cands); first = false; }
            else FilterCands(p, cands);
            if (!first && cands.size() <= 8) break;
        }

        const int before = ReadStartThroughSdk();
        const int probe = (before == 1) ? 0 : 1;
        for (int* addr : cands) {
            int save;
            if (!ReadIntSafe(addr, &save)) continue;
            if (!WriteIntSafe(addr, probe)) continue;
            const bool match = (ReadStartThroughSdk() == probe);
            WriteIntSafe(addr, save);
            if (match) { g_pStart = addr; ok = true; break; }
        }
        if (ok)
            LogF(L"レイヤー固定: 表示開始レイヤー番号の位置を特定しました "
                 L"(maxScroll=%d candidates=%zu)", maxScroll, cands.size());
        else if (g_log)
            g_log->warn(g_log, L"レイヤー固定: 表示開始レイヤー番号を特定できませんでした。"
                               L"固定は無効のままになります。");
    } else {
        static bool reported = false;
        if (!reported) {
            reported = true;
            LogF(L"レイヤー固定: まだ縦スクロール出来ないので位置の特定を保留します。"
                 L"レイヤーが増えたら自動でやり直します。");
        }
    }

    if (g_pStart) WriteIntSafe(g_pStart, original);
    if (ReadStartThroughSdk() != original) SetStartThroughSdk(original);

    InterlockedExchange(&g_suppressPaint, 0);
    if (g_host) InvalidateRect(g_host, nullptr, FALSE);
    InterlockedExchange(&g_dirty, 1);
    InterlockedExchange(&g_locating, 0);
    return ok;
}

static ULONGLONG g_lastLocateTry = 0;
static void EnsureStartPointerAsync() {
    if (g_locating) return;
    if (g_pStart && StartPointerLooksValid()) return;
    const ULONGLONG now = GetTickCount64();
    if (g_lastLocateTry && now - g_lastLocateTry < 1000) return;
    g_lastLocateTry = now;
    g_pStart = nullptr;
    CreateThread(nullptr, 0, [](LPVOID) -> DWORD { LocateStartPointer(); return 0; },
                 nullptr, 0, nullptr);
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
