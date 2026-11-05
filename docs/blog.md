# Writing a GDeflate decoder as a Vulkan compute shader

*November 2026, krisoft20*

I am making a grand strategy game. Its map is a 200 MB file that compresses to 3 MB, and its textures are the usual
tens of megabytes each. I wanted to know whether my GPU, an AMD Radeon RX 6650 XT, could unpack them itself, in
Vulkan. This is what I found, with the numbers first and my opinions at the end.

## Why GDeflate

My first attempt was zstd on the GPU. It died on the format: zstd's matches form one long dependency chain, and the
best I got was the Huffman stage at 6-12 GB/s with everything else stuck on the CPU.

GDeflate is DEFLATE rearranged for exactly this problem.

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="img/format-dark.svg">
  <img src="img/format.svg" alt="A GDeflate stream is a row of independent 64 KB tiles; each tile interleaves 32 bitstreams">
</picture>

Tiles never reference each other, so thousands decode at once. Inside a tile, 32 bitstreams advance together, so
one tile fills one 32-wide SIMD unit. It is the format behind DirectStorage and RTX IO, which means the encoder
already exists and is open.

What did not exist for my card was a decoder. Vulkan's `VK_EXT_memory_decompression` is implemented by NVIDIA's
driver; AMD's does not have it, and I found no other driver that does. So I wrote one: a compute shader, a host library around it, and a slow, readable CPU
decoder to check it against. No code from the reference implementations; the format document in this repository
is what I wrote down while reading them.

## The kernel in four sentences

One workgroup decodes one tile, one invocation owns one bitstream. Every round each lane decodes one symbol, a
prefix sum over the lanes turns sizes into output positions, and literals are stored at once. Matches are copied a
round later, when their distances are known. The first version that produced correct bytes took 14 ms for the 51 MB
`mozilla` file; the one in this repository takes about 4.5 ms. The details are in [`DESIGN.md`](DESIGN.md).

## One tile takes a millisecond

I expected memory bandwidth to be the limit. It is nowhere near: the same kernel writes runs of zeros at
140-200 GB/s on this card. Real data goes at 10-15 GB/s because a round is a chain of dependent table lookups, and
a GPU core runs dependent chains slowly. I measured 0.2 to 0.3 instructions per cycle.

A tile is 750 to 2000 such rounds in a row, about a microsecond each, and nothing in the format shortens that
chain. So one tile takes about a millisecond however idle the GPU is, and so do eight.

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="img/saturation-dark.svg">
  <img src="img/saturation.svg" alt="Output rate against tiles per dispatch: 0.08 GB/s at one tile, 7.5 GB/s at 512">
</picture>

For a game this means putting many small assets into one dispatch.

## What made it faster

- **Copies by shape.** A match with distance 1 to 4 is a repeating pattern. The workgroup writes it as constant
  dwords without reading anything. The map went from 9.3 to 7.3 ms.
- **A 512-byte ring in shared memory.** Matches that read what the previous round just wrote had to go through
  global memory behind a barrier. A small mirror of the newest output removes that round trip: 10-14 % on binaries,
  and a loss of 5-8 % on plain text, which has few such matches.
- **Less shared memory.** This was the one I did not see coming. The kernel's shared memory decides how many tiles
  a SIMD unit keeps in flight, and every slot counts once the GPU is full.

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="img/residency-dark.svg">
  <img src="img/residency.svg" alt="Kernel time rises as waves per SIMD drop from 8 to 2, for three streams">
</picture>

Halving the literal table (a 9-bit root instead of 10) took the kernel from 4 KB to 3 KB, 8 to 10 waves per SIMD.
It doubles the rounds that fall back to the slow decode path, and it is still 7 to 15 % faster on big batches.

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="img/lds-dark.svg">
  <img src="img/lds.svg" alt="Kernel time for 4 KB, 3 KB and 2.5 KB of shared memory on five streams">
</picture>

## What did not help

- **Dword copies for long matches.** The textbook optimization: a quarter of the memory instructions. Three A/B
  runs showed nothing outside the noise. Copies are not where a round spends its time.
- **Going down to 2 KB by dropping the ring.** Sixteen waves per SIMD, and 10-30 % slower on binaries. The ring was
  worth more than the slots.
- **Pipelining short copies across rounds.** The compiler waited anyway and the kernel grew by 17 registers.

Each of these sounded right. I only know they are wrong because every experiment is a switch in one binary and
every comparison is interleaved, best of several rounds, on a machine that swings by 10 % with whatever else is
using the GPU.

## Three bugs the tests caught

- The ring was correct on paper and wrong on four tiles. Output is not written in position order: a match lands a
  round after the literals behind it, so a late copy overwrote a newer byte's slot.
- I shrank an array from 44 to 43 words and left a loop bound at 44. The extra word was the start of the ring in
  the new layout, and two tiles out of twelve thousand came out wrong.
- With a distance of exactly 128 and an unaligned start, one lane read two bytes of a match before another lane
  had written them. Two zeros then travelled down the copy every 128 bytes.

All three were deterministic, and I found none of them by reading the shader. What found them: 811 MB of
streams from the reference encoder compared byte for byte, a fuzzer that checks the CPU and GPU agree on every
broken input, and a CPU decoder simple enough to trust.

## Against the other open decoder

Late in the project I learned that an open GPU decoder already existed: NVIDIA's own GLSL shaders, inside a Khronos
layer that emulates the older `VK_NV_memory_decompression`. vkd3d-proton carries a copy for DirectStorage games on
Linux. It uses the same mapping, 32 invocations per tile, so the comparison is fair: both decoders in one command
buffer, same buffers, GPU timestamps around each, order alternating, both outputs checked.

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="img/khronos_all-dark.svg">
  <img src="img/khronos_all.svg" alt="Kernel time per stream, vkdeflate against the Khronos layer, fifteen streams">
</picture>

On large text streams the two are within a few percent, because both are the same chain of rounds. vkdeflate is
1.3x faster on the map and on textures with long matches, about 2.4x on streams full of short dependent matches,
and 4x on zeros. It loses by 6 % on one database dump. I tuned my kernel on this card, the other one
was written by NVIDIA, and one card is all I have measured.

## What I think

**GDeflate compresses like zlib.** zstd and Oodle pack tighter. That difference in file size is what you pay for a
format a GPU can decode.

**GPU decompression does not shorten loading by much.**

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="img/cpu_gpu-dark.svg">
  <img src="img/cpu_gpu.svg" alt="GPU kernel, GPU end to end and CPU six threads in GB/s for six streams">
</picture>

The kernel decodes at 10 to 40 GB/s. If the result has to come back to host memory, the copy over PCIe brings that
down to 3 to 6 GB/s, which is what a few CPU threads of a good zstd decoder do. It makes sense when the data stays on the GPU and the
GPU is otherwise idle, which means loading screens, or when CPU cores are the scarce thing. During gameplay it
competes with rendering, and the DirectStorage titles that shipped with it show mixed results
([Ratchet & Clank](https://www.tomshardware.com/news/directstorage-bug-destroys-frame-rates-in-ratchet-and-clank-rift-apart),
[Spider-Man 2](https://pcgamer.com/hardware/graphics-cards/spider-man-2s-pc-launch-woes-are-woe-ier-than-you-think-as-it-looks-like-gpu-powered-directstorage-is-hobbling-performance)).

**For my own game it saves tens of milliseconds per large asset.** I built it anyway, because I wanted
to know, and because the next person with an AMD card should not have to start from zero.

**What I would tell myself at the start:** write the slow decoder first and make it match the reference byte for
byte, and build the fuzzer before the first optimization.

## If you want to help

It has run on one GPU. `bench.cmd` downloads the Silesia corpus, compresses it, decodes it on your card and writes a
CSV; `bench.cmd -khr` adds the Khronos layer for comparison. A result from an NVIDIA or Intel card, or a failure,
is worth more than anything else you could send.
