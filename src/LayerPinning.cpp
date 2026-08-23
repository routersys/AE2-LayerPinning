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

static std::unordered_map<int, int> g_pinByScene;
static int g_sceneId  = 0;
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

static const wchar_t* MenuTextPin() {
    return g_config ? g_config->translate(g_config, L"レイヤーを固定") : L"レイヤーを固定";
}
static const wchar_t* MenuTextUnpin() {
    return g_config ? g_config->translate(g_config, L"レイヤーの固定を解除") : L"レイヤーの固定を解除";
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

static int LayerUnderCursor(int screenX, int screenY) {
    if (!g_host) return -1;
    if (!g_geo.valid) {
        if (!g_edit) return -1;
        EDIT_INFO info = {};
        g_edit->get_edit_info(&info, sizeof(info));
        return info.layer;
    }
    POINT pt = { screenX, screenY };
    ScreenToClient(g_host, &pt);
    if (pt.x < g_geo.area.left || pt.x >= g_geo.contentRight) return -1;
    if (pt.y < g_geo.row0Top || pt.y >= g_geo.area.bottom) return -1;

    int row = (pt.y - g_geo.row0Top) / g_geo.rowHeight;
    if (g_pinCount > 0 && row < g_pinCount) return row;

    int start = 0;
    if (g_pStart) { if (!ReadIntSafe(g_pStart, &start)) start = ReadStartThroughSdk(); }
    else start = ReadStartThroughSdk();
    return start + row;
}

static bool IsMenuEligible(int layer) {
    if (layer < 0 || !g_edit) return false;
    if (layer < g_pinCount) return true;
    if (layer != g_pinCount) return false;
    EDIT_INFO info = {}; g_edit->get_edit_info(&info, sizeof(info));
    return layer + 1 < info.display_layer_num;
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
        const bool pinned   = eligible && layer < g_pinCount;
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
        int cur = -1; if (g_pStart) ReadIntSafe(g_pStart, &cur);
        EDIT_INFO info = {}; if (g_edit) g_edit->get_edit_info(&info, sizeof(info));
        LogF(L"  MENU OPEN: pStart=%d hostLayer=%d ourLayer=%d",
             cur, info.layer, g_menuLayer);
    }
#endif
    BOOL r = g_origTPM(menu, flags, x, y, reserved, hwnd, rc);
#ifdef LP_DEBUG_LOG
    {
        int cur = -1; if (g_pStart) ReadIntSafe(g_pStart, &cur);
        EDIT_INFO info = {}; if (g_edit) g_edit->get_edit_info(&info, sizeof(info));
        LogF(L"  MENU CLOSED: pStart=%d hostLayer=%d", cur, info.layer);
    }
#endif
    return r;
}

static void ApplyPinChange(EDIT_SECTION* edit, bool wantPin) {
    if (!g_geo.valid) ProbeGeometry(edit);

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

    const int before = g_pinCount;
    if (wantPin) {
        if (layer != g_pinCount) return;
        g_pinCount = layer + 1;
    } else {
        if (layer >= g_pinCount) return;
        g_pinCount = layer;
    }

    g_pinByScene[g_sceneId] = g_pinCount;
    InterlockedExchange(&g_dirty, 1);
    LogF(L"pinCount %d -> %d (layer %d, scene %d)", before, g_pinCount, layer, g_sceneId);

    if (g_pinCount > 0) EnsureStartPointerAsync();

    if (g_host) InvalidateRect(g_host, nullptr, FALSE);
}

static void OnPinMenu(EDIT_SECTION* edit)   { ApplyPinChange(edit, true); }
static void OnUnpinMenu(EDIT_SECTION* edit) { ApplyPinChange(edit, false); }

static void OnChangeScene(EDIT_SECTION* edit) {
    g_sceneId = edit->info->scene_id;
    auto it = g_pinByScene.find(g_sceneId);
    g_pinCount = (it == g_pinByScene.end()) ? 0 : it->second;
    g_pStart = nullptr;
    InterlockedExchange(&g_dirty, 1);
    ProbeGeometry(edit);
    if (g_pinCount > 0) EnsureStartPointerAsync();
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

    host->register_layer_menu(MenuTextPin(), OnPinMenu);
    host->register_layer_menu(MenuTextUnpin(), OnUnpinMenu);
    host->register_change_scene_handler(OnChangeScene);

    g_tpmSlot = FindIatSlot(GetModuleHandleW(nullptr), "USER32.dll", "TrackPopupMenu");
    if (g_tpmSlot) PatchSlot(g_tpmSlot, (void*)&Hook_TrackPopupMenu, (void**)&g_origTPM);

    LogF(L"LayerPinning registered (host=%p tpmSlot=%p)", (void*)g_host, (void*)g_tpmSlot);
}

EXTERN_C __declspec(dllexport) void UninitializePlugin() {
    if (g_tpmSlot && g_origTPM) PatchSlot(g_tpmSlot, (void*)g_origTPM, nullptr);
}

BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID) { return TRUE; }
