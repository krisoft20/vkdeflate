#include "common.h"

struct stream_t {
    std::vector<uint8_t> in;
    std::vector<vkdeflate::tile_desc_t> tiles;
    size_t size = 0;
    ex::buf_t src, table, dst, status;
};

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("usage: batch <file.gdz> [file.gdz ...]\n");
        return 1;
    }
    ex::gpu_t g = ex::make_gpu();
    vkdeflate::ctx_t* ctx = ex::attach(g);

    std::vector<stream_t> streams(argc - 1);
    std::string err;
    for (size_t i = 0; i < streams.size(); i++) {
        stream_t& s = streams[i];
        s.in = ex::load(argv[i + 1]);
        if (!vkdeflate::describe(s.in.data(), s.in.size(), s.in.size(), s.tiles, s.size, err))
            ex::die(err.c_str());
        s.src = ex::make_buf(g, s.in.size());
        s.table = ex::make_buf(g, s.tiles.size() * sizeof(vkdeflate::tile_desc_t));
        s.dst = ex::make_buf(g, s.size);
        s.status = ex::make_buf(g, s.tiles.size() * 4);
        memcpy(s.src.map, s.in.data(), s.in.size());
        memcpy(s.table.map, s.tiles.data(), s.tiles.size() * sizeof(vkdeflate::tile_desc_t));
    }

    VkCommandBuffer cmd = ex::begin(g);
    for (stream_t& s : streams)
        vkdeflate::record(ctx, cmd, {s.src.addr, s.table.addr, s.dst.addr, s.status.addr, (uint32_t)s.tiles.size(),
                                     vkdeflate::job_flags(s.tiles, s.size)});
    ex::submit_wait(g, cmd);

    bool all = true;
    for (size_t i = 0; i < streams.size(); i++) {
        stream_t& s = streams[i];
        const uint32_t* st = (const uint32_t*)s.status.map;
        bool ok = true;
        for (size_t t = 0; t < s.tiles.size() && ok; t++)
            if (st[t]) {
                printf("[example] %s: tile at byte %u: %s\n", argv[i + 1], s.tiles[t].dst, vkdeflate::tile_error(st[t]));
                ok = false;
            }
        ok = ok && ex::same_as_cpu(s.in, (const uint8_t*)s.dst.map, s.size);
        all = all && ok;
        printf("[example] %s: %zu tiles, %zu bytes, %s\n", argv[i + 1], s.tiles.size(), s.size,
               ok ? "same as the cpu decoder" : "FAILED");
        ex::free_buf(g, s.src);
        ex::free_buf(g, s.table);
        ex::free_buf(g, s.dst);
        ex::free_buf(g, s.status);
    }
    vkdeflate::destroy(ctx);
    ex::destroy(g);
    return all ? 0 : 1;
}
