#include <windows.h>
#include <windowsx.h>
#include <stdio.h>
#include <vector>
#include <unordered_map>

#include "plugin2.h"
#include "logger2.h"
#include "config2.h"

static EDIT_HANDLE*   g_edit    = nullptr;
static LOG_HANDLE*    g_log     = nullptr;
static CONFIG_HANDLE* g_config  = nullptr;
static HWND           g_host    = nullptr;

static void LogF(const wchar_t* fmt, ...) {
    wchar_t buf[1024];
    va_list ap; va_start(ap, fmt);
    _vsnwprintf_s(buf, _TRUNCATE, fmt, ap);
    va_end(ap);
    if (g_log) g_log->log(g_log, buf);
#ifdef LP_DEBUG_LOG
    static wchar_t logPath[MAX_PATH] = {};
    if (!logPath[0]) {
        wchar_t dir[MAX_PATH] = {};
        if (GetTempPathW(MAX_PATH, dir))
            _snwprintf_s(logPath, _TRUNCATE, L"%sLayerPinning.log", dir);
    }
    if (logPath[0]) {
        FILE* f = nullptr;
        if (_wfopen_s(&f, logPath, L"a+, ccs=UTF-8") == 0 && f) {
            fwprintf(f, L"%s\n", buf);
            fclose(f);
        }
    }
#endif
}

COMMON_PLUGIN_TABLE common_plugin_table = {
    L"レイヤー固定",
    L"レイヤー固定 version 1.00",
};

EXTERN_C __declspec(dllexport) COMMON_PLUGIN_TABLE* GetCommonPluginTable(void) {
    return &common_plugin_table;
}
EXTERN_C __declspec(dllexport) DWORD RequiredVersion() { return 2010000; }
EXTERN_C __declspec(dllexport) void InitializeLogger(LOG_HANDLE* h) { g_log = h; }
EXTERN_C __declspec(dllexport) void InitializeConfig(CONFIG_HANDLE* h) { g_config = h; }
EXTERN_C __declspec(dllexport) bool InitializePlugin(DWORD) { return true; }

EXTERN_C __declspec(dllexport) void RegisterPlugin(HOST_APP_TABLE* host) {
    g_edit = host->create_edit_handle();
    g_host = g_edit->get_host_app_window();

    LogF(L"LayerPinning registered (host=%p)", (void*)g_host);
}

EXTERN_C __declspec(dllexport) void UninitializePlugin() {
}

BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID) { return TRUE; }
