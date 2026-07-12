/*
 * commands_internal.h — Internal declarations for split command files.
 * Not part of the public API; only included by commands.c and cmd_*.c.
 */
#ifndef COMMANDS_INTERNAL_H
#define COMMANDS_INTERNAL_H

#include "commands.h"

/* cmd_todo.c */
int cmd_todo(command_ctx_t *ctx, const char *args);

/* cmd_agents.c */
int cmd_agents(command_ctx_t *ctx, const char *args);

#endif /* COMMANDS_INTERNAL_H */
