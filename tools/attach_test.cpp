// attach_test - uses vkdeflate the way a game would: its own Vulkan instance and
// device, its own buffers with device addresses, describe() + attach() + record()
// in its own command buffer, nothing from create().
//
//   attach_test <file.gdz> [original]
//
// Exit code 0 only when every tile reports 0 and the output matches the original
// (or the CPU decoder when no original is given).
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

#define VK_NO_PROTOTYPES
#include <chrono>
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

namespace {

#define VK_FNS(X)                               \
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
    X(vkQueueSubmit)

#define VK_DECLARE(fn) PFN_##fn fn = nullptr;
PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;
VK_FNS(VK_DECLARE)

VkInstance instance = VK_NULL_HANDLE;
VkPhysicalDevice phys = VK_NULL_HANDLE;
VkDevice dev = VK_NULL_HANDLE;
VkQueue queue = VK_NULL_HANDLE;
uint32_t family = 0;
VkPhysicalDeviceMemoryProperties memprops = {};

struct buf_t {
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkDeviceAddress addr = 0;
    void* map = nullptr;
};

static void die(const char* what, VkResult r = VK_SUCCESS) {
    if (r != VK_SUCCESS)
        printf("[attach] %s failed: %d\n", what, (int)r);
    else
        printf("[attach] %s\n", what);
    exit(2);
}

static bool load(const char* path, std::vector<uint8_t>& d) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f)
        return false;
    d.resize((size_t)f.tellg());
    f.seekg(0);
    f.read((char*)d.data(), (std::streamsize)d.size());
    return true;
}

static void make_device() {
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
    app.pApplicationName = "attach_test";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
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
    vkGetPhysicalDeviceProperties(phys, &props);
    vkGetPhysicalDeviceMemoryProperties(phys, &memprops);
    printf("[attach] device %s\n", props.deviceName);

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
    qci.queueFamilyIndex = family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f12};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.pEnabledFeatures = &f10;
    if ((r = vkCreateDevice(phys, &dci, nullptr, &dev)) != VK_SUCCESS)
        die("vkCreateDevice", r);
    vkGetDeviceQueue(dev, family, 0, &queue);
}

static buf_t make_buf(VkDeviceSize size) {
    buf_t b;
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = size < 4 ? 4 : size;
    bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkResult r = vkCreateBuffer(dev, &bci, nullptr, &b.buf);
    if (r != VK_SUCCESS)
        die("vkCreateBuffer", r);
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(dev, b.buf, &req);
    constexpr VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < memprops.memoryTypeCount && type == UINT32_MAX; i++)
        if (((req.memoryTypeBits >> i) & 1) && (memprops.memoryTypes[i].propertyFlags & want) == want)
            type = i;
    if (type == UINT32_MAX)
        die("no host visible memory type");
    VkMemoryAllocateFlagsInfo mfi = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    mfi.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &mfi};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = type;
    if ((r = vkAllocateMemory(dev, &mai, nullptr, &b.mem)) != VK_SUCCESS)
        die("vkAllocateMemory", r);
    if ((r = vkBindBufferMemory(dev, b.buf, b.mem, 0)) != VK_SUCCESS)
        die("vkBindBufferMemory", r);
    if ((r = vkMapMemory(dev, b.mem, 0, VK_WHOLE_SIZE, 0, &b.map)) != VK_SUCCESS)
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

}

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("usage: attach_test <file.gdz> [original]\n");
        return 1;
    }
    std::vector<uint8_t> in, ref;
    if (!load(argv[1], in)) {
        printf("[attach] cant open %s\n", argv[1]);
        return 1;
    }
    if (argc > 2 && !load(argv[2], ref)) {
        printf("[attach] cant open %s\n", argv[2]);
        return 1;
    }

    std::vector<vkdeflate::tile_desc_t> descs;
    size_t out_size = 0;
    std::string err;
    size_t head = in.size() >= 8 ? 8 + 4 * (size_t)(in[2] | (in[3] << 8)) : 0;
    if (!vkdeflate::describe(in.data(), head, in.size(), descs, out_size, err)) {
        printf("[attach] describe: %s\n", err.c_str());
        return 1;
    }
    printf("[attach] %s: %zu tiles, %zu bytes of output, described from the first %zu bytes\n", argv[1], descs.size(),
           out_size, head);

    make_device();
    buf_t src = make_buf((in.size() + 3) / 4 * 4);
    buf_t tiles = make_buf(descs.size() * sizeof(vkdeflate::tile_desc_t));
    buf_t dst = make_buf(out_size);
    buf_t status = make_buf(descs.size() * 4);
    memcpy(src.map, in.data(), in.size());
    memcpy(tiles.map, descs.data(), descs.size() * sizeof(vkdeflate::tile_desc_t));
    memset(status.map, 0xff, descs.size() * 4);

    vkdeflate::config_t cfg;
    cfg.verbose = true;
    vkdeflate::ctx_t* ctx = vkdeflate::attach(instance, phys, dev, vkGetInstanceProcAddr, cfg, err);
    if (!ctx) {
        printf("[attach] attach: %s\n", err.c_str());
        return 2;
    }

    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.queueFamilyIndex = family;
    VkCommandPool pool;
    VkResult r = vkCreateCommandPool(dev, &pci, nullptr, &pool);
    if (r != VK_SUCCESS)
        die("vkCreateCommandPool", r);
    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkCommandBuffer cmd;
    if ((r = vkAllocateCommandBuffers(dev, &cai, &cmd)) != VK_SUCCESS)
        die("vkAllocateCommandBuffers", r);
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    VkMemoryBarrier before = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_HOST_WRITE_BIT,
                              VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &before, 0,
                         nullptr, 0, nullptr);
    vkdeflate::job_t job = {src.addr, tiles.addr, dst.addr, status.addr, (uint32_t)descs.size()};
    vkdeflate::record(ctx, cmd, job);
    VkMemoryBarrier after = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &after, 0,
                         nullptr, 0, nullptr);
    vkEndCommandBuffer(cmd);

    auto t0 = std::chrono::steady_clock::now();
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    if ((r = vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE)) != VK_SUCCESS)
        die("vkQueueSubmit", r);
    vkQueueWaitIdle(queue);
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

    bool ok = true;
    const uint32_t* st = (const uint32_t*)status.map;
    for (size_t i = 0; i < descs.size(); i++)
        if (st[i]) {
            printf("[attach] FAIL tile %zu: %s (%u)\n", i, vkdeflate::tile_error(st[i]), st[i]);
            ok = false;
            break;
        }
    if (ref.empty()) {
        ref.resize(out_size);
        if (!gdeflate::decompress(in.data(), in.size(), ref.data(), out_size, err)) {
            printf("[attach] cpu decoder: %s\n", err.c_str());
            ok = false;
        }
    }
    if (ok && ref.size() != out_size) {
        printf("[attach] FAIL: original has %zu bytes, stream says %zu\n", ref.size(), out_size);
        ok = false;
    }
    if (ok) {
        size_t d = 0;
        const uint8_t* o = (const uint8_t*)dst.map;
        while (d < out_size && o[d] == ref[d])
            d++;
        if (d != out_size) {
            printf("[attach] FAIL: first differing byte %zu of %zu\n", d, out_size);
            ok = false;
        } else {
            printf("[attach] output matches %s byte for byte (%zu bytes), submit to idle %.2f ms, buffers in host "
                   "visible memory\n",
                   argc > 2 ? argv[2] : "the cpu decoder", out_size, ms);
        }
    }

    vkdeflate::destroy(ctx);
    vkDestroyCommandPool(dev, pool, nullptr);
    free_buf(src);
    free_buf(tiles);
    free_buf(dst);
    free_buf(status);
    vkDestroyDevice(dev, nullptr);
    vkDestroyInstance(instance, nullptr);
    return ok ? 0 : 1;
}
