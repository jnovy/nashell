/* sample_plugin.c - Minimal external tool plugin for dlopen testing.
 *
 * Build: gcc -shared -fPIC -I src -o tests/sample_plugin.so tests/sample_plugin.c -lnash
 *
 * This .so self-registers via TOOL_PLUGIN_REGISTER() constructor.
 * The handler is a no-op that returns a simple JSON result. */

#include "tool_plugin.h"  /* tool_result_t, tool_plugin_t, cJSON */
#include <string.h>

/* Minimal handler that returns {"output": "hello from sample_plugin"} */
static tool_result_t sample_hello(void *ctx, cJSON *params) {
    (void)ctx;
    (void)params;
    tool_result_t r = {0};
    r.meta = cJSON_CreateObject();
    cJSON_AddStringToObject(r.meta, "output", "hello from sample_plugin");
    r.success = 1;
    return r;
}

static const tool_param_t hello_params[] = {
    {"name", "string", "Your name", 0, NULL, NULL},
    {0}
};

static const tool_plugin_t hello_plugin = {
    .abi_version = TOOL_PLUGIN_ABI_VERSION,
    .name = "sample_hello",
    .version = "1.0.0",
    .description = "A sample external plugin for testing",
    .params = hello_params,
    .execute = (void *)sample_hello,
    .caps = 0,
    .group = NULL,
};
TOOL_PLUGIN_REGISTER(hello_plugin)
