#include "RefreshRequest.h"

#include <windows.h>

namespace lp {

static volatile LONG g_dirty = 1;
static void (*g_waker)() = nullptr;

void SetRefreshWaker(void (*proc)()) {
    g_waker = proc;
}

static void Wake() {
    if (g_waker) g_waker();
}

void RequestRefresh() {
    InterlockedExchange(&g_dirty, 1);
    Wake();
}

void DeferRefreshRequest() {
    InterlockedExchange(&g_dirty, 1);
}

bool TakeRefreshRequest() {
    return InterlockedExchange(&g_dirty, 0) != 0;
}

}
