#include <stdlib.h>
#include <string.h>

#include "plugin.h"

#define MAX_TOUCHES 24

typedef struct Link {
    VkStructureType sType;
    struct Link *pNext;
} Link;

typedef struct {
    void *ptr;
    size_t size;
    void *backup;
} Touch;

struct DevcfgState {
    VkDeviceCreateInfo info;
    const char **names;
    Touch touches[MAX_TOUCHES];
    int touch_count;
    Link *tail;
    Link *tail_saved;
    Link *last;
    bool appended;
    VkPhysicalDeviceFeatures2 own_f2;
    VkPhysicalDeviceVulkan11Features own_f11;
    VkPhysicalDeviceVulkan12Features own_f12;
    VkPhysicalDeviceSynchronization2Features own_sync2;
    VkPhysicalDeviceSubgroupSizeControlFeatures own_subgroup;
    VkPhysicalDeviceShaderIntegerDotProductFeatures own_dot;
    VkPhysicalDeviceMaintenance4Features own_maint4;
    VkPhysicalDeviceCoherentMemoryFeaturesAMD own_coherent;
#ifdef VK_KHR_compute_shader_derivatives
    VkPhysicalDeviceComputeShaderDerivativesFeaturesKHR own_deriv;
#endif
#ifdef VK_KHR_cooperative_matrix
    VkPhysicalDeviceCooperativeMatrixFeaturesKHR own_coop;
#endif
#ifdef VK_EXT_shader_float8
    VkPhysicalDeviceShaderFloat8FeaturesEXT own_float8;
#endif
};

static Link *find_struct(const void *chain, VkStructureType type) {
    for (const Link *l = chain; l; l = l->pNext)
        if (l->sType == type) return (Link *)l;
    return NULL;
}

static Link *tail_of(const void *chain) {
    const Link *l = chain;
    while (l && l->pNext) l = l->pNext;
    return (Link *)l;
}

static void *touch(DevcfgState *s, void *p, size_t size) {
    if (s->touch_count >= MAX_TOUCHES) return p;
    void *copy = malloc(size);
    if (!copy) return p;
    memcpy(copy, p, size);
    s->touches[s->touch_count++] = (Touch){p, size, copy};
    return p;
}

static void append(DevcfgState *s, void *own) {
    Link *l = own;
    l->pNext = NULL;
    if (!s->appended) {
        s->tail = tail_of(s->info.pNext);
        s->tail_saved = s->tail->pNext;
        s->tail->pNext = l;
        s->appended = true;
    } else {
        s->last->pNext = l;
    }
    s->last = l;
}

static bool has_extension(const VkExtensionProperties *ep, uint32_t n, const char *name) {
    for (uint32_t i = 0; i < n; i++)
        if (!strcmp(ep[i].extensionName, name)) return true;
    return false;
}

static bool app_has_extension(const VkDeviceCreateInfo *ci, const char *name) {
    for (uint32_t i = 0; i < ci->enabledExtensionCount; i++)
        if (!strcmp(ci->ppEnabledExtensionNames[i], name)) return true;
    return false;
}

static void add_name(DevcfgState *s, const VkExtensionProperties *ep, uint32_t n, const char *name, bool *present) {
    *present = has_extension(ep, n, name);
    if (*present && !app_has_extension(&s->info, name)) s->names[s->info.enabledExtensionCount++] = name;
}

DevcfgState *devcfg_build(const DevcfgFns *fns, VkPhysicalDevice phys, const VkDeviceCreateInfo *in) {
    VkPhysicalDeviceProperties props = {0};
    fns->props(phys, &props);
    plog("device: %s, vendor %04x device %04x, Vulkan %u.%u", props.deviceName, props.vendorID, props.deviceID,
         VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion));
    if (props.apiVersion < VK_API_VERSION_1_2 || !fns->features2 || !fns->props2) {
        plog("device: Vulkan 1.2 or newer is needed, FSR 4 is not enabled on this device");
        return NULL;
    }
    uint32_t ext_count = 0;
    fns->enum_ext(phys, NULL, &ext_count, NULL);
    VkExtensionProperties *ep = calloc(ext_count ? ext_count : 1, sizeof *ep);
    if (!ep) return NULL;
    fns->enum_ext(phys, NULL, &ext_count, ep);

    bool x_sync2 = has_extension(ep, ext_count, VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME);
    bool x_subgroup = has_extension(ep, ext_count, VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME);
    bool x_dot = has_extension(ep, ext_count, VK_KHR_SHADER_INTEGER_DOT_PRODUCT_EXTENSION_NAME);
    bool x_maint4 = has_extension(ep, ext_count, VK_KHR_MAINTENANCE_4_EXTENSION_NAME);
    bool x_coherent = has_extension(ep, ext_count, VK_AMD_DEVICE_COHERENT_MEMORY_EXTENSION_NAME);
    bool x_deriv = has_extension(ep, ext_count, "VK_KHR_compute_shader_derivatives");
    bool x_coop = has_extension(ep, ext_count, VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME);
    bool x_float8 = has_extension(ep, ext_count, "VK_EXT_shader_float8");
    bool core13 = props.apiVersion >= VK_API_VERSION_1_3;

    VkPhysicalDeviceVulkan12Features h12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan11Features h11 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, .pNext = &h12};
    VkPhysicalDeviceFeatures2 h = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &h11};
    VkPhysicalDeviceSynchronization2Features h_sync2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES};
    VkPhysicalDeviceSubgroupSizeControlFeatures h_subgroup = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES};
    VkPhysicalDeviceShaderIntegerDotProductFeatures h_dot = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_INTEGER_DOT_PRODUCT_FEATURES};
    VkPhysicalDeviceMaintenance4Features h_maint4 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_FEATURES};
    VkPhysicalDeviceCoherentMemoryFeaturesAMD h_coherent = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COHERENT_MEMORY_FEATURES_AMD};
    Link *tail = (Link *)&h12;
#define CHAIN(cond, obj) \
    if (cond) tail->pNext = (Link *)&(obj), tail = (Link *)&(obj)
    CHAIN(x_sync2 || core13, h_sync2);
    CHAIN(x_subgroup || core13, h_subgroup);
    CHAIN(x_dot || core13, h_dot);
    CHAIN(x_maint4 || core13, h_maint4);
    CHAIN(x_coherent, h_coherent);
#ifdef VK_KHR_compute_shader_derivatives
    VkPhysicalDeviceComputeShaderDerivativesFeaturesKHR h_deriv = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COMPUTE_SHADER_DERIVATIVES_FEATURES_KHR};
    CHAIN(x_deriv, h_deriv);
#endif
#ifdef VK_KHR_cooperative_matrix
    VkPhysicalDeviceCooperativeMatrixFeaturesKHR h_coop = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR};
    CHAIN(x_coop, h_coop);
#endif
#ifdef VK_EXT_shader_float8
    VkPhysicalDeviceShaderFloat8FeaturesEXT h_float8 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT8_FEATURES_EXT};
    CHAIN(x_float8, h_float8);
#endif
#undef CHAIN
    fns->features2(phys, &h);

    VkPhysicalDeviceShaderIntegerDotProductProperties p_dot = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_INTEGER_DOT_PRODUCT_PROPERTIES};
    VkPhysicalDeviceVulkan11Properties p11 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES, .pNext = &p_dot};
    VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &p11};
    fns->props2(phys, &p2);
    const VkSubgroupFeatureFlags need_ops = VK_SUBGROUP_FEATURE_BASIC_BIT | VK_SUBGROUP_FEATURE_VOTE_BIT |
                                            VK_SUBGROUP_FEATURE_ARITHMETIC_BIT | VK_SUBGROUP_FEATURE_BALLOT_BIT |
                                            VK_SUBGROUP_FEATURE_SHUFFLE_BIT | VK_SUBGROUP_FEATURE_QUAD_BIT;
    bool subgroup_ok = (p11.subgroupSupportedStages & VK_SHADER_STAGE_COMPUTE_BIT) && (p11.subgroupSupportedOperations & need_ops) == need_ops;
    bool dot_ok = (x_dot || core13) && h_dot.shaderIntegerDotProduct;
    bool int8_ok = subgroup_ok && h12.shaderInt8 && h12.storageBuffer8BitAccess && h12.uniformAndStorageBuffer8BitAccess && dot_ok &&
                   p_dot.integerDotProduct8BitSignedAccelerated;
    bool fp8_ok = false;
#if defined(VK_KHR_cooperative_matrix) && defined(VK_EXT_shader_float8)
    fp8_ok = x_coop && x_float8 && h_coop.cooperativeMatrix && h_float8.shaderFloat8 && h_float8.shaderFloat8CooperativeMatrix;
#endif
    plog("device: INT8 model prerequisites %d (int8 %d, 8-bit storage %d/%d, dot product %d, accelerated int8 dot %d, subgroup ops %d), "
         "FP8 model prerequisites %d",
         int8_ok, h12.shaderInt8, h12.storageBuffer8BitAccess, h12.uniformAndStorageBuffer8BitAccess, dot_ok,
         p_dot.integerDotProduct8BitSignedAccelerated, subgroup_ok, fp8_ok);
    if (!int8_ok && !fp8_ok) {
        plog("device: the GPU lacks what FSR 4 needs, the device is created unchanged and FSR 3.1 runs");
        free(ep);
        return NULL;
    }

    DevcfgState *s = calloc(1, sizeof *s);
    if (!s) {
        free(ep);
        return NULL;
    }
    s->info = *in;
    s->names = calloc((size_t)in->enabledExtensionCount + 16, sizeof *s->names);
    if (!s->names) {
        free(s);
        free(ep);
        return NULL;
    }
    for (uint32_t i = 0; i < in->enabledExtensionCount; i++) s->names[i] = in->ppEnabledExtensionNames[i];
    s->info.ppEnabledExtensionNames = s->names;

    bool e_sync2, e_subgroup, e_dot, e_maint4, e_coherent, e_deriv, e_coop, e_float8;
    add_name(s, ep, ext_count, VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME, &e_sync2);
    add_name(s, ep, ext_count, VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME, &e_subgroup);
    add_name(s, ep, ext_count, VK_KHR_SHADER_INTEGER_DOT_PRODUCT_EXTENSION_NAME, &e_dot);
    add_name(s, ep, ext_count, VK_KHR_MAINTENANCE_4_EXTENSION_NAME, &e_maint4);
    add_name(s, ep, ext_count, VK_AMD_DEVICE_COHERENT_MEMORY_EXTENSION_NAME, &e_coherent);
    add_name(s, ep, ext_count, "VK_KHR_compute_shader_derivatives", &e_deriv);
    add_name(s, ep, ext_count, VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME, &e_coop);
    add_name(s, ep, ext_count, "VK_EXT_shader_float8", &e_float8);
    bool marker;
    add_name(s, ep, ext_count, VK_AMD_BUFFER_MARKER_EXTENSION_NAME, &marker);
    free(ep);

    VkPhysicalDeviceFeatures *features;
    Link *app_f2 = find_struct(in->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2);
    if (app_f2) {
        VkPhysicalDeviceFeatures2 *f2 = touch(s, app_f2, sizeof *f2);
        features = &f2->features;
    } else {
        s->own_f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        if (in->pEnabledFeatures) s->own_f2.features = *in->pEnabledFeatures;
        s->own_f2.pNext = (void *)in->pNext;
        s->info.pNext = &s->own_f2;
        s->info.pEnabledFeatures = NULL;
        features = &s->own_f2.features;
    }
    VkPhysicalDeviceVulkan11Features *f11;
    Link *app_f11 = find_struct(s->info.pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES);
    if (app_f11) {
        f11 = touch(s, app_f11, sizeof *f11);
    } else {
        s->own_f11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
        append(s, &s->own_f11);
        f11 = &s->own_f11;
    }
    VkPhysicalDeviceVulkan12Features *f12;
    Link *app_f12 = find_struct(s->info.pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES);
    if (app_f12) {
        f12 = touch(s, app_f12, sizeof *f12);
    } else {
        s->own_f12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
        append(s, &s->own_f12);
        f12 = &s->own_f12;
    }

    f12->shaderInt8 |= h12.shaderInt8;
    f12->storageBuffer8BitAccess |= h12.storageBuffer8BitAccess;
    f12->uniformAndStorageBuffer8BitAccess |= h12.uniformAndStorageBuffer8BitAccess;
    f12->shaderFloat16 |= h12.shaderFloat16;
    f12->bufferDeviceAddress |= h12.bufferDeviceAddress;
    f12->vulkanMemoryModel |= h12.vulkanMemoryModel;
    f12->vulkanMemoryModelDeviceScope |= h12.vulkanMemoryModelDeviceScope;
    f12->shaderSubgroupExtendedTypes |= h12.shaderSubgroupExtendedTypes;
    f12->descriptorIndexing |= h12.descriptorIndexing;
    f12->runtimeDescriptorArray |= h12.runtimeDescriptorArray;
    f12->descriptorBindingPartiallyBound |= h12.descriptorBindingPartiallyBound;
    f12->shaderSampledImageArrayNonUniformIndexing |= h12.shaderSampledImageArrayNonUniformIndexing;
    f12->shaderStorageImageArrayNonUniformIndexing |= h12.shaderStorageImageArrayNonUniformIndexing;
    f11->storageBuffer16BitAccess |= h11.storageBuffer16BitAccess;
    f11->uniformAndStorageBuffer16BitAccess |= h11.uniformAndStorageBuffer16BitAccess;
    f11->variablePointers |= h11.variablePointers;
    f11->variablePointersStorageBuffer |= h11.variablePointersStorageBuffer;
    features->shaderInt16 |= h.features.shaderInt16;
    features->shaderStorageImageReadWithoutFormat |= h.features.shaderStorageImageReadWithoutFormat;
    features->shaderStorageImageWriteWithoutFormat |= h.features.shaderStorageImageWriteWithoutFormat;

#define EXT_FEATURE(present, stype, type, own, have_cond, assign)                                  \
    do {                                                                                           \
        if ((present) && (have_cond)) {                                                            \
            Link *existing = find_struct(s->info.pNext, stype);                                    \
            type *target;                                                                          \
            if (existing) {                                                                        \
                target = touch(s, existing, sizeof *target);                                       \
            } else {                                                                               \
                s->own.sType = stype;                                                              \
                append(s, &s->own);                                                                \
                target = &s->own;                                                                  \
            }                                                                                      \
            assign;                                                                                \
        }                                                                                          \
    } while (0)

    EXT_FEATURE(e_sync2 || core13, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES, VkPhysicalDeviceSynchronization2Features,
                own_sync2, h_sync2.synchronization2, target->synchronization2 = VK_TRUE);
    EXT_FEATURE(e_subgroup || core13, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES,
                VkPhysicalDeviceSubgroupSizeControlFeatures, own_subgroup, h_subgroup.subgroupSizeControl,
                (target->subgroupSizeControl = VK_TRUE, target->computeFullSubgroups |= h_subgroup.computeFullSubgroups));
    EXT_FEATURE(e_dot || core13, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_INTEGER_DOT_PRODUCT_FEATURES,
                VkPhysicalDeviceShaderIntegerDotProductFeatures, own_dot, h_dot.shaderIntegerDotProduct,
                target->shaderIntegerDotProduct = VK_TRUE);
    EXT_FEATURE(e_maint4 || core13, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_FEATURES, VkPhysicalDeviceMaintenance4Features,
                own_maint4, h_maint4.maintenance4, target->maintenance4 = VK_TRUE);
    EXT_FEATURE(e_coherent, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COHERENT_MEMORY_FEATURES_AMD, VkPhysicalDeviceCoherentMemoryFeaturesAMD,
                own_coherent, h_coherent.deviceCoherentMemory, target->deviceCoherentMemory = VK_TRUE);
#ifdef VK_KHR_compute_shader_derivatives
    EXT_FEATURE(e_deriv, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COMPUTE_SHADER_DERIVATIVES_FEATURES_KHR,
                VkPhysicalDeviceComputeShaderDerivativesFeaturesKHR, own_deriv,
                h_deriv.computeDerivativeGroupLinear || h_deriv.computeDerivativeGroupQuads,
                (target->computeDerivativeGroupLinear |= h_deriv.computeDerivativeGroupLinear,
                 target->computeDerivativeGroupQuads |= h_deriv.computeDerivativeGroupQuads));
#endif
#ifdef VK_KHR_cooperative_matrix
    EXT_FEATURE(e_coop, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR, VkPhysicalDeviceCooperativeMatrixFeaturesKHR,
                own_coop, h_coop.cooperativeMatrix,
                (target->cooperativeMatrix = VK_TRUE,
                 target->cooperativeMatrixRobustBufferAccess |= h_coop.cooperativeMatrixRobustBufferAccess));
#endif
#ifdef VK_EXT_shader_float8
    EXT_FEATURE(e_float8, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT8_FEATURES_EXT, VkPhysicalDeviceShaderFloat8FeaturesEXT, own_float8,
                h_float8.shaderFloat8,
                (target->shaderFloat8 = VK_TRUE, target->shaderFloat8CooperativeMatrix |= h_float8.shaderFloat8CooperativeMatrix));
#endif
#undef EXT_FEATURE

    plog("device: FSR 4 features enabled (%s model), %u extensions requested in total",
         fp8_ok && int8_ok ? "INT8 and FP8" : fp8_ok ? "FP8" : "INT8", s->info.enabledExtensionCount);
    return s;
}

const VkDeviceCreateInfo *devcfg_info(const DevcfgState *s) { return &s->info; }

void devcfg_restore(DevcfgState *s) {
    for (int i = s->touch_count - 1; i >= 0; i--) {
        if (s->touches[i].backup) memcpy(s->touches[i].ptr, s->touches[i].backup, s->touches[i].size);
        free(s->touches[i].backup);
        s->touches[i].backup = NULL;
    }
    if (s->appended) s->tail->pNext = s->tail_saved;
    s->appended = false;
}

void devcfg_free(DevcfgState *s) {
    if (!s) return;
    devcfg_restore(s);
    free(s->names);
    free(s);
}
