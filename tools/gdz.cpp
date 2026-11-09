// gdz - decode, validate and benchmark GDeflate streams on the CPU and on the GPU
//
//   gdz <file.gdz> [-o out] [-ref original] [-gpu] [-threads N] [-rounds N] [-wave32] [-wave64]
//   gdz -list <manifest> [-gpu] [-threads N] [-rounds N]    manifest: one "<file.gdz> <original>" per line
//   gdz -fuzz <file.gdz> [-n N] [-seed S] [-gpu]              flips bytes and truncates, expects clean errors
//
// Exit code 0 only when every decode matched.
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
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "gdeflate.h"
#include "vkdeflate.h"

namespace {

struct args_t {
    std::string file;
    std::string out;
    std::string ref;
    std::string list;
    std::string fuzz;
    bool info = false;
    bool gpu = false;
    bool cpu = true;
    int threads = 1;
    int rounds = 1;
    int warm = 0;
    int fuzz_n = 500;
    uint32_t seed = 1;
    uint32_t subgroup = 0;
    uint32_t experiment = 0;
    uint32_t ring = 512;
    uint32_t pad = 0;
    uint32_t root = 9;
    uint32_t droot = 7;
    bool verbose = false;
};

static bool load(const std::string& path, std::vector<uint8_t>& d) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f)
        return false;
    d.resize((size_t)f.tellg());
    f.seekg(0);
    f.read((char*)d.data(), (std::streamsize)d.size());
    return true;
}

static bool save(const std::string& path, const uint8_t* p, size_t n) {
    std::ofstream f(path, std::ios::binary);
    if (!f)
        return false;
    f.write((const char*)p, (std::streamsize)n);
    return true;
}

static double ms_since(std::chrono::steady_clock::time_point a) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count();
}

static double mbs(size_t bytes, double ms) {
    return ms > 0 ? (double)bytes / 1e6 / (ms / 1e3) : 0;
}

static size_t first_diff(const uint8_t* a, const uint8_t* b, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (a[i] != b[i])
            return i;
    return n;
}

static bool cpu_decode(const std::vector<uint8_t>& in, const gdeflate::stream_t& s, uint8_t* out, int threads,
                       std::string& err) {
    if (threads <= 1)
        return gdeflate::decompress(in.data(), in.size(), out, s.total, err);
    std::atomic<size_t> next(0);
    std::atomic<bool> failed(false);
    std::string errs[64];
    int n = std::min(threads, 64);
    std::vector<std::thread> pool;
    for (int w = 0; w < n; w++)
        pool.emplace_back([&, w]() {
            for (;;) {
                size_t i = next.fetch_add(1);
                if (i >= s.tiles.size() || failed.load())
                    return;
                const gdeflate::tile_t& t = s.tiles[i];
                std::string why;
                if (!gdeflate::decode_tile(in.data() + t.src, t.csize, out + t.dst, t.size, why)) {
                    errs[w] = "tile " + std::to_string(i) + ": " + why;
                    failed = true;
                    return;
                }
            }
        });
    for (auto& t : pool)
        t.join();
    for (int w = 0; w < n; w++)
        if (!errs[w].empty())
            err = errs[w];
    return !failed.load();
}

struct result_t {
    bool ok = false;
    double cpu_ms = 0;
    double gpu_ms = 0;
    size_t size = 0;
    vkdeflate::timing_t gpu;
};

static bool check_one(const args_t& a, const std::string& path, const std::string& ref_path, vkdeflate::ctx_t* gpu,
                      result_t& r) {
    std::vector<uint8_t> in;
    if (!load(path, in)) {
        printf("[gdz] cant open %s\n", path.c_str());
        return false;
    }
    gdeflate::stream_t s;
    std::string err;
    if (!gdeflate::parse(in.data(), in.size(), s, err)) {
        printf("[gdz] %s: %s\n", path.c_str(), err.c_str());
        return false;
    }
    size_t csize = 0;
    for (const auto& t : s.tiles)
        csize += t.csize;
    printf("[gdz] %s: %zu bytes, %zu tiles, %zu bytes of output (ratio %.3f)\n", path.c_str(), in.size(),
           s.tiles.size(), s.total, s.total ? (double)in.size() / (double)s.total : 0.0);
    r.size = s.total;

    std::vector<uint8_t> ref;
    if (!ref_path.empty()) {
        if (!load(ref_path, ref)) {
            printf("[gdz] cant open %s\n", ref_path.c_str());
            return false;
        }
        if (ref.size() != s.total) {
            printf("[gdz] FAIL: stream says %zu bytes, %s has %zu\n", s.total, ref_path.c_str(), ref.size());
            return false;
        }
    }

    if (a.info) {
        gdeflate::stats_t st;
        std::vector<uint8_t> tmp(gdeflate::TILE_SIZE);
        size_t max_rounds = 0;
        for (const auto& t : s.tiles) {
            gdeflate::stats_t one;
            if (!gdeflate::decode_tile(in.data() + t.src, t.csize, tmp.data(), t.size, err, &one))
                break;
            max_rounds = std::max(max_rounds, one.rounds);
            st.blocks += one.blocks;
            st.stored += one.stored;
            st.fixed += one.fixed;
            st.dynamic += one.dynamic;
            st.literals += one.literals;
            st.matches += one.matches;
            st.match_bytes += one.match_bytes;
            st.max_match = std::max(st.max_match, one.max_match);
            st.long_matches += one.long_matches;
            st.rounds += one.rounds;
            st.long_codes += one.long_codes;
            st.dep_matches += one.dep_matches;
            st.dep_rounds += one.dep_rounds;
            st.lit_rounds += one.lit_rounds;
            for (int b = 0; b < 8; b++)
                st.far_hist[b] += one.far_hist[b];
            for (int b = 0; b < 5; b++) {
                st.dist_hist[b] += one.dist_hist[b];
                st.long_dist_hist[b] += one.long_dist_hist[b];
            }
            for (int b = 0; b < 16; b++) {
                st.len_hist[b] += one.len_hist[b];
                st.dlen_hist[b] += one.dlen_hist[b];
            }
            for (int b = 0; b < 3; b++)
                st.slow_rounds[b] += one.slow_rounds[b];
        }
        printf("[gdz] blocks %zu (%zu stored, %zu fixed, %zu dynamic), %.1f per tile; literals %zu, matches %zu "
               "(%.1f bytes on average, %zu longer than 32, longest %zu); rounds %zu (%.0f per tile, %zu in the longest "
               "tile, %.1f bytes per round); codes longer than 10 bits: %zu (%.2f%% of symbols)\n",
               st.blocks, st.stored, st.fixed, st.dynamic, (double)st.blocks / (double)s.tiles.size(), st.literals,
               st.matches, st.matches ? (double)st.match_bytes / (double)st.matches : 0.0, st.long_matches, st.max_match,
               st.rounds, (double)st.rounds / (double)s.tiles.size(), max_rounds,
               st.rounds ? (double)s.total / (double)st.rounds : 0.0, st.long_codes,
               100.0 * (double)st.long_codes / (double)(st.literals + 2 * st.matches + 1));
        printf("[gdz] distances: 1: %zu, 2-4: %zu, 5-32: %zu, 33-128: %zu, >128: %zu; of matches longer than 32: %zu, %zu, "
               "%zu, %zu, %zu\n",
               st.dist_hist[0], st.dist_hist[1], st.dist_hist[2], st.dist_hist[3], st.dist_hist[4], st.long_dist_hist[0],
               st.long_dist_hist[1], st.long_dist_hist[2], st.long_dist_hist[3], st.long_dist_hist[4]);
        printf("[gdz] dependent matches %zu (%.1f%% of matches) in %zu rounds (%.1f%% of rounds)\n", st.dep_matches,
               st.matches ? 100.0 * (double)st.dep_matches / (double)st.matches : 0.0, st.dep_rounds,
               st.rounds ? 100.0 * (double)st.dep_rounds / (double)st.rounds : 0.0);
        printf("[gdz] rounds where all 32 lanes decode a literal: %.1f%%; match distance up to 512, 1k, 2k, 4k, 8k, 16k, "
               "32k, 64k:", st.rounds ? 100.0 * (double)st.lit_rounds / (double)st.rounds : 0.0);
        for (int b = 0; b < 8; b++)
            printf(" %.0f%%", st.matches ? 100.0 * (double)st.far_hist[b] / (double)st.matches : 0.0);
        printf("\n");
        size_t lit_all = 0, lit9 = 0, lit10 = 0, dist_all = 0, dist7 = 0, dist8 = 0;
        for (int b = 1; b < 16; b++) {
            lit_all += st.len_hist[b];
            lit9 += b > 9 ? st.len_hist[b] : 0;
            lit10 += b > 10 ? st.len_hist[b] : 0;
            dist_all += st.dlen_hist[b];
            dist7 += b > 7 ? st.dlen_hist[b] : 0;
            dist8 += b > 8 ? st.dlen_hist[b] : 0;
        }
        printf("[gdz] lit/len codes longer than 9 bits %.2f%%, than 10 bits %.2f%%; distance codes longer than 7 bits %.2f%%, "
               "than 8 bits %.2f%%; rounds with a slow lane at roots 10/8 %.1f%%, 9/8 %.1f%%, 9/7 %.1f%%\n",
               100.0 * (double)lit9 / (double)(lit_all + 1), 100.0 * (double)lit10 / (double)(lit_all + 1),
               100.0 * (double)dist7 / (double)(dist_all + 1), 100.0 * (double)dist8 / (double)(dist_all + 1),
               100.0 * (double)st.slow_rounds[0] / (double)(st.rounds + 1),
               100.0 * (double)st.slow_rounds[1] / (double)(st.rounds + 1),
               100.0 * (double)st.slow_rounds[2] / (double)(st.rounds + 1));
    }

    std::vector<uint8_t> cpu_out;
    bool ok = true;
    if (a.cpu) {
        cpu_out.assign(s.total, 0);
        r.cpu_ms = 1e30;
        for (int i = 0; i < a.rounds; i++) {
            auto t0 = std::chrono::steady_clock::now();
            if (!cpu_decode(in, s, cpu_out.data(), a.threads, err)) {
                printf("[gdz] FAIL cpu: %s\n", err.c_str());
                return false;
            }
            r.cpu_ms = std::min(r.cpu_ms, ms_since(t0));
        }
        printf("[gdz] cpu %d thread%s: %.2f ms = %.0f MB/s%s\n", a.threads, a.threads == 1 ? "" : "s", r.cpu_ms,
               mbs(s.total, r.cpu_ms), a.rounds > 1 ? " (best round)" : "");
        if (!ref.empty()) {
            size_t d = first_diff(cpu_out.data(), ref.data(), s.total);
            if (d != s.total) {
                printf("[gdz] FAIL cpu: first differing byte %zu of %zu (tile %zu)\n", d, s.total, d / gdeflate::TILE_SIZE);
                ok = false;
            } else {
                printf("[gdz] cpu output matches %s byte for byte (%zu bytes)\n", ref_path.c_str(), s.total);
            }
        }
        if (!a.out.empty() && !save(a.out, cpu_out.data(), s.total))
            printf("[gdz] cant write %s\n", a.out.c_str());
    }

    if (gpu) {
        std::vector<uint8_t> gpu_out(s.total);
        r.gpu_ms = 1e30;
        for (int i = -a.warm; i < a.rounds; i++) {
            vkdeflate::timing_t t;
            if (!vkdeflate::decompress(gpu, in.data(), in.size(), gpu_out.data(), s.total, err, &t)) {
                printf("[gdz] FAIL gpu: %s\n", err.c_str());
                return false;
            }
            if (i < 0)
                continue;
            if (a.verbose)
                printf("[gdz] round %d: kernel %.2f ms\n", i, t.kernel_ms);
            if (t.total_ms < r.gpu_ms) {
                r.gpu_ms = t.total_ms;
                r.gpu = t;
            }
        }
        const vkdeflate::timing_t& t = r.gpu;
        printf("[gdz] gpu kernel %.2f ms = %.0f MB/s; upload %.1f MB %.2f ms, download %.2f ms, host memcpy in %.2f "
               "out %.2f ms, submit to fence %.2f ms; end to end %.2f ms = %.0f MB/s%s\n",
               t.kernel_ms, mbs(s.total, t.kernel_ms), t.upload_bytes / 1e6, t.gpu_up_ms, t.gpu_down_ms, t.host_up_ms,
               t.host_down_ms, t.submit_ms, t.total_ms, mbs(s.total, t.total_ms), a.rounds > 1 ? " (best round)" : "");
        const uint8_t* truth = !ref.empty() ? ref.data() : !cpu_out.empty() ? cpu_out.data() : nullptr;
        const char* truth_name = !ref.empty() ? ref_path.c_str() : "the cpu decoder";
        if (truth) {
            size_t d = first_diff(gpu_out.data(), truth, s.total);
            if (d != s.total) {
                printf("[gdz] FAIL gpu: first differing byte %zu of %zu (tile %zu)\n", d, s.total, d / gdeflate::TILE_SIZE);
                ok = false;
            } else {
                printf("[gdz] gpu output matches %s byte for byte (%zu bytes)\n", truth_name, s.total);
            }
        }
        if (!a.cpu && !a.out.empty() && !save(a.out, gpu_out.data(), s.total))
            printf("[gdz] cant write %s\n", a.out.c_str());
    }
    r.ok = ok;
    return ok;
}

static uint32_t xorshift(uint32_t& s) {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}

static bool fuzz(const args_t& a, vkdeflate::ctx_t* gpu) {
    std::vector<uint8_t> in;
    if (!load(a.fuzz, in)) {
        printf("[gdz] cant open %s\n", a.fuzz.c_str());
        return false;
    }
    gdeflate::stream_t s;
    std::string err;
    if (!gdeflate::parse(in.data(), in.size(), s, err)) {
        printf("[gdz] %s: %s\n", a.fuzz.c_str(), err.c_str());
        return false;
    }
    std::vector<uint8_t> good(s.total);
    if (!gdeflate::decompress(in.data(), in.size(), good.data(), s.total, err)) {
        printf("[gdz] %s does not decode cleanly: %s\n", a.fuzz.c_str(), err.c_str());
        return false;
    }
    uint32_t seed = a.seed ? a.seed : 1;
    int rejected = 0, accepted = 0, same = 0, gpu_disagree = 0;
    std::vector<uint8_t> bad, out(s.total), gout(s.total);
    for (int i = 0; i < a.fuzz_n; i++) {
        bad = in;
        int kind = i % 4;
        if (kind == 0) {
            bad.resize(xorshift(seed) % bad.size());
        } else if (kind == 1) {
            size_t at = xorshift(seed) % bad.size();
            bad[at] ^= (uint8_t)(1u << (xorshift(seed) & 7));
        } else if (kind == 2) {
            size_t at = xorshift(seed) % bad.size();
            bad[at] = (uint8_t)xorshift(seed);
        } else {
            size_t at = xorshift(seed) % bad.size();
            size_t n = std::min<size_t>(bad.size() - at, 1 + xorshift(seed) % 64);
            for (size_t k = 0; k < n; k++)
                bad[at + k] = (uint8_t)xorshift(seed);
        }
        gdeflate::stream_t bs;
        bool parsed = gdeflate::parse(bad.data(), bad.size(), bs, err);
        bool ok = false;
        if (parsed) {
            out.assign(bs.total, 0);
            ok = gdeflate::decompress(bad.data(), bad.size(), out.data(), bs.total, err);
        }
        if (!ok) {
            rejected++;
        } else {
            accepted++;
            if (bs.total == s.total && first_diff(out.data(), good.data(), s.total) == s.total)
                same++;
        }
        if (gpu && parsed) {
            gout.assign(bs.total, 0);
            std::string gerr;
            bool gok = vkdeflate::decompress(gpu, bad.data(), bad.size(), gout.data(), bs.total, gerr);
            if (gok != ok || (ok && first_diff(out.data(), gout.data(), bs.total) != bs.total))
                gpu_disagree++;
        }
    }
    printf("[gdz] fuzz %s: %d broken copies, %d rejected, %d accepted (%d of them decoded to the original bytes)%s\n",
           a.fuzz.c_str(), a.fuzz_n, rejected, accepted, same, gpu ? "" : "");
    if (gpu)
        printf("[gdz] gpu disagreed with the cpu on %d of them\n", gpu_disagree);
    return gpu_disagree == 0;
}

}

int main(int argc, char** argv) {
    args_t a;
    for (int i = 1; i < argc; i++) {
        std::string s = argv[i];
        bool more = i + 1 < argc;
        if (s == "-o" && more)
            a.out = argv[++i];
        else if (s == "-ref" && more)
            a.ref = argv[++i];
        else if (s == "-list" && more)
            a.list = argv[++i];
        else if (s == "-fuzz" && more)
            a.fuzz = argv[++i];
        else if (s == "-gpu")
            a.gpu = true;
        else if (s == "-nocpu")
            a.cpu = false;
        else if (s == "-threads" && more)
            a.threads = atoi(argv[++i]);
        else if (s == "-rounds" && more)
            a.rounds = std::max(1, atoi(argv[++i]));
        else if (s == "-warm" && more)
            a.warm = std::max(0, atoi(argv[++i]));
        else if (s == "-n" && more)
            a.fuzz_n = atoi(argv[++i]);
        else if (s == "-seed" && more)
            a.seed = (uint32_t)atoi(argv[++i]);
        else if (s == "-wave32")
            a.subgroup = 32;
        else if (s == "-wave64")
            a.subgroup = 64;
        else if (s == "-v")
            a.verbose = true;
        else if (s == "-info")
            a.info = true;
        else if (s == "-x" && more)
            a.experiment = (uint32_t)atoi(argv[++i]);
        else if (s == "-ring" && more)
            a.ring = (uint32_t)atoi(argv[++i]);
        else if (s == "-pad" && more)
            a.pad = (uint32_t)atoi(argv[++i]);
        else if (s == "-root" && more)
            a.root = (uint32_t)atoi(argv[++i]);
        else if (s == "-droot" && more)
            a.droot = (uint32_t)atoi(argv[++i]);
        else if (s == "-ringcheck" && more)
            gdeflate::g_ring_check = atoi(argv[++i]);
        else if (s == "-trace" && more)
            gdeflate::g_trace = (size_t)atoll(argv[++i]);
        else if (s[0] == '-' || !a.file.empty()) {
            printf("unknown argument %s\n", argv[i]);
            return 1;
        } else
            a.file = s;
    }
    if (a.file.empty() && a.list.empty() && a.fuzz.empty()) {
        printf("usage: gdz <file.gdz> [-o out] [-ref original] [-gpu] [-nocpu] [-threads N] [-rounds N] [-wave32|-wave64]\n"
               "       gdz -list <manifest> [-gpu] [-threads N] [-rounds N]\n"
               "       gdz -fuzz <file.gdz> [-n N] [-seed S] [-gpu]\n");
        return 1;
    }

    vkdeflate::ctx_t* gpu = nullptr;
    if (a.gpu) {
        vkdeflate::config_t cfg;
        cfg.subgroup_size = a.subgroup;
        cfg.verbose = a.verbose;
        cfg.experiment = a.experiment;
        cfg.ring_bytes = a.ring;
        cfg.pad_bytes = a.pad;
        cfg.lit_root = std::min(std::max(a.root, 6u), 11u);
        cfg.dist_root = std::min(std::max(a.droot, 7u), 10u);
        std::string err;
        gpu = vkdeflate::create(cfg, err);
        if (!gpu) {
            printf("[gdz] no gpu: %s\n", err.c_str());
            return 2;
        }
        printf("[gdz] gpu: %s%s\n", vkdeflate::device_name(gpu),
               a.subgroup ? (a.subgroup == 32 ? ", wave32" : ", wave64") : "");
    }

    bool all_ok = true;
    if (!a.fuzz.empty()) {
        all_ok = fuzz(a, gpu);
    } else if (!a.list.empty()) {
        std::ifstream f(a.list);
        if (!f) {
            printf("[gdz] cant open %s\n", a.list.c_str());
            return 1;
        }
        std::string line;
        int files = 0, failed = 0;
        size_t bytes = 0;
        double cpu_ms = 0, gpu_ms = 0, kernel_ms = 0;
        while (std::getline(f, line)) {
            std::istringstream ls(line);
            std::string gdz, orig;
            ls >> gdz >> orig;
            if (gdz.empty() || gdz[0] == '#')
                continue;
            result_t r;
            files++;
            if (!check_one(a, gdz, orig, gpu, r)) {
                failed++;
                all_ok = false;
                continue;
            }
            cpu_ms += r.cpu_ms;
            gpu_ms += r.gpu_ms;
            kernel_ms += r.gpu.kernel_ms;
            bytes += r.size;
        }
        printf("[gdz] %d files, %d failed, %.1f MB", files, failed, bytes / 1e6);
        if (a.cpu)
            printf(", cpu %.1f ms total (%.0f MB/s)", cpu_ms, mbs(bytes, cpu_ms));
        if (gpu)
            printf(", gpu kernel %.1f ms (%.0f MB/s), end to end %.1f ms (%.0f MB/s)", kernel_ms, mbs(bytes, kernel_ms),
                   gpu_ms, mbs(bytes, gpu_ms));
        printf("\n");
    } else {
        result_t r;
        all_ok = check_one(a, a.file, a.ref, gpu, r);
    }

    if (gpu)
        vkdeflate::destroy(gpu);
    return all_ok ? 0 : 1;
}
