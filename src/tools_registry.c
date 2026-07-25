#include "tools_registry.h"
#include "tool_plugin.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Tool definitions are now self-registered by each tool_*.c file via
 * TOOL_PLUGIN_REGISTER() constructors. The static TOOL_REGISTRY[] array
 * that used to live here has been removed. */

/* Build a comma-separated list of all tool names from the plugin registry.
 * Caller must free() the returned string. */
char *tool_registry_names_csv(void) {
    int count = tool_plugin_count();
    size_t total = 0;
    for (int i = 0; i < count; i++) {
        const tool_plugin_t *p = tool_plugin_get(i);
        if (p) total += strlen(p->name) + 2; /* ", " */
    }
    char *buf = malloc(total + 1);
    if (!buf) return NULL;
    buf[0] = '\0';
    int first = 1;
    for (int i = 0; i < count; i++) {
        const tool_plugin_t *p = tool_plugin_get(i);
        if (!p) continue;
        if (!first) strcat(buf, ", ");
        strcat(buf, p->name);
        first = 0;
    }
    return buf;
}
