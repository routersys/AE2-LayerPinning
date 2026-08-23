#include "LayerMenuHook.h"

#include <windows.h>
#include <vector>

#include "HostContext.h"
#include "LayerGeometry.h"
#include "Log.h"
#include "PinState.h"
#include "ScrollAnchor.h"

namespace lp {

const wchar_t* MenuTextPin() {
    return Translate(L"レイヤーを固定");
}
const wchar_t* MenuTextUnpin() {
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

void* InstallLayerMenuHook() {
    g_tpmSlot = FindIatSlot(GetModuleHandleW(nullptr), "USER32.dll", "TrackPopupMenu");
    if (g_tpmSlot) PatchSlot(g_tpmSlot, (void*)&Hook_TrackPopupMenu, (void**)&g_origTPM);
    return (void*)g_tpmSlot;
}

void UninstallLayerMenuHook() {
    if (g_tpmSlot && g_origTPM) PatchSlot(g_tpmSlot, (void*)g_origTPM, nullptr);
}

int MenuTargetLayer() { return g_menuLayer; }

}
