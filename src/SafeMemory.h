#pragma once

#include <stddef.h>

namespace lp {

size_t ScanBlockSafe(int* base, size_t count, int wanted, int** out, size_t outCap);
bool ReadIntSafe(const int* p, int* out);
bool WriteIntSafe(int* p, int v);

}
