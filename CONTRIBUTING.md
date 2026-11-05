# Contributing

## A result from your card

Close other programs that use the GPU, then:

    .\build.cmd
    .\bench.cmd -khr

`bench.cmd` writes `results\<gpu>.csv`. Open a pull request with that file. `.\bench.cmd` without `-khr` skips the
Khronos layer and needs no python. The README table and the chart are redrawn from the CSV by
`python tools\plot.py`; run it or leave it to me.

## Something failed

Open an issue with:

- the GPU and the driver version;
- the last lines the script printed;
- for a wrong byte or a tile error, the output of `build\gdz.exe test\vectors\<stream>.l6.gdz -gpu -v`.

## Changing the code

- Style: snake_case, `_t` types, no comments except `TODO`, small flat functions. Match the file you are in.
- No code from the GDeflate reference implementations. The decoders are written from `docs/FORMAT.md`.
- After a change to `src\tile.comp` these have to pass (`bench.cmd` creates the streams):

      build\gdz.exe -list test\vectors\silesia.txt -gpu
      build\gdz.exe -fuzz test\vectors\xml.l6.gdz -n 1000 -gpu
      build\attach_test.exe test\vectors\xml.l6.gdz
      cmake --build build --target single_header
      build\single_test.exe test\vectors\xml.l6.gdz

- A change to `src\tile.comp` needs `glslc`, from the Vulkan SDK or as `-DGLSLC=path` in `VKDEFLATE_CMAKE_ARGS`.
  `single_header` also refreshes `prebuilt\tile_spv.inc`; commit both.
- A change that is meant to be faster comes with numbers: before and after, same streams, runs interleaved.
