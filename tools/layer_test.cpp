// layer_test - uses VK_LAYER_vkdeflate_memory_decompression the way an application would: it enables the layer
// and one of the two extension names, turns on none of the features the kernel needs, and decompresses with the
// direct and the indirect call between dispatches of a pipeline of its own.
//
//   layer_test <file.gdz>          (VK_LAYER_PATH must point at the folder with the layer manifest)
//
// Two devices are made: one with separate feature structs and the EXT calls, one with VkPhysicalDeviceFeatures2
// and VkPhysicalDeviceVulkan12Features and the NV calls. Exit code 0 only when every output matches the CPU decoder,
// the application's pipeline and push constants are intact after the decompress calls, and its feature structs
// are unchanged.
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
    X(vkGetPhysicalDeviceProperties2)           \
    X(vkGetPhysicalDeviceFeatures2)             \
    X(vkEnumerateDeviceExtensionProperties)     \
    X(vkGetPhysicalDeviceQueueFamilyProperties) \
    X(vkGetPhysicalDeviceMemoryProperties)      \
    X(vkCreateDevice)                           \
    X(vkGetDeviceProcAddr)

#define VK_DEVICE_FNS(X)               \
    X(vkDestroyDevice)                 \
    X(vkGetDeviceQueue)                \
    X(vkQueueWaitIdle)                 \
    X(vkCreateBuffer)                  \
    X(vkDestroyBuffer)                 \
    X(vkGetBufferMemoryRequirements)   \
    X(vkGetBufferDeviceAddress)        \
    X(vkAllocateMemory)                \
    X(vkFreeMemory)                    \
    X(vkBindBufferMemory)              \
    X(vkMapMemory)                     \
    X(vkCreateCommandPool)             \
    X(vkDestroyCommandPool)            \
    X(vkAllocateCommandBuffers)        \
    X(vkBeginCommandBuffer)            \
    X(vkEndCommandBuffer)              \
    X(vkCmdPipelineBarrier)            \
    X(vkCreateShaderModule)            \
    X(vkDestroyShaderModule)           \
    X(vkCreateDescriptorSetLayout)     \
    X(vkDestroyDescriptorSetLayout)    \
    X(vkCreatePipelineLayout)          \
    X(vkDestroyPipelineLayout)         \
    X(vkCreateComputePipelines)        \
    X(vkDestroyPipeline)               \
    X(vkCreateDescriptorPool)          \
    X(vkDestroyDescriptorPool)         \
    X(vkAllocateDescriptorSets)        \
    X(vkUpdateDescriptorSets)          \
    X(vkCmdBindPipeline)               \
    X(vkCmdBindDescriptorSets)         \
    X(vkCmdPushConstants)              \
    X(vkCmdDispatch)                   \
    X(vkQueueSubmit)

#define VK_DECLARE(fn) PFN_##fn fn = nullptr;
PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;
VK_FNS(VK_DECLARE)
VK_DEVICE_FNS(VK_DECLARE)
PFN_vkCmdDecompressMemoryNV vkCmdDecompressMemoryNV = nullptr;
PFN_vkCmdDecompressMemoryIndirectCountNV vkCmdDecompressMemoryIndirectCountNV = nullptr;
PFN_vkCmdDecompressMemoryEXT vkCmdDecompressMemoryEXT = nullptr;
PFN_vkCmdDecompressMemoryIndirectCountEXT vkCmdDecompressMemoryIndirectCountEXT = nullptr;

constexpr const char* layer_name = "VK_LAYER_vkdeflate_memory_decompression";
constexpr VkFlags64 gdeflate_bit = 1;

// #version 450
// layout(local_size_x = 1) in;
// layout(push_constant) uniform pc_t { uint index; uint value; } pc;
// layout(set = 0, binding = 0) buffer out_t { uint data[]; } o;
// void main() { o.data[pc.index] = pc.value; }
const uint32_t probe_spv[] = {
    0x07230203, 0x00010500, 0x000d000b, 0x0000001b, 0x00000000, 0x00020011, 0x00000001, 0x0006000b, 0x00000001, 0x4c534c47,
    0x6474732e, 0x3035342e, 0x00000000, 0x0003000e, 0x00000000, 0x00000001, 0x0007000f, 0x00000005, 0x00000004, 0x6e69616d,
    0x00000000, 0x0000000a, 0x0000000f, 0x00060010, 0x00000004, 0x00000011, 0x00000001, 0x00000001, 0x00000001, 0x00040047,
    0x00000007, 0x00000006, 0x00000004, 0x00030047, 0x00000008, 0x00000002, 0x00050048, 0x00000008, 0x00000000, 0x00000023,
    0x00000000, 0x00040047, 0x0000000a, 0x00000021, 0x00000000, 0x00040047, 0x0000000a, 0x00000022, 0x00000000, 0x00030047,
    0x0000000d, 0x00000002, 0x00050048, 0x0000000d, 0x00000000, 0x00000023, 0x00000000, 0x00050048, 0x0000000d, 0x00000001,
    0x00000023, 0x00000004, 0x00040047, 0x0000001a, 0x0000000b, 0x00000019, 0x00020013, 0x00000002, 0x00030021, 0x00000003,
    0x00000002, 0x00040015, 0x00000006, 0x00000020, 0x00000000, 0x0003001d, 0x00000007, 0x00000006, 0x0003001e, 0x00000008,
    0x00000007, 0x00040020, 0x00000009, 0x0000000c, 0x00000008, 0x0004003b, 0x00000009, 0x0000000a, 0x0000000c, 0x00040015,
    0x0000000b, 0x00000020, 0x00000001, 0x0004002b, 0x0000000b, 0x0000000c, 0x00000000, 0x0004001e, 0x0000000d, 0x00000006,
    0x00000006, 0x00040020, 0x0000000e, 0x00000009, 0x0000000d, 0x0004003b, 0x0000000e, 0x0000000f, 0x00000009, 0x00040020,
    0x00000010, 0x00000009, 0x00000006, 0x0004002b, 0x0000000b, 0x00000013, 0x00000001, 0x00040020, 0x00000016, 0x0000000c,
    0x00000006, 0x00040017, 0x00000018, 0x00000006, 0x00000003, 0x0004002b, 0x00000006, 0x00000019, 0x00000001, 0x0006002c,
    0x00000018, 0x0000001a, 0x00000019, 0x00000019, 0x00000019, 0x00050036, 0x00000002, 0x00000004, 0x00000000, 0x00000003,
    0x000200f8, 0x00000005, 0x00050041, 0x00000010, 0x00000011, 0x0000000f, 0x0000000c, 0x0004003d, 0x00000006, 0x00000012,
    0x00000011, 0x00050041, 0x00000010, 0x00000014, 0x0000000f, 0x00000013, 0x0004003d, 0x00000006, 0x00000015, 0x00000014,
    0x00060041, 0x00000016, 0x00000017, 0x0000000a, 0x0000000c, 0x00000012, 0x0003003e, 0x00000017, 0x00000015, 0x000100fd,
    0x00010038};

VkInstance instance = VK_NULL_HANDLE;
VkPhysicalDevice phys = VK_NULL_HANDLE;
VkDevice dev = VK_NULL_HANDLE;
VkQueue queue = VK_NULL_HANDLE;
uint32_t family = 0;
VkPhysicalDeviceMemoryProperties memprops = {};
int failures = 0;

struct buf_t {
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkDeviceAddress addr = 0;
    void* map = nullptr;
};

static void die(const char* what, VkResult r = VK_SUCCESS) {
    if (r != VK_SUCCESS)
        printf("[layer_test] %s failed: %d\n", what, (int)r);
    else
        printf("[layer_test] %s\n", what);
    exit(2);
}

static void check(bool ok, const char* mode, const char* what) {
    printf("[layer_test] %-4s %-64s %s\n", mode, what, ok ? "ok" : "FAIL");
    if (!ok)
        failures++;
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

static void make_instance() {
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
    app.pApplicationName = "layer_test";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledLayerCount = 1;
    ici.ppEnabledLayerNames = &layer_name;
    VkResult r = vkCreateInstance(&ici, nullptr, &instance);
    if (r != VK_SUCCESS)
        die("vkCreateInstance with the layer (is VK_LAYER_PATH set?)", r);
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
    printf("[layer_test] device %s\n", props.deviceName);
    uint32_t nfam = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &nfam, nullptr);
    std::vector<VkQueueFamilyProperties> fams(nfam);
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &nfam, fams.data());
    family = UINT32_MAX;
    for (uint32_t i = 0; i < nfam && family == UINT32_MAX; i++)
        if (fams[i].queueFlags & VK_QUEUE_COMPUTE_BIT)
            family = i;
    if (family == UINT32_MAX)
        die("no compute queue");
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

static void run(bool nv, const std::vector<uint8_t>& in, const std::vector<vkdeflate::tile_desc_t>& descs,
                const std::vector<uint8_t>& ref) {
    const char* mode = nv ? "nv" : "ext";
    const char* ext = nv ? "VK_NV_memory_decompression" : "VK_EXT_memory_decompression";

    uint32_t next = 0;
    vkEnumerateDeviceExtensionProperties(phys, nullptr, &next, nullptr);
    std::vector<VkExtensionProperties> exts(next);
    vkEnumerateDeviceExtensionProperties(phys, nullptr, &next, exts.data());
    bool found = false;
    for (const VkExtensionProperties& e : exts)
        found = found || !strcmp(e.extensionName, ext);
    check(found, mode, "extension is listed for the device");
    VkPhysicalDeviceMemoryDecompressionFeaturesEXT fd = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_DECOMPRESSION_FEATURES_EXT};
    VkPhysicalDeviceFeatures2 fq = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &fd};
    vkGetPhysicalDeviceFeatures2(phys, &fq);
    check(fd.memoryDecompression == VK_TRUE, mode, "memoryDecompression feature is reported");
    VkPhysicalDeviceMemoryDecompressionPropertiesEXT pd = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_DECOMPRESSION_PROPERTIES_EXT};
    VkPhysicalDeviceProperties2 pq = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &pd};
    vkGetPhysicalDeviceProperties2(phys, &pq);
    check((pd.decompressionMethods & gdeflate_bit) != 0 && pd.maxDecompressionIndirectCount >= descs.size(), mode,
          "GDeflate 1.0 and an indirect count limit are reported");
    if (!found)
        return;

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkPhysicalDeviceBufferDeviceAddressFeatures bda = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES};
    bda.bufferDeviceAddress = VK_TRUE;
    VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    f12.bufferDeviceAddress = VK_TRUE;
    VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f12};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, nv ? (void*)&f2 : (void*)&bda};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = &ext;
    VkResult r = vkCreateDevice(phys, &dci, nullptr, &dev);
    check(r == VK_SUCCESS, mode, "device is created with only bufferDeviceAddress turned on");
    if (r != VK_SUCCESS)
        return;
    check(!f12.shaderInt8 && !f12.storageBuffer8BitAccess && !f12.vulkanMemoryModel && !f2.features.shaderInt64 &&
              f12.bufferDeviceAddress && bda.bufferDeviceAddress,
          mode, "the application's feature structs are unchanged");
#define VK_LOAD_DEVICE(fn) fn = (PFN_##fn)vkGetDeviceProcAddr(dev, #fn);
    VK_DEVICE_FNS(VK_LOAD_DEVICE)
    VK_LOAD_DEVICE(vkCmdDecompressMemoryNV)
    VK_LOAD_DEVICE(vkCmdDecompressMemoryIndirectCountNV)
    VK_LOAD_DEVICE(vkCmdDecompressMemoryEXT)
    VK_LOAD_DEVICE(vkCmdDecompressMemoryIndirectCountEXT)
    check(nv ? vkCmdDecompressMemoryNV && vkCmdDecompressMemoryIndirectCountNV
             : vkCmdDecompressMemoryEXT && vkCmdDecompressMemoryIndirectCountEXT,
          mode, "the decompress commands resolve");
    vkGetDeviceQueue(dev, family, 0, &queue);

    uint32_t n = (uint32_t)descs.size();
    size_t out_size = ref.size();
    size_t stride = nv ? sizeof(VkDecompressMemoryRegionNV) : sizeof(VkDecompressMemoryRegionEXT);
    buf_t src = make_buf((in.size() + 3) / 4 * 4);
    buf_t dst[3] = {make_buf(out_size), make_buf(out_size), make_buf(out_size)};
    buf_t regions = make_buf(n * stride);
    buf_t count = make_buf(4);
    buf_t probe = make_buf(64);
    memcpy(src.map, in.data(), in.size());
    memcpy(count.map, &n, 4);
    memset(probe.map, 0, 64);
    std::vector<VkDecompressMemoryRegionNV> direct_nv[3];
    std::vector<VkDecompressMemoryRegionEXT> direct_ext[3];
    for (int k = 0; k < 3; k++)
        for (uint32_t i = 0; i < n; i++) {
            VkDecompressMemoryRegionNV reg = {src.addr + 4ull * descs[i].src, dst[k].addr + descs[i].dst, 4ull * descs[i].words,
                                              descs[i].size, gdeflate_bit};
            direct_nv[k].push_back(reg);
            direct_ext[k].push_back({reg.srcAddress, reg.dstAddress, reg.compressedSize, reg.decompressedSize});
        }
    if (nv)
        memcpy(regions.map, direct_nv[1].data(), n * stride);
    else
        memcpy(regions.map, direct_ext[1].data(), n * stride);

    VkShaderModuleCreateInfo smi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smi.codeSize = sizeof(probe_spv);
    smi.pCode = probe_spv;
    VkShaderModule mod;
    if ((r = vkCreateShaderModule(dev, &smi, nullptr, &mod)) != VK_SUCCESS)
        die("vkCreateShaderModule", r);
    VkDescriptorSetLayoutBinding bind = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo sli = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    sli.bindingCount = 1;
    sli.pBindings = &bind;
    VkDescriptorSetLayout set_layout;
    if ((r = vkCreateDescriptorSetLayout(dev, &sli, nullptr, &set_layout)) != VK_SUCCESS)
        die("vkCreateDescriptorSetLayout", r);
    VkPushConstantRange range = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 8};
    VkPipelineLayoutCreateInfo pli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &set_layout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &range;
    VkPipelineLayout layout;
    if ((r = vkCreatePipelineLayout(dev, &pli, nullptr, &layout)) != VK_SUCCESS)
        die("vkCreatePipelineLayout", r);
    VkComputePipelineCreateInfo cpi = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpi.layout = layout;
    cpi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpi.stage.module = mod;
    cpi.stage.pName = "main";
    VkPipeline pipeline;
    if ((r = vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpi, nullptr, &pipeline)) != VK_SUCCESS)
        die("vkCreateComputePipelines", r);
    VkDescriptorPoolSize size = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
    VkDescriptorPoolCreateInfo dpi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets = 1;
    dpi.poolSizeCount = 1;
    dpi.pPoolSizes = &size;
    VkDescriptorPool dpool;
    if ((r = vkCreateDescriptorPool(dev, &dpi, nullptr, &dpool)) != VK_SUCCESS)
        die("vkCreateDescriptorPool", r);
    VkDescriptorSetAllocateInfo dai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dai.descriptorPool = dpool;
    dai.descriptorSetCount = 1;
    dai.pSetLayouts = &set_layout;
    VkDescriptorSet set;
    if ((r = vkAllocateDescriptorSets(dev, &dai, &set)) != VK_SUCCESS)
        die("vkAllocateDescriptorSets", r);
    VkDescriptorBufferInfo dbi = {probe.buf, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet write = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = set;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo = &dbi;
    vkUpdateDescriptorSets(dev, 1, &write, 0, nullptr);

    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.queueFamilyIndex = family;
    VkCommandPool pool;
    if ((r = vkCreateCommandPool(dev, &pci, nullptr, &pool)) != VK_SUCCESS)
        die("vkCreateCommandPool", r);
    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkCommandBuffer cmd;
    if ((r = vkAllocateCommandBuffers(dev, &cai, &cmd)) != VK_SUCCESS)
        die("vkAllocateCommandBuffers", r);

    auto fence = [&] {
        VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_HOST_WRITE_BIT,
                              VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_HOST_READ_BIT};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, nullptr,
                             0, nullptr);
    };
    auto direct = [&](int k) {
        if (nv)
            vkCmdDecompressMemoryNV(cmd, n, direct_nv[k].data());
        else {
            VkDecompressMemoryInfoEXT info = {VK_STRUCTURE_TYPE_DECOMPRESS_MEMORY_INFO_EXT};
            info.decompressionMethod = gdeflate_bit;
            info.regionCount = n;
            info.pRegions = direct_ext[k].data();
            vkCmdDecompressMemoryEXT(cmd, &info);
        }
        fence();
    };
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    fence();
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
    uint32_t pc[2] = {0, 0xa1};
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 8, pc);
    vkCmdDispatch(cmd, 1, 1, 1);
    fence();
    pc[0] = 1;
    pc[1] = 0xb2;
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 8, pc);
    direct(0);
    vkCmdDispatch(cmd, 1, 1, 1);
    fence();
    pc[0] = 2;
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 4, pc);
    if (nv)
        vkCmdDecompressMemoryIndirectCountNV(cmd, regions.addr, count.addr, (uint32_t)stride);
    else
        vkCmdDecompressMemoryIndirectCountEXT(cmd, gdeflate_bit, regions.addr, count.addr, n, (uint32_t)stride);
    fence();
    vkCmdDispatch(cmd, 1, 1, 1);
    fence();
    pc[0] = 3;
    pc[1] = 0xd4;
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 8, pc);
    direct(2);
    vkCmdDispatch(cmd, 1, 1, 1);
    fence();
    vkEndCommandBuffer(cmd);
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    if ((r = vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE)) != VK_SUCCESS)
        die("vkQueueSubmit", r);
    vkQueueWaitIdle(queue);

    check(!memcmp(dst[0].map, ref.data(), out_size), mode, "direct call: output matches the CPU decoder");
    check(!memcmp(dst[1].map, ref.data(), out_size), mode, "indirect count call: output matches the CPU decoder");
    check(!memcmp(dst[2].map, ref.data(), out_size), mode, "second direct call: output matches the CPU decoder");
    const uint32_t* p = (const uint32_t*)probe.map;
    check(p[0] == 0xa1, mode, "the application's own dispatch works");
    check(p[1] == 0xb2, mode, "pipeline and push constants are intact after the direct call");
    check(p[2] == 0xb2, mode, "a partial push constant update is intact after the indirect call");
    check(p[3] == 0xd4, mode, "a repeated push constant update is replayed last");

    vkDestroyCommandPool(dev, pool, nullptr);
    vkDestroyDescriptorPool(dev, dpool, nullptr);
    vkDestroyPipeline(dev, pipeline, nullptr);
    vkDestroyPipelineLayout(dev, layout, nullptr);
    vkDestroyDescriptorSetLayout(dev, set_layout, nullptr);
    vkDestroyShaderModule(dev, mod, nullptr);
    free_buf(src);
    for (buf_t& b : dst)
        free_buf(b);
    free_buf(regions);
    free_buf(count);
    free_buf(probe);
    vkDestroyDevice(dev, nullptr);
}

}

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("usage: layer_test <file.gdz>\n");
        return 1;
    }
    std::vector<uint8_t> in;
    if (!load(argv[1], in)) {
        printf("[layer_test] cant open %s\n", argv[1]);
        return 1;
    }
    std::vector<vkdeflate::tile_desc_t> descs;
    size_t out_size = 0;
    std::string err;
    if (!vkdeflate::describe(in.data(), in.size(), in.size(), descs, out_size, err)) {
        printf("[layer_test] describe: %s\n", err.c_str());
        return 1;
    }
    std::vector<uint8_t> ref(out_size);
    if (!gdeflate::decompress(in.data(), in.size(), ref.data(), out_size, err)) {
        printf("[layer_test] cpu decoder: %s\n", err.c_str());
        return 1;
    }
    make_instance();
    run(false, in, descs, ref);
    run(true, in, descs, ref);
    vkDestroyInstance(instance, nullptr);
    printf("[layer_test] %s\n", failures ? "FAILED" : "all checks passed");
    return failures ? 1 : 0;
}
