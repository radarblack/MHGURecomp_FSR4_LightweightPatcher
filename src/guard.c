#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "plugin.h"

#define MAX_REPORTS 3

static wchar_t guard_path[MAX_PATH];
static bool blocked, installed;
static char stage[64];
static volatile DWORD stage_thread;
static volatile LONG reports;

static void guard_append(const char *text) {
    if (!guard_path[0]) return;
    HANDLE h = CreateFileW(guard_path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD written;
    WriteFile(h, text, (DWORD)strlen(text), &written, NULL);
    WriteFile(h, "\r\n", 2, &written, NULL);
    CloseHandle(h);
}

static void report(const char *fmt, ...) {
    char text[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    plog("%s", text);
    guard_append(text);
}

static const char *module_of(const void *address, char *out, size_t size, uintptr_t *offset) {
    HMODULE module = NULL;
    wchar_t path[MAX_PATH] = {0};
    out[0] = 0;
    *offset = 0;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, address, &module) && module &&
        GetModuleFileNameW(module, path, MAX_PATH)) {
        const wchar_t *name = wcsrchr(path, L'\\');
        WideCharToMultiByte(CP_UTF8, 0, name ? name + 1 : path, -1, out, (int)size, NULL, NULL);
        *offset = (uintptr_t)address - (uintptr_t)module;
    } else {
        snprintf(out, size, "?");
    }
    return out;
}

static LONG CALLBACK on_exception(PEXCEPTION_POINTERS ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    bool fatal = code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_ILLEGAL_INSTRUCTION || code == EXCEPTION_STACK_OVERFLOW ||
                 code == EXCEPTION_PRIV_INSTRUCTION || code == EXCEPTION_IN_PAGE_ERROR || code == 0xC0000409;
    if (!fatal || !stage[0] || GetCurrentThreadId() != stage_thread || InterlockedIncrement(&reports) > MAX_REPORTS) return EXCEPTION_CONTINUE_SEARCH;
    char name[128];
    uintptr_t offset;
    module_of(ep->ExceptionRecord->ExceptionAddress, name, sizeof name, &offset);
    report("EXCEPTION %#lx while \"%s\" at %s+%#llx", (unsigned long)code, stage, name, (unsigned long long)offset);
    if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2)
        report("  access violation: %s address %#llx", ep->ExceptionRecord->ExceptionInformation[0] == 0   ? "reading"
                                                      : ep->ExceptionRecord->ExceptionInformation[0] == 1 ? "writing"
                                                                                                          : "executing",
             (unsigned long long)ep->ExceptionRecord->ExceptionInformation[1]);
    CONTEXT c = *ep->ContextRecord;
    for (int i = 0; i < 12 && c.Rip; i++) {
        module_of((void *)c.Rip, name, sizeof name, &offset);
        report("  frame %d: %s+%#llx", i, name, (unsigned long long)offset);
        DWORD64 base;
        PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(c.Rip, &base, NULL);
        if (fn) {
            PVOID handler_data;
            DWORD64 frame;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c.Rip, fn, &c, &handler_data, &frame, NULL);
        } else {
            c.Rip = *(DWORD64 *)c.Rsp;
            c.Rsp += 8;
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void guard_init(void) {
    if (installed) return;
    installed = true;
    if (!plugin_dir(guard_path, MAX_PATH)) return;
    wcscat(guard_path, L"crashguard.txt");
    AddVectoredExceptionHandler(1, on_exception);
    HANDLE h = CreateFileW(guard_path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    char text[256] = {0};
    DWORD got = 0;
    ReadFile(h, text, sizeof text - 1, &got, NULL);
    CloseHandle(h);
    char build[32] = {0}, where[128] = {0};
    sscanf(text, "%31[^\r\n]\r\n%127[^\r\n]", build, where);
    if (strcmp(build, PLUGIN_VERSION)) {
        plog("an old crashguard.txt (plugin %s) was removed", build);
        DeleteFileW(guard_path);
        return;
    }
    if (settings_flag("MHGU_FSR4_RETRY")) {
        plog("the previous run crashed while \"%s\", retrying because MHGU_FSR4_RETRY is set", where);
        DeleteFileW(guard_path);
        return;
    }
    blocked = true;
    plog("the previous run crashed while \"%s\": FSR 4 stays off and the game's FSR 3.1 runs. Delete crashguard.txt (or set MHGU_FSR4_RETRY=1) to try again, the details of the crash are in crashguard.txt.", where);
}

bool guard_blocked(void) { return blocked; }

void guard_enter(const char *what) {
    stage_thread = GetCurrentThreadId();
    snprintf(stage, sizeof stage, "%s", what);
    HANDLE h = CreateFileW(guard_path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    char text[160];
    int n = snprintf(text, sizeof text, "%s\r\n%s\r\n", PLUGIN_VERSION, what);
    DWORD written;
    WriteFile(h, text, (DWORD)n, &written, NULL);
    CloseHandle(h);
}

void guard_leave(void) {
    stage[0] = 0;
    DeleteFileW(guard_path);
}
