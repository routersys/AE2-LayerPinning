#include "ScrollAnchor.h"

#include <windows.h>
#include <vector>

#include "HostContext.h"
#include "Log.h"
#include "RefreshRequest.h"
#include "SafeMemory.h"

namespace lp {

static int* g_pStart = nullptr;
static volatile LONG g_locating     = 0;
static volatile LONG g_suppressPaint = 0;

bool HasStartPointer() { return g_pStart != nullptr; }
bool ReadStart(int* out) { return ReadIntSafe(g_pStart, out); }
bool WriteStart(int value) { return WriteIntSafe(g_pStart, value); }
void ResetStartPointer() { g_pStart = nullptr; }
void SetStartPointer(int* address) { g_pStart = address; }
bool Locating() { return g_locating != 0; }
bool PaintSuppressed() { return g_suppressPaint != 0; }

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
    CallEditSection([](EDIT_SECTION* e) {
        e->set_display_layer_frame(g_setRequest, e->info->display_frame_start);
    });
    return EditInfo().display_layer_start;
}
int ReadStartThroughSdk() {
    return EditInfo().display_layer_start;
}
static bool StartPointerLooksValid() {
    if (!g_pStart) return false;
    int v;
    return ReadIntSafe(g_pStart, &v) && v == ReadStartThroughSdk();
}

static bool LocateStartPointer() {
    if (!Edit()) return false;
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
        else
            LogWarn(L"レイヤー固定: 表示開始レイヤー番号を特定できませんでした。"
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
    if (HostWindow()) InvalidateRect(HostWindow(), nullptr, FALSE);
    RequestRefresh();
    InterlockedExchange(&g_locating, 0);
    return ok;
}

static ULONGLONG g_lastLocateTry = 0;
void EnsureStartPointerAsync() {
    if (g_locating) return;
    if (g_pStart && StartPointerLooksValid()) return;
    const ULONGLONG now = GetTickCount64();
    if (g_lastLocateTry && now - g_lastLocateTry < 1000) return;
    g_lastLocateTry = now;
    g_pStart = nullptr;
    CreateThread(nullptr, 0, [](LPVOID) -> DWORD { LocateStartPointer(); return 0; },
                 nullptr, 0, nullptr);
}

}
