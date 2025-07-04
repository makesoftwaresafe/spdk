/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation. All rights reserved.
 *   Copyright (c) 2019 Mellanox Technologies LTD. All rights reserved.
 *   Copyright (c) 2021, 2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk/version.h"

#include "spdk_internal/event.h"

#include "spdk/assert.h"
#include "spdk/env.h"
#include "spdk/init.h"
#include "spdk/log.h"
#include "spdk/thread.h"
#include "spdk/trace.h"
#include "spdk/string.h"
#include "spdk/scheduler.h"
#include "spdk/rpc.h"
#include "spdk/util.h"
#include "spdk/file.h"
#include "spdk/config.h"
#include "event_internal.h"

#define SPDK_APP_DEFAULT_LOG_LEVEL		SPDK_LOG_NOTICE
#define SPDK_APP_DEFAULT_LOG_PRINT_LEVEL	SPDK_LOG_INFO
#define SPDK_APP_DEFAULT_NUM_TRACE_ENTRIES	SPDK_DEFAULT_NUM_TRACE_ENTRIES

#define SPDK_APP_DPDK_DEFAULT_MEM_SIZE		-1
#define SPDK_APP_DPDK_DEFAULT_MAIN_CORE		-1
#define SPDK_APP_DPDK_DEFAULT_MEM_CHANNEL	-1
#define SPDK_APP_DPDK_DEFAULT_CORE_MASK		"0x1"
#define SPDK_APP_DPDK_DEFAULT_BASE_VIRTADDR	0x200000000000
#define SPDK_APP_DEFAULT_CORE_LIMIT		0x140000000 /* 5 GiB */

/* For core counts <= 63, the message memory pool size is set to
 * SPDK_DEFAULT_MSG_MEMPOOL_SIZE.
 * For core counts > 63, the message memory pool size is depended on
 * number of cores. Per core, it is calculated as SPDK_MSG_MEMPOOL_CACHE_SIZE
 * multiplied by factor of 4 to have space for multiple spdk threads running
 * on single core (e.g  iscsi + nvmf + vhost ). */
#define SPDK_APP_PER_CORE_MSG_MEMPOOL_SIZE	(4 * SPDK_MSG_MEMPOOL_CACHE_SIZE)

struct spdk_app {
	void				*json_data;
	size_t				json_data_size;
	bool				json_config_ignore_errors;
	bool				stopped;
	const char			*rpc_addr;
	const char			**rpc_allowlist;
	FILE				*rpc_log_file;
	enum spdk_log_level		rpc_log_level;
	int				shm_id;
	spdk_app_shutdown_cb		shutdown_cb;
	int				rc;
};

static struct spdk_app g_spdk_app;
static spdk_msg_fn g_start_fn = NULL;
static void *g_start_arg = NULL;
static bool g_delay_subsystem_init = false;
static bool g_shutdown_sig_received = false;
static char *g_executable_name;
static struct spdk_app_opts g_default_opts;
static int g_core_locks[SPDK_CONFIG_MAX_LCORES];

static struct {
	uint64_t irq;
	uint64_t usr;
	uint64_t sys;
} g_initial_stat[SPDK_CONFIG_MAX_LCORES];

int
spdk_app_get_shm_id(void)
{
	return g_spdk_app.shm_id;
}

/* append one empty option to indicate the end of the array */
static const struct option g_cmdline_options[] = {
#define CONFIG_FILE_OPT_IDX	'c'
	{"config",			required_argument,	NULL, CONFIG_FILE_OPT_IDX},
#define LIMIT_COREDUMP_OPT_IDX 'd'
	{"limit-coredump",		no_argument,		NULL, LIMIT_COREDUMP_OPT_IDX},
#define TPOINT_GROUP_OPT_IDX 'e'
	{"tpoint-group",		required_argument,	NULL, TPOINT_GROUP_OPT_IDX},
#define SINGLE_FILE_SEGMENTS_OPT_IDX 'g'
	{"single-file-segments",	no_argument,		NULL, SINGLE_FILE_SEGMENTS_OPT_IDX},
#define HELP_OPT_IDX		'h'
	{"help",			no_argument,		NULL, HELP_OPT_IDX},
#define SHM_ID_OPT_IDX		'i'
	{"shm-id",			required_argument,	NULL, SHM_ID_OPT_IDX},
#define CPUMASK_OPT_IDX		'm'
	{"cpumask",			required_argument,	NULL, CPUMASK_OPT_IDX},
#define MEM_CHANNELS_OPT_IDX	'n'
	{"mem-channels",		required_argument,	NULL, MEM_CHANNELS_OPT_IDX},
#define MAIN_CORE_OPT_IDX	'p'
	{"main-core",			required_argument,	NULL, MAIN_CORE_OPT_IDX},
#define RPC_SOCKET_OPT_IDX	'r'
	{"rpc-socket",			required_argument,	NULL, RPC_SOCKET_OPT_IDX},
#define MEM_SIZE_OPT_IDX	's'
	{"mem-size",			required_argument,	NULL, MEM_SIZE_OPT_IDX},
#define NO_PCI_OPT_IDX		'u'
	{"no-pci",			no_argument,		NULL, NO_PCI_OPT_IDX},
#define VERSION_OPT_IDX		'v'
	{"version",			no_argument,		NULL, VERSION_OPT_IDX},
#define PCI_BLOCKED_OPT_IDX	'B'
	{"pci-blocked",			required_argument,	NULL, PCI_BLOCKED_OPT_IDX},
#define LOGFLAG_OPT_IDX		'L'
	{"logflag",			required_argument,	NULL, LOGFLAG_OPT_IDX},
#define HUGE_UNLINK_OPT_IDX	'R'
	{"huge-unlink",			no_argument,		NULL, HUGE_UNLINK_OPT_IDX},
#define PCI_ALLOWED_OPT_IDX	'A'
	{"pci-allowed",			required_argument,	NULL, PCI_ALLOWED_OPT_IDX},
#define INTERRUPT_MODE_OPT_IDX 256
	{"interrupt-mode",		no_argument,		NULL, INTERRUPT_MODE_OPT_IDX},
#define SILENCE_NOTICELOG_OPT_IDX 257
	{"silence-noticelog",		no_argument,		NULL, SILENCE_NOTICELOG_OPT_IDX},
#define WAIT_FOR_RPC_OPT_IDX	258
	{"wait-for-rpc",		no_argument,		NULL, WAIT_FOR_RPC_OPT_IDX},
#define HUGE_DIR_OPT_IDX	259
	{"huge-dir",			required_argument,	NULL, HUGE_DIR_OPT_IDX},
#define NUM_TRACE_ENTRIES_OPT_IDX	260
	{"num-trace-entries",		required_argument,	NULL, NUM_TRACE_ENTRIES_OPT_IDX},
#define JSON_CONFIG_OPT_IDX		262
	{"json",			required_argument,	NULL, JSON_CONFIG_OPT_IDX},
#define JSON_CONFIG_IGNORE_INIT_ERRORS_IDX	263
	{"json-ignore-init-errors",	no_argument,		NULL, JSON_CONFIG_IGNORE_INIT_ERRORS_IDX},
#define IOVA_MODE_OPT_IDX	264
	{"iova-mode",			required_argument,	NULL, IOVA_MODE_OPT_IDX},
#define BASE_VIRTADDR_OPT_IDX	265
	{"base-virtaddr",		required_argument,	NULL, BASE_VIRTADDR_OPT_IDX},
#define ENV_CONTEXT_OPT_IDX	266
	{"env-context",			required_argument,	NULL, ENV_CONTEXT_OPT_IDX},
#define DISABLE_CPUMASK_LOCKS_OPT_IDX	267
	{"disable-cpumask-locks",	no_argument,		NULL, DISABLE_CPUMASK_LOCKS_OPT_IDX},
#define RPCS_ALLOWED_OPT_IDX	268
	{"rpcs-allowed",		required_argument,	NULL, RPCS_ALLOWED_OPT_IDX},
#define ENV_VF_TOKEN_OPT_IDX 269
	{"vfio-vf-token",		required_argument,	NULL, ENV_VF_TOKEN_OPT_IDX},
#define MSG_MEMPOOL_SIZE_OPT_IDX 270
	{"msg-mempool-size",		required_argument,	NULL, MSG_MEMPOOL_SIZE_OPT_IDX},
#define LCORES_OPT_IDX	271
	{"lcores",			required_argument,	NULL, LCORES_OPT_IDX},
#define NO_HUGE_OPT_IDX	272
	{"no-huge",			no_argument,		NULL, NO_HUGE_OPT_IDX},
#define NO_RPC_SERVER_OPT_IDX	273
	{"no-rpc-server",		no_argument,		NULL, NO_RPC_SERVER_OPT_IDX},
#define ENFORCE_NUMA_OPT_IDX 274
	{"enforce-numa",		no_argument,		NULL, ENFORCE_NUMA_OPT_IDX},
};

static int
parse_proc_stat(unsigned int core, uint64_t *user, uint64_t *sys, uint64_t *irq)
{
	FILE *f;
	uint64_t i, soft_irq, cpu = 0;
	int rc, found = 0;

	f = fopen("/proc/stat", "r");
	if (!f) {
		return -1;
	}

	for (i = 0; i <= core + 1; i++) {
		/* scanf discards input with '*' in format,
		 * cpu;user;nice;system;idle;iowait;irq;softirq;steal;guest;guest_nice */
		rc = fscanf(f, "cpu%li %li %*i %li %*i %*i %li %li %*i %*i %*i\n",
			    &cpu, user, sys, irq, &soft_irq);
		if (rc != 5) {
			continue;
		}

		/* some cores can be disabled, list may not be in order */
		if (cpu == core) {
			found = 1;
			break;
		}
	}

	*irq += soft_irq;

	fclose(f);
	return found ? 0 : -1;
}

static int
init_proc_stat(unsigned int core)
{
	uint64_t usr, sys, irq;

	if (core >= SPDK_CONFIG_MAX_LCORES) {
		return -1;
	}

	if (parse_proc_stat(core, &usr, &sys, &irq) < 0) {
		return -1;
	}

	g_initial_stat[core].irq = irq;
	g_initial_stat[core].usr = usr;
	g_initial_stat[core].sys = sys;

	return 0;
}

int
app_get_proc_stat(unsigned int core, uint64_t *usr, uint64_t *sys, uint64_t *irq)
{
	uint64_t _usr, _sys, _irq;

	if (core >= SPDK_CONFIG_MAX_LCORES) {
		return -1;
	}

	if (parse_proc_stat(core, &_usr, &_sys, &_irq) < 0) {
		return -1;
	}

	*irq = _irq - g_initial_stat[core].irq;
	*usr = _usr - g_initial_stat[core].usr;
	*sys = _sys - g_initial_stat[core].sys;

	return 0;
}

static void
app_start_shutdown(void *ctx)
{
	if (g_spdk_app.shutdown_cb) {
		g_spdk_app.shutdown_cb();
		g_spdk_app.shutdown_cb = NULL;
	} else {
		spdk_app_stop(0);
	}
}

void
spdk_app_start_shutdown(void)
{
	spdk_thread_send_critical_msg(spdk_thread_get_app_thread(), app_start_shutdown);
}

static void
__shutdown_signal(int signo)
{
	if (!g_shutdown_sig_received) {
		g_shutdown_sig_received = true;
		spdk_app_start_shutdown();
	}
}

static int
app_opts_validate(const char *app_opts)
{
	int i = 0, j;

	for (i = 0; app_opts[i] != '\0'; i++) {
		/* ignore getopt control characters */
		if (app_opts[i] == ':' || app_opts[i] == '+' || app_opts[i] == '-') {
			continue;
		}

		for (j = 0; SPDK_APP_GETOPT_STRING[j] != '\0'; j++) {
			if (app_opts[i] == SPDK_APP_GETOPT_STRING[j]) {
				return app_opts[i];
			}
		}
	}
	return 0;
}

static void
calculate_mempool_size(struct spdk_app_opts *opts,
		       struct spdk_app_opts *opts_user)
{
	uint32_t core_count = spdk_env_get_core_count();

	if (!opts_user->msg_mempool_size) {
		/* The user didn't specify msg_mempool_size, so let's calculate it.
		   Set the default (SPDK_DEFAULT_MSG_MEMPOOL_SIZE) if less than
		   64 cores, and use 4k per core otherwise */
		opts->msg_mempool_size = spdk_max(SPDK_DEFAULT_MSG_MEMPOOL_SIZE,
						  core_count * SPDK_APP_PER_CORE_MSG_MEMPOOL_SIZE);
	} else {
		opts->msg_mempool_size = opts_user->msg_mempool_size;
	}
}

void
spdk_app_opts_init(struct spdk_app_opts *opts, size_t opts_size)
{
	if (!opts) {
		SPDK_ERRLOG("opts should not be NULL\n");
		return;
	}

	if (!opts_size) {
		SPDK_ERRLOG("opts_size should not be zero value\n");
		return;
	}

	memset(opts, 0, opts_size);
	opts->opts_size = opts_size;

#define SET_FIELD(field, value) \
	if (offsetof(struct spdk_app_opts, field) + sizeof(opts->field) <= opts_size) { \
		opts->field = value; \
	} \

	SET_FIELD(enable_coredump, true);
	SET_FIELD(shm_id, -1);
	SET_FIELD(mem_size, SPDK_APP_DPDK_DEFAULT_MEM_SIZE);
	SET_FIELD(main_core, SPDK_APP_DPDK_DEFAULT_MAIN_CORE);
	SET_FIELD(mem_channel, SPDK_APP_DPDK_DEFAULT_MEM_CHANNEL);
	SET_FIELD(base_virtaddr, SPDK_APP_DPDK_DEFAULT_BASE_VIRTADDR);
	SET_FIELD(print_level, SPDK_APP_DEFAULT_LOG_PRINT_LEVEL);
	SET_FIELD(rpc_addr, SPDK_DEFAULT_RPC_ADDR);
	SET_FIELD(num_entries, SPDK_APP_DEFAULT_NUM_TRACE_ENTRIES);
	SET_FIELD(delay_subsystem_init, false);
	SET_FIELD(disable_signal_handlers, false);
	SET_FIELD(interrupt_mode, false);
	SET_FIELD(enforce_numa, false);
	/* Don't set msg_mempool_size here, it is set or calculated later */
	SET_FIELD(rpc_allowlist, NULL);
	SET_FIELD(rpc_log_file, NULL);
	SET_FIELD(rpc_log_level, SPDK_LOG_DISABLED);
	SET_FIELD(disable_cpumask_locks, false);
#undef SET_FIELD
}

static int
app_setup_signal_handlers(struct spdk_app_opts *opts)
{
	struct sigaction	sigact;
	sigset_t		sigmask;
	int			rc;

	sigemptyset(&sigmask);
	memset(&sigact, 0, sizeof(sigact));
	sigemptyset(&sigact.sa_mask);

	sigact.sa_handler = SIG_IGN;
	rc = sigaction(SIGPIPE, &sigact, NULL);
	if (rc < 0) {
		SPDK_ERRLOG("sigaction(SIGPIPE) failed\n");
		return rc;
	}

	/* Install the same handler for SIGINT and SIGTERM */
	g_shutdown_sig_received = false;
	sigact.sa_handler = __shutdown_signal;
	rc = sigaction(SIGINT, &sigact, NULL);
	if (rc < 0) {
		SPDK_ERRLOG("sigaction(SIGINT) failed\n");
		return rc;
	}
	sigaddset(&sigmask, SIGINT);

	rc = sigaction(SIGTERM, &sigact, NULL);
	if (rc < 0) {
		SPDK_ERRLOG("sigaction(SIGTERM) failed\n");
		return rc;
	}
	sigaddset(&sigmask, SIGTERM);

	pthread_sigmask(SIG_UNBLOCK, &sigmask, NULL);

	return 0;
}

static void
app_start_application(int rc, void *arg1)
{
	assert(spdk_thread_is_app_thread(NULL));

	if (rc) {
		SPDK_ERRLOG("Failed to load subsystems for RUNTIME state with code: %d\n", rc);
		spdk_app_stop(rc);
		return;
	}

	if (g_spdk_app.rpc_addr) {
		spdk_rpc_server_resume(g_spdk_app.rpc_addr);
	}

	g_start_fn(g_start_arg);
}

static void
app_subsystem_init_done(int rc, void *arg1)
{
	if (rc) {
		SPDK_ERRLOG("Subsystem initialization failed with code: %d\n", rc);
		spdk_app_stop(rc);
		return;
	}

	spdk_rpc_set_allowlist(g_spdk_app.rpc_allowlist);
	spdk_rpc_set_state(SPDK_RPC_RUNTIME);

	if (g_spdk_app.json_data) {
		/* Load SPDK_RPC_RUNTIME RPCs from config file */
		assert(spdk_rpc_get_state() == SPDK_RPC_RUNTIME);
		spdk_subsystem_load_config(g_spdk_app.json_data, g_spdk_app.json_data_size,
					   app_start_application, NULL,
					   !g_spdk_app.json_config_ignore_errors);
		free(g_spdk_app.json_data);
		g_spdk_app.json_data = NULL;
	} else {
		app_start_application(0, NULL);
	}
}

static void
app_do_spdk_subsystem_init(int rc, void *arg1)
{
	struct spdk_rpc_opts opts;

	if (rc) {
		spdk_app_stop(rc);
		return;
	}

	if (g_spdk_app.rpc_addr) {
		opts.size = SPDK_SIZEOF(&opts, log_level);
		opts.log_file = g_spdk_app.rpc_log_file;
		opts.log_level = g_spdk_app.rpc_log_level;

		rc = spdk_rpc_initialize(g_spdk_app.rpc_addr, &opts);
		if (rc) {
			spdk_app_stop(rc);
			return;
		}
		if (g_delay_subsystem_init) {
			return;
		}
		spdk_rpc_server_pause(g_spdk_app.rpc_addr);
	} else {
		SPDK_DEBUGLOG(app_rpc, "RPC server not started\n");
	}
	spdk_subsystem_init(app_subsystem_init_done, NULL);
}

static int
app_opts_add_pci_addr(struct spdk_app_opts *opts, struct spdk_pci_addr **list, char *bdf)
{
	struct spdk_pci_addr *tmp = *list;
	size_t i = opts->num_pci_addr;

	tmp = realloc(tmp, sizeof(*tmp) * (i + 1));
	if (tmp == NULL) {
		SPDK_ERRLOG("realloc error\n");
		return -ENOMEM;
	}

	*list = tmp;
	if (spdk_pci_addr_parse(*list + i, bdf) < 0) {
		SPDK_ERRLOG("Invalid address %s\n", bdf);
		return -EINVAL;
	}

	opts->num_pci_addr++;
	return 0;
}

static int
app_setup_env(struct spdk_app_opts *opts)
{
	struct spdk_env_opts env_opts = {};
	int rc;

	if (opts == NULL) {
		rc = spdk_env_init(NULL);
		if (rc != 0) {
			SPDK_ERRLOG("Unable to reinitialize SPDK env\n");
		}

		return rc;
	}

	env_opts.opts_size = sizeof(env_opts);
	spdk_env_opts_init(&env_opts);

	env_opts.name = opts->name;
	env_opts.core_mask = opts->reactor_mask;
	env_opts.lcore_map = opts->lcore_map;
	env_opts.shm_id = opts->shm_id;
	env_opts.mem_channel = opts->mem_channel;
	env_opts.main_core = opts->main_core;
	env_opts.mem_size = opts->mem_size;
	env_opts.hugepage_single_segments = opts->hugepage_single_segments;
	env_opts.unlink_hugepage = opts->unlink_hugepage;
	env_opts.hugedir = opts->hugedir;
	env_opts.no_pci = opts->no_pci;
	env_opts.num_pci_addr = opts->num_pci_addr;
	env_opts.pci_blocked = opts->pci_blocked;
	env_opts.pci_allowed = opts->pci_allowed;
	env_opts.base_virtaddr = opts->base_virtaddr;
	env_opts.env_context = opts->env_context;
	env_opts.iova_mode = opts->iova_mode;
	env_opts.vf_token = opts->vf_token;
	env_opts.no_huge = opts->no_huge;
	env_opts.enforce_numa = opts->enforce_numa;

	rc = spdk_env_init(&env_opts);
	free(env_opts.pci_blocked);
	free(env_opts.pci_allowed);

	if (rc < 0) {
		SPDK_ERRLOG("Unable to initialize SPDK env\n");
		if (getuid() != 0) {
			SPDK_ERRLOG("You may need to run as root\n");
		}
	}

	return rc;
}

static int
app_setup_trace(struct spdk_app_opts *opts)
{
	char		shm_name[64];
	uint64_t	tpoint_group_mask, tpoint_mask = -1ULL;
	char		*end = NULL, *tpoint_group_mask_str, *tpoint_group_str = NULL;
	char		*tp_g_str, *tpoint_group, *tpoints;
	bool		error_found = false;
	uint64_t	group_id;

	if (opts->shm_id >= 0) {
		snprintf(shm_name, sizeof(shm_name), "/%s%s%d", opts->name,
			 SPDK_TRACE_SHM_NAME_BASE, opts->shm_id);
	} else {
		snprintf(shm_name, sizeof(shm_name), "/%s%spid%d", opts->name,
			 SPDK_TRACE_SHM_NAME_BASE, (int)getpid());
	}

	if (spdk_trace_init(shm_name, opts->num_entries, 0) != 0) {
		return -1;
	}

	if (opts->tpoint_group_mask == NULL) {
		return 0;
	}

	tpoint_group_mask_str = strdup(opts->tpoint_group_mask);
	if (tpoint_group_mask_str == NULL) {
		SPDK_ERRLOG("Unable to get string of tpoint group mask from opts.\n");
		return -1;
	}
	/* Save a pointer to the original value of the tpoint group mask string
	 * to free later, because spdk_strsepq() modifies given char*. */
	tp_g_str = tpoint_group_mask_str;
	while ((tpoint_group_str = spdk_strsepq(&tpoint_group_mask_str, ",")) != NULL) {
		if (strchr(tpoint_group_str, ':')) {
			/* Get the tpoint group mask */
			tpoint_group = spdk_strsepq(&tpoint_group_str, ":");
			/* Get the tpoint mask inside that group */
			tpoints = spdk_strsepq(&tpoint_group_str, ":");

			errno = 0;
			tpoint_group_mask = strtoull(tpoint_group, &end, 16);
			if (*end != '\0' || errno) {
				tpoint_group_mask = spdk_trace_create_tpoint_group_mask(tpoint_group);
				if (tpoint_group_mask == 0) {
					error_found = true;
					break;
				}
			}
			/* Check if tpoint group mask has only one bit set.
			 * This is to avoid enabling individual tpoints in
			 * more than one tracepoint group at once. */
			if (!spdk_u64_is_pow2(tpoint_group_mask)) {
				SPDK_ERRLOG("Tpoint group mask: %s contains multiple tpoint groups.\n", tpoint_group);
				SPDK_ERRLOG("This is not supported, to prevent from activating tpoints by mistake.\n");
				error_found = true;
				break;
			}

			errno = 0;
			tpoint_mask = strtoull(tpoints, &end, 16);
			if (*end != '\0' || errno) {
				error_found = true;
				break;
			}
		} else {
			errno = 0;
			tpoint_group_mask = strtoull(tpoint_group_str, &end, 16);
			if (*end != '\0' || errno) {
				tpoint_group_mask = spdk_trace_create_tpoint_group_mask(tpoint_group_str);
				if (tpoint_group_mask == 0) {
					error_found = true;
					break;
				}
			}
			tpoint_mask = -1ULL;
		}

		for (group_id = 0; group_id < SPDK_TRACE_MAX_GROUP_ID; ++group_id) {
			if (tpoint_group_mask & (1 << group_id)) {
				spdk_trace_set_tpoints(group_id, tpoint_mask);
			}
		}
	}

	if (error_found) {
		SPDK_ERRLOG("invalid tpoint mask %s\n", opts->tpoint_group_mask);
		free(tp_g_str);
		return -1;
	} else {
		SPDK_NOTICELOG("Tracepoint Group Mask %s specified.\n", opts->tpoint_group_mask);
		SPDK_NOTICELOG("Use 'spdk_trace -s %s %s %d' to capture a snapshot of events at runtime.\n",
			       opts->name,
			       opts->shm_id >= 0 ? "-i" : "-p",
			       opts->shm_id >= 0 ? opts->shm_id : getpid());
#if defined(__linux__)
		SPDK_NOTICELOG("'spdk_trace' without parameters will also work if this is the only\n");
		SPDK_NOTICELOG("SPDK application currently running.\n");
		SPDK_NOTICELOG("Or copy /dev/shm%s for offline analysis/debug.\n", shm_name);
#endif
	}
	free(tp_g_str);

	return 0;
}

static void
bootstrap_fn(void *arg1)
{
	spdk_rpc_set_allowlist(g_spdk_app.rpc_allowlist);

	if (g_spdk_app.json_data) {
		/* Load SPDK_RPC_STARTUP RPCs from config file */
		assert(spdk_rpc_get_state() == SPDK_RPC_STARTUP);
		spdk_subsystem_load_config(g_spdk_app.json_data, g_spdk_app.json_data_size,
					   app_do_spdk_subsystem_init, NULL,
					   !g_spdk_app.json_config_ignore_errors);
	} else {
		app_do_spdk_subsystem_init(0, NULL);
	}
}

static void
app_copy_opts(struct spdk_app_opts *opts, struct spdk_app_opts *opts_user, size_t opts_size)
{
	spdk_app_opts_init(opts, sizeof(*opts));
	opts->opts_size = opts_size;

#define SET_FIELD(field) \
        if (offsetof(struct spdk_app_opts, field) + sizeof(opts->field) <= (opts->opts_size)) { \
		opts->field = opts_user->field; \
	} \

	SET_FIELD(name);
	SET_FIELD(json_config_file);
	SET_FIELD(json_config_ignore_errors);
	SET_FIELD(rpc_addr);
	SET_FIELD(reactor_mask);
	SET_FIELD(lcore_map);
	SET_FIELD(tpoint_group_mask);
	SET_FIELD(shm_id);
	SET_FIELD(shutdown_cb);
	SET_FIELD(enable_coredump);
	SET_FIELD(mem_channel);
	SET_FIELD(main_core);
	SET_FIELD(mem_size);
	SET_FIELD(no_pci);
	SET_FIELD(hugepage_single_segments);
	SET_FIELD(unlink_hugepage);
	SET_FIELD(no_huge);
	SET_FIELD(hugedir);
	SET_FIELD(print_level);
	SET_FIELD(num_pci_addr);
	SET_FIELD(pci_blocked);
	SET_FIELD(pci_allowed);
	SET_FIELD(iova_mode);
	SET_FIELD(delay_subsystem_init);
	SET_FIELD(num_entries);
	SET_FIELD(env_context);
	SET_FIELD(log);
	SET_FIELD(base_virtaddr);
	SET_FIELD(disable_signal_handlers);
	SET_FIELD(interrupt_mode);
	SET_FIELD(enforce_numa);
	SET_FIELD(msg_mempool_size);
	SET_FIELD(rpc_allowlist);
	SET_FIELD(vf_token);
	SET_FIELD(rpc_log_file);
	SET_FIELD(rpc_log_level);
	SET_FIELD(json_data);
	SET_FIELD(json_data_size);
	SET_FIELD(disable_cpumask_locks);

	/* You should not remove this statement, but need to update the assert statement
	 * if you add a new field, and also add a corresponding SET_FIELD statement */
	SPDK_STATIC_ASSERT(sizeof(struct spdk_app_opts) == 253, "Incorrect size");

#undef SET_FIELD
}

static int
unclaim_cpu_cores(uint32_t *failed_core)
{
	char core_name[40];
	uint32_t i;
	int rc;

	for (i = 0; i < SPDK_CONFIG_MAX_LCORES; i++) {
		if (g_core_locks[i] != -1 && g_core_locks[i] != 0) {
			snprintf(core_name, sizeof(core_name), "/var/tmp/spdk_cpu_lock_%03d", i);
			rc = close(g_core_locks[i]);
			if (rc) {
				SPDK_ERRLOG("Failed to close lock fd for core %d, errno: %d\n", i, errno);
				goto error;
			}

			g_core_locks[i] = -1;
			rc = unlink(core_name);
			if (rc) {
				SPDK_ERRLOG("Failed to unlink lock fd for core %d, errno: %d\n", i, errno);
				goto error;
			}
		}
	}

	return 0;

error:
	if (failed_core != NULL) {
		/* Set number of core we failed to claim. */
		*failed_core = i;
	}
	return -1;
}

static int
claim_cpu_cores(uint32_t *failed_core)
{
	char core_name[40];
	int core_fd, pid;
	int *core_map;
	uint32_t core;

	struct flock core_lock = {
		.l_type = F_WRLCK,
		.l_whence = SEEK_SET,
		.l_start = 0,
		.l_len = 0,
	};

	SPDK_ENV_FOREACH_CORE(core) {
		if (g_core_locks[core] != -1) {
			/* If this core is locked already, do not try lock it again. */
			continue;
		}

		snprintf(core_name, sizeof(core_name), "/var/tmp/spdk_cpu_lock_%03d", core);
		core_fd = open(core_name, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
		if (core_fd == -1) {
			SPDK_ERRLOG("Could not open %s (%s).\n", core_name, spdk_strerror(errno));
			/* Return number of core we failed to claim. */
			goto error;
		}

		if (ftruncate(core_fd, sizeof(int)) != 0) {
			SPDK_ERRLOG("Could not truncate %s (%s).\n", core_name, spdk_strerror(errno));
			close(core_fd);
			goto error;
		}

		core_map = mmap(NULL, sizeof(int), PROT_READ | PROT_WRITE, MAP_SHARED, core_fd, 0);
		if (core_map == MAP_FAILED) {
			SPDK_ERRLOG("Could not mmap core %s (%s).\n", core_name, spdk_strerror(errno));
			close(core_fd);
			goto error;
		}

		if (fcntl(core_fd, F_SETLK, &core_lock) != 0) {
			pid = *core_map;
			SPDK_ERRLOG("Cannot create lock on core %" PRIu32 ", probably process %d has claimed it.\n",
				    core, pid);
			munmap(core_map, sizeof(int));
			close(core_fd);
			goto error;
		}

		/* We write the PID to the core lock file so that other processes trying
		* to claim the same core will know what process is holding the lock. */
		*core_map = (int)getpid();
		munmap(core_map, sizeof(int));
		g_core_locks[core] = core_fd;
		/* Keep core_fd open to maintain the lock. */
	}

	return 0;

error:
	if (failed_core != NULL) {
		/* Set number of core we failed to claim. */
		*failed_core = core;
	}
	unclaim_cpu_cores(NULL);
	return -1;
}

int
spdk_app_start(struct spdk_app_opts *opts_user, spdk_msg_fn start_fn,
	       void *arg1)
{
	int			rc;
	char			*tty;
	struct spdk_cpuset	tmp_cpumask = {};
	static bool		g_env_was_setup = false;
	struct spdk_app_opts opts_local = {};
	struct spdk_app_opts *opts = &opts_local;
	uint32_t i, core;

	if (!opts_user) {
		SPDK_ERRLOG("opts_user should not be NULL\n");
		return 1;
	}

	if (!opts_user->opts_size) {
		SPDK_ERRLOG("The opts_size in opts_user structure should not be zero value\n");
		return 1;
	}

	if (opts_user->name == NULL) {
		SPDK_ERRLOG("spdk_app_opts::name not specified\n");
		return 1;
	}

	app_copy_opts(opts, opts_user, opts_user->opts_size);

	if (!start_fn) {
		SPDK_ERRLOG("start_fn should not be NULL\n");
		return 1;
	}

	if (!opts->rpc_addr && opts->delay_subsystem_init) {
		SPDK_ERRLOG("Cannot use '--wait-for-rpc' if no RPC server is going to be started.\n");
		return 1;
	}

	if (!(opts->lcore_map || opts->reactor_mask)) {
		/* Set default CPU mask */
		opts->reactor_mask = SPDK_APP_DPDK_DEFAULT_CORE_MASK;
	}

	tty = ttyname(STDERR_FILENO);
	if (opts->print_level > SPDK_LOG_WARN &&
	    isatty(STDERR_FILENO) &&
	    tty &&
	    !strncmp(tty, "/dev/tty", strlen("/dev/tty"))) {
		printf("Warning: printing stderr to console terminal without -q option specified.\n");
		printf("Suggest using --silence-noticelog to disable logging to stderr and\n");
		printf("monitor syslog, or redirect stderr to a file.\n");
		printf("(Delaying for 10 seconds...)\n");
		sleep(10);
	}

	spdk_log_set_print_level(opts->print_level);

#ifndef SPDK_NO_RLIMIT
	if (opts->enable_coredump) {
		struct rlimit core_limits;

		core_limits.rlim_cur = core_limits.rlim_max = SPDK_APP_DEFAULT_CORE_LIMIT;
		setrlimit(RLIMIT_CORE, &core_limits);
	}
#endif

	if (opts->interrupt_mode) {
		spdk_interrupt_mode_enable();
	}

	memset(&g_spdk_app, 0, sizeof(g_spdk_app));

	g_spdk_app.json_config_ignore_errors = opts->json_config_ignore_errors;
	g_spdk_app.rpc_addr = opts->rpc_addr;
	g_spdk_app.rpc_allowlist = opts->rpc_allowlist;
	g_spdk_app.rpc_log_file = opts->rpc_log_file;
	g_spdk_app.rpc_log_level = opts->rpc_log_level;
	g_spdk_app.shm_id = opts->shm_id;
	g_spdk_app.shutdown_cb = opts->shutdown_cb;
	g_spdk_app.rc = 0;
	g_spdk_app.stopped = false;

	spdk_log_set_level(SPDK_APP_DEFAULT_LOG_LEVEL);

	/* Pass NULL to app_setup_env if SPDK app has been set up, in order to
	 * indicate that this is a reinitialization.
	 */
	if (app_setup_env(g_env_was_setup ? NULL : opts) < 0) {
		return 1;
	}

	/* Calculate mempool size now that the env layer has configured the core count
	 * for the application */
	calculate_mempool_size(opts, opts_user);

	spdk_log_open(opts->log);

	/* Initialize each lock to -1 to indicate "empty" status */
	for (i = 0; i < SPDK_CONFIG_MAX_LCORES; i++) {
		g_core_locks[i] = -1;
	}

	if (!opts->disable_cpumask_locks) {
		if (claim_cpu_cores(NULL)) {
			SPDK_ERRLOG("Unable to acquire lock on assigned core mask - exiting.\n");
			return 1;
		}
	} else {
		SPDK_NOTICELOG("CPU core locks deactivated.\n");
	}

	SPDK_NOTICELOG("Total cores available: %d\n", spdk_env_get_core_count());

	if ((rc = spdk_reactors_init(opts->msg_mempool_size)) != 0) {
		SPDK_ERRLOG("Reactor Initialization failed: rc = %d\n", rc);
		return 1;
	}

	spdk_cpuset_set_cpu(&tmp_cpumask, spdk_env_get_current_core(), true);

	/*
	 * Disable and ignore trace setup if setting num_entries
	 * to be 0.
	 *
	 * Note the call to app_setup_trace() is located here
	 * ahead of app_setup_signal_handlers().
	 * That's because there is not an easy/direct clean
	 * way of unwinding alloc'd resources that can occur
	 * in app_setup_signal_handlers().
	 */
	if (opts->num_entries != 0 && app_setup_trace(opts) != 0) {
		return 1;
	}

	/* Now that the reactors have been initialized, we can create the app thread. */
	spdk_thread_create("app_thread", &tmp_cpumask);
	if (!spdk_thread_get_app_thread()) {
		SPDK_ERRLOG("Unable to create an spdk_thread for initialization\n");
		return 1;
	}

	SPDK_ENV_FOREACH_CORE(core) {
		rc = init_proc_stat(core);
		if (rc) {
			SPDK_NOTICELOG("Unable to parse /proc/stat [core: %d].\n", core);
		}
	}

	if (!opts->disable_signal_handlers && app_setup_signal_handlers(opts) != 0) {
		return 1;
	}

	g_delay_subsystem_init = opts->delay_subsystem_init;
	g_start_fn = start_fn;
	g_start_arg = arg1;

	if (opts->json_config_file != NULL) {
		if (opts->json_data) {
			SPDK_ERRLOG("App opts json_config_file and json_data are mutually exclusive\n");
			return 1;
		}

		g_spdk_app.json_data = spdk_posix_file_load_from_name(opts->json_config_file,
				       &g_spdk_app.json_data_size);
		if (!g_spdk_app.json_data) {
			SPDK_ERRLOG("Read JSON configuration file %s failed: %s\n",
				    opts->json_config_file, spdk_strerror(errno));
			return 1;
		}
	} else if (opts->json_data) {
		g_spdk_app.json_data = calloc(1, opts->json_data_size);
		if (!g_spdk_app.json_data) {
			SPDK_ERRLOG("Failed to allocate JSON data buffer\n");
			return 1;
		}

		memcpy(g_spdk_app.json_data, opts->json_data, opts->json_data_size);
		g_spdk_app.json_data_size = opts->json_data_size;
	}

	spdk_thread_send_msg(spdk_thread_get_app_thread(), bootstrap_fn, NULL);

	/* This blocks until spdk_app_stop is called */
	spdk_reactors_start();

	g_env_was_setup = true;

	return g_spdk_app.rc;
}

void
spdk_app_fini(void)
{
	spdk_trace_cleanup();
	spdk_reactors_fini();
	spdk_env_fini();
	spdk_log_close();
	unclaim_cpu_cores(NULL);
}

static void
subsystem_fini_done(void *arg1)
{
	spdk_rpc_finish();
	spdk_reactors_stop(NULL);
}

static void
_start_subsystem_fini(void *arg1)
{
	if (g_scheduling_in_progress) {
		spdk_thread_send_msg(spdk_thread_get_app_thread(), _start_subsystem_fini, NULL);
		return;
	}

	spdk_subsystem_fini(subsystem_fini_done, NULL);
}

static int
log_deprecation_hits(void *ctx, struct spdk_deprecation *dep)
{
	uint64_t hits = spdk_deprecation_get_hits(dep);

	if (hits == 0) {
		return 0;
	}

	SPDK_WARNLOG("%s: deprecation '%s' scheduled for removal in %s hit %" PRIu64 " times\n",
		     spdk_deprecation_get_tag(dep), spdk_deprecation_get_description(dep),
		     spdk_deprecation_get_remove_release(dep), hits);
	return 0;
}

static void
app_stop(void *arg1)
{
	if (g_spdk_app.rc == 0) {
		g_spdk_app.rc = (int)(intptr_t)arg1;
	}

	if (g_spdk_app.stopped) {
		SPDK_NOTICELOG("spdk_app_stop called twice\n");
		return;
	}

	free(g_spdk_app.json_data);

	g_spdk_app.stopped = true;
	spdk_log_for_each_deprecation(NULL, log_deprecation_hits);
	_start_subsystem_fini(NULL);
}

void
spdk_app_stop(int rc)
{
	if (rc) {
		SPDK_WARNLOG("spdk_app_stop'd on non-zero\n");
	}

	/*
	 * We want to run spdk_subsystem_fini() from the same thread where spdk_subsystem_init()
	 * was called.
	 */
	spdk_thread_send_msg(spdk_thread_get_app_thread(), app_stop, (void *)(intptr_t)rc);
}

static void
usage_memory_size(void)
{
#ifndef __linux__
	if (g_default_opts.mem_size <= 0) {
		printf("all hugepage memory)\n");
	} else
#endif
	{
		printf("%dMB)\n", g_default_opts.mem_size >= 0 ? g_default_opts.mem_size : 0);
	}
}

static void
usage(void (*app_usage)(void))
{
	printf("%s [options]\n", g_executable_name);
	/* Keep entries inside categories roughly sorted by frequency of use. */
	printf("\nCPU options:\n");
	printf(" -m, --cpumask <mask or list>    core mask (like 0xF) or core list of '[]' embraced for DPDK\n");
	printf("                                 (like [0,1,10])\n");
	printf("     --lcores <list>       lcore to CPU mapping list. The list is in the format:\n");
	printf("                           <lcores[@CPUs]>[<,lcores[@CPUs]>...]\n");
	printf("                           lcores and cpus list are grouped by '(' and ')', e.g '--lcores \"(5-7)@(10-12)\"'\n");
	printf("                           Within the group, '-' is used for range separator,\n");
	printf("                           ',' is used for single number separator.\n");
	printf("                           '( )' can be omitted for single element group,\n");
	printf("                           '@' can be omitted if cpus and lcores have the same value\n");
	printf("     --disable-cpumask-locks    Disable CPU core lock files.\n");
	printf("     --interrupt-mode      set app to interrupt mode (Warning: CPU usage will be reduced only if all\n");
	printf("                           pollers in the app support interrupt mode)\n");
	printf(" -p, --main-core <id>      main (primary) core for DPDK\n");

	printf("\nConfiguration options:\n");
	printf(" -c, --config, --json  <config>     JSON config file\n");
	printf(" -r, --rpc-socket <path>   RPC listen address (default %s)\n", SPDK_DEFAULT_RPC_ADDR);
	printf("     --no-rpc-server       skip RPC server initialization. This option ignores '--rpc-socket' value.\n");
	printf("     --wait-for-rpc        wait for RPCs to initialize subsystems\n");
	printf("     --rpcs-allowed	   comma-separated list of permitted RPCS\n");
	printf("     --json-ignore-init-errors    don't exit on invalid config entry\n");

	printf("\nMemory options:\n");
	printf("     --iova-mode <pa/va>   set IOVA mode ('pa' for IOVA_PA and 'va' for IOVA_VA)\n");
	printf("     --base-virtaddr <addr>      the base virtual address for DPDK (default: 0x200000000000)\n");
	printf("     --huge-dir <path>     use a specific hugetlbfs mount to reserve memory from\n");
	printf(" -R, --huge-unlink         unlink huge files after initialization\n");
	printf(" -n, --mem-channels <num>  number of memory channels used for DPDK\n");
	printf(" -s, --mem-size <size>     memory size in MB for DPDK (default: ");
	usage_memory_size();
	printf("     --msg-mempool-size <size>  global message memory pool size in count (default: %d)\n",
	       SPDK_DEFAULT_MSG_MEMPOOL_SIZE);
	printf("     --no-huge             run without using hugepages\n");
	printf("     --enforce-numa        enforce NUMA allocations from the specified NUMA node\n");
	printf(" -i, --shm-id <id>         shared memory ID (optional)\n");
	printf(" -g, --single-file-segments   force creating just one hugetlbfs file\n");

	printf("\nPCI options:\n");
	printf(" -A, --pci-allowed <bdf>   pci addr to allow (-B and -A cannot be used at the same time)\n");
	printf(" -B, --pci-blocked <bdf>   pci addr to block (can be used more than once)\n");
	printf(" -u, --no-pci              disable PCI access\n");
	printf("     --vfio-vf-token       VF token (UUID) shared between SR-IOV PF and VFs for vfio_pci driver\n");

	printf("\nLog options:\n");
	spdk_log_usage(stdout, "-L");
	printf("     --silence-noticelog   disable notice level logging to stderr\n");

	printf("\nTrace options:\n");
	printf("     --num-trace-entries <num>   number of trace entries for each core, must be power of 2,\n");
	printf("                                 setting 0 to disable trace (default %d)\n",
	       SPDK_APP_DEFAULT_NUM_TRACE_ENTRIES);
	printf("                                 Tracepoints vary in size and can use more than one trace entry.\n");
	spdk_trace_mask_usage(stdout, "-e");

	printf("\nOther options:\n");
	printf(" -h, --help                show this usage\n");
	printf(" -v, --version             print SPDK version\n");
	printf(" -d, --limit-coredump      do not set max coredump size to RLIM_INFINITY\n");
	printf("     --env-context         Opaque context for use of the env implementation\n");

	if (app_usage) {
		printf("\nApplication specific:\n");
		app_usage();
	}
}

spdk_app_parse_args_rvals_t
spdk_app_parse_args(int argc, char **argv, struct spdk_app_opts *opts,
		    const char *app_getopt_str, const struct option *app_long_opts,
		    int (*app_parse)(int ch, char *arg),
		    void (*app_usage)(void))
{
	int ch, rc, opt_idx, global_long_opts_len, app_long_opts_len;
	struct option *cmdline_options;
	char *cmdline_short_opts = NULL;
	char *shm_id_str = NULL;
	enum spdk_app_parse_args_rvals retval = SPDK_APP_PARSE_ARGS_FAIL;
	long int tmp;

	memcpy(&g_default_opts, opts, sizeof(g_default_opts));

	if (opts->json_config_file && access(opts->json_config_file, R_OK) != 0) {
		SPDK_WARNLOG("Can't read JSON configuration file '%s'\n", opts->json_config_file);
		opts->json_config_file = NULL;
	}

	if (app_long_opts == NULL) {
		app_long_opts_len = 0;
	} else {
		for (app_long_opts_len = 0;
		     app_long_opts[app_long_opts_len].name != NULL;
		     app_long_opts_len++);
	}

	global_long_opts_len = SPDK_COUNTOF(g_cmdline_options);

	cmdline_options = calloc(global_long_opts_len + app_long_opts_len + 1, sizeof(*cmdline_options));
	if (!cmdline_options) {
		SPDK_ERRLOG("Out of memory\n");
		return SPDK_APP_PARSE_ARGS_FAIL;
	}

	memcpy(&cmdline_options[0], g_cmdline_options, sizeof(g_cmdline_options));
	if (app_long_opts) {
		memcpy(&cmdline_options[global_long_opts_len], app_long_opts,
		       app_long_opts_len * sizeof(*app_long_opts));
	}

	if (app_getopt_str != NULL) {
		ch = app_opts_validate(app_getopt_str);
		if (ch) {
			SPDK_ERRLOG("Duplicated option '%c' between app-specific command line parameter and generic spdk opts.\n",
				    ch);
			goto out;
		}

		if (!app_parse) {
			SPDK_ERRLOG("Parse function is required when app-specific command line parameters are provided.\n");
			goto out;
		}
	}

	cmdline_short_opts = spdk_sprintf_alloc("%s%s", app_getopt_str, SPDK_APP_GETOPT_STRING);
	if (!cmdline_short_opts) {
		SPDK_ERRLOG("Out of memory\n");
		goto out;
	}

	g_executable_name = argv[0];

	while ((ch = getopt_long(argc, argv, cmdline_short_opts, cmdline_options, &opt_idx)) != -1) {
		switch (ch) {
		case CONFIG_FILE_OPT_IDX:
		case JSON_CONFIG_OPT_IDX:
			opts->json_config_file = optarg;
			break;
		case JSON_CONFIG_IGNORE_INIT_ERRORS_IDX:
			opts->json_config_ignore_errors = true;
			break;
		case LIMIT_COREDUMP_OPT_IDX:
			opts->enable_coredump = false;
			break;
		case TPOINT_GROUP_OPT_IDX:
			opts->tpoint_group_mask = optarg;
			break;
		case SINGLE_FILE_SEGMENTS_OPT_IDX:
			opts->hugepage_single_segments = true;
			break;
		case HELP_OPT_IDX:
			usage(app_usage);
			retval = SPDK_APP_PARSE_ARGS_HELP;
			goto out;
		case SHM_ID_OPT_IDX:
			shm_id_str = optarg;
			/* a negative shm-id disables shared configuration file */
			if (optarg[0] == '-') {
				shm_id_str++;
			}
			/* check if the positive value of provided shm_id can be parsed as
			 * an integer
			 */
			opts->shm_id = spdk_strtol(shm_id_str, 0);
			if (opts->shm_id < 0) {
				SPDK_ERRLOG("Invalid shared memory ID %s\n", optarg);
				goto out;
			}
			if (optarg[0] == '-') {
				opts->shm_id = -opts->shm_id;
			}
			break;
		case CPUMASK_OPT_IDX:
			if (opts->lcore_map) {
				SPDK_ERRLOG("lcore map and core mask can't be set simultaneously\n");
				goto out;
			}
			opts->reactor_mask = optarg;
			break;
		case LCORES_OPT_IDX:
			if (opts->reactor_mask) {
				SPDK_ERRLOG("lcore map and core mask can't be set simultaneously\n");
				goto out;
			}
			opts->lcore_map = optarg;
			break;
		case DISABLE_CPUMASK_LOCKS_OPT_IDX:
			opts->disable_cpumask_locks = true;
			break;
		case MEM_CHANNELS_OPT_IDX:
			opts->mem_channel = spdk_strtol(optarg, 0);
			if (opts->mem_channel < 0) {
				SPDK_ERRLOG("Invalid memory channel %s\n", optarg);
				goto out;
			}
			break;
		case MAIN_CORE_OPT_IDX:
			opts->main_core = spdk_strtol(optarg, 0);
			if (opts->main_core < 0) {
				SPDK_ERRLOG("Invalid main core %s\n", optarg);
				goto out;
			}
			break;
		case SILENCE_NOTICELOG_OPT_IDX:
			opts->print_level = SPDK_LOG_WARN;
			break;
		case RPC_SOCKET_OPT_IDX:
			opts->rpc_addr = optarg;
			break;
		case NO_RPC_SERVER_OPT_IDX:
			opts->rpc_addr = NULL;
			break;
		case ENFORCE_NUMA_OPT_IDX:
			opts->enforce_numa = true;
			break;
		case MEM_SIZE_OPT_IDX: {
			uint64_t mem_size_mb;
			bool mem_size_has_prefix;

			rc = spdk_parse_capacity(optarg, &mem_size_mb, &mem_size_has_prefix);
			if (rc != 0) {
				SPDK_ERRLOG("invalid memory pool size `-s %s`\n", optarg);
				usage(app_usage);
				goto out;
			}

			if (mem_size_has_prefix) {
				/* the mem size is in MB by default, so if a prefix was
				 * specified, we need to manually convert to MB.
				 */
				mem_size_mb /= 1024 * 1024;
			}

			if (mem_size_mb > INT_MAX) {
				SPDK_ERRLOG("invalid memory pool size `-s %s`\n", optarg);
				usage(app_usage);
				goto out;
			}

			opts->mem_size = (int) mem_size_mb;
			break;
		}
		case MSG_MEMPOOL_SIZE_OPT_IDX:
			tmp = spdk_strtol(optarg, 10);
			if (tmp <= 0) {
				SPDK_ERRLOG("Invalid message memory pool size %s\n", optarg);
				goto out;
			}

			opts->msg_mempool_size = (size_t)tmp;
			break;

		case NO_PCI_OPT_IDX:
			opts->no_pci = true;
			break;
		case WAIT_FOR_RPC_OPT_IDX:
			opts->delay_subsystem_init = true;
			break;
		case PCI_BLOCKED_OPT_IDX:
			if (opts->pci_allowed) {
				free(opts->pci_allowed);
				opts->pci_allowed = NULL;
				SPDK_ERRLOG("-B and -A cannot be used at the same time\n");
				usage(app_usage);
				goto out;
			}

			rc = app_opts_add_pci_addr(opts, &opts->pci_blocked, optarg);
			if (rc != 0) {
				free(opts->pci_blocked);
				opts->pci_blocked = NULL;
				goto out;
			}
			break;

		case NO_HUGE_OPT_IDX:
			opts->no_huge = true;
			break;

		case LOGFLAG_OPT_IDX:
			rc = spdk_log_set_flag(optarg);
			if (rc < 0) {
				SPDK_ERRLOG("unknown flag: %s\n", optarg);
				usage(app_usage);
				goto out;
			}
#ifdef DEBUG
			opts->print_level = SPDK_LOG_DEBUG;
#endif
			break;
		case HUGE_UNLINK_OPT_IDX:
			opts->unlink_hugepage = true;
			break;
		case PCI_ALLOWED_OPT_IDX:
			if (opts->pci_blocked) {
				free(opts->pci_blocked);
				opts->pci_blocked = NULL;
				SPDK_ERRLOG("-B and -W cannot be used at the same time\n");
				usage(app_usage);
				goto out;
			}

			rc = app_opts_add_pci_addr(opts, &opts->pci_allowed, optarg);
			if (rc != 0) {
				free(opts->pci_allowed);
				opts->pci_allowed = NULL;
				goto out;
			}
			break;
		case BASE_VIRTADDR_OPT_IDX:
			tmp = spdk_strtoll(optarg, 0);
			if (tmp <= 0) {
				SPDK_ERRLOG("Invalid base-virtaddr %s\n", optarg);
				usage(app_usage);
				goto out;
			}
			opts->base_virtaddr = (uint64_t)tmp;
			break;
		case HUGE_DIR_OPT_IDX:
			opts->hugedir = optarg;
			break;
		case IOVA_MODE_OPT_IDX:
			opts->iova_mode = optarg;
			break;
		case NUM_TRACE_ENTRIES_OPT_IDX:
			tmp = spdk_strtoll(optarg, 0);
			if (tmp < 0) {
				SPDK_ERRLOG("Invalid num-trace-entries %s\n", optarg);
				usage(app_usage);
				goto out;
			}
			opts->num_entries = (uint64_t)tmp;
			if (opts->num_entries > 0 && opts->num_entries & (opts->num_entries - 1)) {
				SPDK_ERRLOG("num-trace-entries must be power of 2\n");
				usage(app_usage);
				goto out;
			}
			break;
		case ENV_CONTEXT_OPT_IDX:
			opts->env_context = optarg;
			break;
		case RPCS_ALLOWED_OPT_IDX:
			opts->rpc_allowlist = (const char **)spdk_strarray_from_string(optarg, ",");
			if (opts->rpc_allowlist == NULL) {
				SPDK_ERRLOG("Invalid --rpcs-allowed argument\n");
				usage(app_usage);
				goto out;
			}
			break;
		case ENV_VF_TOKEN_OPT_IDX:
			opts->vf_token = optarg;
			break;
		case INTERRUPT_MODE_OPT_IDX:
			opts->interrupt_mode = true;
			break;
		case VERSION_OPT_IDX:
			printf(SPDK_VERSION_STRING"\n");
			retval = SPDK_APP_PARSE_ARGS_HELP;
			goto out;
		case '?':
			/*
			 * In the event getopt() above detects an option
			 * in argv that is NOT in the getopt_str,
			 * getopt() will return a '?' indicating failure.
			 */
			usage(app_usage);
			goto out;
		default:
			if (!app_parse) {
				SPDK_ERRLOG("Unsupported app-specific command line parameter '%c'.\n", ch);
				goto out;
			}

			rc = app_parse(ch, optarg);
			if (rc) {
				SPDK_ERRLOG("Parsing app-specific command line parameter '%c' failed: %d\n", ch, rc);
				goto out;
			}
		}
	}

	retval = SPDK_APP_PARSE_ARGS_SUCCESS;
out:
	if (retval != SPDK_APP_PARSE_ARGS_SUCCESS) {
		free(opts->pci_blocked);
		opts->pci_blocked = NULL;
		free(opts->pci_allowed);
		opts->pci_allowed = NULL;
		spdk_strarray_free((char **)opts->rpc_allowlist);
		opts->rpc_allowlist = NULL;
	}
	free(cmdline_short_opts);
	free(cmdline_options);
	return retval;
}

void
spdk_app_usage(void)
{
	if (g_executable_name == NULL) {
		SPDK_ERRLOG("%s not valid before calling spdk_app_parse_args()\n", __func__);
		return;
	}

	usage(NULL);
}

static void
rpc_framework_start_init_cpl(int rc, void *arg1)
{
	struct spdk_jsonrpc_request *request = arg1;

	assert(spdk_thread_is_app_thread(NULL));

	if (rc) {
		if (g_spdk_app.rpc_addr) {
			spdk_rpc_server_resume(g_spdk_app.rpc_addr);
		}
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "framework_initialization failed");

		app_subsystem_init_done(rc, NULL);
		return;
	}

	app_subsystem_init_done(0, NULL);

	spdk_jsonrpc_send_bool_response(request, true);
}

static void
rpc_framework_start_init(struct spdk_jsonrpc_request *request,
			 const struct spdk_json_val *params)
{
	if (params != NULL) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "framework_start_init requires no parameters");
		return;
	}

	spdk_rpc_server_pause(g_spdk_app.rpc_addr);
	spdk_subsystem_init(rpc_framework_start_init_cpl, request);
}
SPDK_RPC_REGISTER("framework_start_init", rpc_framework_start_init, SPDK_RPC_STARTUP)

struct subsystem_init_poller_ctx {
	struct spdk_poller *init_poller;
	struct spdk_jsonrpc_request *request;
};

static int
rpc_subsystem_init_poller_ctx(void *ctx)
{
	struct subsystem_init_poller_ctx *poller_ctx = ctx;

	if (spdk_rpc_get_state() == SPDK_RPC_RUNTIME) {
		spdk_jsonrpc_send_bool_response(poller_ctx->request, true);
		spdk_poller_unregister(&poller_ctx->init_poller);
		free(poller_ctx);
	}

	return SPDK_POLLER_BUSY;
}

static void
rpc_framework_wait_init(struct spdk_jsonrpc_request *request,
			const struct spdk_json_val *params)
{
	struct subsystem_init_poller_ctx *ctx;

	if (spdk_rpc_get_state() == SPDK_RPC_RUNTIME) {
		spdk_jsonrpc_send_bool_response(request, true);
	} else {
		ctx = malloc(sizeof(struct subsystem_init_poller_ctx));
		if (ctx == NULL) {
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
							 "Unable to allocate memory for the request context\n");
			return;
		}
		ctx->request = request;
		ctx->init_poller = SPDK_POLLER_REGISTER(rpc_subsystem_init_poller_ctx, ctx, 0);
	}
}
SPDK_RPC_REGISTER("framework_wait_init", rpc_framework_wait_init,
		  SPDK_RPC_STARTUP | SPDK_RPC_RUNTIME)

static void
rpc_framework_disable_cpumask_locks(struct spdk_jsonrpc_request *request,
				    const struct spdk_json_val *params)
{
	char msg[128];
	int rc;
	uint32_t failed_core;

	if (params != NULL) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "framework_disable_cpumask_locks"
						 "requires no arguments");
		return;
	}

	rc = unclaim_cpu_cores(&failed_core);
	if (rc) {
		snprintf(msg, sizeof(msg), "Failed to unclaim CPU core: %" PRIu32, failed_core);
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, msg);
		return;
	}

	spdk_jsonrpc_send_bool_response(request, true);
}
SPDK_RPC_REGISTER("framework_disable_cpumask_locks", rpc_framework_disable_cpumask_locks,
		  SPDK_RPC_STARTUP | SPDK_RPC_RUNTIME)

static void
rpc_framework_enable_cpumask_locks(struct spdk_jsonrpc_request *request,
				   const struct spdk_json_val *params)
{
	char msg[128];
	int rc;
	uint32_t failed_core;

	if (params != NULL) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "framework_enable_cpumask_locks"
						 "requires no arguments");
		return;
	}

	rc = claim_cpu_cores(&failed_core);
	if (rc) {
		snprintf(msg, sizeof(msg), "Failed to claim CPU core: %" PRIu32, failed_core);
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, msg);
		return;
	}

	spdk_jsonrpc_send_bool_response(request, true);
}
SPDK_RPC_REGISTER("framework_enable_cpumask_locks", rpc_framework_enable_cpumask_locks,
		  SPDK_RPC_STARTUP | SPDK_RPC_RUNTIME)
