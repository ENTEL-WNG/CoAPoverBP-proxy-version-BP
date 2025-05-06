// SPDX-License-Identifier: BSD-3-Clause OR Apache-2.0
#ifndef RTCOMPAT_ROUTER_AGENT_H_INCLUDED
#define RTCOMPAT_ROUTER_AGENT_H_INCLUDED

#include "ud3tn/bundle_processor.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Compat. router: Agent ID of the config. agent if using the dtn scheme.
#ifndef ROUTER_AGENT_ID_CONFIG_DTN
#define ROUTER_AGENT_ID_CONFIG_DTN "config"
#endif // ROUTER_AGENT_ID_CONFIG_DTN

// Compat. router: Agent ID of the config. agent if using the ipn scheme.
#ifndef ROUTER_AGENT_ID_CONFIG_IPN
#define ROUTER_AGENT_ID_CONFIG_IPN "9000"
#endif // ROUTER_AGENT_ID_CONFIG_IPN

int compat_router_agent_setup(
	const struct bundle_agent_interface *bundle_agent_interface,
	bool allow_remote_configuration,
	const char *aap2_admin_secret);

#endif // RTCOMPAT_ROUTER_AGENT_H_INCLUDED
