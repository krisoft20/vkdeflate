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

#include <cstdio>
#include <cstring>

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
int g_pol[8][4] = {{256, 1024, 4096, 1}, {256, 2048, 8192, 1}, {128, 1024, 4096, 1}, {256, 512, 4096, 1},
                   {512, 2048, 8192, 1}, {128, 2048, 8192, 1}, {256, 4096, 16384, 1}, {64, 1024, 4096, 1}};

struct ring_t {
    std::vector<uint8_t> slot;
    std::vector<int32_t> mark;
    std::vector<std::pair<size_t, uint8_t>> lits;
    size_t from = 0;
    size_t win = 0;
    size_t round = 0;
    bool reported = false;
    size_t pol_until[8] = {};
    size_t pol_limit[8] = {};
    bool pol_mirror[8] = {};
    size_t pol_phys[8] = {};
    size_t prev_end = 0;
    bool pol_was[8] = {};
    size_t block_round = 0;
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
        t.st->far_hist[d <= 512 ? 0 : d <= 1024 ? 1 : d <= 2048 ? 2 : d <= 4096 ? 3 : d <= 8192 ? 4 : d <= 16384 ? 5 : d <= 32768 ? 6 : 7]++;
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
    size_t win = pos_end >= 512 ? pos_end - 512 : 0;
    size_t lo = win > r.from ? win : r.from;
    uint32_t hits = 0;
    for (int k = 0; k < 8; k++) {
        if (!r.pol_mirror[k]) {
            r.pol_mirror[k] = r.prev_end < r.pol_limit[k];
            if (r.pol_mirror[k]) {
                r.pol_phys[k] = r.prev_end;
                if (t.st)
                    t.st->pol_tiles[k]++;
            }
        }
        if (r.pol_mirror[k] && t.st)
            t.st->pol_on[k]++;
    }
    r.block_round++;
    uint32_t longest = 0;
    for (const job_t& j : jobs)
        if (!j.dep && j.len <= LANES) {
            if (j.len > longest)
                longest = j.len;
            if (t.st)
                t.st->own_bytes += j.len;
            copy(t, j.at, j.len, j.d);
            for (uint32_t k = 0; k < j.len; k++)
                ring_write(r, j.at + k, t.out[j.at + k], 2);
        }
    if (t.st)
        t.st->own_steps += (longest + 3) / 4;
    for (const job_t& j : jobs)
        if (!j.dep && j.len > LANES) {
            copy(t, j.at, j.len, j.d);
            if (j.at + j.len > r.from)
                r.from = j.at + j.len;
            if (g_ring_check)
                for (uint32_t k = 0; k < j.len; k++)
                    r.mark[j.at + k] = -2;
        }
    size_t dep_end = 0;
    bool any_free = false, any_chain = false;
    for (const job_t& j : jobs) {
        if (!j.dep)
            continue;
        if (j.len <= LANES) {
            if (dep_end <= j.at - j.d) {
                any_free = true;
                if (t.st)
                    t.st->dep_free++;
            } else
                any_chain = true;
        }
        if (j.at + j.len > dep_end)
            dep_end = j.at + j.len;
        if (r.from > ring_lo)
            ring_lo = r.from;
        if (r.from > lo)
            lo = r.from;
        if (j.len <= LANES && j.at - j.d >= lo) {
            hits++;
            if (t.st) {
                t.st->ring_hits++;
                for (int k = 0; k < 8; k++)
                    t.st->pol_served[k] += r.pol_mirror[k] && j.at - j.d >= r.pol_phys[k];
            }
        }
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
    for (int k = 0; k < 8; k++) {
        if (!hits)
            continue;
        size_t e = (r.pol_until[k] > pos_end ? r.pol_until[k] : pos_end) + (size_t)g_pol[k][0] * hits;
        if (e > pos_end + (size_t)g_pol[k][2])
            e = pos_end + (size_t)g_pol[k][2];
        r.pol_until[k] = e;
        r.pol_limit[k] = e >= (size_t)g_pol[k][1] ? e - (size_t)g_pol[k][1] : 0;
    }
    if (t.st) {
        std::vector<const job_t*> left;
        for (const job_t& j : jobs)
            if (j.dep) {
                left.push_back(&j);
                t.st->dep_own += j.len <= 24 && j.d >= j.len;
            }
        for (int mode = 0; mode < 2; mode++) {
            std::vector<const job_t*> rem = left;
            while (!rem.empty()) {
                std::vector<const job_t*> next;
                size_t hwm = rem[0]->at;
                for (size_t a = 0; a < rem.size(); a++) {
                    const job_t* i = rem[a];
                    size_t s0 = i->at - i->d;
                    size_t s1 = i->d >= i->len ? s0 + i->len : i->at;
                    bool clash = false;
                    if (mode == 1)
                        clash = s1 > hwm;
                    else
                        for (size_t b = 0; b < a; b++)
                            if (rem[b]->at < s1 && rem[b]->at + rem[b]->len > s0)
                                clash = true;
                    if (clash)
                        next.push_back(i);
                }
                if (mode == 0)
                    t.st->mrr_exact++;
                else
                    t.st->mrr_hwm++;
                rem = next;
            }
        }
        t.st->dep_free_rounds += any_free;
        t.st->dep_chain_rounds += any_chain;
    }
    r.prev_end = pos_end;
    jobs.clear();
    return !t.fail;
}

static bool huffman_block(tile_dec_t& t, const huff_t& lit, const huff_t& dist) {
    bool pending[LANES] = {};
    t.ring.block_round = 0;
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
            any_pending |= pending[lane];
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
                    t.st->dep_short += match_len[lane] <= LANES;
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
        if (t.st && dep) {
            t.st->dep_matches++;
            t.st->dep_short += match_len[lane] <= LANES;
        }
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

}
