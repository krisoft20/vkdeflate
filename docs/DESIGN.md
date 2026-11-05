# Kernel notes

`src/tile.comp` decodes one tile per workgroup, `src/vkdeflate.cpp` is the host. The format is in
[`FORMAT.md`](FORMAT.md), the measurements in [`notebook.md`](notebook.md).

## Mapping

- One workgroup = one subgroup of 32 = one tile. The pipeline requires subgroup size 32; if the device cannot,
  it takes the device's size and the extra lanes exit.
- Lane `i` owns bitstream `i`, in a 64-bit buffer.
- Four buffers by device address in push constants, no descriptor sets: stream (words), tile table, output
  (bytes), one status word per tile.
- Loads on the hot path are unconditional with clamped addresses. A load under an `if` gets its own wait.

## A round

1. Every lane decodes one symbol: one shared-memory lookup in a root table (9 bits literal/length, 8 distance).
   A longer code walks one record per code length. One such lane makes the whole wave walk.
2. Base and extra bits by arithmetic, consume, refill. Packets are fetched a round ahead into two registers and
   handed out with a ballot and two shuffles.
3. Sizes go through `subgroupExclusiveAdd`: every lane gets its output position, the round its total.
4. Literals are stored.
5. Matches decoded in the previous round are copied, see below.
6. `subgroupBarrier()`.

## Copies

| match | how |
|---|---|
| distance 1-4 | constant dwords, nothing read; a run continued from the previous pattern stays in registers |
| up to 32 bytes, source final | its own lane, 4 bytes per step, wrapping source pointer |
| longer, source final | whole workgroup, 128 bytes per step: dwords from 256 bytes and distance 128 up, else bytes |
| source in the previous round's output, up to 32 bytes | from a 512-byte ring in shared memory |
| source in the previous round's output, longer | whole workgroup behind a barrier, one match at a time |

"Source final" = the source ends before the position where the previous round started.

## Tables

- Code lengths: histogram with shared atomics. Lane `l` computes the first code and symbol offset of length `l`.
- Each lane places its symbols: rank among lanes of the same length from ballots.
- Codes up to the root length fill `2^(root-len)` entries, `symbol | length << 9` in 16 bits. Longer codes go
  only into a sorted symbol list.

## Rules that cost bugs

- Every load of a copy step comes after every store that produced its bytes, in program order.
- The ring only takes bytes at or after the current round's window start. Output is not written in position order.
- Lanes read what other lanes stored: `subgroupBarrier()` plus an output buffer declared `subgroupcoherent`. On
  AMD this adds no instructions.

## Shared memory

3072 bytes, 60 vector registers. On RDNA2: 64 KB per CU / 3 KB = 21 workgroups, 10 waves per SIMD. Each slot is
worth 5-15 % once a dispatch has more tiles than slots.

| what | bytes |
|---|---|
| literal/length root table, 512 entries | 1024 |
| distance root table, 256 entries, shared with the precode | 512 |
| sorted symbols, 320 x 9 bits | 428 |
| code lengths, nibbles | 172 |
| length runs, slow-path records | 60 + 60 |
| ring | 512 |

## Knobs

Specialization constants, one binary for all.

| `gdz` flag | `config_t` | default | |
|---|---|---|---|
| `-root N` | `lit_root` | 9 | literal/length root bits; 10 = the 4 KB kernel |
| `-droot N` | `dist_root` | 8 | distance root bits |
| `-ring N` | `ring_bytes` | 512 | ring size, power of two |
| `-pad N` | `pad_bytes` | 0 | unused shared memory, lowers residency |
| `-wave32`, `-wave64` | `subgroup_size` | device | subgroup size |
| `-x 11` | `experiment` | | device-scope fence instead of the subgroup barrier |
| `-x 22` | | | no ring, with `-ring 4` |
| `-x 23` | | | byte copies instead of dword copies |
| `-x 30` | | | buffer addresses written out as base + 64-bit offset |
| `-x 31` | | | buffer indices masked to 31 bits, for streams under 2 GB |
| `-x 32` | | | per-tile base address and tile-relative indices |

## Commands

    .\build.cmd                                              everything
    build\gdz.exe file.gdz -gpu                              CPU and GPU, compared
    build\gdz.exe -list test\vectors\manifest.txt -gpu       the vector suite
    build\gdz.exe -fuzz file.gdz -n 1000 -gpu                broken copies, CPU and GPU must agree
    build\gdz.exe file.gdz -info                             blocks, matches, code lengths
    build\gdz.exe file.gdz -trace N                          matches covering output byte N
    build\gdz.exe file.gdz -gpu -v                           registers, shared memory, ISA
    cmake --build build --target single_header               regenerate single_include\vkdeflate.h, prebuilt\tile_spv.inc
    python tools\plot.py                                     redraw docs\img, refresh the README table
    tools\diag.cmd                                           every switch on five streams, compiler statistics

After a change to `tile.comp`: suite, fuzz, `attach_test`, `single_header`.

`local.cmd` in the root (ignored by git) may set `VKDEFLATE_CMAKE_ARGS` (`-DGLSLC=... -DVULKAN_INCLUDE=...`) and
`VKDEFLATE_BUSY` (a program during which builds refuse to start). `test\make_vectors.cmd` builds the full suite;
edit its paths.
