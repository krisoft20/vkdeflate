# Notebook

What was tried and what came out. One card: AMD Radeon RX 6650 XT, driver 26.10.1, Ryzen 5 5600X. Kernel time in ms
on the GPU, best of several rounds, unless a row says otherwise. The machine was shared, so times move by 10 %
between sessions; comparisons inside one table were run interleaved.

## What was tried

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

## One tile

| tile | rounds | ms | us per round |
|---|---|---|---|
| dickens | 753 | 0.82 | 1.09 |
| x-ray | 1467 | 1.26 | 0.86 |
| terrain_albedo.tex | 2007 | 0.94 | 0.47 |

Zeros, 32 MB in 512 tiles: 0.24 ms (140 GB/s) with an idle GPU clock, 0.16 ms (200 GB/s) warmed up.

## Tiles per dispatch

First n tiles of mozilla.

| tiles | ms | GB/s |
|---|---|---|
| 1 | 0.88 | 0.08 |
| 8 | 1.04 | 0.5 |
| 32 | 1.16 | 1.8 |
| 128 | 1.81 | 4.6 |
| 512 | 4.48 | 7.5 |

## Waves per SIMD

4 KB kernel with unused shared memory added to lower residency.

| stream (tiles) | 8 | 7 | 6 | 5 | 4 | 3 | 2 |
|---|---|---|---|---|---|---|---|
| mozilla (128) | 2.01 | 2.00 | 1.96 | 1.94 | 2.07 | 1.86 | 2.19 |
| mozilla (512) | 3.33 | 3.85 | 3.69 | 3.89 | 4.35 | 4.83 | 6.29 |
| mozilla (782) | 4.82 | 5.08 | 5.29 | 6.13 | 5.98 | 7.04 | 9.41 |
| silesia (2048) | 9.06 | 11.64 | 10.88 | 11.91 | 14.08 | 16.19 | 22.63 |
| provinces.bin (3125) | 5.45 | 6.02 | 6.10 | 6.64 | 7.46 | 8.26 | 10.32 |

## Shared memory size

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

Same buffers, one command buffer, order alternating, best of 7, both outputs byte-exact. The twelve Silesia rows
are also in `results/`.

| stream (tiles) | Khronos layer | vkdeflate | ratio |
|---|---|---|---|
| dickens (1) | 1.73 | 1.01 | 1.7 |
| x-ray (1) | 2.53 | 1.33 | 1.9 |
| terrain_albedo (1) | 1.03 | 1.03 | 1.0 |
| provinces.bin (1) | 0.48 | 0.33 | 1.4 |
| mozilla (128) | 1.86 | 1.59 | 1.2 |
| dickens (156) | 2.69 | 1.15 | 2.3 |
| x-ray (130) | 3.51 | 1.47 | 2.4 |
| xml (82) | 1.43 | 1.14 | 1.3 |
| nci (512) | 1.23 | 1.22 | 1.0 |
| osdb (154) | 1.47 | 1.56 | 0.9 |
| mozilla (782) | 3.55 | 3.27 | 1.1 |
| webster (633) | 3.24 | 3.15 | 1.0 |
| skin.tex (704) | 2.44 | 1.93 | 1.3 |
| human.portrait (848) | 4.31 | 2.82 | 1.5 |
| terrain_albedo.tex (597) | 2.81 | 2.66 | 1.1 |
| climate.bin (98) | 2.09 | 1.56 | 1.3 |
| provinces.bin (3126) | 4.48 | 3.51 | 1.3 |
| silesia (2048) | 7.38 | 5.74 | 1.3 |
| zeros 32 MB (512) | 0.48 | 0.12 | 4.2 |

The layer's direct path, one dispatch per tile: mozilla 3.50, provinces.bin 4.39, xml 1.69.

## CPU and end to end

MB/s of output for the CPU, ms for the GPU. Reference = NVIDIA's libdeflate fork. End to end = upload, kernel,
download into host memory.

| stream | MB | CPU, 1 thread | CPU, 6 threads | reference, 1 thread | GPU kernel | GPU end to end |
|---|---|---|---|---|---|---|
| mozilla | 51.2 | 167 | 817 | 347 | 4.57 | 14.0 |
| webster | 41.5 | 184 | 850 | 333 | 3.73 | 11.2 |
| provinces.bin | 204.8 | 823 | 4200 | 2300 | 5.15 | 33 |
| skin.tex | 46.1 | 240 | 1200 | 675 | 3.03 | 10.9 |
| human.portrait | 55.6 | 189 | 919 | 376 | 4.37 | 14.3 |
| terrain_albedo.tex | 39.1 | 87 | 403 | 266 | 3.61 | 14.2 |

Submit to fence minus kernel: 0.12 ms for one tile.

## The kernel as the driver sees it

60 vector registers, 80 scalar, 3072 bytes of shared memory, no scratch. No `s_barrier`, no cache bypass, no
store wait in the ISA. `VK_EXT_memory_decompression` is not offered by this driver.

## Checked

46 reference-encoded streams, 811.6 MB (Silesia at level 6, five files at levels 0, 1, 3, 9, 12, nine game assets):
CPU and GPU byte-exact. 16 pattern streams and the single header: the same. Fuzzing, several thousand broken
copies: CPU and GPU agree on every verdict and output.

## Bugs the checks found

| bug | caught by |
|---|---|
| symbols 286 and 287 rejected even when the block did not define them | xml, tile 1 |
| ring slot overwritten by a late copy: output is not written in position order | four tiles, then a CPU shadow of the ring |
| zeroing loop one word past a shrunken array, into the ring | samba tile 137, webster tile 4 |
| tail of a dword copy loaded before its source was written | skin.tex tile 276 |
| head of a dword copy stored after a lane had read it, distance 128 | skin.tex tiles 410, 414 |
