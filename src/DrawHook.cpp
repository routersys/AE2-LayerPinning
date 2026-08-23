#include "DrawHook.h"

#include <windows.h>

#include "HostContext.h"
#include "HostImage.h"
#include "Log.h"
#include "PinState.h"
#include "SafeMemory.h"
#include "ScrollAnchor.h"

namespace lp {

static const unsigned char kLoopABytes[] = {
    0x48, 0x8b, 0x87, 0xd0, 0x00, 0x00, 0x00, 0x48, 0x8b, 0x88, 0x50, 0x08,
    0x00, 0x00, 0x8b, 0x91, 0xc4, 0x01, 0x00, 0x00, 0x03, 0xd3, 0x48, 0x8b,
    0xcf, 0xe8, 0x61, 0x00, 0x00, 0x00, 0xff, 0xc3, 0x48, 0x8b, 0x87, 0xf0,
    0x00, 0x00, 0x00, 0x3b, 0x98, 0x38, 0x05, 0x00, 0x00, 0x7c, 0xd1, 0x80,
    0x7c, 0x24, 0x68, 0x00,
};
static const unsigned char kLoopCBytes[] = {
    0x48, 0x8b, 0x83, 0xd0, 0x00, 0x00, 0x00, 0x48, 0x8b, 0x88, 0x50, 0x08,
    0x00, 0x00, 0x8b, 0x91, 0xc4, 0x01, 0x00, 0x00, 0x03, 0xd7, 0x48, 0x8b,
    0xcb, 0xe8, 0xe0, 0x00, 0x00, 0x00, 0xff, 0xc7, 0x48, 0x8b, 0x83, 0xf8,
    0x00, 0x00, 0x00, 0x3b, 0xb8, 0x38, 0x05, 0x00, 0x00, 0x7c, 0xd1, 0x83,
    0xbb, 0x00, 0x03, 0x00, 0x00, 0x00,
};

static const unsigned long kLoopARva = 0x2f431;
static const unsigned long kLoopAEndRva = 0x2f460;
static const unsigned long kLoopCRva = 0x3d392;
static const unsigned long kLoopCEndRva = 0x3d3c1;

static const unsigned long long kRowCountOffset = 0x538;
static const unsigned long long kLoopAInfoOffset = 0xf0;
static const unsigned long long kLoopCInfoOffset = 0xf8;

static const DWORD64 kSlotMask = 0xffff00ffull;
static const DWORD64 kSlotEnable = 0x55ull;
static const ULONGLONG kWatchdogMs = 100;

struct LoopState {
    int drawBase = 0;
    int restore = 0;
    int pinnedRows = 0;
    bool live = false;
};

static const unsigned char* g_loopA = nullptr;
static const unsigned char* g_loopAEnd = nullptr;
static const unsigned char* g_loopC = nullptr;
static const unsigned char* g_loopCEnd = nullptr;

static LoopState g_stateA;
static LoopState g_stateC;

static PVOID g_veh = nullptr;
static volatile LONG g_active = 0;
static DWORD g_armedThread = 0;

static bool g_remapActive = false;
static int g_remapBase = 0;

static int g_lastRestore = 0;
static volatile LONG g_inLoop = 0;
static volatile ULONGLONG g_lastLoopAt = 0;

void SetRemapBase(int value) { g_remapBase = value; g_remapActive = true; }
void ClearRemapBase() { g_remapActive = false; }

static bool ReadRowCount(DWORD64 object, unsigned long long infoOffset, int* out) {
    void* info = nullptr;
    if (!ReadPtrSafe((void* const*)(object + infoOffset), &info)) return false;
    if (!info) return false;
    return ReadIntSafe((const int*)((unsigned char*)info + kRowCountOffset), out);
}

static int CapPinnedRows(int rows) {
    int pinned = PinCount();
    if (pinned > rows - 1) pinned = rows - 1;
    return pinned > 0 ? pinned : 0;
}

static void EnterRow(LoopState& state, int row, DWORD64 object, unsigned long long infoOffset) {
    if (row == 0) {
        state.live = false;
        int memory = 0, rows = 0;
        if (!ReadStart(&memory)) return;
        if (!ReadRowCount(object, infoOffset, &rows)) return;
        if (rows <= 1) return;
        state.restore = memory;
        state.drawBase = g_remapActive ? g_remapBase : memory;
        state.pinnedRows = CapPinnedRows(rows);
        state.live = true;
        g_lastRestore = memory;
        g_lastLoopAt = GetTickCount64();
        InterlockedExchange(&g_inLoop, 1);
    }
    if (!state.live) return;
    WriteStart(row < state.pinnedRows ? 0 : state.drawBase);
}

static void LeaveLoop(LoopState& state) {
    if (!state.live) return;
    state.live = false;
    WriteStart(state.restore);
    InterlockedExchange(&g_inLoop, 0);
}

static LONG CALLBACK DrawVeh(EXCEPTION_POINTERS* info) {
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    if (!g_active) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* context = info->ContextRecord;
    const unsigned char* at = (const unsigned char*)info->ExceptionRecord->ExceptionAddress;
    if (at == g_loopA) EnterRow(g_stateA, (int)context->Rbx, context->Rdi, kLoopAInfoOffset);
    else if (at == g_loopC) EnterRow(g_stateC, (int)context->Rdi, context->Rbx, kLoopCInfoOffset);
    else if (at == g_loopAEnd) LeaveLoop(g_stateA);
    else if (at == g_loopCEnd) LeaveLoop(g_stateC);
    else return EXCEPTION_CONTINUE_SEARCH;
    context->Dr6 = 0;
    context->EFlags |= 0x10000;
    return EXCEPTION_CONTINUE_EXECUTION;
}

static bool WithThreadContext(DWORD threadId, bool (*apply)(CONTEXT*)) {
    const bool self = (threadId == GetCurrentThreadId());
    HANDLE thread = self ? GetCurrentThread()
                         : OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME,
                                      FALSE, threadId);
    if (!thread) return false;
    if (!self) SuspendThread(thread);
    bool ok = false;
    CONTEXT context = {};
    context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    if (GetThreadContext(thread, &context) && apply(&context)) {
        context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        ok = SetThreadContext(thread, &context) != FALSE;
    }
    if (!self) { ResumeThread(thread); CloseHandle(thread); }
    return ok;
}

static bool ApplyArm(CONTEXT* context) {
    context->Dr0 = (DWORD64)g_loopA;
    context->Dr1 = (DWORD64)g_loopAEnd;
    context->Dr2 = (DWORD64)g_loopC;
    context->Dr3 = (DWORD64)g_loopCEnd;
    context->Dr6 = 0;
    context->Dr7 = (context->Dr7 & ~kSlotMask) | kSlotEnable;
    return true;
}

static bool ApplyDisarm(CONTEXT* context) {
    context->Dr0 = 0;
    context->Dr1 = 0;
    context->Dr2 = 0;
    context->Dr3 = 0;
    context->Dr6 = 0;
    context->Dr7 &= ~kSlotMask;
    return true;
}

static bool g_armedNow = false;
static bool ReadArmedState(CONTEXT* context) {
    g_armedNow = (context->Dr7 & kSlotEnable) == kSlotEnable &&
                 context->Dr0 == (DWORD64)g_loopA &&
                 context->Dr1 == (DWORD64)g_loopAEnd &&
                 context->Dr2 == (DWORD64)g_loopC &&
                 context->Dr3 == (DWORD64)g_loopCEnd;
    return false;
}

static bool StillArmed(DWORD threadId) {
    g_armedNow = false;
    WithThreadContext(threadId, ReadArmedState);
    return g_armedNow;
}

bool DrawHookSupported() {
    static int checked = 0;
    static bool result = false;
    if (checked) return result;
    checked = 1;
    result = ImageIsExpectedBuild() &&
             ImageBytesMatch(kLoopARva, kLoopABytes, sizeof(kLoopABytes)) &&
             ImageBytesMatch(kLoopCRva, kLoopCBytes, sizeof(kLoopCBytes));
    if (result) {
        g_loopA = ImageBase() + kLoopARva;
        g_loopAEnd = ImageBase() + kLoopAEndRva;
        g_loopC = ImageBase() + kLoopCRva;
        g_loopCEnd = ImageBase() + kLoopCEndRva;
        LogF(L"レイヤー固定: 本体に直接描かせる方式を使えます");
    } else {
        LogF(L"レイヤー固定: 本体の版が一致しないので取得方式で動きます");
    }
    return result;
}

bool DrawHookActive() { return g_active != 0; }

static void RunWatchdog() {
    if (!g_inLoop) return;
    if (GetTickCount64() - g_lastLoopAt < kWatchdogMs) return;
    g_stateA.live = false;
    g_stateC.live = false;
    InterlockedExchange(&g_inLoop, 0);
    int current = 0;
    if (ReadStart(&current) && current != g_lastRestore) WriteStart(g_lastRestore);
}

void KeepDrawHookArmed() {
    if (!DrawHookSupported() || !HasStartPointer() || PinCount() <= 0) {
        ReleaseDrawHook();
        return;
    }
    if (HostWindow() && GetWindowThreadProcessId(HostWindow(), nullptr) != GetCurrentThreadId()) return;
    if (!g_veh) g_veh = AddVectoredExceptionHandler(1, DrawVeh);
    if (!g_veh) return;
    if (!g_active) {
        g_stateA.live = false;
        g_stateC.live = false;
        if (!WithThreadContext(GetCurrentThreadId(), ApplyArm)) return;
        g_armedThread = GetCurrentThreadId();
        InterlockedExchange(&g_active, 1);
        LogF(L"レイヤー固定: 直接描画を有効にしました");
        return;
    }
    if (!StillArmed(g_armedThread)) WithThreadContext(g_armedThread, ApplyArm);
    RunWatchdog();
}

void ReleaseDrawHook() {
    if (g_active) {
        InterlockedExchange(&g_active, 0);
        WithThreadContext(g_armedThread, ApplyDisarm);
        g_armedThread = 0;
        g_stateA.live = false;
        g_stateC.live = false;
        if (g_inLoop) {
            InterlockedExchange(&g_inLoop, 0);
            int current = 0;
            if (ReadStart(&current) && current != g_lastRestore) WriteStart(g_lastRestore);
        }
    }
    if (g_veh) {
        RemoveVectoredExceptionHandler(g_veh);
        g_veh = nullptr;
    }
}

}
