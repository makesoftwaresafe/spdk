/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation.
 *   Copyright (c) 2023, NVIDIA CORPORATION & AFFILIATES.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk/util.h"
#include "spdk/env_dpdk.h"
#include "spdk/log.h"
#include "spdk/assert.h"

#include "env_internal.h"

#include <rte_config.h>
#include <rte_cycles.h>
#include <rte_malloc.h>
#include <rte_mempool.h>
#include <rte_memzone.h>
#include <rte_version.h>
#include <rte_eal.h>

static __thread bool g_is_thread_unaffinitized;
static bool g_enforce_numa;

SPDK_STATIC_ASSERT(SOCKET_ID_ANY == SPDK_ENV_NUMA_ID_ANY, "SOCKET_ID_ANY mismatch");

void *
spdk_malloc(size_t size, size_t align, uint64_t *unused, int numa_id, uint32_t flags)
{
	void *buf;

	if (flags == 0 || unused != NULL) {
		return NULL;
	}

	align = spdk_max(align, RTE_CACHE_LINE_SIZE);
	buf = rte_malloc_socket(NULL, size, align, numa_id);
	if (buf == NULL && !g_enforce_numa && numa_id != SOCKET_ID_ANY) {
		buf = rte_malloc_socket(NULL, size, align, SOCKET_ID_ANY);
	}
	return buf;
}

void *
spdk_zmalloc(size_t size, size_t align, uint64_t *unused, int numa_id, uint32_t flags)
{
	void *buf;

	if (flags == 0 || unused != NULL) {
		return NULL;
	}

	align = spdk_max(align, RTE_CACHE_LINE_SIZE);
	buf = rte_zmalloc_socket(NULL, size, align, numa_id);
	if (buf == NULL && !g_enforce_numa && numa_id != SOCKET_ID_ANY) {
		buf = rte_zmalloc_socket(NULL, size, align, SOCKET_ID_ANY);
	}
	return buf;
}

void *
spdk_realloc(void *buf, size_t size, size_t align)
{
	align = spdk_max(align, RTE_CACHE_LINE_SIZE);
	return rte_realloc(buf, size, align);
}

void
spdk_free(void *buf)
{
	rte_free(buf);
}

void *
spdk_dma_malloc_socket(size_t size, size_t align, uint64_t *unused, int numa_id)
{
	return spdk_malloc(size, align, unused, numa_id, (SPDK_MALLOC_DMA | SPDK_MALLOC_SHARE));
}

void *
spdk_dma_zmalloc_socket(size_t size, size_t align, uint64_t *unused, int numa_id)
{
	return spdk_zmalloc(size, align, unused, numa_id, (SPDK_MALLOC_DMA | SPDK_MALLOC_SHARE));
}

void *
spdk_dma_malloc(size_t size, size_t align, uint64_t *unused)
{
	return spdk_dma_malloc_socket(size, align, unused, SPDK_ENV_NUMA_ID_ANY);
}

void *
spdk_dma_zmalloc(size_t size, size_t align, uint64_t *unused)
{
	return spdk_dma_zmalloc_socket(size, align, unused, SPDK_ENV_NUMA_ID_ANY);
}

void *
spdk_dma_realloc(void *buf, size_t size, size_t align, uint64_t *unused)
{
	if (unused != NULL) {
		return NULL;
	}
	align = spdk_max(align, RTE_CACHE_LINE_SIZE);
	return rte_realloc(buf, size, align);
}

void
spdk_dma_free(void *buf)
{
	spdk_free(buf);
}

void *
spdk_memzone_reserve_aligned(const char *name, size_t len, int numa_id,
			     unsigned flags, unsigned align)
{
	const struct rte_memzone *mz;
	unsigned dpdk_flags = 0;

	if ((flags & SPDK_MEMZONE_NO_IOVA_CONTIG) == 0) {
		dpdk_flags |= RTE_MEMZONE_IOVA_CONTIG;
	}

	if (numa_id == SPDK_ENV_NUMA_ID_ANY) {
		numa_id = SOCKET_ID_ANY;
	}

	mz = rte_memzone_reserve_aligned(name, len, numa_id, dpdk_flags, align);
	if (mz == NULL && !g_enforce_numa && numa_id != SOCKET_ID_ANY) {
		mz = rte_memzone_reserve_aligned(name, len, SOCKET_ID_ANY, dpdk_flags, align);
	}

	if (mz != NULL) {
		memset(mz->addr, 0, len);
		return mz->addr;
	} else {
		return NULL;
	}
}

void *
spdk_memzone_reserve(const char *name, size_t len, int numa_id, unsigned flags)
{
	return spdk_memzone_reserve_aligned(name, len, numa_id, flags,
					    RTE_CACHE_LINE_SIZE);
}

void *
spdk_memzone_lookup(const char *name)
{
	const struct rte_memzone *mz = rte_memzone_lookup(name);

	if (mz != NULL) {
		return mz->addr;
	} else {
		return NULL;
	}
}

int
spdk_memzone_free(const char *name)
{
	const struct rte_memzone *mz = rte_memzone_lookup(name);

	if (mz != NULL) {
		return rte_memzone_free(mz);
	}

	return -1;
}

void
spdk_memzone_dump(FILE *f)
{
	rte_memzone_dump(f);
}

struct spdk_mempool *
spdk_mempool_create_ctor(const char *name, size_t count,
			 size_t ele_size, size_t cache_size, int numa_id,
			 spdk_mempool_obj_cb_t *obj_init, void *obj_init_arg)
{
	struct rte_mempool *mp;
	size_t tmp;

	if (numa_id == SPDK_ENV_NUMA_ID_ANY) {
		numa_id = SOCKET_ID_ANY;
	}

	/* No more than half of all elements can be in cache */
	tmp = (count / 2) / rte_lcore_count();
	if (cache_size > tmp) {
		cache_size = tmp;
	}

	if (cache_size > RTE_MEMPOOL_CACHE_MAX_SIZE) {
		cache_size = RTE_MEMPOOL_CACHE_MAX_SIZE;
	}

	mp = rte_mempool_create(name, count, ele_size, cache_size,
				0, NULL, NULL, (rte_mempool_obj_cb_t *)obj_init, obj_init_arg,
				numa_id, 0);
	if (mp == NULL && !g_enforce_numa && numa_id != SOCKET_ID_ANY) {
		mp = rte_mempool_create(name, count, ele_size, cache_size,
					0, NULL, NULL, (rte_mempool_obj_cb_t *)obj_init, obj_init_arg,
					SOCKET_ID_ANY, 0);
	}

	return (struct spdk_mempool *)mp;
}


struct spdk_mempool *
spdk_mempool_create(const char *name, size_t count,
		    size_t ele_size, size_t cache_size, int numa_id)
{
	return spdk_mempool_create_ctor(name, count, ele_size, cache_size, numa_id,
					NULL, NULL);
}

char *
spdk_mempool_get_name(struct spdk_mempool *mp)
{
	return ((struct rte_mempool *)mp)->name;
}

void
spdk_mempool_free(struct spdk_mempool *mp)
{
	rte_mempool_free((struct rte_mempool *)mp);
}

void *
spdk_mempool_get(struct spdk_mempool *mp)
{
	void *ele = NULL;
	int rc;

	rc = rte_mempool_get((struct rte_mempool *)mp, &ele);
	if (rc != 0) {
		return NULL;
	}
	return ele;
}

int
spdk_mempool_get_bulk(struct spdk_mempool *mp, void **ele_arr, size_t count)
{
	return rte_mempool_get_bulk((struct rte_mempool *)mp, ele_arr, count);
}

void
spdk_mempool_put(struct spdk_mempool *mp, void *ele)
{
	rte_mempool_put((struct rte_mempool *)mp, ele);
}

void
spdk_mempool_put_bulk(struct spdk_mempool *mp, void **ele_arr, size_t count)
{
	rte_mempool_put_bulk((struct rte_mempool *)mp, ele_arr, count);
}

size_t
spdk_mempool_count(const struct spdk_mempool *pool)
{
	return rte_mempool_avail_count((struct rte_mempool *)pool);
}

uint32_t
spdk_mempool_obj_iter(struct spdk_mempool *mp, spdk_mempool_obj_cb_t obj_cb,
		      void *obj_cb_arg)
{
	return rte_mempool_obj_iter((struct rte_mempool *)mp, (rte_mempool_obj_cb_t *)obj_cb,
				    obj_cb_arg);
}

struct env_mempool_mem_iter_ctx {
	spdk_mempool_mem_cb_t *user_cb;
	void *user_arg;
};

static void
mempool_mem_iter_remap(struct rte_mempool *mp, void *opaque, struct rte_mempool_memhdr *memhdr,
		       unsigned mem_idx)
{
	struct env_mempool_mem_iter_ctx *ctx = opaque;

	ctx->user_cb((struct spdk_mempool *)mp, ctx->user_arg, memhdr->addr, memhdr->iova, memhdr->len,
		     mem_idx);
}

uint32_t
spdk_mempool_mem_iter(struct spdk_mempool *mp, spdk_mempool_mem_cb_t mem_cb,
		      void *mem_cb_arg)
{
	struct env_mempool_mem_iter_ctx ctx = {
		.user_cb = mem_cb,
		.user_arg = mem_cb_arg
	};

	return rte_mempool_mem_iter((struct rte_mempool *)mp, mempool_mem_iter_remap, &ctx);
}

struct spdk_mempool *
spdk_mempool_lookup(const char *name)
{
	return (struct spdk_mempool *)rte_mempool_lookup(name);
}

bool
spdk_process_is_primary(void)
{
	return (rte_eal_process_type() == RTE_PROC_PRIMARY);
}

uint64_t
spdk_get_ticks(void)
{
	return rte_get_timer_cycles();
}

uint64_t
spdk_get_ticks_hz(void)
{
	return rte_get_timer_hz();
}

void
spdk_delay_us(unsigned int us)
{
	rte_delay_us(us);
}

void
spdk_pause(void)
{
	rte_pause();
}

void
spdk_unaffinitize_thread(void)
{
	rte_cpuset_t new_cpuset;
	long num_cores, i;

	if (g_is_thread_unaffinitized) {
		return;
	}

	CPU_ZERO(&new_cpuset);

	num_cores = sysconf(_SC_NPROCESSORS_CONF);

	/* Create a mask containing all CPUs */
	for (i = 0; i < num_cores; i++) {
		CPU_SET(i, &new_cpuset);
	}

	rte_thread_set_affinity(&new_cpuset);
	g_is_thread_unaffinitized = true;
}

void *
spdk_call_unaffinitized(void *cb(void *arg), void *arg)
{
	rte_cpuset_t orig_cpuset;
	void *ret;

	if (cb == NULL) {
		return NULL;
	}

	if (g_is_thread_unaffinitized) {
		ret = cb(arg);
	} else {
		rte_thread_get_affinity(&orig_cpuset);
		spdk_unaffinitize_thread();

		ret = cb(arg);

		rte_thread_set_affinity(&orig_cpuset);
		g_is_thread_unaffinitized = false;
	}

	return ret;
}

struct spdk_ring *
spdk_ring_create(enum spdk_ring_type type, size_t count, int numa_id)
{
	char ring_name[64];
	static uint32_t ring_num = 0;
	unsigned flags = RING_F_EXACT_SZ;
	struct rte_ring *ring;

	switch (type) {
	case SPDK_RING_TYPE_SP_SC:
		flags |= RING_F_SP_ENQ | RING_F_SC_DEQ;
		break;
	case SPDK_RING_TYPE_MP_SC:
		flags |= RING_F_SC_DEQ;
		break;
	case SPDK_RING_TYPE_MP_MC:
		flags |= 0;
		break;
	default:
		return NULL;
	}

	snprintf(ring_name, sizeof(ring_name), "ring_%u_%d",
		 __atomic_fetch_add(&ring_num, 1, __ATOMIC_RELAXED), getpid());

	ring = rte_ring_create(ring_name, count, numa_id, flags);
	if (ring == NULL && !g_enforce_numa && numa_id != SOCKET_ID_ANY) {
		ring = rte_ring_create(ring_name, count, SOCKET_ID_ANY, flags);
	}
	return (struct spdk_ring *)ring;
}

void
spdk_ring_free(struct spdk_ring *ring)
{
	rte_ring_free((struct rte_ring *)ring);
}

size_t
spdk_ring_count(struct spdk_ring *ring)
{
	return rte_ring_count((struct rte_ring *)ring);
}

size_t
spdk_ring_enqueue(struct spdk_ring *ring, void **objs, size_t count,
		  size_t *free_space)
{
	return rte_ring_enqueue_bulk((struct rte_ring *)ring, objs, count,
				     (unsigned int *)free_space);
}

size_t
spdk_ring_dequeue(struct spdk_ring *ring, void **objs, size_t count)
{
	return rte_ring_dequeue_burst((struct rte_ring *)ring, objs, count, NULL);
}

void
spdk_env_dpdk_dump_mem_stats(FILE *file)
{
	fprintf(file, "DPDK memory size %" PRIu64 "\n", rte_eal_get_physmem_size());
	fprintf(file, "DPDK memory layout\n");
	rte_dump_physmem_layout(file);
	fprintf(file, "DPDK memzones.\n");
	rte_memzone_dump(file);
	fprintf(file, "DPDK mempools.\n");
	rte_mempool_list_dump(file);
	fprintf(file, "DPDK malloc stats.\n");
	rte_malloc_dump_stats(file, NULL);
	fprintf(file, "DPDK malloc heaps.\n");
	rte_malloc_dump_heaps(file);
}

int
spdk_get_tid(void)
{
	return rte_sys_gettid();
}

void
mem_enforce_numa(void)
{
	g_enforce_numa = true;
}
