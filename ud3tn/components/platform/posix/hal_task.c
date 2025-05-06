// SPDX-License-Identifier: BSD-3-Clause OR Apache-2.0
/*
 * hal_task.c
 *
 * Description: contains the POSIX implementation of the hardware
 * abstraction layer interface for thread-related functionality
 *
 */

#include "ud3tn/common.h"
#include "ud3tn/result.h"

#include "platform/hal_io.h"
#include "platform/hal_task.h"
#include "platform/hal_time.h"
#include "platform/hal_types.h"

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>

struct task_description {
	void (*task_function)(void *param);
	void *task_parameter;
};

static void *execute_pthread_compat(void *task_description)
{
	struct task_description *desc =
		(struct task_description *)task_description;
	void (*task_function)(void *param) = desc->task_function;
	void *task_parameter = desc->task_parameter;

	free(task_description);
	task_function(task_parameter);
	return NULL;
}

enum ud3tn_result hal_task_create(
	void (*task_function)(void *),
	void *task_parameters,
	bool detach,
	TaskIdentifier_t *task_id)
{
	pthread_t thread_id;
	pthread_attr_t tattr;
	int error_code;
	struct task_description *desc = malloc(sizeof(*desc));

	if (desc == NULL) {
		LOG_ERROR("Allocating the task attribute structure failed!");
		goto fail;
	}

	/* initialize an attribute to the default value */
	if (pthread_attr_init(&tattr)) {
		/* abort if error occurs */
		LOG_ERROR("Initializing the task's attributes failed!");
		goto fail;
	}

	/* Create thread in detached state, so that no cleanup is necessary */
	if (detach) {
		if (pthread_attr_setdetachstate(&tattr, PTHREAD_CREATE_DETACHED)) {
			LOG_ERROR("Setting detached state failed!");
			goto fail_attr;
		}
	} else if (!task_id) {
		LOG_ERROR("Caller must handle task ID when creating non-detached tasks but it is set to NULL");
		goto fail_attr;
	}

	desc->task_function = task_function;
	desc->task_parameter = task_parameters;

	error_code = pthread_create(&thread_id, &tattr,
				    execute_pthread_compat, desc);

	if (error_code) {
		LOG_ERROR("Thread Creation failed!");
		goto fail_attr;
	}

	/* destroy the attr-object */
	pthread_attr_destroy(&tattr);

	if (task_id) {
		/* Set the task_id to thread_id to return a handle for a non-detached thread */
		if (!detach)
			*task_id = (TaskIdentifier_t)thread_id;
		/* Set the task_id to NULL for detached thread */
		else
			*task_id = (TaskIdentifier_t)NULL;
	}

	return UD3TN_OK;

fail_attr:
	/* destroy the attr-object */
	pthread_attr_destroy(&tattr);
fail:
	free(desc);

	return UD3TN_FAIL;
}

enum ud3tn_result hal_task_wait(TaskIdentifier_t task_id)
{
	void *result;
	int rv = pthread_join(task_id, &result);

	return rv ? UD3TN_FAIL : UD3TN_OK;
}

enum ud3tn_result hal_task_start_scheduler(void)
{
	int sig;
	sigset_t signal_set;

	// Ignore SIGPIPE so uD3TN does not crash if a connection is closed
	// during sending data. The event will be reported to us by the result
	// of the send(...) call.
	signal(SIGPIPE, SIG_IGN);
	// Ignore user defined signals (SIGUSER1, SIGUSR2)
	signal(SIGUSR1, SIG_IGN);
	signal(SIGUSR2, SIG_IGN);

	if (sigemptyset(&signal_set) == -1) {
		LOG_ERRNO("HAL", "sigemptyset()", errno);
		return UD3TN_FAIL;
	}
	// SIGINT is only caught in release builds to ensure debuggability.
	// Note that debug builds will not exit cleanly on SIGINT!
	if (!IS_DEBUG_BUILD && sigaddset(&signal_set, SIGINT) == -1) {
		LOG_ERRNO("HAL", "sigaddset(SIGINT)", errno);
		return UD3TN_FAIL;
	}
	if (sigaddset(&signal_set, SIGTERM) == -1) {
		LOG_ERRNO("HAL", "sigaddset(SIGTERM)", errno);
		return UD3TN_FAIL;
	}
	if (sigaddset(&signal_set, SIGHUP) == -1) {
		LOG_ERRNO("HAL", "sigaddset(SIGHUP)", errno);
		return UD3TN_FAIL;
	}
	if (sigaddset(&signal_set, SIGALRM) == -1) {
		LOG_ERRNO("HAL", "sigaddset(SIGALRM)", errno);
		return UD3TN_FAIL;
	}

	if (pthread_sigmask(SIG_BLOCK, &signal_set, NULL) == -1) {
		LOG_ERRNO("HAL", "pthread_sigmask()", errno);
		return UD3TN_FAIL;
	}

	if (sigwait(&signal_set, &sig) == -1) {
		LOG_ERRNO("HAL", "sigwait()", errno);
		return UD3TN_FAIL;
	}

	if (sig == SIGHUP)
		LOG_INFO("SIGHUP detected, terminating");
	if (sig == SIGINT)
		LOG_INFO("SIGINT detected, terminating");
	if (sig == SIGTERM)
		LOG_INFO("SIGTERM detected, terminating");
	if (sig == SIGALRM)
		LOG_INFO("SIGALRM detected, terminating");

	return UD3TN_OK;
}


void hal_task_delay(int delay)
{
	if (delay < 0)
		return;

	struct timespec req = {
		.tv_sec = delay / 1000,
		.tv_nsec = (delay % 1000) * 1000000
	};
	struct timespec rem;

	while (nanosleep(&req, &rem)) {
		if (errno != EINTR)
			break;
		req = rem;
	}
}
