#include "common.h"

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("usage: regions <file.gdz>\n");
        return 1;
    }
    std::vector<uint8_t> in = ex::load(argv[1]);
    gdeflate::stream_t stream;
    std::string err;
    if (!gdeflate::parse(in.data(), in.size(), stream, err))
        ex::die(err.c_str());

    ex::gpu_t g = ex::make_gpu();
    vkdeflate::ctx_t* ctx = ex::attach(g);
    ex::buf_t src = ex::make_buf(g, in.size());
    ex::buf_t dst = ex::make_buf(g, stream.total);
    memcpy(src.map, in.data(), in.size());

    std::vector<vkdeflate::region_t> regions;
    for (const gdeflate::tile_t& t : stream.tiles)
        regions.push_back({src.addr + t.src, dst.addr + t.dst, t.csize, t.size});

    VkCommandBuffer cmd = ex::begin(g);
#if defined(VK_EXT_memory_decompression)
    VkDecompressMemoryInfoEXT info = {VK_STRUCTURE_TYPE_DECOMPRESS_MEMORY_INFO_EXT};
    info.decompressionMethod = VK_MEMORY_DECOMPRESSION_METHOD_GDEFLATE_1_0_BIT_EXT;
    info.regionCount = (uint32_t)regions.size();
    info.pRegions = (const VkDecompressMemoryRegionEXT*)regions.data();
    bool recorded = vkdeflate::cmd_decompress_memory(ctx, cmd, &info);
#else
    bool recorded = vkdeflate::cmd_decompress_memory(ctx, cmd, regions.data(), (uint32_t)regions.size());
#endif
    if (!recorded)
        ex::die("cmd_decompress_memory");
    ex::submit_wait(g, cmd);

    bool ok = ex::same_as_cpu(in, (const uint8_t*)dst.map, stream.total);
    printf("[example] %s: %zu regions, %zu bytes, %s\n", argv[1], regions.size(), stream.total,
           ok ? "same as the cpu decoder" : "DIFFERENT from the cpu decoder");
    ex::free_buf(g, src);
    ex::free_buf(g, dst);
    vkdeflate::destroy(ctx);
    ex::destroy(g);
    return ok ? 0 : 1;
}
