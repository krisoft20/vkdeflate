# Kernel notes

`src/tile.comp` decodes one tile per workgroup, `src/vkdeflate.cpp` is the host. The format is in
[`FORMAT.md`](FORMAT.md), the measurements in [`notebook.md`](notebook.md).

## Mapping

- One workgroup = one subgroup of 32 = one tile. The pipeline asks for subgroup size 32 through subgroup size
  control (Vulkan 1.3 or `VK_EXT_subgroup_size_control`); without it, or when 32 is out of range, it takes the
  device's size and lanes 32 and up exit. Narrower than 32: refused.
- SPIR-V 1.5 (`glslc --target-env=vulkan1.2`), Vulkan memory model.
- Lane `i` owns bitstream `i`, in a 64-bit buffer.
- Push constants, no descriptor sets: stream, tile or region table, output, status (0 = none), count (0 = none),
  region stride (0 = tile table). Each workgroup turns its tile into a source and a destination pointer first; the
  loop indexes from 0.
- Loads on the hot path are unconditional with clamped addresses. A load under an `if` gets its own wait.

## Dispatch

- `describe()` returns the tiles sorted by compressed size, largest first. `status[i]` belongs to `tiles[i]`.
- `job_flags()` sets `JOB_RUNS` when the compressed stream is under 1/20 of its output. `record()` then uses a
  second pipeline with 4608 bytes of unused shared memory: about 4 waves per SIMD instead of 10 on RDNA2.

| call | table | tiles | status |
|---|---|---|---|
| `record()` | `tile_desc_t`: word offset, words, byte offset, size | `num_tiles` | yes |
| `record_regions()` | `region_t` = `VkDecompressMemoryRegionEXT`, any stride | host count | optional |
| `record_regions_indirect()` | the same | `max_count` workgroups, those at or past `*count` exit | optional |
| `cmd_decompress_memory()` | regions from host memory, sorted, `vkCmdUpdateBuffer` into a 1 MB ring | host count | no |

A region shorter than 32 words fails with `ERR_INPUT` before any read.

## A round

1. If fewer than 64 input words are staged, the next 32-word packet is loaded at the top of the round. It is
   written into a 128-word ring in shared memory at the end of the round, so its latency hides behind the round.
2. Every lane decodes one symbol: one shared-memory lookup in a root table (9 bits literal/length, 7 distance).
   A longer code walks one record per code length. One such lane makes the whole wave walk.
3. Base and extra bits by arithmetic, consume, refill: a ballot over the lanes that need a word, each reads its
   word from the input ring.
4. Sizes go through `subgroupExclusiveAdd`: every lane gets its output position, the round its total.
5. Literals are stored.
6. Matches decoded in the previous round are copied, see below.

In blocks whose length codes cover less than 1/16 of the code space, a round in which all 32 lanes decode a
literal skips steps 4 and 6: one lookup, consume, refill, store at `pos + lane`. These blocks run their own copy of
the loop.

## Copies

| match | how |
|---|---|
| distance 1-4 | constant dwords, nothing read; a run continued from the previous pattern stays in registers |
| up to 24 bytes, source final | its own lane: the whole 4-byte-rounded block in descending offsets, no per-byte test; then the literals of this and the previous round are stored again over the overshoot |
| 25-128 bytes, source final, distance at least the length | whole workgroup, two matches per step, one wait for both |
| longer, source final | whole workgroup, 128 bytes per step: dwords from 256 bytes and distance 128 up, else bytes with clamped indices |
| source in the previous round's output, up to 32 bytes | from a 512-byte ring in shared memory, or global memory with clamped lanes |
| source in the previous round's output, longer | whole workgroup, one match at a time |

"Source final" = the source ends before the position where the previous round started. Within 24 bytes of the
tile end the own-lane copy tests each byte instead of overshooting.

## Tables

- Code lengths: histogram with shared atomics. Lane `l` computes the first code and symbol offset of length `l`.
- Each lane places its symbols: rank among lanes of the same length from ballots.
- Codes up to the root length fill `2^(root-len)` entries, `symbol | length << 9` in 16 bits. Longer codes go
  only into a sorted symbol list.

## Rules that cost bugs

- Every load of a copy step comes after every store that produced its bytes, in program order.
- The ring only takes bytes at or after the current round's window start. Output is not written in position order.
- Overshoot bytes land only on positions that a later store of the same round rewrites: later matches (descending
  offsets), long and dependent copies (they run after), the two rounds of literals (stored again). Ring writes of
  overshoot bytes outside the window go to a spare byte.
- Lanes read what other lanes stored: `subgroupBarrier()` plus an output buffer declared `subgroupcoherent`. On
  AMD this adds no instructions.
- Same-wave loops compiled from a register rotation wait for the load at the loop end. A prefetch has to go
  through shared memory, written in the same iteration that loaded it.

## Shared memory and registers

3072 bytes, 96 vector registers on RDNA2 (191 on the RX 9070 XT's compiler). On RDNA2 both allow 10 waves per SIMD.

| what | bytes |
|---|---|
| literal/length root table, 512 entries | 1024 |
| distance root table, 128 entries, shared with the precode | 256 |
| sorted symbols, 320 x 9 bits | 428 |
| code lengths, nibbles | 172 |
| length runs | 60 |
| input ring, 128 words | 512 |
| output ring + spare | 516 |

## Knobs

Specialization constants, one binary for all.

| `gdz` flag | `config_t` | default | |
|---|---|---|---|
| `-root N` | `lit_root` | 9 | literal/length root bits; 10 = a 4 KB kernel |
| `-droot N` | `dist_root` | 7 | distance root bits |
| `-ring N` | `ring_bytes` | 512 | output ring size, power of two |
| `-pad N` | `pad_bytes` | 0 | unused shared memory, lowers residency (also in `bench`) |
| | `runs_pad_bytes` | 4608 | padding of the `JOB_RUNS` pipeline, 0 = no second pipeline |
| `-wave32`, `-wave64` | `subgroup_size` | device | subgroup size |
| `-x 11` | `experiment` | | device-scope fence instead of the subgroup barrier |
| `-x 22` | | | no ring, with `-ring 4` |
| `-x 23` | | | byte copies instead of dword copies |

## Commands

    .\build.cmd  or  ./build.sh                              everything
    build\gdz.exe file.gdz -gpu                              CPU and GPU, compared
    build\gdz.exe -list test\vectors\silesia.txt -gpu        the Silesia streams
    build\gdz.exe -fuzz file.gdz -n 1000 -gpu                broken copies, CPU and GPU must agree
    build\gdz.exe file.gdz -info                             blocks, matches, dependent copies, distances
    build\gdz.exe file.gdz -trace N                          matches covering output byte N
    build\gdz.exe file.gdz -gpu -v                           registers, shared memory, ISA
    cmake --build build --target single_header               regenerate single_include\vkdeflate.h, prebuilt\tile_spv.inc
    build\bench.exe -list test\vectors\silesia.txt -csvdir results   times, plus a device and compiler report
    python tools\plot.py                                     redraw docs\img, refresh the README table

After a change to `tile.comp`: Silesia streams, fuzz, `attach_test`, `single_header`. Time changes with `bench`, several
interleaved rounds: `gdz -rounds` catches the GPU in different clock states. A switch kept as a specialization
constant can move the other variant's timing by 10 %; compare variants as separate builds.

`local.cmd` in the root (ignored by git) may set `VKDEFLATE_CMAKE_ARGS` (`-DGLSLC=... -DVULKAN_INCLUDE=...`) and
`VKDEFLATE_BUSY` (a program during which builds refuse to start); `local.sh` does the same on Linux.
