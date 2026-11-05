// single_test - the single header on its own: decodes a stream on the CPU and on the GPU through
// single_include/vkdeflate.h and compares the two. Built without the library, as C++17, with the
// Vulkan prototypes visible, the way a project that just dropped the file in would build it.
//
//   single_test <file.gdz>
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

#define VKDEFLATE_IMPLEMENTATION
#include "../single_include/vkdeflate.h"

#include <cstdio>
#include <cstring>
#include <fstream>

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("usage: single_test <file.gdz>\n");
        return 1;
    }
    std::ifstream f(argv[1], std::ios::binary | std::ios::ate);
    if (!f) {
        printf("[single] cant open %s\n", argv[1]);
        return 1;
    }
    std::vector<uint8_t> in((size_t)f.tellg());
    f.seekg(0);
    f.read((char*)in.data(), (std::streamsize)in.size());

    gdeflate::stream_t s;
    std::string err;
    if (!gdeflate::parse(in.data(), in.size(), s, err)) {
        printf("[single] %s: %s\n", argv[1], err.c_str());
        return 1;
    }
    std::vector<uint8_t> cpu(s.total), gpu(s.total);
    if (!gdeflate::decompress(in.data(), in.size(), cpu.data(), cpu.size(), err)) {
        printf("[single] cpu: %s\n", err.c_str());
        return 1;
    }
    vkdeflate::ctx_t* ctx = vkdeflate::create({}, err);
    if (!ctx) {
        printf("[single] create: %s\n", err.c_str());
        return 2;
    }
    vkdeflate::timing_t t;
    bool ok = vkdeflate::decompress(ctx, in.data(), in.size(), gpu.data(), gpu.size(), err, &t);
    if (!ok)
        printf("[single] gpu: %s\n", err.c_str());
    else if (memcmp(cpu.data(), gpu.data(), s.total) != 0) {
        printf("[single] FAIL: cpu and gpu outputs differ\n");
        ok = false;
    } else
        printf("[single] %s: %zu tiles, %zu bytes, cpu and gpu agree, kernel %.2f ms on %s\n", argv[1], s.tiles.size(),
               s.total, t.kernel_ms, vkdeflate::device_name(ctx));
    vkdeflate::destroy(ctx);
    return ok ? 0 : 1;
}
