/* sample_plugin_multi.c - Multi-tool external plugin for dlopen testing.
 * Registers two tools from one .so to test shared-dlhandle tracking. */

#include "tool_plugin.h"  /* tool_result_t, tool_plugin_t, cJSON */

static tool_result_t multi_alpha(void *ctx, cJSON *params) {
    (void)ctx; (void)params;
    tool_result_t r = {0};
    r.meta = cJSON_CreateObject();
    cJSON_AddStringToObject(r.meta, "output", "alpha");
    r.success = 1;
    return r;
}

static tool_result_t multi_beta(void *ctx, cJSON *params) {
    (void)ctx; (void)params;
    tool_result_t r = {0};
    r.meta = cJSON_CreateObject();
    cJSON_AddStringToObject(r.meta, "output", "beta");
    r.success = 1;
    return r;
}

static const tool_plugin_t multi_plugins[] = {
    {
        .abi_version = TOOL_PLUGIN_ABI_VERSION,
        .name = "multi_alpha",
        .version = "1.0.0",
        .description = "Alpha tool from multi-plugin",
        .params = NULL,
        .execute = (void *)multi_alpha,
        .caps = 0,
        .group = "multi_sample",
    },
    {
        .abi_version = TOOL_PLUGIN_ABI_VERSION,
        .name = "multi_beta",
        .version = "1.0.0",
        .description = "Beta tool from multi-plugin",
        .params = NULL,
        .execute = (void *)multi_beta,
        .caps = 0,
        .group = "multi_sample",
    },
};
TOOL_PLUGIN_REGISTER_ARRAY(multi_plugins, 2)
