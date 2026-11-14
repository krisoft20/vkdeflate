// bench - decodes GDeflate streams with vkdeflate and, when the Khronos memory decompression layer
// (VK_LAYER_KHRONOS_memory_decompression, NVIDIA's GDeflate shaders behind VK_NV_memory_decompression)
// can be loaded, with that layer too: the same device-local buffers, GPU timestamps around each
// decoder, both outputs checked against the CPU decoder. Writes one csv row per stream.
//
//   bench <file.gdz>... | -list <file> [-rounds N] [-warm N] [-ms N] [-csv out.csv | -csvdir folder] [-nolayer] [-direct] [-regions]
//
// Every stream gets -warm rounds (10), then at least -rounds (30) and at least -ms milliseconds (500) of
// measured rounds; the best one counts.
//
// The layer is found through VK_LAYER_PATH (the folder with VkLayer_khronos_memory_decompression.json).
// -direct uses vkCmdDecompressMemoryNV (one dispatch per tile inside the layer) instead of the indirect
// count path (one dispatch for all tiles). -layer names another layer with the same extension. -csvdir names the file after the GPU. Exit code 0 only when
// every output matches the CPU decoder.
//
// -load also times the path a game takes, file to device-local buffer, best of -load-rounds (10): load() from a
// file just dropped from the OS cache, from a cached file and from bytes in memory, the file read without and
// with the cache, record_load() on the host for the same three, the GPU time of what it records, and next to it
// the CPU decoder on every thread plus the upload of its output.
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
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>
#endif

#include "gdeflate.h"
#include "vkdeflate.h"

namespace {

#define VK_FNS(X)                                 \
    X(vkCreateInstance)                           \
    X(vkDestroyInstance)                          \
    X(vkEnumeratePhysicalDevices)                 \
    X(vkGetPhysicalDeviceProperties)              \
    X(vkGetPhysicalDeviceProperties2)             \
    X(vkGetPhysicalDeviceFeatures)                \
    X(vkGetPhysicalDeviceFeatures2)               \
    X(vkGetPhysicalDeviceQueueFamilyProperties)   \
    X(vkGetPhysicalDeviceMemoryProperties)        \
    X(vkEnumerateDeviceExtensionProperties)       \
    X(vkCreateDevice)                             \
    X(vkDestroyDevice)                            \
    X(vkGetDeviceProcAddr)                        \
    X(vkGetDeviceQueue)                           \
    X(vkQueueWaitIdle)                            \
    X(vkCreateBuffer)                             \
    X(vkDestroyBuffer)                            \
    X(vkGetBufferMemoryRequirements)              \
    X(vkGetBufferDeviceAddress)                   \
    X(vkAllocateMemory)                           \
    X(vkFreeMemory)                               \
    X(vkBindBufferMemory)                         \
    X(vkMapMemory)                                \
    X(vkCreateCommandPool)                        \
    X(vkDestroyCommandPool)                       \
    X(vkResetCommandPool)                         \
    X(vkAllocateCommandBuffers)                   \
    X(vkBeginCommandBuffer)                       \
    X(vkEndCommandBuffer)                         \
    X(vkCmdPipelineBarrier)                       \
    X(vkCmdCopyBuffer)                            \
    X(vkCreateQueryPool)                          \
    X(vkDestroyQueryPool)                         \
    X(vkCmdResetQueryPool)                        \
    X(vkCmdWriteTimestamp)                        \
    X(vkGetQueryPoolResults)                      \
    X(vkQueueSubmit)

#define VK_DECLARE(fn) PFN_##fn fn = nullptr;
PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;
VK_FNS(VK_DECLARE)
PFN_vkCmdDecompressMemoryNV vkCmdDecompressMemoryNV = nullptr;
PFN_vkCmdDecompressMemoryIndirectCountNV vkCmdDecompressMemoryIndirectCountNV = nullptr;

const char* layer_name = "VK_LAYER_KHRONOS_memory_decompression";
constexpr const char* ext_name = "VK_NV_memory_decompression";
constexpr const char* stats_name = "VK_KHR_pipeline_executable_properties";
constexpr const char* subgroup_name = "VK_EXT_subgroup_size_control";

VkInstance instance = VK_NULL_HANDLE;
VkPhysicalDevice phys = VK_NULL_HANDLE;
VkDevice dev = VK_NULL_HANDLE;
VkQueue queue = VK_NULL_HANDLE;
VkCommandPool pool = VK_NULL_HANDLE;
VkCommandBuffer cmd = VK_NULL_HANDLE;
VkQueryPool queries = VK_NULL_HANDLE;
uint32_t family = 0;
double tick_ms = 1e-6;
bool has_layer = false;
std::string gpu_name;
std::string driver_text;
std::string device_text;
bool has_stats = false;
VkPhysicalDeviceMemoryProperties memprops = {};

struct buf_t {
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkDeviceAddress addr = 0;
    void* map = nullptr;
};

struct args_t {
    vkdeflate::config_t cfg;
    uint32_t tile_count = 0;
    std::vector<std::string> files;
    std::string csv;
    std::string csvdir;
    int rounds = 30;
    int warm = 10;
    double min_ms = 500.0;
    bool direct = false;
    bool layer = true;
    bool regions = false;
    bool load = false;
    int load_rounds = 10;
    double idle_ms = 0;
};

struct load_row_t {
    double read_ms = 1e30;
    double cold_ms = 1e30;
    double host_cold_ms = 1e30;
    double host_ms = 1e30;
    double host_mem_ms = 1e30;
    double gpu_ms = 1e30;
    double gpu_up_ms = 1e30;
    double gpu_copy_ms = 1e30;
    double load_cold_ms = 1e30;
    double load_ms = 1e30;
    double load_mem_ms = 1e30;
    double cpu_mt_ms = 1e30;
    double upload_ms = 1e30;
    int threads = 0;
    bool ok = false;
};

struct row_t {
    std::string stream;
    uint32_t tiles = 0;
    size_t out = 0;
    double cpu_ms = 0;
    double vk_ms = 0;
    double khr_ms = 0;
    bool vk_ok = false;
    bool khr_ok = false;
    load_row_t ld;
};

using steady_t = std::chrono::steady_clock;

static double ms_since(steady_t::time_point t0) {
    return std::chrono::duration<double, std::milli>(steady_t::now() - t0).count();
}

static void die(const char* what, VkResult r = VK_SUCCESS) {
    if (r != VK_SUCCESS)
        printf("[bench] %s failed: %d\n", what, (int)r);
    else
        printf("[bench] %s\n", what);
    exit(2);
}

static bool load(const std::string& path, std::vector<uint8_t>& d) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f)
        return false;
    d.resize((size_t)f.tellg());
    f.seekg(0);
    f.read((char*)d.data(), (std::streamsize)d.size());
    return true;
}

static void make_device(bool want_layer) {
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
    PFN_vkEnumerateInstanceLayerProperties layers_fn =
        (PFN_vkEnumerateInstanceLayerProperties)vkGetInstanceProcAddr(nullptr, "vkEnumerateInstanceLayerProperties");
    uint32_t nlayers = 0;
    layers_fn(&nlayers, nullptr);
    std::vector<VkLayerProperties> layers(nlayers);
    layers_fn(&nlayers, layers.data());
    for (const VkLayerProperties& l : layers)
        has_layer = has_layer || (want_layer && strcmp(l.layerName, layer_name) == 0);

    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "vkdeflate bench";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    if (has_layer) {
        ici.enabledLayerCount = 1;
        ici.ppEnabledLayerNames = &layer_name;
    }
    VkResult r = vkCreateInstance(&ici, nullptr, &instance);
    if (r != VK_SUCCESS && has_layer) {
        printf("[bench] the khronos layer is listed but does not load (%d), going on without it\n", (int)r);
        has_layer = false;
        ici.enabledLayerCount = 0;
        r = vkCreateInstance(&ici, nullptr, &instance);
    }
    if (r != VK_SUCCESS)
        die("vkCreateInstance", r);
#define VK_LOAD(fn) fn = (PFN_##fn)vkGetInstanceProcAddr(instance, #fn);
    VK_FNS(VK_LOAD)

    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance, &count, nullptr);
    if (!count)
        die("no vulkan device");
    std::vector<VkPhysicalDevice> devs(count);
    vkEnumeratePhysicalDevices(instance, &count, devs.data());
    phys = devs[0];
    VkPhysicalDeviceProperties props;
    for (VkPhysicalDevice d : devs) {
        vkGetPhysicalDeviceProperties(d, &props);
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            phys = d;
            break;
        }
    }
    VkPhysicalDeviceDriverProperties drv = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceProperties2 props2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &drv};
    vkGetPhysicalDeviceProperties2(phys, &props2);
    props = props2.properties;
    vkGetPhysicalDeviceMemoryProperties(phys, &memprops);
    tick_ms = (double)props.limits.timestampPeriod * 1e-6;
    gpu_name = props.deviceName;
    driver_text = std::string(drv.driverName) + " " + drv.driverInfo;

    uint32_t next = 0;
    vkEnumerateDeviceExtensionProperties(phys, nullptr, &next, nullptr);
    std::vector<VkExtensionProperties> exts(next);
    vkEnumerateDeviceExtensionProperties(phys, nullptr, &next, exts.data());
    auto has_ext = [&](const char* name) {
        for (const VkExtensionProperties& e : exts)
            if (!strcmp(e.extensionName, name))
                return true;
        return false;
    };
    has_layer = has_layer && has_ext(ext_name);
    has_stats = has_ext(stats_name);
    bool core13 = props.apiVersion >= VK_API_VERSION_1_3;
    bool size_ext = !core13 && has_ext(subgroup_name);

    char api[32];
    snprintf(api, sizeof(api), "%u.%u.%u", VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion),
             VK_API_VERSION_PATCH(props.apiVersion));
    device_text = "gpu: " + gpu_name + "\ndriver: " + driver_text + "\nvulkan: " + api + "\n";
    if (has_ext("VK_AMD_shader_core_properties")) {
        VkPhysicalDeviceShaderCorePropertiesAMD core = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_CORE_PROPERTIES_AMD};
        VkPhysicalDeviceProperties2 p2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &core};
        vkGetPhysicalDeviceProperties2(phys, &p2);
        uint32_t cus = core.shaderEngineCount * core.shaderArraysPerEngineCount * core.computeUnitsPerShaderArray;
        device_text += "compute units: " + std::to_string(cus) + ", simds per unit " +
                       std::to_string(core.simdPerComputeUnit) + ", waves per simd " +
                       std::to_string(core.wavefrontsPerSimd) + ", wave size " + std::to_string(core.wavefrontSize) + "\n";
        device_text += "vector registers: " + std::to_string(core.vgprsPerSimd) + " per simd, " +
                       std::to_string(core.minVgprAllocation) + " to " + std::to_string(core.maxVgprAllocation) +
                       " per wave in steps of " + std::to_string(core.vgprAllocationGranularity) + "\n";
    } else if (has_ext("VK_NV_shader_sm_builtins")) {
        VkPhysicalDeviceShaderSMBuiltinsPropertiesNV sm = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_SM_BUILTINS_PROPERTIES_NV};
        VkPhysicalDeviceProperties2 p2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &sm};
        vkGetPhysicalDeviceProperties2(phys, &p2);
        device_text += "streaming multiprocessors: " + std::to_string(sm.shaderSMCount) + "\n";
    }
    printf("[bench] %s, %s, khronos layer %s\n", gpu_name.c_str(), driver_text.c_str(),
           has_layer ? "loaded" : want_layer ? "not found (set VK_LAYER_PATH to compare)" : "off");

    uint32_t nfam = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &nfam, nullptr);
    std::vector<VkQueueFamilyProperties> fams(nfam);
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &nfam, fams.data());
    family = UINT32_MAX;
    for (uint32_t i = 0; i < nfam; i++)
        if (fams[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
            family = i;
            break;
        }
    if (family == UINT32_MAX)
        die("no compute queue");

    VkPhysicalDeviceFeatures have = {};
    vkGetPhysicalDeviceFeatures(phys, &have);
    VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR fstats = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_EXECUTABLE_PROPERTIES_FEATURES_KHR};
    fstats.pipelineExecutableInfo = VK_TRUE;
    VkPhysicalDeviceSubgroupSizeControlFeatures fsc = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES,
                                                       has_stats ? &fstats : nullptr};
    VkPhysicalDeviceFeatures2 query = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
                                       core13 || size_ext ? (void*)&fsc : nullptr};
    vkGetPhysicalDeviceFeatures2(phys, &query);
    bool control = (core13 || size_ext) && fsc.subgroupSizeControl && fsc.computeFullSubgroups;
    fsc.pNext = has_stats ? &fstats : nullptr;
    VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                                            control ? (void*)&fsc : has_stats ? (void*)&fstats : nullptr};
    f12.storageBuffer8BitAccess = VK_TRUE;
    f12.shaderInt8 = VK_TRUE;
    f12.bufferDeviceAddress = VK_TRUE;
    f12.vulkanMemoryModel = VK_TRUE;
    VkPhysicalDeviceFeatures f10 = {};
    f10.shaderInt64 = VK_TRUE;
    f10.shaderInt16 = have.shaderInt16;
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f12};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.pEnabledFeatures = &f10;
    const char* names[3];
    uint32_t nnames = 0;
    if (has_layer)
        names[nnames++] = ext_name;
    if (has_stats)
        names[nnames++] = stats_name;
    if (control && size_ext)
        names[nnames++] = subgroup_name;
    dci.enabledExtensionCount = nnames;
    dci.ppEnabledExtensionNames = names;
    if ((r = vkCreateDevice(phys, &dci, nullptr, &dev)) != VK_SUCCESS)
        die("vkCreateDevice", r);
    vkGetDeviceQueue(dev, family, 0, &queue);
    if (has_layer) {
        vkCmdDecompressMemoryNV = (PFN_vkCmdDecompressMemoryNV)vkGetDeviceProcAddr(dev, "vkCmdDecompressMemoryNV");
        vkCmdDecompressMemoryIndirectCountNV =
            (PFN_vkCmdDecompressMemoryIndirectCountNV)vkGetDeviceProcAddr(dev, "vkCmdDecompressMemoryIndirectCountNV");
        if (!vkCmdDecompressMemoryNV || !vkCmdDecompressMemoryIndirectCountNV)
            die("vkCmdDecompressMemory*NV not provided by the layer");
    }

    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.queueFamilyIndex = family;
    if ((r = vkCreateCommandPool(dev, &pci, nullptr, &pool)) != VK_SUCCESS)
        die("vkCreateCommandPool", r);
    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    if ((r = vkAllocateCommandBuffers(dev, &cai, &cmd)) != VK_SUCCESS)
        die("vkAllocateCommandBuffers", r);
    VkQueryPoolCreateInfo qpi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    qpi.queryType = VK_QUERY_TYPE_TIMESTAMP;
    qpi.queryCount = 4;
    if ((r = vkCreateQueryPool(dev, &qpi, nullptr, &queries)) != VK_SUCCESS)
        die("vkCreateQueryPool", r);
}

static buf_t make_buf(VkDeviceSize size, bool host) {
    buf_t b;
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = size < 4 ? 4 : size;
    bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkResult r = vkCreateBuffer(dev, &bci, nullptr, &b.buf);
    if (r != VK_SUCCESS)
        die("vkCreateBuffer", r);
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(dev, b.buf, &req);
    constexpr VkMemoryPropertyFlags host_bits = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < memprops.memoryTypeCount && type == UINT32_MAX; i++) {
        if (!((req.memoryTypeBits >> i) & 1))
            continue;
        VkMemoryPropertyFlags f = memprops.memoryTypes[i].propertyFlags;
        if (host && (f & host_bits) == host_bits)
            type = i;
        if (!host && (f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) && !(f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
            type = i;
    }
    for (uint32_t i = 0; !host && i < memprops.memoryTypeCount && type == UINT32_MAX; i++)
        if (((req.memoryTypeBits >> i) & 1) && (memprops.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
            type = i;
    if (type == UINT32_MAX)
        die(host ? "no host visible memory type" : "no device local memory type");
    VkMemoryAllocateFlagsInfo mfi = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    mfi.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &mfi};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = type;
    if ((r = vkAllocateMemory(dev, &mai, nullptr, &b.mem)) != VK_SUCCESS)
        die("vkAllocateMemory", r);
    if ((r = vkBindBufferMemory(dev, b.buf, b.mem, 0)) != VK_SUCCESS)
        die("vkBindBufferMemory", r);
    if (host && (r = vkMapMemory(dev, b.mem, 0, VK_WHOLE_SIZE, 0, &b.map)) != VK_SUCCESS)
        die("vkMapMemory", r);
    VkBufferDeviceAddressInfo bai = {VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    bai.buffer = b.buf;
    b.addr = vkGetBufferDeviceAddress(dev, &bai);
    return b;
}

static void free_buf(buf_t& b) {
    vkDestroyBuffer(dev, b.buf, nullptr);
    vkFreeMemory(dev, b.mem, nullptr);
}

static void begin() {
    vkResetCommandPool(dev, pool, 0);
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
}

static void fence_all() {
    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
                          VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_WRITE_BIT,
                          VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT |
                              VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0,
                         nullptr, 0, nullptr);
}

static void submit_wait() {
    vkEndCommandBuffer(cmd);
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    VkResult r = vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
    if (r != VK_SUCCESS)
        die("vkQueueSubmit", r);
    if ((r = vkQueueWaitIdle(queue)) != VK_SUCCESS)
        die("vkQueueWaitIdle", r);
}

static void copy(const buf_t& from, const buf_t& to, VkDeviceSize size) {
    begin();
    VkBufferCopy c = {0, 0, size};
    vkCmdCopyBuffer(cmd, from.buf, to.buf, 1, &c);
    fence_all();
    submit_wait();
}

static void run_layer(bool direct, const std::vector<VkDecompressMemoryRegionNV>& regs, const buf_t& regions,
                      const buf_t& count, uint32_t q) {
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queries, q);
    if (direct)
        vkCmdDecompressMemoryNV(cmd, (uint32_t)regs.size(), regs.data());
    else
        vkCmdDecompressMemoryIndirectCountNV(cmd, regions.addr, count.addr, sizeof(VkDecompressMemoryRegionNV));
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries, q + 1);
    fence_all();
}

static void run_ours(vkdeflate::ctx_t* ctx, const vkdeflate::job_t& job, const std::vector<vkdeflate::region_t>& regs,
                     uint32_t q) {
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queries, q);
    if (regs.empty())
        vkdeflate::record(ctx, cmd, job);
    else
        vkdeflate::cmd_decompress_memory(ctx, cmd, regs.data(), (uint32_t)regs.size());
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries, q + 1);
    fence_all();
}

static bool same(const buf_t& down, const std::vector<uint8_t>& ref) {
    return memcmp(down.map, ref.data(), ref.size()) == 0;
}

static bool run_file(const args_t& a, vkdeflate::ctx_t* ctx, const std::string& path, row_t& row) {
    std::vector<uint8_t> in;
    if (!load(path, in)) {
        printf("[bench] cant open %s\n", path.c_str());
        return false;
    }
    if (a.tile_count != 0) {
        gdeflate::stream_t stream;
        std::string err;
        if (!gdeflate::parse(in.data(), in.size(), stream, err)) {
            printf("[bench] %s: %s\n", path.c_str(), err.c_str());
            return false;
        }
        if (stream.tiles.empty() || stream.tiles[0].size != 65536) {
            printf("[bench] -tile-count needs a full first tile\n");
            return false;
        }
        const gdeflate::tile_t& tile = stream.tiles[0];
        size_t head = 8 + 4 * (size_t)a.tile_count;
        if (tile.csize * (uint64_t)a.tile_count + head > UINT32_MAX) {
            printf("[bench] repeated stream exceeds 4 GB\n");
            return false;
        }
        std::vector<uint8_t> repeated(head + tile.csize * a.tile_count);
        repeated[0] = 4;
        repeated[1] = 0xfb;
        repeated[2] = (uint8_t)a.tile_count;
        repeated[3] = (uint8_t)(a.tile_count >> 8);
        repeated[4] = 1;
        for (uint32_t i = 0; i < a.tile_count; i++) {
            uint32_t entry = (uint32_t)(i ? i * tile.csize : tile.csize);
            for (uint32_t b = 0; b < 4; b++)
                repeated[8 + 4 * i + b] = (uint8_t)(entry >> (8 * b));
            memcpy(repeated.data() + head + i * tile.csize, in.data() + tile.src, tile.csize);
        }
        in.swap(repeated);
    }
    std::vector<vkdeflate::tile_desc_t> descs;
    size_t out_size = 0;
    std::string err;
    if (!vkdeflate::describe(in.data(), in.size(), in.size(), descs, out_size, err)) {
        printf("[bench] %s: %s\n", path.c_str(), err.c_str());
        return false;
    }
    std::vector<uint8_t> ref(out_size);
    auto t0 = std::chrono::steady_clock::now();
    if (!gdeflate::decompress(in.data(), in.size(), ref.data(), out_size, err)) {
        printf("[bench] %s: cpu decoder: %s\n", path.c_str(), err.c_str());
        return false;
    }
    row.cpu_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    uint32_t n = (uint32_t)descs.size();
    row.stream = std::filesystem::path(path).stem().string();
    if (a.tile_count != 0)
        row.stream += ".tiles" + std::to_string(a.tile_count);
    row.tiles = n;
    row.out = out_size;

    VkDeviceSize in_size = (in.size() + 3) / 4 * 4;
    buf_t up = make_buf(in_size, true);
    buf_t src = make_buf(in_size, false);
    buf_t tiles = make_buf(n * sizeof(vkdeflate::tile_desc_t), true);
    buf_t status = make_buf(n * 4, true);
    buf_t regions = make_buf(n * sizeof(VkDecompressMemoryRegionNV), true);
    buf_t count = make_buf(4, true);
    buf_t dst_khr = make_buf(has_layer ? out_size : 4, false);
    buf_t dst_vk = make_buf(out_size, false);
    buf_t down = make_buf(out_size, true);
    memcpy(up.map, in.data(), in.size());
    memcpy(tiles.map, descs.data(), n * sizeof(vkdeflate::tile_desc_t));
    memset(status.map, 0xff, n * 4);
    std::vector<vkdeflate::tile_desc_t> order = descs;
    std::sort(order.begin(), order.end(),
              [](const vkdeflate::tile_desc_t& a, const vkdeflate::tile_desc_t& b) { return a.dst < b.dst; });
    std::vector<VkDecompressMemoryRegionNV> regs(n);
    for (uint32_t i = 0; i < n; i++) {
        regs[i].srcAddress = src.addr + 4ull * order[i].src;
        regs[i].dstAddress = dst_khr.addr + order[i].dst;
        regs[i].compressedSize = 4ull * order[i].words;
        regs[i].decompressedSize = order[i].size;
        regs[i].decompressionMethod = VK_MEMORY_DECOMPRESSION_METHOD_GDEFLATE_1_0_BIT_NV;
    }
    memcpy(regions.map, regs.data(), n * sizeof(VkDecompressMemoryRegionNV));
    memcpy(count.map, &n, 4);
    copy(up, src, in_size);
    vkdeflate::job_t job = {src.addr, tiles.addr, dst_vk.addr, status.addr, n, vkdeflate::job_flags(descs, out_size)};
    std::vector<vkdeflate::region_t> ours;
    if (a.regions) {
        for (const vkdeflate::tile_desc_t& t : descs)
            ours.push_back({src.addr + 4ull * t.src, dst_vk.addr + t.dst, 4ull * t.words, t.size});
        memset(status.map, 0, n * 4);
    }

    double best_khr = 1e30, best_vk = 1e30;
    auto measured = std::chrono::steady_clock::now();
    auto spent = [&] { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - measured).count(); };
    for (int round = -a.warm; round < a.rounds || spent() < a.min_ms; round++) {
        if (round == 0)
            measured = std::chrono::steady_clock::now();
        bool khr_first = (round & 1) == 0;
        if (a.idle_ms > 0)
            std::this_thread::sleep_for(std::chrono::duration<double, std::milli>(a.idle_ms));
        begin();
        vkCmdResetQueryPool(cmd, queries, 0, 4);
        fence_all();
        if (has_layer && khr_first)
            run_layer(a.direct, regs, regions, count, 0);
        run_ours(ctx, job, ours, 2);
        if (has_layer && !khr_first)
            run_layer(a.direct, regs, regions, count, 0);
        submit_wait();
        uint64_t ts[4] = {};
        uint32_t first = has_layer ? 0 : 2;
        vkGetQueryPoolResults(dev, queries, first, 4 - first, (4 - first) * 8, ts + first, 8,
                              VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
        double khr_ms = has_layer ? (double)(ts[1] - ts[0]) * tick_ms : 0.0;
        double vk_ms = (double)(ts[3] - ts[2]) * tick_ms;
        if (round >= 0 && has_layer && khr_ms < best_khr)
            best_khr = khr_ms;
        if (round >= 0 && vk_ms < best_vk)
            best_vk = vk_ms;
    }
    row.vk_ms = best_vk;
    row.khr_ms = has_layer ? best_khr : 0.0;

    row.vk_ok = true;
    const uint32_t* st = (const uint32_t*)status.map;
    for (uint32_t i = 0; i < n; i++)
        if (st[i]) {
            printf("[bench] %s: vkdeflate tile %u: %s (%u)\n", row.stream.c_str(), i, vkdeflate::tile_error(st[i]), st[i]);
            row.vk_ok = false;
            break;
        }
    copy(dst_vk, down, out_size);
    row.vk_ok = row.vk_ok && same(down, ref);
    row.khr_ok = !has_layer;
    if (has_layer) {
        copy(dst_khr, down, out_size);
        row.khr_ok = same(down, ref);
    }

    double mb = (double)out_size / 1e6;
    if (has_layer)
        printf("[bench] %-24s %5u tiles %7.1f MB  cpu %8.1f ms  vkdeflate %6.2f ms %6.1f GB/s %s  khronos %6.2f ms %6.1f GB/s %s  x%.2f\n",
               row.stream.c_str(), n, mb, row.cpu_ms, row.vk_ms, mb / row.vk_ms, row.vk_ok ? "ok" : "FAIL", row.khr_ms,
               mb / row.khr_ms, row.khr_ok ? "ok" : "FAIL", row.khr_ms / row.vk_ms);
    else
        printf("[bench] %-24s %5u tiles %7.1f MB  cpu %8.1f ms  vkdeflate %6.2f ms %6.1f GB/s %s\n", row.stream.c_str(), n,
               mb, row.cpu_ms, row.vk_ms, mb / row.vk_ms, row.vk_ok ? "ok" : "FAIL");

    free_buf(up);
    free_buf(src);
    free_buf(tiles);
    free_buf(status);
    free_buf(regions);
    free_buf(count);
    free_buf(dst_khr);
    free_buf(dst_vk);
    free_buf(down);
    return row.vk_ok && row.khr_ok;
}

static double cold_read_ms(const std::string& path) {
#if defined(_WIN32)
    HANDLE h = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_FLAG_NO_BUFFERING | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return -1;
    constexpr DWORD chunk = 4u << 20;
    void* buf = VirtualAlloc(nullptr, chunk, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    steady_t::time_point t0 = steady_t::now();
    DWORD got = 0;
    while (ReadFile(h, buf, chunk, &got, nullptr) && got == chunk) {
    }
    double ms = ms_since(t0);
    VirtualFree(buf, 0, MEM_RELEASE);
    CloseHandle(h);
    return ms;
#else
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0)
        return -1;
    fdatasync(fd);
    posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
    std::vector<uint8_t> buf(4u << 20);
    steady_t::time_point t0 = steady_t::now();
    while (read(fd, buf.data(), buf.size()) > 0) {
    }
    double ms = ms_since(t0);
    posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
    close(fd);
    return ms;
#endif
}

static bool read_file(const std::string& path, std::vector<uint8_t>& bytes) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f)
        return false;
    fseek(f, 0, SEEK_END);
    long end = ftell(f);
    fseek(f, 0, SEEK_SET);
    bytes.resize(end > 0 ? (size_t)end : 0);
    size_t got = bytes.empty() ? 0 : fread(bytes.data(), 1, bytes.size(), f);
    fclose(f);
    return got == bytes.size();
}

static bool cpu_decode_mt(const std::vector<uint8_t>& in, const gdeflate::stream_t& s, uint8_t* out, int threads) {
    std::atomic<size_t> next(0);
    std::atomic<bool> failed(false);
    std::vector<std::thread> workers;
    for (int w = 0; w < threads; w++)
        workers.emplace_back([&] {
            std::string why;
            for (size_t i = next++; i < s.tiles.size() && !failed; i = next++) {
                const gdeflate::tile_t& t = s.tiles[i];
                if (!gdeflate::decode_tile(in.data() + t.src, t.csize, out + t.dst, t.size, why))
                    failed = true;
            }
        });
    for (std::thread& t : workers)
        t.join();
    return !failed;
}

static double upload_ms(const std::vector<uint8_t>& bytes, const buf_t& up, const buf_t& dst) {
    steady_t::time_point t0 = steady_t::now();
    memcpy(up.map, bytes.data(), bytes.size());
    copy(up, dst, bytes.size());
    return ms_since(t0);
}

static bool timed_record(vkdeflate::ctx_t* ctx, const std::vector<vkdeflate::load_t>& items, double& host_ms, double& gpu_ms,
                         std::string& err) {
    begin();
    vkCmdResetQueryPool(cmd, queries, 0, 2);
    fence_all();
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queries, 0);
    steady_t::time_point t0 = steady_t::now();
    bool ok = vkdeflate::record_load(ctx, cmd, items.data(), (uint32_t)items.size(), err);
    host_ms = ms_since(t0);
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries, 1);
    fence_all();
    submit_wait();
    uint64_t ts[2] = {};
    vkGetQueryPoolResults(dev, queries, 0, 2, sizeof(ts), ts, 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
    gpu_ms = (double)(ts[1] - ts[0]) * tick_ms;
    return ok && vkdeflate::load_result(ctx, err);
}

static double gpu_copy_ms(const buf_t& from, const buf_t& to, VkDeviceSize size) {
    begin();
    vkCmdResetQueryPool(cmd, queries, 0, 2);
    fence_all();
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queries, 0);
    VkBufferCopy c = {0, 0, size};
    vkCmdCopyBuffer(cmd, from.buf, to.buf, 1, &c);
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries, 1);
    fence_all();
    submit_wait();
    uint64_t ts[2] = {};
    vkGetQueryPoolResults(dev, queries, 0, 2, sizeof(ts), ts, 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
    return (double)(ts[1] - ts[0]) * tick_ms;
}

static void keep_best(double& best, double v) {
    best = std::min(best, v);
}

static bool timed_load(vkdeflate::ctx_t* ctx, const std::vector<vkdeflate::load_t>& items, double& ms, std::string& err) {
    steady_t::time_point t0 = steady_t::now();
    bool ok = vkdeflate::load(ctx, queue, family, items.data(), (uint32_t)items.size(), err);
    ms = ms_since(t0);
    return ok;
}

static bool loads_right(vkdeflate::ctx_t* ctx, const std::vector<vkdeflate::load_t>& items, const buf_t& dst, const buf_t& down,
                        const std::vector<uint8_t>& ref, std::string& err) {
    memset(down.map, 0, ref.size());
    copy(down, dst, ref.size());
    vkdeflate::trim(ctx);
    if (!vkdeflate::load(ctx, queue, family, items.data(), (uint32_t)items.size(), err))
        return false;
    copy(dst, down, ref.size());
    return same(down, ref);
}

struct piece_t {
    std::string path;
    std::vector<uint8_t> in;
    gdeflate::stream_t s;
    size_t at = 0;
};

static bool run_load(const args_t& a, vkdeflate::ctx_t* ctx, const std::vector<std::string>& paths, row_t& row) {
    std::vector<piece_t> pieces(paths.size());
    std::string err;
    size_t total = 0, in_total = 0;
    for (size_t i = 0; i < paths.size(); i++) {
        piece_t& p = pieces[i];
        p.path = paths[i];
        if (!read_file(p.path, p.in) || !gdeflate::parse(p.in.data(), p.in.size(), p.s, err)) {
            printf("[bench] %s: cant parse for -load %s\n", p.path.c_str(), err.c_str());
            return false;
        }
        p.at = total;
        total += (p.s.total + 255) / 256 * 256;
        in_total += p.in.size();
    }
    std::vector<uint8_t> ref(total), mt(total);
    for (piece_t& p : pieces)
        if (!gdeflate::decompress(p.in.data(), p.in.size(), ref.data() + p.at, p.s.total, err)) {
            printf("[bench] %s: cpu decoder: %s\n", p.path.c_str(), err.c_str());
            return false;
        }
    load_row_t& r = row.ld;
    r.threads = (int)std::max(1u, std::thread::hardware_concurrency());
    buf_t up = make_buf(total, true);
    buf_t dst = make_buf(total, false);
    buf_t down = make_buf(total, true);
    buf_t other = make_buf(total, false);
    std::vector<vkdeflate::load_t> from_file(pieces.size()), from_mem(pieces.size());
    for (size_t i = 0; i < pieces.size(); i++) {
        from_file[i].path = pieces[i].path.c_str();
        from_file[i].buffer = dst.buf;
        from_file[i].buffer_offset = pieces[i].at;
        from_mem[i].data = pieces[i].in.data();
        from_mem[i].size = pieces[i].in.size();
        from_mem[i].buffer = dst.buf;
        from_mem[i].buffer_offset = pieces[i].at;
    }
    auto drop_all = [&] {
        double ms = 0;
        for (const piece_t& p : pieces)
            ms += cold_read_ms(p.path);
        return ms;
    };

    bool ok = true;
    for (int round = -2; round < a.load_rounds && ok; round++) {
        steady_t::time_point t0 = steady_t::now();
        for (const piece_t& p : pieces)
            ok = ok && cpu_decode_mt(p.in, p.s, mt.data() + p.at, r.threads);
        double cpu = ms_since(t0);
        double upl = upload_ms(mt, up, dst);
        double gpu_up = gpu_copy_ms(up, dst, in_total);
        double gpu_out = gpu_copy_ms(dst, other, total);
        double cold = drop_all();
        double host_cold = 0, host = 0, host_mem = 0, gpu = 0, gpu_mem = 0, load_cold = 0, whole = 0, whole_mem = 0;
        ok = ok && timed_record(ctx, from_file, host_cold, gpu, err);
        drop_all();
        ok = ok && timed_load(ctx, from_file, load_cold, err);
        std::vector<uint8_t> bytes;
        t0 = steady_t::now();
        for (const piece_t& p : pieces)
            ok = ok && read_file(p.path, bytes);
        double read = ms_since(t0);
        ok = ok && timed_record(ctx, from_file, host, gpu_mem, err);
        ok = ok && timed_record(ctx, from_mem, host_mem, gpu_mem, err);
        ok = ok && timed_load(ctx, from_file, whole, err) && timed_load(ctx, from_mem, whole_mem, err);
        if (round < 0)
            continue;
        keep_best(r.cpu_mt_ms, cpu);
        keep_best(r.upload_ms, upl);
        keep_best(r.gpu_up_ms, gpu_up);
        keep_best(r.gpu_copy_ms, gpu_out);
        keep_best(r.cold_ms, cold);
        keep_best(r.host_cold_ms, host_cold);
        keep_best(r.load_cold_ms, load_cold);
        keep_best(r.read_ms, read);
        keep_best(r.host_ms, host);
        keep_best(r.host_mem_ms, host_mem);
        keep_best(r.gpu_ms, std::min(gpu, gpu_mem));
        keep_best(r.load_ms, whole);
        keep_best(r.load_mem_ms, whole_mem);
    }
    if (!ok)
        printf("[bench] %s: -load: %s\n", row.stream.c_str(), err.c_str());
    r.ok = ok && mt == ref && loads_right(ctx, from_file, dst, down, ref, err) &&
           loads_right(ctx, from_mem, dst, down, ref, err);

    double mb = (double)total / 1e6;
    printf("[bench] %-24s load cold %6.2f ms, warm %6.2f, from memory %6.2f (%.1f GB/s)  file uncached %6.2f, cached %6.2f"
           "  record_load host cold %6.2f, warm %6.2f, from memory %6.2f, gpu %6.2f  cpu %d threads %7.2f + upload %6.2f  %s\n",
           row.stream.c_str(), r.load_cold_ms, r.load_ms, r.load_mem_ms, mb / r.load_mem_ms, r.cold_ms, r.read_ms,
           r.host_cold_ms, r.host_ms, r.host_mem_ms, r.gpu_ms, r.threads, r.cpu_mt_ms, r.upload_ms, r.ok ? "ok" : "FAIL");

    free_buf(up);
    free_buf(dst);
    free_buf(down);
    free_buf(other);
    return r.ok;
}

static std::string file_name_for(const std::string& gpu) {
    std::string s;
    for (char c : gpu)
        s += (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ? c : '_';
    return s + ".csv";
}

static std::string field(std::string s) {
    for (char& c : s)
        if (c == ',')
            c = ';';
    return s;
}

static void write_load(FILE* f, const load_row_t& r) {
    fprintf(f, ",%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%d,%.3f,%.3f,%d", r.load_cold_ms, r.load_ms,
            r.load_mem_ms, r.cold_ms, r.read_ms, r.host_cold_ms, r.host_ms, r.host_mem_ms, r.gpu_ms, r.gpu_up_ms,
            r.gpu_copy_ms, r.threads, r.cpu_mt_ms, r.upload_ms, r.ok ? 1 : 0);
}

static bool write_csv(const std::string& path, const std::vector<row_t>& rows, bool with_load) {
    FILE* f = fopen(path.c_str(), "w");
    if (!f)
        return false;
    fprintf(f, "gpu,driver,stream,tiles,out_bytes,cpu1_ms,vkdeflate_ms,khronos_ms,vkdeflate_ok,khronos_ok%s\n",
            with_load ? ",load_cold_ms,load_ms,load_mem_ms,read_uncached_ms,read_cached_ms,record_host_cold_ms,"
                        "record_host_ms,record_host_mem_ms,record_gpu_ms,gpu_upload_ms,gpu_copy_ms,cpu_threads,cpu_mt_ms,"
                        "upload_ms,load_ok"
                      : "");
    for (const row_t& r : rows) {
        fprintf(f, "%s,%s,%s,%u,%zu,%.2f,%.3f,", field(gpu_name).c_str(), field(driver_text).c_str(), field(r.stream).c_str(), r.tiles, r.out,
                r.cpu_ms, r.vk_ms);
        if (has_layer)
            fprintf(f, "%.3f,%d,%d", r.khr_ms, r.vk_ok ? 1 : 0, r.khr_ok ? 1 : 0);
        else
            fprintf(f, ",%d,", r.vk_ok ? 1 : 0);
        if (with_load)
            write_load(f, r.ld);
        fprintf(f, "\n");
    }
    fclose(f);
    return true;
}

}

int main(int argc, char** argv) {
    args_t a;
    for (int i = 1; i < argc; i++) {
        std::string s = argv[i];
        bool more = i + 1 < argc;
        if (s == "-rounds" && more)
            a.rounds = atoi(argv[++i]) < 1 ? 1 : atoi(argv[i]);
        else if (s == "-warm" && more)
            a.warm = atoi(argv[++i]) < 0 ? 0 : atoi(argv[i]);
        else if (s == "-ms" && more)
            a.min_ms = atof(argv[++i]);
        else if (s == "-tile-count" && more) {
            int count = atoi(argv[++i]);
            if (count < 1 || count > 65535) {
                printf("[bench] tile count must be between 1 and 65535\n");
                return 1;
            }
            a.tile_count = (uint32_t)count;
        }
        else if (s == "-root" && more)
            a.cfg.lit_root = (uint32_t)atoi(argv[++i]);
        else if (s == "-droot" && more)
            a.cfg.dist_root = (uint32_t)atoi(argv[++i]);
        else if (s == "-ring" && more)
            a.cfg.ring_bytes = (uint32_t)atoi(argv[++i]);
        else if (s == "-pad" && more)
            a.cfg.pad_bytes = (uint32_t)atoi(argv[++i]);
        else if (s == "-x" && more)
            a.cfg.experiment = (uint32_t)atoi(argv[++i]);
        else if (s == "-csv" && more)
            a.csv = argv[++i];
        else if (s == "-csvdir" && more)
            a.csvdir = argv[++i];
        else if (s == "-list" && more) {
            std::ifstream f(argv[++i]);
            std::string line;
            while (std::getline(f, line)) {
                size_t end = line.find_first_of(" \t\r");
                if (!line.empty() && end != 0)
                    a.files.push_back(line.substr(0, end));
            }
        } else if (s == "-direct")
            a.direct = true;
        else if (s == "-nolayer")
            a.layer = false;
        else if (s == "-layer" && more)
            layer_name = argv[++i];
        else if (s == "-regions")
            a.regions = true;
        else if (s == "-v")
            a.cfg.verbose = true;
        else if (s == "-lits" && more)
            a.cfg.literal_rounds = atoi(argv[++i]);
        else if (s == "-nochecks")
            a.cfg.bounds_checks = false;
        else if (s == "-load")
            a.load = true;
        else if (s == "-read-threads" && more)
            a.cfg.read_threads = (uint32_t)atoi(argv[++i]);
        else if (s == "-chunk" && more)
            a.cfg.load_chunk_tiles = (uint32_t)atoi(argv[++i]);
        else if (s == "-idle-ms" && more)
            a.idle_ms = atof(argv[++i]);
        else if (s == "-load-rounds" && more)
            a.load_rounds = std::max(1, atoi(argv[++i]));
        else if (s[0] == '-') {
            printf("unknown argument %s\n", argv[i]);
            return 1;
        } else
            a.files.push_back(s);
    }
    if (a.files.empty()) {
        printf("usage: bench <file.gdz>... | -list <file> [-rounds N] [-warm N] [-ms N] [-csv out.csv | -csvdir folder] [-nolayer] [-direct] [-regions]\n"
               "       [-root N] [-droot N] [-ring N] [-pad N] [-x N] [-tile-count N] [-lits 0|1] [-nochecks] [-v] [-load] [-load-rounds N] [-chunk N] [-read-threads N] [-idle-ms N]\n");
        return 1;
    }

    make_device(a.layer);
    vkdeflate::config_t cfg = a.cfg;
    cfg.capture_stats = has_stats;
    std::string err;
    vkdeflate::ctx_t* ctx = vkdeflate::attach(instance, phys, dev, vkGetInstanceProcAddr, cfg, err);
    if (!ctx) {
        printf("[bench] attach: %s\n", err.c_str());
        return 2;
    }

    std::vector<row_t> rows;
    bool ok = true;
    for (const std::string& path : a.files) {
        row_t row;
        ok = run_file(a, ctx, path, row) && ok;
        if (a.load && a.tile_count == 0 && !row.stream.empty())
            ok = run_load(a, ctx, {path}, row) && ok;
        if (!row.stream.empty())
            rows.push_back(row);
    }
    if (a.load && a.tile_count == 0 && a.files.size() > 1) {
        row_t all;
        all.stream = "all";
        for (const row_t& r : rows) {
            all.tiles += r.tiles;
            all.out += r.out;
        }
        all.vk_ok = run_load(a, ctx, a.files, all);
        ok = all.vk_ok && ok;
        rows.push_back(all);
    }
    std::string csv = a.csv;
    if (csv.empty() && !a.csvdir.empty()) {
        std::filesystem::create_directories(a.csvdir);
        csv = (std::filesystem::path(a.csvdir) / file_name_for(gpu_name)).string();
    }
    if (!csv.empty()) {
        if (write_csv(csv, rows, a.load && a.tile_count == 0))
            printf("[bench] %zu rows written to %s\n", rows.size(), csv.c_str());
        else
            printf("[bench] cant write %s\n", csv.c_str());
        std::string txt = std::filesystem::path(csv).replace_extension(".txt").string();
        if (FILE* f = fopen(txt.c_str(), "w")) {
            fprintf(f, "%s%s", device_text.c_str(), vkdeflate::report(ctx));
            fclose(f);
            printf("[bench] device and compiler report written to %s\n", txt.c_str());
        }
    }

    vkdeflate::destroy(ctx);
    vkDestroyQueryPool(dev, queries, nullptr);
    vkDestroyCommandPool(dev, pool, nullptr);
    vkDestroyDevice(dev, nullptr);
    vkDestroyInstance(instance, nullptr);
    return ok ? 0 : 1;
}
