/* tool_plugin.h - Self-registering plugin interface for Nash tools.
 *
 * Plugins register themselves via __attribute__((constructor)) at load time.
 * Works for both statically linked tools and dlopen'd external plugins.
 *
 * Static tools: compiled into the nash binary, constructor runs before main().
 * External plugins: shared objects (.so) loaded via tool_plugin_load() or
 *   tool_plugin_load_dir().  dlopen() triggers their constructors, which
 *   call tool_plugin_register() to self-register into the same registry.
 *
 * ABI versioning: TOOL_PLUGIN_ABI_VERSION is embedded into every plugin
 * descriptor.  The registry rejects plugins built against a different ABI.
 *
 * Phase 2 (future): Replace tool_ctx_t* with narrow tool_services_t*. */

#ifndef TOOL_PLUGIN_H
#define TOOL_PLUGIN_H

#include <stdint.h>
#include "cJSON.h"

/* Maximum number of plugins that can be registered */
#define TOOL_PLUGIN_MAX 64

/* ABI version - bump when tool_plugin_t or tool_ctx_t layout changes.
 * Plugins embed this at compile time via TOOL_PLUGIN_REGISTER().
 * The registry rejects mismatched versions to prevent ABI crashes. */
#define TOOL_PLUGIN_ABI_VERSION 2

/* Capability flags (for future Phase 2 context narrowing) */
#define TOOL_CAP_STORE       (1u << 0)
#define TOOL_CAP_CONFIG      (1u << 1)
#define TOOL_CAP_PROVIDER    (1u << 2)
#define TOOL_CAP_MEMORY      (1u << 3)
#define TOOL_CAP_WORKSPACE   (1u << 4)
#define TOOL_CAP_SCRATCHPAD  (1u << 5)
#define TOOL_CAP_SESSION     (1u << 6)
#define TOOL_CAP_EVENTS      (1u << 7)
#define TOOL_CAP_CORE        (1u << 31)

/* Parameter descriptor for a single tool parameter.
 * Replaces the unreadable escaped-JSON params_json strings with a
 * compile-time-validated C struct array.  The JSON Schema required by
 * LLM APIs is generated at runtime by tool_params_to_cjson().
 *
 * Sentinel-terminated: last entry has name == NULL. */
typedef struct {
    const char   *name;          /* "path", "command", "timeout" */
    const char   *type;          /* "string", "integer", "boolean", "array" */
    const char   *description;   /* LLM-facing help text */
    int           required;      /* 1 = required, 0 = optional */
    const char  **enum_values;   /* NULL-terminated list, or NULL if none */
    const char   *items_type;    /* For type="array": element type, e.g. "string" */
} tool_param_t;

/* Plugin descriptor.
 * Combines the old tool_def_t (name + description + schema) with the
 * handler function pointer, so a single struct is the complete definition.
 *
 * The handler signature is void* to avoid a circular dependency between
 * tool_plugin.h and tools.h. Callers cast to/from tool_ctx_t*.
 * Actual type: tool_result_t (*)(tool_ctx_t*, cJSON*) */
typedef struct tool_plugin_t {
    int                abi_version;   /* must equal TOOL_PLUGIN_ABI_VERSION */
    const char        *name;          /* "file_read", "device_control", etc. */
    const char        *version;       /* semver: "1.0.0" (informational) */
    const char        *description;   /* LLM-facing description */
    const tool_param_t *params;       /* Parameter definitions (sentinel-terminated) */

    /* Handler: void* to avoid circular includes.
     * Actual type: tool_result_t (*)(tool_ctx_t*, cJSON*) */
    void *execute;

    /* Capability flags (Phase 2: used for context narrowing) */
    uint32_t      caps;

    /* Behavioral flags (default-off, etc.) */
    uint32_t      flags;

    /* Group name for multi-tool plugins (NULL = standalone) */
    const char   *group;
} tool_plugin_t;

/* Plugin flags */
#define TOOL_FLAG_DEFAULT_OFF  (1u << 0)  /* disabled unless user runs /tool on */

/* ---- Registration API ---- */

/* Register a plugin. Called from __attribute__((constructor)) or manually.
 * Returns 0 on success, -1 on error (registry full, duplicate name). */
int tool_plugin_register(const tool_plugin_t *plugin);

/* Find a plugin by name. Returns NULL if not found. */
const tool_plugin_t *tool_plugin_find(const char *name);

/* Get the number of registered plugins. */
int tool_plugin_count(void);

/* Get plugin at index (for iteration). Returns NULL if out of range. */
const tool_plugin_t *tool_plugin_get(int index);

/* Clear all registered plugins (for testing). */
void tool_plugin_clear(void);

/* Sort registered plugins by name for stable, deterministic ordering.
 * Call once from main() after all constructors have run. */
void tool_plugin_sort(void);

/* ---- External plugin loading (dlopen) ---- */

/* Load an external plugin from a shared object (.so) file.
 * The .so must contain a TOOL_PLUGIN_REGISTER() constructor that calls
 * tool_plugin_register().  dlopen(RTLD_NOW) triggers the constructor.
 * Returns 0 on success, -1 on error (file not found, symbol errors,
 * constructor didn't register, ABI version mismatch). */
int tool_plugin_load(const char *so_path);

/* Load all *.so plugins from a directory.
 * Skips non-.so files.  Logs errors for individual failures but
 * continues loading remaining plugins.
 * Returns the number of plugins successfully loaded, or -1 on
 * directory open failure. */
int tool_plugin_load_dir(const char *dir_path);

/* Unload an external plugin by name.  Removes it from the registry
 * and dlclose's the shared object.
 * Only works for dlopen'd plugins (not statically linked ones).
 * Returns 0 on success, -1 if not found or not a dynamic plugin. */
int tool_plugin_unload(const char *name);

/* Close all dlopen'd plugin handles.  Call at shutdown.
 * Does NOT remove plugins from the registry (they become stale).
 * Typically called right before exit. */
void tool_plugin_cleanup(void);

/* ---- Parameter schema helpers ---- */

/* Build a cJSON object representing the JSON Schema for a tool's parameters.
 * Returns a new cJSON* that the caller must eventually free (or hand off to
 * another cJSON tree via cJSON_AddItemToObject).  Returns NULL if params
 * is NULL or empty (sentinel-only). */
cJSON *tool_params_to_cjson(const tool_param_t *params);

/* Check that all required parameters are present in `actual`.
 * Writes the name of the first missing required param into `missing`
 * (up to `len` bytes).  Returns 1 if all required params are present,
 * 0 if a required param is missing (name written to `missing`). */
int tool_params_check_required(const tool_param_t *params,
                               const cJSON *actual,
                               char *missing, size_t len);

/* Return the name of the first required parameter, or NULL if none.
 * Used by react_get_action_desc() for display. */
const char *tool_params_first_required(const tool_param_t *params);

/* ---- Convenience macros ---- */

/* Register a single plugin variable via constructor */
#define TOOL_PLUGIN_REGISTER(var_name)                  \
    __attribute__((constructor))                         \
    static void _tool_register_##var_name(void) {        \
        tool_plugin_register(&var_name);                 \
    }

/* Register an array of plugins via constructor */
#define TOOL_PLUGIN_REGISTER_ARRAY(arr, count)           \
    __attribute__((constructor))                          \
    static void _tool_register_##arr(void) {              \
        for (int _i = 0; _i < (count); _i++)              \
            tool_plugin_register(&(arr)[_i]);              \
    }

#endif /* TOOL_PLUGIN_H */
