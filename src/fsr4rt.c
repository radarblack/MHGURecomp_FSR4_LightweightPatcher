#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "plugin.h"

#include <ffx_api.h>
#include <ffx_upscale.h>
#include <vk/ffx_api_vk.h>

#define LOADER_DLL L"amd_fidelityfx_loader_vk.dll"
#define UPSCALER_DLL L"amd_fidelityfx_upscaler_vk.dll"
#define LEGACY_LOADER_DLL L"amd_fidelityfx_vk.dll"

typedef struct {
    uint64_t id;
    char name[64];
    uint32_t major, minor, patch;
    bool from_driver;
} FsrVersion;

static PfnFfxCreateContext f_create;
static PfnFfxDestroyContext f_destroy;
static PfnFfxDispatch f_dispatch;
static PfnFfxQuery f_query;
static ffxContext ctx;
static uint64_t chosen_id;
static char prov_name[64];
static uint32_t ctx_flags, ctx_max_rw, ctx_max_rh, ctx_max_ow, ctx_max_oh;
static VkInstance g_inst;
static VkPhysicalDevice g_phys;
static VkDevice g_dev;
static PFN_vkGetDeviceProcAddr g_gdpa;
static uint32_t create_flags, create_rw, create_rh, create_ow, create_oh;
static bool create_seen, ctx_stale, device_ready, prepared, failed, linear_in;
static uint64_t frames;

void fsr4rt_set_device(VkInstance inst, VkPhysicalDevice phys, VkDevice dev) {
    g_inst = inst, g_phys = phys, g_dev = dev;
    HMODULE vk = GetModuleHandleW(L"vulkan-1.dll");
    g_gdpa = vk ? (PFN_vkGetDeviceProcAddr)(void *)GetProcAddress(vk, "vkGetDeviceProcAddr") : NULL;
}

void fsr4rt_set_device_ready(bool ready) { device_ready = ready && g_gdpa; }

void fsr4rt_adopt_game_handles(VkInstance inst, VkPhysicalDevice phys, VkDevice dev, PFN_vkGetDeviceProcAddr gdpa) {
    if (!phys || !dev) return;
    if (phys != g_phys) plog("using the game's physical device handle %p for AMD's DLLs (the layer chain's own handle %p is not valid for the loader)", (void *)phys, (void *)g_phys);
    if (dev != g_dev) plog("the game's VkDevice %p differs from the one the layer created %p", (void *)dev, (void *)g_dev);
    g_inst = inst ? inst : g_inst;
    g_phys = phys;
    g_dev = dev;
    if (gdpa) g_gdpa = gdpa;
}

void fsr4rt_on_context_create(uint32_t flags, uint32_t render_w, uint32_t render_h, uint32_t out_w, uint32_t out_h) {
    create_flags = flags, create_rw = render_w, create_rh = render_h, create_ow = out_w, create_oh = out_h;
    create_seen = true;
    ctx_stale = true;
    plog("the game created its FSR 3.1 context: flags %#x, %ux%u -> %ux%u", flags, render_w, render_h, out_w, out_h);
}

bool fsr4rt_usable(void) { return device_ready && !failed; }
const char *fsr4rt_name(void) { return prov_name[0] ? prov_name : "FSR 4"; }

static void file_version(const wchar_t *path, char *out, size_t n) {
    snprintf(out, n, "?");
    DWORD z, sz = GetFileVersionInfoSizeW(path, &z);
    if (!sz) return;
    void *buf = malloc(sz);
    VS_FIXEDFILEINFO *fi;
    UINT l;
    if (buf && GetFileVersionInfoW(path, 0, sz, buf) && VerQueryValueW(buf, L"\\", (void **)&fi, &l) && fi)
        snprintf(out, n, "%u.%u.%u.%u", HIWORD(fi->dwFileVersionMS), LOWORD(fi->dwFileVersionMS), HIWORD(fi->dwFileVersionLS),
                 LOWORD(fi->dwFileVersionLS));
    free(buf);
}

static bool file_exists(const wchar_t *path) { return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES; }

static void exe_folder(wchar_t *dir) {
    dir[0] = 0;
    GetModuleFileNameW(NULL, dir, MAX_PATH);
    wchar_t *slash = wcsrchr(dir, L'\\');
    if (slash) slash[1] = 0;
}

static bool locate_loader(wchar_t *path) {
    const char *env = settings_text("MHGU_FSR4_LOADER");
    if (env) {
        MultiByteToWideChar(CP_UTF8, 0, env, -1, path, MAX_PATH);
        if (file_exists(path)) return true;
        plog("MHGU_FSR4_LOADER=%s does not exist", env);
        return false;
    }
    wchar_t folders[3][MAX_PATH] = {{0}};
    plugin_dir(folders[0], MAX_PATH);
    exe_folder(folders[1]);
    swprintf(folders[2], MAX_PATH, L"%ls%ls", folders[1], L"fsr4\\");
    for (int i = 0; i < 3; i++) {
        if (!folders[i][0]) continue;
        swprintf(path, MAX_PATH, L"%ls%ls", folders[i], LOADER_DLL);
        if (file_exists(path)) return true;
    }
    swprintf(path, MAX_PATH, L"%ls%ls", folders[1], LEGACY_LOADER_DLL);
    if (file_exists(path))
        plog("found %ls: that is the loader of AMD's SDK 1.x, not the one needed; use %ls and %ls from SDK 2.4 or newer",
             LEGACY_LOADER_DLL, LOADER_DLL, UPSCALER_DLL);
    else
        plog("no %ls in the fsr4plugin folder, beside the exe or in fsr4\\ there (or set MHGU_FSR4_LOADER=<path>)", LOADER_DLL);
    return false;
}

static bool upscaler_dll_present(const wchar_t *loader_path) {
    if (GetModuleHandleW(UPSCALER_DLL)) return true;
    wchar_t dir[MAX_PATH], cand[MAX_PATH];
    wcsncpy(dir, loader_path, MAX_PATH - 1);
    dir[MAX_PATH - 1] = 0;
    wchar_t *slash = wcsrchr(dir, L'\\');
    if (slash) {
        slash[1] = 0;
        swprintf(cand, MAX_PATH, L"%ls%ls", dir, UPSCALER_DLL);
        if (file_exists(cand)) return true;
    }
    exe_folder(dir);
    swprintf(cand, MAX_PATH, L"%ls%ls", dir, UPSCALER_DLL);
    return file_exists(cand);
}

static bool parse_version(const char *name, uint32_t *major, uint32_t *minor, uint32_t *patch) {
    const char *p = name;
    while (*p && (*p < '0' || *p > '9')) p++;
    unsigned a = 0, b = 0, c = 0;
    int n = sscanf(p, "%u.%u.%u", &a, &b, &c);
    if (n < 2) return false;
    *major = a, *minor = b, *patch = n == 3 ? c : 0;
    return true;
}

static int compare_versions(const FsrVersion *a, const FsrVersion *b) {
    if (a->major != b->major) return a->major > b->major ? 1 : -1;
    if (a->minor != b->minor) return a->minor > b->minor ? 1 : -1;
    if (a->patch != b->patch) return a->patch > b->patch ? 1 : -1;
    if (a->from_driver != b->from_driver) return a->from_driver ? -1 : 1;
    return 0;
}

static bool pick_version(void) {
    VkDeviceContext device = {.vkDevice = g_dev, .vkPhysicalDevice = g_phys, .vkDeviceProcAddr = g_gdpa};
    uint64_t count = 0;
    struct ffxQueryDescGetVersions query = {.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS,
                                            .createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE,
                                            .device = &device,
                                            .outputCount = &count};
    ffxReturnCode_t rc = f_query(NULL, &query.header);
    if (rc != FFX_API_RETURN_OK || !count) {
        plog("the loader's upscaler version query failed (ffx return code %d, %llu versions)", (int)rc, (unsigned long long)count);
        return false;
    }
    uint64_t capacity = count;
    uint64_t *ids = calloc((size_t)capacity, sizeof *ids);
    const char **names = calloc((size_t)capacity, sizeof *names);
    FsrVersion *versions = calloc((size_t)capacity, sizeof *versions);
    if (!ids || !names || !versions) {
        free(ids), free(names), free(versions);
        return false;
    }
    query.outputCount = &capacity;
    query.versionIds = ids;
    query.versionNames = names;
    rc = f_query(NULL, &query.header);
    if (rc != FFX_API_RETURN_OK) {
        plog("the loader's upscaler version list failed (ffx return code %d)", (int)rc);
        free(ids), free(names), free(versions);
        return false;
    }
    const char *wanted = settings_text("MHGU_FSR4_VERSION");
    int best = -1;
    for (uint64_t i = 0; i < capacity && i < count; i++) {
        FsrVersion *v = &versions[i];
        v->id = ids[i];
        snprintf(v->name, sizeof v->name, "%s", names[i] ? names[i] : "?");
        v->from_driver = strchr(v->name, '*') != NULL || ((ids[i] >> 31) & 1);
        bool parsed = parse_version(v->name, &v->major, &v->minor, &v->patch);
        plog("loader offers upscaler \"%s\" (id %#llx)%s", v->name, (unsigned long long)v->id, v->from_driver ? " from the driver" : "");
        if (!parsed || v->major != 4) continue;
        if (wanted && !strstr(v->name, wanted)) continue;
        if (best < 0 || compare_versions(v, &versions[best]) > 0) best = (int)i;
    }
    bool ok = best >= 0;
    if (ok) {
        chosen_id = versions[best].id;
        snprintf(prov_name, sizeof prov_name, "FSR %u.%u.%u%s", versions[best].major, versions[best].minor, versions[best].patch,
                 versions[best].from_driver ? " (driver)" : "");
    } else {
        plog("none of the offered upscalers is FSR 4%s: the DLLs or the driver do not support FSR 4 on this GPU",
             wanted ? " (or none matches MHGU_FSR4_VERSION)" : "");
    }
    free(ids), free(names), free(versions);
    return ok;
}

static bool prepare(void) {
    prepared = true;
    wchar_t path[MAX_PATH] = {0};
    if (!locate_loader(path)) return false;
    guard_enter("loading AMD loader and asking for FSR versions");
    if (!upscaler_dll_present(path)) {
        plog("%ls is missing next to the loader or the exe: the loader has no upscaler to offer", UPSCALER_DLL);
        guard_leave();
        return false;
    }
    HMODULE loader = LoadLibraryExW(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!loader) {
        guard_leave();
        DWORD err = GetLastError();
        plog("%ls could not be loaded (error %lu)%s", path, err,
             err == ERROR_MOD_NOT_FOUND || err == ERROR_BAD_EXE_FORMAT
                 ? ": the Microsoft Visual C++ 2015-2022 x64 redistributable may be missing"
                 : "");
        return false;
    }
    char version[32];
    file_version(path, version, sizeof version);
    plog("loader %ls, file version %s", path, version);
    f_query = (PfnFfxQuery)(void *)GetProcAddress(loader, "ffxQuery");
    f_create = (PfnFfxCreateContext)(void *)GetProcAddress(loader, "ffxCreateContext");
    f_destroy = (PfnFfxDestroyContext)(void *)GetProcAddress(loader, "ffxDestroyContext");
    f_dispatch = (PfnFfxDispatch)(void *)GetProcAddress(loader, "ffxDispatch");
    if (!f_query || !f_create || !f_destroy || !f_dispatch) {
        plog("the loader lacks the ffx-api exports");
        guard_leave();
        return false;
    }
    plog("asking the loader for FSR versions: device %p, physical device %p, vkGetDeviceProcAddr %p", (void *)g_dev, (void *)g_phys, (void *)g_gdpa);
    bool picked = pick_version();
    guard_leave();
    if (!picked) return false;
    linear_in = settings_flag("MHGU_FSR4_LINEAR");
    plog("%s selected through AMD's loader", prov_name);
    return true;
}

static void ffx_message(uint32_t type, const wchar_t *msg) {
    plog("runtime %s: %ls", type == FFX_API_MESSAGE_TYPE_ERROR ? "ERROR" : "warning", msg);
}

static uint32_t ffx_format(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R32G32B32A32_SFLOAT: return FFX_API_SURFACE_FORMAT_R32G32B32A32_FLOAT;
    case VK_FORMAT_R16G16B16A16_SFLOAT: return FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT;
    case VK_FORMAT_R32G32_SFLOAT: return FFX_API_SURFACE_FORMAT_R32G32_FLOAT;
    case VK_FORMAT_R8G8B8A8_UNORM: return FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_R8G8B8A8_SNORM: return FFX_API_SURFACE_FORMAT_R8G8B8A8_SNORM;
    case VK_FORMAT_R8G8B8A8_SRGB: return FFX_API_SURFACE_FORMAT_R8G8B8A8_SRGB;
    case VK_FORMAT_B8G8R8A8_UNORM: return FFX_API_SURFACE_FORMAT_B8G8R8A8_UNORM;
    case VK_FORMAT_B8G8R8A8_SRGB: return FFX_API_SURFACE_FORMAT_B8G8R8A8_SRGB;
    case VK_FORMAT_B10G11R11_UFLOAT_PACK32: return FFX_API_SURFACE_FORMAT_R11G11B10_FLOAT;
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return FFX_API_SURFACE_FORMAT_R10G10B10A2_UNORM;
    case VK_FORMAT_R16G16_SFLOAT: return FFX_API_SURFACE_FORMAT_R16G16_FLOAT;
    case VK_FORMAT_R16_SFLOAT: return FFX_API_SURFACE_FORMAT_R16_FLOAT;
    case VK_FORMAT_R16_UNORM:
    case VK_FORMAT_D16_UNORM: return FFX_API_SURFACE_FORMAT_R16_UNORM;
    case VK_FORMAT_R8_UNORM: return FFX_API_SURFACE_FORMAT_R8_UNORM;
    case VK_FORMAT_R32_UINT:
    case VK_FORMAT_D24_UNORM_S8_UINT:
    case VK_FORMAT_X8_D24_UNORM_PACK32: return FFX_API_SURFACE_FORMAT_R32_UINT;
    case VK_FORMAT_R32_SFLOAT:
    case VK_FORMAT_D32_SFLOAT:
    case VK_FORMAT_D32_SFLOAT_S8_UINT: return FFX_API_SURFACE_FORMAT_R32_FLOAT;
    default: return FFX_API_SURFACE_FORMAT_UNKNOWN;
    }
}

static uint32_t ffx_state(FfxVkPortableResourceState s) {
    switch (s) {
    case FFX_VK_PORTABLE_RESOURCE_STATE_GENERIC_READ: return FFX_API_RESOURCE_STATE_GENERIC_READ;
    case FFX_VK_PORTABLE_RESOURCE_STATE_COMPUTE_READ: return FFX_API_RESOURCE_STATE_COMPUTE_READ;
    case FFX_VK_PORTABLE_RESOURCE_STATE_UNORDERED_ACCESS: return FFX_API_RESOURCE_STATE_UNORDERED_ACCESS;
    case FFX_VK_PORTABLE_RESOURCE_STATE_TRANSFER_SOURCE: return FFX_API_RESOURCE_STATE_COPY_SRC;
    case FFX_VK_PORTABLE_RESOURCE_STATE_TRANSFER_DESTINATION: return FFX_API_RESOURCE_STATE_COPY_DEST;
    case FFX_VK_PORTABLE_RESOURCE_STATE_COLOR_ATTACHMENT: return FFX_API_RESOURCE_STATE_RENDER_TARGET;
    case FFX_VK_PORTABLE_RESOURCE_STATE_DEPTH_ATTACHMENT: return FFX_API_RESOURCE_STATE_DEPTH_ATTACHMENT;
    case FFX_VK_PORTABLE_RESOURCE_STATE_PRESENT: return FFX_API_RESOURCE_STATE_PRESENT;
    default: return FFX_API_RESOURCE_STATE_COMMON;
    }
}

static struct FfxApiResource resource(const FfxVkPortableImage *i, bool uav) {
    struct FfxApiResource r = {0};
    if (!i->image) return r;
    r.resource = (void *)i->image;
    r.description.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
    r.description.format = ffx_format(i->format);
    r.description.width = i->extent.width, r.description.height = i->extent.height, r.description.depth = 1;
    r.description.mipCount = i->mipCount ? i->mipCount : 1;
    r.description.usage = uav ? FFX_API_RESOURCE_USAGE_UAV : FFX_API_RESOURCE_USAGE_READ_ONLY;
    r.state = ffx_state(i->state);
    return r;
}

static bool fail(const char *why, int rc) {
    plog("%s (ffx return code %d): FSR 4 is off from here on, FSR 3.1 takes over", why, rc);
    failed = true;
    return false;
}

static bool make_context(const FfxVkPortableUpscaleDispatchInfo *di) {
    uint32_t rw = di->renderSize.width, rh = di->renderSize.height, ow = di->outputSize.width, oh = di->outputSize.height;
    if (create_seen) {
        if (create_rw > rw) rw = create_rw;
        if (create_rh > rh) rh = create_rh;
        if (create_ow > ow) ow = create_ow;
        if (create_oh > oh) oh = create_oh;
    }
    bool fits = ctx && !ctx_stale && di->renderSize.width <= ctx_max_rw && di->renderSize.height <= ctx_max_rh &&
                di->outputSize.width <= ctx_max_ow && di->outputSize.height <= ctx_max_oh;
    if (fits) return true;
    if (ctx) {
        f_destroy(&ctx, NULL);
        ctx = NULL;
    }
    uint32_t flags = create_flags & 0x7fu;
    if (!(create_flags & FFX_VK_PORTABLE_CONTEXT_HDR_COLOR_INPUT) && !linear_in) flags |= FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE;
    if ((create_flags & FFX_VK_PORTABLE_CONTEXT_DEBUG_CHECKING) || settings_flag("MHGU_FSR_DEBUG")) flags |= FFX_UPSCALE_ENABLE_DEBUG_CHECKING;
    struct ffxCreateContextDescUpscale cu = {.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE,
                                             .flags = flags,
                                             .maxRenderSize = {rw, rh},
                                             .maxUpscaleSize = {ow, oh},
                                             .fpMessage = ffx_message};
    struct ffxCreateBackendVKDesc backend = {.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK,
                                             .deviceContext = {.vkDevice = g_dev, .vkPhysicalDevice = g_phys, .vkDeviceProcAddr = g_gdpa}};
    struct ffxOverrideVersion override = {.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION, .versionId = chosen_id};
    struct ffxCreateContextDescUpscaleVersion api = {.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION,
                                                     .version = FFX_UPSCALER_VERSION};
    cu.header.pNext = &backend.header;
    backend.header.pNext = &override.header;
    override.header.pNext = &api.header;
    LARGE_INTEGER t0, t1, freq;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    guard_enter("creating the FSR 4 context");
    ffxReturnCode_t rc = f_create(&ctx, &cu.header, NULL);
    guard_leave();
    if (rc != FFX_API_RETURN_OK || !ctx) {
        ctx = NULL;
        return fail("the FSR 4 context could not be created", (int)rc);
    }
    QueryPerformanceCounter(&t1);
    ctx_flags = flags;
    ctx_max_rw = rw, ctx_max_rh = rh, ctx_max_ow = ow, ctx_max_oh = oh;
    ctx_stale = false;
    plog("%s context %ux%u -> %ux%u created in %.0f ms (flags %#x; colour VkFormat %d, depth %d, motion vectors %d, output %d)",
         prov_name, rw, rh, ow, oh, (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)freq.QuadPart, flags, di->color.format,
         di->depth.format, di->motionVectors.format, di->output.format);
    return true;
}

bool fsr4rt_dispatch(const FfxVkPortableUpscaleDispatchInfo *di) {
    if (!fsr4rt_usable()) return false;
    if (!prepared && !prepare()) {
        failed = true;
        plog("no FSR 4 available: FSR 3.1 is used");
        return false;
    }
    if (failed || !make_context(di)) return false;
    float near_plane = di->cameraNear, far_plane = di->cameraFar, fov = di->cameraVerticalFovRadians;
    if (!(near_plane > 0.0f) || !(far_plane > near_plane)) near_plane = 8.0f, far_plane = 50000.0f;
    if (!(fov > 0.05f && fov < 3.0f)) fov = 1.0f;
    struct ffxDispatchDescUpscale du = {.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE, .commandList = di->commandBuffer};
    du.color = resource(&di->color, false);
    du.depth = resource(&di->depth, false);
    du.motionVectors = resource(&di->motionVectors, false);
    du.exposure = resource(&di->exposure, false);
    du.reactive = resource(&di->reactiveMask, false);
    du.transparencyAndComposition = resource(&di->transparencyAndCompositionMask, false);
    du.output = resource(&di->output, true);
    du.jitterOffset = (struct FfxApiFloatCoords2D){di->jitterOffset.x, di->jitterOffset.y};
    du.motionVectorScale = (struct FfxApiFloatCoords2D){di->motionVectorScale.x, di->motionVectorScale.y};
    du.renderSize = (struct FfxApiDimensions2D){di->renderSize.width, di->renderSize.height};
    du.upscaleSize = (struct FfxApiDimensions2D){di->outputSize.width, di->outputSize.height};
    du.enableSharpening = di->enableSharpening != 0;
    du.sharpness = di->sharpness;
    du.frameTimeDelta = di->frameTimeMilliseconds;
    du.preExposure = di->preExposure > 0.0f ? di->preExposure : 1.0f;
    du.reset = di->reset != 0;
    du.cameraNear = near_plane;
    du.cameraFar = far_plane;
    du.cameraFovAngleVertical = fov;
    du.viewSpaceToMetersFactor = di->viewSpaceToMeters > 0.0f ? di->viewSpaceToMeters : 0.01f;
    du.flags = (ctx_flags & FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE) ? FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB : 0;
    static int fail_at = -2;
    if (fail_at == -2) fail_at = settings_text("MHGU_FSR4_FAILTEST") ? atoi(settings_text("MHGU_FSR4_FAILTEST")) : -1;
    bool guarded = frames < 3;
    LARGE_INTEGER d0, d1, dfreq;
    QueryPerformanceFrequency(&dfreq);
    QueryPerformanceCounter(&d0);
    if (guarded) guard_enter("recording the first FSR 4 dispatches");
    ffxReturnCode_t rc = fail_at >= 0 && frames == (uint64_t)fail_at ? FFX_API_RETURN_ERROR : f_dispatch(&ctx, &du.header);
    if (guarded) guard_leave();
    QueryPerformanceCounter(&d1);
    if (rc != FFX_API_RETURN_OK) return fail("the FSR 4 dispatch failed", (int)rc);
    if (++frames == 1)
        plog("first %s dispatch recorded after %.0f ms (AMD's runtime prepares the model on first use)", prov_name,
             (double)(d1.QuadPart - d0.QuadPart) * 1000.0 / (double)dfreq.QuadPart);
    return true;
}
