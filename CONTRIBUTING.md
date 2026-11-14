# Contributing

## A result from your card

Close other programs that use the GPU. The quickest way is the benchmark package from the
[latest release](https://github.com/krisoft20/vkdeflate/releases/latest): unpack it and run `bench.cmd` or `./bench.sh`.
From the source:

    .\build.cmd              ./build.sh
    .\bench.cmd -khr         ./bench.sh -khr

`bench` writes `results\<gpu>.csv` with the times and `results\<gpu>.txt` with the driver, the subgroup size and
what the shader compiler made of the kernel (registers, shared memory). Open a pull request or an issue with both
files.

Wanted most: any NVIDIA or Intel card and any Linux GPU, which have never run it. Without `-khr` the script skips
the Khronos layer and needs no python. The README table and the chart are redrawn from the CSV by
`python tools\plot.py`; run it or leave it to me.

## Something failed

A crash, a hang or a write outside the output on a broken stream goes to [`SECURITY.md`](SECURITY.md), not to a
public issue. Anything else: open an issue with:

- the GPU and the driver version;
- the last lines the script printed;
- for a wrong byte or a tile error, the output of `build\gdz.exe test\vectors\<stream>.l6.gdz -gpu -v`.

## Slower than it should be

Open an issue with `results\<gpu>.txt` and `results\<gpu>.csv` from a `bench` run. The report says which subgroup
size the driver gave, how many registers and how much shared memory the kernel takes and whether it spills; that
is where a slow card usually differs.

## Changing the code

- Style: snake_case, `_t` types, no comments except `TODO`, small flat functions. Match the file you are in.
- No code from the GDeflate reference implementations. The decoders are written from `docs/FORMAT.md`.
- After a change to `src\` run `check.cmd` and post its last line in the pull request. It runs on your GPU what
  CI cannot: the Silesia streams and a fuzz run against the CPU decoder with both literal-round settings, the
  attach path, the single header, the C interface, the layer and `load()`. About a minute.
- A change to `src\tile.comp` needs `glslc`, from the Vulkan SDK or as `-DGLSLC=path` in `VKDEFLATE_CMAKE_ARGS`.
  `single_header` also refreshes `prebuilt\tile_spv.inc`; commit both. CI fails when they are older than `src\`
  (`python tools\amalgamate.py --check`).
- A change that is meant to be faster comes with numbers: before and after, same streams, runs interleaved.
