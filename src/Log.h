#pragma once

#include <windows.h>

#include "logger2.h"

namespace lp {

void SetLogHandle(LOG_HANDLE* handle);
void LogF(const wchar_t* fmt, ...);
void LogWarn(const wchar_t* text);

}
