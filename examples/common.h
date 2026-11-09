#pragma once

#define VK_NO_PROTOTYPES
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
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

#define EX_FNS(X)                               \
    X(vkCreateInstance)                         \
    X(vkDestroyInstance)                        \
    X(vkEnumeratePhysicalDevices)               \
    X(vkGetPhysicalDeviceProperties)            \
    X(vkGetPhysicalDeviceQueueFamilyProperties) \
    X(vkGetPhysicalDeviceMemoryProperties)      \
    X(vkCreateDevice)                           \
    X(vkDestroyDevice)                          \
    X(vkGetDeviceQueue)                         \
    X(vkQueueWaitIdle)                          \
    X(vkCreateBuffer)                           \
    X(vkDestroyBuffer)                          \
    X(vkGetBufferMemoryRequirements)            \
    X(vkGetBufferDeviceAddress)                 \
    X(vkAllocateMemory)                         \
    X(vkFreeMemory)                             \
    X(vkBindBufferMemory)                       \
    X(vkMapMemory)                              \
    X(vkCreateCommandPool)                      \
    X(vkDestroyCommandPool)                     \
    X(vkAllocateCommandBuffers)                 \
    X(vkBeginCommandBuffer)                     \
    X(vkEndCommandBuffer)                       \
    X(vkCmdPipelineBarrier)                     \
    X(vkCmdFillBuffer)                          \
    X(vkQueueSubmit)

namespace ex {

#define EX_DECLARE(fn) inline PFN_##fn fn = nullptr;
inline PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;
EX_FNS(EX_DECLARE)

struct gpu_t {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice dev = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t family = 0;
    VkPhysicalDeviceMemoryProperties mem = {};
    VkCommandPool pool = VK_NULL_HANDLE;
};

struct buf_t {
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkDeviceAddress addr = 0;
    void* map = nullptr;
};

inline void die(const char* what, VkResult r = VK_SUCCESS) {
    if (r != VK_SUCCESS)
        printf("[example] %s failed: %d\n", what, (int)r);
    else
        printf("[example] %s\n", what);
    exit(2);
}

inline std::vector<uint8_t> load(const char* path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f)
        die((std::string("cant open ") + path).c_str());
    std::vector<uint8_t> d((size_t)f.tellg());
    f.seekg(0);
    f.read((char*)d.data(), (std::streamsize)d.size());
    return d;
}

inline gpu_t make_gpu() {
    gpu_t g;
#if defined(_WIN32)
    HMODULE lib = LoadLibraryW(L"vulkan-1.dll");
    if (!lib)
        die("vulkan-1.dll not found");
    vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)GetProcAddress(lib, "vkGetInstanceProcAddr");
#else
    void* lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!lib)
        die("libvulkan.so.1 not found");
    vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
#endif
    vkCreateInstance = (PFN_vkCreateInstance)vkGetInstanceProcAddr(nullptr, "vkCreateInstance");
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    VkResult r = vkCreateInstance(&ici, nullptr, &g.instance);
    if (r != VK_SUCCESS)
        die("vkCreateInstance", r);
#define EX_LOAD(fn) fn = (PFN_##fn)vkGetInstanceProcAddr(g.instance, #fn);
    EX_FNS(EX_LOAD)

    uint32_t count = 0;
    vkEnumeratePhysicalDevices(g.instance, &count, nullptr);
    if (!count)
        die("no vulkan device");
    std::vector<VkPhysicalDevice> devs(count);
    vkEnumeratePhysicalDevices(g.instance, &count, devs.data());
    g.phys = devs[0];
    VkPhysicalDeviceProperties props;
    for (VkPhysicalDevice d : devs) {
        vkGetPhysicalDeviceProperties(d, &props);
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            g.phys = d;
            break;
        }
    }
    vkGetPhysicalDeviceMemoryProperties(g.phys, &g.mem);

    uint32_t nfam = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(g.phys, &nfam, nullptr);
    std::vector<VkQueueFamilyProperties> fams(nfam);
    vkGetPhysicalDeviceQueueFamilyProperties(g.phys, &nfam, fams.data());
    g.family = UINT32_MAX;
    for (uint32_t i = 0; i < nfam && g.family == UINT32_MAX; i++)
        if (fams[i].queueFlags & VK_QUEUE_COMPUTE_BIT)
            g.family = i;
    if (g.family == UINT32_MAX)
        die("no compute queue");

    VkPhysicalDeviceVulkan13Features f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    f13.subgroupSizeControl = VK_TRUE;
    f13.computeFullSubgroups = VK_TRUE;
    VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &f13};
    f12.storageBuffer8BitAccess = VK_TRUE;
    f12.shaderInt8 = VK_TRUE;
    f12.bufferDeviceAddress = VK_TRUE;
    f12.vulkanMemoryModel = VK_TRUE;
    VkPhysicalDeviceFeatures f10 = {};
    f10.shaderInt64 = VK_TRUE;
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = g.family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f12};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.pEnabledFeatures = &f10;
    if ((r = vkCreateDevice(g.phys, &dci, nullptr, &g.dev)) != VK_SUCCESS)
        die("vkCreateDevice", r);
    vkGetDeviceQueue(g.dev, g.family, 0, &g.queue);

    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.queueFamilyIndex = g.family;
    if ((r = vkCreateCommandPool(g.dev, &pci, nullptr, &g.pool)) != VK_SUCCESS)
        die("vkCreateCommandPool", r);
    printf("[example] %s\n", props.deviceName);
    return g;
}

inline buf_t make_buf(const gpu_t& g, VkDeviceSize size) {
    buf_t b;
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = size < 4 ? 4 : (size + 3) / 4 * 4;
    bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkResult r = vkCreateBuffer(g.dev, &bci, nullptr, &b.buf);
    if (r != VK_SUCCESS)
        die("vkCreateBuffer", r);
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(g.dev, b.buf, &req);
    constexpr VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < g.mem.memoryTypeCount && type == UINT32_MAX; i++)
        if (((req.memoryTypeBits >> i) & 1) && (g.mem.memoryTypes[i].propertyFlags & want) == want)
            type = i;
    if (type == UINT32_MAX)
        die("no host visible memory type");
    VkMemoryAllocateFlagsInfo mfi = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    mfi.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &mfi};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = type;
    if ((r = vkAllocateMemory(g.dev, &mai, nullptr, &b.mem)) != VK_SUCCESS)
        die("vkAllocateMemory", r);
    if ((r = vkBindBufferMemory(g.dev, b.buf, b.mem, 0)) != VK_SUCCESS)
        die("vkBindBufferMemory", r);
    if ((r = vkMapMemory(g.dev, b.mem, 0, VK_WHOLE_SIZE, 0, &b.map)) != VK_SUCCESS)
        die("vkMapMemory", r);
    VkBufferDeviceAddressInfo bai = {VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    bai.buffer = b.buf;
    b.addr = vkGetBufferDeviceAddress(g.dev, &bai);
    return b;
}

inline void free_buf(const gpu_t& g, buf_t& b) {
    vkDestroyBuffer(g.dev, b.buf, nullptr);
    vkFreeMemory(g.dev, b.mem, nullptr);
    b = buf_t{};
}

inline VkCommandBuffer begin(const gpu_t& g) {
    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = g.pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkCommandBuffer cmd;
    VkResult r = vkAllocateCommandBuffers(g.dev, &cai, &cmd);
    if (r != VK_SUCCESS)
        die("vkAllocateCommandBuffers", r);
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    VkMemoryBarrier host = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_HOST_WRITE_BIT,
                            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &host, 0, nullptr,
                         0, nullptr);
    return cmd;
}

inline void submit_wait(const gpu_t& g, VkCommandBuffer cmd) {
    VkMemoryBarrier done = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &done, 0, nullptr,
                         0, nullptr);
    vkEndCommandBuffer(cmd);
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    VkResult r = vkQueueSubmit(g.queue, 1, &si, VK_NULL_HANDLE);
    if (r != VK_SUCCESS)
        die("vkQueueSubmit", r);
    vkQueueWaitIdle(g.queue);
}

inline vkdeflate::ctx_t* attach(const gpu_t& g) {
    std::string err;
    vkdeflate::ctx_t* ctx = vkdeflate::attach(g.instance, g.phys, g.dev, vkGetInstanceProcAddr, {}, err);
    if (!ctx)
        die(("vkdeflate::attach: " + err).c_str());
    return ctx;
}

inline bool same_as_cpu(const std::vector<uint8_t>& stream, const uint8_t* gpu_out, size_t size) {
    std::vector<uint8_t> ref(size);
    std::string err;
    if (!gdeflate::decompress(stream.data(), stream.size(), ref.data(), size, err)) {
        printf("[example] cpu decoder: %s\n", err.c_str());
        return false;
    }
    return memcmp(ref.data(), gpu_out, size) == 0;
}

inline void destroy(gpu_t& g) {
    vkDestroyCommandPool(g.dev, g.pool, nullptr);
    vkDestroyDevice(g.dev, nullptr);
    vkDestroyInstance(g.instance, nullptr);
}

}
