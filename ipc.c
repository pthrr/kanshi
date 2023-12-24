#define _POSIX_C_SOURCE 200809L
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <varlink.h>

#include "config.h"
#include "kanshi.h"
#include "ipc.h"

static long reply_error(VarlinkCall *call, const char *name) {
	VarlinkObject *params = NULL;
	long ret = varlink_object_new(&params);
	if (ret < 0) {
		return ret;
	}
	ret = varlink_call_reply_error(call, name, params);
	varlink_object_unref(params);
	return ret;
}

static void apply_profile_done(void *data, bool success) {
	VarlinkCall *call = data;
	if (success) {
		varlink_call_reply(call, NULL, 0);
	} else {
		reply_error(call, "fr.emersion.kanshi.ProfileNotApplied");
	}
}

static long handle_reload(VarlinkService *service, VarlinkCall *call,
		VarlinkObject *parameters, uint64_t flags, void *userdata) {
	struct kanshi_state *state = userdata;
	if (!kanshi_reload_config(state, apply_profile_done, call)) {
		return reply_error(call, "fr.emersion.kanshi.ProfileNotMatched");
	}
	return 0;
}

static long handle_switch(VarlinkService *service, VarlinkCall *call,
		VarlinkObject *parameters, uint64_t flags, void *userdata) {
	struct kanshi_state *state = userdata;

	const char *profile_name;
	if (varlink_object_get_string(parameters, "profile", &profile_name) < 0) {
		return varlink_call_reply_invalid_parameter(call, "profile");
	}

	struct kanshi_profile *profile;
	bool found = false;
	wl_list_for_each(profile, &state->config->profiles, link) {
		if (strcmp(profile->name, profile_name) == 0) {
			found = true;
			break;
		}
	}
	if (!found) {
		return reply_error(call, "fr.emersion.kanshi.ProfileNotFound");
	}

	if (!kanshi_switch(state, profile, apply_profile_done, call)) {
		return reply_error(call, "fr.emersion.kanshi.ProfileNotMatched");
	}

	return 0;
}

static int set_cloexec(int fd) {
	int flags = fcntl(fd, F_GETFD);
	if (flags < 0) {
		perror("fnctl(F_GETFD) failed");
		return -1;
	}
	if (fcntl(fd, F_SETFD, flags | O_CLOEXEC) < 0) {
		perror("fnctl(F_SETFD) failed");
		return -1;
	}
	return 0;
}

int kanshi_init_ipc(struct kanshi_state *state, int listen_fd) {
	if (listen_fd >= 0 && set_cloexec(listen_fd) < 0) {
		return -1;
	}

	VarlinkService *service;
	char address[PATH_MAX];
	if (get_ipc_address(address, sizeof(address)) < 0) {
		return -1;
	}
	if (varlink_service_new(&service,
			"emersion", "kanshi", KANSHI_VERSION, "https://wayland.emersion.fr/kanshi/",
			address, listen_fd) < 0) {
		fprintf(stderr, "Couldn't start kanshi varlink service at %s.\n"
				"Is the kanshi daemon already running?\n", address);
		return -1;
	}

	const char *interface = "interface fr.emersion.kanshi\n"
		"method Reload() -> ()\n"
		"method Switch(profile: string) -> ()\n"
		"error ProfileNotFound()\n"
		"error ProfileNotMatched()\n"
		"error ProfileNotApplied()\n";

	long result = varlink_service_add_interface(service, interface,
			"Reload", handle_reload, state,
			"Switch", handle_switch, state,
			NULL);
	if (result != 0) {
		fprintf(stderr, "varlink_service_add_interface failed: %s\n",
				varlink_error_string(-result));
		varlink_service_free(service);
		return -1;
	}

	state->service = service;

	return 0;
}

void kanshi_free_ipc(struct kanshi_state *state) {
	if (state->service) {
		varlink_service_free(state->service);
		state->service = NULL;
	}
}
