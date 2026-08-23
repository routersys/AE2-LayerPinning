#include "RefreshRequest.h"

#include <windows.h>

namespace lp {

static volatile LONG g_dirty = 1;

void RequestRefresh() {
    InterlockedExchange(&g_dirty, 1);
}

bool TakeRefreshRequest() {
    return InterlockedExchange(&g_dirty, 0) != 0;
}

}
