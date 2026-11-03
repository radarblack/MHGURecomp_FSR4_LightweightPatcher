#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "plugin.h"

static bool iequals(const char *a, const char *b) {
    for (; *a && *b; a++, b++)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
    return !*a && !*b;
}

static char *trim(char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

bool settings_in_game_exe(void) {
    wchar_t path[MAX_PATH];
    if (!GetModuleFileNameW(NULL, path, MAX_PATH)) return false;
    const wchar_t *name = wcsrchr(path, L'\\');
    name = name ? name + 1 : path;
    return !_wcsicmp(name, L"mhgurecomp.exe");
}

const char *settings_text(const char *name) {
    static char slots[8][512];
    static LONG next;
    char *slot = slots[(unsigned)InterlockedIncrement(&next) % 8];
    DWORD n = GetEnvironmentVariableA(name, slot, 512);
    return n > 0 && n < 512 ? slot : NULL;
}

bool settings_flag(const char *name) {
    const char *v = settings_text(name);
    return v && strcmp(v, "0");
}

static bool read_ini_upscaler(char *value, size_t size) {
    wchar_t path[MAX_PATH];
    const char *home = settings_text("MHGU_HOME");
    if (home && *home) {
        wchar_t whome[MAX_PATH];
        MultiByteToWideChar(CP_UTF8, 0, home, -1, whome, MAX_PATH);
        swprintf(path, MAX_PATH, L"%ls\\mhgurecomp.ini", whome);
    } else {
        if (!GetModuleFileNameW(NULL, path, MAX_PATH)) return false;
        wchar_t *slash = wcsrchr(path, L'\\');
        if (!slash) return false;
        wcscpy(slash + 1, L"mhgurecomp.ini");
    }
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD length = GetFileSize(h, NULL), got = 0;
    char *text = length && length < (1u << 24) ? malloc(length + 1) : NULL;
    if (text && !ReadFile(h, text, length, &got, NULL)) got = 0;
    CloseHandle(h);
    if (!text) return false;
    text[got] = 0;
    bool in_section = false, found = false;
    for (char *line = strtok(text, "\r\n"); line; line = strtok(NULL, "\r\n")) {
        char *s = trim(line);
        if (*s == '[') {
            char *close = strchr(s, ']');
            in_section = close && (size_t)(close - s - 1) == 8 && !_strnicmp(s + 1, "Upscaler", 8);
        } else if (in_section) {
            char *eq = strchr(s, '=');
            if (!eq || *s == ';' || *s == '#') continue;
            *eq = 0;
            if (iequals(trim(s), "Upscaler")) {
                snprintf(value, size, "%s", trim(eq + 1));
                found = true;
            }
        }
    }
    free(text);
    return found;
}

bool settings_fsr4_requested(void) {
    const char *env = settings_text("MHGU_FSR_UPSCALER");
    char value[64] = {0};
    if (env)
        snprintf(value, sizeof value, "%s", env);
    else if (!read_ini_upscaler(value, sizeof value))
        return true;
    if (iequals(value, "3") || iequals(value, "fsr3") || iequals(value, "fsr3.1") || iequals(value, "fsr 3.1")) return false;
    return true;
}
