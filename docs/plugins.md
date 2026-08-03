# Custom Tool Plugins

Nash supports loading external tools as shared-object (`.so`) plugins at
startup. This lets you add new tools without modifying the Nash source -
just compile a `.so`, point Nash at it, and the tool appears in the LLM's
tool list alongside the built-in ones.

## Quick start

1. Write a plugin (see [Minimal example](#minimal-example) below).
2. Build it as a shared library.
3. Set `plugin_dir` in your `config.toml`.
4. Restart Nash - your tool is available.

## Configuration

In `~/.nash/config.toml`, set the `plugin_dir` under `[paths]`:

```toml
[paths]
plugin_dir = "/home/you/.nash/plugins"   # directory containing .so files
```

At startup Nash calls `tool_plugin_load_dir()` which opens every `*.so`
file in that directory via `dlopen`. Each `.so` self-registers its tools
through a constructor function that runs automatically on load.

Leave `plugin_dir` empty (the default) to disable external plugin loading.

## Minimal example

Create `my_tool.c`:

```c
#include "tool_plugin.h"   /* the only Nash header you need */
#include <string.h>

/* Handler: receives opaque context + JSON params, returns a result. */
static tool_result_t my_tool_exec(void *ctx, cJSON *params) {
    (void)ctx;

    /* Read a parameter */
    cJSON *msg = cJSON_GetObjectItem(params, "message");
    const char *text = (msg && msg->valuestring) ? msg->valuestring : "(none)";

    /* Build result JSON */
    cJSON *meta = cJSON_CreateObject();
    cJSON_AddStringToObject(meta, "output", text);

    return tools_make_result(1, meta, NULL);   /* success=1 */
}

/* Parameter definitions (sentinel-terminated array). */
static const tool_param_t my_params[] = {
    TOOL_PARAM("message", "string", "A message to echo back", 1),  /* required */
    TOOL_PARAM_END
};

/* Plugin descriptor. */
static const tool_plugin_t my_plugin = TOOL_DEF(
    "my_tool",                              /* tool name */
    "Echo a message back to the caller.",    /* LLM-visible description */
    my_params,                               /* parameter definitions */
    my_tool_exec                             /* handler function */
);

/* Self-register when dlopen loads this .so */
TOOL_PLUGIN_REGISTER(my_plugin)
```

Build:

```bash
gcc -shared -fPIC -I /path/to/nash/src -o my_tool.so my_tool.c -L /path/to/nash -lnash
```

Copy `my_tool.so` into your `plugin_dir` and restart Nash. The LLM will
now see `my_tool` in its tool list and can call it.

## How it works

### Self-registration

The `TOOL_PLUGIN_REGISTER(var)` macro expands to a GCC constructor:

```c
__attribute__((constructor)) static void _tool_register_my_plugin(void) {
    tool_plugin_register(&my_plugin);
}
```

When `dlopen` loads the `.so`, the constructor runs automatically and
registers the plugin into Nash's global tool registry - the same registry
used by built-in tools.

### ABI versioning

Every plugin embeds `TOOL_PLUGIN_ABI_VERSION` (currently **3**) at compile
time via the `TOOL_DEF` macro. The registry rejects plugins built against
a different ABI version to prevent crashes from struct layout mismatches.

If you upgrade Nash and the ABI version changes, recompile your plugins.

### Overriding built-in tools

If an external plugin registers a tool with the same name as a built-in
tool, the external plugin **replaces** the built-in in-place. This lets
you override default behavior without patching Nash.

## Multi-tool plugins

A single `.so` can register multiple tools using `TOOL_PLUGIN_REGISTER_ARRAY`:

```c
static const tool_plugin_t my_tools[] = {
    TOOL_DEF("tool_alpha", "First tool",  alpha_params, alpha_exec),
    TOOL_DEF("tool_beta",  "Second tool", beta_params,  beta_exec),
};
TOOL_PLUGIN_REGISTER_ARRAY(my_tools, 2)
```

Grouped tools can share a `.group` name for logical association:

```c
static const tool_plugin_t grouped = {
    .abi_version = TOOL_PLUGIN_ABI_VERSION,
    .name = "tool_alpha",
    .version = "1.0.0",
    .description = "...",
    .params = alpha_params,
    .execute = (void *)alpha_exec,
    .group = "my_suite",       /* logical group name */
};
```

## Lifecycle hooks

ABI v3 plugins can optionally provide lifecycle hooks:

```c
static void my_init(void *user_data) {
    /* Called after registration. user_data is plugin-owned state. */
}

static void my_cleanup(const char *session_dir) {
    /* Called once per react loop exit (session end). */
}

static tool_plugin_t my_plugin = {
    .abi_version = TOOL_PLUGIN_ABI_VERSION,
    .name = "my_tool",
    .version = "1.0.0",
    .description = "...",
    .params = my_params,
    .execute = (void *)my_tool_exec,
    .init = my_init,
    .cleanup = my_cleanup,
    .user_data = NULL,   /* set by the plugin or by Nash from config */
};
```

- **`init(user_data)`** - called after registration. Use for one-time
  setup (opening files, connecting to services).
- **`cleanup(session_dir)`** - called at the end of each react loop.
  Use for flushing state or writing session-specific output.

## Parameter types

The `tool_param_t` struct supports these types:

| Type | C macro | Description |
|------|---------|-------------|
| `"string"` | `TOOL_PARAM(name, "string", desc, required)` | String parameter |
| `"integer"` | `TOOL_PARAM(name, "integer", desc, required)` | Integer parameter |
| `"boolean"` | `TOOL_PARAM(name, "boolean", desc, required)` | Boolean parameter |
| `"array"` | `TOOL_PARAM_ARRAY(name, desc, required, items_type)` | Array with element type |
| Enum | `TOOL_PARAM_ENUM(name, type, desc, required, enum_values)` | Restricted values |

Example with an enum:

```c
static const char *modes[] = {"fast", "slow", "auto", NULL};  /* NULL-terminated */

static const tool_param_t params[] = {
    TOOL_PARAM_ENUM("mode", "string", "Processing mode", 1, modes),
    TOOL_PARAM_ARRAY("tags", "List of tags", 0, "string"),
    TOOL_PARAM_END
};
```

## Result helpers

`tool_plugin.h` provides static inline helpers so external plugins do not
need to link against Nash internals for result construction:

```c
/* Success with metadata JSON and optional store reference */
tool_result_t tools_make_result(int success, cJSON *meta, char *ref);

/* Error with message */
tool_result_t tools_make_error(const char *msg);
```

## Behavioral flags

Plugins can be registered with flags:

```c
static const tool_plugin_t my_plugin = TOOL_DEF_FLAGS(
    "my_tool", "...", my_params, my_exec,
    TOOL_FLAG_DEFAULT_OFF     /* disabled unless user runs /tool on */
);
```

`TOOL_FLAG_DEFAULT_OFF` means the tool is registered but not active until
explicitly enabled.

## Headers required

External plugins only need two headers from Nash:

- **`tool_plugin.h`** - plugin API, result types, registration macros
- **`cJSON.h`** - JSON construction/parsing (bundled with Nash)

Both are in `src/`. You do **not** need `tools_internal.h` or any other
Nash headers - the external plugin interface is intentionally narrow.

## Troubleshooting

| Problem | Cause | Fix |
|---------|-------|-----|
| Tool not appearing | `plugin_dir` empty or wrong path | Check `config.toml [paths] plugin_dir` |
| "ABI version mismatch" log | Plugin compiled against different Nash version | Recompile with current `src/tool_plugin.h` |
| dlopen error | Missing `-lnash` or wrong rpath | Build with `-L /path/to/nash -lnash -Wl,-rpath,...` |
| Tool overrides not working | Plugin loads after dispatch | Ensure `.so` is in `plugin_dir` at startup |
