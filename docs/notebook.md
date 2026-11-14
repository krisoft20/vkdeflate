# Notebook

What was tried and what came out. AMD Radeon RX 6650 XT, driver 26.10.1, Ryzen 5 5600X, except the tables for the
RX 9070 XT. Kernel time in ms
on the GPU, best of several rounds, unless a row says otherwise. The machine was shared, so times move by 10 %
between sessions; comparisons inside one table were run interleaved.

## Bounds checks off

`config_t::bounds_checks = false` (spec constant 7) drops the per-round checks for bad symbols, distances, output
overrun and input overrun. The input index stays clamped. With checks on the ISA is byte-identical to before.
46 streams, best of 30, on and off alternating three times, all byte-exact:

| | checks on | checks off |
|---|---|---|
| instructions in the ISA | 9121 | 8941 |
| 46 streams, sum of kernel ms | 40.75 | 39.39 (-3.3 %) |
| per stream | | -6.7 to +9.3 %, median -3.8 % |
| twelve Silesia streams at level 6, median | | -3.4 % |

The level 0 streams got slower (sao +9.3 %, mr +5.7 %, x-ray +4.8 %).

## load(), file to device-local buffer

`bench -load`, twelve Silesia streams, 212 MB of output, best of ten, the old and the new library alternating three
times. Before, `record_load()` read each file into a vector, copied bytes given in memory into a vector too, then
copied both into the staging buffer. Now the header is read on its own for `describe()` and the file straight into
the mapped staging buffer; bytes in memory are copied once.

| sum, ms | before | after |
|---|---|---|
| `load()`, file just dropped from the OS cache | 120.4 | 106.3 |
| `load()`, cached file | 68.3 | 44.0 |
| `load()`, bytes in memory | 56.1 | 32.1 |
| `record_load()` on the host, file dropped from the cache | 74.4 | 62.1 |
| `record_load()` on the host, cached file | 28.2 | 12.6 |
| `record_load()` on the host, bytes in memory | 18.4 | 4.7 |
| GPU time of what `record_load()` records | 33.9 | 29.5 |

mozilla, cached file 15.4 to 8.6 ms, bytes in memory 12.0 to 5.9. xml (82 tiles) stays at 1.9 ms, most of it GPU.

Of the GPU time, sum: kernel 11.6, upload of the 71 MB of compressed input 5.1, copy into the caller's buffer 2.1;
about 11 ms are not accounted for. The CPU decoder on 12 threads plus the upload of its output: 280 ms.

The 11 ms are the clock. `bench -idle-ms N` sleeps before each measured dispatch; after 1 ms of idle the kernel is
2.7 to 3.4 times slower, and 10 ms of idle is no worse than 1:

| kernel, ms | no idle | 1 ms | 3 ms | 10 ms |
|---|---|---|---|---|
| mozilla | 1.95 | 5.23 | 5.34 | 5.18 |
| x-ray | 1.06 | 3.55 | 3.58 | 3.61 |
| xml | 0.63 | 2.14 | 2.16 | 2.14 |

`load()` now cuts the tiles into chunks of 512 in file order (`config_t::load_chunk_tiles`, 0 for one chunk) and
fills the next chunk's half of a double staging buffer while the GPU decodes the last one. Best of ten, the two
settings alternating three times:

| ms | one chunk | 512 tiles |
|---|---|---|
| twelve streams in one `load()`, file dropped from the cache | 79.0 | 73.6 |
| the same, cached files | 29.9 | 22.7 |
| the same, bytes in memory | 18.3 | 17.2 |
| one stream per `load()`, sum, cached files | 42.3 | 42.4 |

A first version took the start of a chunk after sorting its tiles by size and left part of each chunk out of the
upload. The bench passed, because the device buffer still held the bytes of the previous call; `examples/load`
with two items of the same file failed. The bench now checks its output after `trim()`.

The drives: a Lexar NM620 1 TB (NVMe, PCIe 3.0 x4) and a Goodram CX400 512 GB (SATA). 86 vectors, 364 MB on the
NVMe drive, the twelve Silesia originals, 212 MB, on the SATA drive; `fread` in 4 MB pieces, files dropped from
the OS cache first, GB/s, best of three:

| threads | NVMe, cold | NVMe, cached | SATA, cold | SATA, cached |
|---|---|---|---|---|
| 1 | 0.97 to 1.22 | 5.4 to 6.1 | 0.47 to 0.48 | 6.4 to 7.1 |
| 2 | 1.56 to 1.76 | 8.3 to 10.4 | 0.54 to 0.55 | 8.7 to 10.2 |
| 4 | 1.85 to 1.95 | 5.9 to 9.7 | 0.54 | 6.8 to 7.3 |
| 8 | 1.96 to 1.97 | 7.9 to 8.3 | 0.54 | 6.0 to 8.1 |

Unbuffered reads (`FILE_FLAG_NO_BUFFERING`) stop at the same 2.0 to 2.1 GB/s on the NVMe drive, at any queue
depth from 8 up, and lose the cache.

`load()` now reads each chunk with `config_t::read_threads` threads, each with its own file handle and a part of
the chunk. The twelve Silesia streams on each drive, best of ten, thread counts alternating three times:

| ms | 1 thread | 2 | 4 |
|---|---|---|---|
| NVMe, twelve streams in one `load()`, dropped from the cache | 74.2 | 66.7 | 59.9 |
| NVMe, the same, cached | 22.7 | 20.1 | 19.4 |
| NVMe, one stream per `load()`, sum, dropped from the cache | 106.0 | 95.6 | 94.2 |
| SATA, twelve streams in one `load()`, dropped from the cache | 175.0 | 168.9 | 165.4 |
| SATA, the same, cached | 22.7 | 19.8 | 19.3 |

Bytes in memory do not change (17.1 to 17.2 ms). Four threads are no slower than two anywhere here, so 4 is the
default.

## RX 9070 XT, last package

A friend's machine, a package of six kernel variants, best of three rounds, variants alternating, twelve Silesia streams. Every output
byte-exact; 1200 broken copies fuzzed, CPU and GPU agree on all of them.

| against the current kernel (literal rounds on under LLPC) | median |
|---|---|
| literal rounds off | -1.1 %; reymont and samba +5 to +6 % |
| the same kernel again | -0.3 %; single streams up to 3.6 % apart |
| the votes of a round taken as ballots at its start | +2.7 % |
| one vote instead of two before the all-literal round | +5.6 % |
| both | +7.8 %; dickens, mr, sao, x-ray about +10 % |

`s_delay_alu` in the ISA: 1294, with both changes 1287. The gaps do not come from the votes.

One x-ray tile repeated, ms:

| tiles | 1 | 8 | 32 | 128 | 256 | 512 | 1024 | 2048 |
|---|---|---|---|---|---|---|---|---|
| RX 6650 XT | 1.00 | 1.00 | 1.00 | 1.05 | 1.19 | 1.70 | 4.02 | 5.99 |
| RX 9070 XT | 1.23 | 1.20 | 1.18 | 1.14 | 1.19 | 1.27 | 1.68 | 3.11 |

Flat up to 512 tiles there, so ten of the twelve Silesia streams take as long as their slowest tile. Full, the
RX 9070 XT does 1.9 times the work of the RX 6650 XT.

## Fourth pass

From the profiler's instruction timing on x-ray, 256 tiles: a round is about 435 instructions, 39 of them
branches. A branch or a scalar compare costs about ten plain instructions; waits for memory are 14 % of the time.
Each change against the previous one, `bench` interleaved, best of three, twelve Silesia streams.

| change | result | kept |
|---|---|---|
| short-copy loads and stores under nested tests instead of a chain of 13 | x-ray tile 1.15 to 1.09, sum -1.3 % | yes |
| the round's literal stored once, after the copies | sum -4.4 % | yes |
| ring store without a branch (index of a spare slot when outside the window) | sum -1.8 % | yes |
| short-copy loads issued before the refill and the prefix sum | sum -3.4 %, single tiles flat | yes |
| all-literal round tried in every block | +3 to +5 % | no |
| two literals per lane in one round | +2 to +4 % | no |
| literal stores removed (wrong output) | x-ray tile 1.15 to 1.11: stores are not the cost | |

| | before | after |
|---|---|---|
| 46 streams, sum of kernel ms | 45.61 | 40.68 |
| per stream | | -17.6 to 0 %, median -10.2 % |
| ratio to the Khronos layer, lowest, median, highest | 1.51, 2.12, 4.24 | 1.66, 2.33, 4.85 |
| vector registers | 70 | 71 |
| branches in the capture | 231 844 | 141 000 |

The tables under "Against the Khronos layer" are from the third pass.

RX 9070 XT, the same kernel, a friend's machine: the twelve Silesia streams 6 to 10 % faster than the third pass,
ratio to the layer 1.47 to 2.94, median 1.84. One tile, ms:

| tile | RX 6650 XT | RX 9070 XT |
|---|---|---|
| dickens | 0.52 | 0.64 |
| x-ray | 0.99 | 1.22 |
| mozilla | 0.52 | 0.64 |
| webster | 0.56 | 0.73 |

Time is flat up to about 256 tiles there and shared-memory padding up to 2048 bytes costs nothing, so the card is
not full. From the instruction timing of x-ray, 256 tiles, per round:

| | RX 6650 XT | RX 9070 XT |
|---|---|---|
| clock cycles | 2342 | 2730 |
| vector and scalar instructions | 361 | 385 |
| instructions followed by a gap of 4 cycles or more | 154 | 209 |
| time in memory waits over 80 cycles | 16 % | 0 % |

Where the 2705 cycles of a round go on the RX 9070 XT: 45 % in gaps of 4 cycles or more with no dependency marker
before them (scalar mask logic, compares that write a mask, `v_readfirstlane`, selects), 25 % in such gaps right
after an `s_delay_alu` marker, 18 % in waits and branches, 12 % in the 359 instructions under 4 cycles.
The RX 6650 XT column is from the capture before the last change of this pass.

## Third pass

`words[7]` and `pat_t q` initialised where declared: LLPC carried them through every loop.

| | before | after |
|---|---|---|
| vector registers, RX 6650 XT | 93 | 70 |
| vector registers, RX 9070 XT | 190 | 94 |
| kernel time, RX 6650 XT | | -4 to +1 % |
| kernel time, RX 9070 XT | | -4 to +5 %, median +1 % |

| tried | result | kept |
|---|---|---|
| long-code limits in shared memory | 64 and 79 registers, +1 to +4 % | no |
| 8-bit literal root | up to +41 % | no |
| output ring 256 bytes | up to +12 % | no |

## Second pass

Each change against the previous kept one, separate builds, `bench` interleaved, three rounds.

| change | result | kept |
|---|---|---|
| own-lane copy loop unrolled (`break` became `continue`) | no stream slower than 1 %, median -1.9 % | yes |
| one fault check per round | not timed alone | yes |
| input in a 512-byte shared-memory ring, packet loaded at the top of a round | snd -5 %, osdb -6 % | yes |
| two packets prefetched in registers | 0 %: the loop's register rotation waits for the load | no |
| 32-bit table entries with base and extra bits | snd -1 %, big dispatches +45 % (registers, shared memory) | no |
| all-literal round without prefix sum and copies, in literal-dominated blocks | snd -18 %, terrain -7 %, rest -1 to -3 % | yes |
| the same round in every block | snd -16 %, rest +1 to +3 % | no |
| copy loads issued early or deferred a round | +2 to +9 % | no |
| own-lane copy without a per-byte test (overshoot, then literals stored again) | osdb -24 %, garamond -24 %, xml -21 %, dickens -19 % | yes |
| own-lane copy with dword stores | osdb -5 %, rest +3 to +5 % | no |
| tiles sorted by compressed size, largest first | skin -10 %, mozilla -7 %, provinces -12 % | yes |
| clamped lane indices instead of tests in long and short dependent copies | nci -4 %, mozilla -2 % | yes |
| long matches up to 128 bytes copied in pairs, one wait | nci -7 %, provinces -3 % | yes |
| the same in fours, or up to 256 bytes | mozilla +3 %, terrain +19 % (registers) | no |
| run-heavy streams (compressed under 1/20) on a pipeline padded to 4608 bytes | provinces 3.00 to 2.37 | yes |
| two waves per tile, one decoding, one copying | +1 to +34 %; the decoding wave alone -46 to -61 % | no |
| 16-byte pattern stores, 128-byte aligned pattern chunks, period doubling | 0 to +4 % | no |
| per-tile source and destination pointers, for the region API | 0 to +2.3 % on the tile path | yes |
| region path (`cmd_decompress_memory`, upload included) against the tile path | 0 to +0.8 % | |
| SPIR-V 1.5 instead of 1.6, for Vulkan 1.2 devices | -0.8 to +1.0 % | yes |

What the ablations showed, share of kernel time:

| part | osdb (154) | garamond (17) | dickens (156) | snd (66) | provinces (3126) |
|---|---|---|---|---|---|
| own-lane copies, before the overshoot | 54 % | 51 % | 53 % | 24 % | 0 % |
| of that, per-byte tests and stores | 31 % | 45 % | 40 % | 20 % | |
| error checks, upper bound | 4 % | | | 7 % | |
| long dependent pattern copies | | | | | 61 % |

Every lane writing to one 128-byte line instead of its own match: 0 to +2 %. Scattered addresses cost nothing;
instructions do.

## First pass

| change | result | kept |
|---|---|---|
| first correct kernel, one cooperative copy per match | mozilla 14.1 | |
| byte-parallel work list with a binary search per byte | 5.4 | no |
| eight-slot cooperative copies | 6.6 | no |
| short copies by their own lane, long ones by the workgroup | 5.5, dickens 4.3 to 1.25 | yes |
| branch-free symbol math, registers 95 to 56 | 4.36, dickens 1.00 | yes |
| buffer device addresses instead of descriptors | 20-40 % slower, then parity with unconditional loads | yes |
| distance 1-4 as constant dwords | provinces 9.3 to 7.3, suite 212 to 180 | yes |
| short copies pipelined across rounds | no gain, 17 more registers | no |
| ring in shared memory for dependent short matches | x-ray, mozilla 10-14 % faster, text 5-8 % slower | yes |
| ring reads by the owning lane | no gain | no |
| shared memory 5 KB to 4 KB, 6 to 8 waves per SIMD | provinces 7 % faster, rest flat | yes |
| `subgroupBarrier`, output `subgroupcoherent` | same ISA | yes |
| shared memory 4 KB to 3 KB, 9-bit literal root, 10 waves | see table below | yes |
| 2.5 KB, ring 256, 12 waves | same as 3 KB | flag |
| 2 KB, no ring, 7-bit distance root, 16 waves | mozilla-type data 10-30 % slower | flag |
| dword copies for matches of 256 bytes and more | within noise, skin.tex 10 % faster in 2 of 3 runs | yes |
| base + offset addressing for LLPC, RX 9070 XT | 175 fewer vector instructions there, one tile 0.98 to 0.99 | no |

## One tile

First tile of each stream, current kernel; first pass in brackets.

| tile | rounds | ms | us per round |
|---|---|---|---|
| dickens | 753 | 0.52 (0.82) | 0.69 |
| x-ray | 1467 | 0.99 (1.26) | 0.67 |
| terrain_albedo.tex | 2007 | 0.77 (0.94) | 0.38 |

Zeros, 32 MB in 512 tiles: 0.07 ms (450 GB/s of output).

## Tiles per dispatch

First n tiles of mozilla, current kernel; first pass in brackets.

| tiles | ms | GB/s |
|---|---|---|
| 1 | 0.52 (0.88) | 0.13 |
| 8 | 0.66 (1.04) | 0.8 |
| 32 | 0.66 (1.16) | 3.2 |
| 128 | 0.96 (1.81) | 8.7 |
| 512 | 1.48 (4.48) | 22.7 |

## Waves per SIMD

First pass, 4 KB kernel with unused shared memory added to lower residency.

| stream (tiles) | 8 | 7 | 6 | 5 | 4 | 3 | 2 |
|---|---|---|---|---|---|---|---|
| mozilla (128) | 2.01 | 2.00 | 1.96 | 1.94 | 2.07 | 1.86 | 2.19 |
| mozilla (512) | 3.33 | 3.85 | 3.69 | 3.89 | 4.35 | 4.83 | 6.29 |
| mozilla (782) | 4.82 | 5.08 | 5.29 | 6.13 | 5.98 | 7.04 | 9.41 |
| silesia (2048) | 9.06 | 11.64 | 10.88 | 11.91 | 14.08 | 16.19 | 22.63 |
| provinces.bin (3125) | 5.45 | 6.02 | 6.10 | 6.64 | 7.46 | 8.26 | 10.32 |

## Shared memory size

First pass.

| stream (tiles) | 4 KB, 8 waves | 3 KB, 10 waves | 2.5 KB, 12 waves |
|---|---|---|---|
| dickens (1) | 1.33 | 1.46 | 1.36 |
| x-ray (1) | 1.59 | 1.63 | 1.63 |
| texture (1) | 1.28 | 1.26 | 1.44 |
| mozilla (128) | 1.91 | 2.00 | 1.93 |
| mozilla (782) | 4.45 | 4.41 | 4.55 |
| webster (633) | 4.34 | 3.68 | 3.61 |
| skin.tex (704) | 3.19 | 3.11 | 3.21 |
| silesia (2048) | 9.21 | 8.22 | 8.12 |
| provinces.bin (3126) | 5.61 | 5.22 | 5.30 |

Rounds in which at least one lane has a code longer than the root table, percent:

| stream | 10-bit root | 9-bit root |
|---|---|---|
| dickens | 17 | 32 |
| mozilla | 28 | 64 |
| webster | 18 | 39 |
| x-ray | 4 | 47 |
| osdb | 59 | 90 |
| provinces.bin | 0.3 | 9 |
| skin.tex | 9 | 36 |
| terrain_albedo.tex | 3 | 19 |

## Against the Khronos layer

Same buffers, one command buffer, order alternating, best of three runs, both outputs byte-exact. RX 9070 XT: a
friend's machine, driver 26.10.1, the same package. The twelve Silesia rows are also in `results/`.

| stream | tiles | RX 6650 XT layer | RX 6650 XT vkdeflate | ratio | RX 9070 XT layer | RX 9070 XT vkdeflate | ratio |
|---|---|---|---|---|---|---|---|
| mozilla.l6 | 782 | 3.52 | 2.25 | 1.56 | 2.06 | 1.54 | 1.34 |
| fs_straget_big_waves_hit_land.snd.l6 | 66 | 1.11 | 0.71 | 1.56 | 1.11 | 0.80 | 1.39 |
| human.portrait.l6 | 848 | 4.37 | 2.74 | 1.59 | 3.07 | 2.46 | 1.25 |
| terrain_albedo.tex.l6 | 598 | 2.76 | 1.73 | 1.60 | 1.70 | 1.34 | 1.27 |
| nci.l6 | 512 | 1.18 | 0.73 | 1.61 | 0.68 | 0.42 | 1.63 |
| garamond.font.l6 | 17 | 1.26 | 0.77 | 1.64 | 1.22 | 0.86 | 1.42 |
| surfaces_albedo.tex.l6 | 470 | 2.73 | 1.64 | 1.67 | 1.69 | 1.42 | 1.19 |
| skin.tex.l6 | 705 | 2.40 | 1.42 | 1.70 | 1.61 | 1.26 | 1.28 |
| ooffice.l6 | 94 | 2.39 | 1.38 | 1.73 | 2.05 | 1.50 | 1.37 |
| climate.bin.l6 | 98 | 2.06 | 1.19 | 1.73 | 1.80 | 1.26 | 1.43 |
| provinces.bin.l6 | 3126 | 4.43 | 2.37 | 1.87 | 1.89 | 0.93 | 2.03 |
| mr.l0 | 153 | 0.19 | 0.10 | 1.88 | 0.20 | 0.09 | 2.16 |
| sao.l6 | 111 | 2.21 | 1.17 | 1.89 | 1.72 | 1.17 | 1.46 |
| sao.l9 | 111 | 2.24 | 1.19 | 1.89 | 1.74 | 1.19 | 1.46 |
| sao.l12 | 111 | 2.25 | 1.19 | 1.89 | 1.74 | 1.20 | 1.46 |
| webster.l6 | 633 | 3.48 | 1.81 | 1.92 | 1.80 | 0.79 | 2.29 |
| sao.l0 | 111 | 0.19 | 0.10 | 1.92 | 0.21 | 0.10 | 2.03 |
| xml.l9 | 82 | 1.28 | 0.66 | 1.94 | 1.17 | 0.74 | 1.59 |
| xml.l0 | 82 | 0.20 | 0.10 | 1.97 | 0.20 | 0.09 | 2.19 |
| x-ray.l0 | 130 | 0.20 | 0.10 | 2.01 | 0.20 | 0.10 | 2.13 |
| osdb.l6 | 154 | 1.46 | 0.72 | 2.02 | 1.04 | 0.71 | 1.46 |
| xml.l12 | 82 | 1.28 | 0.63 | 2.03 | 1.17 | 0.71 | 1.64 |
| dickens.l0 | 156 | 0.20 | 0.10 | 2.03 | 0.20 | 0.10 | 2.14 |
| sao.l3 | 111 | 2.46 | 1.18 | 2.09 | 1.86 | 1.18 | 1.58 |
| sao.l1 | 111 | 2.52 | 1.19 | 2.12 | 1.91 | 1.19 | 1.61 |
| villages.txt.l6 | 82 | 1.31 | 0.59 | 2.23 | 0.99 | 0.63 | 1.58 |
| xml.l6 | 82 | 1.38 | 0.62 | 2.23 | 1.21 | 0.67 | 1.80 |
| xml.l3 | 82 | 1.59 | 0.69 | 2.30 | 1.39 | 0.76 | 1.83 |
| xml.l1 | 82 | 1.72 | 0.73 | 2.36 | 1.46 | 0.77 | 1.91 |
| x-ray.l9 | 130 | 3.43 | 1.42 | 2.42 | 2.59 | 1.43 | 1.82 |
| x-ray.l12 | 130 | 3.54 | 1.44 | 2.46 | 2.68 | 1.46 | 1.84 |
| samba.l6 | 330 | 1.99 | 0.76 | 2.63 | 1.44 | 0.75 | 1.93 |
| reymont.l6 | 102 | 1.75 | 0.65 | 2.69 | 1.26 | 0.69 | 1.84 |
| mr.l12 | 153 | 2.57 | 0.95 | 2.69 | 1.82 | 0.96 | 1.90 |
| mr.l9 | 153 | 2.59 | 0.96 | 2.70 | 1.84 | 0.96 | 1.92 |
| x-ray.l6 | 130 | 3.48 | 1.25 | 2.77 | 2.53 | 1.26 | 2.00 |
| x-ray.l1 | 130 | 3.55 | 1.26 | 2.82 | 2.57 | 1.27 | 2.01 |
| x-ray.l3 | 130 | 3.54 | 1.24 | 2.85 | 2.56 | 1.25 | 2.04 |
| mr.l6 | 153 | 2.60 | 0.84 | 3.09 | 1.83 | 0.85 | 2.15 |
| mr.l1 | 153 | 2.82 | 0.84 | 3.37 | 1.96 | 0.84 | 2.33 |
| mr.l3 | 153 | 2.81 | 0.83 | 3.40 | 1.94 | 0.83 | 2.35 |
| dickens.l12 | 156 | 2.48 | 0.72 | 3.43 | 1.70 | 0.74 | 2.30 |
| dickens.l9 | 156 | 2.51 | 0.72 | 3.47 | 1.72 | 0.73 | 2.34 |
| dickens.l6 | 156 | 2.61 | 0.68 | 3.84 | 1.80 | 0.67 | 2.68 |
| dickens.l1 | 156 | 3.24 | 0.80 | 4.07 | 2.16 | 0.80 | 2.70 |
| dickens.l3 | 156 | 3.00 | 0.72 | 4.16 | 2.02 | 0.73 | 2.79 |

Ratio, RX 6650 XT / RX 9070 XT: lowest 1.56 / 1.19, median 2.09 / 1.84.

Single tiles and other dispatches, RX 6650 XT, best of two runs:

| stream (tiles) | Khronos layer | vkdeflate | ratio |
|---|---|---|---|
| dickens (1) | 1.70 | 0.57 | 3.0 |
| x-ray (1) | 2.50 | 1.16 | 2.2 |
| terrain_albedo (1) | 0.94 | 0.84 | 1.1 |
| provinces.bin (1) | 0.17 | 0.03 | 5.7 |
| mozilla (128) | 1.79 | 1.08 | 1.7 |
| silesia (2048) | 7.29 | 4.24 | 1.7 |
| zeros 32 MB (512) | 0.24 | 0.07 | 3.3 |

## RX 9070 XT

| | RX 6650 XT | RX 9070 XT |
|---|---|---|
| vector registers, first pass | 60 | 90 |
| vector registers, second pass | 96 | 191 |
| ratio to the layer, second pass, lowest and median | 1.56, 2.09 | 1.19, 1.84 |

The second pass helped small copy-heavy dispatches there as much as here (nci -30 %, osdb -22 %, villages -25 %).
Big texture dispatches gained nothing: surfaces 1.19x, human.portrait 1.25x, terrain 1.27x, skin 1.28x, mozilla
1.34x. The extra registers lower the waves per SIMD there.

## CPU and end to end

MB/s of output for the CPU, ms for the GPU. Reference = NVIDIA's libdeflate fork. End to end = upload, kernel,
download into host memory. GPU columns from the fourth pass, CPU columns from the first.

| stream | MB | CPU, 1 thread | CPU, 6 threads | reference, 1 thread | GPU kernel | GPU end to end |
|---|---|---|---|---|---|---|
| mozilla | 51.2 | 167 | 817 | 347 | 1.93 | 13.0 |
| webster | 41.5 | 184 | 850 | 333 | 1.63 | 10.4 |
| provinces.bin | 204.8 | 823 | 4200 | 2300 | 2.25 | 32 |
| skin.tex | 46.1 | 240 | 1200 | 675 | 1.17 | 9.6 |
| human.portrait | 55.6 | 189 | 919 | 376 | 2.45 | 14.3 |
| terrain_albedo.tex | 39.1 | 87 | 403 | 266 | 1.58 | 13.7 |

Submit to fence minus kernel: 0.12 ms for one tile.

## The kernel as the driver sees it

96 vector registers, 77 scalar, 3072 bytes of shared memory, no scratch. No `s_barrier`, no cache bypass, no
store wait in the ISA. `VK_EXT_memory_decompression` is not offered by this driver.

## Checked

46 reference-encoded streams, 811.6 MB (Silesia at level 6, five files at levels 0, 1, 3, 9, 12, nine game assets):
CPU and GPU byte-exact. 16 pattern streams and the single header: the same. Fuzzing, several thousand broken
copies: CPU and GPU agree on every verdict and output. Second pass: the same, plus 37 edge vectors and 2400
broken copies of eight streams. Subgroup 64 (`-wave64`): the same 46 and 37 streams byte-exact. Linux (gcc 13,
Ubuntu 24.04 under WSL): builds without warnings, CPU decoder byte-exact on the 46 and 37 streams. lavapipe
offers subgroups of 8 or 16 and cannot run the kernel.

## Bugs the checks found

| bug | caught by |
|---|---|
| symbols 286 and 287 rejected even when the block did not define them | xml, tile 1 |
| ring slot overwritten by a late copy: output is not written in position order | four tiles, then a CPU shadow of the ring |
| zeroing loop one word past a shrunken array, into the ring | samba tile 137, webster tile 4 |
| tail of a dword copy loaded before its source was written | skin.tex tile 276 |
| head of a dword copy stored after a lane had read it, distance 128 | skin.tex tiles 410, 414 |

## VolcanStorage

`ahmetenesay/VolcanStorage` at 3d1df3e ships a GLSL port of Microsoft's reference shader (the path without wave
intrinsics). As shipped it does not finish on the RX 6650 XT, and the library dispatches it with no buffers bound.

| fix | why |
|---|---|
| `scan`, `scan16`: shuffle moved out of `?:` | GLSL runs one side only, HLSL both; lanes skipped their shared-memory store |
| `allThreads`: `0xFFFFFFFF` instead of `(1u << 32) - 1u` | shift by 32 is undefined |
| two shift counts masked to 5 bits, shuffle index clamped | undefined in GLSL; not tested one by one |
| loop cap | a wrong decode ends instead of hanging the card |

After the fixes: 46 of 46 streams byte-exact. One round, all three decoders in one command buffer, 41 streams:

| | min | median | max |
|---|---|---|---|
| VolcanStorage ms / vkdeflate ms | 2.29 | 4.56 | 7.15 |
| VolcanStorage ms / Khronos ms | 1.59 | 1.98 | 3.30 |
