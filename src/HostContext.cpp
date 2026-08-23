#include "HostContext.h"

namespace lp {

static EDIT_HANDLE*   g_edit   = nullptr;
static CONFIG_HANDLE* g_config = nullptr;
static HWND           g_host   = nullptr;
static WNDPROC        g_hostProc = nullptr;

void SetEditHandle(EDIT_HANDLE* handle) { g_edit = handle; }
void SetConfigHandle(CONFIG_HANDLE* handle) { g_config = handle; }
void SetHostWindow(HWND window) { g_host = window; }
void SetHostWindowProc(WNDPROC proc) { g_hostProc = proc; }

EDIT_HANDLE* Edit() { return g_edit; }
CONFIG_HANDLE* Config() { return g_config; }
HWND HostWindow() { return g_host; }
WNDPROC HostWindowProc() { return g_hostProc; }

LRESULT CallHostWindowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    return CallWindowProcW(g_hostProc, hwnd, msg, wp, lp);
}

EDIT_INFO EditInfo() {
    EDIT_INFO info = {};
    if (g_edit) g_edit->get_edit_info(&info, sizeof(info));
    return info;
}

int LayoutSize(const char* key) {
    return g_config ? g_config->get_layout_size(g_config, key) : 0;
}

const wchar_t* Translate(const wchar_t* text) {
    return g_config ? g_config->translate(g_config, text) : text;
}

void CallEditSection(void (*proc)(EDIT_SECTION* edit)) {
    if (g_edit) g_edit->call_edit_section(proc);
}

}
