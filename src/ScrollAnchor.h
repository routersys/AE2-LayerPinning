#pragma once

namespace lp {

bool HasStartPointer();
bool ReadStart(int* out);
bool WriteStart(int value);
void ResetStartPointer();
void SetStartPointer(int* address);

int ReadStartThroughSdk();
void EnsureStartPointerAsync();

bool Locating();
bool PaintSuppressed();

}
