#include "common.h"

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("usage: decompress <file.gdz>\n");
        return 1;
    }
    std::vector<uint8_t> in = ex::load(argv[1]);

    std::string err;
    vkdeflate::ctx_t* ctx = vkdeflate::create({}, err);
    if (!ctx) {
        printf("[example] %s\n", err.c_str());
        return 2;
    }

    std::vector<vkdeflate::tile_desc_t> tiles;
    size_t size = 0;
    if (!vkdeflate::describe(in.data(), in.size(), in.size(), tiles, size, err)) {
        printf("[example] %s\n", err.c_str());
        return 1;
    }
    std::vector<uint8_t> out(size);
    if (!vkdeflate::decompress(ctx, in.data(), in.size(), out.data(), out.size(), err)) {
        printf("[example] %s\n", err.c_str());
        return 1;
    }

    bool ok = ex::same_as_cpu(in, out.data(), out.size());
    printf("[example] %s: %zu bytes on %s, %s\n", argv[1], out.size(), vkdeflate::device_name(ctx),
           ok ? "same as the cpu decoder" : "DIFFERENT from the cpu decoder");
    vkdeflate::destroy(ctx);
    return ok ? 0 : 1;
}
