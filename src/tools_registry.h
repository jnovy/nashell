#ifndef TOOLS_REGISTRY_H
#define TOOLS_REGISTRY_H

#include "cJSON.h"
#include "provider.h"

/* Shared tool definition — defined once, formatted per-provider.
 * Eliminates the 3x duplication of tool definitions across
 * provider_local.c, provider_openai.c, and provider_anthropic.c.
 * Also replaces llm.c:build_tools_array() — single source of truth. */

typedef struct {
    const char *name;
    const char *description;
    const char *params_json;   /* JSON schema string for parameters */
} tool_def_t;

/* The canonical tool registry — single source of truth for all tools.
 * Defined in tools_registry.c.  Terminated by a {NULL,NULL,NULL} sentinel. */
extern const tool_def_t TOOL_REGISTRY[];

/* FIX #14: Changed from extern const int to #define so it can be used in
 * _Static_assert at compile time. Previously the magic number 18 had to
 * be duplicated in tools.c's assertion. */
#define TOOL_REGISTRY_COUNT 20

/* Build a comma-separated list of all tool names from the registry.
 * Caller must free() the returned string. */
char *tool_registry_names_csv(void);

/* Build a cJSON tools array from the registry, formatted for the given provider type.
 * Handles the structural differences between Local/OpenAI/Anthropic APIs.
 * For OpenAI: adds "strict":true and "additionalProperties":false recursively.
 * For Anthropic: uses "input_schema" instead of "parameters" and no "function" wrapper. */
cJSON *build_tools_from_registry(provider_type_t type);

/* Build tool definitions with an optional filter (whitelist/blacklist).
 * filter=NULL means all tools included. */
cJSON *build_tools_from_registry_filtered(provider_type_t type,
                                           const struct tool_filter_t *filter);

#endif /* TOOLS_REGISTRY_H */
