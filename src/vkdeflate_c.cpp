// vkdeflate_c - the C interface: thin wrappers over gdeflate:: and vkdeflate::
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

#include <cstring>
#include <string>
#include <vector>

#include "gdeflate.h"
#include "vkdeflate.h"
#include "vkdeflate_c.h"

static thread_local std::string vkdeflate_c_error;

static_assert(sizeof(vkdeflate_tile) == sizeof(vkdeflate::tile_desc_t), "vkdeflate_tile mirrors tile_desc_t");
static_assert(sizeof(vkdeflate_job) == sizeof(vkdeflate::job_t), "vkdeflate_job mirrors job_t");
static_assert(sizeof(vkdeflate_region) == sizeof(vkdeflate::region_t), "vkdeflate_region mirrors region_t");

extern "C" {

const char* vkdeflate_last_error(void) {
    return vkdeflate_c_error.c_str();
}

size_t gdeflate_compress_bound(size_t size) {
    return gdeflate::compress_bound(size);
}

int gdeflate_compress(const uint8_t* data, size_t size, uint8_t* out, size_t capacity, size_t* written, int level) {
    size_t got = 0;
    if (!gdeflate::compress_into(data, size, out, capacity, got, vkdeflate_c_error, level))
        return 0;
    if (written)
        *written = got;
    return 1;
}

int gdeflate_size(const uint8_t* stream, size_t size, size_t* out_size) {
    gdeflate::stream_t s;
    if (!gdeflate::parse(stream, size, s, vkdeflate_c_error))
        return 0;
    *out_size = s.total;
    return 1;
}

int gdeflate_decompress(const uint8_t* stream, size_t size, uint8_t* out, size_t out_size) {
    return gdeflate::decompress(stream, size, out, out_size, vkdeflate_c_error) ? 1 : 0;
}

vkdeflate_ctx* vkdeflate_create(void) {
    return (vkdeflate_ctx*)vkdeflate::create({}, vkdeflate_c_error);
}

vkdeflate_ctx* vkdeflate_attach(VkInstance instance, VkPhysicalDevice phys, VkDevice device, PFN_vkGetInstanceProcAddr gipa) {
    return (vkdeflate_ctx*)vkdeflate::attach(instance, phys, device, gipa, {}, vkdeflate_c_error);
}

void vkdeflate_destroy(vkdeflate_ctx* ctx) {
    vkdeflate::destroy((vkdeflate::ctx_t*)ctx);
}

const char* vkdeflate_device_name(const vkdeflate_ctx* ctx) {
    return vkdeflate::device_name((const vkdeflate::ctx_t*)ctx);
}

const char* vkdeflate_tile_error(uint32_t code) {
    return vkdeflate::tile_error(code);
}

int vkdeflate_describe(const uint8_t* stream, size_t header_bytes, size_t stream_size, vkdeflate_tile* tiles,
                       uint32_t capacity, uint32_t* count, size_t* out_size) {
    std::vector<vkdeflate::tile_desc_t> list;
    size_t total = 0;
    if (!vkdeflate::describe(stream, header_bytes, stream_size, list, total, vkdeflate_c_error))
        return 0;
    if (count)
        *count = (uint32_t)list.size();
    if (out_size)
        *out_size = total;
    if (!tiles)
        return 1;
    if (list.size() > capacity) {
        vkdeflate_c_error = "tile array too small";
        return 0;
    }
    memcpy(tiles, list.data(), list.size() * sizeof(vkdeflate_tile));
    return 1;
}

uint32_t vkdeflate_job_flags(const vkdeflate_tile* tiles, uint32_t count, size_t out_size) {
    const vkdeflate::tile_desc_t* first = (const vkdeflate::tile_desc_t*)tiles;
    return vkdeflate::job_flags(std::vector<vkdeflate::tile_desc_t>(first, first + count), out_size);
}

void vkdeflate_record(vkdeflate_ctx* ctx, VkCommandBuffer cmd, const vkdeflate_job* job) {
    vkdeflate::record((vkdeflate::ctx_t*)ctx, cmd,
                      vkdeflate::job_t{job->src, job->tiles, job->dst, job->status, job->num_tiles, job->flags});
}

int vkdeflate_cmd_decompress_memory(vkdeflate_ctx* ctx, VkCommandBuffer cmd, const vkdeflate_region* regions, uint32_t count) {
    if (vkdeflate::cmd_decompress_memory((vkdeflate::ctx_t*)ctx, cmd, (const vkdeflate::region_t*)regions, count))
        return 1;
    vkdeflate_c_error = "region buffer could not be made";
    return 0;
}

void vkdeflate_record_regions_indirect(vkdeflate_ctx* ctx, VkCommandBuffer cmd, VkDeviceAddress regions, VkDeviceAddress count,
                                       uint32_t max_count, uint32_t stride, VkDeviceAddress status, uint32_t flags) {
    vkdeflate::record_regions_indirect((vkdeflate::ctx_t*)ctx, cmd, regions, count, max_count, stride, status, flags);
}

int vkdeflate_load(vkdeflate_ctx* ctx, VkQueue queue, uint32_t queue_family, const vkdeflate_load_item* items, uint32_t count) {
    std::vector<vkdeflate::load_t> list(count);
    for (uint32_t i = 0; i < count; i++) {
        vkdeflate::load_t& to = list[i];
        to.path = items[i].path;
        to.offset = items[i].offset;
        to.size = items[i].size;
        to.data = items[i].data;
        to.buffer = items[i].buffer;
        to.buffer_offset = items[i].buffer_offset;
        to.image = items[i].image;
        to.layers = {VK_IMAGE_ASPECT_COLOR_BIT, items[i].mip_level, items[i].array_layer, 1};
        to.extent = items[i].extent;
        if (items[i].layout != VK_IMAGE_LAYOUT_UNDEFINED)
            to.layout = items[i].layout;
    }
    return vkdeflate::load((vkdeflate::ctx_t*)ctx, queue, queue_family, list.data(), count, vkdeflate_c_error) ? 1 : 0;
}

int vkdeflate_decompress(vkdeflate_ctx* ctx, const uint8_t* stream, size_t size, uint8_t* out, size_t out_size) {
    return vkdeflate::decompress((vkdeflate::ctx_t*)ctx, stream, size, out, out_size, vkdeflate_c_error) ? 1 : 0;
}

}
