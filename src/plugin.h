#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <windows.h>

#include <vulkan/vulkan.h>

#include "ffx_vk_portable_abi.h"

#define LAYER_NAME "VK_LAYER_MHGU_fsr4"
#ifndef PLUGIN_VERSION
#define PLUGIN_VERSION "dev"
#endif

void plog(const char *fmt, ...);
HMODULE plugin_module(void);
bool plugin_dir(wchar_t *out, size_t count);

bool settings_in_game_exe(void);
bool settings_fsr4_requested(void);
bool settings_flag(const char *name);
const char *settings_text(const char *name);

bool coff_find_symbol(const uint8_t *file, size_t size, const char *name, uint32_t *rva);

void guard_init(void);
bool guard_blocked(void);
void guard_enter(const char *what);
void guard_leave(void);

bool hook_install(void);
bool hook_active(void);

typedef struct {
    PFN_vkGetPhysicalDeviceFeatures2 features2;
    PFN_vkGetPhysicalDeviceProperties2 props2;
    PFN_vkGetPhysicalDeviceProperties props;
    PFN_vkEnumerateDeviceExtensionProperties enum_ext;
} DevcfgFns;

typedef struct DevcfgState DevcfgState;

DevcfgState *devcfg_build(const DevcfgFns *fns, VkPhysicalDevice phys, const VkDeviceCreateInfo *in);
const VkDeviceCreateInfo *devcfg_info(const DevcfgState *s);
void devcfg_restore(DevcfgState *s);
void devcfg_free(DevcfgState *s);

void fsr4rt_set_device(VkInstance inst, VkPhysicalDevice phys, VkDevice dev);
void fsr4rt_set_device_ready(bool ready);
void fsr4rt_adopt_game_handles(VkInstance inst, VkPhysicalDevice phys, VkDevice dev, PFN_vkGetDeviceProcAddr gdpa);
void fsr4rt_on_context_create(uint32_t flags, uint32_t render_w, uint32_t render_h, uint32_t out_w, uint32_t out_h);
bool fsr4rt_dispatch(const FfxVkPortableUpscaleDispatchInfo *di);
bool fsr4rt_usable(void);
const char *fsr4rt_name(void);
