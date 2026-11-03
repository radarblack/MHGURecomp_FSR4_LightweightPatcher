// THIS IS DISABLED! Log file size substantially bloats up but you can enable this again if you need to debug something. idc. lol

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "plugin.h"

static HMODULE self;
static CRITICAL_SECTION lock;
static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
static HANDLE file = INVALID_HANDLE_VALUE;
static bool enabled;

HMODULE plugin_module(void) { return self; }

bool plugin_dir(wchar_t *out, size_t count) {
    if (!self || !GetModuleFileNameW(self, out, (DWORD)count)) return false;
    wchar_t *slash = wcsrchr(out, L'\\');
    if (!slash) return false;
    slash[1] = 0;
    return true;
}

static BOOL CALLBACK open_log(PINIT_ONCE o, PVOID p, PVOID *c) {
    (void)o, (void)p, (void)c;
    InitializeCriticalSection(&lock);
    enabled = settings_flag("MHGU_FSR4_LOG");
    wchar_t path[MAX_PATH];
    if (enabled && plugin_dir(path, MAX_PATH)) {
        wcscat(path, L"fsr4_layer.log");
        file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    }
    return TRUE;
}

void plog(const char *fmt, ...) {
    InitOnceExecuteOnce(&once, open_log, NULL, NULL);
    if (!enabled) return;
    char line[1024];
    SYSTEMTIME t;
    GetLocalTime(&t);
    int n = snprintf(line, sizeof line, "%02u:%02u:%02u.%03u ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line + n, sizeof line - (size_t)n - 2, fmt, ap);
    va_end(ap);
    size_t len = strlen(line);
    if (len && line[len - 1] != '\n') line[len++] = '\n', line[len] = 0;
    EnterCriticalSection(&lock);
    if (file != INVALID_HANDLE_VALUE) {
        DWORD written;
        WriteFile(file, line, (DWORD)len, &written, NULL);
    }
    LeaveCriticalSection(&lock);
    OutputDebugStringA(line);
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        self = h;
        DisableThreadLibraryCalls(h);
    }
    return TRUE;
}
