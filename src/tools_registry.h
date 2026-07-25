#ifndef TOOLS_REGISTRY_H
#define TOOLS_REGISTRY_H

#include "cJSON.h"
#include "provider.h"

/* Legacy tool definition struct - superseded by tool_plugin_t.
 * Retained only for backwards compatibility; no active users. */

typedef struct {
    const char *name;
    const char *description;
} tool_def_t;

/* Build a comma-separated list of all tool names from the plugin registry.
 * Caller must free() the returned string. */
char *tool_registry_names_csv(void);

/* Build a cJSON tools array from the plugin registry, formatted for the given
 * provider type.  Handles structural differences between Local/OpenAI/Anthropic
 * APIs.  For OpenAI: adds "strict":true and "additionalProperties":false
 * recursively.  For Anthropic: uses "input_schema" instead of "parameters"
 * and no "function" wrapper. */
cJSON *build_tools_from_registry(provider_type_t type);

/* Build tool definitions with an optional filter (whitelist/blacklist).
 * filter=NULL means all tools included. */
cJSON *build_tools_from_registry_filtered(provider_type_t type,
                                           const struct tool_filter_t *filter);

#endif /* TOOLS_REGISTRY_H */
