#ifndef KANSHI_IPC_H
#define KANSHI_IPC_H

#include <stdbool.h>

#include "kanshi.h"

int kanshi_init_ipc(struct kanshi_state *state, int listen_fd);
void kanshi_free_ipc(struct kanshi_state *state);

int get_ipc_address(char *address, size_t size);

#endif
