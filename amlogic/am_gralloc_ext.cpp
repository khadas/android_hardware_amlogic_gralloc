/*
 * Copyright (c) 2017 Amlogic, Inc. All rights reserved.
 *
 * This source code is subject to the terms and conditions defined in the
 * file 'LICENSE' which is part of this source code package.
 *
 * Description:
 */

#include "am_gralloc_ext.h"
#include <hardware/gralloc1.h>
#include <gralloc_priv.h>
#include <android/hardware/graphics/common/1.0/types.h>

using android::hardware::graphics::common::V1_0::BufferUsage;

uint64_t am_gralloc_get_video_overlay_producer_usage() {
    return static_cast<uint64_t>(BufferUsage::VIDEO_DECODER);
}

uint64_t am_gralloc_get_omx_metadata_producer_usage() {
    return static_cast<uint64_t>(BufferUsage::VIDEO_DECODER
        | BufferUsage::CPU_READ_OFTEN
        | BufferUsage::CPU_WRITE_OFTEN);
}

uint64_t am_gralloc_get_omx_osd_producer_usage() {
    return static_cast<uint64_t>(BufferUsage::VIDEO_DECODER
        | BufferUsage::GPU_RENDER_TARGET);
}

bool am_gralloc_is_omx_metadata_producer(uint64_t usage) {
    if (!am_gralloc_is_omx_osd_producer(usage)) {
        uint64_t omx_metadata_usage = am_gralloc_get_omx_metadata_producer_usage();
        if (((usage & omx_metadata_usage) == omx_metadata_usage)
            && !(usage & BufferUsage::GPU_RENDER_TARGET)) {
            return true;
        }
    }

    return false;
}

bool am_gralloc_is_omx_osd_producer(uint64_t usage) {
    uint64_t omx_osd_usage = am_gralloc_get_omx_osd_producer_usage();
    if ((usage & omx_osd_usage) == omx_osd_usage) {
        return true;
    }

    return false;
}


int am_gralloc_get_format(const native_handle_t * hnd) {
    private_handle_t const* buffer = hnd ? private_handle_t::dynamicCast(hnd) : NULL;
    if (buffer)
        return buffer->format;

    return -ENOMEM;
}

int am_gralloc_get_buffer_fd(const native_handle_t * hnd) {
    private_handle_t const* buffer = hnd ? private_handle_t::dynamicCast(hnd) : NULL;
    if (buffer)
        return buffer->share_fd;

    return -1;
}

int am_gralloc_get_stride_in_byte(const native_handle_t * hnd) {
    private_handle_t const* buffer = hnd ? private_handle_t::dynamicCast(hnd) : NULL;
    if (buffer)
        return buffer->byte_stride;

    return 0;
}

int am_gralloc_get_stride_in_pixel(const native_handle_t * hnd) {
    private_handle_t const* buffer = hnd ? private_handle_t::dynamicCast(hnd) : NULL;
    if (buffer)
        return buffer->stride;

    return 0;
}

bool am_gralloc_is_secure_buffer(const native_handle_t *hnd) {
    private_handle_t const* buffer = hnd ? private_handle_t::dynamicCast(hnd) : NULL;
     if (NULL == buffer)
        return true;

     if (buffer->flags & private_handle_t::PRIV_FLAGS_SECURE_PROTECTED)
         return true;

     return false;
}

bool am_gralloc_is_coherent_buffer(const native_handle_t * hnd) {
    private_handle_t const* buffer = hnd ? private_handle_t::dynamicCast(hnd) : NULL;
    if ((buffer->flags & private_handle_t::PRIV_FLAGS_CONTINUOUS_BUF)
            || (buffer->flags & private_handle_t::PRIV_FLAGS_USES_ION_DMA_HEAP)) {
        return true;
    }

    return false;
}

bool am_gralloc_is_overlay_buffer(const native_handle_t * hnd) {
     private_handle_t const* buffer = hnd ? private_handle_t::dynamicCast(hnd) : NULL;
     if (buffer && (buffer->flags & private_handle_t::PRIV_FLAGS_VIDEO_OVERLAY))
         return true;

     return false;
}

bool am_gralloc_is_omx_metadata_buffer(
    const native_handle_t * hnd) {
    private_handle_t const* buffer = hnd ? private_handle_t::dynamicCast(hnd) : NULL;
    if (buffer && (buffer->flags & private_handle_t::PRIV_FLAGS_VIDEO_OMX))
        return true;

    return false;
 }

 typedef struct am_sideband_handle {
    native_handle_t base;
    int id;
    int flags;
 } am_sideband_handle_t;

#define AM_SIDEBAND_HANDLE_NUM_INT (2)
#define AM_SIDEBAND_HANDLE_NUM_FD (0)
#define AM_SIDEBAND_IDENTIFIER (0xabcdcdef)

native_handle_t * am_gralloc_create_sideband_handle(int type) {
    am_sideband_handle_t * pHnd = (am_sideband_handle_t *)
        native_handle_create(AM_SIDEBAND_HANDLE_NUM_FD,
        AM_SIDEBAND_HANDLE_NUM_INT);
    pHnd->id = AM_SIDEBAND_IDENTIFIER;

    if (type == AM_TV_SIDEBAND) {
        pHnd->flags = private_handle_t::PRIV_FLAGS_VIDEO_OVERLAY;
    } else if (type == AM_OMX_SIDEBAND) {
        pHnd->flags = private_handle_t::PRIV_FLAGS_VIDEO_OMX;
    }

    return (native_handle_t *)pHnd;
}

int am_gralloc_destroy_sideband_handle(native_handle_t * hnd) {
    if (hnd) {
        native_handle_delete(hnd);
    }

    return 0;
}

int am_gralloc_get_vpu_afbc_mask(const native_handle_t * hnd) {
    private_handle_t const* buffer = hnd ? private_handle_t::dynamicCast(hnd) : NULL;

    if (buffer) {
        int internalFormat = buffer->internal_format;
        int afbcFormat = 0;

        if (internalFormat & MALI_GRALLOC_INTFMT_AFBCENABLE_MASK) {
            afbcFormat |=
                (VPU_AFBC_EN | VPU_AFBC_YUV_TRANSFORM |VPU_AFBC_BLOCK_SPLIT);

            if (internalFormat & MALI_GRALLOC_INTFMT_AFBC_WIDEBLK) {
                afbcFormat |= VPU_AFBC_SUPER_BLOCK_ASPECT;
            }

            if (internalFormat & MALI_GRALLOC_INTFMT_AFBC_SPLITBLK) {
                afbcFormat |= VPU_AFBC_BLOCK_SPLIT;
            }

            /*if (internalFormat & MALI_GRALLOC_FORMAT_CAPABILITY_AFBC_WIDEBLK_YUV_DISABLE) {
                afbcFormat &= ~VPU_AFBC_YUV_TRANSFORM;
            }*/

            if (internalFormat & MALI_GRALLOC_INTFMT_AFBC_TILED_HEADERS) {
                afbcFormat |= VPU_AFBC_TILED_HEADER_EN;
            }
        }

        return afbcFormat;
    }

    return 0;
}

