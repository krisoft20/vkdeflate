// bench - decodes GDeflate streams with vkdeflate and, when the Khronos memory decompression layer
// (VK_LAYER_KHRONOS_memory_decompression, NVIDIA's GDeflate shaders behind VK_NV_memory_decompression)
// can be loaded, with that layer too: the same device-local buffers, GPU timestamps around each
// decoder, both outputs checked against the CPU decoder. Writes one csv row per stream.
//
//   bench <file.gdz>... | -list <file> [-rounds N] [-warm N] [-csv out.csv | -csvdir folder] [-nolayer] [-direct]
//
// The layer is found through VK_LAYER_PATH (the folder with VkLayer_khronos_memory_decompression.json).
// -direct uses vkCmdDecompressMemoryNV (one dispatch per tile inside the layer) instead of the indirect
// count path (one dispatch for all tiles). -csvdir names the file after the GPU. Exit code 0 only when
// every output matches the CPU decoder.
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
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
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

namespace {

#define VK_FNS(X)                                 \
    X(vkCreateInstance)                           \
    X(vkDestroyInstance)                          \
    X(vkEnumeratePhysicalDevices)                 \
    X(vkGetPhysicalDeviceProperties)              \
    X(vkGetPhysicalDeviceProperties2)             \
    X(vkGetPhysicalDeviceFeatures)                \
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

constexpr const char* layer_name = "VK_LAYER_KHRONOS_memory_decompression";
constexpr const char* ext_name = "VK_NV_memory_decompression";

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
VkPhysicalDeviceMemoryProperties memprops = {};

struct buf_t {
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkDeviceAddress addr = 0;
    void* map = nullptr;
};

struct args_t {
    std::vector<std::string> files;
    std::string csv;
    std::string csvdir;
    int rounds = 30;
    int warm = 10;
    bool direct = false;
    bool layer = true;
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
};

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

    if (has_layer) {
        uint32_t next = 0;
        vkEnumerateDeviceExtensionProperties(phys, nullptr, &next, nullptr);
        std::vector<VkExtensionProperties> exts(next);
        vkEnumerateDeviceExtensionProperties(phys, nullptr, &next, exts.data());
        bool found = false;
        for (const VkExtensionProperties& e : exts)
            found = found || strcmp(e.extensionName, ext_name) == 0;
        has_layer = found;
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
    if (has_layer) {
        dci.enabledExtensionCount = 1;
        dci.ppEnabledExtensionNames = &ext_name;
    }
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

static void run_ours(vkdeflate::ctx_t* ctx, const vkdeflate::job_t& job, uint32_t q) {
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queries, q);
    vkdeflate::record(ctx, cmd, job);
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
    std::vector<VkDecompressMemoryRegionNV> regs(n);
    for (uint32_t i = 0; i < n; i++) {
        regs[i].srcAddress = src.addr + 4ull * descs[i].src;
        regs[i].dstAddress = dst_khr.addr + descs[i].dst;
        regs[i].compressedSize = 4ull * descs[i].words;
        regs[i].decompressedSize = descs[i].size;
        regs[i].decompressionMethod = VK_MEMORY_DECOMPRESSION_METHOD_GDEFLATE_1_0_BIT_NV;
    }
    memcpy(regions.map, regs.data(), n * sizeof(VkDecompressMemoryRegionNV));
    memcpy(count.map, &n, 4);
    copy(up, src, in_size);
    vkdeflate::job_t job = {src.addr, tiles.addr, dst_vk.addr, status.addr, n};

    double best_khr = 1e30, best_vk = 1e30;
    for (int round = -a.warm; round < a.rounds; round++) {
        bool khr_first = (round & 1) == 0;
        begin();
        vkCmdResetQueryPool(cmd, queries, 0, 4);
        fence_all();
        if (has_layer && khr_first)
            run_layer(a.direct, regs, regions, count, 0);
        run_ours(ctx, job, 2);
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

static bool write_csv(const std::string& path, const std::vector<row_t>& rows) {
    FILE* f = fopen(path.c_str(), "w");
    if (!f)
        return false;
    fprintf(f, "gpu,driver,stream,tiles,out_bytes,cpu1_ms,vkdeflate_ms,khronos_ms,vkdeflate_ok,khronos_ok\n");
    for (const row_t& r : rows) {
        fprintf(f, "%s,%s,%s,%u,%zu,%.2f,%.3f,", field(gpu_name).c_str(), field(driver_text).c_str(), field(r.stream).c_str(), r.tiles, r.out,
                r.cpu_ms, r.vk_ms);
        if (has_layer)
            fprintf(f, "%.3f,%d,%d\n", r.khr_ms, r.vk_ok ? 1 : 0, r.khr_ok ? 1 : 0);
        else
            fprintf(f, ",%d,\n", r.vk_ok ? 1 : 0);
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
        else if (s[0] == '-') {
            printf("unknown argument %s\n", argv[i]);
            return 1;
        } else
            a.files.push_back(s);
    }
    if (a.files.empty()) {
        printf("usage: bench <file.gdz>... | -list <file> [-rounds N] [-warm N] [-csv out.csv | -csvdir folder] [-nolayer] [-direct]\n");
        return 1;
    }

    make_device(a.layer);
    vkdeflate::config_t cfg;
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
        if (!row.stream.empty())
            rows.push_back(row);
    }
    std::string csv = a.csv;
    if (csv.empty() && !a.csvdir.empty()) {
        std::filesystem::create_directories(a.csvdir);
        csv = (std::filesystem::path(a.csvdir) / file_name_for(gpu_name)).string();
    }
    if (!csv.empty()) {
        if (write_csv(csv, rows))
            printf("[bench] %zu rows written to %s\n", rows.size(), csv.c_str());
        else
            printf("[bench] cant write %s\n", csv.c_str());
    }

    vkdeflate::destroy(ctx);
    vkDestroyQueryPool(dev, queries, nullptr);
    vkDestroyCommandPool(dev, pool, nullptr);
    vkDestroyDevice(dev, nullptr);
    vkDestroyInstance(instance, nullptr);
    return ok ? 0 : 1;
}
