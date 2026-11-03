#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "MinHook.h"
#include "plugin.h"

#define NAME_RECORD "ffxVkPortableUpscaleContextRecordDispatch"
#define NAME_CREATE "ffxVkPortableUpscaleContextCreate"

typedef FfxVkPortableResult (*RecordFn)(FfxVkPortableUpscaleContext *, const FfxVkPortableUpscaleDispatchInfo *);
typedef FfxVkPortableResult (*CreateFn)(const FfxVkPortableDeviceInfo *, const FfxVkPortableUpscaleCreateInfo *,
                                        FfxVkPortableUpscaleContext **);

static RecordFn orig_record;
static CreateFn orig_create;
static bool installed, size_warned;
static uint8_t *exe_bytes;
static size_t exe_size;

static uint64_t status_start, status_interval_ms, count_fsr4, count_fsr3;

static void status_tick(bool used_fsr4) {
    if (used_fsr4) count_fsr4++;
    else count_fsr3++;
    uint64_t now = GetTickCount64();
    if (!status_start) {
        const char *seconds = settings_text("MHGU_FSR4_STATUS_SECONDS");
        status_interval_ms = seconds ? (uint64_t)(atof(seconds) * 1000.0) : 10000;
        status_start = now;
    }
    if (now - status_start < status_interval_ms) return;
    plog("status: %llu upscales with %s and %llu with the game's FSR 3.1 in the last %.1f s%s", (unsigned long long)count_fsr4,
         fsr4rt_name(), (unsigned long long)count_fsr3, (double)(now - status_start) / 1000.0,
         count_fsr4 ? "" : " (FSR 4 is not running)");
    status_start = now;
    count_fsr4 = count_fsr3 = 0;
}

static FfxVkPortableResult hook_record(FfxVkPortableUpscaleContext *context, const FfxVkPortableUpscaleDispatchInfo *di) {
    FfxVkPortableResult r;
    bool used = false;
    if (di && fsr4rt_usable()) {
        if (di->structSize >= sizeof *di) {
            used = fsr4rt_dispatch(di);
        } else if (!size_warned) {
            size_warned = true;
            plog("the game's dispatch struct is %u bytes, this plugin was built for %zu: FSR 4 is not used",
                 di->structSize, sizeof *di);
        }
    }
    r = used ? FFX_VK_PORTABLE_OK : orig_record(context, di);
    status_tick(used);
    return r;
}

static FfxVkPortableResult hook_create(const FfxVkPortableDeviceInfo *device, const FfxVkPortableUpscaleCreateInfo *ci,
                                       FfxVkPortableUpscaleContext **out) {
    if (device && device->structSize >= offsetof(FfxVkPortableDeviceInfo, getDeviceProcAddr) + sizeof device->getDeviceProcAddr)
        fsr4rt_adopt_game_handles(device->instance, device->physicalDevice, device->device, device->getDeviceProcAddr);
    if (ci && ci->structSize >= offsetof(FfxVkPortableUpscaleCreateInfo, maxOutputSize) + sizeof ci->maxOutputSize)
        fsr4rt_on_context_create(ci->flags, ci->maxRenderSize.width, ci->maxRenderSize.height, ci->maxOutputSize.width,
                                 ci->maxOutputSize.height);
    return orig_create(device, ci, out);
}

static bool load_exe_file(void) {
    if (exe_bytes) return true;
    wchar_t path[MAX_PATH];
    if (!GetModuleFileNameW(NULL, path, MAX_PATH)) return false;
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size;
    bool ok = GetFileSizeEx(h, &size) && size.QuadPart > 0 && size.QuadPart < (1ll << 30);
    if (ok) {
        exe_bytes = malloc((size_t)size.QuadPart);
        DWORD got = 0;
        ok = exe_bytes && ReadFile(h, exe_bytes, (DWORD)size.QuadPart, &got, NULL) && got == (DWORD)size.QuadPart;
        if (ok) exe_size = (size_t)size.QuadPart;
        else free(exe_bytes), exe_bytes = NULL;
    }
    CloseHandle(h);
    return ok;
}

static void *find_function(const char *name) {
    HMODULE exe = GetModuleHandleW(NULL);
    void *p = (void *)GetProcAddress(exe, name);
    if (p) {
        plog("%s found in the exe's export table", name);
        return p;
    }
    uint32_t rva;
    if (load_exe_file() && coff_find_symbol(exe_bytes, exe_size, name, &rva)) {
        plog("%s found in the exe's symbol table", name);
        return (uint8_t *)exe + rva;
    }
    plog("%s was not found in the exe", name);
    return NULL;
}

bool hook_active(void) { return installed; }

bool hook_install(void) {
    if (installed) return true;
    MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) {
        plog("MinHook failed to start (%s)", MH_StatusToString(st));
        return false;
    }
    void *record = find_function(NAME_RECORD);
    if (!record) {
        plog("the game's FSR dispatch function is missing: the plugin does nothing and FSR 3.1 runs");
        return false;
    }
    st = MH_CreateHook(record, (void *)hook_record, (void **)&orig_record);
    if (st != MH_OK) {
        plog("hooking %s failed (%s)", NAME_RECORD, MH_StatusToString(st));
        return false;
    }
    void *create = find_function(NAME_CREATE);
    if (create && MH_CreateHook(create, (void *)hook_create, (void **)&orig_create) != MH_OK) orig_create = NULL;
    st = MH_EnableHook(MH_ALL_HOOKS);
    if (st != MH_OK) {
        plog("enabling the hooks failed (%s)", MH_StatusToString(st));
        return false;
    }
    free(exe_bytes);
    exe_bytes = NULL;
    installed = true;
    plog("hooked %s%s", NAME_RECORD, orig_create ? " and " NAME_CREATE : "");
    return true;
}
