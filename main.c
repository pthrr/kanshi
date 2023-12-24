#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wayland-client.h>

#include "config.h"
#include "kanshi.h"
#include "parser.h"
#include "ipc.h"
#include "wlr-output-management-unstable-v1-client-protocol.h"

#define HEADS_MAX 64

static bool match_and_apply(struct kanshi_state *state,
	kanshi_apply_done_func callback, void *data);

static bool match_profile_output(struct kanshi_profile_output *output,
		struct kanshi_head *head) {
	const char *make = head->make ? head->make : "Unknown";
	const char *model = head->model ? head->model : "Unknown";
	const char *serial_number =
		head->serial_number ? head->serial_number : "Unknown";

	char identifier[1024];
	assert(sizeof(identifier) >= strlen(make) + strlen(model) + strlen(serial_number) + 3);
	snprintf(identifier, sizeof(identifier), "%s %s %s", make, model, serial_number);

	return strcmp(output->name, "*") == 0 ||
		strcmp(output->name, head->name) == 0 ||
		strcmp(output->name, identifier) == 0;
}

static bool match_profile(struct kanshi_state *state,
		struct kanshi_profile *profile,
		struct kanshi_profile_output *matches[static HEADS_MAX]) {
	if (wl_list_length(&profile->outputs) != wl_list_length(&state->heads)) {
		return false;
	}

	memset(matches, 0, HEADS_MAX * sizeof(struct kanshi_head *));

	// Wildcards are stored at the end of the list, so those will be matched
	// last
	struct kanshi_profile_output *profile_output;
	wl_list_for_each(profile_output, &profile->outputs, link) {
		bool output_matched = false;
		ssize_t i = -1;
		struct kanshi_head *head;
		wl_list_for_each(head, &state->heads, link) {
			i++;

			if (matches[i] != NULL) {
				continue; // already matched
			}

			if (match_profile_output(profile_output, head)) {
				matches[i] = profile_output;
				output_matched = true;
				break;
			}
		}

		if (!output_matched) {
			return false;
		}
	}

	return true;
}

static struct kanshi_profile *match(struct kanshi_state *state,
		struct kanshi_profile_output *matches[static HEADS_MAX]) {
	struct kanshi_profile *profile;
	wl_list_for_each(profile, &state->config->profiles, link) {
		if (match_profile(state, profile, matches)) {
			return profile;
		}
	}
	return NULL;
}


static void exec_command(char *cmd) {
	pid_t child, grandchild;
	// Fork process
	if ((child = fork()) == 0) {
		// Fork child process again so we can unparent the process
		setsid();
		sigset_t set;
		sigemptyset(&set);
		sigprocmask(SIG_SETMASK, &set, NULL);

		struct sigaction action;
		sigfillset(&action.sa_mask);
		action.sa_flags = 0;
		action.sa_handler = SIG_DFL;
		sigaction(SIGINT, &action, NULL);
		sigaction(SIGQUIT, &action, NULL);
		sigaction(SIGTERM, &action, NULL);
		sigaction(SIGHUP, &action, NULL);

		if ((grandchild = fork()) == 0) {
			execl("/bin/sh", "/bin/sh", "-c", cmd, (void *)NULL);
			fprintf(stderr, "Executing command '%s' failed: %s\n", cmd, strerror(errno));
			_exit(-1);
		}
		if (grandchild < 0) {
			fprintf(stderr, "Impossible to fork a new process to execute"
					" command '%s': %s\n", cmd, strerror(errno));
			_exit(1);
		}
		_exit(0); // Close child process
	}

	if (child < 0) {
		perror("Impossible to fork a new process");
		return;
	}

	// cleanup child process
	if (waitpid(child, NULL, 0) < 0) {
		perror("Impossible to clean up child process");
	}
}

static void config_handle_succeeded(void *data,
		struct zwlr_output_configuration_v1 *config) {
	struct kanshi_pending_profile *pending = data;
	zwlr_output_configuration_v1_destroy(config);

	struct kanshi_state *state = pending->state;
	struct kanshi_profile *profile = pending->profile;

	struct kanshi_profile_command *command;
	wl_list_for_each(command, &profile->commands, link) {
		fprintf(stderr, "running command '%s'\n", command->command);
		exec_command(command->command);
	}

	fprintf(stderr, "configuration for profile '%s' applied\n", profile->name);
	state->current_profile = profile;
	if (profile == state->pending_profile) {
		state->pending_profile = NULL;
	}
	if (pending->callback != NULL) {
		pending->callback(pending->callback_data, true);
	}
	free(pending);
}

static void config_handle_failed(void *data,
		struct zwlr_output_configuration_v1 *config) {
	struct kanshi_pending_profile *pending = data;
	zwlr_output_configuration_v1_destroy(config);
	fprintf(stderr, "failed to apply configuration for profile '%s'\n",
			pending->profile->name);
	if (pending->profile == pending->state->pending_profile) {
		pending->state->pending_profile = NULL;
	}
	if (pending->callback != NULL) {
		pending->callback(pending->callback_data, false);
	}
	free(pending);
}

static void config_handle_cancelled(void *data,
		struct zwlr_output_configuration_v1 *config) {
	struct kanshi_pending_profile *pending = data;
	zwlr_output_configuration_v1_destroy(config);
	// Wait for new serial
	fprintf(stderr, "configuration for profile '%s' cancelled, retrying\n",
			pending->profile->name);
	if (pending->profile == pending->state->pending_profile) {
		pending->state->pending_profile = NULL;
	}
	if (pending->serial != pending->state->serial) {
		// We've already received a new serial, try re-applying the profile
		// immediately
		match_and_apply(pending->state, NULL, NULL);
	}
	if (pending->callback != NULL) {
		pending->callback(pending->callback_data, false);
	}
	free(pending);
}

static const struct zwlr_output_configuration_v1_listener config_listener = {
	.succeeded = config_handle_succeeded,
	.failed = config_handle_failed,
	.cancelled = config_handle_cancelled,
};

static bool match_refresh(const struct kanshi_mode *mode, int refresh, int *delta) {
	int v = refresh - mode->refresh;
	int mode_delta = abs(v);
	/* If we have a refresh, pick one with the lowest delta from our target.
	 * Doing a simple fuzzy match that picks the greatest (due to ordering) here can lead us to picking a refresh
	 * such as 120.01 or 60.01, which is problematic for two reasons:
	 *  - Modes such as 4K 120.01Hz is too much for link bandwidth of DP 1.4 without DSC.
	 *  - It becomes out of phase with the majority of content being displayed.
	 */
	if (mode_delta < 50 && mode_delta < *delta) {
		*delta = mode_delta;
		return true;
	}
	return false;
}

static struct kanshi_mode *match_mode(struct kanshi_head *head,
		int width, int height, int refresh) {
	struct kanshi_mode *mode;
	struct kanshi_mode *last_match = NULL;
	int mode_delta = INT32_MAX;

	wl_list_for_each(mode, &head->modes, link) {
		if (mode->width != width || mode->height != height) {
			continue;
		}

		if (refresh) {
			if (match_refresh(mode, refresh, &mode_delta)) {
				last_match = mode;
			}
		} else {
			if (!last_match || mode->refresh > last_match->refresh) {
				last_match = mode;
			}
		}
	}

	return last_match;
}

static bool apply_profile(struct kanshi_state *state,
		struct kanshi_profile *profile, struct kanshi_profile_output **matches,
		kanshi_apply_done_func callback, void *data) {
	if (state->pending_profile == profile || state->current_profile == profile) {
		if (callback != NULL) {
			callback(data, true);
		}
		return true;
	}

	fprintf(stderr, "applying profile '%s'\n", profile->name);

	struct kanshi_pending_profile *pending = calloc(1, sizeof(*pending));
	pending->serial = state->serial;
	pending->state = state;
	pending->profile = profile;
	pending->callback = callback;
	pending->callback_data = data;
	state->pending_profile = profile;

	struct zwlr_output_configuration_v1 *config =
		zwlr_output_manager_v1_create_configuration(state->output_manager,
		state->serial);
	zwlr_output_configuration_v1_add_listener(config, &config_listener, pending);

	ssize_t i = -1;
	struct kanshi_head *head;
	wl_list_for_each(head, &state->heads, link) {
		i++;
		struct kanshi_profile_output *profile_output = matches[i];

		fprintf(stderr, "applying profile output '%s' on connected head '%s'\n",
			profile_output->name, head->name);

		bool enabled = head->enabled;
		if (profile_output->fields & KANSHI_OUTPUT_ENABLED) {
			enabled = profile_output->enabled;
		}

		if (!enabled) {
			zwlr_output_configuration_v1_disable_head(config, head->wlr_head);
			continue;
		}

		struct zwlr_output_configuration_head_v1 *config_head =
			zwlr_output_configuration_v1_enable_head(config, head->wlr_head);
		if (profile_output->fields & KANSHI_OUTPUT_MODE) {
			// TODO: support custom modes
			struct kanshi_mode *mode = match_mode(head,
				profile_output->mode.width, profile_output->mode.height,
				profile_output->mode.refresh);
			if (mode == NULL) {
				fprintf(stderr,
					"output '%s' doesn't support mode '%dx%d@%fHz'\n",
					head->name,
					profile_output->mode.width, profile_output->mode.height,
					(float)profile_output->mode.refresh / 1000);
				goto error;
			}
			zwlr_output_configuration_head_v1_set_mode(config_head,
				mode->wlr_mode);
		}
		if (profile_output->fields & KANSHI_OUTPUT_POSITION) {
			zwlr_output_configuration_head_v1_set_position(config_head,
				profile_output->position.x, profile_output->position.y);
		}
		if (profile_output->fields & KANSHI_OUTPUT_SCALE) {
			zwlr_output_configuration_head_v1_set_scale(config_head,
				wl_fixed_from_double(profile_output->scale));
		}
		if (profile_output->fields & KANSHI_OUTPUT_TRANSFORM) {
			zwlr_output_configuration_head_v1_set_transform(config_head,
				profile_output->transform);
		}
		if (profile_output->fields & KANSHI_OUTPUT_ADAPTIVE_SYNC) {
			zwlr_output_configuration_head_v1_set_adaptive_sync(config_head,
				profile_output->adaptive_sync);
		}
	}

	zwlr_output_configuration_v1_apply(config);
	return true;

error:
	free(pending);
	zwlr_output_configuration_v1_destroy(config);
	return false;
}


static void mode_handle_size(void *data, struct zwlr_output_mode_v1 *wlr_mode,
		int32_t width, int32_t height) {
	struct kanshi_mode *mode = data;
	mode->width = width;
	mode->height = height;
}

static void mode_handle_refresh(void *data,
		struct zwlr_output_mode_v1 *wlr_mode, int32_t refresh) {
	struct kanshi_mode *mode = data;
	mode->refresh = refresh;
}

static void mode_handle_preferred(void *data,
		struct zwlr_output_mode_v1 *wlr_mode) {
	struct kanshi_mode *mode = data;
	mode->preferred = true;
}

static void mode_handle_finished(void *data,
		struct zwlr_output_mode_v1 *wlr_mode) {
	struct kanshi_mode *mode = data;
	wl_list_remove(&mode->link);
	if (zwlr_output_mode_v1_get_version(mode->wlr_mode) >= 3) {
		zwlr_output_mode_v1_release(mode->wlr_mode);
	} else {
		zwlr_output_mode_v1_destroy(mode->wlr_mode);
	}
	free(mode);
}

static const struct zwlr_output_mode_v1_listener mode_listener = {
	.size = mode_handle_size,
	.refresh = mode_handle_refresh,
	.preferred = mode_handle_preferred,
	.finished = mode_handle_finished,
};

static void head_handle_name(void *data,
		struct zwlr_output_head_v1 *wlr_head, const char *name) {
	struct kanshi_head *head = data;
	head->name = strdup(name);
}

static void head_handle_description(void *data,
		struct zwlr_output_head_v1 *wlr_head, const char *description) {
	struct kanshi_head *head = data;
	head->description = strdup(description);
}

static void head_handle_physical_size(void *data,
		struct zwlr_output_head_v1 *wlr_head, int32_t width, int32_t height) {
	struct kanshi_head *head = data;
	head->phys_width = width;
	head->phys_height = height;
}

static void head_handle_mode(void *data,
		struct zwlr_output_head_v1 *wlr_head,
		struct zwlr_output_mode_v1 *wlr_mode) {
	struct kanshi_head *head = data;

	struct kanshi_mode *mode = calloc(1, sizeof(*mode));
	mode->head = head;
	mode->wlr_mode = wlr_mode;
	wl_list_insert(head->modes.prev, &mode->link);

	zwlr_output_mode_v1_add_listener(wlr_mode, &mode_listener, mode);
}

static void head_handle_enabled(void *data,
		struct zwlr_output_head_v1 *wlr_head, int32_t enabled) {
	struct kanshi_head *head = data;
	head->enabled = !!enabled;
	if (!enabled) {
		head->mode = NULL;
	}
}

static void head_handle_current_mode(void *data,
		struct zwlr_output_head_v1 *wlr_head,
		struct zwlr_output_mode_v1 *wlr_mode) {
	struct kanshi_head *head = data;
	struct kanshi_mode *mode;
	wl_list_for_each(mode, &head->modes, link) {
		if (mode->wlr_mode == wlr_mode) {
			head->mode = mode;
			return;
		}
	}
	fprintf(stderr, "received unknown current_mode\n");
	head->mode = NULL;
}

static void head_handle_position(void *data,
		struct zwlr_output_head_v1 *wlr_head, int32_t x, int32_t y) {
	struct kanshi_head *head = data;
	head->x = x;
	head->y = y;
}

static void head_handle_transform(void *data,
		struct zwlr_output_head_v1 *wlr_head, int32_t transform) {
	struct kanshi_head *head = data;
	head->transform = transform;
}

static void head_handle_scale(void *data,
		struct zwlr_output_head_v1 *wlr_head, wl_fixed_t scale) {
	struct kanshi_head *head = data;
	head->scale = wl_fixed_to_double(scale);
}

static void head_handle_finished(void *data,
		struct zwlr_output_head_v1 *wlr_head) {
	struct kanshi_head *head = data;
	wl_list_remove(&head->link);
	if (zwlr_output_head_v1_get_version(head->wlr_head) >= 3) {
		zwlr_output_head_v1_release(head->wlr_head);
	} else {
		zwlr_output_head_v1_destroy(head->wlr_head);
	}
	free(head->name);
	free(head->description);
	free(head->make);
	free(head->model);
	free(head->serial_number);
	free(head);
}

void head_handle_make(void *data,
		struct zwlr_output_head_v1 *zwlr_output_head_v1,
		const char *make) {
	struct kanshi_head *head = data;
	head->make = strdup(make);
}

void head_handle_model(void *data,
		struct zwlr_output_head_v1 *zwlr_output_head_v1,
		const char *model) {
	struct kanshi_head *head = data;
	head->model = strdup(model);
}

void head_handle_serial_number(void *data,
		struct zwlr_output_head_v1 *zwlr_output_head_v1,
		const char *serial_number) {
	struct kanshi_head *head = data;
	head->serial_number = strdup(serial_number);
}

static void head_handle_adaptive_sync(void *data,
		struct zwlr_output_head_v1 *zwlr_output_head_v1, uint32_t state) {
	struct kanshi_head *head = data;
	head->adaptive_sync = state;
}

static const struct zwlr_output_head_v1_listener head_listener = {
	.name = head_handle_name,
	.description = head_handle_description,
	.physical_size = head_handle_physical_size,
	.mode = head_handle_mode,
	.enabled = head_handle_enabled,
	.current_mode = head_handle_current_mode,
	.position = head_handle_position,
	.transform = head_handle_transform,
	.scale = head_handle_scale,
	.finished = head_handle_finished,
	.make = head_handle_make,
	.model = head_handle_model,
	.serial_number = head_handle_serial_number,
	.adaptive_sync = head_handle_adaptive_sync,
};

static void output_manager_handle_head(void *data,
		struct zwlr_output_manager_v1 *manager,
		struct zwlr_output_head_v1 *wlr_head) {
	struct kanshi_state *state = data;

	struct kanshi_head *head = calloc(1, sizeof(*head));
	head->state = state;
	head->wlr_head = wlr_head;
	head->scale = 1.0;
	wl_list_init(&head->modes);
	wl_list_insert(&state->heads, &head->link);

	zwlr_output_head_v1_add_listener(wlr_head, &head_listener, head);
}

static bool match_and_apply(struct kanshi_state *state,
		kanshi_apply_done_func callback, void *data) {
	assert(wl_list_length(&state->heads) <= HEADS_MAX);
	// matches[i] gives the kanshi_profile_output for the i-th head
	struct kanshi_profile_output *matches[HEADS_MAX];
	if (state->current_profile != NULL &&
			match_profile(state, state->current_profile, matches)) {
		// keep the current profile if it still matches
		if (callback != NULL) {
			callback(data, true);
		}
		return true;
	}
	struct kanshi_profile *profile = match(state, matches);
	if (profile != NULL) {
		return apply_profile(state, profile, matches, callback, data);
	}
	fprintf(stderr, "no profile matched\n");
	return false;
}

bool kanshi_switch(struct kanshi_state *state, struct kanshi_profile *profile,
		kanshi_apply_done_func callback, void *data) {
	struct kanshi_profile_output *matches[HEADS_MAX];
	if (!match_profile(state, profile, matches)) {
		return false;
	}

	return apply_profile(state, profile, matches, callback, data);
}

static void output_manager_handle_done(void *data,
		struct zwlr_output_manager_v1 *manager, uint32_t serial) {
	struct kanshi_state *state = data;
	state->serial = serial;
	match_and_apply(state, NULL, NULL);
}

static void output_manager_handle_finished(void *data,
		struct zwlr_output_manager_v1 *manager) {
	// This space is intentionally left blank
}

static const struct zwlr_output_manager_v1_listener output_manager_listener = {
	.head = output_manager_handle_head,
	.done = output_manager_handle_done,
	.finished = output_manager_handle_finished,
};

static void registry_handle_global(void *data, struct wl_registry *registry,
		uint32_t name, const char *interface, uint32_t version) {
	struct kanshi_state *state = data;

	if (strcmp(interface, zwlr_output_manager_v1_interface.name) == 0) {
		uint32_t bind_version = 2;
		if (version >= 4) {
			bind_version = 4;
		} else if (version > bind_version) {
			bind_version = version;
		}
		state->output_manager = wl_registry_bind(registry, name,
			&zwlr_output_manager_v1_interface, bind_version);
		zwlr_output_manager_v1_add_listener(state->output_manager,
			&output_manager_listener, state);
	}
}

static void registry_handle_global_remove(void *data,
		struct wl_registry *registry, uint32_t name) {
	// This space is intentionally left blank
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_handle_global,
	.global_remove = registry_handle_global_remove,
};

static struct kanshi_config *read_config(const char *config) {
	if (config != NULL) {
		return parse_config(config);
	}

	const char config_filename[] = "kanshi/config";
	char config_path[PATH_MAX];
	const char *xdg_config_home = getenv("XDG_CONFIG_HOME");
	const char *home = getenv("HOME");
	if (xdg_config_home != NULL) {
		snprintf(config_path, sizeof(config_path), "%s/%s",
			xdg_config_home, config_filename);
	} else if (home != NULL) {
		snprintf(config_path, sizeof(config_path), "%s/.config/%s",
			home, config_filename);
	} else {
		fprintf(stderr, "HOME not set\n");
		return NULL;
	}

	return parse_config(config_path);
}

static void destroy_config(struct kanshi_config *config) {
	struct kanshi_profile *profile, *tmp_profile;
	wl_list_for_each_safe(profile, tmp_profile, &config->profiles, link) {
		struct kanshi_profile_output *output, *tmp_output;
		wl_list_for_each_safe(output, tmp_output, &profile->outputs, link) {
			free(output->name);
			wl_list_remove(&output->link);
			free(output);
		}
		struct kanshi_profile_command *command, *tmp_command;
		wl_list_for_each_safe(command, tmp_command, &profile->commands, link) {
			free(command->command);
			wl_list_remove(&command->link);
			free(command);
		}
		wl_list_remove(&profile->link);
		free(profile);
	}
	free(config);
}

bool kanshi_reload_config(struct kanshi_state *state,
		kanshi_apply_done_func callback, void *data) {
	fprintf(stderr, "reloading config\n");
	struct kanshi_config *config = read_config(state->config_arg);
	if (config == NULL) {
		return false;
	}
	destroy_config(state->config);
	state->config = config;
	state->pending_profile = NULL;
	state->current_profile = NULL;
	return match_and_apply(state, callback, data);
}

static const char usage[] = "Usage: %s [options...]\n"
"  -h, --help           Show help message and quit\n"
"  -c, --config <path>  Path to config file.\n";

static const struct option long_options[] = {
	{"help", no_argument, 0, 'h'},
	{"config", required_argument, 0, 'c'},
	{"listen-fd", required_argument, 0, 'l'},
	{0},
};

int main(int argc, char *argv[]) {
	const char *config_arg = NULL;
#if KANSHI_HAS_VARLINK
	int listen_fd = -1;
#endif

	int opt;
	while ((opt = getopt_long(argc, argv, "hc:l:", long_options, NULL)) != -1) {
		switch (opt) {
		case 'c':
			config_arg = optarg;
			break;
		case 'l':
#if KANSHI_HAS_VARLINK
			listen_fd = strtol(optarg, NULL, 10);
#else
			fprintf(stderr, "IPC support is disabled, "
				"-l/--listen-fd is not supported\n");
			return EXIT_FAILURE;
#endif
			break;
		case 'h':
			fprintf(stderr, usage, argv[0]);
			return EXIT_SUCCESS;
		default:
			fprintf(stderr, usage, argv[0]);
			return EXIT_FAILURE;
		}
	}

	struct kanshi_config *config = read_config(config_arg);
	if (config == NULL) {
		return EXIT_FAILURE;
	}

	struct wl_display *display = wl_display_connect(NULL);
	if (display == NULL) {
		fprintf(stderr, "failed to connect to display\n");
		return EXIT_FAILURE;
	}

	struct kanshi_state state = {
		.running = true,
		.display = display,
		.config = config,
		.config_arg = config_arg,
	};
	int ret = EXIT_SUCCESS;
#if KANSHI_HAS_VARLINK
	if (kanshi_init_ipc(&state, listen_fd) != 0) {
		ret = EXIT_FAILURE;
		goto done;
	}
#endif
	wl_list_init(&state.heads);

	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, &state);
	if (wl_display_roundtrip(display) < 0) {
		fprintf(stderr, "wl_display_roundtrip() failed\n");
		return EXIT_FAILURE;
	}

	if (state.output_manager == NULL) {
		fprintf(stderr, "compositor doesn't support "
			"wlr-output-management-unstable-v1\n");
		ret = EXIT_FAILURE;
		goto done;
	}

	ret = kanshi_main_loop(&state);

done:
#if KANSHI_HAS_VARLINK
	kanshi_free_ipc(&state);
#endif
	wl_display_disconnect(display);

	return ret;
}
