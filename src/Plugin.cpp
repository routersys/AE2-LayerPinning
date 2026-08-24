#include <windows.h>

#include "DrawHook.h"
#include "HostContext.h"
#include "HostWindowHook.h"
#include "LayerMenuHook.h"
#include "Log.h"
#include "PinningController.h"

using namespace lp;

COMMON_PLUGIN_TABLE common_plugin_table = {
    L"レイヤー固定",
    L"レイヤー固定 version 1.0.0",
};

EXTERN_C __declspec(dllexport) COMMON_PLUGIN_TABLE* GetCommonPluginTable(void) {
    return &common_plugin_table;
}
EXTERN_C __declspec(dllexport) DWORD RequiredVersion() { return 2010000; }
EXTERN_C __declspec(dllexport) void InitializeLogger(LOG_HANDLE* h) { SetLogHandle(h); }
EXTERN_C __declspec(dllexport) void InitializeConfig(CONFIG_HANDLE* h) { SetConfigHandle(h); }
EXTERN_C __declspec(dllexport) bool InitializePlugin(DWORD) { return true; }

EXTERN_C __declspec(dllexport) void RegisterPlugin(HOST_APP_TABLE* host) {
    SetEditHandle(host->create_edit_handle());
    SetHostWindow(Edit()->get_host_app_window());

    host->register_layer_menu(MenuTextPin(), OnPinMenu);
    host->register_layer_menu(MenuTextUnpin(), OnUnpinMenu);
    host->register_change_scene_handler(OnChangeScene);
    host->register_event_listener(EVENT_TYPE::UPDATE_OBJECT,       nullptr, OnAnyEvent);
    host->register_event_listener(EVENT_TYPE::CHANGE_EDIT_FRAME,   nullptr, OnAnyEvent);
    host->register_event_listener(EVENT_TYPE::CHANGE_FOCUS_OBJECT, nullptr, OnAnyEvent);
    host->register_event_listener(EVENT_TYPE::CHANGE_EDIT_SCENE,   nullptr, OnAnyEvent);

    InstallHostWindowHook();

    void* tpmSlot = InstallLayerMenuHook();

    LogF(L"LayerPinning registered (host=%p tpmSlot=%p)", (void*)HostWindow(), tpmSlot);
}

EXTERN_C __declspec(dllexport) void UninitializePlugin() {
    ReleaseDrawHook();
    UninstallLayerMenuHook();
    DestroyOverlay();
    UninstallHostWindowHook();
    ReleaseCaches();
}

BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID) { return TRUE; }
