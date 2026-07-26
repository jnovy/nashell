#include "tools_internal.h"
#include "tool_plugin.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>

/* Forward declarations */
static void todo_free_lines(char **lines, int count);

/* ── Persistent per-workspace TODO list ──────────────── */
/* Storage: {workspace_root}/todo.md  (or {nash_root}/todo.md if no workspace)
 * Format:  GitHub-flavored markdown checkboxes
 *   - [ ] Open item (session:1782139036)
 *   - [x] Completed item
 */

/* Derive the todo.md path from the active workspace.
 * Uses memory_dir() on the appropriate memory_t, strips "/memory" suffix,
 * and appends "/todo.md". */
static int todo_path(tool_ctx_t *ctx, char *buf, size_t sz) {
    const char *mdir = NULL;

    /* Prefer workspace memory if active, else global */
    if (ctx->ws && ctx->ws->workspace)
        mdir = memory_dir(ctx->ws->workspace);
    else if (ctx->ws && ctx->ws->global)
        mdir = memory_dir(ctx->ws->global);
    else if (ctx->memory)
        mdir = memory_dir(ctx->memory);

    if (!mdir) return -1;

    /* mdir is "{root}/memory" — strip the "/memory" suffix to get root */
    size_t mlen = strlen(mdir);
    const char *suffix = "/memory";
    size_t slen = strlen(suffix);

    if (mlen > slen && strcmp(mdir + mlen - slen, suffix) == 0) {
        /* Build "{root}/todo.md" */
        size_t rlen = mlen - slen;
        if (rlen + sizeof("/todo.md") > sz) return -1;
        memcpy(buf, mdir, rlen);
        memcpy(buf + rlen, "/todo.md", sizeof("/todo.md")); /* includes NUL */
    } else {
        /* Fallback: just put todo.md next to the memory dir */
        snprintf(buf, sz, "%s/../todo.md", mdir);
    }
    return 0;
}

/* Load all lines from todo.md into a dynamic array.
 * Returns line count; *lines_out is heap-allocated (caller frees each + array).
 * Returns 0 with *lines_out = NULL if file doesn't exist. */
static int todo_load(const char *path, char ***lines_out) {
    *lines_out = NULL;
    FILE *f = fopen(path, "r");
    if (!f) return 0;

    char **lines = NULL;
    int count = 0, cap = 0;
    char linebuf[4096];

    while (fgets(linebuf, sizeof(linebuf), f)) {
        /* Strip trailing newline */
        size_t len = strlen(linebuf);
        while (len > 0 && (linebuf[len-1] == '\n' || linebuf[len-1] == '\r'))
            linebuf[--len] = '\0';

        /* Skip empty lines */
        if (len == 0) continue;

        if (count >= cap) {
            cap = cap ? cap * 2 : 16;
            char **tmp = realloc(lines, sizeof(char *) * (size_t)cap);
            if (!tmp) {
                todo_free_lines(lines, count);
                fclose(f);
                return -1;
            }
            lines = tmp;
        }
        char *dup = strdup(linebuf);
        if (!dup) {
            todo_free_lines(lines, count);
            fclose(f);
            return -1;
        }
        lines[count++] = dup;
    }
    fclose(f);
    *lines_out = lines;
    return count;
}

/* Write lines back to file atomically (.tmp + rename). */
static int todo_save(const char *path, char **lines, int count) {
    char tmp[NASH_PATH_MAX + 8];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);

    FILE *f = fopen(tmp, "w");
    if (!f) return -1;

    for (int i = 0; i < count; i++)
        fprintf(f, "%s\n", lines[i]);

    fclose(f);
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

static void todo_free_lines(char **lines, int count) {
    if (!lines) return;
    for (int i = 0; i < count; i++)
        free(lines[i]);
    free(lines);
}

tool_result_t tool_todo(tool_ctx_t *ctx, cJSON *params) {
    cJSON *op_j = cJSON_GetObjectItem(params, "op");
    if (!op_j || !op_j->valuestring)
        return tools_make_error("missing 'op' (add|list|done|remove|purge)");

    const char *op = op_j->valuestring;
    char fpath[NASH_PATH_MAX];
    if (todo_path(ctx, fpath, sizeof(fpath)) != 0)
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

        /* Grow by one slot for the new item */
        char **tmp = realloc(lines, sizeof(char *) * (size_t)(count + 1));
        if (!tmp) {
            todo_free_lines(lines, count);
            return tools_make_error("cannot allocate memory for todo.md");
        }
        lines = tmp;
        lines[count++] = strdup(line);
        if (!lines[count - 1]) {
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
        if (idx < 1 || idx > count) {
            todo_free_lines(lines, count);
            return tools_make_error("index out of range");
        }

        /* Flip - [ ] → - [x] */
        char *line = lines[idx - 1];
        char *check = strstr(line, "- [ ]");
        if (!check) {
            todo_free_lines(lines, count);
            return tools_make_error("item is not open (already done?)");
        }
        /* Replace [ ] with [x] — [x] is same width as [ ] */
        check[3] = 'x';

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
        if (idx < 1 || idx > count) {
            todo_free_lines(lines, count);
            return tools_make_error("index out of range");
        }

        char *removed = strdup(lines[idx - 1]);

        /* Shift lines down */
        free(lines[idx - 1]);
        for (int i = idx - 1; i < count - 1; i++)
            lines[i] = lines[i + 1];
        count--;

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

        /* Keep only non-[x] lines */
        int kept = 0, purged = 0;
        for (int i = 0; i < count; i++) {
            if (strstr(lines[i], "- [x]")) {
                free(lines[i]);
                lines[i] = NULL;
                purged++;
            }
        }
        /* Compact */
        for (int i = 0; i < count; i++) {
            if (lines[i])
                lines[kept++] = lines[i];
        }

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
