#include "tools_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>

/* ── file_read ───────────────────────────────────────── */

tool_result_t tool_file_read(tool_ctx_t *ctx, cJSON *params) {
    cJSON *path_j = cJSON_GetObjectItem(params, "path");
    if (!path_j || !path_j->valuestring || !path_j->valuestring[0])
        return tools_make_error("file_read requires a non-empty 'path' string. "
                          "Provide the file path to read.");

    const char *path = path_j->valuestring;

    /* Resolve aliases and store/ paths */
    char *resolved = NULL;
    char resolved_buf[NASH_PATH_MAX];
    path = tools_resolve_path(ctx, path, resolved_buf, &resolved);

    /* Safety: check file type and size before reading */
    struct stat st;
    if (stat(path, &st) != 0) {
        char msg[4224];
        snprintf(msg, sizeof(msg), "cannot stat '%.4095s': %s", path, strerror(errno));
        return tools_make_error(msg);
    }
    if (!S_ISREG(st.st_mode)) {
        return tools_make_error("not a regular file (refusing to read pipes, devices, etc.)");
    }
    int max_file = ctx->cfg ? ctx->cfg->file_max_size : 52428800;  /* 50MB default */
    if (st.st_size > max_file) {
        char msg[256];
        snprintf(msg, sizeof(msg), "file too large (%ld bytes, limit %d)", (long)st.st_size, max_file);
        return tools_make_error(msg);
    }

    size_t len = 0;
    char *content = slurp_file(path, &len);
    if (!content) {
        char msg[4224];
        snprintf(msg, sizeof(msg), "cannot read '%.4095s': %s", path, strerror(errno));
        return tools_make_error(msg);
    }

    /* Detect binary files: check for null bytes in the first 8KB.
     * Binary data (images, executables, etc.) cannot be meaningfully
     * displayed as text and will corrupt JSON payloads when sent to
     * the LLM provider, causing HTTP 400 errors. */
    {
        size_t check_len = len < 8192 ? len : 8192;
        for (size_t i = 0; i < check_len; i++) {
            if (content[i] == '\0') {
                free(content);
                free(resolved);  /* free heap-allocated alias resolution */
                char msg[4224];
                snprintf(msg, sizeof(msg),
                    "'%.4060s' appears to be a binary file (not text). "
                    "file_read only supports text files. For images, "
                    "use image_analyze instead.", path);
                return tools_make_error(msg);
            }
        }
    }

    int total_lines = count_lines(content);

    /* Line range support: start_line and end_line parameters.
     * - start_line: 1-based (default: 1). Negative = from end (tail).
     * - end_line: 1-based inclusive (default: EOF).
     * - No params = full file (backward compatible).
     * Eliminates the need for shell_exec sed/head/tail hacks. */
    cJSON *sl_j = cJSON_GetObjectItem(params, "start_line");
    cJSON *el_j = cJSON_GetObjectItem(params, "end_line");
    int start_line = sl_j ? (int)cJSON_GetNumberValue(sl_j) : 0;
    int end_line = el_j ? (int)cJSON_GetNumberValue(el_j) : 0;

    char *display_content = NULL;  /* content to show (with line numbers if range) */
    int display_lines = total_lines;
    size_t display_len = len;
    int range_start = 1, range_end = total_lines;

    if (start_line != 0 || end_line != 0) {
        /* Resolve line range */
        if (start_line < 0) {
            /* Negative = from end: start_line=-20 means last 20 lines */
            range_start = total_lines + start_line + 1;
            if (range_start < 1) range_start = 1;
            range_end = total_lines;
        } else {
            range_start = start_line > 0 ? start_line : 1;
            range_end = end_line > 0 ? end_line : total_lines;
        }
        if (range_start > total_lines) range_start = total_lines;
        if (range_end > total_lines) range_end = total_lines;
        if (range_end < range_start) range_end = range_start;

        /* Extract lines and prepend line numbers */
        str_t out = str_new(4096);
        const char *p = content;
        int line_num = 1;
        while (*p) {
            const char *eol = strchr(p, '\n');
            int line_len = eol ? (int)(eol - p) : (int)strlen(p);

            if (line_num >= range_start && line_num <= range_end) {
                /* Prepend line number for precise file_edit targeting */
                str_appendf(&out, "%4d: ", line_num);
                str_append(&out, p, line_len);
                str_append_cstr(&out, "\n");
            }

            if (!eol) break;
            p = eol + 1;
            line_num++;
            if (line_num > range_end) break;  /* early exit */
        }

        display_content = str_steal(&out);
        display_len = strlen(display_content);
        display_lines = range_end - range_start + 1;
    }

    const char *store_content = display_content ? display_content : content;
    char *hash = store_save(ctx->store, store_content);
    char *alias = tool_register_alias(ctx, hash ? hash : "");

    cJSON *meta = cJSON_CreateObject();
    cJSON_AddStringToObject(meta, "path", path_j->valuestring);
    cJSON_AddNumberToObject(meta, "total_lines", total_lines);
    if (display_content) {
        /* Range mode: show which lines */
        char showing[64];
        snprintf(showing, sizeof(showing), "%d-%d of %d",
                 range_start, range_end, total_lines);
        cJSON_AddStringToObject(meta, "showing", showing);
        cJSON_AddNumberToObject(meta, "lines", display_lines);
    } else {
        cJSON_AddNumberToObject(meta, "lines", total_lines);
    }
    cJSON_AddNumberToObject(meta, "chars", (double)display_len);
    cJSON_AddStringToObject(meta, "ref", alias);

    /* Compute max inline chars: % of context window, or fixed limit fallback.
     * file_read_context_pct (default 10) caps inline content to N% of the
     * context window in chars (context_size × chars_per_token × pct/100).
     * Falls back to file_read_max_inline (default 50000) when context_size
     * is unknown (e.g. auto-detect not yet resolved). */
    int max_inline = ctx->cfg->file_read_max_inline;
    if (ctx->cfg->file_read_context_pct > 0 && ctx->provider &&
        ctx->provider->cfg.context_size > 0) {
        float cpt = ctx->provider->cfg.chars_per_token > 0
                   ? ctx->provider->cfg.chars_per_token : 3.5f;
        int ctx_chars = (int)((double)ctx->provider->cfg.context_size * cpt
                             * ctx->cfg->file_read_context_pct / 100.0);
        if (ctx_chars > 0 && ctx_chars < max_inline)
            max_inline = ctx_chars;
    }

    /* file_read MUST return content — that's its purpose */
    if ((int)display_len <= max_inline) {
        cJSON_AddStringToObject(meta, "content", store_content);
    } else {
        char *trunc = malloc(max_inline + 1);
        if (trunc) {
            utf8_truncate(trunc, store_content, max_inline);
            /* Truncate at line boundary so we don't cut mid-line */
            size_t trunc_len = strlen(trunc);
            for (size_t i = trunc_len; i > 0; i--) {
                if (trunc[i - 1] == '\n') {
                    trunc[i] = '\0';
                    trunc_len = i;
                    break;
                }
            }
            /* Count lines in truncated content to report truncation point */
            int trunc_lines = 0;
            for (size_t i = 0; i < trunc_len; i++) {
                if (trunc[i] == '\n') trunc_lines++;
            }
            /* truncated_at_line: absolute file line number where content was cut.
             * In range mode, offset by range_start; otherwise 1-based line count. */
            int trunc_at_line = display_content
                ? range_start + trunc_lines - 1
                : trunc_lines;

            cJSON_AddStringToObject(meta, "content", trunc);
            cJSON_AddBoolToObject(meta, "truncated", 1);
            cJSON_AddNumberToObject(meta, "truncated_at_line", trunc_at_line);
            free(trunc);
        }
    }

    tools_inject_thought(ctx, params);
    tool_journal(ctx, "file_read", params, alias,
                   display_len, display_lines, NULL, NULL);

    char *ref_copy = strdup(alias);
    free(alias);
    free(content);
    free(display_content);  /* NULL-safe: free line-range extracted content */
    free(hash);
    free(resolved);  /* BUG 2 fix: free heap-allocated alias resolution */
    return tools_make_result(1, meta, ref_copy);
}

/* ── file_write ──────────────────────────────────────── */

tool_result_t tool_file_write(tool_ctx_t *ctx, cJSON *params) {
    cJSON *path_j = cJSON_GetObjectItem(params, "path");
    cJSON *content_j = cJSON_GetObjectItem(params, "content");
    if (!path_j || !path_j->valuestring || !path_j->valuestring[0])
        return tools_make_error("file_write requires a non-empty 'path' string.");
    if (!content_j || !content_j->valuestring)
        return tools_make_error("file_write requires a 'content' parameter.");

    const char *path = path_j->valuestring;
    const char *content = content_j->valuestring;

    /* FIX 5b: Create parent directories if they don't exist.
     * Previously file_write to a non-existent directory path failed
     * with a cryptic errno message. */
    {
        char parent[NASH_PATH_MAX];
        snprintf(parent, sizeof(parent), "%s", path);
        char *last_slash = strrchr(parent, '/');
        if (last_slash && last_slash != parent) {
            *last_slash = '\0';
            struct stat st;
            if (stat(parent, &st) != 0) {
                /* Recursive mkdir: walk forward creating each component */
                for (char *p = parent + 1; *p; p++) {
                    if (*p == '/') {
                        *p = '\0';
                        mkdir(parent, 0755);
                        *p = '/';
                    }
                }
                mkdir(parent, 0755);
            }
        }
    }

    size_t len = strlen(content);
    if (write_file(path, content, len) != 0) {
        char msg[512];
        snprintf(msg, sizeof(msg), "cannot write '%s': %s", path, strerror(errno));
        return tools_make_error(msg);
    }

    /* Store written content for full audit trail */
    char *hash = store_save(ctx->store, content);
    char *alias = tool_register_alias(ctx, hash ? hash : "");

    cJSON *meta = cJSON_CreateObject();
    cJSON_AddStringToObject(meta, "status", "ok");
    cJSON_AddStringToObject(meta, "path", path);
    cJSON_AddNumberToObject(meta, "bytes", (double)len);
    cJSON_AddStringToObject(meta, "ref", alias);

    tools_inject_thought(ctx, params);
    tool_journal(ctx, "file_write", params, alias,
                   len, count_lines(content), NULL, NULL);

    char *ref_copy = strdup(alias);
    free(alias);
    free(hash);
    return tools_make_result(1, meta, ref_copy);
}

/* ── file_edit ───────────────────────────────────────── */

tool_result_t tool_file_edit(tool_ctx_t *ctx, cJSON *params) {
    cJSON *path_j     = cJSON_GetObjectItem(params, "path");
    cJSON *old_text_j = cJSON_GetObjectItem(params, "old_text");
    cJSON *new_text_j = cJSON_GetObjectItem(params, "new_text");
    if (!path_j || !path_j->valuestring || !path_j->valuestring[0])
        return tools_make_error("file_edit requires a non-empty 'path' string.");
    if (!old_text_j || !old_text_j->valuestring || !old_text_j->valuestring[0])
        return tools_make_error("file_edit requires a non-empty 'old_text' string. "
                          "Copy the exact text to replace from the file. "
                          "Use file_read first to see the current content.");
    if (!new_text_j || !new_text_j->valuestring)
        return tools_make_error("file_edit requires a 'new_text' parameter.");

    const char *path = path_j->valuestring;
    const char *old_text = old_text_j->valuestring;
    const char *new_text = new_text_j->valuestring;

    size_t flen = 0;
    char *content = slurp_file(path, &flen);
    if (!content) {
        char msg[512];
        snprintf(msg, sizeof(msg), "cannot read '%s': %s", path, strerror(errno));
        return tools_make_error(msg);
    }

    /* Store pre-edit content */
    char *pre_hash = store_save(ctx->store, content);
    char *pre_alias = tool_register_alias(ctx, pre_hash ? pre_hash : "");

    size_t old_len = strlen(old_text);
    char *pos = strstr(content, old_text);
    if (!pos) {
        free(content);
        free(pre_hash);
        free(pre_alias);
        return tools_make_error("old_text not found in file");
    }

    /* FIX #1: Detect multiple matches — if old_text appears more than once,
     * the caller must provide more context to disambiguate. Silently replacing
     * only the first match causes data corruption when the intent was to edit
     * a specific occurrence. */
    {
        char *second = strstr(pos + old_len, old_text);
        if (second) {
            /* Count total matches for a helpful error message */
            int match_count = 2;
            char *scan = second;
            while ((scan = strstr(scan + old_len, old_text)) != NULL)
                match_count++;
            free(content);
            free(pre_hash);
            free(pre_alias);
            char errmsg[256];
            snprintf(errmsg, sizeof(errmsg),
                     "old_text matches %d locations in %s — provide more "
                     "surrounding context to disambiguate", match_count, path);
            return tools_make_error(errmsg);
        }
    }

    /* Build new content */
    size_t new_len = strlen(new_text);
    size_t result_len = flen - old_len + new_len;
    char *result = malloc(result_len + 1);
    if (!result) { free(content); free(pre_hash); free(pre_alias); return tools_make_error("malloc failed"); }

    size_t prefix_len = (size_t)(pos - content);
    memcpy(result, content, prefix_len);
    memcpy(result + prefix_len, new_text, new_len);
    memcpy(result + prefix_len + new_len, pos + old_len, flen - prefix_len - old_len);
    result[result_len] = '\0';

    if (write_file(path, result, result_len) < 0) {
        free(content); free(result); free(pre_hash); free(pre_alias);
        return tools_make_error("cannot write file");
    }

    /* Store post-edit content */
    char *post_hash = store_save(ctx->store, result);
    char *post_alias = tool_register_alias(ctx, post_hash ? post_hash : "");

    /* Generate diff output with line numbers (matching nashell format).
     * Format: "  Added N lines, removed M lines\n"
     *         "  {lnum:5d} +added_line\n"
     *         "  {lnum:5d} -removed_line\n"
     *         "  {lnum:5d}  context_line\n"
     * The TUI's md_render detects this format in ```diff blocks. */
    {
        /* Count lines before the edit for context — walk backward from pos */
        /* First, find the start of the line containing pos */
        char *line_start = pos;
        {
            char *scan = pos - 1;
            while (scan >= content && *scan != '\n') scan--;
            line_start = scan + 1;
        }

        /* Compute 1-based line number of line_start */
        int start_lnum = 1;
        for (const char *p = content; p < line_start; p++)
            if (*p == '\n') start_lnum++;

        /* Walk backward from the byte before line_start, skipping the
         * newline that terminates the line before the edit line. Then
         * count ctx_before more newlines to find our context boundary. */
        int ctx_before = 3;
        char *ctx_start = line_start;
        char *scan = line_start - 1;

        /* Skip the newline immediately before line_start (it belongs to
         * the previous line, not to our context count) */
        if (scan >= content && *scan == '\n') scan--;

        /* Now count ctx_before newlines walking backward */
        int found = 0;
        while (scan >= content && found < ctx_before) {
            if (*scan == '\n') {
                found++;
                if (found == ctx_before) {
                    ctx_start = scan + 1;
                    break;
                }
            }
            scan--;
        }
        if (scan < content && found < ctx_before) {
            ctx_start = content;
        }

        /* Compute 1-based line number of ctx_start */
        int ctx_start_lnum = 1;
        for (const char *p = content; p < ctx_start; p++)
            if (*p == '\n') ctx_start_lnum++;

        /* Find context after the edit */
        char *after_edit = result + (size_t)(pos - content) + new_len;
        size_t after_len = (result + result_len) - after_edit;
        char *ctx_end = after_edit;
        for (int i = 0; i < ctx_before; i++) {
            char *nl = memchr(ctx_end, '\n', after_len > (size_t)(ctx_end - after_edit) ? after_len - (size_t)(ctx_end - after_edit) : 0);
            if (!nl) break;
            ctx_end = nl + 1;
        }
        if (ctx_end > result + result_len) ctx_end = result + result_len;

        /* Build diff output — summary is prepended after computing actual diff. */
        size_t diff_cap = 4096;
        char *diff = malloc(diff_cap);
        int diff_len = 0;
        int actual_added = 0, actual_removed = 0;  /* filled by diff algorithm */

        /* Track line numbers: old_lnum for removed, new_lnum for added */
        int old_lnum = ctx_start_lnum;
        int new_lnum = ctx_start_lnum;

        /* Context before — stop at line_start (start of edit line), not pos */
        char *cl = ctx_start;
        while (cl < line_start) {
            char *nl = strchr(cl, '\n');
            int llen = nl ? (int)(nl - cl) : (int)(line_start - cl);
            if ((size_t)(diff_len + llen + 16) >= diff_cap) {
                diff_cap *= 2;
                diff = realloc(diff, diff_cap);
            }
            diff_len += snprintf(diff + diff_len, diff_cap - (size_t)diff_len,
                                 "  %5d  %.*s\n", new_lnum, llen, cl);
            old_lnum++;
            new_lnum++;
            cl = nl ? nl + 1 : cl + llen;
        }

        /* Line-level diff: find common prefix/suffix between old and new,
         * emit shared lines as context, only truly changed lines as +/-.
         * This avoids showing identical boundary lines as removed+added. */
        {
            /* Split old_text and new_text into line arrays */
            typedef struct { const char *s; int len; } dline_t;
            int old_cap = 64, new_cap = 64;
            int old_cnt = 0, new_cnt = 0;
            dline_t *old_lines = malloc((size_t)old_cap * sizeof(dline_t));
            dline_t *new_lines = malloc((size_t)new_cap * sizeof(dline_t));

            /* Parse old_text into lines */
            const char *p = pos;
            while (p < pos + old_len) {
                const char *nl = memchr(p, '\n', old_len - (size_t)(p - pos));
                int llen = nl ? (int)(nl - p) : (int)(pos + old_len - p);
                if (old_cnt >= old_cap) {
                    old_cap *= 2;
                    old_lines = realloc(old_lines, (size_t)old_cap * sizeof(dline_t));
                }
                old_lines[old_cnt++] = (dline_t){ p, llen };
                p = nl ? nl + 1 : p + llen;
            }

            /* Parse new_text into lines */
            p = new_text;
            while (p < new_text + new_len) {
                const char *nl = memchr(p, '\n', new_len - (size_t)(p - new_text));
                int llen = nl ? (int)(nl - p) : (int)(new_text + new_len - p);
                if (new_cnt >= new_cap) {
                    new_cap *= 2;
                    new_lines = realloc(new_lines, (size_t)new_cap * sizeof(dline_t));
                }
                new_lines[new_cnt++] = (dline_t){ p, llen };
                p = nl ? nl + 1 : p + llen;
            }

            /* Find common prefix lines */
            int prefix = 0;
            while (prefix < old_cnt && prefix < new_cnt &&
                   old_lines[prefix].len == new_lines[prefix].len &&
                   memcmp(old_lines[prefix].s, new_lines[prefix].s,
                          (size_t)old_lines[prefix].len) == 0)
                prefix++;

            /* Find common suffix lines (don't overlap with prefix) */
            int suffix = 0;
            while (suffix < (old_cnt - prefix) && suffix < (new_cnt - prefix) &&
                   old_lines[old_cnt - 1 - suffix].len == new_lines[new_cnt - 1 - suffix].len &&
                   memcmp(old_lines[old_cnt - 1 - suffix].s,
                          new_lines[new_cnt - 1 - suffix].s,
                          (size_t)old_lines[old_cnt - 1 - suffix].len) == 0)
                suffix++;

            /* Emit common prefix as context */
            for (int i = 0; i < prefix; i++) {
                dline_t *dl = &new_lines[i];
                if ((size_t)(diff_len + dl->len + 16) >= diff_cap) {
                    diff_cap *= 2;
                    diff = realloc(diff, diff_cap);
                }
                diff_len += snprintf(diff + diff_len, diff_cap - (size_t)diff_len,
                                     "  %5d  %.*s\n", new_lnum, dl->len, dl->s);
                old_lnum++;
                new_lnum++;
            }

            /* Emit removed lines (middle section of old) */
            for (int i = prefix; i < old_cnt - suffix; i++) {
                dline_t *dl = &old_lines[i];
                if ((size_t)(diff_len + dl->len + 16) >= diff_cap) {
                    diff_cap *= 2;
                    diff = realloc(diff, diff_cap);
                }
                diff_len += snprintf(diff + diff_len, diff_cap - (size_t)diff_len,
                                     "  %5d -%.*s\n", old_lnum, dl->len, dl->s);
                old_lnum++;
                actual_removed++;
            }

            /* Emit added lines (middle section of new) */
            for (int i = prefix; i < new_cnt - suffix; i++) {
                dline_t *dl = &new_lines[i];
                if ((size_t)(diff_len + dl->len + 16) >= diff_cap) {
                    diff_cap *= 2;
                    diff = realloc(diff, diff_cap);
                }
                diff_len += snprintf(diff + diff_len, diff_cap - (size_t)diff_len,
                                     "  %5d +%.*s\n", new_lnum, dl->len, dl->s);
                new_lnum++;
                actual_added++;
            }

            /* Emit common suffix as context */
            for (int i = old_cnt - suffix; i < old_cnt; i++) {
                dline_t *dl = &new_lines[new_cnt - suffix + (i - (old_cnt - suffix))];
                if ((size_t)(diff_len + dl->len + 16) >= diff_cap) {
                    diff_cap *= 2;
                    diff = realloc(diff, diff_cap);
                }
                diff_len += snprintf(diff + diff_len, diff_cap - (size_t)diff_len,
                                     "  %5d  %.*s\n", new_lnum, dl->len, dl->s);
                old_lnum++;
                new_lnum++;
            }

            free(old_lines);
            free(new_lines);
        }

        /* Context after — use new_lnum (post-edit line numbers) */
        cl = after_edit;
        while (cl < ctx_end) {
            char *nl = strchr(cl, '\n');
            int llen = nl ? (int)(nl - cl) : (int)(ctx_end - cl);
            if ((size_t)(diff_len + llen + 16) >= diff_cap) {
                diff_cap *= 2;
                diff = realloc(diff, diff_cap);
            }
            diff_len += snprintf(diff + diff_len, diff_cap - (size_t)diff_len,
                                 "  %5d  %.*s\n", new_lnum, llen, cl);
            new_lnum++;
            cl = nl ? nl + 1 : cl + llen;
        }

        /* Prepend summary header with actual counts */
        {
            char hdr[64];
            int hlen = snprintf(hdr, sizeof(hdr),
                                "  Added %d lines, removed %d lines\n",
                                actual_added, actual_removed);
            /* Grow buffer if needed, then shift body right and insert header */
            if ((size_t)(diff_len + hlen + 1) >= diff_cap) {
                diff_cap = (size_t)(diff_len + hlen + 64);
                diff = realloc(diff, diff_cap);
            }
            memmove(diff + hlen, diff, (size_t)diff_len + 1);  /* +1 for NUL */
            memcpy(diff, hdr, (size_t)hlen);
            diff_len += hlen;
        }

        /* Store diff as the display ref */
        char *diff_hash = store_save(ctx->store, diff);
        char *diff_alias = tool_register_alias(ctx, diff_hash ? diff_hash : "");
        free(diff);
        free(diff_hash);

        cJSON *meta = cJSON_CreateObject();
        cJSON_AddStringToObject(meta, "status", "ok");
        cJSON_AddStringToObject(meta, "path", path);
        cJSON_AddStringToObject(meta, "pre_ref", pre_alias);
        cJSON_AddStringToObject(meta, "post_ref", post_alias);

        tools_inject_thought(ctx, params);
        tool_journal(ctx, "file_edit", params, diff_alias,
                       diff_len, 0, NULL, NULL);

        free(content);
        free(result);
        free(pre_alias);
        free(pre_hash);
        free(post_hash);
        free(post_alias);
        return tools_make_result(1, meta, diff_alias);
    }
}
