// VK_LAYER_vkdeflate_memory_decompression - adds VK_EXT_memory_decompression and VK_NV_memory_decompression
// (GDeflate 1.0) to devices whose driver lacks them. Each decompress command becomes a dispatch of vkdeflate's
// compute kernel.
//
// Enable it by name, or with VK_INSTANCE_LAYERS and VK_LAYER_PATH pointing at the folder with the manifest.
// VKDEFLATE_LAYER_FORCE=1 keeps it active when the driver has the extension too.
// VKDEFLATE_LAYER_MAX_COUNT is the region limit of the NV indirect call, which carries none (default 65536).
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
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "vkdeflate.h"

#if defined(_WIN32)
#define LAYER_EXPORT extern "C" __declspec(dllexport)
#else
#define LAYER_EXPORT extern "C" __attribute__((visibility("default")))
#endif

namespace {

constexpr const char* layer_name = "VK_LAYER_vkdeflate_memory_decompression";
constexpr const char* ext_names[2] = {"VK_EXT_memory_decompression", "VK_NV_memory_decompression"};
constexpr const char* subgroup_ext = "VK_EXT_subgroup_size_control";
constexpr VkFlags64 gdeflate_bit = 1;

struct instance_link_t {
    instance_link_t* next;
    PFN_vkGetInstanceProcAddr gipa;
    void* gpdpa;
};

struct device_link_t {
    device_link_t* next;
    PFN_vkGetInstanceProcAddr gipa;
    PFN_vkGetDeviceProcAddr gdpa;
};

struct link_info_t {
    VkStructureType type;
    const void* next;
    int function;
    void* link;
};

struct negotiate_t {
    int type;
    void* next;
    uint32_t version;
    PFN_vkGetInstanceProcAddr gipa;
    PFN_vkGetDeviceProcAddr gdpa;
    void* gpdpa;
};

struct instance_t {
    VkInstance handle = VK_NULL_HANDLE;
    PFN_vkGetInstanceProcAddr gipa = nullptr;
    uint32_t api = 0;
    PFN_vkDestroyInstance destroy = nullptr;
    PFN_vkEnumerateDeviceExtensionProperties extensions = nullptr;
    PFN_vkGetPhysicalDeviceProperties properties = nullptr;
    PFN_vkGetPhysicalDeviceFeatures2 features2 = nullptr;
    PFN_vkGetPhysicalDeviceProperties2 properties2 = nullptr;
};

struct push_call_t {
    VkPipelineLayout layout;
    VkShaderStageFlags stages;
    uint32_t offset;
    std::vector<uint8_t> data;
};

struct state_t {
    VkPipeline pipeline = VK_NULL_HANDLE;
    std::vector<push_call_t> pushes;
};

struct device_t {
    VkDevice handle = VK_NULL_HANDLE;
    PFN_vkGetDeviceProcAddr gdpa = nullptr;
    vkdeflate::ctx_t* ctx = nullptr;
    uint32_t max_count = 65536;
    std::mutex lock;
    std::unordered_map<VkCommandBuffer, state_t> states;
    PFN_vkDestroyDevice destroy = nullptr;
    PFN_vkCmdBindPipeline bind = nullptr;
    PFN_vkCmdPushConstants push = nullptr;
    PFN_vkBeginCommandBuffer begin = nullptr;
    PFN_vkFreeCommandBuffers free_cmds = nullptr;
    PFN_vkCmdExecuteCommands execute = nullptr;
    PFN_vkCmdDecompressMemoryNV nv = nullptr;
    PFN_vkCmdDecompressMemoryIndirectCountNV nv_count = nullptr;
    PFN_vkCmdDecompressMemoryEXT ext = nullptr;
    PFN_vkCmdDecompressMemoryIndirectCountEXT ext_count = nullptr;
};

std::mutex table_lock;
std::unordered_map<void*, instance_t*> instances;
std::unordered_map<void*, device_t*> devices;

template <typename T>
static void* key(T handle) {
    return *(void**)handle;
}

template <typename T>
static instance_t* instance_of(T handle) {
    std::lock_guard<std::mutex> hold(table_lock);
    auto it = instances.find(key(handle));
    return it == instances.end() ? nullptr : it->second;
}

template <typename T>
static device_t* device_of(T handle) {
    std::lock_guard<std::mutex> hold(table_lock);
    auto it = devices.find(key(handle));
    return it == devices.end() ? nullptr : it->second;
}

static bool env_on(const char* name) {
    const char* v = getenv(name);
    return v && *v && strcmp(v, "0") != 0;
}

static std::vector<VkExtensionProperties> driver_extensions(instance_t* in, VkPhysicalDevice phys) {
    uint32_t n = 0;
    in->extensions(phys, nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> list(n);
    if (n)
        in->extensions(phys, nullptr, &n, list.data());
    list.resize(n);
    return list;
}

static bool listed(const std::vector<VkExtensionProperties>& list, const char* name) {
    for (const VkExtensionProperties& e : list)
        if (!strcmp(e.extensionName, name))
            return true;
    return false;
}

static bool usable(instance_t* in, VkPhysicalDevice phys) {
    if (in->api < VK_API_VERSION_1_2 || !in->features2 || !in->properties2)
        return false;
    VkPhysicalDeviceProperties base;
    in->properties(phys, &base);
    if (base.apiVersion < VK_API_VERSION_1_2)
        return false;
    std::vector<VkExtensionProperties> list = driver_extensions(in, phys);
    if (!env_on("VKDEFLATE_LAYER_FORCE") && (listed(list, ext_names[0]) || listed(list, ext_names[1])))
        return false;
    bool control = base.apiVersion >= VK_API_VERSION_1_3 || listed(list, subgroup_ext);
    VkPhysicalDeviceSubgroupSizeControlProperties psc = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_PROPERTIES};
    VkPhysicalDeviceVulkan11Properties p11 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES, control ? &psc : nullptr};
    VkPhysicalDeviceProperties2 p2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &p11};
    in->properties2(phys, &p2);
    VkPhysicalDeviceSubgroupSizeControlFeatures fsc = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES};
    VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, control ? &fsc : nullptr};
    VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f12};
    in->features2(phys, &f2);
    if (!f2.features.shaderInt64 || !f12.shaderInt8 || !f12.storageBuffer8BitAccess || !f12.bufferDeviceAddress ||
        !f12.vulkanMemoryModel)
        return false;
    bool sized = control && fsc.subgroupSizeControl && fsc.computeFullSubgroups &&
                 (psc.requiredSubgroupSizeStages & VK_SHADER_STAGE_COMPUTE_BIT) && psc.minSubgroupSize <= 32 &&
                 psc.maxSubgroupSize >= 32;
    return p11.subgroupSize >= 32 || sized;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL down_gdpa(VkDevice dev, const char* name) {
    device_t* d = device_of(dev);
    return d ? d->gdpa(dev, name) : nullptr;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL down_gipa(VkInstance instance, const char* name) {
    instance_t* in = instance_of(instance);
    if (!in)
        return nullptr;
    if (!strcmp(name, "vkGetDeviceProcAddr"))
        return (PFN_vkVoidFunction)down_gdpa;
    return in->gipa(instance, name);
}

static VKAPI_ATTR VkResult VKAPI_CALL create_instance(const VkInstanceCreateInfo* info, const VkAllocationCallbacks* alloc,
                                                      VkInstance* out) {
    link_info_t* chain = (link_info_t*)info->pNext;
    while (chain && !(chain->type == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO && chain->function == 0))
        chain = (link_info_t*)chain->next;
    if (!chain || !chain->link)
        return VK_ERROR_INITIALIZATION_FAILED;
    instance_link_t* link = (instance_link_t*)chain->link;
    PFN_vkGetInstanceProcAddr gipa = link->gipa;
    chain->link = link->next;
    PFN_vkCreateInstance create = (PFN_vkCreateInstance)gipa(VK_NULL_HANDLE, "vkCreateInstance");
    if (!create)
        return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = create(info, alloc, out);
    if (r != VK_SUCCESS)
        return r;
    instance_t* in = new instance_t;
    in->handle = *out;
    in->gipa = gipa;
    in->api = info->pApplicationInfo && info->pApplicationInfo->apiVersion ? info->pApplicationInfo->apiVersion : VK_API_VERSION_1_0;
    in->destroy = (PFN_vkDestroyInstance)gipa(*out, "vkDestroyInstance");
    in->extensions = (PFN_vkEnumerateDeviceExtensionProperties)gipa(*out, "vkEnumerateDeviceExtensionProperties");
    in->properties = (PFN_vkGetPhysicalDeviceProperties)gipa(*out, "vkGetPhysicalDeviceProperties");
    in->features2 = (PFN_vkGetPhysicalDeviceFeatures2)gipa(*out, "vkGetPhysicalDeviceFeatures2");
    in->properties2 = (PFN_vkGetPhysicalDeviceProperties2)gipa(*out, "vkGetPhysicalDeviceProperties2");
    if (!in->features2)
        in->features2 = (PFN_vkGetPhysicalDeviceFeatures2)gipa(*out, "vkGetPhysicalDeviceFeatures2KHR");
    if (!in->properties2)
        in->properties2 = (PFN_vkGetPhysicalDeviceProperties2)gipa(*out, "vkGetPhysicalDeviceProperties2KHR");
    std::lock_guard<std::mutex> hold(table_lock);
    instances[key(*out)] = in;
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL destroy_instance(VkInstance instance, const VkAllocationCallbacks* alloc) {
    if (!instance)
        return;
    instance_t* in = nullptr;
    {
        std::lock_guard<std::mutex> hold(table_lock);
        auto it = instances.find(key(instance));
        if (it == instances.end())
            return;
        in = it->second;
        instances.erase(it);
    }
    in->destroy(instance, alloc);
    delete in;
}

static VKAPI_ATTR VkResult VKAPI_CALL enumerate_extensions(VkPhysicalDevice phys, const char* layer, uint32_t* count,
                                                           VkExtensionProperties* props) {
    instance_t* in = instance_of(phys);
    bool ours = layer && !strcmp(layer, layer_name);
    if (layer && !ours)
        return in->extensions(phys, layer, count, props);
    std::vector<VkExtensionProperties> list;
    if (!ours)
        list = driver_extensions(in, phys);
    if (ours || usable(in, phys))
        for (const char* name : ext_names)
            if (!listed(list, name)) {
                VkExtensionProperties e = {};
                strncpy(e.extensionName, name, sizeof(e.extensionName) - 1);
                e.specVersion = 1;
                list.push_back(e);
            }
    if (!props) {
        *count = (uint32_t)list.size();
        return VK_SUCCESS;
    }
    uint32_t n = *count < list.size() ? *count : (uint32_t)list.size();
    memcpy(props, list.data(), n * sizeof(VkExtensionProperties));
    *count = n;
    return n < list.size() ? VK_INCOMPLETE : VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL features2(VkPhysicalDevice phys, VkPhysicalDeviceFeatures2* out) {
    instance_t* in = instance_of(phys);
    if (!in->features2)
        return;
    in->features2(phys, out);
    if (!usable(in, phys))
        return;
    for (VkBaseOutStructure* s = (VkBaseOutStructure*)out->pNext; s; s = s->pNext)
        if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_DECOMPRESSION_FEATURES_EXT)
            ((VkPhysicalDeviceMemoryDecompressionFeaturesEXT*)s)->memoryDecompression = VK_TRUE;
}

static uint32_t max_count_setting() {
    const char* v = getenv("VKDEFLATE_LAYER_MAX_COUNT");
    int n = v ? atoi(v) : 0;
    return n > 0 ? (uint32_t)n : 65536;
}

static VKAPI_ATTR void VKAPI_CALL properties2(VkPhysicalDevice phys, VkPhysicalDeviceProperties2* out) {
    instance_t* in = instance_of(phys);
    if (!in->properties2)
        return;
    in->properties2(phys, out);
    if (!usable(in, phys))
        return;
    for (VkBaseOutStructure* s = (VkBaseOutStructure*)out->pNext; s; s = s->pNext)
        if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_DECOMPRESSION_PROPERTIES_EXT) {
            VkPhysicalDeviceMemoryDecompressionPropertiesEXT* p = (VkPhysicalDeviceMemoryDecompressionPropertiesEXT*)s;
            p->decompressionMethods = gdeflate_bit;
            p->maxDecompressionIndirectCount = max_count_setting();
        }
}

struct undo_t {
    std::vector<std::pair<VkBool32*, VkBool32>> list;

    void set(VkBool32& field) {
        list.push_back({&field, field});
        field = VK_TRUE;
    }

    ~undo_t() {
        for (auto& e : list)
            *e.first = e.second;
    }
};

static VkBaseOutStructure* find(const void* chain, VkStructureType type) {
    for (VkBaseOutStructure* s = (VkBaseOutStructure*)chain; s; s = s->pNext)
        if (s->sType == type)
            return s;
    return nullptr;
}

static VKAPI_ATTR VkResult VKAPI_CALL create_device(VkPhysicalDevice phys, const VkDeviceCreateInfo* info,
                                                    const VkAllocationCallbacks* alloc, VkDevice* out) {
    instance_t* in = instance_of(phys);
    link_info_t* chain = (link_info_t*)info->pNext;
    while (chain && !(chain->type == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO && chain->function == 0))
        chain = (link_info_t*)chain->next;
    if (!in || !chain || !chain->link)
        return VK_ERROR_INITIALIZATION_FAILED;
    device_link_t* link = (device_link_t*)chain->link;
    PFN_vkGetInstanceProcAddr gipa = link->gipa;
    PFN_vkGetDeviceProcAddr gdpa = link->gdpa;
    chain->link = link->next;
    PFN_vkCreateDevice create = (PFN_vkCreateDevice)gipa(in->handle, "vkCreateDevice");
    if (!create)
        return VK_ERROR_INITIALIZATION_FAILED;

    std::vector<const char*> names;
    bool wanted = false;
    for (uint32_t i = 0; i < info->enabledExtensionCount; i++) {
        const char* name = info->ppEnabledExtensionNames[i];
        if (!strcmp(name, ext_names[0]) || !strcmp(name, ext_names[1]))
            wanted = true;
        else
            names.push_back(name);
    }
    bool active = wanted && usable(in, phys);
    if (!active) {
        VkResult r = create(phys, info, alloc, out);
        if (r != VK_SUCCESS)
            return r;
        device_t* d = new device_t;
        d->handle = *out;
        d->gdpa = gdpa;
        d->destroy = (PFN_vkDestroyDevice)gdpa(*out, "vkDestroyDevice");
        d->nv = (PFN_vkCmdDecompressMemoryNV)gdpa(*out, "vkCmdDecompressMemoryNV");
        d->nv_count = (PFN_vkCmdDecompressMemoryIndirectCountNV)gdpa(*out, "vkCmdDecompressMemoryIndirectCountNV");
        d->ext = (PFN_vkCmdDecompressMemoryEXT)gdpa(*out, "vkCmdDecompressMemoryEXT");
        d->ext_count = (PFN_vkCmdDecompressMemoryIndirectCountEXT)gdpa(*out, "vkCmdDecompressMemoryIndirectCountEXT");
        std::lock_guard<std::mutex> hold(table_lock);
        devices[key(*out)] = d;
        return VK_SUCCESS;
    }

    VkPhysicalDeviceProperties base;
    in->properties(phys, &base);
    std::vector<VkExtensionProperties> list = driver_extensions(in, phys);
    bool core13 = base.apiVersion >= VK_API_VERSION_1_3 && in->api >= VK_API_VERSION_1_3;
    bool control = core13 || listed(list, subgroup_ext);
    VkPhysicalDeviceSubgroupSizeControlFeatures have = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES};
    VkPhysicalDeviceFeatures2 query = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, control ? &have : nullptr};
    in->features2(phys, &query);
    control = control && have.subgroupSizeControl && have.computeFullSubgroups;

    VkDeviceCreateInfo copy = *info;
    undo_t undo;
    VkPhysicalDeviceFeatures plain = {};
    if (VkPhysicalDeviceFeatures2* f2 = (VkPhysicalDeviceFeatures2*)find(info->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2))
        undo.set(f2->features.shaderInt64);
    else {
        if (info->pEnabledFeatures)
            plain = *info->pEnabledFeatures;
        plain.shaderInt64 = VK_TRUE;
        copy.pEnabledFeatures = &plain;
    }
    VkPhysicalDevice8BitStorageFeatures own_8bit = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES};
    VkPhysicalDeviceShaderFloat16Int8Features own_int8 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES};
    VkPhysicalDeviceBufferDeviceAddressFeatures own_bda = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES};
    VkPhysicalDeviceVulkanMemoryModelFeatures own_model = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES};
    VkPhysicalDeviceSubgroupSizeControlFeatures own_size = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES};
    auto add = [&](void* s) {
        ((VkBaseOutStructure*)s)->pNext = (VkBaseOutStructure*)copy.pNext;
        copy.pNext = s;
    };
    if (VkPhysicalDeviceVulkan12Features* f12 = (VkPhysicalDeviceVulkan12Features*)find(info->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES)) {
        undo.set(f12->storageBuffer8BitAccess);
        undo.set(f12->shaderInt8);
        undo.set(f12->bufferDeviceAddress);
        undo.set(f12->vulkanMemoryModel);
    } else {
        if (auto* s = (VkPhysicalDevice8BitStorageFeatures*)find(info->pNext, own_8bit.sType))
            undo.set(s->storageBuffer8BitAccess);
        else {
            own_8bit.storageBuffer8BitAccess = VK_TRUE;
            add(&own_8bit);
        }
        if (auto* s = (VkPhysicalDeviceShaderFloat16Int8Features*)find(info->pNext, own_int8.sType))
            undo.set(s->shaderInt8);
        else {
            own_int8.shaderInt8 = VK_TRUE;
            add(&own_int8);
        }
        if (auto* s = (VkPhysicalDeviceBufferDeviceAddressFeatures*)find(info->pNext, own_bda.sType))
            undo.set(s->bufferDeviceAddress);
        else {
            own_bda.bufferDeviceAddress = VK_TRUE;
            add(&own_bda);
        }
        if (auto* s = (VkPhysicalDeviceVulkanMemoryModelFeatures*)find(info->pNext, own_model.sType))
            undo.set(s->vulkanMemoryModel);
        else {
            own_model.vulkanMemoryModel = VK_TRUE;
            add(&own_model);
        }
    }
    if (control) {
        if (VkPhysicalDeviceVulkan13Features* f13 = (VkPhysicalDeviceVulkan13Features*)find(info->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES)) {
            undo.set(f13->subgroupSizeControl);
            undo.set(f13->computeFullSubgroups);
        } else if (auto* s = (VkPhysicalDeviceSubgroupSizeControlFeatures*)find(info->pNext, own_size.sType)) {
            undo.set(s->subgroupSizeControl);
            undo.set(s->computeFullSubgroups);
        } else {
            own_size.subgroupSizeControl = VK_TRUE;
            own_size.computeFullSubgroups = VK_TRUE;
            add(&own_size);
        }
        bool named = false;
        for (const char* name : names)
            named = named || !strcmp(name, subgroup_ext);
        if (!core13 && !named)
            names.push_back(subgroup_ext);
    }
    copy.enabledExtensionCount = (uint32_t)names.size();
    copy.ppEnabledExtensionNames = names.data();

    VkResult r = create(phys, &copy, alloc, out);
    if (r != VK_SUCCESS)
        return r;
    device_t* d = new device_t;
    d->handle = *out;
    d->gdpa = gdpa;
    d->max_count = max_count_setting();
    d->destroy = (PFN_vkDestroyDevice)gdpa(*out, "vkDestroyDevice");
    d->bind = (PFN_vkCmdBindPipeline)gdpa(*out, "vkCmdBindPipeline");
    d->push = (PFN_vkCmdPushConstants)gdpa(*out, "vkCmdPushConstants");
    d->begin = (PFN_vkBeginCommandBuffer)gdpa(*out, "vkBeginCommandBuffer");
    d->free_cmds = (PFN_vkFreeCommandBuffers)gdpa(*out, "vkFreeCommandBuffers");
    d->execute = (PFN_vkCmdExecuteCommands)gdpa(*out, "vkCmdExecuteCommands");
    {
        std::lock_guard<std::mutex> hold(table_lock);
        devices[key(*out)] = d;
    }
    std::string err;
    d->ctx = vkdeflate::attach(in->handle, phys, *out, down_gipa, {}, err);
    if (!d->ctx) {
        fprintf(stderr, "[vkdeflate layer] %s\n", err.c_str());
        {
            std::lock_guard<std::mutex> hold(table_lock);
            devices.erase(key(*out));
        }
        d->destroy(*out, alloc);
        delete d;
        return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL destroy_device(VkDevice dev, const VkAllocationCallbacks* alloc) {
    if (!dev)
        return;
    device_t* d = device_of(dev);
    if (!d)
        return;
    if (d->ctx)
        vkdeflate::destroy(d->ctx);
    {
        std::lock_guard<std::mutex> hold(table_lock);
        devices.erase(key(dev));
    }
    d->destroy(dev, alloc);
    delete d;
}

static VKAPI_ATTR void VKAPI_CALL bind_pipeline(VkCommandBuffer cmd, VkPipelineBindPoint point, VkPipeline pipeline) {
    device_t* d = device_of(cmd);
    if (point == VK_PIPELINE_BIND_POINT_COMPUTE) {
        std::lock_guard<std::mutex> hold(d->lock);
        d->states[cmd].pipeline = pipeline;
    }
    d->bind(cmd, point, pipeline);
}

static VKAPI_ATTR void VKAPI_CALL push_constants(VkCommandBuffer cmd, VkPipelineLayout layout, VkShaderStageFlags stages,
                                                 uint32_t offset, uint32_t size, const void* values) {
    device_t* d = device_of(cmd);
    if (stages & VK_SHADER_STAGE_COMPUTE_BIT) {
        std::lock_guard<std::mutex> hold(d->lock);
        std::vector<push_call_t>& pushes = d->states[cmd].pushes;
        for (size_t i = 0; i < pushes.size(); i++)
            if (pushes[i].layout == layout && pushes[i].stages == stages && pushes[i].offset == offset && pushes[i].data.size() == size) {
                pushes.erase(pushes.begin() + i);
                break;
            }
        pushes.push_back({layout, stages, offset, std::vector<uint8_t>((const uint8_t*)values, (const uint8_t*)values + size)});
    }
    d->push(cmd, layout, stages, offset, size, values);
}

static VKAPI_ATTR VkResult VKAPI_CALL begin_command_buffer(VkCommandBuffer cmd, const VkCommandBufferBeginInfo* info) {
    device_t* d = device_of(cmd);
    {
        std::lock_guard<std::mutex> hold(d->lock);
        d->states.erase(cmd);
    }
    return d->begin(cmd, info);
}

static VKAPI_ATTR void VKAPI_CALL free_command_buffers(VkDevice dev, VkCommandPool pool, uint32_t count,
                                                       const VkCommandBuffer* cmds) {
    device_t* d = device_of(dev);
    {
        std::lock_guard<std::mutex> hold(d->lock);
        for (uint32_t i = 0; cmds && i < count; i++)
            d->states.erase(cmds[i]);
    }
    d->free_cmds(dev, pool, count, cmds);
}

static VKAPI_ATTR void VKAPI_CALL execute_commands(VkCommandBuffer cmd, uint32_t count, const VkCommandBuffer* cmds) {
    device_t* d = device_of(cmd);
    {
        std::lock_guard<std::mutex> hold(d->lock);
        d->states.erase(cmd);
    }
    d->execute(cmd, count, cmds);
}

static void restore(device_t* d, VkCommandBuffer cmd) {
    auto it = d->states.find(cmd);
    if (it == d->states.end())
        return;
    if (it->second.pipeline)
        d->bind(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, it->second.pipeline);
    for (const push_call_t& p : it->second.pushes)
        d->push(cmd, p.layout, p.stages, p.offset, (uint32_t)p.data.size(), p.data.data());
}

static VKAPI_ATTR void VKAPI_CALL decompress_nv(VkCommandBuffer cmd, uint32_t count, const VkDecompressMemoryRegionNV* regions) {
    device_t* d = device_of(cmd);
    if (!d->ctx) {
        if (d->nv)
            d->nv(cmd, count, regions);
        return;
    }
    std::vector<vkdeflate::region_t> list;
    for (uint32_t i = 0; i < count; i++)
        if (regions[i].decompressionMethod & gdeflate_bit)
            list.push_back({regions[i].srcAddress, regions[i].dstAddress, regions[i].compressedSize, regions[i].decompressedSize});
    std::lock_guard<std::mutex> hold(d->lock);
    if (!vkdeflate::cmd_decompress_memory(d->ctx, cmd, list.data(), (uint32_t)list.size()))
        fprintf(stderr, "[vkdeflate layer] vkCmdDecompressMemoryNV not recorded: region buffer could not be made\n");
    restore(d, cmd);
}

static VKAPI_ATTR void VKAPI_CALL decompress_count_nv(VkCommandBuffer cmd, VkDeviceAddress regions, VkDeviceAddress count,
                                                      uint32_t stride) {
    device_t* d = device_of(cmd);
    if (!d->ctx) {
        if (d->nv_count)
            d->nv_count(cmd, regions, count, stride);
        return;
    }
    std::lock_guard<std::mutex> hold(d->lock);
    vkdeflate::record_regions_indirect(d->ctx, cmd, regions, count, d->max_count, stride);
    restore(d, cmd);
}

static VKAPI_ATTR void VKAPI_CALL decompress_ext(VkCommandBuffer cmd, const VkDecompressMemoryInfoEXT* info) {
    device_t* d = device_of(cmd);
    if (!d->ctx) {
        if (d->ext)
            d->ext(cmd, info);
        return;
    }
    if (!(info->decompressionMethod & gdeflate_bit))
        return;
    std::lock_guard<std::mutex> hold(d->lock);
    if (!vkdeflate::cmd_decompress_memory(d->ctx, cmd, (const vkdeflate::region_t*)info->pRegions, info->regionCount))
        fprintf(stderr, "[vkdeflate layer] vkCmdDecompressMemoryEXT not recorded: region buffer could not be made\n");
    restore(d, cmd);
}

static VKAPI_ATTR void VKAPI_CALL decompress_count_ext(VkCommandBuffer cmd, VkMemoryDecompressionMethodFlagsEXT method,
                                                       VkDeviceAddress regions, VkDeviceAddress count, uint32_t max_count,
                                                       uint32_t stride) {
    device_t* d = device_of(cmd);
    if (!d->ctx) {
        if (d->ext_count)
            d->ext_count(cmd, method, regions, count, max_count, stride);
        return;
    }
    if (!(method & gdeflate_bit))
        return;
    std::lock_guard<std::mutex> hold(d->lock);
    vkdeflate::record_regions_indirect(d->ctx, cmd, regions, count, max_count, stride);
    restore(d, cmd);
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL get_device_proc_addr(VkDevice dev, const char* name);

static PFN_vkVoidFunction device_entry(const char* name, bool active) {
    struct entry_t {
        const char* name;
        PFN_vkVoidFunction fn;
        bool always;
    };
    static const entry_t table[] = {
        {"vkGetDeviceProcAddr", (PFN_vkVoidFunction)get_device_proc_addr, true},
        {"vkDestroyDevice", (PFN_vkVoidFunction)destroy_device, true},
        {"vkCmdDecompressMemoryNV", (PFN_vkVoidFunction)decompress_nv, true},
        {"vkCmdDecompressMemoryIndirectCountNV", (PFN_vkVoidFunction)decompress_count_nv, true},
        {"vkCmdDecompressMemoryEXT", (PFN_vkVoidFunction)decompress_ext, true},
        {"vkCmdDecompressMemoryIndirectCountEXT", (PFN_vkVoidFunction)decompress_count_ext, true},
        {"vkCmdBindPipeline", (PFN_vkVoidFunction)bind_pipeline, false},
        {"vkCmdPushConstants", (PFN_vkVoidFunction)push_constants, false},
        {"vkBeginCommandBuffer", (PFN_vkVoidFunction)begin_command_buffer, false},
        {"vkFreeCommandBuffers", (PFN_vkVoidFunction)free_command_buffers, false},
        {"vkCmdExecuteCommands", (PFN_vkVoidFunction)execute_commands, false},
    };
    for (const entry_t& e : table)
        if (!strcmp(name, e.name))
            return e.always || active ? e.fn : nullptr;
    return nullptr;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL get_device_proc_addr(VkDevice dev, const char* name) {
    device_t* d = device_of(dev);
    if (!d)
        return nullptr;
    if (PFN_vkVoidFunction fn = device_entry(name, d->ctx != nullptr)) {
        bool command = !strncmp(name, "vkCmdDecompress", 15);
        if (!command || d->ctx || d->gdpa(dev, name))
            return fn;
        return nullptr;
    }
    return d->gdpa(dev, name);
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL get_instance_proc_addr(VkInstance instance, const char* name) {
    struct entry_t {
        const char* name;
        PFN_vkVoidFunction fn;
    };
    static const entry_t table[] = {
        {"vkGetInstanceProcAddr", (PFN_vkVoidFunction)get_instance_proc_addr},
        {"vkCreateInstance", (PFN_vkVoidFunction)create_instance},
        {"vkDestroyInstance", (PFN_vkVoidFunction)destroy_instance},
        {"vkCreateDevice", (PFN_vkVoidFunction)create_device},
        {"vkEnumerateDeviceExtensionProperties", (PFN_vkVoidFunction)enumerate_extensions},
        {"vkGetPhysicalDeviceFeatures2", (PFN_vkVoidFunction)features2},
        {"vkGetPhysicalDeviceFeatures2KHR", (PFN_vkVoidFunction)features2},
        {"vkGetPhysicalDeviceProperties2", (PFN_vkVoidFunction)properties2},
        {"vkGetPhysicalDeviceProperties2KHR", (PFN_vkVoidFunction)properties2},
        {"vkGetDeviceProcAddr", (PFN_vkVoidFunction)get_device_proc_addr},
        {"vkDestroyDevice", (PFN_vkVoidFunction)destroy_device},
    };
    for (const entry_t& e : table)
        if (!strcmp(name, e.name))
            return e.fn;
    if (!instance)
        return nullptr;
    instance_t* in = instance_of(instance);
    return in ? in->gipa(instance, name) : nullptr;
}

}

LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkNegotiateLoaderLayerInterfaceVersion(void* raw) {
    negotiate_t* n = (negotiate_t*)raw;
    if (!n || n->type != 1)
        return VK_ERROR_INITIALIZATION_FAILED;
    if (n->version > 2)
        n->version = 2;
    n->gipa = get_instance_proc_addr;
    n->gdpa = get_device_proc_addr;
    n->gpdpa = nullptr;
    return VK_SUCCESS;
}
