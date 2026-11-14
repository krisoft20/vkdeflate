# Examples

Each one decodes on the GPU and compares the result with the CPU decoder. `.\build.cmd` builds them as
`build\example_*.exe`.

| example | API | what it shows |
|---|---|---|
| [`decompress.cpp`](decompress.cpp) | `create()`, `decompress()` | `create()` makes the device and the buffers; output lands in host memory |
| [`batch.cpp`](batch.cpp) | `attach()`, `describe()`, `record()` | your device and buffers, several streams in one command buffer |
| [`regions.cpp`](regions.cpp) | `cmd_decompress_memory()` | the `vkCmdDecompressMemoryEXT` call shape: an array of regions in host memory, one raw tile each |
| [`indirect.cpp`](indirect.cpp) | `record_regions_indirect()` | regions in GPU memory and their count written on the GPU, as `vkCmdDecompressMemoryIndirectCountEXT` |
| [`load.cpp`](load.cpp) | `load()`, `record_load()`, `device_features()` | a file into a buffer and into an image with two mip levels; the same in your own command buffer |

    build\example_decompress.exe test\vectors\dickens.l6.gdz
    build\example_batch.exe a.gdz b.gdz c.gdz
    build\example_regions.exe test\vectors\dickens.l6.gdz
    build\example_indirect.exe test\vectors\dickens.l6.gdz
    build\example_load.exe test\vectors\dickens.l6.gdz

[`common.h`](common.h) is the Vulkan setup they share: an instance, a device with the features vkdeflate needs,
host-visible buffers with device addresses, one submit.

## Coming from `VK_EXT_memory_decompression`

| extension | vkdeflate |
|---|---|
| `VkDecompressMemoryRegionEXT` | `vkdeflate::region_t`, same layout |
| `vkCmdDecompressMemoryEXT(cmd, &info)` | `vkdeflate::cmd_decompress_memory(ctx, cmd, &info)` |
| `vkCmdDecompressMemoryIndirectCountEXT(cmd, method, regions, count, max, stride)` | `vkdeflate::record_regions_indirect(ctx, cmd, regions, count, max, stride)` |
| `VK_PIPELINE_STAGE_2_MEMORY_DECOMPRESSION_BIT_EXT` in barriers | `VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT` with shader read and write access |

- One region is one raw GDeflate tile of at most 64 KB, as in the extension. `srcAddress` and `dstAddress` must
  be 4-byte aligned.
- `cmd_decompress_memory()` copies the regions into a 1 MB ring inside the context with `vkCmdUpdateBuffer`, so the
  array can be freed after the call. The ring is reused in recording order, behind barriers: submit the command
  buffers to one queue in the order they were recorded, or keep fewer than 32768 regions in flight.
- The extension reports no errors. `record_regions()` and `record_regions_indirect()` take an optional status
  buffer, one `uint32_t` per region, 0 for a good tile.
