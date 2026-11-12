// vkdeflate_c.h - the C interface of vkdeflate, for C and for bindings from other languages.
//
// Compile one C++ file that defines VKDEFLATE_IMPLEMENTATION and includes vkdeflate.h (or link the vkdeflate
// library), then include this header from C. Functions that can fail return 1 on success and 0 on failure;
// vkdeflate_last_error() then names the reason for the calling thread.
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

#ifndef VKDEFLATE_C_H
#define VKDEFLATE_C_H

#include <stddef.h>
#include <stdint.h>

#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct vkdeflate_ctx vkdeflate_ctx;

typedef struct vkdeflate_tile {
    uint32_t src;
    uint32_t words;
    uint32_t dst;
    uint32_t size;
} vkdeflate_tile;

typedef struct vkdeflate_job {
    VkDeviceAddress src;
    VkDeviceAddress tiles;
    VkDeviceAddress dst;
    VkDeviceAddress status;
    uint32_t num_tiles;
    uint32_t flags;
} vkdeflate_job;

typedef struct vkdeflate_region {
    VkDeviceAddress src;
    VkDeviceAddress dst;
    VkDeviceSize csize;
    VkDeviceSize size;
} vkdeflate_region;

typedef struct vkdeflate_load_item {
    const char* path;
    uint64_t offset;
    uint64_t size;
    const uint8_t* data;
    VkBuffer buffer;
    VkDeviceSize buffer_offset;
    VkImage image;
    uint32_t mip_level;
    uint32_t array_layer;
    VkExtent3D extent;
    VkImageLayout layout;
} vkdeflate_load_item;

const char* vkdeflate_last_error(void);

size_t gdeflate_compress_bound(size_t size);
int gdeflate_compress(const uint8_t* data, size_t size, uint8_t* out, size_t capacity, size_t* written, int level);
int gdeflate_size(const uint8_t* stream, size_t size, size_t* out_size);
int gdeflate_decompress(const uint8_t* stream, size_t size, uint8_t* out, size_t out_size);

vkdeflate_ctx* vkdeflate_create(void);
vkdeflate_ctx* vkdeflate_attach(VkInstance instance, VkPhysicalDevice phys, VkDevice device, PFN_vkGetInstanceProcAddr gipa);
void vkdeflate_destroy(vkdeflate_ctx* ctx);
const char* vkdeflate_device_name(const vkdeflate_ctx* ctx);
const char* vkdeflate_tile_error(uint32_t code);

int vkdeflate_describe(const uint8_t* stream, size_t header_bytes, size_t stream_size, vkdeflate_tile* tiles,
                       uint32_t capacity, uint32_t* count, size_t* out_size);
uint32_t vkdeflate_job_flags(const vkdeflate_tile* tiles, uint32_t count, size_t out_size);
void vkdeflate_record(vkdeflate_ctx* ctx, VkCommandBuffer cmd, const vkdeflate_job* job);
int vkdeflate_cmd_decompress_memory(vkdeflate_ctx* ctx, VkCommandBuffer cmd, const vkdeflate_region* regions, uint32_t count);
void vkdeflate_record_regions_indirect(vkdeflate_ctx* ctx, VkCommandBuffer cmd, VkDeviceAddress regions, VkDeviceAddress count,
                                       uint32_t max_count, uint32_t stride, VkDeviceAddress status, uint32_t flags);
int vkdeflate_load(vkdeflate_ctx* ctx, VkQueue queue, uint32_t queue_family, const vkdeflate_load_item* items, uint32_t count);
int vkdeflate_decompress(vkdeflate_ctx* ctx, const uint8_t* stream, size_t size, uint8_t* out, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif
