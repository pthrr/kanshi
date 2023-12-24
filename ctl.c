#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <varlink.h>

#include "ipc.h"

static void usage(void) {
	fprintf(stderr, "Usage: kanshictl [command]\n"
		"\n"
		"Commands:\n"
		"  reload            Reload the configuration file\n"
		"  switch <profile>  Switch to another profile\n");
}

static long handle_call_done(VarlinkConnection *connection, const char *error,
		VarlinkObject *parameters, uint64_t flags, void *userdata) {
	if (error != NULL) {
		if (strcmp(error, "fr.emersion.kanshi.ProfileNotFound") == 0) {
			fprintf(stderr, "Profile not found\n");
		} else if (strcmp(error, "fr.emersion.kanshi.ProfileNotMatched") == 0) {
			fprintf(stderr, "Profile does not match the current output configuration\n");
		} else if (strcmp(error, "fr.emersion.kanshi.ProfileNotApplied") == 0) {
			fprintf(stderr, "Profile could not be applied by the compositor\n");
		} else {
			fprintf(stderr, "Error: %s\n", error);
		}
		exit(EXIT_FAILURE);
	}
	return varlink_connection_close(connection);
}

static int set_blocking(int fd) {
	int flags = fcntl(fd, F_GETFL);
	if (flags == -1) {
		fprintf(stderr, "fnctl F_GETFL failed: %s\n", strerror(errno));
		return -1;
	}
	flags &= ~O_NONBLOCK;
	if (fcntl(fd, F_SETFL, flags) == -1) {
		fprintf(stderr, "fnctl F_SETFL failed: %s\n", strerror(errno));
		return -1;
	}
	return 0;
}

static int wait_for_event(VarlinkConnection *connection) {
	int fd = varlink_connection_get_fd(connection);
	if (set_blocking(fd) != 0) {
		return -1;
	}

	while (!varlink_connection_is_closed(connection)) {
		uint32_t events = varlink_connection_get_events(connection);
		long result = varlink_connection_process_events(connection, events);
		if (result != 0) {
			fprintf(stderr, "varlink_connection_process_events failed: %s\n",
					varlink_error_string(-result));
			return -1;
		}
	}
	return 0;
}

int main(int argc, char *argv[]) {
	if (argc < 2) {
		usage();
		return EXIT_FAILURE;
	}
	if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
		usage();
		return EXIT_SUCCESS;
	}

	VarlinkConnection *connection;
	char address[PATH_MAX];
	if (get_ipc_address(address, sizeof(address)) < 0) {
		return EXIT_FAILURE;
	}
	if (varlink_connection_new(&connection, address) != 0) {
		fprintf(stderr, "Couldn't connect to kanshi at %s.\n"
				"Is the kanshi daemon running?\n", address);
		return EXIT_FAILURE;
	}

	const char *command = argv[1];
	long ret;
	if (strcmp(command, "reload") == 0) {
		ret = varlink_connection_call(connection,
			"fr.emersion.kanshi.Reload", NULL, 0, handle_call_done, NULL);
	} else if (strcmp(command, "switch") == 0) {
		if (argc < 3) {
			usage();
			return EXIT_FAILURE;
		}
		const char *profile = argv[2];

		VarlinkObject *params = NULL;
		varlink_object_new(&params);
		varlink_object_set_string(params, "profile", profile);
		ret = varlink_connection_call(connection,
			"fr.emersion.kanshi.Switch", params, 0, handle_call_done, NULL);
		varlink_object_unref(params);
	} else {
		fprintf(stderr, "invalid command: %s\n", argv[1]);
		usage();
		return EXIT_FAILURE;
	}
	if (ret != 0) {
		fprintf(stderr, "varlink_connection_call failed: %s\n",
			varlink_error_string(-ret));
		return EXIT_FAILURE;
	}

	return wait_for_event(connection);
}
