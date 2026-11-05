// gdref: drives the GDeflate reference implementation (Microsoft DirectStorage + NVIDIA libdeflate fork,
// cloned into third_party_build/, their code under their licenses, Apache 2.0 / MIT) to produce test
// vectors for vkdeflate and to cross-check its decoders. Built by build.cmd into third_party_build/gdref/build.
//
//   gdref c <in> <out.gdz> [level 0-12, default 6]
//   gdref d <in.gdz> <out>
//   gdref t <in.gdz>            prints the tile table
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

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>

#include "GDeflate.h"

static std::vector<uint8_t> load(const char* path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) {
        printf("[gdref] cant open %s\n", path);
        exit(1);
    }
    std::vector<uint8_t> d((size_t)f.tellg());
    f.seekg(0);
    f.read((char*)d.data(), (std::streamsize)d.size());
    return d;
}

static void save(const char* path, const uint8_t* p, size_t n) {
    std::ofstream f(path, std::ios::binary);
    if (!f) {
        printf("[gdref] cant write %s\n", path);
        exit(1);
    }
    f.write((const char*)p, (std::streamsize)n);
}

static uint32_t le32(const uint8_t* p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        printf("usage: gdref c <in> <out.gdz> [level]\n       gdref d <in.gdz> <out>\n       gdref t <in.gdz>\n");
        return 1;
    }
    std::string mode = argv[1];
    std::vector<uint8_t> in = load(argv[2]);
    if (mode == "c") {
        if (argc < 4)
            return 1;
        uint32_t level = argc > 4 ? (uint32_t)atoi(argv[4]) : 6;
        size_t bound = GDeflate::CompressBound(in.size());
        std::vector<uint8_t> out(bound);
        size_t out_size = bound;
        auto t0 = std::chrono::steady_clock::now();
        if (!GDeflate::Compress(out.data(), &out_size, in.data(), in.size(), level, 0)) {
            printf("[gdref] compress failed\n");
            return 1;
        }
        auto t1 = std::chrono::steady_clock::now();
        save(argv[3], out.data(), out_size);
        printf("[gdref] %s: %zu -> %zu bytes (%.3f) level %u in %.1f ms\n", argv[2], in.size(), out_size,
               (double)out_size / (double)in.size(), level,
               std::chrono::duration<double, std::milli>(t1 - t0).count());
        return 0;
    }
    if (mode == "d") {
        if (argc < 4 || in.size() < 8)
            return 1;
        uint32_t num_tiles = in[2] | (in[3] << 8);
        uint32_t w = le32(in.data() + 4);
        uint32_t last = (w >> 2) & 0x3ffff;
        size_t total = (size_t)(num_tiles ? num_tiles - 1 : 0) * 65536 + (last ? last : (num_tiles ? 65536 : 0));
        std::vector<uint8_t> out(total);
        auto t0 = std::chrono::steady_clock::now();
        if (!GDeflate::Decompress(out.data(), out.size(), in.data(), in.size(), 1)) {
            printf("[gdref] decompress failed\n");
            return 1;
        }
        auto t1 = std::chrono::steady_clock::now();
        save(argv[3], out.data(), out.size());
        printf("[gdref] %s: %zu -> %zu bytes in %.1f ms (1 thread)\n", argv[2], in.size(), out.size(),
               std::chrono::duration<double, std::milli>(t1 - t0).count());
        return 0;
    }
    if (mode == "t") {
        if (in.size() < 8)
            return 1;
        uint32_t num_tiles = in[2] | (in[3] << 8);
        uint32_t w = le32(in.data() + 4);
        printf("[gdref] id %u magic %02x tiles %u size_idx %u last %u reserved %u\n", in[0], in[1], num_tiles, w & 3,
               (w >> 2) & 0x3ffff, w >> 20);
        const uint8_t* tab = in.data() + 8;
        for (uint32_t i = 0; i < num_tiles && i < 8; i++)
            printf("[gdref] entry %u = %u\n", i, le32(tab + 4 * i));
        return 0;
    }
    return 1;
}
