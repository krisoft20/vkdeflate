// vkdeflate - a GDeflate 1.0 decoder for Vulkan compute and for the CPU
//
// GPU path: one workgroup of 32 lanes per tile, see tile.comp. This file owns
// the Vulkan boilerplate: a per-context function table (loaded from vulkan-1.dll
// through vkGetInstanceProcAddr for create(), from the caller's for attach()),
// the device, the pipeline with its four push-constant addresses, the staging
// buffers and timestamps of the host-to-host path.
//
// MIT License
//
// Copyright (c) 2026 krisoft20
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#define _CRT_SECURE_NO_WARNINGS
#define VK_NO_PROTOTYPES
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "gdeflate.h"
#include "vkdeflate.h"

namespace vkdeflate {

constexpr uint32_t lanes = 32;

#define VK_FN_GLOBAL(X) X(vkCreateInstance)

#define VK_FN_INSTANCE(X)                       \
    X(vkDestroyInstance)                        \
    X(vkEnumeratePhysicalDevices)               \
    X(vkEnumerateDeviceExtensionProperties)     \
    X(vkGetPhysicalDeviceProperties)            \
    X(vkGetPhysicalDeviceProperties2)           \
    X(vkGetPhysicalDeviceFeatures2)             \
    X(vkGetPhysicalDeviceQueueFamilyProperties) \
    X(vkGetPhysicalDeviceMemoryProperties)      \
    X(vkCreateDevice)                           \
    X(vkGetDeviceProcAddr)

#define VK_FN_PIPELINE(X)            \
    X(vkCreateShaderModule)          \
    X(vkDestroyShaderModule)         \
    X(vkCreatePipelineLayout)        \
    X(vkDestroyPipelineLayout)       \
    X(vkCreateComputePipelines)      \
    X(vkDestroyPipeline)             \
    X(vkCmdBindPipeline)             \
    X(vkCmdPushConstants)            \
    X(vkCmdDispatch)                 \
    X(vkCmdUpdateBuffer)             \
    X(vkCmdPipelineBarrier)          \
    X(vkCreateBuffer)                \
    X(vkDestroyBuffer)               \
    X(vkGetBufferMemoryRequirements) \
    X(vkGetBufferDeviceAddress)      \
    X(vkAllocateMemory)              \
    X(vkFreeMemory)                  \
    X(vkBindBufferMemory)

#define VK_FN_DEVICE(X)         \
    X(vkDestroyDevice)          \
    X(vkGetDeviceQueue)         \
    X(vkDeviceWaitIdle)         \
    X(vkMapMemory)              \
    X(vkCreateCommandPool)      \
    X(vkDestroyCommandPool)     \
    X(vkResetCommandPool)       \
    X(vkAllocateCommandBuffers) \
    X(vkBeginCommandBuffer)     \
    X(vkEndCommandBuffer)       \
    X(vkCmdCopyBuffer)          \
    X(vkCmdCopyBufferToImage)   \
    X(vkCmdResetQueryPool)      \
    X(vkCmdWriteTimestamp)      \
    X(vkCreateQueryPool)        \
    X(vkDestroyQueryPool)       \
    X(vkGetQueryPoolResults)    \
    X(vkQueueSubmit)            \
    X(vkCreateFence)            \
    X(vkDestroyFence)           \
    X(vkWaitForFences)          \
    X(vkResetFences)

#define VK_FN_STATS(X)                                    \
    X(vkGetPipelineExecutablePropertiesKHR)               \
    X(vkGetPipelineExecutableStatisticsKHR)               \
    X(vkGetPipelineExecutableInternalRepresentationsKHR)

struct vk_t {
#define VK_MEMBER(fn) PFN_##fn fn = nullptr;
    VK_FN_GLOBAL(VK_MEMBER)
    VK_FN_INSTANCE(VK_MEMBER)
    VK_FN_PIPELINE(VK_MEMBER)
    VK_FN_DEVICE(VK_MEMBER)
    VK_FN_STATS(VK_MEMBER)
};

static const uint32_t tile_spv[] =
#include "tile_spv.inc"
    ;

struct buf_t {
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    VkDeviceAddress addr = 0;
    void* map = nullptr;
};

struct push_t {
    VkDeviceAddress src;
    VkDeviceAddress tiles;
    VkDeviceAddress dst;
    VkDeviceAddress status;
    VkDeviceAddress count;
    uint32_t stride;
    uint32_t pad;
};

constexpr VkDeviceSize REGION_RING = 1 << 20;
constexpr const char* subgroup_ext = "VK_EXT_subgroup_size_control";

struct ctx_t {
#if defined(_WIN32)
    HMODULE lib = nullptr;
#else
    void* lib = nullptr;
#endif
    vk_t f;
    bool owns_device = false;
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice dev = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t family = 0;
    VkPhysicalDeviceMemoryProperties mem = {};
    float ts_period = 0;
    std::string name;
    uint32_t subgroup = 0;
    bool control = false;
    bool control_ext = false;
    uint32_t experiment = 0;
    uint32_t ring = 512;
    uint32_t pad = 0;
    uint32_t runs_pad = 0;
    uint32_t lit_root = 9;
    uint32_t dist_root = 7;
    bool verbose = false;
    bool stats = false;
    std::string report;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkQueryPool queries = VK_NULL_HANDLE;
    VkShaderModule shader = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipeline pipeline_runs = VK_NULL_HANDLE;
    buf_t in, tiles, out, status, up, down, regions;
    VkCommandPool load_pool = VK_NULL_HANDLE;
    VkCommandBuffer load_cmd = VK_NULL_HANDLE;
    VkFence load_fence = VK_NULL_HANDLE;
    uint32_t load_family = UINT32_MAX;
    VkDeviceSize regions_at = 0;
    std::string err;
};

static bool fail(ctx_t& g, const char* what, VkResult r) {
    g.err = std::string(what) + " failed: " + std::to_string((int)r);
    return false;
}

static bool fail(ctx_t& g, const char* what) {
    g.err = what;
    return false;
}

static bool has_extension(ctx_t& g, const char* name) {
    uint32_t n = 0;
    g.f.vkEnumerateDeviceExtensionProperties(g.phys, nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> exts(n);
    g.f.vkEnumerateDeviceExtensionProperties(g.phys, nullptr, &n, exts.data());
    for (const VkExtensionProperties& e : exts)
        if (!strcmp(e.extensionName, name))
            return true;
    return false;
}

static bool pick_subgroup(ctx_t& g, const config_t& cfg) {
    VkPhysicalDeviceProperties base;
    g.f.vkGetPhysicalDeviceProperties(g.phys, &base);
    if (base.apiVersion < VK_API_VERSION_1_2)
        return fail(g, "device is below vulkan 1.2");
    bool core13 = base.apiVersion >= VK_API_VERSION_1_3;
    bool has_control = core13 || has_extension(g, subgroup_ext);

    VkPhysicalDeviceSubgroupSizeControlProperties psc = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_PROPERTIES};
    VkPhysicalDeviceVulkan11Properties p11 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES,
                                              has_control ? &psc : nullptr};
    VkPhysicalDeviceProperties2 p2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &p11};
    g.f.vkGetPhysicalDeviceProperties2(g.phys, &p2);
    g.name = p2.properties.deviceName;
    g.ts_period = p2.properties.limits.timestampPeriod;
    constexpr VkSubgroupFeatureFlags need_ops = VK_SUBGROUP_FEATURE_BASIC_BIT | VK_SUBGROUP_FEATURE_BALLOT_BIT |
                                                VK_SUBGROUP_FEATURE_ARITHMETIC_BIT | VK_SUBGROUP_FEATURE_SHUFFLE_BIT;
    if ((p11.subgroupSupportedOperations & need_ops) != need_ops || !(p11.subgroupSupportedStages & VK_SHADER_STAGE_COMPUTE_BIT))
        return fail(g, "device lacks subgroup ballot, arithmetic or shuffle in compute");

    VkPhysicalDeviceSubgroupSizeControlFeatures fsc = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES};
    VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                                            has_control ? &fsc : nullptr};
    VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f12};
    g.f.vkGetPhysicalDeviceFeatures2(g.phys, &f2);
    if (!f2.features.shaderInt64 || !f12.storageBuffer8BitAccess || !f12.shaderInt8 || !f12.bufferDeviceAddress ||
        !f12.vulkanMemoryModel)
        return fail(g, "device lacks shaderInt64, shaderInt8, storageBuffer8BitAccess, bufferDeviceAddress or vulkanMemoryModel");
    g.control = has_control && fsc.subgroupSizeControl && fsc.computeFullSubgroups &&
                (psc.requiredSubgroupSizeStages & VK_SHADER_STAGE_COMPUTE_BIT);
    g.control_ext = g.control && !core13;

    g.subgroup = cfg.subgroup_size;
    if (!g.subgroup) {
        if (p11.subgroupSize == lanes)
            g.subgroup = lanes;
        else if (g.control && psc.minSubgroupSize <= lanes && lanes <= psc.maxSubgroupSize)
            g.subgroup = lanes;
        else
            g.subgroup = p11.subgroupSize;
    }
    if (g.subgroup < lanes)
        return fail(g, "subgroup size below 32 is not supported");
    if (g.subgroup != p11.subgroupSize && !g.control)
        return fail(g, "device cannot select the requested subgroup size");
    if (g.subgroup != p11.subgroupSize && (g.subgroup < psc.minSubgroupSize || g.subgroup > psc.maxSubgroupSize))
        return fail(g, "requested subgroup size is outside the device range");
    return true;
}

static bool init_device(ctx_t& g, const config_t& cfg) {
#if defined(_WIN32)
    g.lib = LoadLibraryW(L"vulkan-1.dll");
    PFN_vkGetInstanceProcAddr gipa = g.lib ? (PFN_vkGetInstanceProcAddr)GetProcAddress(g.lib, "vkGetInstanceProcAddr") : nullptr;
#else
    g.lib = dlopen("libvulkan.so.1", RTLD_NOW);
    PFN_vkGetInstanceProcAddr gipa = g.lib ? (PFN_vkGetInstanceProcAddr)dlsym(g.lib, "vkGetInstanceProcAddr") : nullptr;
#endif
    if (!g.lib)
        return fail(g, "vulkan loader not found");
    if (!gipa)
        return fail(g, "vkGetInstanceProcAddr not found");
#define LOAD_GLOBAL(fn) g.f.fn = (PFN_##fn)gipa(nullptr, #fn);
    VK_FN_GLOBAL(LOAD_GLOBAL)

    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "vkdeflate";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    VkResult r = g.f.vkCreateInstance(&ici, nullptr, &g.instance);
    if (r != VK_SUCCESS)
        return fail(g, "vkCreateInstance", r);
    g.owns_device = true;
#define LOAD_INSTANCE(fn) g.f.fn = (PFN_##fn)gipa(g.instance, #fn);
    VK_FN_INSTANCE(LOAD_INSTANCE)

    uint32_t count = 0;
    g.f.vkEnumeratePhysicalDevices(g.instance, &count, nullptr);
    if (!count)
        return fail(g, "no vulkan device");
    std::vector<VkPhysicalDevice> devs(count);
    g.f.vkEnumeratePhysicalDevices(g.instance, &count, devs.data());
    g.phys = devs[0];
    VkPhysicalDeviceProperties props;
    for (VkPhysicalDevice d : devs) {
        g.f.vkGetPhysicalDeviceProperties(d, &props);
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            g.phys = d;
            break;
        }
    }
    g.f.vkGetPhysicalDeviceMemoryProperties(g.phys, &g.mem);
    if (!pick_subgroup(g, cfg))
        return false;

    uint32_t nfam = 0;
    g.f.vkGetPhysicalDeviceQueueFamilyProperties(g.phys, &nfam, nullptr);
    std::vector<VkQueueFamilyProperties> fams(nfam);
    g.f.vkGetPhysicalDeviceQueueFamilyProperties(g.phys, &nfam, fams.data());
    g.family = UINT32_MAX;
    for (uint32_t i = 0; i < nfam; i++)
        if ((fams[i].queueFlags & VK_QUEUE_COMPUTE_BIT) && fams[i].timestampValidBits) {
            g.family = i;
            break;
        }
    if (g.family == UINT32_MAX)
        return fail(g, "no compute queue with timestamps");

    uint32_t next = 0;
    g.f.vkEnumerateDeviceExtensionProperties(g.phys, nullptr, &next, nullptr);
    std::vector<VkExtensionProperties> exts(next);
    g.f.vkEnumerateDeviceExtensionProperties(g.phys, nullptr, &next, exts.data());
    const char* stats_ext = "VK_KHR_pipeline_executable_properties";
    for (const VkExtensionProperties& e : exts)
        if ((g.verbose || cfg.capture_stats) && !strcmp(e.extensionName, stats_ext))
            g.stats = true;

    VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR want_stats = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_EXECUTABLE_PROPERTIES_FEATURES_KHR};
    want_stats.pipelineExecutableInfo = VK_TRUE;
    VkPhysicalDeviceSubgroupSizeControlFeatures want_sc = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES,
                                                           g.stats ? &want_stats : nullptr};
    want_sc.subgroupSizeControl = VK_TRUE;
    want_sc.computeFullSubgroups = VK_TRUE;
    VkPhysicalDeviceVulkan12Features want12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                                               g.control ? (void*)&want_sc : g.stats ? (void*)&want_stats : nullptr};
    want12.storageBuffer8BitAccess = VK_TRUE;
    want12.shaderInt8 = VK_TRUE;
    want12.bufferDeviceAddress = VK_TRUE;
    want12.vulkanMemoryModel = VK_TRUE;
    VkPhysicalDeviceFeatures want = {};
    want.shaderInt64 = VK_TRUE;
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = g.family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &want12};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.pEnabledFeatures = &want;
    const char* ext_names[2];
    uint32_t next_names = 0;
    if (g.stats)
        ext_names[next_names++] = stats_ext;
    if (g.control_ext)
        ext_names[next_names++] = subgroup_ext;
    dci.enabledExtensionCount = next_names;
    dci.ppEnabledExtensionNames = ext_names;
    r = g.f.vkCreateDevice(g.phys, &dci, nullptr, &g.dev);
    if (r != VK_SUCCESS)
        return fail(g, "vkCreateDevice", r);
#define LOAD_DEVICE(fn) g.f.fn = (PFN_##fn)g.f.vkGetDeviceProcAddr(g.dev, #fn);
    VK_FN_PIPELINE(LOAD_DEVICE)
    VK_FN_DEVICE(LOAD_DEVICE)
    if (g.stats)
        VK_FN_STATS(LOAD_DEVICE)
    g.f.vkGetDeviceQueue(g.dev, g.family, 0, &g.queue);

    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.queueFamilyIndex = g.family;
    if ((r = g.f.vkCreateCommandPool(g.dev, &pci, nullptr, &g.pool)) != VK_SUCCESS)
        return fail(g, "vkCreateCommandPool", r);
    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = g.pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    if ((r = g.f.vkAllocateCommandBuffers(g.dev, &cai, &g.cmd)) != VK_SUCCESS)
        return fail(g, "vkAllocateCommandBuffers", r);
    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if ((r = g.f.vkCreateFence(g.dev, &fci, nullptr, &g.fence)) != VK_SUCCESS)
        return fail(g, "vkCreateFence", r);
    VkQueryPoolCreateInfo qpi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    qpi.queryType = VK_QUERY_TYPE_TIMESTAMP;
    qpi.queryCount = 4;
    if ((r = g.f.vkCreateQueryPool(g.dev, &qpi, nullptr, &g.queries)) != VK_SUCCESS)
        return fail(g, "vkCreateQueryPool", r);
    return true;
}

static std::string stat_text(const VkPipelineExecutableStatisticKHR& s) {
    char v[64];
    if (s.format == VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR)
        snprintf(v, sizeof(v), "%llu", (unsigned long long)s.value.u64);
    else if (s.format == VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_INT64_KHR)
        snprintf(v, sizeof(v), "%lld", (long long)s.value.i64);
    else if (s.format == VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_FLOAT64_KHR)
        snprintf(v, sizeof(v), "%g", s.value.f64);
    else
        snprintf(v, sizeof(v), "%s", s.value.b32 ? "yes" : "no");
    return std::string(s.name) + ": " + v;
}

static void make_report(ctx_t& g) {
    g.report = "subgroup: " + std::to_string(g.subgroup);
    g.report += g.control ? ", chosen through subgroup size control\n" : ", native\n";
    g.report += "pipelines: " + std::string(g.pipeline_runs ? "2, runs padded by " + std::to_string(g.runs_pad) + " bytes" : "1") + "\n";
    if (!g.stats) {
        g.report += "compiler statistics: not available\n";
        return;
    }
    VkPipelineExecutableInfoKHR ei = {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INFO_KHR};
    ei.pipeline = g.pipeline;
    uint32_t ns = 0;
    g.f.vkGetPipelineExecutableStatisticsKHR(g.dev, &ei, &ns, nullptr);
    std::vector<VkPipelineExecutableStatisticKHR> st(ns, {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_STATISTIC_KHR});
    g.f.vkGetPipelineExecutableStatisticsKHR(g.dev, &ei, &ns, st.data());
    for (const VkPipelineExecutableStatisticKHR& s : st)
        g.report += "compiler " + stat_text(s) + "\n";
}

static void print_stats(ctx_t& g) {
    VkPipelineInfoKHR pi = {VK_STRUCTURE_TYPE_PIPELINE_INFO_KHR};
    pi.pipeline = g.pipeline;
    uint32_t n = 0;
    g.f.vkGetPipelineExecutablePropertiesKHR(g.dev, &pi, &n, nullptr);
    for (uint32_t e = 0; e < n; e++) {
        VkPipelineExecutableInfoKHR ei = {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INFO_KHR};
        ei.pipeline = g.pipeline;
        ei.executableIndex = e;
        uint32_t ns = 0;
        g.f.vkGetPipelineExecutableStatisticsKHR(g.dev, &ei, &ns, nullptr);
        std::vector<VkPipelineExecutableStatisticKHR> st(ns, {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_STATISTIC_KHR});
        g.f.vkGetPipelineExecutableStatisticsKHR(g.dev, &ei, &ns, st.data());
        for (const VkPipelineExecutableStatisticKHR& s : st)
            printf("[vkdeflate] %s\n", stat_text(s).c_str());
        uint32_t ni = 0;
        g.f.vkGetPipelineExecutableInternalRepresentationsKHR(g.dev, &ei, &ni, nullptr);
        std::vector<VkPipelineExecutableInternalRepresentationKHR> irs(
            ni, {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INTERNAL_REPRESENTATION_KHR});
        g.f.vkGetPipelineExecutableInternalRepresentationsKHR(g.dev, &ei, &ni, irs.data());
        std::vector<std::vector<char>> texts(ni);
        for (uint32_t i = 0; i < ni; i++) {
            texts[i].resize(irs[i].dataSize);
            irs[i].pData = texts[i].data();
        }
        g.f.vkGetPipelineExecutableInternalRepresentationsKHR(g.dev, &ei, &ni, irs.data());
        for (uint32_t i = 0; i < ni; i++) {
            std::string path = std::string("vkdeflate_tile_") + std::to_string(i) + ".txt";
            FILE* file = fopen(path.c_str(), "wb");
            if (file) {
                fwrite(texts[i].data(), 1, irs[i].dataSize, file);
                fclose(file);
            }
            printf("[vkdeflate] %s (%s) written to %s, %zu bytes\n", irs[i].name, irs[i].isText ? "text" : "binary",
                   path.c_str(), (size_t)irs[i].dataSize);
        }
    }
}

static bool make_pipeline(ctx_t& g) {
    VkShaderModuleCreateInfo smci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smci.codeSize = sizeof(tile_spv);
    smci.pCode = tile_spv;
    VkResult r = g.f.vkCreateShaderModule(g.dev, &smci, nullptr, &g.shader);
    if (r != VK_SUCCESS)
        return fail(g, "vkCreateShaderModule", r);

    VkPushConstantRange pcr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push_t)};
    VkPipelineLayoutCreateInfo plci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &pcr;
    if ((r = g.f.vkCreatePipelineLayout(g.dev, &plci, nullptr, &g.layout)) != VK_SUCCESS)
        return fail(g, "vkCreatePipelineLayout", r);

    uint32_t spec_data[6] = {g.subgroup, g.experiment, g.ring, g.pad, g.lit_root, g.dist_root};
    VkSpecializationMapEntry spec_map[6] = {{0, 0, 4}, {1, 4, 4}, {2, 8, 4}, {3, 12, 4}, {4, 16, 4}, {5, 20, 4}};
    VkSpecializationInfo spec = {6, spec_map, sizeof(spec_data), spec_data};
    VkPipelineShaderStageRequiredSubgroupSizeCreateInfo rss = {
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO};
    rss.requiredSubgroupSize = g.subgroup;
    VkComputePipelineCreateInfo cpci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpci.flags = 0;
    if (g.stats)
        cpci.flags |= VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR;
    if (g.stats && g.verbose)
        cpci.flags |= VK_PIPELINE_CREATE_CAPTURE_INTERNAL_REPRESENTATIONS_BIT_KHR;
    VkPipelineShaderStageCreateFlags full = 0;
    if (g.control)
        full = VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT;
    cpci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, g.control ? &rss : nullptr, full,
                  VK_SHADER_STAGE_COMPUTE_BIT, g.shader, "main", &spec};
    cpci.layout = g.layout;
    if ((r = g.f.vkCreateComputePipelines(g.dev, VK_NULL_HANDLE, 1, &cpci, nullptr, &g.pipeline)) != VK_SUCCESS)
        return fail(g, "vkCreateComputePipelines", r);
    if (g.runs_pad) {
        spec_data[3] = g.pad + g.runs_pad;
        cpci.flags = 0;
        if ((r = g.f.vkCreateComputePipelines(g.dev, VK_NULL_HANDLE, 1, &cpci, nullptr, &g.pipeline_runs)) != VK_SUCCESS)
            return fail(g, "vkCreateComputePipelines", r);
    }
    make_report(g);
    if (g.stats && g.verbose)
        print_stats(g);
    if (g.verbose)
        printf("[vkdeflate] %s, subgroup size %u%s\n", g.name.c_str(), g.subgroup,
               g.owns_device ? "" : ", attached to the caller's device");
    return true;
}

static uint32_t pick_memory(const VkPhysicalDeviceMemoryProperties& mp, uint32_t bits, VkMemoryPropertyFlags want,
                            VkMemoryPropertyFlags avoid) {
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
        VkMemoryPropertyFlags f = mp.memoryTypes[i].propertyFlags;
        if (((bits >> i) & 1) && (f & want) == want && !(f & avoid))
            return i;
    }
    return UINT32_MAX;
}

static void free_buf(ctx_t& g, buf_t& b) {
    if (b.buf)
        g.f.vkDestroyBuffer(g.dev, b.buf, nullptr);
    if (b.mem)
        g.f.vkFreeMemory(g.dev, b.mem, nullptr);
    b = buf_t{};
}

static bool make_buf(ctx_t& g, buf_t& b, VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags want,
                     VkMemoryPropertyFlags avoid) {
    if (b.buf && b.size >= size)
        return true;
    free_buf(g, b);
    b.size = size < 4 ? 4 : size;
    bool addressed = (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) != 0;
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = b.size;
    bci.usage = usage;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkResult r = g.f.vkCreateBuffer(g.dev, &bci, nullptr, &b.buf);
    if (r != VK_SUCCESS)
        return fail(g, "vkCreateBuffer", r);
    VkMemoryRequirements req;
    g.f.vkGetBufferMemoryRequirements(g.dev, b.buf, &req);
    uint32_t type = pick_memory(g.mem, req.memoryTypeBits, want, avoid);
    if (type == UINT32_MAX)
        type = pick_memory(g.mem, req.memoryTypeBits, want, 0);
    if (type == UINT32_MAX)
        type = pick_memory(g.mem, req.memoryTypeBits, want & ~VK_MEMORY_PROPERTY_HOST_CACHED_BIT, 0);
    if (type == UINT32_MAX)
        return fail(g, "no suitable memory type");
    VkMemoryAllocateFlagsInfo mfi = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    mfi.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, addressed ? &mfi : nullptr};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = type;
    if ((r = g.f.vkAllocateMemory(g.dev, &mai, nullptr, &b.mem)) != VK_SUCCESS)
        return fail(g, "vkAllocateMemory", r);
    if ((r = g.f.vkBindBufferMemory(g.dev, b.buf, b.mem, 0)) != VK_SUCCESS)
        return fail(g, "vkBindBufferMemory", r);
    if (want & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
        if ((r = g.f.vkMapMemory(g.dev, b.mem, 0, VK_WHOLE_SIZE, 0, &b.map)) != VK_SUCCESS)
            return fail(g, "vkMapMemory", r);
    if (addressed) {
        VkBufferDeviceAddressInfo bai = {VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
        bai.buffer = b.buf;
        b.addr = g.f.vkGetBufferDeviceAddress(g.dev, &bai);
    }
    return true;
}

static VkDeviceSize align_up(VkDeviceSize v, VkDeviceSize a) {
    return (v + a - 1) / a * a;
}

static void barrier(ctx_t& g, VkCommandBuffer cmd, VkPipelineStageFlags from, VkPipelineStageFlags to,
                    VkAccessFlags src, VkAccessFlags dst) {
    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, src, dst};
    g.f.vkCmdPipelineBarrier(cmd, from, to, 0, 1, &mb, 0, nullptr, 0, nullptr);
}

static double ms_between(std::chrono::steady_clock::time_point a, std::chrono::steady_clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}

const char* tile_error(uint32_t code) {
    switch (code) {
    case 0: return "ok";
    case 1: return "read past the end of the tile";
    case 2: return "reserved block type";
    case 3: return "bad precode";
    case 4: return "bad code lengths";
    case 5: return "bad literal/length code";
    case 6: return "bad distance code";
    case 7: return "bad symbol";
    case 8: return "output past the end of the tile";
    case 9: return "match before the start of the tile";
    case 10: return "tile decoded to the wrong size";
    default: return "unknown error";
    }
}

ctx_t* create(const config_t& cfg, std::string& err) {
    ctx_t* g = new ctx_t;
    g->verbose = cfg.verbose;
    g->experiment = cfg.experiment;
    g->ring = cfg.ring_bytes;
    g->pad = cfg.pad_bytes;
    g->runs_pad = cfg.runs_pad_bytes;
    g->lit_root = cfg.lit_root;
    g->dist_root = cfg.dist_root;
    if (!init_device(*g, cfg) || !make_pipeline(*g)) {
        err = g->err;
        destroy(g);
        return nullptr;
    }
    return g;
}

ctx_t* attach(VkInstance instance, VkPhysicalDevice phys, VkDevice dev, PFN_vkGetInstanceProcAddr gipa,
              const config_t& cfg, std::string& err) {
    ctx_t* g = new ctx_t;
    g->verbose = cfg.verbose;
    g->experiment = cfg.experiment;
    g->ring = cfg.ring_bytes;
    g->pad = cfg.pad_bytes;
    g->runs_pad = cfg.runs_pad_bytes;
    g->lit_root = cfg.lit_root;
    g->dist_root = cfg.dist_root;
    g->instance = instance;
    g->phys = phys;
    g->dev = dev;
#define LOAD_INSTANCE_EXT(fn) g->f.fn = (PFN_##fn)gipa(instance, #fn);
    VK_FN_INSTANCE(LOAD_INSTANCE_EXT)
    if (!g->f.vkGetDeviceProcAddr || !g->f.vkGetPhysicalDeviceProperties2 || !g->f.vkGetPhysicalDeviceFeatures2) {
        err = "vkGetInstanceProcAddr did not give the instance functions";
        delete g;
        return nullptr;
    }
#define LOAD_DEVICE_EXT(fn) g->f.fn = (PFN_##fn)g->f.vkGetDeviceProcAddr(dev, #fn);
    VK_FN_PIPELINE(LOAD_DEVICE_EXT)
    g->f.vkGetPhysicalDeviceMemoryProperties(phys, &g->mem);
    if (cfg.capture_stats) {
        VK_FN_STATS(LOAD_DEVICE_EXT)
        g->stats = g->f.vkGetPipelineExecutablePropertiesKHR && g->f.vkGetPipelineExecutableStatisticsKHR &&
                   g->f.vkGetPipelineExecutableInternalRepresentationsKHR;
    }
    if (!pick_subgroup(*g, cfg) || !make_pipeline(*g)) {
        err = g->err;
        destroy(g);
        return nullptr;
    }
    return g;
}

void destroy(ctx_t* g) {
    if (!g)
        return;
    if (g->dev) {
        if (g->owns_device)
            g->f.vkDeviceWaitIdle(g->dev);
        free_buf(*g, g->in);
        free_buf(*g, g->tiles);
        free_buf(*g, g->out);
        free_buf(*g, g->status);
        free_buf(*g, g->up);
        free_buf(*g, g->down);
        free_buf(*g, g->regions);
        if (g->load_fence)
            g->f.vkDestroyFence(g->dev, g->load_fence, nullptr);
        if (g->load_pool)
            g->f.vkDestroyCommandPool(g->dev, g->load_pool, nullptr);
        if (g->pipeline)
            g->f.vkDestroyPipeline(g->dev, g->pipeline, nullptr);
        if (g->pipeline_runs)
            g->f.vkDestroyPipeline(g->dev, g->pipeline_runs, nullptr);
        if (g->layout)
            g->f.vkDestroyPipelineLayout(g->dev, g->layout, nullptr);
        if (g->shader)
            g->f.vkDestroyShaderModule(g->dev, g->shader, nullptr);
        if (g->queries)
            g->f.vkDestroyQueryPool(g->dev, g->queries, nullptr);
        if (g->fence)
            g->f.vkDestroyFence(g->dev, g->fence, nullptr);
        if (g->pool)
            g->f.vkDestroyCommandPool(g->dev, g->pool, nullptr);
        if (g->owns_device)
            g->f.vkDestroyDevice(g->dev, nullptr);
    }
    if (g->instance && g->owns_device)
        g->f.vkDestroyInstance(g->instance, nullptr);
#if defined(_WIN32)
    if (g->lib)
        FreeLibrary(g->lib);
#else
    if (g->lib)
        dlclose(g->lib);
#endif
    delete g;
}

const char* device_name(const ctx_t* g) {
    return g ? g->name.c_str() : "none";
}

const char* report(const ctx_t* g) {
    return g ? g->report.c_str() : "";
}

bool describe(const uint8_t* p, size_t n, size_t stream_size, std::vector<tile_desc_t>& tiles, size_t& out_size,
              std::string& err) {
    gdeflate::stream_t s;
    if (!gdeflate::parse_head(p, n, stream_size, s, err))
        return false;
    tiles.resize(s.tiles.size());
    for (size_t i = 0; i < s.tiles.size(); i++) {
        const gdeflate::tile_t& t = s.tiles[i];
        if (t.src % 4) {
            err = "tile " + std::to_string(i) + " is not 4-byte aligned";
            return false;
        }
        tiles[i] = tile_desc_t{(uint32_t)(t.src / 4), (uint32_t)(t.csize / 4), (uint32_t)t.dst, (uint32_t)t.size};
    }
    std::stable_sort(tiles.begin(), tiles.end(), [](const tile_desc_t& a, const tile_desc_t& b) { return a.words > b.words; });
    out_size = s.total;
    return true;
}

uint32_t job_flags(const std::vector<tile_desc_t>& tiles, size_t out_size) {
    uint64_t words = 0;
    for (const tile_desc_t& t : tiles)
        words += t.words;
    return words * 4 * 20 < out_size ? JOB_RUNS : 0u;
}

static void dispatch(ctx_t* g, VkCommandBuffer cmd, const push_t& pc, uint32_t groups, uint32_t flags) {
    bool runs = (flags & JOB_RUNS) != 0 && g->pipeline_runs;
    g->f.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, runs ? g->pipeline_runs : g->pipeline);
    g->f.vkCmdPushConstants(cmd, g->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
    g->f.vkCmdDispatch(cmd, groups, 1, 1);
}

void record(ctx_t* g, VkCommandBuffer cmd, const job_t& job) {
    if (!job.num_tiles)
        return;
    dispatch(g, cmd, push_t{job.src, job.tiles, job.dst, job.status, 0, 0, 0}, job.num_tiles, job.flags);
}

uint32_t region_flags(const region_t* regions, uint32_t count) {
    uint64_t in = 0, out = 0;
    for (uint32_t i = 0; i < count; i++) {
        in += regions[i].csize;
        out += regions[i].size;
    }
    return in * 20 < out ? JOB_RUNS : 0u;
}

void record_regions(ctx_t* g, VkCommandBuffer cmd, VkDeviceAddress regions, uint32_t count, uint32_t stride,
                    VkDeviceAddress status, uint32_t flags) {
    if (!count)
        return;
    dispatch(g, cmd, push_t{0, regions, 0, status, 0, stride, 0}, count, flags);
}

void record_regions_indirect(ctx_t* g, VkCommandBuffer cmd, VkDeviceAddress regions, VkDeviceAddress count,
                             uint32_t max_count, uint32_t stride, VkDeviceAddress status, uint32_t flags) {
    if (!max_count)
        return;
    dispatch(g, cmd, push_t{0, regions, 0, status, count, stride, 0}, max_count, flags);
}

bool cmd_decompress_memory(ctx_t* g, VkCommandBuffer cmd, const region_t* regions, uint32_t count) {
    constexpr VkBufferUsageFlags usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                         VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    constexpr uint32_t per_dispatch = (uint32_t)(REGION_RING / 2 / sizeof(region_t));
    if (!count)
        return true;
    if (!make_buf(*g, g->regions, REGION_RING, usage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0))
        return false;
    std::vector<region_t> sorted(regions, regions + count);
    std::stable_sort(sorted.begin(), sorted.end(), [](const region_t& a, const region_t& b) { return a.csize > b.csize; });
    uint32_t flags = region_flags(regions, count);
    for (uint32_t first = 0; first < count; first += per_dispatch) {
        uint32_t n = std::min(per_dispatch, count - first);
        VkDeviceSize bytes = (VkDeviceSize)n * sizeof(region_t);
        if (g->regions_at + bytes > REGION_RING)
            g->regions_at = 0;
        VkMemoryBarrier before = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_SHADER_READ_BIT,
                                  VK_ACCESS_TRANSFER_WRITE_BIT};
        g->f.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1,
                                  &before, 0, nullptr, 0, nullptr);
        for (VkDeviceSize done = 0; done < bytes; done += 65536) {
            VkDeviceSize piece = std::min<VkDeviceSize>(65536, bytes - done);
            g->f.vkCmdUpdateBuffer(cmd, g->regions.buf, g->regions_at + done, piece,
                                   (const uint8_t*)(sorted.data() + first) + done);
        }
        VkMemoryBarrier after = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_TRANSFER_WRITE_BIT,
                                 VK_ACCESS_SHADER_READ_BIT};
        g->f.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
                                  &after, 0, nullptr, 0, nullptr);
        record_regions(g, cmd, g->regions.addr + g->regions_at, n, sizeof(region_t), 0, flags);
        g->regions_at += bytes;
    }
    return true;
}

static bool read_range(const load_t& item, std::vector<uint8_t>& bytes, std::string& err) {
    FILE* f = fopen(item.path, "rb");
    if (!f) {
        err = std::string("cant open ") + item.path;
        return false;
    }
    uint64_t size = item.size;
    if (!size) {
#if defined(_WIN32)
        _fseeki64(f, 0, SEEK_END);
        uint64_t end = (uint64_t)_ftelli64(f);
#else
        fseeko(f, 0, SEEK_END);
        uint64_t end = (uint64_t)ftello(f);
#endif
        size = end > item.offset ? end - item.offset : 0;
    }
#if defined(_WIN32)
    _fseeki64(f, (long long)item.offset, SEEK_SET);
#else
    fseeko(f, (off_t)item.offset, SEEK_SET);
#endif
    bytes.resize((size_t)size);
    size_t got = size ? fread(bytes.data(), 1, bytes.size(), f) : 0;
    fclose(f);
    if (got != bytes.size()) {
        err = std::string("short read from ") + item.path;
        return false;
    }
    return true;
}

bool load(ctx_t* g, VkQueue queue, uint32_t queue_family, const load_t* items, uint32_t count, std::string& err) {
    struct owner_t {
        tile_desc_t tile;
        uint32_t item;
        uint32_t index;
    };
    struct span_t {
        VkDeviceSize out;
        size_t size;
    };
    if (!count)
        return true;
    if (g->owns_device && !queue) {
        queue = g->queue;
        queue_family = g->family;
    }
    if (!g->f.vkQueueSubmit) {
#define LOAD_DEVICE_LATE(fn) g->f.fn = (PFN_##fn)g->f.vkGetDeviceProcAddr(g->dev, #fn);
        VK_FN_DEVICE(LOAD_DEVICE_LATE)
    }

    std::vector<std::vector<uint8_t>> files(count);
    std::vector<owner_t> owners;
    std::vector<span_t> spans(count);
    VkDeviceSize in_size = 0, out_size = 0;
    for (uint32_t i = 0; i < count; i++) {
        const load_t& item = items[i];
        const uint8_t* bytes = item.data;
        size_t n = (size_t)item.size;
        if (!bytes) {
            if (!item.path) {
                err = "item " + std::to_string(i) + ": neither path nor data";
                return false;
            }
            if (!read_range(item, files[i], err))
                return false;
            bytes = files[i].data();
            n = files[i].size();
        }
        if ((item.buffer != VK_NULL_HANDLE) == (item.image != VK_NULL_HANDLE)) {
            err = "item " + std::to_string(i) + ": needs a buffer or an image, not both";
            return false;
        }
        std::vector<tile_desc_t> descs;
        size_t total = 0;
        if (!describe(bytes, n, n, descs, total, err)) {
            err = "item " + std::to_string(i) + ": " + err;
            return false;
        }
        if (in_size / 4 + n / 4 + 1 > UINT32_MAX || out_size + total > UINT32_MAX) {
            err = "more than 4 GB in one load call";
            return false;
        }
        for (const tile_desc_t& t : descs)
            owners.push_back({{t.src + (uint32_t)(in_size / 4), t.words, t.dst + (uint32_t)out_size, t.size}, i,
                              t.dst / (uint32_t)gdeflate::TILE_SIZE});
        spans[i] = {out_size, total};
        if (files[i].empty() && n)
            files[i].assign(bytes, bytes + n);
        in_size += align_up(n, 4);
        out_size += align_up(total, 256);
    }
    if (owners.empty())
        return true;
    std::stable_sort(owners.begin(), owners.end(), [](const owner_t& a, const owner_t& b) { return a.tile.words > b.tile.words; });
    std::vector<tile_desc_t> tiles(owners.size());
    for (size_t i = 0; i < owners.size(); i++)
        tiles[i] = owners[i].tile;

    constexpr VkBufferUsageFlags storage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    constexpr VkBufferUsageFlags xfer_dst = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    constexpr VkBufferUsageFlags xfer_src = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    constexpr VkMemoryPropertyFlags local = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    constexpr VkMemoryPropertyFlags visible = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    VkDeviceSize tiles_size = tiles.size() * sizeof(tile_desc_t);
    VkDeviceSize status_size = tiles.size() * 4;
    VkDeviceSize up_tiles = align_up(in_size, 256);
    VkResult r = VK_SUCCESS;
    if (!make_buf(*g, g->in, in_size, storage | xfer_dst, local, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) ||
        !make_buf(*g, g->tiles, tiles_size, storage | xfer_dst, local, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) ||
        !make_buf(*g, g->out, out_size, storage | xfer_src, local, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) ||
        !make_buf(*g, g->status, status_size, storage | xfer_src, local, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) ||
        !make_buf(*g, g->up, up_tiles + tiles_size, xfer_src, visible, local) ||
        !make_buf(*g, g->down, status_size, xfer_dst, visible | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, local)) {
        err = g->err;
        return false;
    }
    if (g->load_family != queue_family) {
        if (g->load_pool)
            g->f.vkDestroyCommandPool(g->dev, g->load_pool, nullptr);
        g->load_pool = VK_NULL_HANDLE;
        VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.queueFamilyIndex = queue_family;
        VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        if ((r = g->f.vkCreateCommandPool(g->dev, &pci, nullptr, &g->load_pool)) == VK_SUCCESS) {
            cai.commandPool = g->load_pool;
            r = g->f.vkAllocateCommandBuffers(g->dev, &cai, &g->load_cmd);
        }
        VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (r == VK_SUCCESS && !g->load_fence)
            r = g->f.vkCreateFence(g->dev, &fci, nullptr, &g->load_fence);
        if (r != VK_SUCCESS) {
            err = "load: command pool or fence failed: " + std::to_string((int)r);
            return false;
        }
        g->load_family = queue_family;
    }

    uint8_t* up = (uint8_t*)g->up.map;
    VkDeviceSize at = 0;
    for (uint32_t i = 0; i < count; i++) {
        VkDeviceSize padded = align_up(files[i].size(), 4);
        if (!files[i].empty())
            memcpy(up + at, files[i].data(), files[i].size());
        memset(up + at + files[i].size(), 0, (size_t)(padded - files[i].size()));
        at += padded;
    }
    memcpy(up + up_tiles, tiles.data(), (size_t)tiles_size);

    g->f.vkResetCommandPool(g->dev, g->load_pool, 0);
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VkCommandBuffer cmd = g->load_cmd;
    g->f.vkBeginCommandBuffer(cmd, &bi);
    VkBufferCopy c_in = {0, 0, in_size};
    VkBufferCopy c_tiles = {up_tiles, 0, tiles_size};
    g->f.vkCmdCopyBuffer(cmd, g->up.buf, g->in.buf, 1, &c_in);
    g->f.vkCmdCopyBuffer(cmd, g->up.buf, g->tiles.buf, 1, &c_tiles);
    barrier(*g, cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_SHADER_READ_BIT);
    record(g, cmd, job_t{g->in.addr, g->tiles.addr, g->out.addr, g->status.addr, (uint32_t)tiles.size(),
                         job_flags(tiles, (size_t)out_size)});
    barrier(*g, cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
            VK_ACCESS_TRANSFER_READ_BIT);
    for (uint32_t i = 0; i < count; i++) {
        const load_t& item = items[i];
        if (!spans[i].size)
            continue;
        if (item.buffer) {
            VkBufferCopy c = {spans[i].out, item.buffer_offset, spans[i].size};
            g->f.vkCmdCopyBuffer(cmd, g->out.buf, item.buffer, 1, &c);
            continue;
        }
        VkImageMemoryBarrier ib = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        ib.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        ib.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        ib.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        ib.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        ib.image = item.image;
        ib.subresourceRange = {item.layers.aspectMask, item.layers.mipLevel, 1, item.layers.baseArrayLayer, item.layers.layerCount};
        g->f.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                                  nullptr, 1, &ib);
        VkBufferImageCopy c = {};
        c.bufferOffset = spans[i].out;
        c.imageSubresource = item.layers;
        c.imageExtent = item.extent;
        g->f.vkCmdCopyBufferToImage(cmd, g->out.buf, item.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        ib.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        ib.dstAccessMask = 0;
        ib.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        ib.newLayout = item.layout;
        g->f.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr,
                                  0, nullptr, 1, &ib);
    }
    VkBufferCopy c_status = {0, 0, status_size};
    g->f.vkCmdCopyBuffer(cmd, g->status.buf, g->down.buf, 1, &c_status);
    barrier(*g, cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_HOST_READ_BIT);
    g->f.vkEndCommandBuffer(cmd);

    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    if ((r = g->f.vkQueueSubmit(queue, 1, &si, g->load_fence)) != VK_SUCCESS) {
        err = "vkQueueSubmit failed: " + std::to_string((int)r);
        return false;
    }
    r = g->f.vkWaitForFences(g->dev, 1, &g->load_fence, VK_TRUE, UINT64_MAX);
    g->f.vkResetFences(g->dev, 1, &g->load_fence);
    if (r != VK_SUCCESS) {
        err = "vkWaitForFences failed: " + std::to_string((int)r);
        return false;
    }
    const uint32_t* status = (const uint32_t*)g->down.map;
    for (size_t i = 0; i < owners.size(); i++)
        if (status[i]) {
            err = "item " + std::to_string(owners[i].item) + ", tile " + std::to_string(owners[i].index) + ": " +
                  tile_error(status[i]);
            return false;
        }
    return true;
}

bool decompress(ctx_t* g, const uint8_t* in, size_t n, uint8_t* out, size_t size, std::string& err, timing_t* timing) {
    if (!g->owns_device) {
        err = "decompress needs a context from create(); an attached context only records";
        return false;
    }
    std::vector<tile_desc_t> descs;
    size_t total = 0;
    if (!describe(in, n, n, descs, total, err))
        return false;
    if (size < total) {
        err = "output buffer too small";
        return false;
    }
    if (descs.empty())
        return true;

    constexpr VkBufferUsageFlags storage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    constexpr VkBufferUsageFlags xfer_dst = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    constexpr VkBufferUsageFlags xfer_src = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    constexpr VkMemoryPropertyFlags local = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    constexpr VkMemoryPropertyFlags visible = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    VkDeviceSize in_size = align_up(n, 4);
    VkDeviceSize tiles_size = descs.size() * sizeof(tile_desc_t);
    VkDeviceSize status_size = descs.size() * 4;
    VkDeviceSize up_tiles = align_up(in_size, 256);
    VkDeviceSize down_status = align_up(total, 256);
    if (!make_buf(*g, g->in, in_size, storage | xfer_dst, local, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) ||
        !make_buf(*g, g->tiles, tiles_size, storage | xfer_dst, local, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) ||
        !make_buf(*g, g->out, total, storage | xfer_src, local, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) ||
        !make_buf(*g, g->status, status_size, storage | xfer_src, local, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) ||
        !make_buf(*g, g->up, up_tiles + tiles_size, xfer_src, visible, local) ||
        !make_buf(*g, g->down, down_status + status_size, xfer_dst, visible | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, local)) {
        err = g->err;
        return false;
    }

    auto t0 = std::chrono::steady_clock::now();
    uint8_t* up = (uint8_t*)g->up.map;
    memcpy(up, in, n);
    if (in_size > n)
        memset(up + n, 0, (size_t)(in_size - n));
    memcpy(up + up_tiles, descs.data(), (size_t)tiles_size);
    auto t1 = std::chrono::steady_clock::now();

    g->f.vkResetCommandPool(g->dev, g->pool, 0);
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VkCommandBuffer cmd = g->cmd;
    g->f.vkBeginCommandBuffer(cmd, &bi);
    g->f.vkCmdResetQueryPool(cmd, g->queries, 0, 4);
    g->f.vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, g->queries, 0);
    VkBufferCopy c_in = {0, 0, in_size};
    VkBufferCopy c_tiles = {up_tiles, 0, tiles_size};
    g->f.vkCmdCopyBuffer(cmd, g->up.buf, g->in.buf, 1, &c_in);
    g->f.vkCmdCopyBuffer(cmd, g->up.buf, g->tiles.buf, 1, &c_tiles);
    barrier(*g, cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_SHADER_READ_BIT);
    g->f.vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g->queries, 1);
    job_t job = {g->in.addr, g->tiles.addr, g->out.addr, g->status.addr, (uint32_t)descs.size(), job_flags(descs, total)};
    record(g, cmd, job);
    barrier(*g, cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
            VK_ACCESS_TRANSFER_READ_BIT);
    g->f.vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g->queries, 2);
    VkBufferCopy c_out = {0, 0, total};
    VkBufferCopy c_status = {0, down_status, status_size};
    g->f.vkCmdCopyBuffer(cmd, g->out.buf, g->down.buf, 1, &c_out);
    g->f.vkCmdCopyBuffer(cmd, g->status.buf, g->down.buf, 1, &c_status);
    barrier(*g, cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_HOST_READ_BIT);
    g->f.vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g->queries, 3);
    g->f.vkEndCommandBuffer(cmd);

    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    VkResult r = g->f.vkQueueSubmit(g->queue, 1, &si, g->fence);
    if (r != VK_SUCCESS) {
        err = "vkQueueSubmit failed: " + std::to_string((int)r);
        return false;
    }
    r = g->f.vkWaitForFences(g->dev, 1, &g->fence, VK_TRUE, UINT64_MAX);
    g->f.vkResetFences(g->dev, 1, &g->fence);
    if (r != VK_SUCCESS) {
        err = "vkWaitForFences failed: " + std::to_string((int)r);
        return false;
    }
    auto t2 = std::chrono::steady_clock::now();

    memcpy(out, g->down.map, total);
    const uint32_t* status = (const uint32_t*)((const uint8_t*)g->down.map + down_status);
    for (size_t i = 0; i < descs.size(); i++)
        if (status[i]) {
            err = "tile " + std::to_string(descs[i].dst / gdeflate::TILE_SIZE) + ": " + tile_error(status[i]);
            return false;
        }
    auto t3 = std::chrono::steady_clock::now();

    if (timing) {
        uint64_t ts[4];
        g->f.vkGetQueryPoolResults(g->dev, g->queries, 0, 4, sizeof(ts), ts, 8,
                                   VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
        double tick = g->ts_period * 1e-6;
        timing->host_up_ms = ms_between(t0, t1);
        timing->submit_ms = ms_between(t1, t2);
        timing->host_down_ms = ms_between(t2, t3);
        timing->total_ms = ms_between(t0, t3);
        timing->gpu_up_ms = (double)(ts[1] - ts[0]) * tick;
        timing->kernel_ms = (double)(ts[2] - ts[1]) * tick;
        timing->gpu_down_ms = (double)(ts[3] - ts[2]) * tick;
        timing->upload_bytes = (size_t)(in_size + tiles_size);
    }
    return true;
}

}
