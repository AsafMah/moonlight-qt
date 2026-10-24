#pragma once

// The parts of pyrowave.h (C API 101.0.0) that the decoder uses.
//
// Moonlight loads the runtime at run time (PyrowaveRuntime) and must build
// without third-party/pyrowave checked out, so it can't include pyrowave.h.
// These declarations mirror it in their own namespace. PyrowaveRuntime only
// accepts exactly 101.0.0, and pyrowaveruntime.cpp checks the layouts below
// against the real header whenever the submodule is present.
//
// Only include this with HAVE_LIBPLACEBO_VULKAN, which provides the Vulkan
// headers.

#include <vulkan/vulkan.h>

#include <cstddef>
#include <cstdint>

namespace PyrowaveAbi {

// C enums are int-sized on every platform Moonlight supports.
typedef int pyrowave_result;
enum : int
{
    PYROWAVE_SUCCESS = 0,
    PYROWAVE_TIMEOUT = 1,
    PYROWAVE_ERROR_GENERIC = -1,
    PYROWAVE_ERROR_INVALID_ARGUMENT = -2,
    PYROWAVE_ERROR_OUT_OF_HOST_MEMORY = -3,
    PYROWAVE_ERROR_OUT_OF_DEVICE_MEMORY = -4,
    PYROWAVE_ERROR_NO_VULKAN = -5,
    PYROWAVE_ERROR_NOT_IMPLEMENTED = -6,
};

typedef int pyrowave_chroma_subsampling;
enum : int
{
    PYROWAVE_CHROMA_SUBSAMPLING_420 = 0,
    PYROWAVE_CHROMA_SUBSAMPLING_444 = 1,
};

typedef int pyrowave_color_primaries;
enum : int
{
    PYROWAVE_COLOR_PRIMARIES_BT709 = 0,
    PYROWAVE_COLOR_PRIMARIES_BT2020 = 1,
};

typedef int pyrowave_transfer_function;
enum : int
{
    PYROWAVE_TRANSFER_BT709 = 0,
    PYROWAVE_TRANSFER_PQ = 1,
    PYROWAVE_TRANSFER_HLG = 2,
};

typedef int pyrowave_ycbcr_transform;
enum : int
{
    PYROWAVE_YCBCR_BT709 = 0,
    PYROWAVE_YCBCR_BT2020 = 1,
};

typedef int pyrowave_ycbcr_range;
enum : int
{
    PYROWAVE_YCBCR_FULL = 0,
    PYROWAVE_YCBCR_LIMITED = 1,
};

// pyrowave_color_metadata::chroma_siting values (pyrowave_bitstream_header.hpp).
enum : uint32_t
{
    PYROWAVE_CHROMA_SITING_CENTER = 0,
    PYROWAVE_CHROMA_SITING_LEFT = 1,
};

struct pyrowave_decoder_opaque;
struct pyrowave_device_opaque;
struct pyrowave_image_opaque;
typedef pyrowave_decoder_opaque* pyrowave_decoder;
typedef pyrowave_device_opaque* pyrowave_device;
typedef pyrowave_image_opaque* pyrowave_image;

struct pyrowave_device_create_queue_info
{
    VkQueue queue;
    uint32_t familyIndex;
    uint32_t index;
};

typedef void (*pyrowave_queue_lock_cb)(void* userdata);

struct pyrowave_device_create_info
{
    PFN_vkGetInstanceProcAddr GetInstanceProcAddr;
    VkInstance instance;
    VkPhysicalDevice physical_device;
    VkDevice device;
    // Must stay valid for the lifetime of the pyrowave_device.
    const VkInstanceCreateInfo* instance_create_info;
    const VkDeviceCreateInfo* device_create_info;
    pyrowave_device_create_queue_info* queue_info;
    uint32_t queue_info_count;
    pyrowave_queue_lock_cb queue_lock_callback;
    pyrowave_queue_lock_cb queue_unlock_callback;
    void* userdata;
};

struct pyrowave_image_view
{
    VkImage image;
    uint32_t width;
    uint32_t height;
    VkFormat image_format;
    VkFormat view_format;
    uint32_t mip_level;
    uint32_t layer;
    VkImageAspectFlagBits aspect;
    VkComponentSwizzle swizzle;
    VkImageLayout layout;
};

struct pyrowave_color_metadata
{
    pyrowave_color_primaries primaries;
    pyrowave_transfer_function transfer;
    pyrowave_ycbcr_transform transform;
    pyrowave_ycbcr_range range;
    uint32_t chroma_siting;
};

struct pyrowave_sync_point
{
    VkSemaphore semaphore;
    // 0 for a binary semaphore, otherwise the timeline value.
    uint64_t value;
};

struct pyrowave_gpu_external_reference
{
    pyrowave_image image;
    uint32_t queue_family_index;
};

struct pyrowave_gpu_buffers
{
    pyrowave_image_view planes[3];
};

struct pyrowave_gpu_sync_operation
{
    const pyrowave_gpu_external_reference* images;
    size_t num_images;
    pyrowave_sync_point sync;
};

// Only the 4:2:0 8-bit format of the CPU path is used (SDR CPU staging).
typedef int pyrowave_cpu_buffer_format;
enum : int
{
    PYROWAVE_CPU_BUFFER_FORMAT_YUV420P = 1,
};

struct pyrowave_cpu_buffer
{
    void* data[3];
    size_t row_stride_in_bytes[3];
    size_t plane_size_in_bytes[3];
    int width;
    int height;
    pyrowave_cpu_buffer_format format;
};

struct pyrowave_decoder_create_info
{
    pyrowave_device device;
    int width;
    int height;
    pyrowave_chroma_subsampling chroma;
    bool fragment_path;
};

typedef pyrowave_result (*PFN_pyrowave_create_device)(const pyrowave_device_create_info*,
                                                      pyrowave_device*);
typedef void (*PFN_pyrowave_device_destroy)(pyrowave_device);
typedef pyrowave_result (*PFN_pyrowave_device_set_queue_type)(pyrowave_device, VkQueueFlagBits);
typedef bool (*PFN_pyrowave_decoder_device_prefers_fragment_path)(pyrowave_device);
typedef pyrowave_result (*PFN_pyrowave_decoder_create)(const pyrowave_decoder_create_info*,
                                                       pyrowave_decoder*);
typedef void (*PFN_pyrowave_decoder_clear)(pyrowave_decoder);
typedef pyrowave_result (*PFN_pyrowave_decoder_push_packet)(pyrowave_decoder, const void*, size_t);
typedef bool (*PFN_pyrowave_decoder_decode_is_ready)(pyrowave_decoder, bool);
typedef pyrowave_result (*PFN_pyrowave_decoder_decode_gpu_buffer)(
    pyrowave_decoder, const pyrowave_gpu_sync_operation*, const pyrowave_gpu_sync_operation*,
    const pyrowave_gpu_buffers*);
typedef pyrowave_result (*PFN_pyrowave_decoder_decode_cpu_buffer_synchronous)(
    pyrowave_decoder, const pyrowave_cpu_buffer*);
typedef bool (*PFN_pyrowave_decoder_get_color_metadata)(pyrowave_decoder, pyrowave_color_metadata*);
typedef void (*PFN_pyrowave_decoder_destroy)(pyrowave_decoder);

}

#ifdef PYROWAVE_ABI_CHECK
// Included by pyrowaveruntime.cpp only. The real header declares the same
// names in the global namespace, so nothing else may see both.
#if defined(__has_include)
#if __has_include("../../../../third-party/pyrowave/pyrowave.h")
#include "../../../../third-party/pyrowave/pyrowave.h"

#include <cstddef>

#define PYROWAVE_ABI_SAME_SIZE(type)                                                               \
    static_assert(sizeof(PyrowaveAbi::type) == sizeof(::type), #type " size differs")
#define PYROWAVE_ABI_SAME_OFFSET(type, field)                                                      \
    static_assert(offsetof(PyrowaveAbi::type, field) == offsetof(::type, field),                   \
                  #type "::" #field " offset differs")

static_assert(PYROWAVE_API_VERSION_MAJOR == 101 && PYROWAVE_API_VERSION_MINOR == 0 &&
                  PYROWAVE_API_VERSION_PATCH == 0,
              "pyrowaveabi.h mirrors pyrowave.h 101.0.0; update it with the submodule");
static_assert(sizeof(::pyrowave_result) == sizeof(int), "pyrowave enums are expected to be int");
static_assert(sizeof(::pyrowave_color_primaries) == sizeof(int), "enum size differs");

PYROWAVE_ABI_SAME_SIZE(pyrowave_device_create_queue_info);
PYROWAVE_ABI_SAME_SIZE(pyrowave_device_create_info);
PYROWAVE_ABI_SAME_OFFSET(pyrowave_device_create_info, instance_create_info);
PYROWAVE_ABI_SAME_OFFSET(pyrowave_device_create_info, queue_info_count);
PYROWAVE_ABI_SAME_OFFSET(pyrowave_device_create_info, userdata);
PYROWAVE_ABI_SAME_SIZE(pyrowave_image_view);
PYROWAVE_ABI_SAME_OFFSET(pyrowave_image_view, aspect);
PYROWAVE_ABI_SAME_OFFSET(pyrowave_image_view, layout);
PYROWAVE_ABI_SAME_SIZE(pyrowave_color_metadata);
PYROWAVE_ABI_SAME_OFFSET(pyrowave_color_metadata, chroma_siting);
PYROWAVE_ABI_SAME_SIZE(pyrowave_sync_point);
PYROWAVE_ABI_SAME_SIZE(pyrowave_gpu_external_reference);
PYROWAVE_ABI_SAME_SIZE(pyrowave_gpu_buffers);
PYROWAVE_ABI_SAME_SIZE(pyrowave_gpu_sync_operation);
PYROWAVE_ABI_SAME_OFFSET(pyrowave_gpu_sync_operation, sync);
PYROWAVE_ABI_SAME_SIZE(pyrowave_cpu_buffer);
PYROWAVE_ABI_SAME_OFFSET(pyrowave_cpu_buffer, plane_size_in_bytes);
PYROWAVE_ABI_SAME_OFFSET(pyrowave_cpu_buffer, format);
PYROWAVE_ABI_SAME_SIZE(pyrowave_decoder_create_info);
PYROWAVE_ABI_SAME_OFFSET(pyrowave_decoder_create_info, fragment_path);

static_assert(PyrowaveAbi::PYROWAVE_YCBCR_LIMITED == ::PYROWAVE_YCBCR_LIMITED, "enum differs");
static_assert(PyrowaveAbi::PYROWAVE_TRANSFER_BT709 == ::PYROWAVE_TRANSFER_BT709, "enum differs");
static_assert(PyrowaveAbi::PYROWAVE_CPU_BUFFER_FORMAT_YUV420P == ::PYROWAVE_CPU_BUFFER_FORMAT_YUV420P,
              "enum differs");

#undef PYROWAVE_ABI_SAME_SIZE
#undef PYROWAVE_ABI_SAME_OFFSET
#endif
#endif
#endif
