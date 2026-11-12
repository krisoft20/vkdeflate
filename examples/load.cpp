#include "common.h"

#define LOAD_FNS(X)                  \
    X(vkCreateImage)                 \
    X(vkDestroyImage)                \
    X(vkGetImageMemoryRequirements)  \
    X(vkBindImageMemory)             \
    X(vkCmdCopyImageToBuffer)

#define LOAD_DECLARE(fn) static PFN_##fn fn = nullptr;
LOAD_FNS(LOAD_DECLARE)

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
    uint32_t width = size >= 4096 ? 4096 : 1;
    uint32_t height = (uint32_t)(size / width > 4096 ? 4096 : size / width);
    size_t texels = (size_t)width * height;

    ex::buf_t buffer = ex::make_buf(g, size);
    ex::buf_t back = ex::make_buf(g, texels);
    VkImageCreateInfo ici = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = VK_FORMAT_R8_UNORM;
    ici.extent = {width, height, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    VkImage image;
    VkResult r = vkCreateImage(g.dev, &ici, nullptr, &image);
    if (r != VK_SUCCESS)
        ex::die("vkCreateImage", r);
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(g.dev, image, &req);
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < g.mem.memoryTypeCount && type == UINT32_MAX; i++)
        if (((req.memoryTypeBits >> i) & 1) && (g.mem.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
            type = i;
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = type;
    VkDeviceMemory memory;
    if (type == UINT32_MAX || (r = ex::vkAllocateMemory(g.dev, &mai, nullptr, &memory)) != VK_SUCCESS)
        ex::die("image memory", r);
    vkBindImageMemory(g.dev, image, memory, 0);

    vkdeflate::load_t items[2];
    items[0].path = argv[1];
    items[0].buffer = buffer.buf;
    items[1].path = argv[1];
    items[1].image = image;
    items[1].extent = {width, height, 1};
    items[1].layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    if (!vkdeflate::load(ctx, g.queue, g.family, items, 2, err))
        ex::die(("vkdeflate::load: " + err).c_str());

    VkCommandBuffer cmd = ex::begin(g);
    VkBufferImageCopy copy = {};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {width, height, 1};
    vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, back.buf, 1, &copy);
    ex::submit_wait(g, cmd);

    std::vector<uint8_t> ref(size);
    if (!gdeflate::decompress(in.data(), in.size(), ref.data(), size, err))
        ex::die(err.c_str());
    bool buffer_ok = memcmp(buffer.map, ref.data(), size) == 0;
    bool image_ok = memcmp(back.map, ref.data(), texels) == 0;
    printf("[example] %s: buffer of %zu bytes %s, image %ux%u %s\n", argv[1], size,
           buffer_ok ? "same as the cpu decoder" : "FAILED", width, height, image_ok ? "same as the cpu decoder" : "FAILED");

    vkDestroyImage(g.dev, image, nullptr);
    ex::vkFreeMemory(g.dev, memory, nullptr);
    ex::free_buf(g, buffer);
    ex::free_buf(g, back);
    vkdeflate::destroy(ctx);
    ex::destroy(g);
    return buffer_ok && image_ok ? 0 : 1;
}
