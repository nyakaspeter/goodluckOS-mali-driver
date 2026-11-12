/*
 * Copyright (C) 2013-2016 ARM Limited. All rights reserved.
 * 
 * This program is free software and is provided to you under the terms of the GNU General Public License version 2
 * as published by the Free Software Foundation, and any use by you of this program is subject to the terms of such GNU licence.
 * 
 * A copy of the licence is included with the program, and can also be obtained from Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include <linux/list.h>
#include <linux/mm.h>
#include <linux/mm_types.h>
#include <linux/fs.h>
#include <linux/dma-mapping.h>
#include <linux/slab.h>
#include <linux/platform_device.h>

#include "mali_osk.h"
#include "mali_osk_mali.h"
#include "mali_kernel_linux.h"
#include "mali_scheduler.h"

#include "mali_memory.h"
#include "mali_memory_os_alloc.h"
#if defined(CONFIG_DMA_SHARED_BUFFER)
#include "mali_memory_dma_buf.h"
#include "mali_memory_secure.h"
#endif
#if defined(CONFIG_MALI400_UMP)
#include "mali_memory_ump.h"
#endif
#include "mali_memory_external.h"
#include "mali_memory_manager.h"
#include "mali_memory_virtual.h"
#include "mali_memory_cow.h"
#include "mali_memory_block_alloc.h"
#include "mali_memory_swap_alloc.h"
#include <linux/module.h>
#include "mali_executor.h"
#include "mali_mmu_page_directory.h"



/**
*function @_mali_free_allocation_mem - free a memory allocation
*/
static u32 _mali_free_allocation_mem(mali_mem_allocation *mali_alloc)
{
	mali_mem_backend *mem_bkend = NULL;
	u32 free_pages_nr = 0;

	struct mali_session_data *session = mali_alloc->session;
	MALI_DEBUG_PRINT(4, (" _mali_free_allocation_mem, psize =0x%x! \n", mali_alloc->psize));
	if (0 == mali_alloc->psize)
		goto out;

	/* Get backend memory & Map on CPU */
	mutex_lock(&mali_idr_mutex);
	mem_bkend = idr_find(&mali_backend_idr, mali_alloc->backend_handle);
	mutex_unlock(&mali_idr_mutex);
	MALI_DEBUG_ASSERT(NULL != mem_bkend);

	switch (mem_bkend->type) {
	case MALI_MEM_OS:
		free_pages_nr = mali_mem_os_release(mem_bkend);
		atomic_sub(free_pages_nr, &session->mali_mem_allocated_pages);
		break;
	case MALI_MEM_UMP:
#if defined(CONFIG_MALI400_UMP)
		mali_mem_unbind_ump_buf(mem_bkend);
		atomic_sub(mem_bkend->size / MALI_MMU_PAGE_SIZE, &session->mali_mem_array[mem_bkend->type]);
#else
		MALI_DEBUG_PRINT(1, ("UMP not supported\n"));
#endif
		break;
	case MALI_MEM_DMA_BUF:
#if defined(CONFIG_DMA_SHARED_BUFFER)
		mali_mem_unbind_dma_buf(mem_bkend);
		atomic_sub(mem_bkend->size / MALI_MMU_PAGE_SIZE, &session->mali_mem_array[mem_bkend->type]);
#else
		MALI_DEBUG_PRINT(1, ("DMA not supported\n"));
#endif
		break;
	case MALI_MEM_EXTERNAL:
		mali_mem_unbind_ext_buf(mem_bkend);
		atomic_sub(mem_bkend->size / MALI_MMU_PAGE_SIZE, &session->mali_mem_array[mem_bkend->type]);
		break;

	case MALI_MEM_BLOCK:
		free_pages_nr = mali_mem_block_release(mem_bkend);
		atomic_sub(free_pages_nr, &session->mali_mem_allocated_pages);
		break;

	case MALI_MEM_COW:
		if (mem_bkend->flags & MALI_MEM_BACKEND_FLAG_SWAP_COWED) {
			free_pages_nr = mali_mem_swap_release(mem_bkend, MALI_TRUE);
		} else {
			free_pages_nr = mali_mem_cow_release(mem_bkend, MALI_TRUE);
		}
		atomic_sub(free_pages_nr, &session->mali_mem_allocated_pages);
		break;
	case MALI_MEM_SWAP:
		free_pages_nr = mali_mem_swap_release(mem_bkend, MALI_TRUE);
		atomic_sub(free_pages_nr, &session->mali_mem_allocated_pages);
		atomic_sub(free_pages_nr, &session->mali_mem_array[mem_bkend->type]);
		break;
	case MALI_MEM_SECURE:
#if defined(CONFIG_DMA_SHARED_BUFFER)
		free_pages_nr = mali_mem_secure_release(mem_bkend);
		atomic_sub(free_pages_nr, &session->mali_mem_allocated_pages);
#else
		MALI_DEBUG_PRINT(1, ("DMA not supported for mali secure memory\n"));
#endif
		break;
	default:
		MALI_DEBUG_PRINT(1, ("mem type %d is not in the mali_mem_type enum.\n", mem_bkend->type));
		break;
	}

	/*Remove backend memory idex */
	mutex_lock(&mali_idr_mutex);
	idr_remove(&mali_backend_idr, mali_alloc->backend_handle);
	mutex_unlock(&mali_idr_mutex);
	kfree(mem_bkend);
out:
	/* remove memory allocation  */
	mali_vma_offset_remove(&session->allocation_mgr, &mali_alloc->mali_vma_node);
	mali_mem_allocation_struct_destory(mali_alloc);
	return free_pages_nr;
}

/*
 * Parked allocations. ARM's GLES driver takes the memory for a draw's
 * transformed vertices (position and varyings, when over 4 KB) with MEM_ALLOC
 * and gives it back after the frame: a few hundred allocations a frame, each
 * with its allocation and backend records, its id and its pages made and
 * taken apart again (most of the 14 us either call takes). A freed allocation
 * of the plain kind is kept instead: unmapped from the GPU and out of the
 * address tree, but with its records, id and pages, and the next MEM_ALLOC of
 * that size and those flags gets it mapped at its new address.
 * mali_mem_park=0 turns it off.
 */
#define MALI_PARK_TOTAL_PAGES (6144u) /* 24 MB */
int mali_mem_park = 1;
module_param(mali_mem_park, int, 0644);

#define MALI_PARK_MAX_NUM (4096u)

/*
 * Lazy unmap (mali_mem_park_lazy=1): a parked allocation's page table entries
 * are cleared without stalling the running jobs' MMUs to zap their TLBs. Its
 * pages (deferred kind) wait on the session's pending list, and one zap
 * covers them all before they go back to the pool; pages of the plain kind
 * stay with their record. Every job start zaps the TLB anyway
 * (mali_group_activate_page_directory), so only a job running during the
 * free can still hold an entry, and what it points at stays this session's
 * until the zap. An unmap that would release a page table is done the
 * normal way.
 */
int mali_mem_park_lazy = 1;
module_param(mali_mem_park_lazy, int, 0644);
#define MALI_PARK_PENDING_PAGES (256u) /* 1 MB */

static void mali_park_flush(struct mali_session_data *session)
{
	struct mali_allocation_manager *mgr = &session->allocation_mgr;
	LIST_HEAD(pages);
	u32 count;

	mutex_lock(&mgr->park_mutex);
	if (!mgr->zap_pending) {
		mutex_unlock(&mgr->park_mutex);
		return;
	}
	list_splice_init(&mgr->pending_pages, &pages);
	count = mgr->pending_count;
	mgr->pending_count = 0;
	mgr->zap_pending = false;
	mutex_unlock(&mgr->park_mutex);
	mali_session_memory_lock(session);
	mali_executor_zap_all_active(session);
	mali_session_memory_unlock(session);
	if (count)
		mali_mem_os_free(&pages, count, MALI_FALSE);
}

/* Whether unmapping this range empties (and so releases) a page table. */
static mali_bool mali_park_unmap_frees_table(struct mali_page_directory *pagedir, u32 addr, u32 size)
{
	u32 left = size;

	while (left) {
		u32 offset = addr & (MALI_MMU_VIRTUAL_PAGE_SIZE - 1);
		u32 in_pde = left < MALI_MMU_VIRTUAL_PAGE_SIZE - offset ? left : MALI_MMU_VIRTUAL_PAGE_SIZE - offset;

		if (pagedir->page_entries_usage_count[MALI_MMU_PDE_ENTRY(addr)] <= in_pde / MALI_MMU_PAGE_SIZE)
			return MALI_TRUE;
		left -= in_pde;
		addr += in_pde;
	}
	return MALI_FALSE;
}

/* Releases a parked allocation: not mapped, not in the tree. */
static void mali_park_destroy(mali_mem_allocation *alloc)
{
	mali_mem_backend *mem_bkend = alloc->park_backend;

	if (mem_bkend->os_mem.count) {
		u32 pages;

		/* They may have been unmapped lazily. */
		mali_park_flush(alloc->session);
		mutex_lock(&mem_bkend->mutex);
		pages = mali_mem_os_free(&mem_bkend->os_mem.pages, mem_bkend->os_mem.count, MALI_FALSE);
		mutex_unlock(&mem_bkend->mutex);
		atomic_sub(pages, &alloc->session->mali_mem_allocated_pages);
	}
	mutex_lock(&mali_idr_mutex);
	idr_remove(&mali_backend_idr, alloc->backend_handle);
	mutex_unlock(&mali_idr_mutex);
	kfree(mem_bkend);
	mali_mem_allocation_struct_destory(alloc);
}

static mali_bool mali_park_put(mali_mem_allocation *alloc)
{
	struct mali_allocation_manager *mgr = &alloc->session->allocation_mgr;
	mali_mem_backend *mem_bkend = alloc->park_backend;
	u32 pages = alloc->psize / MALI_MMU_PAGE_SIZE;
	mali_bool defer = (alloc->flags & _MALI_MEMORY_ALLOCATE_DEFER_BIND) ? MALI_TRUE : MALI_FALSE;
	int lazy;
	mali_bool flush = MALI_FALSE;

	if (!mali_mem_park || !alloc->parkable || MALI_MEM_OS != alloc->type || NULL == mem_bkend ||
	    MALI_MEM_OS != mem_bkend->type || mgr->park_closed || mgr->parked_num >= MALI_PARK_MAX_NUM)
		return MALI_FALSE;
	if (defer) {
		/* Bound at the GP job's start, or never. */
		if (mem_bkend->flags & ~(MALI_MEM_BACKEND_FLAG_NOT_BINDED | MALI_MEM_BACKEND_FLAG_BINDED))
			return MALI_FALSE;
	} else if (0 != mem_bkend->flags || mem_bkend->os_mem.count != pages ||
		   mgr->parked_pages + pages > MALI_PARK_TOTAL_PAGES) {
		return MALI_FALSE;
	}
	/* Off the GPU and out of the tree: the address is free again. */
	{
		struct mali_session_data *session = alloc->session;
		u32 start = alloc->mali_vma_node.vm_node.start;
		u32 size = alloc->psize;

		if (alloc->flags & MALI_MEM_FLAG_MALI_GUARD_PAGE)
			size += MALI_MMU_PAGE_SIZE;
		lazy = mali_mem_park_lazy;
		mali_session_memory_lock(session);
		if (lazy && mali_park_unmap_frees_table(session->page_directory, start, size))
			lazy = 0;
		mali_mmu_pagedir_unmap(session->page_directory, start, size);
		if (!lazy)
			mali_executor_zap_all_active(session);
		mali_session_memory_unlock(session);
	}
	if (defer) {
		/* As a new deferred allocation: no pages until its next job. */
		if (mem_bkend->os_mem.count && lazy) {
			mutex_lock(&mem_bkend->mutex);
			mutex_lock(&mgr->park_mutex);
			list_splice_init(&mem_bkend->os_mem.pages, &mgr->pending_pages);
			mgr->pending_count += mem_bkend->os_mem.count;
			mgr->zap_pending = true;
			flush = mgr->pending_count >= MALI_PARK_PENDING_PAGES;
			mutex_unlock(&mgr->park_mutex);
			mutex_unlock(&mem_bkend->mutex);
			atomic_sub(mem_bkend->os_mem.count, &alloc->session->mali_mem_allocated_pages);
		} else if (mem_bkend->os_mem.count) {
			u32 freed;

			mutex_lock(&mem_bkend->mutex);
			freed = mali_mem_os_free(&mem_bkend->os_mem.pages, mem_bkend->os_mem.count, MALI_FALSE);
			mutex_unlock(&mem_bkend->mutex);
			atomic_sub(freed, &alloc->session->mali_mem_allocated_pages);
		}
		mem_bkend->os_mem.count = 0;
		INIT_LIST_HEAD(&mem_bkend->os_mem.pages);
		mem_bkend->flags = MALI_MEM_BACKEND_FLAG_NOT_BINDED;
	}
	mali_vma_offset_remove(mgr, &alloc->mali_vma_node);
	mutex_lock(&mgr->park_mutex);
	if (mgr->park_closed) {
		mutex_unlock(&mgr->park_mutex);
		mali_park_destroy(alloc);
		return MALI_TRUE;
	}
	list_add(&alloc->park, &mgr->parked[defer ? 1 : 0][pages]);
	mgr->parked_num++;
	if (!defer) {
		mgr->parked_pages += pages;
		if (lazy)
			mgr->zap_pending = true;
	}
	mutex_unlock(&mgr->park_mutex);
	if (flush)
		mali_park_flush(alloc->session);
	return MALI_TRUE;
}

/* MEM_ALLOC: a parked allocation of this size and these flags, set up at
 * vaddr (MALI_TRUE, *result the outcome), or none (MALI_FALSE). */
mali_bool mali_park_take(struct mali_session_data *session, u32 vaddr, u32 psize, u32 vsize, u32 flags,
			 _mali_osk_errcode_t *result)
{
	struct mali_allocation_manager *mgr = &session->allocation_mgr;
	u32 pages = psize / MALI_MMU_PAGE_SIZE;
	mali_bool defer = (flags & _MALI_MEMORY_ALLOCATE_DEFER_BIND) ? MALI_TRUE : MALI_FALSE;
	struct list_head *bucket;
	mali_mem_allocation *alloc = NULL;
	mali_mem_backend *mem_bkend;
	_mali_osk_errcode_t err;

	if (0 == mgr->parked_num || psize != vsize || 0 == pages || pages > MALI_PARK_MAX_PAGES ||
	    (psize & (MALI_MMU_PAGE_SIZE - 1)))
		return MALI_FALSE;
	bucket = &mgr->parked[defer ? 1 : 0][pages];
	mutex_lock(&mgr->park_mutex);
	if (!list_empty(bucket)) {
		alloc = list_first_entry(bucket, mali_mem_allocation, park);
		if (alloc->park_flags == flags) {
			list_del(&alloc->park);
			mgr->parked_num--;
			if (!defer)
				mgr->parked_pages -= pages;
		} else {
			alloc = NULL;
		}
	}
	mutex_unlock(&mgr->park_mutex);
	if (NULL == alloc)
		return MALI_FALSE;

	mem_bkend = alloc->park_backend;
	alloc->mali_vma_node.vm_node.start = vaddr;
	alloc->mali_vma_node.vm_node.size = vsize;
	if (mali_vma_offset_add_checked(mgr, &alloc->mali_vma_node)) {
		/* The address is in use: back to its list. */
		memset(&alloc->mali_vma_node.vm_node, 0, sizeof(alloc->mali_vma_node.vm_node));
		mutex_lock(&mgr->park_mutex);
		if (mgr->park_closed) {
			mutex_unlock(&mgr->park_mutex);
			mali_park_destroy(alloc);
		} else {
			list_add(&alloc->park, bucket);
			mgr->parked_num++;
			if (!defer)
				mgr->parked_pages += pages;
			mutex_unlock(&mgr->park_mutex);
		}
		*result = _MALI_OSK_ERR_FAULT;
		return MALI_TRUE;
	}
	_mali_osk_atomic_init(&alloc->mem_alloc_refcount, 1);
	alloc->mali_mapping.addr = vaddr;
	_mali_osk_mutex_wait(session->memory_lock);
	err = mali_mem_mali_map_prepare(alloc);
	if (_MALI_OSK_ERR_OK == err && !defer)
		err = mali_mem_os_mali_map(&mem_bkend->os_mem, session, vaddr, 0, pages, alloc->mali_mapping.properties);
	_mali_osk_mutex_signal(session->memory_lock);
	if (_MALI_OSK_ERR_OK != err) {
		mali_vma_offset_remove(mgr, &alloc->mali_vma_node);
		mali_park_destroy(alloc);
	}
	*result = err;
	return MALI_TRUE;
}

/**
*  ref_count for allocation
*/
u32 mali_allocation_unref(struct mali_mem_allocation **alloc)
{
	u32 free_pages_nr = 0;
	mali_mem_allocation *mali_alloc = *alloc;
	*alloc = NULL;
	if (0 == _mali_osk_atomic_dec_return(&mali_alloc->mem_alloc_refcount)) {
		u32 pages = mali_alloc->psize / MALI_MMU_PAGE_SIZE;

		if (mali_park_put(mali_alloc))
			return pages;
		free_pages_nr = _mali_free_allocation_mem(mali_alloc);
	}
	return free_pages_nr;
}

void mali_allocation_ref(struct mali_mem_allocation *alloc)
{
	_mali_osk_atomic_inc(&alloc->mem_alloc_refcount);
}

void mali_free_session_allocations(struct mali_session_data *session)
{
	struct mali_mem_allocation *entry, *next;

	struct mali_allocation_manager *mgr = &session->allocation_mgr;
	LIST_HEAD(parked);
	int i;

	MALI_DEBUG_PRINT(4, (" mali_free_session_allocations! \n"));
	mutex_lock(&mgr->park_mutex);
	mgr->park_closed = true;
	for (i = 0; i <= MALI_PARK_MAX_PAGES; i++) {
		list_splice_init(&mgr->parked[0][i], &parked);
		list_splice_init(&mgr->parked[1][i], &parked);
	}
	mgr->parked_pages = 0;
	mgr->parked_num = 0;
	mutex_unlock(&mgr->park_mutex);
	list_for_each_entry_safe(entry, next, &parked, park) {
		list_del(&entry->park);
		mali_park_destroy(entry);
	}
	mali_park_flush(session);

	list_for_each_entry_safe(entry, next, &session->allocation_mgr.head, list) {
		mali_allocation_unref(&entry);
	}
}
