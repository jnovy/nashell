#include "tools_internal.h"
#include "tool_plugin.h"
#include "todo_core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>

/* ── Persistent per-workspace TODO list ──────────────── */
/* Storage: {workspace_root}/todo.md  (or {nash_root}/todo.md if no workspace)
 * Format:  GitHub-flavored markdown checkboxes
 *   - [ ] Open item (session:1782139036)
 *   - [x] Completed item
 */

tool_result_t tool_todo(tool_ctx_t *ctx, cJSON *params) {
    cJSON *op_j = cJSON_GetObjectItem(params, "op");
    if (!op_j || !op_j->valuestring)
        return tools_make_error("missing 'op' (add|list|done|remove|purge)");

    const char *op = op_j->valuestring;
    char fpath[NASH_PATH_MAX];
    if (todo_resolve_path(ctx->ws, ctx->memory, fpath, sizeof(fpath)) != 0)
        return tools_make_error("cannot determine todo.md path (no workspace or memory)");

    /* ── ADD ── */
    if (strcmp(op, "add") == 0) {
        cJSON *text_j = cJSON_GetObjectItem(params, "text");
        if (!text_j || !text_j->valuestring || !text_j->valuestring[0])
            return tools_make_error("'add' requires non-empty 'text'");

        const char *text = text_j->valuestring;

        /* Build the line: - [ ] {text} (session:{dir_basename}) */
        char line[4096];
        const char *sess = ctx->session_dir;
        const char *sbase = sess ? strrchr(sess, '/') : NULL;
        if (sbase) sbase++; else sbase = sess;

        if (sbase && sbase[0])
            snprintf(line, sizeof(line), "- [ ] %s (session:%s)", text, sbase);
        else
            snprintf(line, sizeof(line), "- [ ] %s", text);

        /* Load existing lines, append new one, save atomically */
        char **lines = NULL;
        int count = todo_load(fpath, &lines);
        if (count < 0)
            return tools_make_error("cannot read todo.md");

        count = todo_add(&lines, count, line);
        if (count < 0) {
            todo_free_lines(lines, count);
            return tools_make_error("cannot allocate memory for todo.md");
        }

        /* Try creating parent dir if needed */
        if (todo_save(fpath, lines, count) != 0) {
            char *slash = strrchr(fpath, '/');
            if (slash) {
                *slash = '\0';
                mkdir(fpath, 0755);
                *slash = '/';
            }
            if (todo_save(fpath, lines, count) != 0) {
                todo_free_lines(lines, count);
                return tools_make_error("cannot write todo.md");
            }
        }

        int index = count;
        todo_free_lines(lines, count);

        cJSON *meta = cJSON_CreateObject();
        cJSON_AddStringToObject(meta, "status", "ok");
        cJSON_AddNumberToObject(meta, "index", index);
        cJSON_AddStringToObject(meta, "item", line);
        if (ctx->ws && ctx->ws->name)
            cJSON_AddStringToObject(meta, "workspace", ctx->ws->name);

        /* Audit trail */
        char *hash = store_save(ctx->store, line);
        char *alias = tool_register_alias(ctx, hash ? hash : "");
        tools_inject_thought(ctx, params);
        tool_journal(ctx, "todo",
                       params, alias, strlen(line), 0, NULL, NULL);
        free(alias); free(hash);
        return tools_make_result(1, meta, NULL);

    /* ── LIST ── */
    } else if (strcmp(op, "list") == 0) {
        char **lines;
        int count = todo_load(fpath, &lines);

        str_t out = str_new(512);
        int open = 0, done_count = 0;
        for (int i = 0; i < count; i++) {
            str_appendf(&out, "%d. %s\n", i + 1, lines[i]);
            if (strstr(lines[i], "- [ ]")) open++;
            else if (strstr(lines[i], "- [x]")) done_count++;
        }

        cJSON *meta = cJSON_CreateObject();
        cJSON_AddNumberToObject(meta, "count", count);
        cJSON_AddNumberToObject(meta, "open", open);
        cJSON_AddNumberToObject(meta, "done", done_count);
        if (ctx->ws && ctx->ws->name)
            cJSON_AddStringToObject(meta, "workspace", ctx->ws->name);
        if (count > 0)
            cJSON_AddStringToObject(meta, "items", out.data);
        else
            cJSON_AddStringToObject(meta, "items", "(no items)");

        char *hash = store_save(ctx->store, out.data);
        char *alias = tool_register_alias(ctx, hash ? hash : "");
        tools_inject_thought(ctx, params);
        tool_journal(ctx, "todo",
                       params, alias, out.len, count, NULL, NULL);

        char *ref_copy = strdup(alias);
        free(alias); free(hash);
        todo_free_lines(lines, count);
        str_free(&out);
        return tools_make_result(1, meta, ref_copy);

    /* ── DONE ── */
    } else if (strcmp(op, "done") == 0) {
        cJSON *idx_j = cJSON_GetObjectItem(params, "index");
        if (!idx_j || !cJSON_IsNumber(idx_j))
            return tools_make_error("'done' requires 'index' (integer)");
        int idx = (int)cJSON_GetNumberValue(idx_j);

        char **lines;
        int count = todo_load(fpath, &lines);
        const char *err_msg = NULL;
        if (todo_mark_done(lines, count, idx, &err_msg) != 0) {
            todo_free_lines(lines, count);
            return tools_make_error(err_msg ? err_msg : "cannot mark done");
        }

        todo_save(fpath, lines, count);

        cJSON *meta = cJSON_CreateObject();
        cJSON_AddStringToObject(meta, "status", "ok");
        cJSON_AddStringToObject(meta, "item", lines[idx - 1]);

        char *hash = store_save(ctx->store, lines[idx - 1]);
        char *alias = tool_register_alias(ctx, hash ? hash : "");
        tools_inject_thought(ctx, params);
        tool_journal(ctx, "todo",
                       params, alias, strlen(lines[idx - 1]), 0, NULL, NULL);
        free(alias); free(hash);
        todo_free_lines(lines, count);
        return tools_make_result(1, meta, NULL);

    /* ── REMOVE ── */
    } else if (strcmp(op, "remove") == 0) {
        cJSON *idx_j = cJSON_GetObjectItem(params, "index");
        if (!idx_j || !cJSON_IsNumber(idx_j))
            return tools_make_error("'remove' requires 'index' (integer)");
        int idx = (int)cJSON_GetNumberValue(idx_j);

        char **lines;
        int count = todo_load(fpath, &lines);
        char *removed = NULL;
        const char *err_msg = NULL;
        count = todo_remove(lines, count, idx, &removed, &err_msg);
        if (count < 0) {
            todo_free_lines(lines, count);
            return tools_make_error(err_msg ? err_msg : "cannot remove");
        }

        todo_save(fpath, lines, count);

        cJSON *meta = cJSON_CreateObject();
        cJSON_AddStringToObject(meta, "status", "ok");
        cJSON_AddStringToObject(meta, "removed", removed);

        char *hash = store_save(ctx->store, removed);
        char *alias = tool_register_alias(ctx, hash ? hash : "");
        tools_inject_thought(ctx, params);
        tool_journal(ctx, "todo",
                       params, alias, strlen(removed), 0, NULL, NULL);
        free(alias); free(hash); free(removed);
        todo_free_lines(lines, count);
        return tools_make_result(1, meta, NULL);

    /* ── PURGE ── */
    } else if (strcmp(op, "purge") == 0) {
        char **lines;
        int count = todo_load(fpath, &lines);

        int purged = 0;
        int kept = todo_purge(lines, count, &purged);

        todo_save(fpath, lines, kept);

        cJSON *meta = cJSON_CreateObject();
        cJSON_AddStringToObject(meta, "status", "ok");
        cJSON_AddNumberToObject(meta, "purged", purged);
        cJSON_AddNumberToObject(meta, "remaining", kept);

        char info[128];
        snprintf(info, sizeof(info), "purged %d completed items, %d remaining", purged, kept);
        char *hash = store_save(ctx->store, info);
        char *alias = tool_register_alias(ctx, hash ? hash : "");
        tools_inject_thought(ctx, params);
        tool_journal(ctx, "todo",
                       params, alias, strlen(info), 0, NULL, NULL);
        free(alias); free(hash);
        todo_free_lines(lines, kept);
        return tools_make_result(1, meta, NULL);

    } else {
        return tools_make_error("unknown op (use: add, list, done, remove, purge)");
    }
}

/* ── plugin registration ──────────────────────────────── */

static const tool_param_t todo_params[] = {
    TOOL_PARAM("op",    "string",  "Operation: add, list, done, remove, purge", 1),
    TOOL_PARAM("text",  "string",  "TODO text (for add)",                       0),
    TOOL_PARAM("index", "integer", "Item number (for done/remove)",             0),
    TOOL_PARAM_END
};

static const tool_plugin_t todo_plugin =
    TOOL_DEF("todo",
             "Persistent per-workspace TODO list that survives across sessions. Use to park findings, ideas, or action items for later. Stored in todo.md within the active workspace directory (human-editable).",
             todo_params, tool_todo);
TOOL_PLUGIN_REGISTER(todo_plugin)
