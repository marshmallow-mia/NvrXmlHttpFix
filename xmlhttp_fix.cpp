/*
 * NvrXmlHttpFix.dll -- gives Echo VR the real MSXML XMLHTTP object again.
 *
 * nEVR runtime 4.0.0 hooks ole32!CoCreateInstance and answers the game's request for
 * MSXML6's FreeThreadedXMLHTTP60 with a stand-in shaped like WinHTTP's IWinHttpRequest.
 * The game drives that object as IXMLHTTPRequest, whose vtable differs, so the first
 * call past IDispatch lands in the wrong stub. In windowed mode (-windowed) the game
 * makes such a request at the main menu and crashes (a write through a garbage pointer
 * in Stub_GetTypeInfo).
 *
 * This plugin hooks CoCreateInstance on top of nEVR's hook. For that one class it builds
 * the system's own object (CoGetClassObject + IClassFactory::CreateInstance, which nEVR
 * leaves alone), as the game had it before nEVR; every other class goes on through
 * nEVR's hook unchanged. Drop it once nEVR's stand-in speaks IXMLHTTPRequest.
 */

#include <windows.h>
#include <objbase.h>
#include <MinHook.h>

#include <cstdarg>
#include <cstdio>

#include "nevr_plugin_interface.h"

namespace {

/* {88D96A09-F192-11D4-A65F-0040963251E5}: MSXML6's FreeThreadedXMLHTTP60, the class
 * nEVR 4.0.0 replaces. */
const CLSID kFreeThreadedXmlHttp60 = {
    0x88d96a09, 0xf192, 0x11d4, {0xa6, 0x5f, 0x00, 0x40, 0x96, 0x32, 0x51, 0xe5}};

using CoCreateInstanceFn = HRESULT(WINAPI*)(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID*);

CoCreateInstanceFn g_next = nullptr;  // whatever was there before us (nEVR's hook)
void* g_target = nullptr;
FILE* g_log = nullptr;
volatile LONG g_served = 0;

void Log(const char* fmt, ...) {
    if (!g_log) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(g_log, "[%02u:%02u:%02u] ", t.wHour, t.wMinute, t.wSecond);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

/* plugin_logs/NvrXmlHttpFix/NvrXmlHttpFix.log beside the game's exe, where the other
 * plugins log. */
void OpenLog() {
    wchar_t dir[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, dir, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return;
    wchar_t* slash = wcsrchr(dir, L'\\');
    if (!slash) return;
    *slash = 0;
    wchar_t path[MAX_PATH];
    _snwprintf(path, MAX_PATH, L"%ls\\plugin_logs", dir);
    CreateDirectoryW(path, nullptr);
    _snwprintf(path, MAX_PATH, L"%ls\\plugin_logs\\NvrXmlHttpFix", dir);
    CreateDirectoryW(path, nullptr);
    _snwprintf(path, MAX_PATH, L"%ls\\plugin_logs\\NvrXmlHttpFix\\NvrXmlHttpFix.log", dir);
    g_log = _wfopen(path, L"w");
}

HRESULT WINAPI CoCreateInstanceHook(REFCLSID clsid, LPUNKNOWN outer, DWORD context,
                                    REFIID riid, LPVOID* out) {
    if (!IsEqualCLSID(clsid, kFreeThreadedXmlHttp60)) {
        return g_next(clsid, outer, context, riid, out);
    }
    IClassFactory* factory = nullptr;
    HRESULT hr = CoGetClassObject(clsid, context, nullptr, IID_IClassFactory,
                                  reinterpret_cast<void**>(&factory));
    if (SUCCEEDED(hr)) {
        hr = factory->CreateInstance(outer, riid, out);
        factory->Release();
    }
    LONG n = InterlockedIncrement(&g_served);
    if (n <= 5 || FAILED(hr)) {
        Log("FreeThreadedXMLHTTP60 #%ld: the system's object, hr=0x%08lX", n,
            static_cast<unsigned long>(hr));
    }
    if (FAILED(hr)) {
        // No system object (no msxml6): fall back to what nEVR does.
        Log("falling back to nEVR's stand-in");
        return g_next(clsid, outer, context, riid, out);
    }
    return hr;
}

int Install() {
    OpenLog();
    HMODULE ole32 = LoadLibraryW(L"ole32.dll");
    g_target = ole32 ? reinterpret_cast<void*>(GetProcAddress(ole32, "CoCreateInstance"))
                     : nullptr;
    if (!g_target) {
        Log("ERROR: ole32!CoCreateInstance not found");
        return 1;
    }
    MH_STATUS s = MH_Initialize();
    if (s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED) {
        Log("ERROR: MH_Initialize: %s", MH_StatusToString(s));
        return 1;
    }
    s = MH_CreateHook(g_target, reinterpret_cast<void*>(CoCreateInstanceHook),
                      reinterpret_cast<void**>(&g_next));
    if (s != MH_OK) {
        Log("ERROR: MH_CreateHook(CoCreateInstance): %s", MH_StatusToString(s));
        return 1;
    }
    s = MH_EnableHook(g_target);
    if (s != MH_OK) {
        Log("ERROR: MH_EnableHook(CoCreateInstance): %s", MH_StatusToString(s));
        MH_RemoveHook(g_target);
        return 1;
    }
    Log("CoCreateInstance hooked at %p: FreeThreadedXMLHTTP60 is the system's again", g_target);
    return 0;
}

}  // namespace

extern "C" {

NvrPluginInfo NvrPluginGetInfo(void) {
    NvrPluginInfo info = {};
    info.name = "xmlhttp_fix";
    info.description = "Gives the game the system's XMLHTTP (nEVR 4.0.0's stand-in crashes windowed mode)";
    info.version_major = 1;
    info.version_minor = 0;
    info.version_patch = 0;
    return info;
}

uint32_t NvrPluginGetApiVersion(void) { return NEVR_PLUGIN_API_VERSION; }

uint32_t NvrPluginGetCapabilities(void) {
    return NEVR_PLUGIN_CAP_NETWORK | NEVR_PLUGIN_CAP_HOOKS_ENGINE;
}

int NvrPluginInitEx(const NvrGameContext*, const char*) { return Install(); }

int NvrPluginInit(const NvrGameContext*) { return Install(); }

void NvrPluginShutdown(void) {
    if (g_target) {
        MH_DisableHook(g_target);
        MH_RemoveHook(g_target);
        g_target = nullptr;
    }
    if (g_log) {
        fclose(g_log);
        g_log = nullptr;
    }
}

}  // extern "C"

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(module);
    return TRUE;
}
