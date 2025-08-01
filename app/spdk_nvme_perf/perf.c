/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2015 Intel Corporation.
 *   All rights reserved.
 *
 *   Copyright (c) 2019-2021 Mellanox Technologies LTD. All rights reserved.
 *   Copyright (c) 2021, 2022 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "spdk/stdinc.h"

#include "spdk/config.h"
#include "spdk/env.h"
#include "spdk/fd.h"
#include "spdk/nvme.h"
#include "spdk_internal/nvme_util.h"
#include "spdk/vmd.h"
#include "spdk/queue.h"
#include "spdk/string.h"
#include "spdk/nvme_intel.h"
#include "spdk/histogram_data.h"
#include "spdk/endian.h"
#include "spdk/dif.h"
#include "spdk/util.h"
#include "spdk/log.h"
#include "spdk/likely.h"
#include "spdk/sock.h"
#include "spdk/zipf.h"
#include "spdk/nvmf.h"
#include "spdk/keyring.h"
#include "spdk/module/keyring/file.h"

#ifdef SPDK_CONFIG_URING
#include <liburing.h>
#endif

#if HAVE_LIBAIO
#include <libaio.h>
#endif

#define HELP_RETURN_CODE UINT16_MAX

struct ctrlr_entry {
	struct spdk_nvme_ctrlr			*ctrlr;
	enum spdk_nvme_transport_type		trtype;
	struct spdk_nvme_intel_rw_latency_page	*latency_page;

	struct spdk_nvme_qpair			**unused_qpairs;

	TAILQ_ENTRY(ctrlr_entry)		link;
	char					name[1024];
};

enum entry_type {
	ENTRY_TYPE_NVME_NS,
	ENTRY_TYPE_AIO_FILE,
	ENTRY_TYPE_URING_FILE,
};

struct ns_fn_table;

struct ns_entry {
	enum entry_type		type;
	const struct ns_fn_table	*fn_table;

	union {
		struct {
			struct spdk_nvme_ctrlr	*ctrlr;
			struct spdk_nvme_ns	*ns;
		} nvme;
#ifdef SPDK_CONFIG_URING
		struct {
			int			fd;
		} uring;
#endif
#if HAVE_LIBAIO
		struct {
			int                     fd;
		} aio;
#endif
	} u;

	TAILQ_ENTRY(ns_entry)	link;
	uint32_t		io_size_blocks;
	uint32_t		num_io_requests;
	uint64_t		size_in_ios;
	uint32_t		block_size;
	uint32_t		md_size;
	bool			md_interleave;
	uint64_t		seed;
	struct spdk_zipf	*zipf;
	bool			pi_loc;
	enum spdk_nvme_pi_type	pi_type;
	uint32_t		io_flags;
	char			name[1024];
};

static const double g_latency_cutoffs[] = {
	0.01,
	0.10,
	0.25,
	0.50,
	0.75,
	0.90,
	0.95,
	0.98,
	0.99,
	0.995,
	0.999,
	0.9999,
	0.99999,
	0.999999,
	0.9999999,
	-1,
};

struct ns_worker_stats {
	uint64_t		io_submitted;
	uint64_t		io_completed;
	uint64_t		last_io_completed;
	uint64_t		total_tsc;
	uint64_t		min_tsc;
	uint64_t		max_tsc;
	uint64_t		last_tsc;
	uint64_t		busy_tsc;
	uint64_t		idle_tsc;
	uint64_t		last_busy_tsc;
	uint64_t		last_idle_tsc;
};

struct ns_worker_ctx {
	struct ns_entry		*entry;
	struct ns_worker_stats	stats;
	uint64_t		current_queue_depth;
	uint64_t		offset_in_ios;
	bool			is_draining;

	union {
		struct {
			int				num_active_qpairs;
			int				num_all_qpairs;
			struct spdk_nvme_qpair		**qpair;
			struct spdk_nvme_poll_group	*group;
			int				last_qpair;
		} nvme;

#ifdef SPDK_CONFIG_URING
		struct {
			struct io_uring		ring;
			uint64_t		io_inflight;
			uint64_t		io_pending;
			struct io_uring_cqe	**cqes;

		} uring;
#endif
#if HAVE_LIBAIO
		struct {
			struct io_event		*events;
			io_context_t		ctx;
		} aio;
#endif
	} u;

	TAILQ_ENTRY(ns_worker_ctx)	link;

	TAILQ_HEAD(, perf_task)		queued_tasks;

	struct spdk_histogram_data	*histogram;
	int				status;
};

struct perf_task {
	struct ns_worker_ctx	*ns_ctx;
	struct iovec		*iovs; /* array of iovecs to transfer. */
	int			iovcnt; /* Number of iovecs in iovs array. */
	int			iovpos; /* Current iovec position. */
	uint32_t		iov_offset; /* Offset in current iovec. */
	struct iovec		md_iov;
	uint64_t		submit_tsc;
	bool			is_read;
	struct spdk_dif_ctx	dif_ctx;
#if HAVE_LIBAIO
	struct iocb		iocb;
#endif
	TAILQ_ENTRY(perf_task)	link;
};

struct worker_thread {
	TAILQ_HEAD(, ns_worker_ctx)	ns_ctx;
	TAILQ_ENTRY(worker_thread)	link;
	unsigned			lcore;
};

struct ns_fn_table {
	void	(*setup_payload)(struct perf_task *task, uint8_t pattern);

	int	(*submit_io)(struct perf_task *task, struct ns_worker_ctx *ns_ctx,
			     struct ns_entry *entry, uint64_t offset_in_ios);

	int64_t	(*check_io)(struct ns_worker_ctx *ns_ctx);

	void	(*verify_io)(struct perf_task *task, struct ns_entry *entry);

	int	(*init_ns_worker_ctx)(struct ns_worker_ctx *ns_ctx);

	void	(*cleanup_ns_worker_ctx)(struct ns_worker_ctx *ns_ctx);
	void	(*dump_transport_stats)(uint32_t lcore, struct ns_worker_ctx *ns_ctx);
};

static uint32_t g_io_unit_size = (UINT32_MAX & (~0x03));

static int g_outstanding_commands;

static bool g_latency_ssd_tracking_enable;
static int g_latency_sw_tracking_level;

static bool g_vmd;
static const char *g_workload_type;
static TAILQ_HEAD(, ctrlr_entry) g_controllers = TAILQ_HEAD_INITIALIZER(g_controllers);
static TAILQ_HEAD(, ns_entry) g_namespaces = TAILQ_HEAD_INITIALIZER(g_namespaces);
static uint32_t g_num_namespaces;
static TAILQ_HEAD(, worker_thread) g_workers = TAILQ_HEAD_INITIALIZER(g_workers);
static uint32_t g_num_workers = 0;
static bool g_use_every_core = false;
static uint32_t g_main_core;
static pthread_barrier_t g_worker_sync_barrier;

static uint64_t g_tsc_rate;

static bool g_monitor_perf_cores = false;

static uint32_t g_io_align = 0x200;
static bool g_io_align_specified;
static uint32_t g_io_size_bytes;
static uint32_t g_max_io_md_size;
static uint32_t g_max_io_size_blocks;
static uint32_t g_metacfg_pract_flag;
static uint32_t g_metacfg_prchk_flags;
static int g_rw_percentage = -1;
static int g_is_random;
static uint32_t g_queue_depth;
static int g_nr_io_queues_per_ns = 1;
static int g_nr_unused_io_queues;
static int g_time_in_sec;
static uint64_t g_number_ios;
static uint64_t g_elapsed_time_in_usec;
static int g_warmup_time_in_sec;
static uint32_t g_max_completions;
static uint32_t g_disable_sq_cmb;
static bool g_enable_interrupt;
static bool g_use_uring;
static bool g_warn;
static bool g_header_digest;
static bool g_data_digest;
static bool g_no_shn_notification;
static bool g_mix_specified;
/* The flag is used to exit the program while keep alive fails on the transport */
static bool g_exit;
/* Default to 10 seconds for the keep alive value. This value is arbitrary. */
static uint32_t g_keep_alive_timeout_in_ms = 10000;
static bool g_continue_on_error = false;
static uint32_t g_quiet_count = 1;
static double g_zipf_theta;
/* Set default io_queue_size to UINT16_MAX, NVMe driver will then reduce this
 * to MQES to maximize the io_queue_size as much as possible.
 */
static uint32_t g_io_queue_size = UINT16_MAX;

static uint32_t g_sock_zcopy_threshold;
static char *g_sock_threshold_impl;

static uint8_t g_transport_tos = 0;

static uint32_t g_rdma_srq_size;
static struct spdk_key *g_psk = NULL, *g_dhchap = NULL, *g_dhchap_ctrlr = NULL;

/* When user specifies -Q, some error messages are rate limited.  When rate
 * limited, we only print the error message every g_quiet_count times the
 * error occurs.
 *
 * Note: the __count is not thread safe, meaning the rate limiting will not
 * be exact when running perf with multiple thread with lots of errors.
 * Thread-local __count would mean rate-limiting per thread which doesn't
 * seem as useful.
 */
#define RATELIMIT_LOG(...) \
	{								\
		static uint64_t __count = 0;				\
		if ((__count % g_quiet_count) == 0) {			\
			if (__count > 0 && g_quiet_count > 1) {		\
				fprintf(stderr, "Message suppressed %" PRIu32 " times: ",	\
					g_quiet_count - 1);		\
			}						\
			fprintf(stderr, __VA_ARGS__);			\
		}							\
		__count++;						\
	}

static bool g_dump_transport_stats;
static pthread_mutex_t g_stats_mutex;

#define MAX_ALLOWED_PCI_DEVICE_NUM 128
static struct spdk_pci_addr g_allowed_pci_addr[MAX_ALLOWED_PCI_DEVICE_NUM];

struct _trid_entry {
	struct spdk_nvme_trid_entry entry;
	TAILQ_ENTRY(_trid_entry) tailq;
};

#define MAX_TRID_ENTRY 256
static struct _trid_entry g_trids[MAX_TRID_ENTRY];
static TAILQ_HEAD(, _trid_entry) g_trid_list = TAILQ_HEAD_INITIALIZER(g_trid_list);

static int g_file_optind; /* Index of first filename in argv */

static inline void task_complete(struct perf_task *task);

static void
perf_set_sock_opts(const char *impl_name, const char *field, uint32_t val, const char *valstr)
{
	struct spdk_sock_impl_opts sock_opts = {};
	size_t opts_size = sizeof(sock_opts);
	int rc;

	rc = spdk_sock_impl_get_opts(impl_name, &sock_opts, &opts_size);
	if (rc != 0) {
		if (errno == EINVAL) {
			fprintf(stderr, "Unknown sock impl %s\n", impl_name);
		} else {
			fprintf(stderr, "Failed to get opts for sock impl %s: error %d (%s)\n", impl_name, errno,
				strerror(errno));
		}
		return;
	}

	if (opts_size != sizeof(sock_opts)) {
		fprintf(stderr, "Warning: sock_opts size mismatch. Expected %zu, received %zu\n",
			sizeof(sock_opts), opts_size);
		opts_size = sizeof(sock_opts);
	}

	if (!field) {
		fprintf(stderr, "Warning: no socket opts field specified\n");
		return;
	} else if (strcmp(field, "enable_zerocopy_send_client") == 0) {
		sock_opts.enable_zerocopy_send_client = val;
	} else if (strcmp(field, "tls_version") == 0) {
		sock_opts.tls_version = val;
	} else if (strcmp(field, "ktls") == 0) {
		sock_opts.enable_ktls = val;
	} else if (strcmp(field, "zerocopy_threshold") == 0) {
		sock_opts.zerocopy_threshold = val;
	} else {
		fprintf(stderr, "Warning: invalid or unprocessed socket opts field: %s\n", field);
		return;
	}

	if (spdk_sock_impl_set_opts(impl_name, &sock_opts, opts_size)) {
		fprintf(stderr, "Failed to set %s: %d for sock impl %s : error %d (%s)\n", field, val, impl_name,
			errno, strerror(errno));
	}
}

static void
nvme_perf_reset_sgl(void *ref, uint32_t sgl_offset)
{
	struct iovec *iov;
	struct perf_task *task = (struct perf_task *)ref;

	task->iov_offset = sgl_offset;
	for (task->iovpos = 0; task->iovpos < task->iovcnt; task->iovpos++) {
		iov = &task->iovs[task->iovpos];
		if (task->iov_offset < iov->iov_len) {
			break;
		}

		task->iov_offset -= iov->iov_len;
	}
}

static int
nvme_perf_next_sge(void *ref, void **address, uint32_t *length)
{
	struct iovec *iov;
	struct perf_task *task = (struct perf_task *)ref;

	assert(task->iovpos < task->iovcnt);

	iov = &task->iovs[task->iovpos];
	assert(task->iov_offset <= iov->iov_len);

	*address = iov->iov_base + task->iov_offset;
	*length = iov->iov_len - task->iov_offset;
	task->iovpos++;
	task->iov_offset = 0;

	return 0;
}

static int
nvme_perf_allocate_iovs(struct perf_task *task, void *buf, uint32_t length)
{
	int iovpos = 0;
	struct iovec *iov;
	uint32_t offset = 0;

	task->iovcnt = SPDK_CEIL_DIV(length, (uint64_t)g_io_unit_size);
	task->iovs = calloc(task->iovcnt, sizeof(struct iovec));
	if (!task->iovs) {
		return -1;
	}

	while (length > 0) {
		iov = &task->iovs[iovpos];
		iov->iov_len = spdk_min(length, g_io_unit_size);
		iov->iov_base = buf + offset;
		length -= iov->iov_len;
		offset += iov->iov_len;
		iovpos++;
	}

	return 0;
}

#ifdef SPDK_CONFIG_URING

static void
uring_setup_payload(struct perf_task *task, uint8_t pattern)
{
	struct iovec *iov;

	task->iovs = calloc(1, sizeof(struct iovec));
	if (!task->iovs) {
		fprintf(stderr, "perf task failed to allocate iovs\n");
		exit(1);
	}
	task->iovcnt = 1;

	iov = &task->iovs[0];
	iov->iov_base = spdk_dma_zmalloc(g_io_size_bytes, g_io_align, NULL);
	iov->iov_len = g_io_size_bytes;
	if (iov->iov_base == NULL) {
		fprintf(stderr, "spdk_dma_zmalloc() for task->iovs[0].iov_base failed\n");
		free(task->iovs);
		exit(1);
	}
	memset(iov->iov_base, pattern, iov->iov_len);
}

static int
uring_submit_io(struct perf_task *task, struct ns_worker_ctx *ns_ctx,
		struct ns_entry *entry, uint64_t offset_in_ios)
{
	struct io_uring_sqe *sqe;

	sqe = io_uring_get_sqe(&ns_ctx->u.uring.ring);
	if (!sqe) {
		fprintf(stderr, "Cannot get sqe\n");
		return -1;
	}

	if (task->is_read) {
		io_uring_prep_readv(sqe, entry->u.uring.fd, task->iovs, 1, offset_in_ios * task->iovs[0].iov_len);
	} else {
		io_uring_prep_writev(sqe, entry->u.uring.fd, task->iovs, 1, offset_in_ios * task->iovs[0].iov_len);
	}

	io_uring_sqe_set_data(sqe, task);
	ns_ctx->u.uring.io_pending++;

	return 0;
}

static int64_t
uring_check_io(struct ns_worker_ctx *ns_ctx)
{
	int i, to_complete, to_submit, count = 0, ret = 0;
	struct perf_task *task;

	to_submit = ns_ctx->u.uring.io_pending;

	if (to_submit > 0) {
		/* If there are I/O to submit, use io_uring_submit here.
		 * It will automatically call spdk_io_uring_enter appropriately. */
		ret = io_uring_submit(&ns_ctx->u.uring.ring);
		if (ret < 0) {
			ns_ctx->status = 1;
			return -1;
		}
		ns_ctx->u.uring.io_pending = 0;
		ns_ctx->u.uring.io_inflight += to_submit;
	}

	to_complete = ns_ctx->u.uring.io_inflight;
	if (to_complete > 0) {
		count = io_uring_peek_batch_cqe(&ns_ctx->u.uring.ring, ns_ctx->u.uring.cqes, to_complete);
		ns_ctx->u.uring.io_inflight -= count;
		for (i = 0; i < count; i++) {
			int res;

			assert(ns_ctx->u.uring.cqes[i] != NULL);
			task = (struct perf_task *)ns_ctx->u.uring.cqes[i]->user_data;
			res = ns_ctx->u.uring.cqes[i]->res;
			if (res != (int)task->iovs[0].iov_len) {
				fprintf(stderr, "cqe->status=%d, iov_len=%d\n", res,
					(int)task->iovs[0].iov_len);
				ns_ctx->status = 1;
				if (res == -EIO) {
					/* The block device has been removed.
					 * Stop trying to send I/O to it.
					 */
					ns_ctx->is_draining = true;
				}
			}
			io_uring_cqe_seen(&ns_ctx->u.uring.ring, ns_ctx->u.uring.cqes[i]);
			task_complete(task);
		}
	}
	return count;
}

static void
uring_verify_io(struct perf_task *task, struct ns_entry *entry)
{
}

static int
uring_init_ns_worker_ctx(struct ns_worker_ctx *ns_ctx)
{
	if (io_uring_queue_init(g_queue_depth, &ns_ctx->u.uring.ring, 0) < 0) {
		SPDK_ERRLOG("uring I/O context setup failure\n");
		return -1;
	}

	ns_ctx->u.uring.cqes = calloc(g_queue_depth, sizeof(struct io_uring_cqe *));
	if (!ns_ctx->u.uring.cqes) {
		io_uring_queue_exit(&ns_ctx->u.uring.ring);
		return -1;
	}

	return 0;
}

static void
uring_cleanup_ns_worker_ctx(struct ns_worker_ctx *ns_ctx)
{
	io_uring_queue_exit(&ns_ctx->u.uring.ring);
	free(ns_ctx->u.uring.cqes);
}

static const struct ns_fn_table uring_fn_table = {
	.setup_payload          = uring_setup_payload,
	.submit_io              = uring_submit_io,
	.check_io               = uring_check_io,
	.verify_io              = uring_verify_io,
	.init_ns_worker_ctx     = uring_init_ns_worker_ctx,
	.cleanup_ns_worker_ctx  = uring_cleanup_ns_worker_ctx,
};

#endif

#ifdef HAVE_LIBAIO
static void
aio_setup_payload(struct perf_task *task, uint8_t pattern)
{
	struct iovec *iov;

	task->iovs = calloc(1, sizeof(struct iovec));
	if (!task->iovs) {
		fprintf(stderr, "perf task failed to allocate iovs\n");
		exit(1);
	}
	task->iovcnt = 1;

	iov = &task->iovs[0];
	iov->iov_base = spdk_dma_zmalloc(g_io_size_bytes, g_io_align, NULL);
	iov->iov_len = g_io_size_bytes;
	if (iov->iov_base == NULL) {
		fprintf(stderr, "spdk_dma_zmalloc() for task->iovs[0].iov_base failed\n");
		free(task->iovs);
		exit(1);
	}
	memset(iov->iov_base, pattern, iov->iov_len);
}

static int
aio_submit(io_context_t aio_ctx, struct iocb *iocb, int fd, enum io_iocb_cmd cmd,
	   struct iovec *iov, uint64_t offset, void *cb_ctx)
{
	iocb->aio_fildes = fd;
	iocb->aio_reqprio = 0;
	iocb->aio_lio_opcode = cmd;
	iocb->u.c.buf = iov->iov_base;
	iocb->u.c.nbytes = iov->iov_len;
	iocb->u.c.offset = offset * iov->iov_len;
	iocb->data = cb_ctx;

	if (io_submit(aio_ctx, 1, &iocb) < 0) {
		printf("io_submit");
		return -1;
	}

	return 0;
}

static int
aio_submit_io(struct perf_task *task, struct ns_worker_ctx *ns_ctx,
	      struct ns_entry *entry, uint64_t offset_in_ios)
{
	if (task->is_read) {
		return aio_submit(ns_ctx->u.aio.ctx, &task->iocb, entry->u.aio.fd, IO_CMD_PREAD,
				  task->iovs, offset_in_ios, task);
	} else {
		return aio_submit(ns_ctx->u.aio.ctx, &task->iocb, entry->u.aio.fd, IO_CMD_PWRITE,
				  task->iovs, offset_in_ios, task);
	}
}

static int64_t
aio_check_io(struct ns_worker_ctx *ns_ctx)
{
	int count, i;
	struct timespec timeout;
	struct perf_task *task;

	timeout.tv_sec = 0;
	timeout.tv_nsec = 0;

	count = io_getevents(ns_ctx->u.aio.ctx, 1, g_queue_depth, ns_ctx->u.aio.events, &timeout);
	if (count < 0) {
		fprintf(stderr, "io_getevents error\n");
		ns_ctx->status = 1;
		return -1;
	}

	for (i = 0; i < count; i++) {
		unsigned long res;

		task = (struct perf_task *)ns_ctx->u.aio.events[i].data;
		res = ns_ctx->u.aio.events[i].res;
		if (res != (uint64_t)task->iovs[0].iov_len) {
			fprintf(stderr, "event->res=%ld, iov_len=%lu\n", (long)res,
				(uint64_t)task->iovs[0].iov_len);
			ns_ctx->status = 1;
			if ((long)res == -EIO) {
				/* The block device has been removed.  Stop trying to send I/O to it. */
				ns_ctx->is_draining = true;
			}
		}
		task_complete(ns_ctx->u.aio.events[i].data);
	}
	return count;
}

static void
aio_verify_io(struct perf_task *task, struct ns_entry *entry)
{
}

static int
aio_init_ns_worker_ctx(struct ns_worker_ctx *ns_ctx)
{
	ns_ctx->u.aio.events = calloc(g_queue_depth, sizeof(struct io_event));
	if (!ns_ctx->u.aio.events) {
		return -1;
	}
	ns_ctx->u.aio.ctx = 0;
	if (io_setup(g_queue_depth, &ns_ctx->u.aio.ctx) < 0) {
		free(ns_ctx->u.aio.events);
		perror("io_setup");
		return -1;
	}
	return 0;
}

static void
aio_cleanup_ns_worker_ctx(struct ns_worker_ctx *ns_ctx)
{
	io_destroy(ns_ctx->u.aio.ctx);
	free(ns_ctx->u.aio.events);
}

static const struct ns_fn_table aio_fn_table = {
	.setup_payload		= aio_setup_payload,
	.submit_io		= aio_submit_io,
	.check_io		= aio_check_io,
	.verify_io		= aio_verify_io,
	.init_ns_worker_ctx	= aio_init_ns_worker_ctx,
	.cleanup_ns_worker_ctx	= aio_cleanup_ns_worker_ctx,
};

#endif /* HAVE_LIBAIO */

#if defined(HAVE_LIBAIO) || defined(SPDK_CONFIG_URING)

static int
register_file(const char *path)
{
	struct ns_entry *entry;

	int flags, fd;
	uint64_t size;
	uint32_t blklen;

	if (g_rw_percentage == 100) {
		flags = O_RDONLY;
	} else if (g_rw_percentage == 0) {
		flags = O_WRONLY;
	} else {
		flags = O_RDWR;
	}

	flags |= O_DIRECT;

	fd = open(path, flags);
	if (fd < 0) {
		fprintf(stderr, "Could not open device %s: %s\n", path, strerror(errno));
		return -1;
	}

	size = spdk_fd_get_size(fd);
	if (size == 0) {
		fprintf(stderr, "Could not determine size of device %s\n", path);
		close(fd);
		return -1;
	}

	blklen = spdk_fd_get_blocklen(fd);
	if (blklen == 0) {
		fprintf(stderr, "Could not determine block size of device %s\n", path);
		close(fd);
		return -1;
	}

	/*
	 * TODO: This should really calculate the LCM of the current g_io_align and blklen.
	 * For now, it's fairly safe to just assume all block sizes are powers of 2.
	 */
	if (g_io_align < blklen) {
		if (g_io_align_specified) {
			fprintf(stderr, "Wrong IO alignment (%u). aio requires block-sized alignment (%u)\n", g_io_align,
				blklen);
			close(fd);
			return -1;
		}

		g_io_align = blklen;
	}

	entry = calloc(1, sizeof(struct ns_entry));
	if (entry == NULL) {
		close(fd);
		perror("ns_entry malloc");
		return -1;
	}

	if (g_use_uring) {
#ifdef SPDK_CONFIG_URING
		entry->type = ENTRY_TYPE_URING_FILE;
		entry->fn_table = &uring_fn_table;
		entry->u.uring.fd = fd;
#endif
	} else {
#if HAVE_LIBAIO
		entry->type = ENTRY_TYPE_AIO_FILE;
		entry->fn_table = &aio_fn_table;
		entry->u.aio.fd = fd;
#endif
	}
	entry->size_in_ios = size / g_io_size_bytes;
	entry->io_size_blocks = g_io_size_bytes / blklen;

	if (g_is_random) {
		entry->seed = rand();
		if (g_zipf_theta > 0) {
			entry->zipf = spdk_zipf_create(entry->size_in_ios, g_zipf_theta, 0);
		}
	}

	snprintf(entry->name, sizeof(entry->name), "%s", path);

	g_num_namespaces++;
	TAILQ_INSERT_TAIL(&g_namespaces, entry, link);

	return 0;
}

static int
register_files(int argc, char **argv)
{
	int i;

	/* Treat everything after the options as files for AIO/URING */
	for (i = g_file_optind; i < argc; i++) {
		if (register_file(argv[i]) != 0) {
			return 1;
		}
	}

	return 0;
}
#endif

static void io_complete(void *ctx, const struct spdk_nvme_cpl *cpl);

static void
nvme_setup_payload(struct perf_task *task, uint8_t pattern)
{
	struct spdk_nvme_ctrlr *ctrlr;
	uint32_t max_io_size_bytes, max_io_md_size;
	int32_t numa_id;
	void *buf;
	int rc;

	ctrlr = task->ns_ctx->entry->u.nvme.ctrlr;
	numa_id = spdk_nvme_ctrlr_get_numa_id(ctrlr);

	/* maximum extended lba format size from all active namespace,
	 * it's same with g_io_size_bytes for namespace without metadata.
	 */
	max_io_size_bytes = g_io_size_bytes + g_max_io_md_size * g_max_io_size_blocks;
	buf = spdk_dma_zmalloc_socket(max_io_size_bytes, g_io_align, NULL, numa_id);
	if (buf == NULL) {
		fprintf(stderr, "task->buf spdk_dma_zmalloc failed\n");
		exit(1);
	}
	memset(buf, pattern, max_io_size_bytes);

	rc = nvme_perf_allocate_iovs(task, buf, max_io_size_bytes);
	if (rc < 0) {
		fprintf(stderr, "perf task failed to allocate iovs\n");
		spdk_dma_free(buf);
		exit(1);
	}

	max_io_md_size = g_max_io_md_size * g_max_io_size_blocks;
	if (max_io_md_size != 0) {
		task->md_iov.iov_base = spdk_dma_zmalloc(max_io_md_size, g_io_align, NULL);
		task->md_iov.iov_len = max_io_md_size;
		if (task->md_iov.iov_base == NULL) {
			fprintf(stderr, "task->md_buf spdk_dma_zmalloc failed\n");
			spdk_dma_free(task->iovs[0].iov_base);
			free(task->iovs);
			exit(1);
		}
	}
}

static int
nvme_submit_io(struct perf_task *task, struct ns_worker_ctx *ns_ctx,
	       struct ns_entry *entry, uint64_t offset_in_ios)
{
	uint64_t lba;
	int rc;
	int qp_num;
	struct spdk_dif_ctx_init_ext_opts dif_opts;

	enum dif_mode {
		DIF_MODE_NONE = 0,
		DIF_MODE_DIF = 1,
		DIF_MODE_DIX = 2,
	}  mode = DIF_MODE_NONE;

	lba = offset_in_ios * entry->io_size_blocks;

	if (entry->md_size != 0 && !(entry->io_flags & SPDK_NVME_IO_FLAGS_PRACT)) {
		if (entry->md_interleave) {
			mode = DIF_MODE_DIF;
		} else {
			mode = DIF_MODE_DIX;
		}
	}

	qp_num = ns_ctx->u.nvme.last_qpair;
	ns_ctx->u.nvme.last_qpair++;
	if (ns_ctx->u.nvme.last_qpair == ns_ctx->u.nvme.num_active_qpairs) {
		ns_ctx->u.nvme.last_qpair = 0;
	}

	if (mode != DIF_MODE_NONE) {
		dif_opts.size = SPDK_SIZEOF(&dif_opts, dif_pi_format);
		dif_opts.dif_pi_format = SPDK_DIF_PI_FORMAT_16;
		rc = spdk_dif_ctx_init(&task->dif_ctx, entry->block_size, entry->md_size,
				       entry->md_interleave, entry->pi_loc,
				       (enum spdk_dif_type)entry->pi_type, entry->io_flags,
				       lba, 0xFFFF, (uint16_t)entry->io_size_blocks, 0, 0, &dif_opts);
		if (rc != 0) {
			fprintf(stderr, "Initialization of DIF context failed\n");
			exit(1);
		}
	}

	if (task->is_read) {
		if (task->iovcnt == 1) {
			return spdk_nvme_ns_cmd_read_with_md(entry->u.nvme.ns, ns_ctx->u.nvme.qpair[qp_num],
							     task->iovs[0].iov_base, task->md_iov.iov_base,
							     lba,
							     entry->io_size_blocks, io_complete,
							     task, entry->io_flags,
							     task->dif_ctx.apptag_mask, task->dif_ctx.app_tag);
		} else {
			return spdk_nvme_ns_cmd_readv_with_md(entry->u.nvme.ns, ns_ctx->u.nvme.qpair[qp_num],
							      lba, entry->io_size_blocks,
							      io_complete, task, entry->io_flags,
							      nvme_perf_reset_sgl, nvme_perf_next_sge,
							      task->md_iov.iov_base,
							      task->dif_ctx.apptag_mask, task->dif_ctx.app_tag);
		}
	} else {
		switch (mode) {
		case DIF_MODE_DIF:
			rc = spdk_dif_generate(task->iovs, task->iovcnt, entry->io_size_blocks, &task->dif_ctx);
			if (rc != 0) {
				fprintf(stderr, "Generation of DIF failed\n");
				return rc;
			}
			break;
		case DIF_MODE_DIX:
			rc = spdk_dix_generate(task->iovs, task->iovcnt, &task->md_iov, entry->io_size_blocks,
					       &task->dif_ctx);
			if (rc != 0) {
				fprintf(stderr, "Generation of DIX failed\n");
				return rc;
			}
			break;
		default:
			break;
		}

		if (task->iovcnt == 1) {
			return spdk_nvme_ns_cmd_write_with_md(entry->u.nvme.ns, ns_ctx->u.nvme.qpair[qp_num],
							      task->iovs[0].iov_base, task->md_iov.iov_base,
							      lba,
							      entry->io_size_blocks, io_complete,
							      task, entry->io_flags,
							      task->dif_ctx.apptag_mask, task->dif_ctx.app_tag);
		} else {
			return spdk_nvme_ns_cmd_writev_with_md(entry->u.nvme.ns, ns_ctx->u.nvme.qpair[qp_num],
							       lba, entry->io_size_blocks,
							       io_complete, task, entry->io_flags,
							       nvme_perf_reset_sgl, nvme_perf_next_sge,
							       task->md_iov.iov_base,
							       task->dif_ctx.apptag_mask, task->dif_ctx.app_tag);
		}
	}
}

static void
perf_disconnect_cb(struct spdk_nvme_qpair *qpair, void *ctx)
{
	struct ns_worker_ctx *ns_ctx = ctx;

	ns_ctx->is_draining = true;
	ns_ctx->status = 1;
}

static int64_t
nvme_check_io(struct ns_worker_ctx *ns_ctx)
{
	int64_t rc;

	if (g_enable_interrupt) {
		rc = spdk_nvme_poll_group_wait(ns_ctx->u.nvme.group, perf_disconnect_cb);
	} else {
		rc = spdk_nvme_poll_group_process_completions(ns_ctx->u.nvme.group, g_max_completions,
				perf_disconnect_cb);
	}
	if (rc < 0) {
		fprintf(stderr, "NVMe io qpair process completion error\n");
		ns_ctx->status = 1;
		return -1;
	}
	return rc;
}

static void
nvme_verify_io(struct perf_task *task, struct ns_entry *entry)
{
	struct spdk_dif_error err_blk = {};
	int rc;

	if (!task->is_read || (entry->io_flags & SPDK_NVME_IO_FLAGS_PRACT)) {
		return;
	}

	if (entry->md_interleave) {
		rc = spdk_dif_verify(task->iovs, task->iovcnt, entry->io_size_blocks, &task->dif_ctx,
				     &err_blk);
		if (rc != 0) {
			fprintf(stderr, "DIF error detected. type=%d, offset=%" PRIu32 "\n",
				err_blk.err_type, err_blk.err_offset);
		}
	} else {
		rc = spdk_dix_verify(task->iovs, task->iovcnt, &task->md_iov, entry->io_size_blocks,
				     &task->dif_ctx, &err_blk);
		if (rc != 0) {
			fprintf(stderr, "DIX error detected. type=%d, offset=%" PRIu32 "\n",
				err_blk.err_type, err_blk.err_offset);
		}
	}
}

/*
 * TODO: If a controller has multiple namespaces, they could all use the same queue.
 *  For now, give each namespace/thread combination its own queue.
 */
static int
nvme_init_ns_worker_ctx(struct ns_worker_ctx *ns_ctx)
{
	const struct spdk_nvme_ctrlr_opts *ctrlr_opts;
	struct spdk_nvme_io_qpair_opts opts;
	struct ns_entry *entry = ns_ctx->entry;
	struct spdk_nvme_poll_group *group;
	struct spdk_nvme_qpair *qpair;
	uint64_t poll_timeout_tsc;
	int i, rc;

	ns_ctx->u.nvme.num_active_qpairs = g_nr_io_queues_per_ns;
	ns_ctx->u.nvme.num_all_qpairs = g_nr_io_queues_per_ns + g_nr_unused_io_queues;
	ns_ctx->u.nvme.qpair = calloc(ns_ctx->u.nvme.num_all_qpairs, sizeof(struct spdk_nvme_qpair *));
	if (!ns_ctx->u.nvme.qpair) {
		return -1;
	}

	spdk_nvme_ctrlr_get_default_io_qpair_opts(entry->u.nvme.ctrlr, &opts, sizeof(opts));
	if (opts.io_queue_requests < entry->num_io_requests) {
		opts.io_queue_requests = entry->num_io_requests;
	}

	opts.delay_cmd_submit = g_enable_interrupt ? false : true;
	opts.create_only = true;

	ctrlr_opts = spdk_nvme_ctrlr_get_opts(entry->u.nvme.ctrlr);
	opts.async_mode = !(spdk_nvme_ctrlr_get_transport_id(entry->u.nvme.ctrlr)->trtype ==
			    SPDK_NVME_TRANSPORT_PCIE
			    && ns_ctx->u.nvme.num_all_qpairs > ctrlr_opts->admin_queue_size);

	ns_ctx->u.nvme.group = spdk_nvme_poll_group_create(ns_ctx, NULL);
	if (ns_ctx->u.nvme.group == NULL) {
		goto poll_group_failed;
	}

	group = ns_ctx->u.nvme.group;
	for (i = 0; i < ns_ctx->u.nvme.num_all_qpairs; i++) {
		ns_ctx->u.nvme.qpair[i] = spdk_nvme_ctrlr_alloc_io_qpair(entry->u.nvme.ctrlr, &opts,
					  sizeof(opts));
		qpair = ns_ctx->u.nvme.qpair[i];
		if (!qpair) {
			printf("ERROR: spdk_nvme_ctrlr_alloc_io_qpair failed\n");
			goto qpair_failed;
		}

		if (spdk_nvme_poll_group_add(group, qpair)) {
			printf("ERROR: unable to add I/O qpair to poll group.\n");
			spdk_nvme_ctrlr_free_io_qpair(qpair);
			goto qpair_failed;
		}

		if (spdk_nvme_ctrlr_connect_io_qpair(entry->u.nvme.ctrlr, qpair)) {
			printf("ERROR: unable to connect I/O qpair.\n");
			spdk_nvme_ctrlr_free_io_qpair(qpair);
			goto qpair_failed;
		}
	}

	/* Busy poll here until all qpairs are connected - this ensures once we start
	 * I/O we aren't still waiting for some qpairs to connect. Limit the poll to
	 * 10 seconds though.
	 */
	poll_timeout_tsc = spdk_get_ticks() + 10 * spdk_get_ticks_hz();
	rc = -EAGAIN;
	while (spdk_get_ticks() < poll_timeout_tsc && rc == -EAGAIN) {
		spdk_nvme_poll_group_process_completions(group, 0, perf_disconnect_cb);
		rc = spdk_nvme_poll_group_all_connected(group);
		if (rc == 0) {
			return 0;
		}
	}

	/* If we reach here, it means we either timed out, or some connection failed. */
	assert(spdk_get_ticks() > poll_timeout_tsc || rc == -EIO);

qpair_failed:
	for (; i > 0; --i) {
		spdk_nvme_ctrlr_free_io_qpair(ns_ctx->u.nvme.qpair[i - 1]);
	}

	spdk_nvme_poll_group_destroy(ns_ctx->u.nvme.group);
poll_group_failed:
	free(ns_ctx->u.nvme.qpair);
	return -1;
}

static void
nvme_cleanup_ns_worker_ctx(struct ns_worker_ctx *ns_ctx)
{
	int i;

	for (i = 0; i < ns_ctx->u.nvme.num_all_qpairs; i++) {
		spdk_nvme_ctrlr_free_io_qpair(ns_ctx->u.nvme.qpair[i]);
	}

	spdk_nvme_poll_group_destroy(ns_ctx->u.nvme.group);
	free(ns_ctx->u.nvme.qpair);
}

static void
nvme_dump_rdma_statistics(struct spdk_nvme_transport_poll_group_stat *stat)
{
	struct spdk_nvme_rdma_device_stat *device_stats;
	uint32_t i;

	printf("RDMA transport:\n");
	for (i = 0; i < stat->rdma.num_devices; i++) {
		device_stats = &stat->rdma.device_stats[i];
		printf("\tdev name:              %s\n", device_stats->name);
		printf("\tpolls:                 %"PRIu64"\n", device_stats->polls);
		printf("\tidle_polls:            %"PRIu64"\n", device_stats->idle_polls);
		printf("\tcompletions:           %"PRIu64"\n", device_stats->completions);
		printf("\tqueued_requests:       %"PRIu64"\n", device_stats->queued_requests);
		printf("\ttotal_send_wrs:        %"PRIu64"\n", device_stats->total_send_wrs);
		printf("\tsend_doorbell_updates: %"PRIu64"\n", device_stats->send_doorbell_updates);
		printf("\ttotal_recv_wrs:        %"PRIu64"\n", device_stats->total_recv_wrs);
		printf("\trecv_doorbell_updates: %"PRIu64"\n", device_stats->recv_doorbell_updates);
		printf("\t---------------------------------\n");
	}
}

static void
nvme_dump_pcie_statistics(struct spdk_nvme_transport_poll_group_stat *stat)
{
	struct spdk_nvme_pcie_stat *pcie_stat;

	pcie_stat = &stat->pcie;

	printf("PCIE transport:\n");
	printf("\tpolls:               %"PRIu64"\n", pcie_stat->polls);
	printf("\tidle_polls:          %"PRIu64"\n", pcie_stat->idle_polls);
	printf("\tcompletions:         %"PRIu64"\n", pcie_stat->completions);
	printf("\tcq_mmio_doorbell_updates: %"PRIu64"\n", pcie_stat->cq_mmio_doorbell_updates);
	printf("\tcq_shadow_doorbell_updates: %"PRIu64"\n", pcie_stat->cq_shadow_doorbell_updates);
	printf("\tsubmitted_requests:  %"PRIu64"\n", pcie_stat->submitted_requests);
	printf("\tsq_mmio_doorbell_updates:  %"PRIu64"\n", pcie_stat->sq_mmio_doorbell_updates);
	printf("\tsq_shadow_doorbell_updates:  %"PRIu64"\n", pcie_stat->sq_shadow_doorbell_updates);
	printf("\tqueued_requests:     %"PRIu64"\n", pcie_stat->queued_requests);
}

static void
nvme_dump_tcp_statistics(struct spdk_nvme_transport_poll_group_stat *stat)
{
	struct spdk_nvme_tcp_stat *tcp_stat;

	tcp_stat = &stat->tcp;

	printf("TCP transport:\n");
	printf("\tpolls:              %"PRIu64"\n", tcp_stat->polls);
	printf("\tidle_polls:         %"PRIu64"\n", tcp_stat->idle_polls);
	printf("\tsock_completions:   %"PRIu64"\n", tcp_stat->socket_completions);
	printf("\tnvme_completions:   %"PRIu64"\n", tcp_stat->nvme_completions);
	printf("\tsubmitted_requests: %"PRIu64"\n", tcp_stat->submitted_requests);
	printf("\tqueued_requests:    %"PRIu64"\n", tcp_stat->queued_requests);
}

static void
nvme_dump_transport_stats(uint32_t lcore, struct ns_worker_ctx *ns_ctx)
{
	struct spdk_nvme_poll_group *group;
	struct spdk_nvme_poll_group_stat *stat = NULL;
	uint32_t i;
	int rc;

	group = ns_ctx->u.nvme.group;
	if (group == NULL) {
		return;
	}

	rc = spdk_nvme_poll_group_get_stats(group, &stat);
	if (rc) {
		fprintf(stderr, "Can't get transport stats, error %d\n", rc);
		return;
	}

	printf("\n====================\n");
	printf("lcore %u, ns %s statistics:\n", lcore, ns_ctx->entry->name);

	for (i = 0; i < stat->num_transports; i++) {
		switch (stat->transport_stat[i]->trtype) {
		case SPDK_NVME_TRANSPORT_RDMA:
			nvme_dump_rdma_statistics(stat->transport_stat[i]);
			break;
		case SPDK_NVME_TRANSPORT_PCIE:
			nvme_dump_pcie_statistics(stat->transport_stat[i]);
			break;
		case SPDK_NVME_TRANSPORT_TCP:
			nvme_dump_tcp_statistics(stat->transport_stat[i]);
			break;
		default:
			fprintf(stderr, "Unknown transport statistics %d %s\n", stat->transport_stat[i]->trtype,
				spdk_nvme_transport_id_trtype_str(stat->transport_stat[i]->trtype));
		}
	}

	spdk_nvme_poll_group_free_stats(group, stat);
}

static const struct ns_fn_table nvme_fn_table = {
	.setup_payload		= nvme_setup_payload,
	.submit_io		= nvme_submit_io,
	.check_io		= nvme_check_io,
	.verify_io		= nvme_verify_io,
	.init_ns_worker_ctx	= nvme_init_ns_worker_ctx,
	.cleanup_ns_worker_ctx	= nvme_cleanup_ns_worker_ctx,
	.dump_transport_stats	= nvme_dump_transport_stats
};

static void
register_ns(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_ns *ns)
{
	struct ns_entry *entry;
	const struct spdk_nvme_ctrlr_data *cdata;
	uint32_t max_xfer_size, entries, sector_size;
	uint64_t ns_size;
	struct spdk_nvme_io_qpair_opts opts;

	cdata = spdk_nvme_ctrlr_get_data(ctrlr);

	if (!spdk_nvme_ns_is_active(ns)) {
		printf("Controller %-20.20s (%-20.20s): Skipping inactive NS %u\n",
		       cdata->mn, cdata->sn,
		       spdk_nvme_ns_get_id(ns));
		g_warn = true;
		return;
	}

	ns_size = spdk_nvme_ns_get_size(ns);
	sector_size = spdk_nvme_ns_get_sector_size(ns);

	if (ns_size < g_io_size_bytes || sector_size > g_io_size_bytes) {
		printf("WARNING: controller %-20.20s (%-20.20s) ns %u has invalid "
		       "ns size %" PRIu64 " / block size %u for I/O size %u\n",
		       cdata->mn, cdata->sn, spdk_nvme_ns_get_id(ns),
		       ns_size, spdk_nvme_ns_get_sector_size(ns), g_io_size_bytes);
		g_warn = true;
		return;
	}

	max_xfer_size = spdk_nvme_ns_get_max_io_xfer_size(ns);
	spdk_nvme_ctrlr_get_default_io_qpair_opts(ctrlr, &opts, sizeof(opts));
	/* NVMe driver may add additional entries based on
	 * stripe size and maximum transfer size, we assume
	 * 1 more entry be used for stripe.
	 */
	entries = (g_io_size_bytes - 1) / max_xfer_size + 2;
	if ((g_queue_depth * entries) > opts.io_queue_size) {
		printf("Controller IO queue size %u, less than required.\n",
		       opts.io_queue_size);
		printf("Consider using lower queue depth or smaller IO size, because "
		       "IO requests may be queued at the NVMe driver.\n");
	}
	/* For requests which have children requests, parent request itself
	 * will also occupy 1 entry.
	 */
	entries += 1;

	entry = calloc(1, sizeof(struct ns_entry));
	if (entry == NULL) {
		perror("ns_entry malloc");
		exit(1);
	}

	entry->type = ENTRY_TYPE_NVME_NS;
	entry->fn_table = &nvme_fn_table;
	entry->u.nvme.ctrlr = ctrlr;
	entry->u.nvme.ns = ns;
	entry->num_io_requests = entries * spdk_divide_round_up(g_queue_depth, g_nr_io_queues_per_ns);

	entry->size_in_ios = ns_size / g_io_size_bytes;
	entry->io_size_blocks = g_io_size_bytes / sector_size;

	if (g_is_random) {
		entry->seed = spdk_rand_xorshift64_seed();
		if (g_zipf_theta > 0) {
			entry->zipf = spdk_zipf_create(entry->size_in_ios, g_zipf_theta, 0);
		}
	}

	entry->block_size = spdk_nvme_ns_get_extended_sector_size(ns);
	entry->md_size = spdk_nvme_ns_get_md_size(ns);
	entry->md_interleave = spdk_nvme_ns_supports_extended_lba(ns);
	entry->pi_loc = spdk_nvme_ns_get_data(ns)->dps.md_start;
	entry->pi_type = spdk_nvme_ns_get_pi_type(ns);

	if (spdk_nvme_ns_get_flags(ns) & SPDK_NVME_NS_DPS_PI_SUPPORTED) {
		entry->io_flags = g_metacfg_pract_flag | g_metacfg_prchk_flags;
	}

	/* If metadata size = 8 bytes, PI is stripped (read) or inserted (write),
	 *  and so reduce metadata size from block size.  (If metadata size > 8 bytes,
	 *  PI is passed (read) or replaced (write).  So block size is not necessary
	 *  to change.)
	 */
	if ((entry->io_flags & SPDK_NVME_IO_FLAGS_PRACT) && (entry->md_size == 8)) {
		entry->block_size = spdk_nvme_ns_get_sector_size(ns);
	}

	if (g_io_size_bytes % entry->block_size != 0) {
		printf("WARNING: IO size %u (-o) is not a multiple of nsid %u sector size %u."
		       " Removing this ns from test\n", g_io_size_bytes, spdk_nvme_ns_get_id(ns), entry->block_size);
		g_warn = true;
		spdk_zipf_free(&entry->zipf);
		free(entry);
		return;
	}

	if (g_max_io_md_size < entry->md_size) {
		g_max_io_md_size = entry->md_size;
	}

	if (g_max_io_size_blocks < entry->io_size_blocks) {
		g_max_io_size_blocks = entry->io_size_blocks;
	}

	spdk_nvme_build_name(entry->name, sizeof(entry->name), ctrlr, ns);

	g_num_namespaces++;
	TAILQ_INSERT_TAIL(&g_namespaces, entry, link);
}

static void
unregister_namespaces(void)
{
	struct ns_entry *entry, *tmp;

	TAILQ_FOREACH_SAFE(entry, &g_namespaces, link, tmp) {
		TAILQ_REMOVE(&g_namespaces, entry, link);
		spdk_zipf_free(&entry->zipf);
		if (g_use_uring) {
#ifdef SPDK_CONFIG_URING
			close(entry->u.uring.fd);
#endif
		} else {
#if HAVE_LIBAIO
			close(entry->u.aio.fd);
#endif
		}
		free(entry);
	}
}

static void
enable_latency_tracking_complete(void *cb_arg, const struct spdk_nvme_cpl *cpl)
{
	if (spdk_nvme_cpl_is_error(cpl)) {
		printf("enable_latency_tracking_complete failed\n");
	}
	g_outstanding_commands--;
}

static void
set_latency_tracking_feature(struct spdk_nvme_ctrlr *ctrlr, bool enable)
{
	int res;
	union spdk_nvme_intel_feat_latency_tracking latency_tracking;

	if (enable) {
		latency_tracking.bits.enable = 0x01;
	} else {
		latency_tracking.bits.enable = 0x00;
	}

	res = spdk_nvme_ctrlr_cmd_set_feature(ctrlr, SPDK_NVME_INTEL_FEAT_LATENCY_TRACKING,
					      latency_tracking.raw, 0, NULL, 0, enable_latency_tracking_complete, NULL);
	if (res) {
		printf("fail to allocate nvme request.\n");
		return;
	}
	g_outstanding_commands++;

	while (g_outstanding_commands) {
		spdk_nvme_ctrlr_process_admin_completions(ctrlr);
	}
}

static void
register_ctrlr(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_trid_entry *trid_entry)
{
	struct spdk_nvme_ns *ns;
	struct ctrlr_entry *entry = malloc(sizeof(struct ctrlr_entry));
	uint32_t nsid;

	if (entry == NULL) {
		perror("ctrlr_entry malloc");
		exit(1);
	}

	entry->latency_page = spdk_dma_zmalloc(sizeof(struct spdk_nvme_intel_rw_latency_page),
					       4096, NULL);
	if (entry->latency_page == NULL) {
		printf("Allocation error (latency page)\n");
		exit(1);
	}

	spdk_nvme_build_name(entry->name, sizeof(entry->name), ctrlr, NULL);
	printf("Attached to NVMe%s Controller at %s\n",
	       trid_entry->trid.trtype != SPDK_NVME_TRANSPORT_PCIE ? "oF" : "", entry->name);

	entry->ctrlr = ctrlr;
	entry->trtype = trid_entry->trid.trtype;
	TAILQ_INSERT_TAIL(&g_controllers, entry, link);

	if (g_latency_ssd_tracking_enable &&
	    spdk_nvme_ctrlr_is_feature_supported(ctrlr, SPDK_NVME_INTEL_FEAT_LATENCY_TRACKING)) {
		set_latency_tracking_feature(ctrlr, true);
	}

	if (trid_entry->nsid == 0) {
		for (nsid = spdk_nvme_ctrlr_get_first_active_ns(ctrlr);
		     nsid != 0; nsid = spdk_nvme_ctrlr_get_next_active_ns(ctrlr, nsid)) {
			ns = spdk_nvme_ctrlr_get_ns(ctrlr, nsid);
			if (ns == NULL) {
				continue;
			}
			register_ns(ctrlr, ns);
		}
	} else {
		ns = spdk_nvme_ctrlr_get_ns(ctrlr, trid_entry->nsid);
		if (!ns) {
			perror("Namespace does not exist.");
			exit(1);
		}

		register_ns(ctrlr, ns);
	}
}

static inline void
submit_single_io(struct perf_task *task)
{
	uint64_t		rand_value, offset_in_ios;
	int			rc;
	struct ns_worker_ctx	*ns_ctx = task->ns_ctx;
	struct ns_entry		*entry = ns_ctx->entry;

	assert(!ns_ctx->is_draining);

	if (entry->zipf) {
		offset_in_ios = spdk_zipf_generate(entry->zipf);
	} else if (g_is_random) {
		rand_value = spdk_rand_xorshift64(&entry->seed);
		offset_in_ios = rand_value % entry->size_in_ios;
	} else {
		offset_in_ios = ns_ctx->offset_in_ios++;
		if (ns_ctx->offset_in_ios == entry->size_in_ios) {
			ns_ctx->offset_in_ios = 0;
		}
	}

	task->submit_tsc = spdk_get_ticks();

	if ((g_rw_percentage == 100) ||
	    (g_rw_percentage != 0 &&
	     ((spdk_rand_xorshift64(&entry->seed) % 100) < (uint64_t)g_rw_percentage))) {
		task->is_read = true;
	} else {
		task->is_read = false;
	}

	rc = entry->fn_table->submit_io(task, ns_ctx, entry, offset_in_ios);

	if (spdk_unlikely(rc != 0)) {
		if (g_continue_on_error) {
			/* We can't just resubmit here or we can get in a loop that
			 * stack overflows. */
			TAILQ_INSERT_TAIL(&ns_ctx->queued_tasks, task, link);
		} else {
			RATELIMIT_LOG("starting I/O failed: %d\n", rc);
			spdk_dma_free(task->iovs[0].iov_base);
			free(task->iovs);
			spdk_dma_free(task->md_iov.iov_base);
			task->ns_ctx->status = 1;
			free(task);
		}
	} else {
		ns_ctx->current_queue_depth++;
		ns_ctx->stats.io_submitted++;
	}

	if (spdk_unlikely(g_number_ios && ns_ctx->stats.io_submitted >= g_number_ios)) {
		ns_ctx->is_draining = true;
	}
}

static inline void
task_complete(struct perf_task *task)
{
	struct ns_worker_ctx	*ns_ctx;
	uint64_t		tsc_diff;
	struct ns_entry		*entry;

	ns_ctx = task->ns_ctx;
	entry = ns_ctx->entry;
	ns_ctx->current_queue_depth--;
	ns_ctx->stats.io_completed++;
	tsc_diff = spdk_get_ticks() - task->submit_tsc;
	ns_ctx->stats.total_tsc += tsc_diff;
	if (spdk_unlikely(ns_ctx->stats.min_tsc > tsc_diff)) {
		ns_ctx->stats.min_tsc = tsc_diff;
	}
	if (spdk_unlikely(ns_ctx->stats.max_tsc < tsc_diff)) {
		ns_ctx->stats.max_tsc = tsc_diff;
	}
	if (spdk_unlikely(g_latency_sw_tracking_level > 0)) {
		spdk_histogram_data_tally(ns_ctx->histogram, tsc_diff);
	}

	if (spdk_unlikely(entry->md_size > 0)) {
		/* add application level verification for end-to-end data protection */
		entry->fn_table->verify_io(task, entry);
	}

	/*
	 * is_draining indicates when time has expired or io_submitted exceeded
	 * g_number_ios for the test run and we are just waiting for the previously
	 * submitted I/O to complete. In this case, do not submit a new I/O to
	 * replace the one just completed.
	 */
	if (spdk_unlikely(ns_ctx->is_draining)) {
		spdk_dma_free(task->iovs[0].iov_base);
		free(task->iovs);
		spdk_dma_free(task->md_iov.iov_base);
		free(task);
	} else {
		submit_single_io(task);
	}
}

static void
io_complete(void *ctx, const struct spdk_nvme_cpl *cpl)
{
	struct perf_task *task = ctx;

	if (spdk_unlikely(spdk_nvme_cpl_is_error(cpl))) {
		if (task->is_read) {
			RATELIMIT_LOG("Read completed with error (sct=%d, sc=%d)\n",
				      cpl->status.sct, cpl->status.sc);
		} else {
			RATELIMIT_LOG("Write completed with error (sct=%d, sc=%d)\n",
				      cpl->status.sct, cpl->status.sc);
		}
		if (!g_continue_on_error) {
			if (cpl->status.sct == SPDK_NVME_SCT_GENERIC &&
			    cpl->status.sc == SPDK_NVME_SC_INVALID_NAMESPACE_OR_FORMAT) {
				/* The namespace was hotplugged.  Stop trying to send I/O to it. */
				task->ns_ctx->is_draining = true;
			}

			task->ns_ctx->status = 1;
		}
	}

	task_complete(task);
}

static struct perf_task *
allocate_task(struct ns_worker_ctx *ns_ctx, int queue_depth)
{
	struct perf_task *task;

	task = calloc(1, sizeof(*task));
	if (task == NULL) {
		fprintf(stderr, "Out of memory allocating tasks\n");
		exit(1);
	}

	task->ns_ctx = ns_ctx;
	ns_ctx->entry->fn_table->setup_payload(task, queue_depth % 8 + 1);

	return task;
}

static void
submit_io(struct ns_worker_ctx *ns_ctx, int queue_depth)
{
	struct perf_task *task;

	while (queue_depth-- > 0) {
		task = allocate_task(ns_ctx, queue_depth);
		submit_single_io(task);
	}
}

static int
init_ns_worker_ctx(struct ns_worker_ctx *ns_ctx)
{
	TAILQ_INIT(&ns_ctx->queued_tasks);
	return ns_ctx->entry->fn_table->init_ns_worker_ctx(ns_ctx);
}

static void
cleanup_ns_worker_ctx(struct ns_worker_ctx *ns_ctx)
{
	struct perf_task *task, *ttask;

	TAILQ_FOREACH_SAFE(task, &ns_ctx->queued_tasks, link, ttask) {
		TAILQ_REMOVE(&ns_ctx->queued_tasks, task, link);
		task_complete(task);
	}
	ns_ctx->entry->fn_table->cleanup_ns_worker_ctx(ns_ctx);
}

static void
print_periodic_performance(bool warmup)
{
	uint64_t io_this_second;
	double mb_this_second;
	struct worker_thread *worker;
	struct ns_worker_ctx *ns_ctx;
	uint64_t busy_tsc;
	uint64_t idle_tsc;
	uint64_t core_busy_tsc = 0;
	uint64_t core_idle_tsc = 0;
	double core_busy_perc = 0;

	if (!isatty(STDOUT_FILENO)) {
		/* Don't print periodic stats if output is not going
		 * to a terminal.
		 */
		return;
	}
	io_this_second = 0;
	TAILQ_FOREACH(worker, &g_workers, link) {
		busy_tsc = 0;
		idle_tsc = 0;
		TAILQ_FOREACH(ns_ctx, &worker->ns_ctx, link) {
			io_this_second += ns_ctx->stats.io_completed - ns_ctx->stats.last_io_completed;
			ns_ctx->stats.last_io_completed = ns_ctx->stats.io_completed;

			if (g_monitor_perf_cores) {
				busy_tsc += ns_ctx->stats.busy_tsc - ns_ctx->stats.last_busy_tsc;
				idle_tsc += ns_ctx->stats.idle_tsc - ns_ctx->stats.last_idle_tsc;
				ns_ctx->stats.last_busy_tsc = ns_ctx->stats.busy_tsc;
				ns_ctx->stats.last_idle_tsc = ns_ctx->stats.idle_tsc;
			}
		}
		if (g_monitor_perf_cores) {
			core_busy_tsc += busy_tsc;
			core_idle_tsc += idle_tsc;
		}
	}
	mb_this_second = (double)io_this_second * g_io_size_bytes / (1024 * 1024);

	printf("%s%9ju IOPS, %8.2f MiB/s", warmup ? "[warmup] " : "", io_this_second, mb_this_second);
	if (g_monitor_perf_cores) {
		core_busy_perc = (double)core_busy_tsc / (core_idle_tsc + core_busy_tsc) * 100;
		printf("%3d Core(s): %6.2f%% Busy", g_num_workers, core_busy_perc);
	}
	printf("\r");
	fflush(stdout);
}

static void
perf_dump_transport_statistics(struct worker_thread *worker)
{
	struct ns_worker_ctx *ns_ctx;

	TAILQ_FOREACH(ns_ctx, &worker->ns_ctx, link) {
		if (ns_ctx->entry->fn_table->dump_transport_stats) {
			ns_ctx->entry->fn_table->dump_transport_stats(worker->lcore, ns_ctx);
		}
	}
}

static int
work_fn(void *arg)
{
	uint64_t tsc_start, tsc_end, tsc_current, tsc_next_print;
	struct worker_thread *worker = (struct worker_thread *) arg;
	struct ns_worker_ctx *ns_ctx = NULL;
	uint32_t unfinished_ns_ctx;
	bool warmup = false;
	int rc;
	int64_t check_rc;
	uint64_t check_now;
	TAILQ_HEAD(, perf_task)	swap;
	struct perf_task *task;

	/* Allocate queue pairs for each namespace. */
	TAILQ_FOREACH(ns_ctx, &worker->ns_ctx, link) {
		if (init_ns_worker_ctx(ns_ctx) != 0) {
			printf("ERROR: init_ns_worker_ctx() failed\n");
			/* Wait on barrier to avoid blocking of successful workers */
			pthread_barrier_wait(&g_worker_sync_barrier);
			ns_ctx->status = 1;
			return 1;
		}
	}

	rc = pthread_barrier_wait(&g_worker_sync_barrier);
	if (rc != 0 && rc != PTHREAD_BARRIER_SERIAL_THREAD) {
		printf("ERROR: failed to wait on thread sync barrier\n");
		ns_ctx->status = 1;
		return 1;
	}

	tsc_start = spdk_get_ticks();
	tsc_current = tsc_start;
	tsc_next_print = tsc_current + g_tsc_rate;

	if (g_warmup_time_in_sec) {
		warmup = true;
		tsc_end = tsc_current + g_warmup_time_in_sec * g_tsc_rate;
	} else {
		tsc_end = tsc_current + g_time_in_sec * g_tsc_rate;
	}

	/* Submit initial I/O for each namespace. */
	TAILQ_FOREACH(ns_ctx, &worker->ns_ctx, link) {
		submit_io(ns_ctx, g_queue_depth);
	}

	while (spdk_likely(!g_exit)) {
		bool all_draining = true;

		/*
		 * Check for completed I/O for each controller. A new
		 * I/O will be submitted in the io_complete callback
		 * to replace each I/O that is completed.
		 */
		TAILQ_FOREACH(ns_ctx, &worker->ns_ctx, link) {
			if (g_continue_on_error && !ns_ctx->is_draining) {
				/* Submit any I/O that is queued up */
				TAILQ_INIT(&swap);
				TAILQ_SWAP(&swap, &ns_ctx->queued_tasks, perf_task, link);
				while (!TAILQ_EMPTY(&swap)) {
					task = TAILQ_FIRST(&swap);
					TAILQ_REMOVE(&swap, task, link);
					if (ns_ctx->is_draining) {
						TAILQ_INSERT_TAIL(&ns_ctx->queued_tasks,
								  task, link);
						continue;
					}
					submit_single_io(task);
				}
			}

			check_now = spdk_get_ticks();
			check_rc = ns_ctx->entry->fn_table->check_io(ns_ctx);

			if (check_rc > 0) {
				ns_ctx->stats.busy_tsc += check_now - ns_ctx->stats.last_tsc;
			} else {
				ns_ctx->stats.idle_tsc += check_now - ns_ctx->stats.last_tsc;
			}
			ns_ctx->stats.last_tsc = check_now;

			if (!ns_ctx->is_draining) {
				all_draining = false;
			}
		}

		if (spdk_unlikely(all_draining)) {
			break;
		}

		tsc_current = spdk_get_ticks();

		if (worker->lcore == g_main_core && tsc_current > tsc_next_print) {
			tsc_next_print += g_tsc_rate;
			print_periodic_performance(warmup);
		}

		if (tsc_current > tsc_end) {
			if (warmup) {
				/* Update test start and end time, clear statistics */
				tsc_start = spdk_get_ticks();
				tsc_end = tsc_start + g_time_in_sec * g_tsc_rate;

				TAILQ_FOREACH(ns_ctx, &worker->ns_ctx, link) {
					memset(&ns_ctx->stats, 0, sizeof(ns_ctx->stats));
					ns_ctx->stats.min_tsc = UINT64_MAX;
					spdk_histogram_data_reset(ns_ctx->histogram);
				}

				if (worker->lcore == g_main_core && isatty(STDOUT_FILENO)) {
					/* warmup stage prints a longer string to stdout, need to erase it */
					printf("%c[2K", 27);
				}

				warmup = false;
			} else {
				break;
			}
		}
	}

	/* Capture the actual elapsed time when we break out of the main loop. This will account
	 * for cases where we exit prematurely due to a signal. We only need to capture it on
	 * one core, so use the main core.
	 */
	if (worker->lcore == g_main_core) {
		g_elapsed_time_in_usec = (tsc_current - tsc_start) * SPDK_SEC_TO_USEC / g_tsc_rate;
	}

	/* drain the io of each ns_ctx in round robin to make the fairness */
	do {
		unfinished_ns_ctx = 0;
		TAILQ_FOREACH(ns_ctx, &worker->ns_ctx, link) {
			/* first time will enter into this if case */
			if (!ns_ctx->is_draining) {
				ns_ctx->is_draining = true;
			}

			if (ns_ctx->current_queue_depth > 0) {
				ns_ctx->entry->fn_table->check_io(ns_ctx);
				if (ns_ctx->current_queue_depth > 0) {
					unfinished_ns_ctx++;
				}
			}
		}
	} while (unfinished_ns_ctx > 0);

	if (g_dump_transport_stats) {
		pthread_mutex_lock(&g_stats_mutex);
		perf_dump_transport_statistics(worker);
		pthread_mutex_unlock(&g_stats_mutex);
	}

	TAILQ_FOREACH(ns_ctx, &worker->ns_ctx, link) {
		cleanup_ns_worker_ctx(ns_ctx);
	}

	return 0;
}

static void
usage(char *program_name)
{
	printf("%s options", program_name);
#if defined(SPDK_CONFIG_URING) || defined(HAVE_LIBAIO)
	printf(" [Kernel device(s)]...");
#endif
	printf("\n\n");
	printf("==== BASIC OPTIONS ====\n\n");
	printf("\t-q, --io-depth <val> io depth\n");
	printf("\t-o, --io-size <val> io size in bytes\n");
	printf("\t-w, --io-pattern <pattern> io pattern type, must be one of\n");
	printf("\t\t(read, write, randread, randwrite, rw, randrw)\n");
	printf("\t-M, --rwmixread <0-100> rwmixread (100 for reads, 0 for writes)\n");
	printf("\t-t, --time <sec> time in seconds\n");
	printf("\t-a, --warmup-time <sec> warmup time in seconds\n");
	printf("\t-c, --core-mask <mask> core mask for I/O submission/completion.\n");
	printf("\t\t(default: 1)\n");
	spdk_nvme_transport_id_usage(stdout,
				     SPDK_NVME_TRID_USAGE_OPT_LONGOPT | SPDK_NVME_TRID_USAGE_OPT_MULTI | SPDK_NVME_TRID_USAGE_OPT_NS |
				     SPDK_NVME_TRID_USAGE_OPT_HOSTNQN);
	printf("\n");

	printf("==== ADVANCED OPTIONS ====\n\n");
	printf("\t--use-every-core for each namespace, I/Os are submitted from all cores\n");
	printf("\t--io-queue-size <val> size of NVMe IO queue. Default: maximum allowed by controller\n");
	printf("\t-O, --io-unit-size io unit size in bytes (4-byte aligned) for SPDK driver. default: same as io size\n");
	printf("\t-P, --num-qpairs <val> number of io queues per namespace. default: 1\n");
	printf("\t-U, --num-unused-qpairs <val> number of unused io queues per controller. default: 0\n");
	printf("\t-A, --buffer-alignment IO buffer alignment. Must be power of 2 and not less than cache line (%u)\n",
	       SPDK_CACHE_LINE_SIZE);
	printf("\t-s, --hugemem-size <MB> DPDK huge memory size in MB.\n");
	printf("\t-g, --mem-single-seg use single file descriptor for DPDK memory segments\n");
	printf("\t-C, --max-completion-per-poll <val> max completions per poll\n");
	printf("\t\t(default: 0 - unlimited)\n");
	printf("\t-i, --shmem-grp-id <id> shared memory group ID\n");
	printf("\t-d, --number-ios <val> number of I/O to perform per thread on each namespace. Note: this is additional exit criteria.\n");
	printf("\t\t(default: 0 - unlimited)\n");
	printf("\t-e, --metadata <fmt> metadata configuration\n");
	printf("\t\t Keys:\n");
	printf("\t\t  PRACT      Protection Information Action bit (PRACT=1 or PRACT=0)\n");
	printf("\t\t  PRCHK      Control of Protection Information Checking (PRCHK=GUARD|REFTAG|APPTAG)\n");
	printf("\t\t Example: -e 'PRACT=0,PRCHK=GUARD|REFTAG|APPTAG'\n");
	printf("\t\t          -e 'PRACT=1,PRCHK=GUARD'\n");
	printf("\t-F, --zipf <theta> use zipf distribution for random I/O\n");
#ifdef SPDK_CONFIG_URING
	printf("\t-R, --enable-uring enable using liburing to drive kernel devices (Default: libaio)\n");
#endif
	printf("\t--iova-mode <mode> specify DPDK IOVA mode: va|pa\n");
	printf("\t--no-huge, SPDK is run without hugepages\n");
	printf("\n");

	printf("==== PCIe OPTIONS ====\n\n");
	printf("\t-b, --allowed-pci-addr <addr> allowed local PCIe device address\n");
	printf("\t\t Example: -b 0000:d8:00.0 -b 0000:d9:00.0\n");
	printf("\t-V, --enable-vmd enable VMD enumeration\n");
	printf("\t-D, --disable-sq-cmb disable submission queue in controller memory buffer, default: enabled\n");
	printf("\t-E, --enable-interrupt enable interrupts on completion queue, default: disabled\n");
	printf("\n");

	printf("==== TCP OPTIONS ====\n\n");
	printf("\t-S, --default-sock-impl <impl> set the default sock impl, e.g. \"posix\"\n");
	printf("\t--disable-ktls disable Kernel TLS. Only valid for ssl impl. Default for ssl impl\n");
	printf("\t--enable-ktls enable Kernel TLS. Only valid for ssl impl\n");
	printf("\t--tls-version <val> TLS version to use. Only valid for ssl impl. Default: 0 (auto-negotiation)\n");
	printf("\t--psk-path <val> Path to PSK file (only applies when sock_impl == ssl)\n");
	printf("\t--psk-identity <val> Default PSK ID, e.g. psk.spdk.io (only applies when sock_impl == ssl)\n");
	printf("\t--dhchap-key <val> Path to DH-HMAC-CHAP key file (required if controller key is specified)\n");
	printf("\t--dhchap-ctrlr-key <val> Path to DH-HMAC-CHAP controller key file\n");
	printf("\t--zerocopy-threshold <val> data is sent with MSG_ZEROCOPY if size is greater than this val. Default: 0 to disable it\n");
	printf("\t--zerocopy-threshold-sock-impl <impl> specify the sock implementation to set zerocopy_threshold\n");
	printf("\t-z, --disable-zcopy <impl> disable zero copy send for the given sock implementation. Default for posix impl\n");
	printf("\t-Z, --enable-zcopy <impl> enable zero copy send for the given sock implementation\n");
	printf("\t-k, --keepalive <ms> keep alive timeout period in millisecond\n");
	printf("\t-H, --enable-tcp-hdgst enable header digest for TCP transport, default: disabled\n");
	printf("\t-I, --enable-tcp-ddgst enable data digest for TCP transport, default: disabled\n");
	printf("\n");

	printf("==== RDMA OPTIONS ====\n\n");
	printf("\t--transport-tos <val> specify the type of service for RDMA transport. Default: 0 (disabled)\n");
	printf("\t--rdma-srq-size <val> The size of a shared rdma receive queue. Default: 0 (disabled)\n");
	printf("\t-k, --keepalive <ms> keep alive timeout period in millisecond\n");
	printf("\n");

	printf("==== LOGGING ====\n\n");
	printf("\t-L, --enable-sw-latency-tracking enable latency tracking via sw, default: disabled\n");
	printf("\t\t-L for latency summary, -LL for detailed histogram\n");
	printf("\t-l, --enable-ssd-latency-tracking enable latency tracking via ssd (if supported), default: disabled\n");
	printf("\t-N, --no-shst-notification no shutdown notification process for controllers, default: disabled\n");
	printf("\t-Q, --continue-on-error <val> Do not stop on error. Log I/O errors every N times (default: 1)\n");
	spdk_log_usage(stdout, "\t-T");
	printf("\t-m, --cpu-usage display real-time overall cpu usage on used cores\n");
#ifdef DEBUG
	printf("\t-G, --enable-debug enable debug logging\n");
#else
	printf("\t-G, --enable-debug enable debug logging (flag disabled, must reconfigure with --enable-debug)\n");
#endif
	printf("\t--transport-stats dump transport statistics\n");
	printf("\n\n");
}

static void
check_cutoff(void *ctx, uint64_t start, uint64_t end, uint64_t count,
	     uint64_t total, uint64_t so_far)
{
	double so_far_pct;
	double **cutoff = ctx;

	if (count == 0) {
		return;
	}

	so_far_pct = (double)so_far / total;
	while (so_far_pct >= **cutoff && **cutoff > 0) {
		printf("%9.5f%% : %9.3fus\n", **cutoff * 100, (double)end * 1000 * 1000 / g_tsc_rate);
		(*cutoff)++;
	}
}

static void
print_bucket(void *ctx, uint64_t start, uint64_t end, uint64_t count,
	     uint64_t total, uint64_t so_far)
{
	double so_far_pct;

	if (count == 0) {
		return;
	}

	so_far_pct = (double)so_far * 100 / total;
	printf("%9.3f - %9.3f: %9.4f%%  (%9ju)\n",
	       (double)start * 1000 * 1000 / g_tsc_rate,
	       (double)end * 1000 * 1000 / g_tsc_rate,
	       so_far_pct, count);
}

static void
print_performance(void)
{
	uint64_t total_io_completed, total_io_tsc;
	double io_per_second, mb_per_second, average_latency, min_latency, max_latency;
	double sum_ave_latency, min_latency_so_far, max_latency_so_far;
	double total_io_per_second, total_mb_per_second;
	int ns_count;
	struct worker_thread	*worker;
	struct ns_worker_ctx	*ns_ctx;
	uint32_t max_strlen;

	total_io_per_second = 0;
	total_mb_per_second = 0;
	total_io_completed = 0;
	total_io_tsc = 0;
	min_latency_so_far = (double)UINT64_MAX;
	max_latency_so_far = 0;
	ns_count = 0;

	max_strlen = 0;
	TAILQ_FOREACH(worker, &g_workers, link) {
		TAILQ_FOREACH(ns_ctx, &worker->ns_ctx, link) {
			max_strlen = spdk_max(strlen(ns_ctx->entry->name), max_strlen);
		}
	}

	printf("========================================================\n");
	printf("%*s\n", max_strlen + 60, "Latency(us)");
	printf("%-*s: %10s %10s %10s %10s %10s\n",
	       max_strlen + 13, "Device Information", "IOPS", "MiB/s", "Average", "min", "max");

	TAILQ_FOREACH(worker, &g_workers, link) {
		TAILQ_FOREACH(ns_ctx, &worker->ns_ctx, link) {
			if (ns_ctx->stats.io_completed != 0) {
				io_per_second = (double)ns_ctx->stats.io_completed * 1000 * 1000 / g_elapsed_time_in_usec;
				mb_per_second = io_per_second * g_io_size_bytes / (1024 * 1024);
				average_latency = ((double)ns_ctx->stats.total_tsc / ns_ctx->stats.io_completed) * 1000 * 1000 /
						  g_tsc_rate;
				min_latency = (double)ns_ctx->stats.min_tsc * 1000 * 1000 / g_tsc_rate;
				if (min_latency < min_latency_so_far) {
					min_latency_so_far = min_latency;
				}

				max_latency = (double)ns_ctx->stats.max_tsc * 1000 * 1000 / g_tsc_rate;
				if (max_latency > max_latency_so_far) {
					max_latency_so_far = max_latency;
				}

				printf("%-*.*s from core %2u: %10.2f %10.2f %10.2f %10.2f %10.2f\n",
				       max_strlen, max_strlen, ns_ctx->entry->name, worker->lcore,
				       io_per_second, mb_per_second,
				       average_latency, min_latency, max_latency);
				total_io_per_second += io_per_second;
				total_mb_per_second += mb_per_second;
				total_io_completed += ns_ctx->stats.io_completed;
				total_io_tsc += ns_ctx->stats.total_tsc;
				ns_count++;
			}
		}
	}

	if (ns_count != 0 && total_io_completed) {
		sum_ave_latency = ((double)total_io_tsc / total_io_completed) * 1000 * 1000 / g_tsc_rate;
		printf("========================================================\n");
		printf("%-*s: %10.2f %10.2f %10.2f %10.2f %10.2f\n",
		       max_strlen + 13, "Total", total_io_per_second, total_mb_per_second,
		       sum_ave_latency, min_latency_so_far, max_latency_so_far);
		printf("\n");
	}

	if (g_latency_sw_tracking_level == 0 || total_io_completed == 0) {
		return;
	}

	TAILQ_FOREACH(worker, &g_workers, link) {
		TAILQ_FOREACH(ns_ctx, &worker->ns_ctx, link) {
			const double *cutoff = g_latency_cutoffs;

			printf("Summary latency data for %-43.43s from core %u:\n", ns_ctx->entry->name, worker->lcore);
			printf("=================================================================================\n");

			spdk_histogram_data_iterate(ns_ctx->histogram, check_cutoff, &cutoff);

			printf("\n");
		}
	}

	if (g_latency_sw_tracking_level == 1) {
		return;
	}

	TAILQ_FOREACH(worker, &g_workers, link) {
		TAILQ_FOREACH(ns_ctx, &worker->ns_ctx, link) {
			printf("Latency histogram for %-43.43s from core %u:\n", ns_ctx->entry->name, worker->lcore);
			printf("==============================================================================\n");
			printf("       Range in us     Cumulative    IO count\n");

			spdk_histogram_data_iterate(ns_ctx->histogram, print_bucket, NULL);
			printf("\n");
		}
	}

}

static void
print_latency_page(struct ctrlr_entry *entry)
{
	int i;

	printf("\n");
	printf("%s\n", entry->name);
	printf("--------------------------------------------------------\n");

	for (i = 0; i < 32; i++) {
		if (entry->latency_page->buckets_32us[i]) {
			printf("Bucket %dus - %dus: %d\n", i * 32, (i + 1) * 32, entry->latency_page->buckets_32us[i]);
		}
	}
	for (i = 0; i < 31; i++) {
		if (entry->latency_page->buckets_1ms[i]) {
			printf("Bucket %dms - %dms: %d\n", i + 1, i + 2, entry->latency_page->buckets_1ms[i]);
		}
	}
	for (i = 0; i < 31; i++) {
		if (entry->latency_page->buckets_32ms[i])
			printf("Bucket %dms - %dms: %d\n", (i + 1) * 32, (i + 2) * 32,
			       entry->latency_page->buckets_32ms[i]);
	}
}

static void
print_latency_statistics(const char *op_name, enum spdk_nvme_intel_log_page log_page)
{
	struct ctrlr_entry	*ctrlr;

	printf("%s Latency Statistics:\n", op_name);
	printf("========================================================\n");
	TAILQ_FOREACH(ctrlr, &g_controllers, link) {
		if (spdk_nvme_ctrlr_is_log_page_supported(ctrlr->ctrlr, log_page)) {
			if (spdk_nvme_ctrlr_cmd_get_log_page(ctrlr->ctrlr, log_page, SPDK_NVME_GLOBAL_NS_TAG,
							     ctrlr->latency_page, sizeof(struct spdk_nvme_intel_rw_latency_page), 0,
							     enable_latency_tracking_complete,
							     NULL)) {
				printf("nvme_ctrlr_cmd_get_log_page() failed\n");
				exit(1);
			}

			g_outstanding_commands++;
		} else {
			printf("Controller %s: %s latency statistics not supported\n", ctrlr->name, op_name);
		}
	}

	while (g_outstanding_commands) {
		TAILQ_FOREACH(ctrlr, &g_controllers, link) {
			spdk_nvme_ctrlr_process_admin_completions(ctrlr->ctrlr);
		}
	}

	TAILQ_FOREACH(ctrlr, &g_controllers, link) {
		if (spdk_nvme_ctrlr_is_log_page_supported(ctrlr->ctrlr, log_page)) {
			print_latency_page(ctrlr);
		}
	}
	printf("\n");
}

static void
print_stats(void)
{
	print_performance();
	if (g_latency_ssd_tracking_enable) {
		if (g_rw_percentage != 0) {
			print_latency_statistics("Read", SPDK_NVME_INTEL_LOG_READ_CMD_LATENCY);
		}
		if (g_rw_percentage != 100) {
			print_latency_statistics("Write", SPDK_NVME_INTEL_LOG_WRITE_CMD_LATENCY);
		}
	}
}

static int
add_allowed_pci_device(const char *bdf_str, struct spdk_env_opts *env_opts)
{
	int rc;

	if (env_opts->num_pci_addr >= MAX_ALLOWED_PCI_DEVICE_NUM) {
		fprintf(stderr, "Currently we only support allowed PCI device num=%d\n",
			MAX_ALLOWED_PCI_DEVICE_NUM);
		return -1;
	}

	rc = spdk_pci_addr_parse(&env_opts->pci_allowed[env_opts->num_pci_addr], bdf_str);
	if (rc < 0) {
		fprintf(stderr, "Failed to parse the given bdf_str=%s\n", bdf_str);
		return -1;
	}

	env_opts->num_pci_addr++;
	return 0;
}

static size_t
parse_next_key(const char **str, char *key, char *val, size_t key_buf_size,
	       size_t val_buf_size)
{
	const char *sep;
	const char *separator = ", \t\n";
	size_t key_len, val_len;

	*str += strspn(*str, separator);

	sep = strchr(*str, '=');
	if (!sep) {
		fprintf(stderr, "Key without '=' separator\n");
		return 0;
	}

	key_len = sep - *str;
	if (key_len >= key_buf_size) {
		fprintf(stderr, "Key length %zu is greater than maximum allowed %zu\n",
			key_len, key_buf_size - 1);
		return 0;
	}

	memcpy(key, *str, key_len);
	key[key_len] = '\0';

	*str += key_len + 1;	/* Skip key */
	val_len = strcspn(*str, separator);
	if (val_len == 0) {
		fprintf(stderr, "Key without value\n");
		return 0;
	}

	if (val_len >= val_buf_size) {
		fprintf(stderr, "Value length %zu is greater than maximum allowed %zu\n",
			val_len, val_buf_size - 1);
		return 0;
	}

	memcpy(val, *str, val_len);
	val[val_len] = '\0';

	*str += val_len;

	return val_len;
}

static int
parse_metadata(const char *metacfg_str)
{
	const char *str;
	size_t val_len;
	char key[32];
	char val[1024];

	if (metacfg_str == NULL) {
		return -EINVAL;
	}

	str = metacfg_str;

	while (*str != '\0') {
		val_len = parse_next_key(&str, key, val, sizeof(key), sizeof(val));
		if (val_len == 0) {
			fprintf(stderr, "Failed to parse metadata\n");
			return -EINVAL;
		}

		if (strcmp(key, "PRACT") == 0) {
			if (*val == '1') {
				g_metacfg_pract_flag = SPDK_NVME_IO_FLAGS_PRACT;
			}
		} else if (strcmp(key, "PRCHK") == 0) {
			if (strstr(val, "GUARD") != NULL) {
				g_metacfg_prchk_flags |= SPDK_NVME_IO_FLAGS_PRCHK_GUARD;
			}
			if (strstr(val, "REFTAG") != NULL) {
				g_metacfg_prchk_flags |= SPDK_NVME_IO_FLAGS_PRCHK_REFTAG;
			}
			if (strstr(val, "APPTAG") != NULL) {
				g_metacfg_prchk_flags |= SPDK_NVME_IO_FLAGS_PRCHK_APPTAG;
			}
		} else {
			fprintf(stderr, "Unknown key '%s'\n", key);
		}
	}

	return 0;
}

static void
free_key(struct spdk_key **key)
{
	if (*key == NULL) {
		return;
	}

	spdk_keyring_put_key(*key);
	spdk_keyring_file_remove_key(spdk_key_get_name(*key));
	*key = NULL;
}

static struct spdk_key *
alloc_key(const char *name, const char *path)
{
	struct spdk_key *key;
	int rc;

	rc = spdk_keyring_file_add_key(name, path);
	if (rc != 0) {
		return NULL;
	}

	key = spdk_keyring_get_key(name);
	if (key == NULL) {
		return NULL;
	}

	return key;
}

#define PERF_GETOPT_SHORT "a:b:c:d:e:ghi:lmo:q:r:k:s:t:w:z:A:C:DEF:GHILM:NO:P:Q:RS:T:U:VZ:"

static const struct option g_perf_cmdline_opts[] = {
#define PERF_WARMUP_TIME	'a'
	{"warmup-time",			required_argument,	NULL, PERF_WARMUP_TIME},
#define PERF_ALLOWED_PCI_ADDR	'b'
	{"allowed-pci-addr",			required_argument,	NULL, PERF_ALLOWED_PCI_ADDR},
#define PERF_CORE_MASK	'c'
	{"core-mask",			required_argument,	NULL, PERF_CORE_MASK},
#define PERF_METADATA	'e'
	{"metadata",			required_argument,	NULL, PERF_METADATA},
#define PERF_MEM_SINGL_SEG	'g'
	{"mem-single-seg", no_argument, NULL, PERF_MEM_SINGL_SEG},
#define PERF_HELP		'h'
	{"help", no_argument, NULL, PERF_HELP},
#define PERF_SHMEM_GROUP_ID	'i'
	{"shmem-grp-id",			required_argument,	NULL, PERF_SHMEM_GROUP_ID},
#define PERF_ENABLE_SSD_LATENCY_TRACING	'l'
	{"enable-ssd-latency-tracking", no_argument, NULL, PERF_ENABLE_SSD_LATENCY_TRACING},
#define PERF_CPU_USAGE	'm'
	{"cpu-usage", no_argument, NULL, PERF_CPU_USAGE},
#define PERF_IO_SIZE	'o'
	{"io-size",			required_argument,	NULL, PERF_IO_SIZE},
#define PERF_IO_DEPTH	'q'
	{"io-depth",			required_argument,	NULL, PERF_IO_DEPTH},
#define PERF_TRANSPORT	'r'
	{"transport",			required_argument,	NULL, PERF_TRANSPORT},
#define PERF_KEEPALIVE	'k'
	{"keepalive",			required_argument,	NULL, PERF_KEEPALIVE},
#define PERF_HUGEMEM_SIZE	's'
	{"hugemem-size",			required_argument,	NULL, PERF_HUGEMEM_SIZE},
#define PERF_TIME	't'
	{"time",			required_argument,	NULL, PERF_TIME},
#define PERF_NUMBER_IOS	'd'
	{"number-ios",			required_argument,	NULL, PERF_NUMBER_IOS},
#define PERF_IO_PATTERN	'w'
	{"io-pattern",			required_argument,	NULL, PERF_IO_PATTERN},
#define PERF_DISABLE_ZCOPY	'z'
	{"disable-zcopy",			required_argument,	NULL, PERF_DISABLE_ZCOPY},
#define PERF_BUFFER_ALIGNMENT	'A'
	{"buffer-alignment",			required_argument,	NULL, PERF_BUFFER_ALIGNMENT},
#define PERF_MAX_COMPLETIONS_PER_POLL	'C'
	{"max-completion-per-poll",			required_argument,	NULL, PERF_MAX_COMPLETIONS_PER_POLL},
#define PERF_DISABLE_SQ_CMB	'D'
	{"disable-sq-cmb",			no_argument,	NULL, PERF_DISABLE_SQ_CMB},
#define PERF_ENABLE_INTERRUPT	'E'
	{"enable-interrupt",			no_argument,	NULL, PERF_ENABLE_INTERRUPT},
#define PERF_ZIPF		'F'
	{"zipf",				required_argument,	NULL, PERF_ZIPF},
#define PERF_ENABLE_DEBUG	'G'
	{"enable-debug",			no_argument,	NULL, PERF_ENABLE_DEBUG},
#define PERF_ENABLE_TCP_HDGST	'H'
	{"enable-tcp-hdgst",			no_argument,	NULL, PERF_ENABLE_TCP_HDGST},
#define PERF_ENABLE_TCP_DDGST	'I'
	{"enable-tcp-ddgst",			no_argument,	NULL, PERF_ENABLE_TCP_DDGST},
#define PERF_ENABLE_SW_LATENCY_TRACING	'L'
	{"enable-sw-latency-tracking", no_argument, NULL, PERF_ENABLE_SW_LATENCY_TRACING},
#define PERF_RW_MIXREAD	'M'
	{"rwmixread", required_argument, NULL, PERF_RW_MIXREAD},
#define PERF_NO_SHST_NOTIFICATION	'N'
	{"no-shst-notification", no_argument, NULL, PERF_NO_SHST_NOTIFICATION},
#define PERF_IO_UNIT_SIZE	'O'
	{"io-unit-size",			required_argument,	NULL, PERF_IO_UNIT_SIZE},
#define PERF_IO_QUEUES_PER_NS	'P'
	{"num-qpairs", required_argument, NULL, PERF_IO_QUEUES_PER_NS},
#define PERF_CONTINUE_ON_ERROR	'Q'
	{"continue-on-error",			required_argument,	NULL, PERF_CONTINUE_ON_ERROR},
#define PERF_ENABLE_URING	'R'
	{"enable-uring", no_argument, NULL, PERF_ENABLE_URING},
#define PERF_DEFAULT_SOCK_IMPL	'S'
	{"default-sock-impl", required_argument, NULL, PERF_DEFAULT_SOCK_IMPL},
#define PERF_LOG_FLAG	'T'
	{"logflag", required_argument, NULL, PERF_LOG_FLAG},
#define PERF_NUM_UNUSED_IO_QPAIRS	'U'
	{"num-unused-qpairs", required_argument, NULL, PERF_NUM_UNUSED_IO_QPAIRS},
#define PERF_ENABLE_VMD	'V'
	{"enable-vmd", no_argument, NULL, PERF_ENABLE_VMD},
#define PERF_ENABLE_ZCOPY	'Z'
	{"enable-zcopy",			required_argument,	NULL, PERF_ENABLE_ZCOPY},
#define PERF_TRANSPORT_STATISTICS	257
	{"transport-stats", no_argument, NULL, PERF_TRANSPORT_STATISTICS},
#define PERF_IOVA_MODE		258
	{"iova-mode", required_argument, NULL, PERF_IOVA_MODE},
#define PERF_IO_QUEUE_SIZE	259
	{"io-queue-size", required_argument, NULL, PERF_IO_QUEUE_SIZE},
#define PERF_DISABLE_KTLS	260
	{"disable-ktls", no_argument, NULL, PERF_DISABLE_KTLS},
#define PERF_ENABLE_KTLS	261
	{"enable-ktls", no_argument, NULL, PERF_ENABLE_KTLS},
#define PERF_TLS_VERSION	262
	{"tls-version", required_argument, NULL, PERF_TLS_VERSION},
#define PERF_PSK_PATH		263
	{"psk-path", required_argument, NULL, PERF_PSK_PATH},
#define PERF_PSK_IDENTITY	264
	{"psk-identity ", required_argument, NULL, PERF_PSK_IDENTITY},
#define PERF_ZEROCOPY_THRESHOLD		265
	{"zerocopy-threshold", required_argument, NULL, PERF_ZEROCOPY_THRESHOLD},
#define PERF_SOCK_IMPL		266
	{"zerocopy-threshold-sock-impl", required_argument, NULL, PERF_SOCK_IMPL},
#define PERF_TRANSPORT_TOS		267
	{"transport-tos", required_argument, NULL, PERF_TRANSPORT_TOS},
#define PERF_RDMA_SRQ_SIZE	268
	{"rdma-srq-size", required_argument, NULL, PERF_RDMA_SRQ_SIZE},
#define PERF_USE_EVERY_CORE	269
	{"use-every-core", no_argument, NULL, PERF_USE_EVERY_CORE},
#define PERF_NO_HUGE		270
	{"no-huge", no_argument, NULL, PERF_NO_HUGE},
#define PERF_DHCHAP_PATH		271
	{"dhchap-key", required_argument, NULL, PERF_DHCHAP_PATH},
#define PERF_DHCHAP_CTRLR_PATH		272
	{"dhchap-ctrlr-key", required_argument, NULL, PERF_DHCHAP_CTRLR_PATH},
	/* Should be the last element */
	{0, 0, 0, 0}
};

static int
parse_args(int argc, char **argv, struct spdk_env_opts *env_opts)
{
	int op, long_idx;
	long int val;
	uint64_t val_u64;
	int rc;
	char *endptr;
	bool ssl_used = false;
	char *sock_impl = "posix";
	uint32_t trid_count = 0;

	while ((op = getopt_long(argc, argv, PERF_GETOPT_SHORT, g_perf_cmdline_opts, &long_idx)) != -1) {
		switch (op) {
		case PERF_WARMUP_TIME:
		case PERF_SHMEM_GROUP_ID:
		case PERF_MAX_COMPLETIONS_PER_POLL:
		case PERF_IO_QUEUES_PER_NS:
		case PERF_KEEPALIVE:
		case PERF_TIME:
		case PERF_RW_MIXREAD:
		case PERF_NUM_UNUSED_IO_QPAIRS:
		case PERF_CONTINUE_ON_ERROR:
		case PERF_RDMA_SRQ_SIZE:
			val = spdk_strtol(optarg, 10);
			if (val < 0) {
				fprintf(stderr, "Converting a string to integer failed\n");
				return val;
			}
			switch (op) {
			case PERF_WARMUP_TIME:
				g_warmup_time_in_sec = val;
				break;
			case PERF_SHMEM_GROUP_ID:
				env_opts->shm_id = val;
				break;
			case PERF_MAX_COMPLETIONS_PER_POLL:
				g_max_completions = val;
				break;
			case PERF_IO_QUEUES_PER_NS:
				g_nr_io_queues_per_ns = val;
				break;
			case PERF_KEEPALIVE:
				g_keep_alive_timeout_in_ms = val;
				break;
			case PERF_TIME:
				g_time_in_sec = val;
				break;
			case PERF_RW_MIXREAD:
				g_rw_percentage = val;
				g_mix_specified = true;
				break;
			case PERF_CONTINUE_ON_ERROR:
				g_quiet_count = val;
				g_continue_on_error = true;
				break;
			case PERF_NUM_UNUSED_IO_QPAIRS:
				g_nr_unused_io_queues = val;
				break;
			case PERF_RDMA_SRQ_SIZE:
				g_rdma_srq_size = val;
				break;
			}
			break;
		case PERF_IO_SIZE:
		case PERF_IO_UNIT_SIZE:
		case PERF_ZEROCOPY_THRESHOLD:
		case PERF_BUFFER_ALIGNMENT:
		case PERF_HUGEMEM_SIZE:
		case PERF_NUMBER_IOS:
		case PERF_IO_DEPTH:
		case PERF_IO_QUEUE_SIZE:
			rc = spdk_parse_capacity(optarg, &val_u64, NULL);
			if (rc != 0) {
				fprintf(stderr, "Converting a string to integer failed\n");
				return 1;
			}
			switch (op) {
			case PERF_IO_SIZE:
				g_io_size_bytes = (uint32_t)val_u64;
				break;
			case PERF_IO_UNIT_SIZE:
				g_io_unit_size = (uint32_t)val_u64;
				break;
			case PERF_ZEROCOPY_THRESHOLD:
				g_sock_zcopy_threshold = (uint32_t)val_u64;
				break;
			case PERF_IO_DEPTH:
				g_queue_depth = (uint32_t)val_u64;
				break;
			case PERF_IO_QUEUE_SIZE:
				g_io_queue_size = (uint32_t)val_u64;
				break;
			case PERF_BUFFER_ALIGNMENT:
				g_io_align = (uint32_t)val_u64;
				if (!spdk_u32_is_pow2(g_io_align) || g_io_align < SPDK_CACHE_LINE_SIZE) {
					fprintf(stderr, "Wrong alignment %u. Must be power of 2 and not less than cache lize (%u)\n",
						g_io_align, SPDK_CACHE_LINE_SIZE);
					usage(argv[0]);
					return 1;
				}
				g_io_align_specified = true;
				break;
			case PERF_HUGEMEM_SIZE:
				env_opts->mem_size = (int)val_u64;
				break;
			case PERF_NUMBER_IOS:
				g_number_ios = val_u64;
				break;
			}
			break;
		case PERF_ZIPF:
			errno = 0;
			g_zipf_theta = strtod(optarg, &endptr);
			if (errno || optarg == endptr || g_zipf_theta < 0) {
				fprintf(stderr, "Illegal zipf theta value %s\n", optarg);
				return 1;
			}
			break;
		case PERF_ALLOWED_PCI_ADDR:
			if (add_allowed_pci_device(optarg, env_opts)) {
				usage(argv[0]);
				return 1;
			}
			break;
		case PERF_CORE_MASK:
			env_opts->core_mask = optarg;
			break;
		case PERF_METADATA:
			if (parse_metadata(optarg)) {
				usage(argv[0]);
				return 1;
			}
			break;
		case PERF_MEM_SINGL_SEG:
			env_opts->hugepage_single_segments = true;
			break;
		case PERF_ENABLE_SSD_LATENCY_TRACING:
			g_latency_ssd_tracking_enable = true;
			break;
		case PERF_CPU_USAGE:
			g_monitor_perf_cores = true;
			break;
		case PERF_TRANSPORT:
			if (trid_count == MAX_TRID_ENTRY) {
				fprintf(stderr, "Number of Transport ID specified with -r is limited to %u\n", MAX_TRID_ENTRY);
				return 1;
			}

			rc = spdk_nvme_trid_entry_parse(&g_trids[trid_count].entry, optarg);
			if (rc < 0) {
				usage(argv[0]);
				return 1;
			}

			TAILQ_INSERT_TAIL(&g_trid_list, &g_trids[trid_count], tailq);
			++trid_count;
			break;
		case PERF_IO_PATTERN:
			g_workload_type = optarg;
			break;
		case PERF_DISABLE_SQ_CMB:
			g_disable_sq_cmb = 1;
			break;
		case PERF_ENABLE_INTERRUPT:
			g_enable_interrupt = 1;
			break;
		case PERF_ENABLE_DEBUG:
#ifndef DEBUG
			fprintf(stderr, "%s must be configured with --enable-debug for -G flag\n",
				argv[0]);
			usage(argv[0]);
			return 1;
#else
			spdk_log_set_flag("nvme");
			spdk_log_set_print_level(SPDK_LOG_DEBUG);
			break;
#endif
		case PERF_ENABLE_TCP_HDGST:
			g_header_digest = 1;
			break;
		case PERF_ENABLE_TCP_DDGST:
			g_data_digest = 1;
			break;
		case PERF_ENABLE_SW_LATENCY_TRACING:
			g_latency_sw_tracking_level++;
			break;
		case PERF_NO_SHST_NOTIFICATION:
			g_no_shn_notification = true;
			break;
		case PERF_ENABLE_URING:
#ifndef SPDK_CONFIG_URING
			fprintf(stderr, "%s must be rebuilt with CONFIG_URING=y for -R flag.\n",
				argv[0]);
			usage(argv[0]);
			return 0;
#endif
			g_use_uring = true;
			break;
		case PERF_LOG_FLAG:
			rc = spdk_log_set_flag(optarg);
			if (rc < 0) {
				fprintf(stderr, "unknown flag\n");
				usage(argv[0]);
				exit(EXIT_FAILURE);
			}
#ifdef DEBUG
			spdk_log_set_print_level(SPDK_LOG_DEBUG);
#endif
			break;
		case PERF_ENABLE_VMD:
			g_vmd = true;
			break;
		case PERF_DISABLE_KTLS:
			ssl_used = true;
			perf_set_sock_opts("ssl", "ktls", 0, NULL);
			break;
		case PERF_ENABLE_KTLS:
			ssl_used = true;
			perf_set_sock_opts("ssl", "ktls", 1, NULL);
			break;
		case PERF_TLS_VERSION:
			ssl_used = true;
			val = spdk_strtol(optarg, 10);
			if (val < 0) {
				fprintf(stderr, "Illegal tls version value %s\n", optarg);
				return val;
			}
			perf_set_sock_opts("ssl", "tls_version", val, NULL);
			break;
		case PERF_PSK_PATH:
			ssl_used = true;
			free_key(&g_psk);
			g_psk = alloc_key("perf-psk", optarg);
			if (g_psk == NULL) {
				fprintf(stderr, "Unable to set PSK at %s\n", optarg);
				return 1;
			}
			break;
		case PERF_PSK_IDENTITY:
			ssl_used = true;
			perf_set_sock_opts("ssl", "psk_identity", 0, optarg);
			break;
		case PERF_DHCHAP_PATH:
			free_key(&g_dhchap);
			g_dhchap = alloc_key("perf-dhchap", optarg);
			if (g_dhchap == NULL) {
				fprintf(stderr, "Unable to set dhchap at %s\n", optarg);
				return 1;
			}
			break;
		case PERF_DHCHAP_CTRLR_PATH:
			free_key(&g_dhchap_ctrlr);
			g_dhchap_ctrlr = alloc_key("perf-dhchap-ctrlr", optarg);
			if (g_dhchap_ctrlr == NULL) {
				fprintf(stderr, "Unable to set dhchap-ctrl at %s\n", optarg);
				return 1;
			}
			break;
		case PERF_DISABLE_ZCOPY:
			perf_set_sock_opts(optarg, "enable_zerocopy_send_client", 0, NULL);
			break;
		case PERF_ENABLE_ZCOPY:
			perf_set_sock_opts(optarg, "enable_zerocopy_send_client", 1, NULL);
			break;
		case PERF_USE_EVERY_CORE:
			g_use_every_core = true;
			break;
		case PERF_DEFAULT_SOCK_IMPL:
			sock_impl = optarg;
			rc = spdk_sock_set_default_impl(optarg);
			if (rc) {
				fprintf(stderr, "Failed to set sock impl %s, err %d (%s)\n", optarg, errno, strerror(errno));
				return 1;
			}
			break;
		case PERF_TRANSPORT_STATISTICS:
			g_dump_transport_stats = true;
			break;
		case PERF_IOVA_MODE:
			env_opts->iova_mode = optarg;
			break;
		case PERF_SOCK_IMPL:
			g_sock_threshold_impl = optarg;
			break;
		case PERF_TRANSPORT_TOS:
			val = spdk_strtol(optarg, 10);
			if (val < 0) {
				fprintf(stderr, "Invalid TOS value\n");
				return 1;
			}
			g_transport_tos = val;
			break;
		case PERF_NO_HUGE:
			env_opts->no_huge = true;
			break;
		case PERF_HELP:
			usage(argv[0]);
			return HELP_RETURN_CODE;
		default:
			usage(argv[0]);
			return 1;
		}
	}

	if (!g_nr_io_queues_per_ns) {
		usage(argv[0]);
		return 1;
	}

	if (!g_queue_depth) {
		fprintf(stderr, "missing -q (--io-depth) operand\n");
		usage(argv[0]);
		return 1;
	}
	if (!g_io_size_bytes) {
		fprintf(stderr, "missing -o (--io-size) operand\n");
		usage(argv[0]);
		return 1;
	}
	if (!g_io_unit_size || g_io_unit_size % 4) {
		fprintf(stderr, "io unit size can not be 0 or non 4-byte aligned\n");
		return 1;
	}
	if (!g_workload_type) {
		fprintf(stderr, "missing -w (--io-pattern) operand\n");
		usage(argv[0]);
		return 1;
	}
	if (!g_time_in_sec) {
		fprintf(stderr, "missing -t (--time) operand\n");
		usage(argv[0]);
		return 1;
	}
	if (!g_quiet_count) {
		fprintf(stderr, "-Q (--continue-on-error) value must be greater than 0\n");
		usage(argv[0]);
		return 1;
	}

	if (strncmp(g_workload_type, "rand", 4) == 0) {
		g_is_random = 1;
		g_workload_type = &g_workload_type[4];
	}

	if (ssl_used && strncmp(sock_impl, "ssl", 3) != 0) {
		fprintf(stderr, "sock impl is not SSL but tried to use one of the SSL only options\n");
		usage(argv[0]);
		return 1;
	}


	if (strcmp(g_workload_type, "read") == 0 || strcmp(g_workload_type, "write") == 0) {
		g_rw_percentage = strcmp(g_workload_type, "read") == 0 ? 100 : 0;
		if (g_mix_specified) {
			fprintf(stderr, "Ignoring -M (--rwmixread) option... Please use -M option"
				" only when using rw or randrw.\n");
		}
	} else if (strcmp(g_workload_type, "rw") == 0) {
		if (g_rw_percentage < 0 || g_rw_percentage > 100) {
			fprintf(stderr,
				"-M (--rwmixread) must be specified to value from 0 to 100 "
				"for rw or randrw.\n");
			return 1;
		}
	} else {
		fprintf(stderr,
			"-w (--io-pattern) io pattern type must be one of\n"
			"(read, write, randread, randwrite, rw, randrw)\n");
		return 1;
	}

	if (g_sock_zcopy_threshold > 0) {
		if (!g_sock_threshold_impl) {
			fprintf(stderr,
				"--zerocopy-threshold must be set with sock implementation specified(--zerocopy-threshold-sock-impl <impl>)\n");
			return 1;
		}

		perf_set_sock_opts(g_sock_threshold_impl, "zerocopy_threshold", g_sock_zcopy_threshold, NULL);
	}

	if (g_number_ios && g_warmup_time_in_sec) {
		fprintf(stderr, "-d (--number-ios) with -a (--warmup-time) is not supported\n");
		return 1;
	}

	if (g_number_ios && g_number_ios < g_queue_depth) {
		fprintf(stderr, "-d (--number-ios) less than -q (--io-depth) is not supported\n");
		return 1;
	}

	if (g_rdma_srq_size != 0) {
		struct spdk_nvme_transport_opts opts;

		spdk_nvme_transport_get_opts(&opts, sizeof(opts));
		opts.rdma_srq_size = g_rdma_srq_size;

		rc = spdk_nvme_transport_set_opts(&opts, sizeof(opts));
		if (rc != 0) {
			fprintf(stderr, "Failed to set NVMe transport options.\n");
			return 1;
		}
	}

	if (TAILQ_EMPTY(&g_trid_list)) {
		/* If no transport IDs specified, default to enumerating all local PCIe devices */
		rc = spdk_nvme_trid_entry_parse(&g_trids[trid_count].entry, "trtype:PCIe");
		if (rc < 0) {
			return 1;
		}

		TAILQ_INSERT_TAIL(&g_trid_list, &g_trids[trid_count], tailq);
	} else {
		struct _trid_entry *trid_entry;

		env_opts->no_pci = true;
		/* check whether there is local PCIe type */
		TAILQ_FOREACH(trid_entry, &g_trid_list, tailq) {
			if (trid_entry->entry.trid.trtype == SPDK_NVME_TRANSPORT_PCIE) {
				env_opts->no_pci = false;
				break;
			}
		}
	}

	g_file_optind = optind;

	return 0;
}

static int
register_workers(void)
{
	uint32_t i;
	struct worker_thread *worker;

	SPDK_ENV_FOREACH_CORE(i) {
		worker = calloc(1, sizeof(*worker));
		if (worker == NULL) {
			fprintf(stderr, "Unable to allocate worker\n");
			return -1;
		}

		TAILQ_INIT(&worker->ns_ctx);
		worker->lcore = i;
		TAILQ_INSERT_TAIL(&g_workers, worker, link);
		g_num_workers++;
	}

	return 0;
}

static void
unregister_workers(void)
{
	struct worker_thread *worker, *tmp_worker;
	struct ns_worker_ctx *ns_ctx, *tmp_ns_ctx;

	/* Free namespace context and worker thread */
	TAILQ_FOREACH_SAFE(worker, &g_workers, link, tmp_worker) {
		TAILQ_REMOVE(&g_workers, worker, link);

		TAILQ_FOREACH_SAFE(ns_ctx, &worker->ns_ctx, link, tmp_ns_ctx) {
			TAILQ_REMOVE(&worker->ns_ctx, ns_ctx, link);
			spdk_histogram_data_free(ns_ctx->histogram);
			free(ns_ctx);
		}

		free(worker);
	}
}

static bool
probe_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	 struct spdk_nvme_ctrlr_opts *opts)
{
	struct spdk_nvme_trid_entry *trid_entry = cb_ctx;

	if (trid->trtype == SPDK_NVME_TRANSPORT_PCIE) {
		if (g_disable_sq_cmb) {
			opts->use_cmb_sqs = false;
		}
		if (g_no_shn_notification) {
			opts->no_shn_notification = true;
		}
		if (g_enable_interrupt) {
			opts->enable_interrupts = true;
		}
	}

	if (trid->trtype != trid_entry->trid.trtype &&
	    strcasecmp(trid->trstring, trid_entry->trid.trstring)) {
		return false;
	}

	opts->io_queue_size = g_io_queue_size;

	/* Set the header and data_digest */
	opts->header_digest = g_header_digest;
	opts->data_digest = g_data_digest;
	opts->keep_alive_timeout_ms = g_keep_alive_timeout_in_ms;
	opts->tls_psk = g_psk;
	opts->dhchap_key = g_dhchap;
	opts->dhchap_ctrlr_key = g_dhchap_ctrlr;
	memcpy(opts->hostnqn, trid_entry->hostnqn, sizeof(opts->hostnqn));

	opts->transport_tos = g_transport_tos;
	if (opts->num_io_queues < g_num_workers * g_nr_io_queues_per_ns) {
		opts->num_io_queues = g_num_workers * g_nr_io_queues_per_ns;
	}

	return true;
}

static void
attach_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	  struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_ctrlr_opts *opts)
{
	if (trid->trtype == SPDK_NVME_TRANSPORT_PCIE) {
		if (g_enable_interrupt && !opts->enable_interrupts) {
			fprintf(stderr, "Couldn't enable interrupts on NVMe controller at %s\n", trid->traddr);
			return;
		}
	}

	register_ctrlr(ctrlr, cb_ctx);
}

static int
register_controllers(void)
{
	struct _trid_entry *trid_entry;

	printf("Initializing NVMe Controllers\n");

	if (g_vmd && spdk_vmd_init()) {
		fprintf(stderr, "Failed to initialize VMD."
			" Some NVMe devices can be unavailable.\n");
	}

	TAILQ_FOREACH(trid_entry, &g_trid_list, tailq) {
		if (spdk_nvme_probe(&trid_entry->entry.trid, &trid_entry->entry, probe_cb, attach_cb, NULL) != 0) {
			fprintf(stderr, "spdk_nvme_probe() failed for transport address '%s'\n",
				trid_entry->entry.trid.traddr);
			return -1;
		}
	}

	return 0;
}

static void
unregister_controllers(void)
{
	struct ctrlr_entry *entry, *tmp;
	struct spdk_nvme_detach_ctx *detach_ctx = NULL;

	TAILQ_FOREACH_SAFE(entry, &g_controllers, link, tmp) {
		TAILQ_REMOVE(&g_controllers, entry, link);

		spdk_dma_free(entry->latency_page);
		if (g_latency_ssd_tracking_enable &&
		    spdk_nvme_ctrlr_is_feature_supported(entry->ctrlr, SPDK_NVME_INTEL_FEAT_LATENCY_TRACKING)) {
			set_latency_tracking_feature(entry->ctrlr, false);
		}

		if (g_nr_unused_io_queues) {
			int i;

			for (i = 0; i < g_nr_unused_io_queues; i++) {
				spdk_nvme_ctrlr_free_io_qpair(entry->unused_qpairs[i]);
			}

			free(entry->unused_qpairs);
		}

		spdk_nvme_detach_async(entry->ctrlr, &detach_ctx);
		free(entry);
	}

	if (detach_ctx) {
		spdk_nvme_detach_poll(detach_ctx);
	}

	if (g_vmd) {
		spdk_vmd_fini();
	}
}

static int
allocate_ns_worker(struct ns_entry *entry, struct worker_thread *worker)
{
	struct ns_worker_ctx	*ns_ctx;

	ns_ctx = calloc(1, sizeof(struct ns_worker_ctx));
	if (!ns_ctx) {
		return -1;
	}

	printf("Associating %s with lcore %d\n", entry->name, worker->lcore);
	ns_ctx->stats.min_tsc = UINT64_MAX;
	ns_ctx->entry = entry;
	ns_ctx->histogram = spdk_histogram_data_alloc();
	TAILQ_INSERT_TAIL(&worker->ns_ctx, ns_ctx, link);

	return 0;
}

static int
associate_workers_with_ns(void)
{
	struct ns_entry		*entry = TAILQ_FIRST(&g_namespaces);
	struct worker_thread	*worker = TAILQ_FIRST(&g_workers);
	int			i, count;

	/* Each core contains single worker, and namespaces are associated as follows:
	 * --use-every-core not specified (default):
	 * 2) equal workers and namespaces - each worker associated with single namespace
	 * 3) more workers than namespaces - each namespace is associated with one or more workers
	 * 4) more namespaces than workers - each worker is associated with one or more namespaces
	 * --use-every-core option enabled - every worker is associated with all namespaces
	 */
	if (g_use_every_core) {
		TAILQ_FOREACH(worker, &g_workers, link) {
			TAILQ_FOREACH(entry, &g_namespaces, link) {
				if (allocate_ns_worker(entry, worker) != 0) {
					return -1;
				}
			}
		}
		return 0;
	}

	count = g_num_namespaces > g_num_workers ? g_num_namespaces : g_num_workers;

	for (i = 0; i < count; i++) {
		if (entry == NULL) {
			break;
		}

		if (allocate_ns_worker(entry, worker) != 0) {
			return -1;
		}

		worker = TAILQ_NEXT(worker, link);
		if (worker == NULL) {
			worker = TAILQ_FIRST(&g_workers);
		}

		entry = TAILQ_NEXT(entry, link);
		if (entry == NULL) {
			entry = TAILQ_FIRST(&g_namespaces);
		}

	}

	return 0;
}

static void *
nvme_poll_ctrlrs(void *arg)
{
	struct ctrlr_entry *entry;
	int oldstate;
	int rc;

	spdk_unaffinitize_thread();

	while (true) {
		pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &oldstate);

		TAILQ_FOREACH(entry, &g_controllers, link) {
			if (entry->trtype != SPDK_NVME_TRANSPORT_PCIE) {
				rc = spdk_nvme_ctrlr_process_admin_completions(entry->ctrlr);
				if (spdk_unlikely(rc < 0 && !g_exit)) {
					g_exit = true;
				}
			}
		}

		pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, &oldstate);

		/* This is a pthread cancellation point and cannot be removed. */
		sleep(1);
	}

	return NULL;
}

static void
sig_handler(int signo)
{
	g_exit = true;
}

static int
setup_sig_handlers(void)
{
	struct sigaction sigact = {};
	int rc;

	sigemptyset(&sigact.sa_mask);
	sigact.sa_handler = sig_handler;
	rc = sigaction(SIGINT, &sigact, NULL);
	if (rc < 0) {
		fprintf(stderr, "sigaction(SIGINT) failed, errno %d (%s)\n", errno, strerror(errno));
		return -1;
	}

	rc = sigaction(SIGTERM, &sigact, NULL);
	if (rc < 0) {
		fprintf(stderr, "sigaction(SIGTERM) failed, errno %d (%s)\n", errno, strerror(errno));
		return -1;
	}

	return 0;
}

int
main(int argc, char **argv)
{
	int rc;
	struct worker_thread *worker, *main_worker;
	struct ns_worker_ctx *ns_ctx;
	struct spdk_env_opts opts;
	pthread_t thread_id = 0;

	/* Use the runtime PID to set the random seed */
	srand(getpid());

	opts.opts_size = sizeof(opts);
	spdk_env_opts_init(&opts);
	opts.name = "perf";
	opts.pci_allowed = g_allowed_pci_addr;
	rc = parse_args(argc, argv, &opts);
	if (rc != 0 || rc == HELP_RETURN_CODE) {
		free_key(&g_psk);
		free_key(&g_dhchap);
		free_key(&g_dhchap_ctrlr);
		if (rc == HELP_RETURN_CODE) {
			return 0;
		}

		return rc;
	}
	/* Transport statistics are printed from each thread.
	 * To avoid mess in terminal, init and use mutex */
	rc = pthread_mutex_init(&g_stats_mutex, NULL);
	if (rc != 0) {
		fprintf(stderr, "Failed to init mutex\n");
		free_key(&g_psk);
		free_key(&g_dhchap);
		free_key(&g_dhchap_ctrlr);
		return -1;
	}
	if (spdk_env_init(&opts) < 0) {
		fprintf(stderr, "Unable to initialize SPDK env\n");
		pthread_mutex_destroy(&g_stats_mutex);
		free_key(&g_psk);
		free_key(&g_dhchap);
		free_key(&g_dhchap_ctrlr);
		return -1;
	}

	rc = spdk_keyring_init();
	if (rc != 0) {
		fprintf(stderr, "Unable to initialize keyring: %s\n", spdk_strerror(-rc));
		pthread_mutex_destroy(&g_stats_mutex);
		free_key(&g_psk);
		free_key(&g_dhchap);
		free_key(&g_dhchap_ctrlr);
		spdk_env_fini();
		return -1;
	}

	rc = setup_sig_handlers();
	if (rc != 0) {
		rc = -1;
		goto cleanup;
	}

	g_tsc_rate = spdk_get_ticks_hz();

	if (register_workers() != 0) {
		rc = -1;
		goto cleanup;
	}

#if defined(HAVE_LIBAIO) || defined(SPDK_CONFIG_URING)
	if (register_files(argc, argv) != 0) {
		rc = -1;
		goto cleanup;
	}
#endif

	if (register_controllers() != 0) {
		rc = -1;
		goto cleanup;
	}

	if (g_warn) {
		printf("WARNING: Some requested NVMe devices were skipped\n");
	}

	if (g_num_namespaces == 0) {
		fprintf(stderr, "No valid NVMe controllers or AIO or URING devices found\n");
		goto cleanup;
	}

	if (g_num_workers > 1 && g_quiet_count > 1) {
		fprintf(stderr, "Error message rate-limiting enabled across multiple threads.\n");
		fprintf(stderr, "Error suppression count may not be exact.\n");
	}

	rc = pthread_create(&thread_id, NULL, &nvme_poll_ctrlrs, NULL);
	if (rc != 0) {
		fprintf(stderr, "Unable to spawn a thread to poll admin queues.\n");
		goto cleanup;
	}

	if (associate_workers_with_ns() != 0) {
		rc = -1;
		goto cleanup;
	}

	rc = pthread_barrier_init(&g_worker_sync_barrier, NULL, g_num_workers);
	if (rc != 0) {
		fprintf(stderr, "Unable to initialize thread sync barrier\n");
		goto cleanup;
	}

	printf("Initialization complete. Launching workers.\n");

	/* Launch all of the secondary workers */
	g_main_core = spdk_env_get_current_core();
	main_worker = NULL;
	TAILQ_FOREACH(worker, &g_workers, link) {
		if (worker->lcore != g_main_core) {
			spdk_env_thread_launch_pinned(worker->lcore, work_fn, worker);
		} else {
			assert(main_worker == NULL);
			main_worker = worker;
		}
	}

	assert(main_worker != NULL);
	work_fn(main_worker);

	spdk_env_thread_wait_all();

	print_stats();

	pthread_barrier_destroy(&g_worker_sync_barrier);

cleanup:
	fflush(stdout);

	if (thread_id && pthread_cancel(thread_id) == 0) {
		pthread_join(thread_id, NULL);
	}

	/* Collect errors from all workers and namespaces */
	TAILQ_FOREACH(worker, &g_workers, link) {
		if (rc != 0) {
			break;
		}

		TAILQ_FOREACH(ns_ctx, &worker->ns_ctx, link) {
			if (ns_ctx->status != 0) {
				rc = ns_ctx->status;
				break;
			}
		}
	}

	unregister_namespaces();
	unregister_controllers();
	unregister_workers();

	free_key(&g_psk);
	free_key(&g_dhchap);
	free_key(&g_dhchap_ctrlr);
	spdk_keyring_cleanup();
	spdk_env_fini();

	pthread_mutex_destroy(&g_stats_mutex);

	if (rc != 0) {
		fprintf(stderr, "%s: errors occurred\n", argv[0]);
	}

	return rc;
}
