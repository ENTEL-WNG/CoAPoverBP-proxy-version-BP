// SPDX-License-Identifier: BSD-3-Clause OR Apache-2.0
#include "ud3tn/agent_manager.h"
#include "ud3tn/bundle_processor.h"
#include "ud3tn/cmdline.h"
#include "ud3tn/common.h"
#include "ud3tn/init.h"

#include "aap2/aap2_agent.h"

#include "agents/application_agent.h"
#include "agents/echo_agent.h"

#include "cla/cla.h"

#include "routing/compat/router_agent.h"

#include "platform/hal_io.h"
#include "platform/hal_platform.h"
#include "platform/hal_queue.h"
#include "platform/hal_task.h"

#include <stdbool.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

static struct bundle_agent_interface bundle_agent_interface;
uint8_t LOG_LEVEL = DEFAULT_LOG_LEVEL;

// References kept for program runtime
static struct application_agent_config *aa_cfg;
static struct aap2_agent_config *aa2_cfg;

// Whether the uD3TN tasks were launched via start_tasks.
static bool daemon_mode;

void init(int argc, char *argv[])
{
	hal_platform_init(argc, argv);
	LOG_INFO("INIT: uD3TN starting up...");
}

void start_tasks(const struct ud3tn_cmdline_options *const opt)
{
	if (!opt) {
		LOG_ERROR("INIT: Error parsing options, terminating...");
		exit(EXIT_FAILURE);
	}

	if (opt->exit_immediately)
		exit(EXIT_SUCCESS);

	LOG_LEVEL = opt->log_level;

	LOGF_INFO("INIT: Configured to use EID \"%s\" and BPv%d",
	     opt->eid, opt->bundle_version);

	bundle_agent_interface.local_eid = opt->eid;

	/* Initialize queues to communicate with the subsystems */
	bundle_agent_interface.bundle_signaling_queue = hal_queue_create(
		BUNDLE_QUEUE_LENGTH,
		sizeof(struct bundle_processor_signal)
	);
	if (!bundle_agent_interface.bundle_signaling_queue) {
		LOG_ERROR("INIT: Allocation of `bundle_signaling_queue` failed");
		abort();
	}

	// NOTE: Must be called before launching the BP which calls the function
	// to register agents from its thread.
	struct agent_manager *am = agent_manager_create(
		bundle_agent_interface.local_eid,
		opt->aap2_bdm_secret
	);

	if (!am) {
		LOG_ERROR("INIT: Agent Manager could not be initialized!");
		abort();
	}

	struct bundle_processor_task_parameters *bundle_processor_task_params =
		malloc(sizeof(struct bundle_processor_task_parameters));

	if (!bundle_processor_task_params) {
		LOG_ERROR("INIT: Allocation of `bundle_processor_task_params` failed");
		abort();
	}
	bundle_processor_task_params->signaling_queue =
			bundle_agent_interface.bundle_signaling_queue;
	bundle_processor_task_params->local_eid =
			bundle_agent_interface.local_eid;
	bundle_processor_task_params->maximum_bundle_size_bytes = opt->mbs;
	bundle_processor_task_params->status_reporting =
			opt->status_reporting;
	bundle_processor_task_params->allow_remote_configuration =
			opt->allow_remote_configuration;
	bundle_processor_task_params->agent_manager = am;

	const enum ud3tn_result bp_task_result = hal_task_create(
		bundle_processor_task,
		bundle_processor_task_params,
		true,
		NULL
	);

	if (bp_task_result != UD3TN_OK) {
		LOG_ERROR("INIT: Bundle processor task could not be started!");
		abort();
	}

	int result = echo_agent_setup(
		&bundle_agent_interface,
		opt->lifetime_s * 1000
	);

	if (result) {
		LOG_ERROR("INIT: Echo agent could not be initialized!");
		abort();
	}

	if (opt->allow_remote_configuration)
		LOG_WARN("!! WARNING !! Remote configuration capability ENABLED!");

	/* Initialize the communication subsystem (CLA) */
	if (cla_initialize_all(opt->cla_options,
			       &bundle_agent_interface) != UD3TN_OK) {
		LOG_ERROR("INIT: CLA subsystem could not be initialized!");
		abort();
	}

	aa_cfg = application_agent_setup(
		&bundle_agent_interface,
		opt->aap_socket,
		opt->aap_node,
		opt->aap_service,
		opt->bundle_version,
		opt->lifetime_s * 1000,
		// For compatibility reasons, we allow administrative actions
		// (such as contact config.) if either the "allow remote
		// configuration" flag is set or no BDM secret is set.
		opt->allow_remote_configuration || opt->aap2_bdm_secret == NULL
	);

	if (!aa_cfg) {
		LOG_ERROR("INIT: Application agent could not be initialized!");
		abort();
	}

	if (opt->aap_node && opt->aap_service)
		LOG_INFO("INIT: AAP TCP socket configured: Make sure that it is not exposed externally!");

	uint64_t max_bundle_size_bytes = cla_get_max_bundle_size_bytes();

	if (opt->mbs != 0 && (max_bundle_size_bytes == 0 ||
			      opt->mbs < max_bundle_size_bytes))
		max_bundle_size_bytes = opt->mbs;

	if (max_bundle_size_bytes != 0)
		LOGF_INFO("INIT: Limiting maximum bundle size to %llu bytes.",
			  max_bundle_size_bytes);

	aa2_cfg = aap2_agent_setup(
		&bundle_agent_interface,
		opt->aap2_socket,
		opt->aap2_node,
		opt->aap2_service,
		opt->bundle_version,
		opt->lifetime_s * 1000,
		max_bundle_size_bytes
	);

	if (!aa2_cfg) {
		LOG_ERROR("INIT: AAP2 agent could not be initialized!");
		abort();
	}

	if (opt->aap2_node && opt->aap2_service)
		LOG_INFO("INIT: AAP 2.0 TCP socket configured: Make sure that it is not exposed externally!");

	if (!opt->external_dispatch) {
		const int cra_rv = compat_router_agent_setup(
			&bundle_agent_interface,
			opt->allow_remote_configuration,
			opt->aap2_bdm_secret
		);

		if (cra_rv) {
			LOG_ERROR("INIT: Router could not be initialized!");
			abort();
		}
	}

	daemon_mode = true;
}

enum ud3tn_result start_os(void)
{
	enum ud3tn_result result = hal_task_start_scheduler();

	if (daemon_mode) {
		aap2_agent_terminate(aa2_cfg);
		cla_terminate_all();
	}
	return result;
}
