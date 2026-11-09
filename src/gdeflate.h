// vkdeflate - a GDeflate 1.0 decoder for Vulkan compute and for the CPU
//
// This header is the CPU side: parsing the stream container and the reference
// decoder, written for clarity, not speed. docs/FORMAT.md is the format.
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

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace gdeflate {
    inline constexpr size_t TILE_SIZE = 65536;
    inline constexpr size_t MAX_TILES = 65535;
    inline constexpr size_t LANES = 32;
    inline constexpr size_t HEADER_SIZE = 8;

    struct tile_t {
        size_t src;
        size_t csize;
        size_t dst;
        size_t size;
    };

    struct stream_t {
        size_t total = 0;
        std::vector<tile_t> tiles;
    };

    struct stats_t {
        size_t blocks = 0;
        size_t stored = 0;
        size_t fixed = 0;
        size_t dynamic = 0;
        size_t literals = 0;
        size_t matches = 0;
        size_t match_bytes = 0;
        size_t max_match = 0;
        size_t long_matches = 0;
        size_t rounds = 0;
        size_t long_codes = 0;
        size_t dep_matches = 0;
        size_t dep_rounds = 0;
        size_t dist_hist[5] = {};
        size_t long_dist_hist[5] = {};
        size_t len_hist[16] = {};
        size_t dlen_hist[16] = {};
        size_t slow_rounds[3] = {};
        size_t lit_rounds = 0;
        size_t far_hist[8] = {};
    };

    extern int g_ring_check;
    extern size_t g_trace;
    bool is_gdeflate(const uint8_t* p, size_t n);
    bool parse(const uint8_t* p, size_t n, stream_t& s, std::string& err);
    bool parse_head(const uint8_t* p, size_t n, size_t stream_size, stream_t& s, std::string& err);
    bool decode_tile(const uint8_t* p, size_t n, uint8_t* out, size_t size, std::string& err, stats_t* st = nullptr);
    bool decompress(const uint8_t* p, size_t n, uint8_t* out, size_t size, std::string& err);
}
