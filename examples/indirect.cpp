#include "common.h"

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("usage: indirect <file.gdz>\n");
        return 1;
    }
    std::vector<uint8_t> in = ex::load(argv[1]);
    gdeflate::stream_t stream;
    std::string err;
    if (!gdeflate::parse(in.data(), in.size(), stream, err))
        ex::die(err.c_str());
    uint32_t n = (uint32_t)stream.tiles.size();

    ex::gpu_t g = ex::make_gpu();
    vkdeflate::ctx_t* ctx = ex::attach(g);
    ex::buf_t src = ex::make_buf(g, in.size());
    ex::buf_t dst = ex::make_buf(g, stream.total);
    ex::buf_t regions = ex::make_buf(g, n * sizeof(vkdeflate::region_t));
    ex::buf_t count = ex::make_buf(g, 4);
    ex::buf_t status = ex::make_buf(g, n * 4);
    memcpy(src.map, in.data(), in.size());
    vkdeflate::region_t* r = (vkdeflate::region_t*)regions.map;
    for (uint32_t i = 0; i < n; i++) {
        const gdeflate::tile_t& t = stream.tiles[i];
        r[i] = {src.addr + t.src, dst.addr + t.dst, t.csize, t.size};
    }
    memset(status.map, 0xff, n * 4);

    VkCommandBuffer cmd = ex::begin(g);
    ex::vkCmdFillBuffer(cmd, count.buf, 0, 4, n);
    VkMemoryBarrier filled = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_TRANSFER_WRITE_BIT,
                              VK_ACCESS_SHADER_READ_BIT};
    ex::vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &filled, 0,
                             nullptr, 0, nullptr);
    vkdeflate::record_regions_indirect(ctx, cmd, regions.addr, count.addr, n + 64, sizeof(vkdeflate::region_t),
                                       status.addr);
    ex::submit_wait(g, cmd);

    bool ok = true;
    const uint32_t* st = (const uint32_t*)status.map;
    for (uint32_t i = 0; i < n && ok; i++)
        if (st[i]) {
            printf("[example] region %u: %s\n", i, vkdeflate::tile_error(st[i]));
            ok = false;
        }
    ok = ok && ex::same_as_cpu(in, (const uint8_t*)dst.map, stream.total);
    printf("[example] %s: count %u read on the gpu, %zu bytes, %s\n", argv[1], n, stream.total,
           ok ? "same as the cpu decoder" : "FAILED");
    ex::free_buf(g, src);
    ex::free_buf(g, dst);
    ex::free_buf(g, regions);
    ex::free_buf(g, count);
    ex::free_buf(g, status);
    vkdeflate::destroy(ctx);
    ex::destroy(g);
    return ok ? 0 : 1;
}
