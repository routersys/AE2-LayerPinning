#include "SafeMemory.h"

#include <windows.h>

namespace lp {

size_t ScanBlockSafe(int* base, size_t count, int wanted, int** out, size_t outCap) {
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

bool ReadIntSafe(const int* p, int* out) {
    __try { *out = *p; return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool WriteIntSafe(int* p, int v) {
    __try { *p = v; return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

}
