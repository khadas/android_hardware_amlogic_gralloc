/*
 * Copyright (C) 2016-2020 ARM Limited. All rights reserved.
 *
 * Copyright (C) 2008 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <string.h>
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdlib.h>
#include <limits.h>

#include <log/log.h>
#include <cutils/atomic.h>

#ifdef GRALLOC_AML_EXTEND
#ifdef BUILD_KERNEL_4_9
#include <ion/ion.h>
#include "ion/ion_4.12.h"
#else
#include "ion/ion_5.4.h"
#include "ion/ion_54.h"
#endif
#endif

#include <linux/dma-buf.h>
#include <vector>
#include <sys/ioctl.h>

#include <hardware/hardware.h>
#include <hardware/gralloc1.h>

#include "mali_gralloc_private_interface_types.h"
#include "mali_gralloc_buffer.h"
#include "gralloc_helper.h"
#include "1.x/framebuffer_device.h"
#include "mali_gralloc_formats.h"
#include "mali_gralloc_usages.h"
#include "core/mali_gralloc_bufferdescriptor.h"
#include "core/mali_gralloc_bufferallocation.h"

#ifdef GRALLOC_AML_EXTEND
#include "mali_gralloc_ion.h"
#include <map>

#ifdef BUILD_KERNEL_4_9
static std::map<int, ion_user_handle_t> imported_user_hnd;
static std::map<int, int> imported_ion_client;
#endif

#endif

//meson graphics changes start
#ifdef GRALLOC_AML_EXTEND
#include <am_gralloc_internal.h>
ion_heap_type am_gralloc_pick_ion_heap(
	buffer_descriptor_t *bufDescriptor, uint64_t usage);
void am_gralloc_set_ion_flags(enum ion_heap_type heap_type, uint64_t usage,
	unsigned int *priv_heap_flag, unsigned int *ion_flags);

#define V4L2_DECODER_BUFFER_MAX_WIDTH       4096
#define V4L2_DECODER_BUFFER_MAX_HEIGHT      2304
#define V4L2_DECODER_BUFFER_8k_MAX_WIDTH       8192
#define V4L2_DECODER_BUFFER_8k_MAX_HEIGHT      4352

#endif
//meson graphics changes end


#define INIT_ZERO(obj) (memset(&(obj), 0, sizeof((obj))))

#define HEAP_MASK_FROM_ID(id) (1 << id)
#define HEAP_MASK_FROM_TYPE(type) (1 << type)

static const enum ion_heap_type ION_HEAP_TYPE_INVALID = ((enum ion_heap_type)~0);
static const enum ion_heap_type ION_HEAP_TYPE_SECURE = (enum ion_heap_type)(((unsigned int)ION_HEAP_TYPE_CUSTOM) + 1);

#if defined(ION_HEAP_SECURE_MASK)
#if (HEAP_MASK_FROM_TYPE(ION_HEAP_TYPE_SECURE) != ION_HEAP_SECURE_MASK)
#error "ION_HEAP_TYPE_SECURE value is not compatible with ION_HEAP_SECURE_MASK"
#endif
#endif

struct ion_device
{
	int client()
	{
		return ion_client;
	}
	bool use_legacy()
	{
		return use_legacy_ion;
	}

	static void close()
	{
		ion_device &dev = get_inst();
		if (dev.ion_client >= 0)
		{
			ion_close(dev.ion_client);
			dev.ion_client = -1;
		}
	}

	static ion_device *get()
	{
		ion_device &dev = get_inst();
		if (dev.ion_client < 0)
		{
			if (dev.open_and_query_ion() != 0)
			{
				close();
			}
		}

		if (dev.ion_client < 0)
		{
			return nullptr;
		}
		return &dev;
	}

#ifdef GRALLOC_AML_EXTEND
    static void close_uvm()
    {
        ion_device &dev = get_inst();
        if (dev.uvm_dev >= 0)
        {
            ::close(dev.uvm_dev);
            dev.uvm_dev = -1;
        }
    }

    static int get_uvm()
    {
        ion_device &dev = get_inst();
        if (dev.uvm_dev < 0)
        {
            dev.uvm_dev = open("/dev/uvm", O_RDONLY | O_CLOEXEC);
        }

        if (dev.uvm_dev < 0)
        {
            return -1;
        }
        return dev.uvm_dev;
    }
#endif

	/*
	 *  Identifies a heap and retrieves file descriptor from ION for allocation
	 *
	 * @param usage     [in]    Producer and consumer combined usage.
	 * @param size      [in]    Requested buffer size (in bytes).
	 * @param heap_type [in]    Requested heap type.
	 * @param flags     [in]    ION allocation attributes defined by ION_FLAG_*.
	 * @param min_pgsz  [out]   Minimum page size (in bytes).
	 *
	 * @return File handle which can be used for allocation, on success
	 *         -1, otherwise.
	 */
#ifdef GRALLOC_AML_EXTEND
        int alloc_from_ion_heap(uint64_t usage, size_t size, enum ion_heap_type *ptype, unsigned int flags,
                                int *min_pgsz);
#else
        int alloc_from_ion_heap(uint64_t usage, size_t size, enum ion_heap_type heap_type, unsigned int flags,
                                int *min_pgsz);
#endif


	enum ion_heap_type pick_ion_heap(uint64_t usage);
	bool check_buffers_sharable(const gralloc_buffer_descriptor_t *descriptors, uint32_t numDescriptors);

private:
	int ion_client;
#ifdef GRALLOC_AML_EXTEND
	int uvm_dev;
#endif
	bool use_legacy_ion;
	bool secure_heap_exists;
	/*
	* Cache the heap types / IDs information to avoid repeated IOCTL calls
	* Assumption: Heap types / IDs would not change after boot up.
	*/
	int heap_cnt;
	ion_heap_data heap_info[ION_NUM_HEAP_IDS];

	ion_device()
	    : ion_client(-1)
#ifdef GRALLOC_AML_EXTEND
	    , uvm_dev(-1)
#endif
	    , use_legacy_ion(false)
	    , secure_heap_exists(false)
	    , heap_cnt(0)
	{
	}

	static ion_device& get_inst()
	{
		static ion_device dev;
		return dev;
	}

	/*
	 * Opens the ION module. Queries heap information and stores it for later use
	 *
	 * @return              0 in case of success
	 *                      -1 for all error cases
	 */
	int open_and_query_ion();
};

#ifdef GRALLOC_AML_EXTEND
	//workaround for unsupported 4k video play on some platforms
#include <cutils/properties.h>
static int am_gralloc_exec_omx_policy(int size, int scalar) {
	char prop[PROPERTY_VALUE_MAX];

	size /= scalar * scalar;
	if (property_get("ro.vendor.platform.support.4k", prop, NULL) > 0)
	{
		if (strstr(prop, "false"))
		{
			size = (GRALLOC_ALIGN(1920/scalar, 64) * GRALLOC_ALIGN(1080/scalar, 64)) * 3 / 2;
		}
	}
	return size;
}
#endif

static void set_ion_flags(enum ion_heap_type heap_type, uint64_t usage,
                          unsigned int *priv_heap_flag, unsigned int *ion_flags)
{
#if !defined(GRALLOC_USE_ION_DMA_HEAP) || !GRALLOC_USE_ION_DMA_HEAP
	GRALLOC_UNUSED(heap_type);
#endif

	if (priv_heap_flag)
	{
#if defined(GRALLOC_USE_ION_DMA_HEAP) && GRALLOC_USE_ION_DMA_HEAP
		if (heap_type == ION_HEAP_TYPE_DMA)
		{
			*priv_heap_flag = private_handle_t::PRIV_FLAGS_USES_ION_DMA_HEAP;
		}
#endif
	}

	if (ion_flags)
	{
#if defined(GRALLOC_USE_ION_DMA_HEAP) && GRALLOC_USE_ION_DMA_HEAP
		if (heap_type != ION_HEAP_TYPE_DMA)
		{
#endif
			if ((usage & GRALLOC_USAGE_SW_READ_MASK) == GRALLOC_USAGE_SW_READ_OFTEN)
			{
				*ion_flags = ION_FLAG_CACHED | ION_FLAG_CACHED_NEEDS_SYNC;
			}
#if defined(GRALLOC_USE_ION_DMA_HEAP) && GRALLOC_USE_ION_DMA_HEAP
		}
#endif
	}
}

#ifdef GRALLOC_AML_EXTEND
int ion_device::alloc_from_ion_heap(uint64_t usage, size_t size, enum ion_heap_type *ptype, unsigned int flags,
                                    int *min_pgsz)
#else
int ion_device::alloc_from_ion_heap(uint64_t usage, size_t size, enum ion_heap_type heap_type, unsigned int flags,
                                    int *min_pgsz)
#endif
{
	int shared_fd = -1;
	int ret = -1;
//meson graphics changes start
#ifdef GRALLOC_AML_EXTEND
   enum ion_heap_type heap_type = *ptype;
#endif
//meson graphics changes end

	if (ion_client < 0 ||
	    size <= 0 ||
	    heap_type == ION_HEAP_TYPE_INVALID ||
	    min_pgsz == NULL)
	{
		return -1;
	}

	if (heap_type == ION_HEAP_TYPE_CUSTOM ||
		heap_type == ION_HEAP_TYPE_DMA)
		flags |= ION_FLAG_EXTEND_MESON_HEAP;

	bool system_heap_exist = false;

	if (use_legacy_ion == false)
	{
		int i = 0;
		bool is_heap_matched = false;

		/* Attempt to allocate memory from each matching heap type (of
		 * enumerated heaps) until successful
		 */
		do
		{
			if (heap_type == heap_info[i].type)
			{
				is_heap_matched = true;
				ret = ion_alloc_fd(ion_client, size, 0,
				                   HEAP_MASK_FROM_ID(heap_info[i].heap_id),
				                   flags, &shared_fd);
			}

			if (heap_info[i].type == ION_HEAP_TYPE_SYSTEM)
			{
				system_heap_exist = true;
			}

			i++;
		} while ((ret < 0) && (i < heap_cnt));

		if (is_heap_matched == false)
		{
			MALI_GRALLOC_LOGE("Failed to find matching ION heap. Trying to fall back on system heap");
		}
	}
	else
	{
		/* This assumes that when the heaps were defined, the heap ids were
		 * defined as (1 << type) and that ION interprets the heap_mask as
		 * (1 << type).
		 */
		unsigned int heap_mask = HEAP_MASK_FROM_TYPE(heap_type);

		ret = ion_alloc_fd(ion_client, size, 0, heap_mask, flags, &shared_fd);
	}

	/* Check if allocation from selected heap failed and fall back to system
	 * heap if possible.
	 */
	if (ret < 0)
	{
		/* Don't allow falling back to sytem heap if secure was requested. */
		if (heap_type == ION_HEAP_TYPE_SECURE)
		{
			return -1;
		}
		/* hw fb canot fallback to system heap.*/
		if (usage & GRALLOC_USAGE_HW_FB) {
			MALI_GRALLOC_LOGE("GRALLOC_USAGE_HW_FB Allocation failed on on dma heap. Cannot fallback.");
			return -1;
		}

		/* Can't fall back to system heap if system heap was the heap that
		 * already failed
		 */
		if (heap_type == ION_HEAP_TYPE_SYSTEM)
		{
			MALI_GRALLOC_LOGE("%s: Allocation failed on on system heap. Cannot fallback.", __func__);
			return -1;
		}

		heap_type = ION_HEAP_TYPE_SYSTEM;

		/* Set ION flags for system heap allocation */
//meson graphics changes start
#ifdef GRALLOC_AML_EXTEND
                am_gralloc_set_ion_flags(heap_type, usage, NULL, &flags);
#else
                set_ion_flags(heap_type, usage, NULL, &flags);
#endif
//meson graphics changes end

		if (use_legacy_ion == false)
		{
			int i = 0;

			if (system_heap_exist == false)
			{
				MALI_GRALLOC_LOGE("%s: System heap not available for fallback", __func__);
				return -1;
			}

			/* Attempt to allocate memory from each system heap type (of
			 * enumerated heaps) until successful
			 */
			do
			{
				if (heap_info[i].type == ION_HEAP_TYPE_SYSTEM)
				{
					ret = ion_alloc_fd(ion_client, size, 0,
					                   HEAP_MASK_FROM_ID(heap_info[i].heap_id),
					                   flags, &shared_fd);
				}

				i++;
			} while ((ret < 0) && (i < heap_cnt));
		}
		else /* Use legacy ION API */
		{
			ret = ion_alloc_fd(ion_client, size, 0,
			                   HEAP_MASK_FROM_TYPE(heap_type),
			                   flags, &shared_fd);
		}

		if (ret != 0)
		{
			MALI_GRALLOC_LOGE("Fallback ion_alloc_fd(%d, %zd, %d, %u, %p) failed",
			      ion_client, size, 0, flags, &shared_fd);
			return -1;
		}
	}

	switch (heap_type)
	{
	case ION_HEAP_TYPE_SYSTEM:
		*min_pgsz = SZ_4K;
		break;

	case ION_HEAP_TYPE_SYSTEM_CONTIG:
	case ION_HEAP_TYPE_CARVEOUT:
#if defined(GRALLOC_USE_ION_DMA_HEAP) && GRALLOC_USE_ION_DMA_HEAP
	case ION_HEAP_TYPE_DMA:
		*min_pgsz = size;
		break;
#endif
#if defined(GRALLOC_USE_ION_COMPOUND_PAGE_HEAP) && GRALLOC_USE_ION_COMPOUND_PAGE_HEAP
	case ION_HEAP_TYPE_COMPOUND_PAGE:
		*min_pgsz = SZ_2M;
		break;
#endif
	/* If have customized heap please set the suitable pg type according to
	 * the customized ION implementation
	 */
	case ION_HEAP_TYPE_CUSTOM:
		*min_pgsz = SZ_4K;
		break;
	default:
		*min_pgsz = SZ_4K;
		break;
	}

//meson graphics changes start
#ifdef GRALLOC_AML_EXTEND
*ptype = heap_type;
#endif
//meson graphics changes end

	return shared_fd;
}

enum ion_heap_type ion_device::pick_ion_heap(uint64_t usage)
{
	enum ion_heap_type heap_type = ION_HEAP_TYPE_INVALID;

	if (usage & GRALLOC_USAGE_PROTECTED)
	{
		if (secure_heap_exists)
		{
			heap_type = ION_HEAP_TYPE_SECURE;
		}
		else
		{
			MALI_GRALLOC_LOGE("Protected ION memory is not supported on this platform.");
		}
	}
	else if (!(usage & GRALLOC_USAGE_HW_VIDEO_ENCODER) && (usage & GRALLOC_USAGE_HW_FB))
	{
#if defined(GRALLOC_USE_ION_COMPOUND_PAGE_HEAP) && GRALLOC_USE_ION_COMPOUND_PAGE_HEAP
		heap_type = ION_HEAP_TYPE_COMPOUND_PAGE;
#elif defined(GRALLOC_USE_ION_DMA_HEAP) && GRALLOC_USE_ION_DMA_HEAP
		heap_type = ION_HEAP_TYPE_DMA;
#else
		heap_type = ION_HEAP_TYPE_SYSTEM;
#endif
	}
	else
	{
		heap_type = ION_HEAP_TYPE_SYSTEM;
	}

	return heap_type;
}


bool ion_device::check_buffers_sharable(const gralloc_buffer_descriptor_t *descriptors,
                                   uint32_t numDescriptors)
{
	enum ion_heap_type shared_backend_heap_type = ION_HEAP_TYPE_INVALID;
	unsigned int shared_ion_flags = 0;
	uint64_t usage;
	uint32_t i;

	if (numDescriptors <= 1)
	{
		return false;
	}

	for (i = 0; i < numDescriptors; i++)
	{
		unsigned int ion_flags;
		enum ion_heap_type heap_type;

		buffer_descriptor_t *bufDescriptor = (buffer_descriptor_t *)descriptors[i];

		usage = bufDescriptor->consumer_usage | bufDescriptor->producer_usage;

//meson graphics changes start
#ifdef GRALLOC_AML_EXTEND
        heap_type = am_gralloc_pick_ion_heap(bufDescriptor, usage);
#else
        heap_type = pick_ion_heap(usage);
#endif
//meson graphics changes end

		if (heap_type == ION_HEAP_TYPE_INVALID)
		{
			return false;
		}

//meson graphics changes start
#ifdef GRALLOC_AML_EXTEND
        am_gralloc_set_ion_flags(heap_type, usage, NULL, &ion_flags);
#else
        set_ion_flags(heap_type, usage, NULL, &ion_flags);
#endif
//meson graphics changes end


		if (shared_backend_heap_type != ION_HEAP_TYPE_INVALID)
		{
			if (shared_backend_heap_type != heap_type || shared_ion_flags != ion_flags)
			{
				return false;
			}
		}
		else
		{
			shared_backend_heap_type = heap_type;
			shared_ion_flags = ion_flags;
		}
	}

	return true;
}

static int get_max_buffer_descriptor_index(const gralloc_buffer_descriptor_t *descriptors, uint32_t numDescriptors)
{
	uint32_t i, max_buffer_index = 0;
	size_t max_buffer_size = 0;

	for (i = 0; i < numDescriptors; i++)
	{
		buffer_descriptor_t *bufDescriptor = (buffer_descriptor_t *)descriptors[i];

		if (max_buffer_size < bufDescriptor->size)
		{
			max_buffer_index = i;
			max_buffer_size = bufDescriptor->size;
		}
	}

	return max_buffer_index;
}

int ion_device::open_and_query_ion()
{
	int ret = -1;
	if (ion_client >= 0)
	{
		MALI_GRALLOC_LOGW("ION device already open");
		return 0;
	}

	ion_client = ion_open();
	if (ion_client < 0)
	{
		MALI_GRALLOC_LOGE("ion_open failed with %s", strerror(errno));
		return -1;
	}

	INIT_ZERO(heap_info);
	heap_cnt = 0;

#ifdef GRALLOC_AML_EXTEND

#ifdef BUILD_KERNEL_4_9
	use_legacy_ion = true;
#else
	use_legacy_ion = false;
#endif

#else
	use_legacy_ion = (ion_is_legacy(ion_client) != 0);
#endif

	if (use_legacy_ion == false)
	{
		int cnt;
		ret = ion_query_heap_cnt(ion_client, &cnt);
		if (ret == 0)
		{
			if (cnt > (int)ION_NUM_HEAP_IDS)
			{
				MALI_GRALLOC_LOGE("Retrieved heap count %d is more than maximun heaps %zu on ion",
				      cnt, ION_NUM_HEAP_IDS);
				return -1;
			}

			std::vector<struct ion_heap_data> heap_data(cnt);
			ret = ion_query_get_heaps(ion_client, cnt, heap_data.data());
			if (ret == 0)
			{
				int heap_info_idx = 0;
				for (std::vector<struct ion_heap_data>::iterator heap = heap_data.begin();
                                            heap != heap_data.end(); heap++)
				{
					if (heap_info_idx >= (int)ION_NUM_HEAP_IDS)
					{
						MALI_GRALLOC_LOGE("Iterator exceeding max index, cannot cache heap information");
						return -1;
					}

					if (strcmp(heap->name, "ion_protected_heap") == 0)
					{
						heap->type = ION_HEAP_TYPE_SECURE;
						secure_heap_exists = true;
					}

					heap_info[heap_info_idx] = *heap;
					heap_info_idx++;
				}
			}
		}
		if (ret < 0)
		{
			MALI_GRALLOC_LOGE("%s: Failed to query ION heaps.", __func__);
			return ret;
		}

		heap_cnt = cnt;
	}
	else
	{
#if defined(ION_HEAP_SECURE_MASK)
		secure_heap_exists = true;
#endif
	}

	return 0;
}


/*
 * Signal start of CPU access to the DMABUF exported from ION.
 *
 * @param hnd   [in]    Buffer handle
 * @param read  [in]    Flag indicating CPU read access to memory
 * @param write [in]    Flag indicating CPU write access to memory
 *
 * @return              0 in case of success
 *                      errno for all error cases
 */
int mali_gralloc_ion_sync_start(const private_handle_t * const hnd,
                                const bool read,
                                const bool write)
{
	if (hnd == NULL)
	{
		return -EINVAL;
	}

	ion_device *dev = ion_device::get();
	if (!dev)
	{
		return -ENODEV;
	}

	GRALLOC_UNUSED(read);
	GRALLOC_UNUSED(write);

	switch (hnd->flags & private_handle_t::PRIV_FLAGS_USES_ION)
	{
	case private_handle_t::PRIV_FLAGS_USES_ION:
		if (!(hnd->flags & private_handle_t::PRIV_FLAGS_USES_ION_DMA_HEAP))
		{
			if (dev->use_legacy() == false)
			{
#if defined(GRALLOC_USE_ION_DMABUF_SYNC) && (GRALLOC_USE_ION_DMABUF_SYNC == 1)
				uint64_t flags = DMA_BUF_SYNC_START;
				if (read)
				{
					flags |= DMA_BUF_SYNC_READ;
				}
				if (write)
				{
					flags |= DMA_BUF_SYNC_WRITE;
				}

				const struct dma_buf_sync payload = { flags };

				int ret, retry = 5;
				do
				{
					ret = ioctl(hnd->share_fd, DMA_BUF_IOCTL_SYNC, &payload);
					retry--;
				} while ((ret == -EAGAIN || ret == -EINTR) && retry);

				if (ret < 0)
				{
					MALI_GRALLOC_LOGE("ioctl: 0x%" PRIx64 ", flags: 0x%" PRIx64 "failed with code %d: %s",
					     (uint64_t)DMA_BUF_IOCTL_SYNC, flags, ret, strerror(errno));
					return -errno;
				}
#endif
			}
			else
			{
				ion_sync_fd(dev->client(), hnd->share_fd);
			}
		}

		break;
	}

	return 0;
}


/*
 * Signal end of CPU access to the DMABUF exported from ION.
 *
 * @param hnd   [in]    Buffer handle
 * @param read  [in]    Flag indicating CPU read access to memory
 * @param write [in]    Flag indicating CPU write access to memory
 *
 * @return              0 in case of success
 *                      errno for all error cases
 */
int mali_gralloc_ion_sync_end(const private_handle_t * const hnd,
                              const bool read,
                              const bool write)
{
	if (hnd == NULL)
	{
		return -EINVAL;
	}

	ion_device *dev = ion_device::get();
	if (!dev)
	{
		return -ENODEV;
	}

	GRALLOC_UNUSED(read);
	GRALLOC_UNUSED(write);

	switch (hnd->flags & private_handle_t::PRIV_FLAGS_USES_ION)
	{
	case private_handle_t::PRIV_FLAGS_USES_ION:
		if (!(hnd->flags & private_handle_t::PRIV_FLAGS_USES_ION_DMA_HEAP))
		{
			if (dev->use_legacy() == false)
			{
#if defined(GRALLOC_USE_ION_DMABUF_SYNC) && (GRALLOC_USE_ION_DMABUF_SYNC == 1)
				uint64_t flags = DMA_BUF_SYNC_END;
				if (read)
				{
					flags |= DMA_BUF_SYNC_READ;
				}
				if (write)
				{
					flags |= DMA_BUF_SYNC_WRITE;
				}

				const struct dma_buf_sync payload = { flags };

				int ret, retry = 5;
				do
				{
					ret = ioctl(hnd->share_fd, DMA_BUF_IOCTL_SYNC, &payload);
					retry--;
				} while ((ret == -EAGAIN || ret == -EINTR) && retry);

				if (ret < 0)
				{
					MALI_GRALLOC_LOGE("ioctl: 0x%" PRIx64 ", flags: 0x%" PRIx64 "failed with code %d: %s",
					     (uint64_t)DMA_BUF_IOCTL_SYNC, flags, ret, strerror(errno));
					return -errno;
				}
#endif
			}
			else
			{
				ion_sync_fd(dev->client(), hnd->share_fd);
			}
		}
		break;
	}

	return 0;
}


void mali_gralloc_ion_free(private_handle_t * const hnd)
{
	if (hnd->flags & private_handle_t::PRIV_FLAGS_FRAMEBUFFER)
	{
		return;
	}
	else if (hnd->flags & private_handle_t::PRIV_FLAGS_USES_ION)
	{
		/* Buffer might be unregistered already so we need to assure we have a valid handle */
		if (hnd->base != 0)
		{
			if (munmap((void *)hnd->base, hnd->size) != 0)
			{
				MALI_GRALLOC_LOGE("Failed to munmap handle %p", hnd);
			}
		}

		close(hnd->share_fd);
		hnd->share_fd = -1;
	}
}

static void mali_gralloc_ion_free_internal(buffer_handle_t * const pHandle,
                                           const uint32_t num_hnds)
{
	for (uint32_t i = 0; i < num_hnds; i++)
	{
		if (pHandle[i] != NULL)
		{
			private_handle_t * const hnd = (private_handle_t * const)pHandle[i];
			mali_gralloc_ion_free(hnd);
		}
	}
}


/*
 *  Allocates ION buffers
 *
 * @param descriptors     [in]    Buffer request descriptors
 * @param numDescriptors  [in]    Number of descriptors
 * @param pHandle         [out]   Handle for each allocated buffer
 * @param shared_backend  [out]   Shared buffers flag
 *
 * @return File handle which can be used for allocation, on success
 *         -1, otherwise.
 */
int mali_gralloc_ion_allocate(const gralloc_buffer_descriptor_t *descriptors,
                              uint32_t numDescriptors, buffer_handle_t *pHandle,
                              bool *shared_backend)
{
	static int support_protected = 1;
	unsigned int priv_heap_flag = 0;
	enum ion_heap_type heap_type;
	unsigned char *cpu_ptr = NULL;
	uint64_t usage;
	uint32_t i, max_buffer_index = 0;
	int shared_fd;
	unsigned int ion_flags = 0;
	int min_pgsz = 0;
#ifdef GRALLOC_AML_EXTEND
	uint32_t delay_alloc = 0;
	int uvm_fd = -1;
	/* passed to UVM */
	uint32_t uvm_flags = UVM_IMM_ALLOC;
	int ret;
	int buf_scalar = 1;
	/* judge if it is allocated from UVM */
	int uvm_buffer_flag = 0;
	int v4l2_dec_max_buf_size =
			(V4L2_DECODER_BUFFER_MAX_WIDTH * V4L2_DECODER_BUFFER_MAX_HEIGHT) * 3 / 2;
#endif

	ion_device *dev = ion_device::get();
#ifdef GRALLOC_AML_EXTEND
    uvm_fd = ion_device::get_uvm();
#endif

	if (!dev)
	{
		return -1;
	}

	*shared_backend = dev->check_buffers_sharable(descriptors, numDescriptors);

	if (*shared_backend)
	{
		buffer_descriptor_t *max_bufDescriptor;

		max_buffer_index = get_max_buffer_descriptor_index(descriptors, numDescriptors);
		max_bufDescriptor = (buffer_descriptor_t *)(descriptors[max_buffer_index]);
		usage = max_bufDescriptor->consumer_usage | max_bufDescriptor->producer_usage;

//meson graphics changes start
#ifdef GRALLOC_AML_EXTEND
        heap_type = am_gralloc_pick_ion_heap(max_bufDescriptor, usage);
#else
        heap_type = dev->pick_ion_heap(usage);
#endif
//meson graphics changes end

		if (heap_type == ION_HEAP_TYPE_INVALID)
		{
			MALI_GRALLOC_LOGE("Failed to find an appropriate ion heap");
			return -1;
		}

//meson graphics changes start
#ifdef GRALLOC_AML_EXTEND
        am_gralloc_set_ion_flags(heap_type, usage, NULL, &ion_flags);

		if (am_gralloc_is_video_overlay_extend_usage(usage) ||
			am_gralloc_is_omx_metadata_extend_usage(usage) ||
			am_gralloc_is_omx_osd_extend_usage(usage)) {

			uvm_buffer_flag |= private_handle_t::PRIV_FLAGS_UVM_BUFFER;

			if (am_gralloc_is_omx_osd_extend_usage(usage) ||
				am_gralloc_is_video_decoder_full_buffer_usage(usage) ||
				am_gralloc_is_video_decoder_OSD_buffer_usage(usage)) {
				buf_scalar = 1;
			} else if (am_gralloc_is_video_decoder_quarter_buffer_usage(usage)) {
				buf_scalar = 2;
			} else if (am_gralloc_is_video_decoder_one_sixteenth_buffer_usage(usage)) {
				buf_scalar = 4;
			} else {
				uvm_flags = UVM_DELAY_ALLOC;
				delay_alloc = 1;
			}

			if (usage & GRALLOC_USAGE_PROTECTED) {
				uvm_flags |= UVM_USAGE_PROTECTED;
			}
			if ((max_bufDescriptor->width * max_bufDescriptor->height) > (V4L2_DECODER_BUFFER_MAX_WIDTH * V4L2_DECODER_BUFFER_MAX_HEIGHT)) {
				v4l2_dec_max_buf_size = V4L2_DECODER_BUFFER_8k_MAX_WIDTH * V4L2_DECODER_BUFFER_8k_MAX_HEIGHT * 3 / 2;
			}

#ifdef GRALLOC_AML_EXTEND
			//workaround for unsupported 4k video play on some platforms
			v4l2_dec_max_buf_size = am_gralloc_exec_omx_policy(v4l2_dec_max_buf_size, buf_scalar);
#endif

			struct uvm_alloc_data uad = {
				.size = (int)max_bufDescriptor->size,
				.byte_stride = max_bufDescriptor->pixel_stride,
				.width = max_bufDescriptor->width,
				.height = max_bufDescriptor->height,
				.align = 0,
				.flags = uvm_flags,
				.scalar = buf_scalar,
				.scaled_buf_size = v4l2_dec_max_buf_size
			};
			ret = ioctl(uvm_fd, UVM_IOC_ALLOC, &uad);
			if (ret < 0) {
				MALI_GRALLOC_LOGE("%s, ioctl::UVM_IOC_ALLOC failed uvm_fd=%d",
					__func__, uvm_fd);
				return ret;
			}
			ALOGE("ioctl UVM_IOC_ALLOC fd sharefd: %d\n", uad.fd);
			shared_fd = uad.fd;
		} else {
			shared_fd = dev->alloc_from_ion_heap(usage, max_bufDescriptor->size, &heap_type, ion_flags, &min_pgsz);
			delay_alloc = 0;
		}
		/*update private heap flag*/
		am_gralloc_set_ion_flags(heap_type, usage, &priv_heap_flag, NULL);
#else
		set_ion_flags(heap_type, usage, &priv_heap_flag, &ion_flags);

		shared_fd = dev->alloc_from_ion_heap(usage, max_bufDescriptor->size, heap_type, ion_flags, &min_pgsz);
#endif
//meson graphics changes end

		if (shared_fd < 0)
		{
			MALI_GRALLOC_LOGE("ion_alloc failed form client: ( %d )", dev->client());
			return -1;
		}

		for (i = 0; i < numDescriptors; i++)
		{
			buffer_descriptor_t *bufDescriptor = (buffer_descriptor_t *)(descriptors[i]);
			int tmp_fd;

			if (i != max_buffer_index)
			{
				tmp_fd = dup(shared_fd);

				if (tmp_fd < 0)
				{
					MALI_GRALLOC_LOGE("Ion shared fd:%d of index:%d could not be duplicated for descriptor:%d",
					      shared_fd, max_buffer_index, i);

					/* It is possible that already opened shared_fd for the
					 * max_bufDescriptor is also not closed */
					if (i < max_buffer_index)
					{
						close(shared_fd);
					}

					/* Need to free already allocated memory. */
					mali_gralloc_ion_free_internal(pHandle, numDescriptors);
					return -1;
				}
			}
			else
			{
				tmp_fd = shared_fd;
			}

			private_handle_t *hnd = new private_handle_t(
			    private_handle_t::PRIV_FLAGS_USES_ION | priv_heap_flag | uvm_buffer_flag, bufDescriptor->size,
			    bufDescriptor->consumer_usage, bufDescriptor->producer_usage, tmp_fd, bufDescriptor->hal_format,
			    bufDescriptor->old_internal_format, bufDescriptor->alloc_format,
			    bufDescriptor->width, bufDescriptor->height, bufDescriptor->pixel_stride,
			    bufDescriptor->old_alloc_width, bufDescriptor->old_alloc_height, bufDescriptor->old_byte_stride,
			    max_bufDescriptor->size, bufDescriptor->layer_count, bufDescriptor->plane_info);
#ifdef AML_GRALLOC_DEBUG
			AML_GRALLOC_LOGI("%s: width:%d height:%d stride:%d format=0x%" PRIx64 " usage=0x%" PRIx64,
				    __FUNCTION__, bufDescriptor->width, bufDescriptor->height, bufDescriptor->pixel_stride,
				    bufDescriptor->hal_format, usage);
#endif
			if (NULL == hnd)
			{
				MALI_GRALLOC_LOGE("Private handle could not be created for descriptor:%d of shared usecase", i);

				/* Close the obtained shared file descriptor for the current handle */
				close(tmp_fd);

				/* It is possible that already opened shared_fd for the
				 * max_bufDescriptor is also not closed */
				if (i < max_buffer_index)
				{
					close(shared_fd);
				}

				/* Free the resources allocated for the previous handles */
				mali_gralloc_ion_free_internal(pHandle, numDescriptors);
				return -1;
			}

#ifdef GRALLOC_AML_EXTEND
			hnd->ion_delay_alloc = delay_alloc;
			hnd->am_extend_fd = ::dup(hnd->share_fd);
			hnd->am_extend_type = 0;
#endif

			pHandle[i] = hnd;
		}
	}
	else
	{
		for (i = 0; i < numDescriptors; i++)
		{
			buffer_descriptor_t *bufDescriptor = (buffer_descriptor_t *)(descriptors[i]);
			usage = bufDescriptor->consumer_usage | bufDescriptor->producer_usage;

//meson graphics changes start
#ifdef GRALLOC_AML_EXTEND
            heap_type = am_gralloc_pick_ion_heap(bufDescriptor, usage);
#else
            heap_type = dev->pick_ion_heap(usage);
#endif
//meson graphics changes end

			if (heap_type == ION_HEAP_TYPE_INVALID)
			{
				MALI_GRALLOC_LOGE("Failed to find an appropriate ion heap");
				mali_gralloc_ion_free_internal(pHandle, numDescriptors);
				return -1;
			}

//meson graphics changes start
#ifdef GRALLOC_AML_EXTEND
			am_gralloc_set_ion_flags(heap_type, usage, NULL, &ion_flags);

			if (am_gralloc_is_video_overlay_extend_usage(usage) ||
				am_gralloc_is_omx_metadata_extend_usage(usage) ||
				am_gralloc_is_omx_osd_extend_usage(usage)) {

				uvm_buffer_flag |= private_handle_t::PRIV_FLAGS_UVM_BUFFER;

				if (am_gralloc_is_omx_osd_extend_usage(usage) ||
					am_gralloc_is_video_decoder_full_buffer_usage(usage) ||
					am_gralloc_is_video_decoder_OSD_buffer_usage(usage)) {
					buf_scalar = 1;
				} else if (am_gralloc_is_video_decoder_quarter_buffer_usage(usage)) {
					buf_scalar = 2;
				} else if (am_gralloc_is_video_decoder_one_sixteenth_buffer_usage(usage)) {
					buf_scalar = 4;
				} else {
					uvm_flags = UVM_DELAY_ALLOC;
					delay_alloc = 1;
				}

				if (usage & GRALLOC_USAGE_PROTECTED) {
					uvm_flags |= UVM_USAGE_PROTECTED;
				}
				if ((bufDescriptor->width * bufDescriptor->height) > (V4L2_DECODER_BUFFER_MAX_WIDTH * V4L2_DECODER_BUFFER_MAX_HEIGHT)) {
					v4l2_dec_max_buf_size = V4L2_DECODER_BUFFER_8k_MAX_WIDTH * V4L2_DECODER_BUFFER_8k_MAX_HEIGHT * 3 / 2;
				}
#ifdef GRALLOC_AML_EXTEND
				//workaround for unsupported 4k video play on some platforms
				v4l2_dec_max_buf_size = am_gralloc_exec_omx_policy(v4l2_dec_max_buf_size, buf_scalar);
#endif

				struct uvm_alloc_data uad = {
					.size = (int)bufDescriptor->size,
					.byte_stride = bufDescriptor->pixel_stride,
					.width = bufDescriptor->width,
					.height = bufDescriptor->height,
					.align = 0,
					.flags = uvm_flags,
					.scalar = buf_scalar,
					.scaled_buf_size = v4l2_dec_max_buf_size
				};

				ret = ioctl(uvm_fd, UVM_IOC_ALLOC, &uad);
				if (ret < 0) {
					MALI_GRALLOC_LOGE("%s, ioctl::UVM_IOC_ALLOC failed uvm_fd=%d",
						__func__, uvm_fd);
					return ret;
				}

				shared_fd = uad.fd;
			} else {
				shared_fd = dev->alloc_from_ion_heap(usage, bufDescriptor->size, &heap_type, ion_flags, &min_pgsz);
				delay_alloc = 0;
			}
			am_gralloc_set_ion_flags(heap_type, usage, &priv_heap_flag, NULL);
#else
			set_ion_flags(heap_type, usage, &priv_heap_flag, &ion_flags);
			shared_fd = dev->alloc_from_ion_heap(usage, bufDescriptor->size, heap_type, ion_flags, &min_pgsz);
#endif
//meson graphics changes end

			if (shared_fd < 0)
			{
				MALI_GRALLOC_LOGE("ion_alloc failed from client ( %d )", dev->client());

				/* need to free already allocated memory. not just this one */
				mali_gralloc_ion_free_internal(pHandle, numDescriptors);

				return -1;
			}

			private_handle_t *hnd = new private_handle_t(
			    private_handle_t::PRIV_FLAGS_USES_ION | priv_heap_flag | uvm_buffer_flag, bufDescriptor->size,
			    bufDescriptor->consumer_usage, bufDescriptor->producer_usage, shared_fd, bufDescriptor->hal_format,
			    bufDescriptor->old_internal_format, bufDescriptor->alloc_format,
			    bufDescriptor->width, bufDescriptor->height, bufDescriptor->pixel_stride,
			    bufDescriptor->old_alloc_width, bufDescriptor->old_alloc_height, bufDescriptor->old_byte_stride,
			    bufDescriptor->size, bufDescriptor->layer_count, bufDescriptor->plane_info);
#ifdef AML_GRALLOC_DEBUG
			AML_GRALLOC_LOGI("%s: width:%d height:%d stride:%d format=0x%" PRIx64 " usage=0x%" PRIx64,
				    __FUNCTION__, bufDescriptor->width, bufDescriptor->height, bufDescriptor->pixel_stride,
				    bufDescriptor->hal_format, usage);
#endif
			if (NULL == hnd)
			{
				MALI_GRALLOC_LOGE("Private handle could not be created for descriptor:%d in non-shared usecase", i);

				/* Close the obtained shared file descriptor for the current handle */
				close(shared_fd);
				mali_gralloc_ion_free_internal(pHandle, numDescriptors);
				return -1;
			}
#ifdef GRALLOC_AML_EXTEND
			hnd->ion_delay_alloc = delay_alloc;
			hnd->am_extend_fd = ::dup(hnd->share_fd);
			hnd->am_extend_type = 0;
#endif
			pHandle[i] = hnd;
		}
	}

	for (i = 0; i < numDescriptors; i++)
	{
		buffer_descriptor_t *bufDescriptor = (buffer_descriptor_t *)(descriptors[i]);
		private_handle_t *hnd = (private_handle_t *)(pHandle[i]);

		usage = bufDescriptor->consumer_usage | bufDescriptor->producer_usage;

#ifdef GRALLOC_AML_EXTEND
		if (!(usage & GRALLOC_USAGE_PROTECTED) && !hnd->ion_delay_alloc)
#else
		if (!(usage & GRALLOC_USAGE_PROTECTED))
#endif
		{
#ifdef GRALLOC_AML_EXTEND
		if (am_gralloc_is_video_decoder_quarter_buffer_usage(usage) ||
			am_gralloc_is_video_decoder_one_sixteenth_buffer_usage(usage)) {
				break;
		}
#endif
			cpu_ptr =
			    (unsigned char *)mmap(NULL, bufDescriptor->size, PROT_READ | PROT_WRITE, MAP_SHARED, hnd->share_fd, 0);

			if (MAP_FAILED == cpu_ptr)
			{
				MALI_GRALLOC_LOGE("mmap failed from client ( %d ), fd ( %d )", dev->client(), hnd->share_fd);
				mali_gralloc_ion_free_internal(pHandle, numDescriptors);
				return -1;
			}

#if defined(GRALLOC_INIT_AFBC) && (GRALLOC_INIT_AFBC == 1)
			if ((bufDescriptor->alloc_format & MALI_GRALLOC_INTFMT_AFBCENABLE_MASK) && (!(*shared_backend)))
			{
				mali_gralloc_ion_sync_start(hnd, true, true);

				/* For separated plane YUV, there is a header to initialise per plane. */
				const plane_info_t *plane_info = bufDescriptor->plane_info;
				const bool is_multi_plane = hnd->is_multi_plane();
				for (int i = 0; i < MAX_PLANES && (i == 0 || plane_info[i].byte_stride != 0); i++)
				{
#if GRALLOC_USE_LEGACY_CALCS == 1
					if (i == 0)
					{
						uint32_t w = GRALLOC_MAX((uint32_t)bufDescriptor->old_alloc_width, plane_info[0].alloc_width);
						uint32_t h = GRALLOC_MAX((uint32_t)bufDescriptor->old_alloc_height, plane_info[0].alloc_height);

						init_afbc(cpu_ptr,
						          bufDescriptor->old_internal_format,
						          is_multi_plane,
						          w,
						          h);
					}
					else
#endif
					{
						init_afbc(cpu_ptr + plane_info[i].offset,
						          bufDescriptor->alloc_format,
						          is_multi_plane,
						          plane_info[i].alloc_width,
						          plane_info[i].alloc_height);
					}
				}

				mali_gralloc_ion_sync_end(hnd, true, true);
			}
#endif
			hnd->base = cpu_ptr;
		}
	}

	return 0;
}


int mali_gralloc_ion_map(private_handle_t *hnd)
{
	int retval = -EINVAL;

	switch (hnd->flags & private_handle_t::PRIV_FLAGS_USES_ION)
	{
	case private_handle_t::PRIV_FLAGS_USES_ION:
		size_t size = hnd->size;

#ifdef GRALLOC_AML_EXTEND
		if (hnd->ion_delay_alloc)
			return 0;

#ifdef BUILD_KERNEL_4_9
		ion_device *dev = ion_device::get();
		if (!dev) {
			return -1;
		}
		ion_user_handle_t user_hnd;
		ion_import(dev->client(), hnd->share_fd, &user_hnd);
#endif
		uint64_t usage = hnd->producer_usage | hnd->consumer_usage;
		if (am_gralloc_is_video_decoder_quarter_buffer_usage(usage) ||
			am_gralloc_is_video_decoder_one_sixteenth_buffer_usage(usage)) {
			return 0;
		}
#endif
		unsigned char *mappedAddress = (unsigned char *)mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, hnd->share_fd, 0);
#ifdef GRALLOC_AML_EXTEND
#ifdef BUILD_KERNEL_4_9
		imported_ion_client.emplace(hnd->share_fd, dev->client());
		imported_user_hnd.emplace(hnd->share_fd, user_hnd);
#endif
#endif

		if (MAP_FAILED == mappedAddress)
		{
			MALI_GRALLOC_LOGE("mmap( share_fd:%d ) failed with %s", hnd->share_fd, strerror(errno));
			retval = -errno;
			break;
		}

		hnd->base = (void *)(uintptr_t(mappedAddress) + hnd->offset);
		retval = 0;
		break;
	}

	return retval;
}

void mali_gralloc_ion_unmap(private_handle_t *hnd)
{
	switch (hnd->flags & private_handle_t::PRIV_FLAGS_USES_ION)
	{
	case private_handle_t::PRIV_FLAGS_USES_ION:
		void *base = (void *)(uintptr_t(hnd->base) - hnd->offset);
		size_t size = hnd->size;

#ifdef GRALLOC_AML_EXTEND
		uint64_t usage = hnd->producer_usage | hnd->consumer_usage;
		if (am_gralloc_is_video_decoder_quarter_buffer_usage(usage) ||
			am_gralloc_is_video_decoder_one_sixteenth_buffer_usage(usage)) {
			break;
		}
		if (!hnd->ion_delay_alloc && munmap(base, size) < 0)
#else
		if (munmap(base, size) < 0)
#endif
		{
			MALI_GRALLOC_LOGE("Could not munmap base:%p size:%zd '%s'", base, size, strerror(errno));
		}
		else
		{
			hnd->base = 0;
			hnd->cpu_read = 0;
			hnd->cpu_write = 0;
		}
#ifdef GRALLOC_AML_EXTEND
#ifdef BUILD_KERNEL_4_9
		auto user_hnd_iter = imported_user_hnd.find(hnd->share_fd);
		auto ion_client_iter = imported_ion_client.find(hnd->share_fd);
		if (user_hnd_iter != imported_user_hnd.end()
				&& ion_client_iter != imported_ion_client.end()) {
			ion_free(ion_client_iter->second, user_hnd_iter->second);
			imported_user_hnd.erase(user_hnd_iter);
			imported_ion_client.erase(ion_client_iter);
		}
#endif
#endif
		break;
	}
}

void mali_gralloc_ion_close(void)
{
	ion_device::close();
#ifdef GRALLOC_AML_EXTEND
	ion_device::close_uvm();
#endif
}

#ifdef GRALLOC_AML_EXTEND
/*for ion alloc*/
bool is_android_yuv_format(int req_format)
{
	bool rval = false;

	switch (req_format)
	{
	case HAL_PIXEL_FORMAT_YV12:
	case HAL_PIXEL_FORMAT_Y8:
	case HAL_PIXEL_FORMAT_Y16:
	case HAL_PIXEL_FORMAT_YCbCr_420_888:
	case HAL_PIXEL_FORMAT_YCbCr_422_888:
	case HAL_PIXEL_FORMAT_YCbCr_444_888:
	case HAL_PIXEL_FORMAT_YCrCb_420_SP:
	case HAL_PIXEL_FORMAT_YCbCr_422_SP:
	case HAL_PIXEL_FORMAT_YCBCR_422_I:
	case HAL_PIXEL_FORMAT_RAW16:
	case HAL_PIXEL_FORMAT_RAW12:
	case HAL_PIXEL_FORMAT_RAW10:
	case HAL_PIXEL_FORMAT_RAW_OPAQUE:
	case HAL_PIXEL_FORMAT_BLOB:
	case HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED:
		rval = true;
		break;
	}

	return rval;
}

enum ion_heap_type am_gralloc_pick_ion_heap(
	buffer_descriptor_t *bufDescriptor, uint64_t usage)
{
	enum ion_heap_type ret = ION_HEAP_TYPE_SYSTEM;

	if (usage & GRALLOC_USAGE_HW_FB)
	{
		ret = ION_HEAP_TYPE_DMA;
		goto out;
	}

	if (am_gralloc_is_omx_osd_extend_usage(usage) ||
		(usage & GRALLOC1_PRODUCER_USAGE_CAMERA))
	{
		ret = ION_HEAP_TYPE_CUSTOM;
		goto out;
	}

	if (am_gralloc_is_omx_metadata_extend_usage(usage))
	{
		ret = ION_HEAP_TYPE_SYSTEM;
		goto out;
	}

#ifdef AML_ALLOC_SCANOUT_FOR_COMPOSE
        #if BOARD_RESOLUTION_RATIO == 720
            static unsigned int max_composer_buf_width = 1280;
            static unsigned int max_composer_buf_height = 720;
        #else
            static unsigned int max_composer_buf_width = 1920;
            static unsigned int max_composer_buf_height = 1080;
        #endif

	if (usage & GRALLOC_USAGE_HW_COMPOSER)
	{
		if ( (bufDescriptor->width <= max_composer_buf_width) &&
			(bufDescriptor->height <= max_composer_buf_height) &&
                        (!is_android_yuv_format(bufDescriptor->hal_format)))
		{
			ret = ION_HEAP_TYPE_DMA;
			goto out;
		}
	}
 #else
        /*for compile warning.*/
        bufDescriptor;
#endif

out:
    return ret;
}

void am_gralloc_set_ion_flags(ion_heap_type heap_type, uint64_t usage,
	unsigned int *priv_heap_flag, unsigned int *ion_flags)
{
	if (priv_heap_flag)
	{
		int coherent_buffer_flag = am_gralloc_get_coherent_extend_flag();

		if (heap_type == ION_HEAP_TYPE_SYSTEM)
		{
			*priv_heap_flag &= ~coherent_buffer_flag;
		}
		else if (heap_type == ION_HEAP_TYPE_DMA)
		{
			*priv_heap_flag |= coherent_buffer_flag;
		}
		else if (heap_type == ION_HEAP_TYPE_CUSTOM)
		{
			*priv_heap_flag |= coherent_buffer_flag;
		}

		/*Must check omx metadata first,
		*for it have some same bits with video overlay.
		*/
		if (am_gralloc_is_omx_metadata_extend_usage(usage))
		{
			*priv_heap_flag |= am_gralloc_get_omx_metadata_extend_flag();
		}
		else if (am_gralloc_is_video_overlay_extend_usage(usage))
		{
			*priv_heap_flag |= am_gralloc_get_video_overlay_extend_flag();
		}
		if (am_gralloc_is_secure_extend_usage(usage))
		{
		    *priv_heap_flag |= am_gralloc_get_secure_extend_flag();
		}
	}

	if (ion_flags)
	{
		if ((heap_type != ION_HEAP_TYPE_DMA) &&
			(heap_type != ION_HEAP_TYPE_CUSTOM))
		{
			if ((usage & (GRALLOC_USAGE_SW_WRITE_MASK | GRALLOC_USAGE_SW_READ_MASK))
				|| (usage == GRALLOC_USAGE_HW_TEXTURE))
			{
				*ion_flags = ION_FLAG_CACHED | ION_FLAG_CACHED_NEEDS_SYNC;
			}
		}
	}
}
#endif

