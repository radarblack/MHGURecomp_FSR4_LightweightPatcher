#include <stdlib.h>
#include <string.h>

#include <vulkan/vk_layer.h>

#include "plugin.h"

#define MAX_OBJECTS 16
#define LAYER_EXPORT __declspec(dllexport)

typedef struct {
    void *key;
    VkInstance instance;
    PFN_vkGetInstanceProcAddr gipa;
    PFN_vkDestroyInstance destroy;
    PFN_vkCreateDevice create_device;
    DevcfgFns fns;
    bool active;
} InstanceData;

typedef struct {
    void *key;
    PFN_vkGetDeviceProcAddr gdpa;
    PFN_vkDestroyDevice destroy;
} DeviceData;

static InstanceData instances[MAX_OBJECTS];
static DeviceData devices[MAX_OBJECTS];
static SRWLOCK lock = SRWLOCK_INIT;
static bool active_decided, active_value;

static void *dispatch_key(const void *handle) { return *(void *const *)handle; }

static InstanceData *find_instance(const void *handle) {
    void *key = dispatch_key(handle);
    for (int i = 0; i < MAX_OBJECTS; i++)
        if (instances[i].key == key) return &instances[i];
    return NULL;
}

static DeviceData *find_device(const void *handle) {
    void *key = dispatch_key(handle);
    for (int i = 0; i < MAX_OBJECTS; i++)
        if (devices[i].key == key) return &devices[i];
    return NULL;
}

static bool layer_active(void) {
    if (!active_decided) {
        active_decided = true;
        active_value = settings_in_game_exe() && settings_fsr4_requested();
        if (active_value) {
            guard_init();
            if (guard_blocked()) active_value = false;
        }
        plog("layer loaded in %s (plugin %s), FSR 4 %s", settings_in_game_exe() ? "mhgurecomp.exe" : "another program", PLUGIN_VERSION,
             active_value ? "requested" : "off (Upscaler = FSR3, not the game, or the crash guard): the layer passes everything through");
    }
    return active_value;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL layer_gdpa(VkDevice device, const char *name);
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL layer_gipa(VkInstance instance, const char *name);

static VKAPI_ATTR VkResult VKAPI_CALL layer_create_instance(const VkInstanceCreateInfo *ci, const VkAllocationCallbacks *alloc,
                                                            VkInstance *out) {
    VkLayerInstanceCreateInfo *link = (VkLayerInstanceCreateInfo *)ci->pNext;
    while (link && !(link->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO && link->function == VK_LAYER_LINK_INFO))
        link = (VkLayerInstanceCreateInfo *)link->pNext;
    if (!link) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkGetInstanceProcAddr next_gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkCreateInstance next_create = (PFN_vkCreateInstance)next_gipa(NULL, "vkCreateInstance");
    if (!next_create) return VK_ERROR_INITIALIZATION_FAILED;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;

    bool active = layer_active();
    VkInstanceCreateInfo copy = *ci;
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO};
    if (active) {
        if (ci->pApplicationInfo) app = *ci->pApplicationInfo;
        if (app.apiVersion < VK_API_VERSION_1_3) {
            plog("instance: Vulkan API version %u.%u raised to 1.3 for FSR 4's shaders", VK_API_VERSION_MAJOR(app.apiVersion),
                 VK_API_VERSION_MINOR(app.apiVersion));
            app.apiVersion = VK_API_VERSION_1_3;
        }
        copy.pApplicationInfo = &app;
    }
    VkResult r = next_create(&copy, alloc, out);
    if (r != VK_SUCCESS) return r;

    AcquireSRWLockExclusive(&lock);
    for (int i = 0; i < MAX_OBJECTS; i++) {
        if (instances[i].key) continue;
        InstanceData *d = &instances[i];
        d->key = dispatch_key(*out);
        d->instance = *out;
        d->gipa = next_gipa;
        d->destroy = (PFN_vkDestroyInstance)next_gipa(*out, "vkDestroyInstance");
        d->create_device = (PFN_vkCreateDevice)next_gipa(*out, "vkCreateDevice");
        d->fns.features2 = (PFN_vkGetPhysicalDeviceFeatures2)next_gipa(*out, "vkGetPhysicalDeviceFeatures2");
        if (!d->fns.features2) d->fns.features2 = (PFN_vkGetPhysicalDeviceFeatures2)next_gipa(*out, "vkGetPhysicalDeviceFeatures2KHR");
        d->fns.props2 = (PFN_vkGetPhysicalDeviceProperties2)next_gipa(*out, "vkGetPhysicalDeviceProperties2");
        if (!d->fns.props2) d->fns.props2 = (PFN_vkGetPhysicalDeviceProperties2)next_gipa(*out, "vkGetPhysicalDeviceProperties2KHR");
        d->fns.props = (PFN_vkGetPhysicalDeviceProperties)next_gipa(*out, "vkGetPhysicalDeviceProperties");
        d->fns.enum_ext = (PFN_vkEnumerateDeviceExtensionProperties)next_gipa(*out, "vkEnumerateDeviceExtensionProperties");
        d->active = active;
        break;
    }
    ReleaseSRWLockExclusive(&lock);
    if (active) hook_install();
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL layer_destroy_instance(VkInstance instance, const VkAllocationCallbacks *alloc) {
    AcquireSRWLockExclusive(&lock);
    InstanceData *d = find_instance(instance);
    PFN_vkDestroyInstance destroy = d ? d->destroy : NULL;
    if (d) memset(d, 0, sizeof *d);
    ReleaseSRWLockExclusive(&lock);
    if (destroy) destroy(instance, alloc);
}

static VKAPI_ATTR VkResult VKAPI_CALL layer_create_device(VkPhysicalDevice phys, const VkDeviceCreateInfo *ci,
                                                          const VkAllocationCallbacks *alloc, VkDevice *out) {
    VkLayerDeviceCreateInfo *link = (VkLayerDeviceCreateInfo *)ci->pNext;
    while (link && !(link->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO && link->function == VK_LAYER_LINK_INFO))
        link = (VkLayerDeviceCreateInfo *)link->pNext;
    AcquireSRWLockShared(&lock);
    InstanceData *found = find_instance(phys);
    InstanceData inst = found ? *found : (InstanceData){0};
    ReleaseSRWLockShared(&lock);
    if (!link || !inst.key) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkGetInstanceProcAddr next_gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr next_gdpa = link->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    PFN_vkCreateDevice next_create = (PFN_vkCreateDevice)next_gipa(inst.instance, "vkCreateDevice");
    if (!next_create) return VK_ERROR_INITIALIZATION_FAILED;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;

    DevcfgState *state = inst.active && inst.fns.props && inst.fns.enum_ext ? devcfg_build(&inst.fns, phys, ci) : NULL;
    VkResult r = next_create(phys, state ? devcfg_info(state) : ci, alloc, out);
    if (state) {
        devcfg_restore(state);
        if (r != VK_SUCCESS) {
            plog("vkCreateDevice failed (%d) with FSR 4's extensions and features: creating the device without them", (int)r);
            devcfg_free(state);
            state = NULL;
            r = next_create(phys, ci, alloc, out);
        }
    }
    if (r != VK_SUCCESS) return r;

    AcquireSRWLockExclusive(&lock);
    for (int i = 0; i < MAX_OBJECTS; i++) {
        if (devices[i].key) continue;
        devices[i].key = dispatch_key(*out);
        devices[i].gdpa = next_gdpa;
        devices[i].destroy = (PFN_vkDestroyDevice)next_gdpa(*out, "vkDestroyDevice");
        break;
    }
    ReleaseSRWLockExclusive(&lock);
    if (inst.active) {
        fsr4rt_set_device(inst.instance, phys, *out);
        fsr4rt_set_device_ready(state != NULL);
    }
    devcfg_free(state);
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL layer_destroy_device(VkDevice device, const VkAllocationCallbacks *alloc) {
    AcquireSRWLockExclusive(&lock);
    DeviceData *d = find_device(device);
    PFN_vkDestroyDevice destroy = d ? d->destroy : NULL;
    if (d) memset(d, 0, sizeof *d);
    ReleaseSRWLockExclusive(&lock);
    if (destroy) destroy(device, alloc);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL layer_gdpa(VkDevice device, const char *name) {
    if (!strcmp(name, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)layer_gdpa;
    if (!strcmp(name, "vkDestroyDevice")) return (PFN_vkVoidFunction)layer_destroy_device;
    AcquireSRWLockShared(&lock);
    DeviceData *d = device ? find_device(device) : NULL;
    PFN_vkGetDeviceProcAddr next = d ? d->gdpa : NULL;
    ReleaseSRWLockShared(&lock);
    return next ? next(device, name) : NULL;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL layer_gipa(VkInstance instance, const char *name) {
    if (!strcmp(name, "vkGetInstanceProcAddr")) return (PFN_vkVoidFunction)layer_gipa;
    if (!strcmp(name, "vkCreateInstance")) return (PFN_vkVoidFunction)layer_create_instance;
    if (!strcmp(name, "vkDestroyInstance")) return (PFN_vkVoidFunction)layer_destroy_instance;
    if (!strcmp(name, "vkCreateDevice")) return (PFN_vkVoidFunction)layer_create_device;
    if (!strcmp(name, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)layer_gdpa;
    AcquireSRWLockShared(&lock);
    InstanceData *d = instance ? find_instance(instance) : NULL;
    PFN_vkGetInstanceProcAddr next = d ? d->gipa : NULL;
    ReleaseSRWLockShared(&lock);
    return next ? next(instance, name) : NULL;
}

LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface *v) {
    if (!v || v->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT) return VK_ERROR_INITIALIZATION_FAILED;
    if (v->loaderLayerInterfaceVersion >= 2) {
        v->pfnGetInstanceProcAddr = layer_gipa;
        v->pfnGetDeviceProcAddr = layer_gdpa;
        v->pfnGetPhysicalDeviceProcAddr = NULL;
    }
    if (v->loaderLayerInterfaceVersion > CURRENT_LOADER_LAYER_INTERFACE_VERSION) v->loaderLayerInterfaceVersion = CURRENT_LOADER_LAYER_INTERFACE_VERSION;
    return VK_SUCCESS;
}
