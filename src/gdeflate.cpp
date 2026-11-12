// vkdeflate - a GDeflate 1.0 decoder for Vulkan compute and for the CPU
//
// CPU reference decoder. One tile at a time, 32 lanes decoded in rounds exactly
// as docs/FORMAT.md describes them, Huffman codes read one bit at a time. It is
// the oracle the GPU decoder is checked against, so it is kept obvious.
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

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

#include "gdeflate.h"

namespace gdeflate {

constexpr uint8_t stream_id = 4;
constexpr int max_code_len = 15;
constexpr int precode_syms = 19;
constexpr int litlen_syms = 288;
constexpr int dist_syms = 32;
constexpr int end_of_block = 256;
constexpr int precode_order[precode_syms] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
constexpr uint32_t length_base[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                      31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 3};
constexpr uint8_t length_extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                      2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 16};
constexpr uint32_t dist_base[32] = {1,    2,    3,    4,    5,    7,     9,     13,    17,    25,    33,
                                    49,   65,   97,   129,  193,  257,   385,   513,   769,   1025,  1537,
                                    2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577, 32769, 49153};
constexpr uint8_t dist_extra[32] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4,  4,  5,  5,  6,  6,
                                    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13, 14, 14};

int g_ring_check = 0;

struct ring_t {
    std::vector<uint8_t> slot;
    std::vector<int32_t> mark;
    std::vector<std::pair<size_t, uint8_t>> lits;
    size_t from = 0;
    size_t win = 0;
    size_t round = 0;
    bool reported = false;
};

static void ring_write(ring_t& r, size_t x, uint8_t v, int32_t kind) {
    if (g_ring_check && x >= r.win) {
        r.slot[x & (r.slot.size() - 1)] = v;
        r.mark[x] = (int32_t)r.round * 10 + kind;
    }
}

static uint32_t le32(const uint8_t* p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

bool is_gdeflate(const uint8_t* p, size_t n) {
    return n >= HEADER_SIZE && p[0] == stream_id && p[1] == (stream_id ^ 0xff);
}

bool parse(const uint8_t* p, size_t n, stream_t& s, std::string& err) {
    return parse_head(p, n, n, s, err);
}

bool parse_head(const uint8_t* p, size_t n, size_t stream_size, stream_t& s, std::string& err) {
    s.total = 0;
    s.tiles.clear();
    if (n < HEADER_SIZE) {
        err = "input shorter than the 8-byte header";
        return false;
    }
    if (p[0] != stream_id || p[1] != (stream_id ^ 0xff)) {
        err = "not a gdeflate stream";
        return false;
    }
    size_t num_tiles = p[2] | ((size_t)p[3] << 8);
    uint32_t w = le32(p + 4);
    size_t last = (w >> 2) & 0x3ffff;
    if ((w & 3) != 1) {
        err = "tile size index is not 1 (64 KB)";
        return false;
    }
    if (w >> 20) {
        err = "reserved header bits set";
        return false;
    }
    if (last > TILE_SIZE) {
        err = "last tile size above 64 KB";
        return false;
    }
    if (!num_tiles)
        return true;
    size_t table = HEADER_SIZE + 4 * num_tiles;
    if (table > n) {
        err = "tile table past the end of the input";
        return false;
    }
    size_t last_csize = le32(p + HEADER_SIZE);
    s.tiles.resize(num_tiles);
    for (size_t i = 0; i < num_tiles; i++) {
        size_t off = i ? le32(p + HEADER_SIZE + 4 * i) : 0;
        size_t end = i + 1 < num_tiles ? le32(p + HEADER_SIZE + 4 * (i + 1)) : off + last_csize;
        if (end <= off || end - off < LANES * 4) {
            err = "tile " + std::to_string(i) + " has a bad size in the tile table";
            return false;
        }
        if (stream_size < table || end > stream_size - table) {
            err = "tile " + std::to_string(i) + " ends past the end of the input";
            return false;
        }
        bool is_last = i + 1 == num_tiles;
        s.tiles[i] = tile_t{table + off, end - off, i * TILE_SIZE, is_last && last ? last : TILE_SIZE};
    }
    s.total = (num_tiles - 1) * TILE_SIZE + s.tiles.back().size;
    return true;
}

struct lane_t {
    uint64_t buf = 0;
    uint32_t cnt = 0;
};

struct tile_dec_t {
    const uint8_t* in;
    size_t words;
    size_t next;
    lane_t lanes[LANES];
    uint8_t* out;
    size_t size;
    size_t pos;
    const char* fail = nullptr;
    stats_t* st = nullptr;
    ring_t ring;
    int lit_max = 0;
    int dist_max = 0;
};

static void refill(tile_dec_t& t, int lane) {
    lane_t& l = t.lanes[lane];
    if (l.cnt >= 32)
        return;
    if (t.next >= t.words) {
        t.fail = "read past the end of the tile";
        return;
    }
    l.buf |= (uint64_t)le32(t.in + 4 * t.next) << l.cnt;
    l.cnt += 32;
    t.next++;
}

static uint32_t bits(tile_dec_t& t, int lane, int n) {
    lane_t& l = t.lanes[lane];
    uint32_t v = (uint32_t)l.buf & ((1u << n) - 1);
    l.buf >>= n;
    l.cnt -= n;
    return v;
}

struct huff_t {
    uint16_t count[max_code_len + 1] = {};
    uint16_t syms[litlen_syms] = {};
    bool empty = true;
    bool single = false;
};

static bool build(const uint8_t* lens, int n, huff_t& h) {
    h = huff_t{};
    uint32_t used = 0;
    for (int i = 0; i < n; i++) {
        if (lens[i] > max_code_len)
            return false;
        if (lens[i]) {
            h.count[lens[i]]++;
            used += 1u << (max_code_len - lens[i]);
        }
    }
    if (!used)
        return true;
    h.empty = false;
    if (used != 1u << max_code_len) {
        if (used != 1u << (max_code_len - 1) || h.count[1] != 1)
            return false;
        h.single = true;
    }
    uint16_t offs[max_code_len + 2];
    offs[1] = 0;
    for (int len = 1; len <= max_code_len; len++)
        offs[len + 1] = offs[len] + h.count[len];
    for (int i = 0; i < n; i++)
        if (lens[i])
            h.syms[offs[lens[i]]++] = (uint16_t)i;
    return true;
}

static int decode(tile_dec_t& t, int lane, const huff_t& h, int kind = 2) {
    lane_t& l = t.lanes[lane];
    if (h.single) {
        bits(t, lane, 1);
        return h.syms[0];
    }
    uint32_t v = (uint32_t)l.buf;
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= max_code_len; len++) {
        code |= (v >> (len - 1)) & 1;
        int count = h.count[len];
        if (code - first < count) {
            bits(t, lane, len);
            if (t.st) {
                if (len > 10)
                    t.st->long_codes++;
                if (kind == 0) {
                    t.st->len_hist[len]++;
                    if (len > t.lit_max)
                        t.lit_max = len;
                } else if (kind == 1) {
                    t.st->dlen_hist[len]++;
                    if (len > t.dist_max)
                        t.dist_max = len;
                }
            }
            return h.syms[index + code - first];
        }
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

static bool fixed_codes(huff_t& lit, huff_t& dist) {
    uint8_t lens[litlen_syms];
    for (int i = 0; i < 144; i++)
        lens[i] = 8;
    for (int i = 144; i < 256; i++)
        lens[i] = 9;
    for (int i = 256; i < 280; i++)
        lens[i] = 7;
    for (int i = 280; i < 288; i++)
        lens[i] = 8;
    if (!build(lens, litlen_syms, lit))
        return false;
    for (int i = 0; i < dist_syms; i++)
        lens[i] = 5;
    return build(lens, dist_syms, dist);
}

static bool dynamic_codes(tile_dec_t& t, huff_t& lit, huff_t& dist) {
    int hlit = (int)bits(t, 0, 5) + 257;
    int hdist = (int)bits(t, 0, 5) + 1;
    int hclen = (int)bits(t, 0, 4) + 4;
    refill(t, 0);

    uint8_t plens[precode_syms] = {};
    for (int i = 0; i < hclen; i++) {
        plens[precode_order[i]] = (uint8_t)bits(t, i, 3);
        refill(t, i);
    }
    huff_t pre;
    if (!build(plens, precode_syms, pre) || pre.empty) {
        t.fail = "bad precode";
        return false;
    }

    uint8_t lens[litlen_syms + dist_syms] = {};
    int total = hlit + hdist;
    int n = 0;
    int prev = -1;
    while (n < total) {
        for (int lane = 0; lane < (int)LANES && n < total; lane++) {
            int sym = decode(t, lane, pre);
            int rep = 1;
            int val = sym;
            if (sym < 0) {
                t.fail = "bad precode symbol";
                return false;
            }
            if (sym == 16) {
                if (prev < 0) {
                    t.fail = "repeat with no previous code length";
                    return false;
                }
                rep = 3 + (int)bits(t, lane, 2);
                val = prev;
            } else if (sym == 17) {
                rep = 3 + (int)bits(t, lane, 3);
                val = 0;
            } else if (sym == 18) {
                rep = 11 + (int)bits(t, lane, 7);
                val = 0;
            }
            refill(t, lane);
            if (n + rep > total) {
                t.fail = "code length run past the end";
                return false;
            }
            for (int i = 0; i < rep; i++)
                lens[n++] = (uint8_t)val;
            prev = val;
        }
        if (t.fail)
            return false;
    }
    if ((hlit > 286 && lens[286]) || (hlit > 287 && lens[287])) {
        t.fail = "code length for symbol 286 or 287";
        return false;
    }
    if (!build(lens, hlit, lit) || lit.empty || !lens[end_of_block]) {
        t.fail = "bad literal/length code";
        return false;
    }
    if (!build(lens + hlit, hdist, dist)) {
        t.fail = "bad distance code";
        return false;
    }
    return true;
}

size_t g_trace = SIZE_MAX;

static bool copy(tile_dec_t& t, size_t at, uint32_t len, uint32_t dist) {
    if (at <= g_trace && g_trace < at + len)
        printf("[trace] match at %zu len %u dist %u\n", at, len, dist);
    if (dist > at) {
        t.fail = "match before the start of the tile";
        return false;
    }
    for (uint32_t i = 0; i < len; i++)
        t.out[at + i] = t.out[at + i - dist];
    return true;
}

static bool read_dist(tile_dec_t& t, int lane, const huff_t& dist, size_t at, uint32_t len, size_t seg, bool& dep,
                      uint32_t& d) {
    int sym = dist.empty ? -1 : decode(t, lane, dist, 1);
    if (sym < 0 || sym >= dist_syms) {
        t.fail = "bad distance symbol";
        return false;
    }
    d = dist_base[sym] + bits(t, lane, dist_extra[sym]);
    refill(t, lane);
    dep = at - d + len > seg;
    if (t.st) {
        int bucket = d <= 1 ? 0 : d <= 4 ? 1 : d <= 32 ? 2 : d <= 128 ? 3 : 4;
        t.st->dist_hist[bucket]++;
        int far = 0;
        while (far < 7 && d > (512u << far))
            far++;
        t.st->far_hist[far]++;
        if (len > LANES)
            t.st->long_dist_hist[bucket]++;
    }
    if (d > at) {
        t.fail = "match before the start of the tile";
        return false;
    }
    return true;
}

struct job_t {
    int lane;
    size_t at;
    uint32_t len;
    uint32_t d;
    bool dep;
};

static bool run_copies(tile_dec_t& t, std::vector<job_t>& jobs, size_t pos_end) {
    ring_t& r = t.ring;
    size_t ring_lo = 0;
    if (g_ring_check) {
        r.win = pos_end >= r.slot.size() ? pos_end - r.slot.size() : 0;
        for (auto& lit : r.lits)
            ring_write(r, lit.first, lit.second, 1);
        r.lits.clear();
        ring_lo = r.win > r.from ? r.win : r.from;
    }
    for (const job_t& j : jobs)
        if (!j.dep && j.len <= LANES) {
            copy(t, j.at, j.len, j.d);
            for (uint32_t k = 0; k < j.len; k++)
                ring_write(r, j.at + k, t.out[j.at + k], 2);
        }
    for (const job_t& j : jobs)
        if (!j.dep && j.len > LANES) {
            copy(t, j.at, j.len, j.d);
            if (j.at + j.len > r.from)
                r.from = j.at + j.len;
            if (g_ring_check)
                for (uint32_t k = 0; k < j.len; k++)
                    r.mark[j.at + k] = -2;
        }
    for (const job_t& j : jobs) {
        if (!j.dep)
            continue;
        if (r.from > ring_lo)
            ring_lo = r.from;
        if (g_ring_check && j.len <= LANES && j.at - j.d >= ring_lo) {
            for (uint32_t k = 0; k < j.len && !r.reported; k++) {
                size_t src = j.at - j.d + (k % j.d);
                uint8_t have = r.slot[src & (r.slot.size() - 1)];
                uint8_t want = t.out[src];
                if (have != want) {
                    printf("[ringcheck] round %zu: dep match lane %d at %zu len %u dist %u reads %zu: ring %02x out %02x "
                           "(pos_end %zu ring_lo %zu from %zu) mark of %zu: %d, of %zu: %d, of %zu: %d\n",
                           r.round, j.lane, j.at, j.len, j.d, src, have, want, pos_end, ring_lo, r.from, src, r.mark[src],
                           src - 1, r.mark[src - 1], src + 1, r.mark[src + 1]);
                    r.reported = true;
                }
            }
            copy(t, j.at, j.len, j.d);
            for (uint32_t k = 0; k < j.len; k++)
                ring_write(r, j.at + k, t.out[j.at + k], 3);
        } else {
            copy(t, j.at, j.len, j.d);
            if (j.len <= LANES)
                for (uint32_t k = 0; k < j.len; k++)
                    ring_write(r, j.at + k, t.out[j.at + k], 5);
            else {
                if (j.at + j.len > r.from)
                    r.from = j.at + j.len;
                if (g_ring_check)
                    for (uint32_t k = 0; k < j.len; k++)
                        r.mark[j.at + k] = -3;
            }
        }
    }
    jobs.clear();
    return !t.fail;
}

static bool huffman_block(tile_dec_t& t, const huff_t& lit, const huff_t& dist) {
    bool pending[LANES] = {};
    size_t match_at[LANES] = {};
    uint32_t match_len[LANES] = {};
    size_t prev_seg = 0, seg = t.pos;
    std::vector<job_t> jobs;
    int eob = -1;
    while (eob < 0) {
        prev_seg = seg;
        seg = t.pos;
        bool dep_round = false;
        if (t.st) {
            t.st->rounds++;
            t.lit_max = 0;
            t.dist_max = 0;
        }
        t.ring.round++;
        size_t lits_before = t.st ? t.st->literals : 0;
        bool any_pending = false;
        for (int lane = 0; lane < (int)LANES; lane++)
            any_pending = any_pending || pending[lane];
        for (int lane = 0; lane < (int)LANES; lane++) {
            if (pending[lane]) {
                bool dep = false;
                uint32_t d = 0;
                if (!read_dist(t, lane, dist, match_at[lane], match_len[lane], prev_seg, dep, d))
                    return false;
                jobs.push_back(job_t{lane, match_at[lane], match_len[lane], d, dep});
                pending[lane] = false;
                if (t.st && dep) {
                    t.st->dep_matches++;
                    dep_round = true;
                }
                continue;
            }
            if (eob >= 0)
                continue;
            int sym = decode(t, lane, lit, 0);
            if (sym < 0 || sym > 285) {
                t.fail = "bad literal/length symbol";
                return false;
            }
            if (sym < end_of_block) {
                refill(t, lane);
                if (t.pos >= t.size) {
                    t.fail = "output past the end of the tile";
                    return false;
                }
                if (g_ring_check)
                    t.ring.lits.push_back({t.pos, (uint8_t)sym});
                t.out[t.pos++] = (uint8_t)sym;
                if (t.st)
                    t.st->literals++;
            } else if (sym == end_of_block) {
                refill(t, lane);
                eob = lane;
            } else {
                uint32_t len = length_base[sym - 257] + bits(t, lane, length_extra[sym - 257]);
                refill(t, lane);
                if (len > t.size - t.pos) {
                    t.fail = "match past the end of the tile";
                    return false;
                }
                pending[lane] = true;
                match_at[lane] = t.pos;
                match_len[lane] = len;
                t.pos += len;
                if (t.st) {
                    t.st->matches++;
                    t.st->match_bytes += len;
                    t.st->long_matches += len > LANES;
                    if (len > t.st->max_match)
                        t.st->max_match = len;
                }
            }
            if (t.fail)
                return false;
        }
        if (t.fail)
            return false;
        if (t.st && !any_pending && t.st->literals - lits_before == LANES)
            t.st->lit_rounds++;
        if (!run_copies(t, jobs, t.pos))
            return false;
        if (t.st && dep_round)
            t.st->dep_rounds++;
        if (t.st) {
            t.st->slow_rounds[0] += t.lit_max > 10 || t.dist_max > 8;
            t.st->slow_rounds[1] += t.lit_max > 9 || t.dist_max > 8;
            t.st->slow_rounds[2] += t.lit_max > 9 || t.dist_max > 7;
        }
    }
    t.ring.round++;
    for (int lane = 0; lane < (int)LANES; lane++) {
        bool dep = false;
        uint32_t d = 0;
        if (pending[lane]) {
            if (!read_dist(t, lane, dist, match_at[lane], match_len[lane], seg, dep, d))
                return false;
            jobs.push_back(job_t{lane, match_at[lane], match_len[lane], d, dep});
        }
        if (t.st && dep)
            t.st->dep_matches++;
    }
    return run_copies(t, jobs, t.pos);
}

static bool stored_block(tile_dec_t& t) {
    size_t len = bits(t, 0, 16);
    refill(t, 0);
    if (len > t.size - t.pos) {
        t.fail = "stored block past the end of the tile";
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        int lane = (int)(i % LANES);
        uint8_t v = (uint8_t)bits(t, lane, 8);
        t.ring.win = t.pos + (len - i) >= t.ring.slot.size() && g_ring_check ? t.pos + (len - i) - t.ring.slot.size() : 0;
        ring_write(t.ring, t.pos, v, 4);
        t.out[t.pos++] = v;
        refill(t, lane);
        if (t.fail)
            return false;
    }
    return true;
}

bool decode_tile(const uint8_t* p, size_t n, uint8_t* out, size_t size, std::string& err, stats_t* st) {
    tile_dec_t t;
    t.st = st;
    if (g_ring_check) {
        t.ring.slot.assign((size_t)g_ring_check, 0);
        t.ring.mark.assign(size + 1, -1);
    }
    t.in = p;
    t.words = n / 4;
    t.next = 0;
    t.out = out;
    t.size = size;
    t.pos = 0;
    if (t.words < LANES) {
        err = "tile shorter than 32 packets";
        return false;
    }
    for (int lane = 0; lane < (int)LANES; lane++)
        refill(t, lane);

    for (;;) {
        bool final = bits(t, 0, 1);
        int type = (int)bits(t, 0, 2);
        refill(t, 0);
        if (t.fail)
            break;
        if (st) {
            st->blocks++;
            st->stored += type == 0;
            st->fixed += type == 1;
            st->dynamic += type == 2;
        }
        if (type == 0) {
            if (!stored_block(t))
                break;
        } else if (type == 3) {
            t.fail = "reserved block type";
            break;
        } else {
            huff_t lit, dist;
            bool ok = type == 1 ? fixed_codes(lit, dist) : dynamic_codes(t, lit, dist);
            if (!ok || !huffman_block(t, lit, dist))
                break;
        }
        if (final) {
            if (t.pos != t.size)
                t.fail = "tile decoded to the wrong size";
            break;
        }
    }
    if (t.fail) {
        err = t.fail;
        return false;
    }
    return true;
}

bool decompress(const uint8_t* p, size_t n, uint8_t* out, size_t size, std::string& err) {
    stream_t s;
    if (!parse(p, n, s, err))
        return false;
    if (size < s.total) {
        err = "output buffer too small";
        return false;
    }
    for (size_t i = 0; i < s.tiles.size(); i++) {
        const tile_t& t = s.tiles[i];
        std::string why;
        if (!decode_tile(p + t.src, t.csize, out + t.dst, t.size, why)) {
            err = "tile " + std::to_string(i) + ": " + why;
            return false;
        }
    }
    return true;
}

struct lane_out_t {
    uint64_t acc = 0;
    int fill = 0;
    bool has_next = false;
    uint32_t slot = 0;
    uint32_t next_slot = 0;
};

struct tile_enc_t {
    lane_out_t lanes[LANES];
    uint32_t next = (uint32_t)LANES;
    std::vector<uint32_t>& packets;
    uint32_t* data;
    size_t cap;

    explicit tile_enc_t(std::vector<uint32_t>& store) : packets(store) {
        if (packets.size() < 20000)
            packets.resize(20000);
        data = packets.data();
        cap = packets.size();
        for (uint32_t i = 0; i < LANES; i++)
            lanes[i].slot = i;
    }

    void grow() {
        packets.resize(packets.size() * 2);
        data = packets.data();
        cap = packets.size();
    }
};

struct token_t {
    uint16_t len;
    uint16_t val;

    token_t() = default;
    token_t(uint32_t l, uint32_t v) : len((uint16_t)l), val((uint16_t)v) {}
};

static void put(tile_enc_t& t, int lane, uint32_t v, int n) {
    lane_out_t& l = t.lanes[lane];
    l.acc |= (uint64_t)v << l.fill;
    l.fill += n;
    if (l.fill >= 32) {
        t.data[l.slot] = (uint32_t)l.acc;
        l.slot = l.next_slot;
        l.has_next = false;
        l.acc >>= 32;
        l.fill -= 32;
    }
}

static void claim(tile_enc_t& t, int lane) {
    lane_out_t& l = t.lanes[lane];
    if (!l.has_next && l.fill > 0) {
        if (t.next == t.cap)
            t.grow();
        l.next_slot = t.next++;
        l.has_next = true;
    }
}

static void finish(tile_enc_t& t) {
    for (lane_out_t& l : t.lanes) {
        t.data[l.slot] = (uint32_t)l.acc;
        if (l.has_next)
            t.data[l.next_slot] = 0;
    }
}

static void store_words(uint8_t* out, const uint32_t* words, size_t count) {
    const uint32_t one = 1;
    if (*(const uint8_t*)&one) {
        memcpy(out, words, count * 4);
        return;
    }
    for (size_t i = 0; i < count; i++)
        for (int b = 0; b < 4; b++)
            out[4 * i + b] = (uint8_t)(words[i] >> (8 * b));
}

static void code_lengths(const uint32_t* freq, int n, int limit, uint8_t* lens) {
    struct leaves_t {
        int v[litlen_syms];
        int count = 0;
        const int* begin() const { return v; }
        const int* end() const { return v + count; }
    };
    memset(lens, 0, (size_t)n);
    leaves_t leaf;
    for (int i = 0; i < n; i++)
        if (freq[i])
            leaf.v[leaf.count++] = i;
    int used = leaf.count;
    if (!used)
        return;
    if (used == 1) {
        lens[leaf.v[0]] = 1;
        lens[leaf.v[0] ? 0 : 1] = 1;
        return;
    }
    uint64_t keys[litlen_syms];
    for (int i = 0; i < used; i++)
        keys[i] = ((uint64_t)freq[leaf.v[i]] << 16) | (uint64_t)leaf.v[i];
    std::sort(keys, keys + used);
    int order[litlen_syms];
    for (int i = 0; i < used; i++)
        order[i] = (int)(keys[i] & 0xffff);
    uint64_t weight[2 * litlen_syms];
    int parent[2 * litlen_syms];
    for (int i = 0; i < used; i++)
        weight[i] = freq[order[i]];
    int next_leaf = 0, next_node = used, made = used;
    auto take = [&]() {
        if (next_leaf < used && (next_node == made || weight[next_leaf] <= weight[next_node]))
            return next_leaf++;
        return next_node++;
    };
    while (made < 2 * used - 1) {
        int x = take();
        int y = take();
        weight[made] = weight[x] + weight[y];
        parent[x] = made;
        parent[y] = made;
        made++;
    }
    uint8_t depth[2 * litlen_syms];
    depth[made - 1] = 0;
    for (int i = made - 2; i >= 0; i--)
        depth[i] = (uint8_t)(depth[parent[i]] + 1);
    bool over = false;
    for (int i = 0; i < used; i++) {
        over = over || depth[i] > limit;
        lens[order[i]] = (uint8_t)(depth[i] > limit ? limit : depth[i]);
    }
    if (!over)
        return;
    uint32_t full = 1u << limit;
    uint32_t kraft = 0;
    for (int s : leaf)
        kraft += 1u << (limit - lens[s]);
    while (kraft > full) {
        int pick = -1;
        for (int s : leaf)
            if (lens[s] < limit && (pick < 0 || lens[s] > lens[pick] || (lens[s] == lens[pick] && freq[s] < freq[pick])))
                pick = s;
        lens[pick]++;
        kraft -= 1u << (limit - lens[pick]);
    }
    while (kraft < full) {
        int pick = -1;
        for (int s : leaf)
            if (lens[s] > 1 && (1u << (limit - lens[s])) <= full - kraft && (pick < 0 || freq[s] > freq[pick]))
                pick = s;
        if (pick < 0)
            break;
        kraft += 1u << (limit - lens[pick]);
        lens[pick]--;
    }
}

static void make_codes(const uint8_t* lens, int n, uint16_t* codes) {
    uint16_t count[max_code_len + 1] = {};
    uint16_t next[max_code_len + 2] = {};
    for (int i = 0; i < n; i++)
        count[lens[i]]++;
    count[0] = 0;
    for (int len = 1; len <= max_code_len; len++)
        next[len + 1] = (uint16_t)((next[len] + count[len]) << 1);
    for (int i = 0; i < n; i++) {
        codes[i] = 0;
        if (!lens[i])
            continue;
        uint16_t c = next[lens[i]]++;
        for (int b = 0; b < lens[i]; b++)
            codes[i] |= (uint16_t)(((c >> b) & 1) << (lens[i] - 1 - b));
    }
}

struct symbol_tables_t {
    uint8_t length[259];
    uint8_t dist[768];

    symbol_tables_t() {
        for (uint32_t len = 0; len <= 258; len++) {
            int i = 27;
            while (i > 0 && length_base[i] > len)
                i--;
            length[len] = (uint8_t)i;
        }
        for (uint32_t x = 0; x < 768; x++) {
            uint32_t d = x < 256 ? x + 1 : ((x - 256) << 7) + 1;
            int i = dist_syms - 1;
            while (i > 0 && dist_base[i] > d)
                i--;
            dist[x] = (uint8_t)i;
        }
    }
};

static const symbol_tables_t symbol_tables;

static int length_symbol(uint32_t len) {
    return len > 258 ? 28 : symbol_tables.length[len];
}

static int dist_symbol(uint32_t d) {
    d--;
    return symbol_tables.dist[d < 256 ? d : 256 + (d >> 7)];
}

struct hist_t {
    uint32_t lit[286] = {};
    uint32_t dist[dist_syms] = {};
    uint64_t extra = 0;

    void add(const token_t& k) {
        if (!k.len) {
            lit[k.val]++;
            return;
        }
        int ls = length_symbol(k.len);
        int ds = dist_symbol(k.val);
        lit[257 + ls]++;
        dist[ds]++;
        extra += length_extra[ls] + dist_extra[ds];
    }
};

struct marks_t {
    std::vector<hist_t> hist;
    std::vector<uint32_t> at;
};

struct chains_t {
    uint16_t head3[1 << 14];
    uint16_t head4[1 << 16];
    uint16_t prev[TILE_SIZE];
    uint16_t kids[2 * TILE_SIZE];
    marks_t marks;
    std::vector<token_t> store;
    std::vector<token_t> tokens;
    std::vector<token_t> trial;
    std::vector<token_t> step;
    std::vector<token_t> cands;
    std::vector<uint8_t> cand_syms;
    std::vector<uint32_t> first;
    std::vector<uint32_t> cost;
    std::vector<uint32_t> packets;
    std::vector<uint32_t> raw_packets;
};

static void prefetch(const void* p) {
#if defined(_MSC_VER) && defined(_M_X64)
    _mm_prefetch((const char*)p, _MM_HINT_T0);
#elif defined(__GNUC__)
    __builtin_prefetch(p);
#else
    (void)p;
#endif
}

static uint32_t load32(const uint8_t* p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

static uint32_t match_length(const uint8_t* a, const uint8_t* b, uint32_t len, uint32_t limit) {
    while (len + 8 <= limit) {
        uint64_t x, y;
        memcpy(&x, a + len, 8);
        memcpy(&y, b + len, 8);
        if (x != y) {
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_ARM64))
            unsigned long bit;
            _BitScanForward64(&bit, x ^ y);
            return len + (bit >> 3);
#elif defined(__GNUC__) && defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
            return len + ((uint32_t)__builtin_ctzll(x ^ y) >> 3);
#else
            break;
#endif
        }
        len += 8;
    }
    while (len < limit && a[len] == b[len])
        len++;
    return len;
}

static uint32_t first3(uint32_t four) {
    const uint32_t one = 1;
    return *(const uint8_t*)&one ? four & 0xffffff : four >> 8;
}

static size_t find_tokens(chains_t& c, const uint8_t* p, size_t n, int level, hist_t& h, marks_t* marks, size_t points) {
    struct tune_t {
        int depth;
        uint32_t nice;
        bool lazy;
        size_t fill;
        bool short_hash;
        int skip_shift;
        uint32_t lazy_max;
    };
    constexpr uint32_t max_len = 65535;
    constexpr int bits3 = 14;
    constexpr int bits4 = 16;
    static const tune_t tunes[8] = {{2, 12, false, 16, true, 4, 258},   {2, 12, false, 16, true, 4, 258},   {4, 16, false, 1024, true, 5, 258},
                                    {6, 24, false, 1024, true, 0, 258}, {5, 20, true, 1024, true, 5, 16},   {13, 56, true, 1024, true, 5, 32},
                                    {26, 112, true, 1024, true, 7, 40}, {48, 258, true, 1024, true, 7, 56}};
    const tune_t tune = tunes[level < 1 ? 1 : level > 7 ? 7 : level];
    memset(c.head3, 0, sizeof(c.head3));
    memset(c.head4, 0, sizeof(c.head4));
    c.prev[0] = 0;
    uint16_t* const head3 = c.head3;
    uint16_t* const head4 = c.head4;
    uint16_t* const prev = c.prev;
    const uint8_t* const base = p;
    const size_t size = n;
    const uint32_t nice = tune.nice;
    const bool short_hash = tune.short_hash;
    if (c.store.size() < size + 8)
        c.store.resize(TILE_SIZE + 8);
    token_t* const first = c.store.data();
    token_t* dst = first;
    auto insert = [=](size_t i) {
        if (i + 4 > size)
            return;
        uint32_t four = load32(base + i);
        uint32_t h = (four * 0x9e3779b1u) >> (32 - bits4);
        prev[i] = head4[h];
        head4[h] = (uint16_t)i;
        head3[(first3(four) * 0x9e3779b1u) >> (32 - bits3)] = (uint16_t)i;
    };
    auto search = [=](size_t i, int tries, uint32_t floor, uint32_t& dist) -> uint32_t {
        if (i + 4 > size)
            return 0;
        uint32_t limit = (uint32_t)(size - i < max_len ? size - i : max_len);
        uint32_t best = floor;
        if (best >= limit)
            return 0;
        const uint8_t* b = base + i;
        uint32_t want = load32(b);
        uint32_t key3 = first3(want);
        uint32_t h3 = (key3 * 0x9e3779b1u) >> (32 - bits3);
        uint32_t near = head3[h3];
        head3[h3] = (uint16_t)i;
        uint32_t found = 0;
        if (short_hash && floor < 3 && near < i && first3(load32(base + near)) == key3) {
            best = match_length(base + near, b, 3, limit);
            found = (uint32_t)(i - near);
        }
        uint32_t h4 = (want * 0x9e3779b1u) >> (32 - bits4);
        uint32_t at = head4[h4];
        prev[i] = (uint16_t)at;
        head4[h4] = (uint16_t)i;
        if (i + 8 <= size) {
            uint32_t soon = head4[(load32(b + 1) * 0x9e3779b1u) >> (32 - bits4)];
            prefetch(base + soon);
            prefetch(&prev[soon]);
            uint32_t ahead = load32(b + 2);
            prefetch(&head4[(ahead * 0x9e3779b1u) >> (32 - bits4)]);
            prefetch(&head3[(first3(ahead) * 0x9e3779b1u) >> (32 - bits3)]);
        }
        while (at < i && best < nice && best < limit) {
            const uint8_t* a = base + at;
            if (a[best] == b[best] && load32(a) == want) {
                uint32_t len = match_length(a, b, 4, limit);
                if (len > best) {
                    best = len;
                    found = (uint32_t)(i - at);
                }
            }
            uint32_t next = prev[at];
            if (--tries <= 0 || next >= at)
                break;
            at = next;
        }
        dist = found;
        if (best == 3 && found > 4096)
            return 0;
        return best > floor && best >= 3 ? best : 0;
    };
    const int depth = tune.depth;
    const bool lazy = tune.lazy;
    const uint32_t lazy_max = tune.lazy_max;
    const int skip_shift = tune.skip_shift;
    const size_t fill_max = tune.fill;
    constexpr uint32_t lazy_good = 8;
    constexpr int far_shift = 1;
    constexpr int far_shift2 = 5;
    const int lazy_shallow = (depth + 2) / 3;
    const int lazy_deep = (depth + 5) / 6;
    size_t i = 0;
    uint32_t misses = 0;
    size_t mark_step = size / (points + 1) + 1;
    size_t next_mark = marks ? mark_step : size + 1;
    while (i < size) {
        if (i >= next_mark) {
            marks->hist.push_back(h);
            marks->at.push_back((uint32_t)(dst - first));
            next_mark += mark_step;
        }
        uint32_t dist = 0;
        uint32_t len = search(i, depth, 2, dist);
        size_t done = i + 1;
        while (lazy && len && len < lazy_max && i + 1 < size) {
            uint32_t dist2 = 0;
            uint32_t len2 = search(i + 1, len >= lazy_good ? lazy_deep : lazy_shallow, len, dist2);
            done = i + 2;
            if (len2 <= len || (len2 == len + 1 && (dist2 >> far_shift) > dist) || (len2 == len + 2 && (dist2 >> far_shift2) > dist))
                break;
            h.lit[base[i]]++;
            *dst++ = token_t(0, base[i]);
            i++;
            len = len2;
            dist = dist2;
        }
        if (!len) {
            h.lit[base[i]]++;
            *dst++ = token_t(0, base[i]);
            i++;
            misses++;
            if (skip_shift)
                for (uint32_t extra = misses >> skip_shift; extra > 0 && i < size; extra--) {
                    h.lit[base[i]]++;
                    *dst++ = token_t(0, base[i++]);
                }
            continue;
        }
        misses = 0;
        *dst = token_t(len, dist);
        h.add(*dst++);
        size_t stop = i + len;
        size_t fill = stop - done > fill_max ? done + fill_max : stop;
        for (size_t k = done; k < fill; k++)
            insert(k);
        for (size_t k = stop > 8 && stop - 8 > fill ? stop - 8 : fill; k < stop; k++)
            insert(k);
        i = stop;
    }
    return (size_t)(dst - first);
}

struct best_tune_t {
    int depth;
    uint32_t nice;
    int rounds;
    int split_points;
    int split_depth;
    int exact_split;
};

static best_tune_t best_tune(int level) {
    static const best_tune_t tunes[5] = {{6, 24, 1, 7, 1, 0}, {16, 48, 1, 15, 3, 0}, {20, 64, 2, 31, 5, 0}, {40, 128, 3, 63, 6, 0}, {80, 258, 4, 255, 8, 0}};
    best_tune_t t = tunes[level < 8 ? 0 : level > 12 ? 4 : level - 8];
    return t;
}

static void best_tokens(chains_t& c, const uint8_t* p, size_t n, int level, std::vector<token_t>& out) {
    constexpr uint32_t max_len = 65535;
    constexpr uint16_t none = 0xffff;
    best_tune_t tune = best_tune(level);
    uint32_t nice = tune.nice;

    std::vector<token_t>& tokens = c.trial;
    tokens.clear();
    std::vector<uint32_t>& first = c.first;
    std::vector<token_t>& cands = c.cands;
    std::vector<uint8_t>& cand_syms = c.cand_syms;
    first.resize(n + 1);
    std::fill_n(c.head3, (size_t)1 << 13, none);
    std::fill_n(c.head4, (size_t)1 << 15, none);
    {
        uint16_t* const head3 = c.head3;
        uint16_t* const head4 = c.head4;
        uint16_t* const kids = c.kids;
        uint32_t* const starts = first.data();
        const uint8_t* const base = p;
        const size_t size = n;
        const int depth = tune.depth;
        token_t* cand = cands.data();
        uint8_t* cand_sym = cand_syms.data();
        size_t room = cands.size();
        size_t made = 0;
        size_t quiet_until = 0;
        for (size_t i = 0; i < size; i++) {
            starts[i] = (uint32_t)made;
            if (i + 4 > size)
                continue;
            if (made + (size_t)depth + 2 > room) {
                room = room * 2 + 4 * TILE_SIZE;
                cands.resize(room);
                cand_syms.resize(room);
                cand = cands.data();
                cand_sym = cand_syms.data();
            }
            const uint8_t* b = base + i;
            bool quiet = i < quiet_until;
            uint32_t limit = (uint32_t)(size - i < max_len ? size - i : max_len);
            if (quiet && limit > nice)
                limit = nice;
            uint32_t best = 2;
            uint32_t last_dist = 0;
            uint32_t four = load32(b);
            uint32_t key3 = first3(four);
            uint32_t h3 = (key3 * 0x9e3779b1u) >> (32 - 13);
            uint32_t near = head3[h3];
            head3[h3] = (uint16_t)i;
            if (!quiet && near != none && first3(load32(base + near)) == key3) {
                best = 3;
                last_dist = (uint32_t)(i - near);
                cand[made] = token_t(3, (uint32_t)(i - near));
                cand_sym[made++] = (uint8_t)dist_symbol((uint32_t)(i - near));
            }
            uint32_t h4 = (four * 0x9e3779b1u) >> (32 - 15);
            uint32_t cur = head4[h4];
            head4[h4] = (uint16_t)i;
            if (i + 8 <= size) {
                uint32_t ahead = load32(b + 1);
                uint32_t root = head4[(ahead * 0x9e3779b1u) >> (32 - 15)];
                if (root != none) {
                    prefetch(&kids[2 * root]);
                    prefetch(base + root);
                }
                uint32_t later = load32(b + 2);
                prefetch(&head4[(later * 0x9e3779b1u) >> (32 - 15)]);
                prefetch(&head3[(first3(later) * 0x9e3779b1u) >> (32 - 13)]);
            }
            uint16_t* lower = &kids[2 * i];
            uint16_t* higher = &kids[2 * i + 1];
            uint32_t lower_len = 0, higher_len = 0;
            for (int tries = depth;; tries--) {
                if (cur == none || tries == 0) {
                    *lower = none;
                    *higher = none;
                    break;
                }
                const uint8_t* a = base + cur;
                uint32_t len = match_length(a, b, lower_len < higher_len ? lower_len : higher_len, limit);
                if (!quiet && len > best) {
                    best = len;
                    last_dist = (uint32_t)(i - cur);
                    cand[made] = token_t(len, last_dist);
                    cand_sym[made++] = (uint8_t)dist_symbol(last_dist);
                }
                if (len >= nice || len == limit) {
                    *lower = kids[2 * cur];
                    *higher = kids[2 * cur + 1];
                    break;
                }
                if (a[len] < b[len]) {
                    *lower = (uint16_t)cur;
                    lower = &kids[2 * cur + 1];
                    cur = *lower;
                    lower_len = len;
                } else {
                    *higher = (uint16_t)cur;
                    higher = &kids[2 * cur];
                    cur = *higher;
                    higher_len = len;
                }
            }
            if (!quiet && best >= nice)
                quiet_until = i + best;
        }
        starts[size] = (uint32_t)made;
    }

    {
        const token_t* cand = cands.data();
        const uint32_t* starts = first.data();
        tokens.clear();
        for (size_t i = 0; i < n;) {
            if (starts[i] == starts[i + 1]) {
                tokens.push_back(token_t(0, p[i]));
                i++;
                continue;
            }
            token_t pick = cand[starts[i + 1] - 1];
            while (i + 1 < n && starts[i + 1] != starts[i + 2]) {
                token_t next = cand[starts[i + 2] - 1];
                if (next.len <= pick.len || (next.len == pick.len + 1u && (next.val >> 1) > pick.val))
                    break;
                tokens.push_back(token_t(0, p[i]));
                i++;
                pick = next;
            }
            tokens.push_back(pick);
            i += pick.len;
        }
    }

    std::vector<uint32_t>& cost = c.cost;
    std::vector<token_t>& step = c.step;
    cost.resize(n + 1);
    step.resize(n + 1);
    for (int round = 0; round < tune.rounds; round++) {
        hist_t h;
        for (const token_t& k : tokens)
            h.add(k);
        h.lit[end_of_block]++;
        uint32_t lit_total = 0, dist_total = 0;
        for (uint32_t f : h.lit)
            lit_total += f;
        for (uint32_t f : h.dist)
            dist_total += f;
        auto bits = [](uint32_t f, uint32_t total) {
            double v = std::log2((double)(total + 1) / (f ? (double)f : 0.35));
            return (uint32_t)(v * 16.0 + 0.5);
        };
        uint32_t lit_cost[286], dist_cost[dist_syms], len_cost[259];
        for (int i = 0; i < 286; i++)
            lit_cost[i] = bits(h.lit[i], lit_total);
        for (int i = 0; i < dist_syms; i++)
            dist_cost[i] = bits(h.dist[i], dist_total) + 16u * dist_extra[i];
        for (uint32_t l = 3; l <= 258; l++) {
            int sym = length_symbol(l);
            len_cost[l] = lit_cost[257 + sym] + 16u * length_extra[sym];
        }
        uint32_t long_cost = lit_cost[285] + 16 * 16;

        constexpr uint32_t rough_tail = 4;
        const bool rough = round + 1 < tune.rounds;
        const token_t* const cand = cands.data();
        const uint8_t* const cand_sym = cand_syms.data();
        const uint32_t* const starts = first.data();
        uint32_t* const costs = cost.data();
        token_t* const steps = step.data();
        const uint8_t* const bytes = p;
        costs[n] = 0;
        for (size_t i = n; i-- > 0;) {
            uint32_t best = lit_cost[bytes[i]] + costs[i + 1];
            token_t pick = {0, bytes[i]};
            uint32_t lo = 3;
            const uint32_t* ahead = costs + i;
            for (uint32_t k = starts[i], end = starts[i + 1]; k < end; k++) {
                uint32_t len = cand[k].len;
                uint32_t dc = dist_cost[cand_sym[k]];
                uint32_t hi = len > 258 ? 258 : len;
                if (len > 258 && long_cost + dc + ahead[len] < best) {
                    best = long_cost + dc + ahead[len];
                    pick = {len, cand[k].val};
                }
                uint32_t low = UINT32_MAX, low_at = 0;
                for (uint32_t l = rough && hi > lo + rough_tail ? hi - rough_tail : lo; l <= hi; l++) {
                    uint32_t v = len_cost[l] + ahead[l];
                    if (v < low) {
                        low = v;
                        low_at = l;
                    }
                }
                if (low != UINT32_MAX && low + dc < best) {
                    best = low + dc;
                    pick = {low_at, cand[k].val};
                }
                lo = hi + 1;
            }
            costs[i] = best;
            steps[i] = pick;
        }
        tokens.clear();
        for (size_t at = 0; at < n;) {
            tokens.push_back(step[at]);
            at += step[at].len ? step[at].len : 1;
        }
    }
    out.swap(tokens);
}

struct item_t {
    uint8_t sym;
    uint8_t extra;
};

struct block_t {
    uint8_t lit_lens[286];
    uint8_t dist_lens[dist_syms];
    uint8_t pre_lens[precode_syms];
    uint16_t pre_code[precode_syms];
    int hlit;
    int hdist;
    int hclen;
    int item_count;
    item_t items[286 + dist_syms];
};

static uint64_t plan_block(hist_t h, block_t& b) {
    h.lit[end_of_block]++;
    code_lengths(h.lit, 286, max_code_len, b.lit_lens);
    code_lengths(h.dist, dist_syms, max_code_len, b.dist_lens);
    b.hlit = 286;
    b.hdist = dist_syms;
    while (b.hlit > 257 && !b.lit_lens[b.hlit - 1])
        b.hlit--;
    while (b.hdist > 1 && !b.dist_lens[b.hdist - 1])
        b.hdist--;
    uint8_t lens[286 + dist_syms];
    memcpy(lens, b.lit_lens, (size_t)b.hlit);
    memcpy(lens + b.hlit, b.dist_lens, (size_t)b.hdist);
    int total = b.hlit + b.hdist;
    b.item_count = 0;
    for (int i = 0; i < total;) {
        int run = 1;
        while (i + run < total && lens[i + run] == lens[i])
            run++;
        int left = run;
        if (!lens[i]) {
            while (left >= 11) {
                int r = left > 138 ? 138 : left;
                b.items[b.item_count++] = {18, (uint8_t)(r - 11)};
                left -= r;
            }
            if (left >= 3) {
                b.items[b.item_count++] = {17, (uint8_t)(left - 3)};
                left = 0;
            }
        } else {
            b.items[b.item_count++] = {lens[i], 0};
            left--;
            while (left >= 3) {
                int r = left > 6 ? 6 : left;
                b.items[b.item_count++] = {16, (uint8_t)(r - 3)};
                left -= r;
            }
        }
        for (; left > 0; left--)
            b.items[b.item_count++] = {lens[i], 0};
        i += run;
    }
    uint32_t pre_freq[precode_syms] = {};
    for (int i = 0; i < b.item_count; i++)
        pre_freq[b.items[i].sym]++;
    code_lengths(pre_freq, precode_syms, 7, b.pre_lens);
    make_codes(b.pre_lens, precode_syms, b.pre_code);
    b.hclen = precode_syms;
    while (b.hclen > 4 && !b.pre_lens[precode_order[b.hclen - 1]])
        b.hclen--;

    uint64_t bits = 3 + 14 + 3 * (uint64_t)b.hclen + h.extra;
    for (int i = 0; i < b.item_count; i++) {
        const item_t& it = b.items[i];
        bits += b.pre_lens[it.sym] + (it.sym == 16 ? 2 : it.sym == 17 ? 3 : it.sym == 18 ? 7 : 0);
    }
    for (int i = 0; i < 286; i++)
        bits += (uint64_t)h.lit[i] * b.lit_lens[i];
    for (int i = 0; i < dist_syms; i++)
        bits += (uint64_t)h.dist[i] * b.dist_lens[i];
    return bits;
}

static void write_block(tile_enc_t& t, const token_t* tokens, size_t count, const block_t& b, bool final) {
    uint16_t lit_code[286], dist_code[dist_syms];
    make_codes(b.lit_lens, 286, lit_code);
    make_codes(b.dist_lens, dist_syms, dist_code);
    put(t, 0, final ? 1 : 0, 1);
    put(t, 0, 2, 2);
    claim(t, 0);
    put(t, 0, (uint32_t)(b.hlit - 257), 5);
    put(t, 0, (uint32_t)(b.hdist - 1), 5);
    put(t, 0, (uint32_t)(b.hclen - 4), 4);
    claim(t, 0);
    for (int i = 0; i < b.hclen; i++) {
        put(t, i, b.pre_lens[precode_order[i]], 3);
        claim(t, i);
    }
    for (int k = 0; k < b.item_count; k++) {
        int lane = k % (int)LANES;
        const item_t& it = b.items[k];
        put(t, lane, b.pre_code[it.sym], b.pre_lens[it.sym]);
        if (it.sym >= 16)
            put(t, lane, it.extra, it.sym == 16 ? 2 : it.sym == 17 ? 3 : 7);
        claim(t, lane);
    }

    while (t.cap < (size_t)t.next + 2 * count + 4 * LANES)
        t.grow();
    uint64_t acc[LANES];
    int fill[LANES];
    uint32_t slot[LANES], next_slot[LANES];
    uint32_t has = 0;
    for (int lane = 0; lane < (int)LANES; lane++) {
        const lane_out_t& l = t.lanes[lane];
        acc[lane] = l.acc;
        fill[lane] = l.fill;
        slot[lane] = l.slot;
        next_slot[lane] = l.next_slot;
        has |= (uint32_t)l.has_next << lane;
    }
    uint32_t* const data = t.data;
    uint32_t next = t.next;
    uint8_t lit_bits[286], dist_bits[dist_syms];
    uint16_t lit_bit_code[286], dist_bit_code[dist_syms];
    memcpy(lit_bits, b.lit_lens, 286);
    memcpy(dist_bits, b.dist_lens, dist_syms);
    memcpy(lit_bit_code, lit_code, sizeof(lit_bit_code));
    memcpy(dist_bit_code, dist_code, sizeof(dist_bit_code));
    uint32_t pending = 0;
    uint32_t dist[LANES];
    for (uint32_t& d : dist)
        d = 1;
    size_t at = 0;
    bool eob = false;
    bool last = false;
    while (!last) {
        last = eob;
        for (int lane = 0; lane < (int)LANES; lane++) {
            uint32_t v;
            int nb;
            if ((pending >> lane) & 1) {
                uint32_t d = dist[lane];
                int ds = dist_symbol(d);
                v = dist_bit_code[ds] | ((d - dist_base[ds]) << dist_bits[ds]);
                nb = dist_bits[ds] + dist_extra[ds];
                pending &= ~(1u << lane);
            } else if (eob) {
                continue;
            } else if (at == count) {
                v = lit_bit_code[end_of_block];
                nb = lit_bits[end_of_block];
                eob = true;
            } else {
                const token_t k = tokens[at++];
                if (!k.len) {
                    v = lit_bit_code[k.val];
                    nb = lit_bits[k.val];
                } else {
                    int ls = length_symbol(k.len);
                    int bits = lit_bits[257 + ls];
                    v = lit_bit_code[257 + ls] | ((k.len - length_base[ls]) << bits);
                    nb = bits + length_extra[ls];
                    pending |= 1u << lane;
                    dist[lane] = k.val;
                }
            }
            uint64_t a = acc[lane] | ((uint64_t)v << fill[lane]);
            int f = fill[lane] + nb;
            if (f >= 32) {
                data[slot[lane]] = (uint32_t)a;
                slot[lane] = next_slot[lane];
                has &= ~(1u << lane);
                a >>= 32;
                f -= 32;
            }
            if (!((has >> lane) & 1) && f > 0) {
                next_slot[lane] = next++;
                has |= 1u << lane;
            }
            acc[lane] = a;
            fill[lane] = f;
        }
    }
    for (int lane = 0; lane < (int)LANES; lane++) {
        lane_out_t& l = t.lanes[lane];
        l.acc = acc[lane];
        l.fill = fill[lane];
        l.slot = slot[lane];
        l.next_slot = next_slot[lane];
        l.has_next = ((has >> lane) & 1) != 0;
    }
    t.next = next;
}

struct entropy_table_t {
    double v[4096];

    entropy_table_t() {
        v[0] = 0;
        for (int i = 1; i < 4096; i++)
            v[i] = i * std::log2((double)i);
    }
};

static const entropy_table_t entropy_table;

static double weighted_log(uint32_t f) {
    return f < 4096 ? entropy_table.v[f] : f * std::log2((double)f);
}

static uint64_t hist_bits(const hist_t& h, const hist_t* minus) {
    uint32_t lit_total = 1, dist_total = 0, used = 0;
    double sum = 0;
    for (int i = 0; i < 286; i++) {
        uint32_t f = h.lit[i] - (minus ? minus->lit[i] : 0) + (i == end_of_block ? 1 : 0);
        lit_total += f;
        used += f != 0;
        sum += weighted_log(f);
    }
    for (int i = 0; i < dist_syms; i++) {
        uint32_t f = h.dist[i] - (minus ? minus->dist[i] : 0);
        dist_total += f;
        used += f != 0;
        sum += weighted_log(f);
    }
    double bits = weighted_log(lit_total - 1) + weighted_log(dist_total) - sum;
    return (uint64_t)bits + 80 + 5 * (uint64_t)used + h.extra - (minus ? minus->extra : 0);
}

static uint64_t exact_bits(const hist_t& h, const hist_t* minus) {
    uint32_t lit[286], dist[dist_syms];
    uint8_t lit_lens[286], dist_lens[dist_syms];
    for (int i = 0; i < 286; i++)
        lit[i] = h.lit[i] - (minus ? minus->lit[i] : 0);
    for (int i = 0; i < dist_syms; i++)
        dist[i] = h.dist[i] - (minus ? minus->dist[i] : 0);
    lit[end_of_block]++;
    code_lengths(lit, 286, max_code_len, lit_lens);
    code_lengths(dist, dist_syms, max_code_len, dist_lens);
    uint64_t bits = 80 + h.extra - (minus ? minus->extra : 0);
    for (int i = 0; i < 286; i++)
        bits += (uint64_t)lit[i] * lit_lens[i] + (lit_lens[i] ? 4 : 0);
    for (int i = 0; i < dist_syms; i++)
        bits += (uint64_t)dist[i] * dist_lens[i] + (dist_lens[i] ? 4 : 0);
    return bits;
}

static void split_blocks(const marks_t& m, size_t from, size_t to, int depth, bool exact, std::vector<size_t>& cuts,
                         std::vector<hist_t>& hists) {
    constexpr uint32_t smallest = 256;
    uint64_t best = UINT64_MAX;
    size_t best_at = 0;
    for (size_t j = from + 1; depth > 0 && j < to; j++) {
        if (m.at[j] - m.at[from] < smallest || m.at[to] - m.at[j] < smallest)
            continue;
        uint64_t bits = exact ? exact_bits(m.hist[j], &m.hist[from]) + exact_bits(m.hist[to], &m.hist[j])
                              : hist_bits(m.hist[j], &m.hist[from]) + hist_bits(m.hist[to], &m.hist[j]);
        if (bits < best) {
            best = bits;
            best_at = j;
        }
    }
    if (!best_at || exact_bits(m.hist[best_at], &m.hist[from]) + exact_bits(m.hist[to], &m.hist[best_at]) + 48 >=
                        exact_bits(m.hist[to], &m.hist[from])) {
        hist_t h;
        for (int i = 0; i < 286; i++)
            h.lit[i] = m.hist[to].lit[i] - m.hist[from].lit[i];
        for (int i = 0; i < dist_syms; i++)
            h.dist[i] = m.hist[to].dist[i] - m.hist[from].dist[i];
        h.extra = m.hist[to].extra - m.hist[from].extra;
        hists.push_back(h);
        return;
    }
    split_blocks(m, from, best_at, depth - 1, exact, cuts, hists);
    cuts.push_back(m.at[best_at]);
    split_blocks(m, best_at, to, depth - 1, exact, cuts, hists);
}

static size_t encode_tile(chains_t& c, const uint8_t* p, size_t n, int level, const uint32_t*& words) {
    const token_t* tokens = nullptr;
    size_t count = 0;
    hist_t all;
    marks_t& marks = c.marks;
    marks.hist.clear();
    marks.at.clear();
    marks.hist.push_back(all);
    marks.at.push_back(0);
    std::vector<size_t> cuts;
    std::vector<hist_t> hists;
    if (level >= 8) {
        best_tune_t tune = best_tune(level);
        c.tokens.clear();
        best_tokens(c, p, n, level, c.tokens);
        tokens = c.tokens.data();
        count = c.tokens.size();
        size_t step = count / ((size_t)tune.split_points + 1) + 1;
        size_t next = step;
        for (size_t i = 0; i < count; i++) {
            if (i == next) {
                marks.hist.push_back(all);
                marks.at.push_back((uint32_t)i);
                next += step;
            }
            all.add(tokens[i]);
        }
        marks.hist.push_back(all);
        marks.at.push_back((uint32_t)count);
        split_blocks(marks, 0, marks.at.size() - 1, tune.split_depth, tune.exact_split != 0, cuts, hists);
    } else {
        static const int depths[8] = {1, 1, 1, 1, 1, 2, 2, 3};
        static const size_t points[8] = {7, 7, 7, 7, 7, 15, 15, 15};
        int at = level < 1 ? 1 : level;
        count = find_tokens(c, p, n, level, all, &marks, points[at]);
        tokens = c.store.data();
        marks.hist.push_back(all);
        marks.at.push_back((uint32_t)count);
        split_blocks(marks, 0, marks.at.size() - 1, depths[at], false, cuts, hists);
    }
    cuts.push_back(count);
    tile_enc_t t(c.packets);
    block_t b;
    size_t from = 0;
    for (size_t i = 0; i < cuts.size(); i++) {
        plan_block(hists[i], b);
        write_block(t, tokens + from, cuts[i] - from, b, i + 1 == cuts.size());
        from = cuts[i];
    }
    finish(t);
    words = t.data;
    if ((size_t)t.next * 4 <= n + 136)
        return t.next;
    tile_enc_t raw(c.raw_packets);
    for (size_t at = 0; at < n;) {
        size_t len = n - at > 65535 ? 65535 : n - at;
        put(raw, 0, at + len == n ? 1 : 0, 1);
        put(raw, 0, 0, 2);
        claim(raw, 0);
        put(raw, 0, (uint32_t)len, 16);
        claim(raw, 0);
        for (size_t i = 0; i < len; i++) {
            put(raw, (int)(i % LANES), p[at + i], 8);
            claim(raw, (int)(i % LANES));
        }
        at += len;
    }
    finish(raw);
    if (raw.next >= t.next)
        return t.next;
    words = raw.data;
    return raw.next;
}

template <typename sink_t>
static void encode_tiles(const uint8_t* p, size_t n, int level, unsigned threads, sink_t sink) {
    size_t num_tiles = (n + TILE_SIZE - 1) / TILE_SIZE;
    std::atomic<size_t> next{0};
    auto work = [&] {
        std::vector<chains_t> c(1);
        for (size_t i = next++; i < num_tiles; i = next++) {
            const uint32_t* words = nullptr;
            size_t count = encode_tile(c[0], p + i * TILE_SIZE, i + 1 < num_tiles ? TILE_SIZE : n - i * TILE_SIZE, level, words);
            sink(i, words, count);
        }
    };
    if (!threads)
        threads = std::thread::hardware_concurrency();
    if (threads > num_tiles)
        threads = (unsigned)num_tiles;
    std::vector<std::thread> pool;
    for (unsigned i = 1; i < threads; i++)
        pool.emplace_back(work);
    work();
    for (std::thread& th : pool)
        th.join();
}

static void write_head(uint8_t* out, size_t n, size_t num_tiles) {
    out[0] = stream_id;
    out[1] = stream_id ^ 0xff;
    out[2] = (uint8_t)num_tiles;
    out[3] = (uint8_t)(num_tiles >> 8);
    uint32_t word = 1 | ((uint32_t)(n % TILE_SIZE) << 2);
    for (int b = 0; b < 4; b++)
        out[4 + b] = (uint8_t)(word >> (8 * b));
}

size_t compress_bound(size_t n) {
    size_t num_tiles = (n + TILE_SIZE - 1) / TILE_SIZE;
    return HEADER_SIZE + n + num_tiles * 400;
}

bool compress_into(const uint8_t* p, size_t n, uint8_t* out, size_t capacity, size_t& written, std::string& err, int level,
                   unsigned threads) {
    constexpr size_t stride = TILE_SIZE + 264;
    size_t num_tiles = (n + TILE_SIZE - 1) / TILE_SIZE;
    if (num_tiles > MAX_TILES) {
        err = "input above 65535 tiles of 64 KB";
        return false;
    }
    if (capacity < compress_bound(n)) {
        err = "output buffer below compress_bound()";
        return false;
    }
    size_t table = HEADER_SIZE + 4 * num_tiles;
    std::vector<uint32_t> sizes(num_tiles);
    encode_tiles(p, n, level, threads, [&](size_t i, const uint32_t* words, size_t count) {
        store_words(out + table + i * stride, words, count);
        sizes[i] = (uint32_t)(count * 4);
    });
    write_head(out, n, num_tiles);
    size_t at = table;
    for (size_t i = 0; i < num_tiles; i++) {
        if (at - table > UINT32_MAX) {
            err = "compressed tiles above 4 GB";
            return false;
        }
        uint32_t entry = (uint32_t)(i ? at - table : sizes[num_tiles - 1]);
        for (int b = 0; b < 4; b++)
            out[HEADER_SIZE + 4 * i + b] = (uint8_t)(entry >> (8 * b));
        if (i)
            memmove(out + at, out + table + i * stride, sizes[i]);
        at += sizes[i];
    }
    written = at;
    return true;
}

bool compress(const uint8_t* p, size_t n, std::vector<uint8_t>& out, std::string& err, int level, unsigned threads) {
    size_t num_tiles = (n + TILE_SIZE - 1) / TILE_SIZE;
    if (num_tiles > MAX_TILES) {
        err = "input above 65535 tiles of 64 KB";
        return false;
    }
    std::vector<std::vector<uint8_t>> tiles(num_tiles);
    encode_tiles(p, n, level, threads, [&](size_t i, const uint32_t* words, size_t count) {
        tiles[i].resize(count * 4);
        store_words(tiles[i].data(), words, count);
    });
    size_t table = HEADER_SIZE + 4 * num_tiles;
    size_t total = table;
    for (const std::vector<uint8_t>& t : tiles)
        total += t.size();
    if (total - table > UINT32_MAX) {
        err = "compressed tiles above 4 GB";
        return false;
    }
    out.clear();
    out.reserve(total);
    out.resize(table);
    write_head(out.data(), n, num_tiles);
    size_t at = table;
    for (size_t i = 0; i < num_tiles; i++) {
        uint32_t entry = (uint32_t)(i ? at - table : tiles.back().size());
        for (int b = 0; b < 4; b++)
            out[HEADER_SIZE + 4 * i + b] = (uint8_t)(entry >> (8 * b));
        at += tiles[i].size();
    }
    for (size_t i = 0; i < num_tiles; i++)
        out.insert(out.end(), tiles[i].begin(), tiles[i].end());
    return true;
}

}
