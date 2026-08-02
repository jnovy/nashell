#include "tools_internal.h"
#include "tool_plugin.h"
#include "subprocess.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

/* ── grep_search ─────────────────────────────────────── */

tool_result_t tool_grep_search(tool_ctx_t *ctx, cJSON *params) {
    TOOL_REQ_STR(params, "pattern", pattern);
    cJSON *path_j    = cJSON_GetObjectItem(params, "path");
    const char *path = path_j && path_j->valuestring && path_j->valuestring[0]
                       ? path_j->valuestring : ".";

    /* Resolve aliases and store/ paths */
    char *resolved = NULL;
    char resolved_path[NASH_PATH_MAX];
    path = tools_resolve_path(ctx, path, resolved_path, &resolved);

    int max_matches = ctx->cfg ? ctx->cfg->grep_max_matches : 50;
    if (max_matches <= 0) max_matches = 50;
    char max_matches_str[16];
    snprintf(max_matches_str, sizeof(max_matches_str), "%d", max_matches);

    char *const argv[] = {
        "grep", "-rn", "--binary-files=without-match",
        "--exclude-dir=.git", "--exclude-dir=node_modules",
        "--exclude-dir=__pycache__", "--exclude-dir=.tox",
        "--exclude-dir=vendor", "--exclude-dir=target",
        "--exclude-dir=build", "--exclude-dir=dist",
        "--exclude=*.o", "--exclude=*.a", "--exclude=*.so",
        "--exclude=*.dylib", "--exclude=*.pyc",
        "-m", max_matches_str,
        (char *)pattern, (char *)path, NULL
    };

    int grep_timeout = ctx->cfg ? ctx->cfg->grep_timeout : 60;
    int grep_max = ctx->cfg ? ctx->cfg->shell_max_output : 512000;
    str_t out = str_new(4096);
    subprocess_result_t r = subprocess_run(argv, NULL, grep_timeout,
                                           grep_max, 0,
                                           SUBPROCESS_PIPE_STDERR, &out);
    if (r.output_capped)
        str_append_cstr(&out, "\n... [output truncated at limit]\n");

    /* grep -m N limits per-file, not globally. Truncate to max_matches total
     * lines so a recursive search doesn't return N * num_files matches. */
    int matches = count_lines(out.data);
    if (matches > max_matches && out.data) {
        char *p = out.data;
        for (int i = 0; i < max_matches && *p; i++) {
            p = strchr(p, '\n');
            if (!p) break;
            p++;
        }
        if (p && *p) {
            int shown = max_matches;
            int omitted = matches - shown;
            /* Truncate and append notice */
            size_t keep = (size_t)(p - out.data);
            out.len = keep;
            out.data[keep] = '\0';
            char notice[128];
            snprintf(notice, sizeof(notice),
                     "... [%d more matches omitted, showing first %d]\n",
                     omitted, shown);
            str_append_cstr(&out, notice);
            matches = shown;
        }
    }
    char *hash = store_save(ctx->store, out.data);
    char *alias = tool_register_alias(ctx, hash ? hash : "");

    cJSON *meta = cJSON_CreateObject();
    cJSON_AddStringToObject(meta, "pattern", pattern);
    cJSON_AddStringToObject(meta, "path", path_j && path_j->valuestring ? path_j->valuestring : ".");
    cJSON_AddNumberToObject(meta, "matches", matches);
    cJSON_AddNumberToObject(meta, "chars", (double)out.len);
    cJSON_AddStringToObject(meta, "ref", alias);

    tools_inject_thought(ctx, params);
    tool_journal(ctx, "grep_search", params, alias,
                   out.len, matches, NULL, NULL);

    char *ref_copy = strdup(alias);
    free(alias);
    str_free(&out);
    free(hash);
    free(resolved);  /* BUG 2 fix: free heap-allocated resolved path */
    return tools_make_result(1, meta, ref_copy);
}

/* ── glob_search ──────────────────────────────────────── */

/* Parse glob pattern: extracts directory path and glob name.
 * Patterns supported: bare globs, dir/glob, doublestar globs, exact paths
 * Outputs: root (search directory), name (glob pattern), recursive/exact flags
 */

static void parse_glob_pattern(const char *pattern,
                               char *root, size_t root_sz,
                               char *name, size_t name_sz,
                               int *recursive, int *exact,
                               char *exact_path, size_t exact_path_sz) {
    /* FIX #7: Use root_sz for bounds checking (was previously suppressed). */
    const char *p = pattern;
    *recursive = 0;
    *exact = 0;
    root[0] = '.';
    root[1] = '\0';
    name[0] = '\0';
    exact_path[0] = '\0';

    /* Strip leading ./ */
    if (p[0] == '.' && p[1] == '/')
        p += 2;

    /* Handle doublestar/ prefix (recursive from root) */
    if (p[0] == '*' && p[1] == '*' && p[2] == '/') {
        *recursive = 1;
        p += 3;
    }

    /* Find the last '/' to separate directory from basename */
    const char *last_slash = strrchr(p, '/');

    if (last_slash != NULL) {
        /* Check if the part after last slash contains glob chars */
        const char *after_slash = last_slash + 1;
        int glob_after = (strchr(after_slash, '*') != NULL
                        || strchr(after_slash, '?') != NULL
                        || strchr(after_slash, '[') != NULL);

        if (glob_after) {
            /* Directory prefix with glob basename */
            size_t dir_len = (size_t)(last_slash - p);

            /* Check if directory part ends with doublestar/ (recursive within dir) */
            if (dir_len >= 3 && last_slash[-1] == '*'
                && last_slash[-2] == '*' && last_slash[-3] == '/') {
                dir_len -= 3;  /* strip the doublestar/ from directory part */
                *recursive = 1;
            }

            /* FIX #7: Clamp dir_len to root buffer size */
            if (dir_len >= root_sz) dir_len = root_sz - 1;
            if (dir_len > 0) {
                strncpy(root, p, dir_len);
                root[dir_len] = '\0';
            }
            strncpy(name, after_slash, name_sz - 1);
            name[name_sz - 1] = '\0';
        } else {
            /* Exact file path, e.g. src/main.c */
            *exact = 1;
            strncpy(exact_path, p, exact_path_sz - 1);
            exact_path[exact_path_sz - 1] = '\0';
        }
    } else {
        /* No slash — bare glob pattern, search recursively from root */
        *recursive = 1;
        strncpy(name, p, name_sz - 1);
        name[name_sz - 1] = '\0';
    }
}

tool_result_t tool_glob_search(tool_ctx_t *ctx, cJSON *params) {
    TOOL_REQ_STR(params, "pattern", pattern);
    cJSON *path_j = cJSON_GetObjectItem(params, "path");
    const char *search_path = path_j && path_j->valuestring ? path_j->valuestring : ".";

    /* Parse the glob pattern into components */
    char root[1024], name[1024], exact_path[1024];
    int recursive, exact;
    parse_glob_pattern(pattern, root, sizeof(root), name, sizeof(name),
                       &recursive, &exact, exact_path, sizeof(exact_path));

    /* Build find arguments — use fork/execvp to avoid shell injection.
     * (grep_search already uses fork/execlp for the same reason.) */
    char full_path[NASH_PATH_MAX];

    /* For exact file path, just check existence directly */
    if (exact) {
        if (exact_path[0] == '/')
            snprintf(full_path, sizeof(full_path), "%s", exact_path);
        else
            snprintf(full_path, sizeof(full_path), "%s/%s", search_path, exact_path);
        str_t out = str_new(256);
        struct stat st;
        if (stat(full_path, &st) == 0 && S_ISREG(st.st_mode)) {
            str_append_cstr(&out, full_path);
            str_append_cstr(&out, "\n");
        }

        int matches = count_lines(out.data);
        char *hash = store_save(ctx->store, out.len > 0 ? out.data : "(no matches)");
        char *alias = tool_register_alias(ctx, hash ? hash : "");

        cJSON *meta = cJSON_CreateObject();
        cJSON_AddStringToObject(meta, "pattern", pattern);
        if (strcmp(search_path, ".") != 0)
            cJSON_AddStringToObject(meta, "path", search_path);
        cJSON_AddNumberToObject(meta, "matches", matches);
        cJSON_AddStringToObject(meta, "ref", alias);

        tools_inject_thought(ctx, params);
        tool_journal(ctx, "glob_search", params, alias,
                       out.len, matches, NULL, NULL);

        char *ref_copy = strdup(alias);
        free(alias);
        str_free(&out);
        free(hash);
        return tools_make_result(1, meta, ref_copy);
    }

    /* Build find_path: the directory to search in.
     * If root is an absolute path (starts with /), use it directly
     * instead of prepending search_path — avoids creating invalid
     * paths like "./home/user/dir". */
    if (strcmp(root, ".") == 0) {
        snprintf(full_path, sizeof(full_path), "%s", search_path);
    } else if (root[0] == '/') {
        snprintf(full_path, sizeof(full_path), "%s", root);
    } else {
        snprintf(full_path, sizeof(full_path), "%s/%s", search_path, root);
    }

    int glob_timeout = ctx->cfg ? ctx->cfg->grep_timeout : 60;
    str_t out = str_new(4096);
    subprocess_result_t r;

    if (recursive) {
        char *const argv[] = {
            "find", full_path, "-type", "f", "-name", (char *)name,
            "!", "-path", "*/.git/*",
            "!", "-path", "*/node_modules/*",
            "!", "-path", "*/__pycache__/*",
            "!", "-name", "*.o", NULL
        };
        r = subprocess_run(argv, NULL, glob_timeout, 0, 200, 0, &out);
    } else {
        char *const argv[] = {
            "find", full_path, "-maxdepth", "1",
            "-type", "f", "-name", (char *)name,
            "!", "-path", "*/.git/*",
            "!", "-path", "*/node_modules/*",
            "!", "-path", "*/__pycache__/*",
            "!", "-name", "*.o", NULL
        };
        r = subprocess_run(argv, NULL, glob_timeout, 0, 200, 0, &out);
    }

    /* Truncate to 200 lines if we overshot */
    if (r.line_count > 200 && out.data) {
        int seen = 0;
        for (size_t i = 0; i < out.len; i++) {
            if (out.data[i] == '\n') {
                seen++;
                if (seen >= 200) {
                    out.len = i + 1;
                    out.data[out.len] = '\0';  /* str_t guarantees space for NUL at data[len] */
                    break;
                }
            }
        }
    }

    int matches = count_lines(out.data);
    char *hash = store_save(ctx->store, out.len > 0 ? out.data : "(no matches)");
    char *alias = tool_register_alias(ctx, hash ? hash : "");

    cJSON *meta = cJSON_CreateObject();
    cJSON_AddStringToObject(meta, "pattern", pattern);
    if (strcmp(search_path, ".") != 0)
        cJSON_AddStringToObject(meta, "path", search_path);
    cJSON_AddNumberToObject(meta, "matches", matches);
    cJSON_AddStringToObject(meta, "ref", alias);

    tools_inject_thought(ctx, params);
    tool_journal(ctx, "glob_search", params, alias,
                   out.len, matches, NULL, NULL);

    char *ref_copy = strdup(alias);
    free(alias);
    str_free(&out);
    free(hash);
    return tools_make_result(1, meta, ref_copy);
}

/* ── Plugin registration ──────────────────────────────── */

static const tool_param_t grep_search_params[] = {
    TOOL_PARAM("pattern", "string",  "Regex pattern",                  1),
    TOOL_PARAM("path",    "string",  "Directory or file to search in", 0),
    TOOL_PARAM_END
};

static const tool_param_t glob_search_params[] = {
    TOOL_PARAM("pattern", "string",  "Glob pattern (e.g. **/*.py)",    1),
    TOOL_PARAM("path",    "string",  "Directory or file to search in", 0),
    TOOL_PARAM_END
};

static const tool_plugin_t search_plugins[] = {
    TOOL_DEF("grep_search", "Search file contents with a regex pattern.",
             grep_search_params, tool_grep_search),
    TOOL_DEF("glob_search", "Search for files matching a glob pattern.",
             glob_search_params, tool_glob_search),
};
TOOL_PLUGIN_REGISTER_ARRAY(search_plugins, 2)
