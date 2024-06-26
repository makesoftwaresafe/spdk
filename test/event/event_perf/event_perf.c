/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"

#include "spdk/env.h"
#include "spdk/event.h"
#include "spdk/log.h"
#include "spdk/string.h"

static uint64_t g_tsc_rate;
static uint64_t g_tsc_end;

static int g_time_in_sec;

static uint64_t *call_count;

static bool g_app_stopped = false;

static void
submit_new_event(void *arg1, void *arg2)
{
	struct spdk_event *event;
	static __thread uint32_t next_lcore = UINT32_MAX;

	if (spdk_get_ticks() > g_tsc_end) {
		if (__sync_bool_compare_and_swap(&g_app_stopped, false, true)) {
			spdk_app_stop(0);
		}
		return;
	}

	if (next_lcore == UINT32_MAX) {
		next_lcore = spdk_env_get_next_core(spdk_env_get_current_core());
		if (next_lcore == UINT32_MAX) {
			next_lcore = spdk_env_get_first_core();
		}
	}

	call_count[next_lcore]++;
	event = spdk_event_allocate(next_lcore, submit_new_event, NULL, NULL);
	spdk_event_call(event);
}

static void
event_work_fn(void *arg1, void *arg2)
{

	submit_new_event(NULL, NULL);
	submit_new_event(NULL, NULL);
	submit_new_event(NULL, NULL);
	submit_new_event(NULL, NULL);
}

static void
event_perf_start(void *arg1)
{
	uint32_t i;

	call_count = calloc(spdk_env_get_last_core() + 1, sizeof(*call_count));
	if (call_count == NULL) {
		fprintf(stderr, "call_count allocation failed\n");
		spdk_app_stop(1);
		return;
	}

	g_tsc_rate = spdk_get_ticks_hz();
	g_tsc_end = spdk_get_ticks() + g_time_in_sec * g_tsc_rate;

	printf("Running I/O for %d seconds...", g_time_in_sec);
	fflush(stdout);

	SPDK_ENV_FOREACH_CORE(i) {
		spdk_event_call(spdk_event_allocate(i, event_work_fn,
						    NULL, NULL));
	}

}

static void
usage(char *program_name)
{
	printf("%s options\n", program_name);
	printf("\t[-m core mask for distributing I/O submission/completion work\n");
	printf("\t\t(default: 0x1 - use core 0 only)]\n");
	printf("\t[-t time in seconds]\n");
}

static void
performance_dump(int io_time)
{
	uint32_t i;

	if (call_count == NULL) {
		return;
	}

	printf("\n");
	SPDK_ENV_FOREACH_CORE(i) {
		printf("lcore %2d: %8ju\n", i, call_count[i] / g_time_in_sec);
	}

	fflush(stdout);
	free(call_count);
}

int
main(int argc, char **argv)
{
	struct spdk_app_opts opts = {};
	int op;
	int rc = 0;

	spdk_app_opts_init(&opts, sizeof(opts));
	opts.name = "event_perf";
	opts.rpc_addr = NULL;

	g_time_in_sec = 0;

	while ((op = getopt(argc, argv, "m:t:")) != -1) {
		switch (op) {
		case 'm':
			opts.reactor_mask = optarg;
			break;
		case 't':
			g_time_in_sec = spdk_strtol(optarg, 10);
			if (g_time_in_sec < 0) {
				fprintf(stderr, "Invalid run time\n");
				return g_time_in_sec;
			}
			break;
		default:
			usage(argv[0]);
			exit(1);
		}
	}

	if (!g_time_in_sec) {
		usage(argv[0]);
		exit(1);
	}

	printf("Running I/O for %d seconds...", g_time_in_sec);
	fflush(stdout);

	rc = spdk_app_start(&opts, event_perf_start, NULL);

	spdk_app_fini();
	performance_dump(g_time_in_sec);

	printf("done.\n");
	return rc;
}
