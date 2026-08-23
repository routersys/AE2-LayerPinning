#pragma once

#include <windows.h>

#include "plugin2.h"
#include "config2.h"

namespace lp {

void SetEditHandle(EDIT_HANDLE* handle);
void SetConfigHandle(CONFIG_HANDLE* handle);
void SetHostWindow(HWND window);
void SetHostWindowProc(WNDPROC proc);

EDIT_HANDLE* Edit();
CONFIG_HANDLE* Config();
HWND HostWindow();
WNDPROC HostWindowProc();
LRESULT CallHostWindowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

EDIT_INFO EditInfo();
int LayoutSize(const char* key);
const wchar_t* Translate(const wchar_t* text);
void CallEditSection(void (*proc)(EDIT_SECTION* edit));

}
