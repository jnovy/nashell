/* tool_plugin.c - Dynamic plugin registry for Nash tools.
 *
 * Provides a growable array of tool_plugin_t descriptors that plugins
 * append to via tool_plugin_register(), typically called from
 * __attribute__((constructor)) functions at load time.
 *
 * This replaces the old parallel-array approach (TOOL_REGISTRY[] +
 * TOOL_HANDLERS[]) with a single unified registry. */

#include "tool_plugin.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <dlfcn.h>
#include <dirent.h>

/* Static registry array - filled by constructors before main() */
static const tool_plugin_t *plugin_registry[TOOL_PLUGIN_MAX];
static void *plugin_dlhandles[TOOL_PLUGIN_MAX]; /* non-NULL for dlopen'd plugins */
static int n_plugins = 0;

/* External-loading context: set by tool_plugin_load() around dlopen() so that
 * tool_plugin_register() (called from the .so constructor) knows it may
 * override an existing tool instead of rejecting a duplicate name. */
static int loading_external = 0;

/* Pending overrides: filled by tool_plugin_register() during external loading.
 * tool_plugin_load() reads these after dlopen() to tag new dlhandles and
 * dlclose orphaned old handles. */
static void *pending_old_handles[TOOL_PLUGIN_MAX];
static int   pending_override_slots[TOOL_PLUGIN_MAX];
static int   n_pending_overrides = 0;

int tool_plugin_register(const tool_plugin_t *plugin) {
    if (!plugin || !plugin->name) return -1;
    if (plugin->abi_version != TOOL_PLUGIN_ABI_VERSION) {
        fprintf(stderr, "tool_plugin: ABI mismatch for '%s' "
                "(plugin=%d, host=%d)\n",
                plugin->name, plugin->abi_version,
                TOOL_PLUGIN_ABI_VERSION);
        return -1;
    }
    if (n_plugins >= TOOL_PLUGIN_MAX) {
        fprintf(stderr, "tool_plugin: registry full (max %d)\n",
                TOOL_PLUGIN_MAX);
        return -1;
    }
    /* Check for duplicate names */
    for (int i = 0; i < n_plugins; i++) {
        if (strcmp(plugin_registry[i]->name, plugin->name) == 0) {
            if (!loading_external) {
                fprintf(stderr, "tool_plugin: duplicate tool name '%s'\n",
                        plugin->name);
                return -1;
            }
            /* External plugin overrides existing tool in-place */
            fprintf(stderr, "tool_plugin: '%s' overridden by external plugin\n",
                    plugin->name);
            pending_old_handles[n_pending_overrides] = plugin_dlhandles[i];
            pending_override_slots[n_pending_overrides] = i;
            n_pending_overrides++;
            plugin_registry[i] = plugin;
            plugin_dlhandles[i] = NULL; /* tagged by tool_plugin_load() */
            return 0;
        }
    }
    plugin_registry[n_plugins++] = plugin;
    return 0;
}

const tool_plugin_t *tool_plugin_find(const char *name) {
    if (!name) return NULL;
    for (int i = 0; i < n_plugins; i++) {
        if (strcmp(plugin_registry[i]->name, name) == 0)
            return plugin_registry[i];
    }
    return NULL;
}

int tool_plugin_count(void) {
    return n_plugins;
}

const tool_plugin_t *tool_plugin_get(int index) {
    if (index < 0 || index >= n_plugins) return NULL;
    return plugin_registry[index];
}

void tool_plugin_clear(void) {
    /* Close unique dlhandles before clearing (mirrors tool_plugin_cleanup) */
    void *closed[TOOL_PLUGIN_MAX];
    int n_closed = 0;
    for (int i = 0; i < n_plugins; i++) {
        void *h = plugin_dlhandles[i];
        if (!h) continue;
        int dup = 0;
        for (int j = 0; j < n_closed; j++) {
            if (closed[j] == h) { dup = 1; break; }
        }
        if (!dup) {
            dlclose(h);
            closed[n_closed++] = h;
        }
    }
    n_plugins = 0;
    memset(plugin_registry, 0, sizeof(plugin_registry));
    memset(plugin_dlhandles, 0, sizeof(plugin_dlhandles));
}

/* Sort entry: keeps plugin pointer and dlhandle together during sort */
typedef struct {
    const tool_plugin_t *plugin;
    void                *dlhandle;
} sort_entry_t;

static int sort_entry_cmp(const void *a, const void *b) {
    const sort_entry_t *ea = (const sort_entry_t *)a;
    const sort_entry_t *eb = (const sort_entry_t *)b;
    return strcmp(ea->plugin->name, eb->plugin->name);
}

void tool_plugin_sort(void) {
    if (n_plugins <= 1) return;
    /* Build temporary array that keeps plugin+dlhandle pairs together */
    sort_entry_t entries[TOOL_PLUGIN_MAX];
    for (int i = 0; i < n_plugins; i++) {
        entries[i].plugin   = plugin_registry[i];
        entries[i].dlhandle = plugin_dlhandles[i];
    }
    qsort(entries, (size_t)n_plugins, sizeof(entries[0]), sort_entry_cmp);
    for (int i = 0; i < n_plugins; i++) {
        plugin_registry[i]  = entries[i].plugin;
        plugin_dlhandles[i] = entries[i].dlhandle;
    }
}

/* ---- External plugin loading (dlopen) ---- */

int tool_plugin_load(const char *so_path) {
    if (!so_path) return -1;

    int before = n_plugins;

    /* Allow the constructor to override existing tools */
    loading_external = 1;
    n_pending_overrides = 0;

    void *handle = dlopen(so_path, RTLD_NOW);

    loading_external = 0;

    if (!handle) {
        fprintf(stderr, "tool_plugin: dlopen '%s': %s\n",
                so_path, dlerror());
        n_pending_overrides = 0;
        return -1;
    }

    int new_count = n_plugins - before;

    /* The .so's constructor should have called tool_plugin_register().
     * Check that at least one new plugin was added or overridden. */
    if (new_count == 0 && n_pending_overrides == 0) {
        fprintf(stderr, "tool_plugin: '%s' registered no tools "
                "(missing TOOL_PLUGIN_REGISTER?)\n", so_path);
        dlclose(handle);
        return -1;
    }

    /* Tag all newly appended plugins with this dlhandle */
    for (int i = before; i < n_plugins; i++)
        plugin_dlhandles[i] = handle;

    /* Tag overridden slots with the new dlhandle and clean up old handles */
    for (int k = 0; k < n_pending_overrides; k++) {
        int slot = pending_override_slots[k];
        plugin_dlhandles[slot] = handle;

        /* dlclose the old handle if it is no longer used by any plugin */
        void *old_h = pending_old_handles[k];
        if (old_h) {
            int still_used = 0;
            for (int j = 0; j < n_plugins; j++) {
                if (plugin_dlhandles[j] == old_h) {
                    still_used = 1;
                    break;
                }
            }
            if (!still_used)
                dlclose(old_h);
        }
    }
    n_pending_overrides = 0;

    return 0;
}

int tool_plugin_load_dir(const char *dir_path) {
    if (!dir_path) return -1;

    DIR *d = opendir(dir_path);
    if (!d) {
        /* Not an error if directory simply doesn't exist yet */
        return -1;
    }

    int loaded = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        const char *name = ent->d_name;
        size_t len = strlen(name);
        if (len < 4 || strcmp(name + len - 3, ".so") != 0)
            continue;

        char path[4096];
        snprintf(path, sizeof(path), "%s/%s", dir_path, name);

        if (tool_plugin_load(path) == 0)
            loaded++;
    }
    closedir(d);
    return loaded;
}

int tool_plugin_unload(const char *name) {
    if (!name) return -1;

    for (int i = 0; i < n_plugins; i++) {
        if (strcmp(plugin_registry[i]->name, name) != 0)
            continue;

        void *handle = plugin_dlhandles[i];

        /* Remove from registry by shifting down */
        for (int j = i; j < n_plugins - 1; j++) {
            plugin_registry[j]  = plugin_registry[j + 1];
            plugin_dlhandles[j] = plugin_dlhandles[j + 1];
        }
        n_plugins--;
        plugin_registry[n_plugins]  = NULL;
        plugin_dlhandles[n_plugins] = NULL;

        /* For dlopen'd plugins, dlclose the handle if no other plugin
         * shares it.  Built-in tools (handle == NULL) have no handle
         * to close - their structs live in the .data segment. */
        if (handle) {
            int still_used = 0;
            for (int j = 0; j < n_plugins; j++) {
                if (plugin_dlhandles[j] == handle) {
                    still_used = 1;
                    break;
                }
            }
            if (!still_used)
                dlclose(handle);
        }

        return 0;
    }
    return -1; /* not found */
}

void tool_plugin_cleanup(void) {
    /* Collect unique dlhandles to close */
    void *closed[TOOL_PLUGIN_MAX];
    int n_closed = 0;

    for (int i = 0; i < n_plugins; i++) {
        void *h = plugin_dlhandles[i];
        if (!h) continue;

        /* Skip if already closed */
        int dup = 0;
        for (int j = 0; j < n_closed; j++) {
            if (closed[j] == h) { dup = 1; break; }
        }
        if (!dup) {
            dlclose(h);
            closed[n_closed++] = h;
        }
        plugin_dlhandles[i] = NULL;
    }
}

/* ---- Parameter schema helpers ---- */

cJSON *tool_params_to_cjson(const tool_param_t *params) {
    if (!params || !params[0].name) return NULL;

    cJSON *schema = cJSON_CreateObject();
    cJSON_AddStringToObject(schema, "type", "object");

    cJSON *props = cJSON_CreateObject();
    cJSON *req = cJSON_CreateArray();

    for (const tool_param_t *p = params; p->name; p++) {
        cJSON *prop = cJSON_CreateObject();
        cJSON_AddStringToObject(prop, "type", p->type);
        if (p->description)
            cJSON_AddStringToObject(prop, "description", p->description);
        if (p->enum_values) {
            cJSON *ev = cJSON_CreateArray();
            for (const char **e = p->enum_values; *e; e++)
                cJSON_AddItemToArray(ev, cJSON_CreateString(*e));
            cJSON_AddItemToObject(prop, "enum", ev);
        }
        if (p->items_type) {
            cJSON *items = cJSON_CreateObject();
            cJSON_AddStringToObject(items, "type", p->items_type);
            cJSON_AddItemToObject(prop, "items", items);
        }
        cJSON_AddItemToObject(props, p->name, prop);
        if (p->required)
            cJSON_AddItemToArray(req, cJSON_CreateString(p->name));
    }

    cJSON_AddItemToObject(schema, "properties", props);
    if (cJSON_GetArraySize(req) > 0)
        cJSON_AddItemToObject(schema, "required", req);
    else
        cJSON_Delete(req);

    return schema;
}

int tool_params_check_required(const tool_param_t *params,
                               const cJSON *actual,
                               char *missing, size_t len) {
    if (!params) return 1;
    for (const tool_param_t *p = params; p->name; p++) {
        if (!p->required) continue;
        const cJSON *val = cJSON_GetObjectItemCaseSensitive(actual, p->name);
        if (!val || (cJSON_IsString(val) &&
                     (!val->valuestring || !val->valuestring[0]))) {
            if (missing && len > 0)
                snprintf(missing, len, "%s", p->name);
            return 0;
        }
    }
    return 1;
}

const char *tool_params_first_required(const tool_param_t *params) {
    if (!params) return NULL;
    for (const tool_param_t *p = params; p->name; p++) {
        if (p->required) return p->name;
    }
    return NULL;
}

/* ---- Registry helpers ---- */

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

/* Run session-end cleanup hooks for all ABI v3+ plugins. */
void tool_plugin_run_cleanups(const char *session_dir) {
    int count = tool_plugin_count();
    for (int i = 0; i < count; i++) {
        const tool_plugin_t *p = tool_plugin_get(i);
        if (p && p->abi_version >= 3 && p->cleanup)
            p->cleanup(session_dir);
    }
}
