// c_test - the C interface from a C compiler: pack a file, read it back on the CPU and on the GPU.
//
//   c_test <any file>
//
// Exit code 0 only when both decoders return the file's bytes.
//
// MIT License, Copyright (c) 2026 krisoft20, see LICENSE.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vkdeflate_c.h"

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("usage: c_test <file>\n");
        return 1;
    }
    FILE* f = fopen(argv[1], "rb");
    if (!f) {
        printf("[c_test] cant open %s\n", argv[1]);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    size_t size = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* data = (uint8_t*)malloc(size ? size : 1);
    if (fread(data, 1, size, f) != size) {
        printf("[c_test] short read\n");
        return 1;
    }
    fclose(f);

    size_t capacity = gdeflate_compress_bound(size);
    uint8_t* stream = (uint8_t*)malloc(capacity);
    size_t packed = 0, unpacked = 0;
    if (!gdeflate_compress(data, size, stream, capacity, &packed, 6)) {
        printf("[c_test] compress: %s\n", vkdeflate_last_error());
        return 1;
    }
    if (!gdeflate_size(stream, packed, &unpacked) || unpacked != size) {
        printf("[c_test] size: %s\n", vkdeflate_last_error());
        return 1;
    }
    uint8_t* cpu = (uint8_t*)malloc(size ? size : 1);
    uint8_t* gpu = (uint8_t*)malloc(size ? size : 1);
    if (!gdeflate_decompress(stream, packed, cpu, size) || memcmp(cpu, data, size)) {
        printf("[c_test] cpu decode: %s\n", vkdeflate_last_error());
        return 1;
    }
    uint32_t tiles = 0;
    if (!vkdeflate_describe(stream, packed, packed, NULL, 0, &tiles, NULL)) {
        printf("[c_test] describe: %s\n", vkdeflate_last_error());
        return 1;
    }
    vkdeflate_ctx* ctx = vkdeflate_create();
    if (!ctx) {
        printf("[c_test] create: %s\n", vkdeflate_last_error());
        return 2;
    }
    if (!vkdeflate_decompress(ctx, stream, packed, gpu, size) || memcmp(gpu, data, size)) {
        printf("[c_test] gpu decode: %s\n", vkdeflate_last_error());
        return 1;
    }
    printf("[c_test] %s: %zu -> %zu bytes, %u tiles, cpu and gpu (%s) return the file\n", argv[1], size, packed, tiles,
           vkdeflate_device_name(ctx));
    vkdeflate_destroy(ctx);
    free(data);
    free(stream);
    free(cpu);
    free(gpu);
    return 0;
}
