/* sample_plugin_bad_abi.c - Plugin with wrong ABI version for testing rejection. */

#include "tool_plugin.h" /* tool_result_t, tool_plugin_t, cJSON */

static tool_result_t bad_handler(void *ctx, cJSON *params) {
  (void)ctx;
  (void)params;
  tool_result_t r = {0};
  r.meta = cJSON_CreateObject();
  r.success = 1;
  return r;
}

static const tool_plugin_t bad_plugin = {
  .abi_version = 999, /* wrong ABI */
  .name = "bad_abi_tool",
  .version = "1.0.0",
  .description = "Should be rejected",
  .params = NULL,
  .execute = (void *)bad_handler,
  .caps = 0,
  .group = NULL,
};
TOOL_PLUGIN_REGISTER(bad_plugin)
