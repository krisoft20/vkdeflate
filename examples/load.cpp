#include "common.h"

#define LOAD_FNS(X)                  \
    X(vkCreateImage)                 \
    X(vkDestroyImage)                \
    X(vkGetImageMemoryRequirements)  \
    X(vkBindImageMemory)             \
    X(vkCmdCopyImageToBuffer)

#define LOAD_DECLARE(fn) static PFN_##fn fn = nullptr;
LOAD_FNS(LOAD_DECLARE)

struct image_t {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
};

static image_t make_image(const ex::gpu_t& g, uint32_t width, uint32_t height, uint32_t mips) {
    image_t out;
    VkImageCreateInfo ici = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = VK_FORMAT_R8_UNORM;
    ici.extent = {width, height, 1};
    ici.mipLevels = mips;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    VkResult r = vkCreateImage(g.dev, &ici, nullptr, &out.image);
    if (r != VK_SUCCESS)
        ex::die("vkCreateImage", r);
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(g.dev, out.image, &req);
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < g.mem.memoryTypeCount && type == UINT32_MAX; i++)
        if (((req.memoryTypeBits >> i) & 1) && (g.mem.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
            type = i;
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = type;
    if (type == UINT32_MAX || (r = ex::vkAllocateMemory(g.dev, &mai, nullptr, &out.memory)) != VK_SUCCESS)
        ex::die("image memory", r);
    vkBindImageMemory(g.dev, out.image, out.memory, 0);
    return out;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("usage: load <file.gdz>\n");
        return 1;
    }
    ex::gpu_t g = ex::make_gpu();
    vkdeflate::ctx_t* ctx = ex::attach(g);
#define LOAD_GET(fn) fn = (PFN_##fn)ex::vkGetInstanceProcAddr(g.instance, #fn);
    LOAD_FNS(LOAD_GET)

    std::vector<uint8_t> in = ex::load(argv[1]);
    std::vector<vkdeflate::tile_desc_t> tiles;
    size_t size = 0;
    std::string err;
    if (!vkdeflate::describe(in.data(), in.size(), in.size(), tiles, size, err))
        ex::die(err.c_str());
    std::vector<uint8_t> ref(size);
    if (!gdeflate::decompress(in.data(), in.size(), ref.data(), size, err))
        ex::die(err.c_str());

    uint32_t width = size >= 8192 ? 4096 : 2;
    uint32_t height = (uint32_t)(size / width > 4096 ? 4096 : size / width) & ~1u;
    if (height < 2) {
        printf("[example] %s is too small for the image test\n", argv[1]);
        return 1;
    }
    size_t top = (size_t)width * height;
    size_t half = (size_t)(width / 2) * (height / 2);
    bool mips = top + half <= size;

    ex::buf_t buffer = ex::make_buf(g, size);
    ex::buf_t back = ex::make_buf(g, top + half);
    image_t image = make_image(g, width, height, mips ? 2 : 1);
    VkBufferImageCopy regions[2] = {};
    regions[0].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    regions[0].imageExtent = {width, height, 1};
    regions[1].bufferOffset = top;
    regions[1].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 1, 0, 1};
    regions[1].imageExtent = {width / 2, height / 2, 1};

    vkdeflate::load_t items[2];
    items[0].path = argv[1];
    items[0].buffer = buffer.buf;
    items[1].data = in.data();
    items[1].size = in.size();
    items[1].image = image.image;
    items[1].regions = regions;
    items[1].region_count = mips ? 2 : 1;
    items[1].layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    if (!vkdeflate::load(ctx, g.queue, g.family, items, 2, err))
        ex::die(("vkdeflate::load: " + err).c_str());

    VkCommandBuffer cmd = ex::begin(g);
    VkBufferImageCopy copies[2] = {regions[0], regions[1]};
    vkCmdCopyImageToBuffer(cmd, image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, back.buf, mips ? 2 : 1, copies);
    ex::submit_wait(g, cmd);
    bool buffer_ok = memcmp(buffer.map, ref.data(), size) == 0;
    bool image_ok = memcmp(back.map, ref.data(), mips ? top + half : top) == 0;
    printf("[example] %s: load() into a buffer of %zu bytes %s, into a %ux%u image with %d mip level%s %s\n", argv[1], size,
           buffer_ok ? "ok" : "FAILED", width, height, mips ? 2 : 1, mips ? "s" : "", image_ok ? "ok" : "FAILED");

    memset(buffer.map, 0, size);
    cmd = ex::begin(g);
    bool recorded = vkdeflate::record_load(ctx, cmd, items, 1, err);
    ex::submit_wait(g, cmd);
    bool record_ok = recorded && vkdeflate::load_result(ctx, err) && memcmp(buffer.map, ref.data(), size) == 0;
    printf("[example] record_load() in the caller's command buffer %s\n", record_ok ? "ok" : "FAILED");

    vkdeflate::trim(ctx);
    memset(buffer.map, 0, size);
    bool trim_ok = vkdeflate::load(ctx, g.queue, g.family, items, 1, err) && memcmp(buffer.map, ref.data(), size) == 0;
    printf("[example] load() after trim() %s\n", trim_ok ? "ok" : "FAILED");

    vkdeflate::features_t want;
    bool listed = vkdeflate::device_features(g.instance, g.phys, ex::vkGetInstanceProcAddr, want, err);
    VkDeviceCreateInfo bare = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    vkdeflate::config_t strict;
    strict.created_with = &bare;
    std::string why;
    vkdeflate::ctx_t* refused = vkdeflate::attach(g.instance, g.phys, g.dev, ex::vkGetInstanceProcAddr, strict, why);
    bool check_ok = listed && want.v12.bufferDeviceAddress && !refused && why.find("shaderInt64") != std::string::npos;
    printf("[example] device_features() and the created_with check %s\n", check_ok ? "ok" : "FAILED");
    if (refused)
        vkdeflate::destroy(refused);

    vkDestroyImage(g.dev, image.image, nullptr);
    ex::vkFreeMemory(g.dev, image.memory, nullptr);
    ex::free_buf(g, buffer);
    ex::free_buf(g, back);
    vkdeflate::destroy(ctx);
    ex::destroy(g);
    return buffer_ok && image_ok && record_ok && trim_ok && check_ok ? 0 : 1;
}
