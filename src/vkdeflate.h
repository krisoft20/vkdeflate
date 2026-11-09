// vkdeflate - a GDeflate 1.0 decoder for Vulkan compute and for the CPU
//
// This header is the GPU side. Two ways to get a context:
//
//   create(): the library opens the Vulkan loader itself, makes an instance, a
//   device and a compute queue. decompress() then decodes a stream from host
//   memory to host memory (upload, kernel, download) and can report timings.
//   This is what the gdz tool uses.
//
//   attach(): the caller already has a VkDevice (a game). The library only
//   builds its pipeline on it. describe() turns a stream's first 8 + 4n bytes
//   into one tile_desc_t per tile, the caller puts the stream, the descriptors,
//   an output buffer and a status buffer in device memory, and record() writes
//   the dispatch into the caller's command buffer. Buffers are passed as device
//   addresses (VK_KHR_buffer_device_address, core 1.2), so the caller's buffers
//   need VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT and their memory
//   VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT; every address 4-byte aligned. The
//   caller's device must have shaderInt64, shaderInt8, storageBuffer8BitAccess,
//   bufferDeviceAddress, subgroupSizeControl and computeFullSubgroups enabled.
//   The caller owns the barriers: the stream, descriptors and status readable
//   and the output writable by compute shaders before the dispatch, and whatever
//   consumes the output after it. One status word per tile, 0 when the tile
//   decoded, else an error code (tile_error() names it).
//
// vulkan-1.dll / libvulkan.so.1 is loaded at runtime by create(); attach() uses
// the vkGetInstanceProcAddr the caller passes. No import library, no SDK, only
// the headers.
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

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

namespace vkdeflate {
    struct config_t {
        uint32_t subgroup_size = 0;
        uint32_t experiment = 0;
        uint32_t ring_bytes = 512;
        uint32_t pad_bytes = 0;
        uint32_t lit_root = 9;
        uint32_t dist_root = 7;
        uint32_t runs_pad_bytes = 4608;
        bool verbose = false;
    };

    struct timing_t {
        double host_up_ms = 0;
        double gpu_up_ms = 0;
        double kernel_ms = 0;
        double gpu_down_ms = 0;
        double host_down_ms = 0;
        double submit_ms = 0;
        double total_ms = 0;
        size_t upload_bytes = 0;
    };

    struct tile_desc_t {
        uint32_t src;
        uint32_t words;
        uint32_t dst;
        uint32_t size;
    };

    struct job_t {
        VkDeviceAddress src;
        VkDeviceAddress tiles;
        VkDeviceAddress dst;
        VkDeviceAddress status;
        uint32_t num_tiles;
        uint32_t flags = 0;
    };

    inline constexpr uint32_t JOB_RUNS = 1;

    struct region_t {
        VkDeviceAddress src;
        VkDeviceAddress dst;
        VkDeviceSize csize;
        VkDeviceSize size;
    };

    struct ctx_t;

    ctx_t* create(const config_t& cfg, std::string& err);
    ctx_t* attach(VkInstance instance, VkPhysicalDevice phys, VkDevice dev, PFN_vkGetInstanceProcAddr gipa,
                  const config_t& cfg, std::string& err);
    void destroy(ctx_t* ctx);
    const char* device_name(const ctx_t* ctx);
    const char* tile_error(uint32_t code);

    bool describe(const uint8_t* p, size_t n, size_t stream_size, std::vector<tile_desc_t>& tiles, size_t& out_size,
                  std::string& err);
    uint32_t job_flags(const std::vector<tile_desc_t>& tiles, size_t out_size);
    void record(ctx_t* ctx, VkCommandBuffer cmd, const job_t& job);

    uint32_t region_flags(const region_t* regions, uint32_t count);
    void record_regions(ctx_t* ctx, VkCommandBuffer cmd, VkDeviceAddress regions, uint32_t count,
                        uint32_t stride = sizeof(region_t), VkDeviceAddress status = 0, uint32_t flags = 0);
    void record_regions_indirect(ctx_t* ctx, VkCommandBuffer cmd, VkDeviceAddress regions, VkDeviceAddress count,
                                 uint32_t max_count, uint32_t stride = sizeof(region_t), VkDeviceAddress status = 0,
                                 uint32_t flags = 0);
    bool cmd_decompress_memory(ctx_t* ctx, VkCommandBuffer cmd, const region_t* regions, uint32_t count);

#if defined(VK_EXT_memory_decompression)
    static_assert(sizeof(region_t) == sizeof(VkDecompressMemoryRegionEXT), "region_t mirrors VkDecompressMemoryRegionEXT");

    inline bool cmd_decompress_memory(ctx_t* ctx, VkCommandBuffer cmd, const VkDecompressMemoryInfoEXT* info) {
        return cmd_decompress_memory(ctx, cmd, (const region_t*)info->pRegions, info->regionCount);
    }
#endif

    bool decompress(ctx_t* ctx, const uint8_t* in, size_t n, uint8_t* out, size_t size, std::string& err,
                    timing_t* timing = nullptr);
}
