#include "tools_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <signal.h>
#include <poll.h>
#include <fcntl.h>
#include <errno.h>

/* ── grep_search ─────────────────────────────────────── */

tool_result_t tool_grep_search(tool_ctx_t *ctx, cJSON *params) {
    cJSON *pattern_j = cJSON_GetObjectItem(params, "pattern");
    cJSON *path_j    = cJSON_GetObjectItem(params, "path");
    if (!pattern_j || !pattern_j->valuestring || !pattern_j->valuestring[0])
        return tools_make_error("grep_search requires a non-empty 'pattern' string. "
                          "Provide a regex pattern to search for.");

    const char *pattern = pattern_j->valuestring;
    const char *path = path_j && path_j->valuestring && path_j->valuestring[0]
                       ? path_j->valuestring : ".";

    /* Resolve aliases and store/ paths */
    char *resolved = NULL;
    char resolved_path[NASH_PATH_MAX];
    path = tools_resolve_path(ctx, path, resolved_path, &resolved);

    /* use fork/execvp to avoid shell injection */
    int pipefd[2];
    if (pipe(pipefd) < 0) return tools_make_error("pipe failed");

    int max_matches = ctx->cfg ? ctx->cfg->grep_max_matches : 50;
    if (max_matches <= 0) max_matches = 50;
    char max_matches_str[16];
    snprintf(max_matches_str, sizeof(max_matches_str), "%d", max_matches);

    pid_t pid = fork();
    if (pid < 0) { close(pipefd[0]); close(pipefd[1]); return tools_make_error("fork failed"); }

    if (pid == 0) {
        setsid();
        int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) { dup2(devnull, STDIN_FILENO); close(devnull); }
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        execlp("grep", "grep", "-rn",
               "--binary-files=without-match",
               "--exclude-dir=.git", "--exclude-dir=node_modules",
               "--exclude-dir=__pycache__", "--exclude-dir=.tox",
               "--exclude-dir=vendor", "--exclude-dir=target",
               "--exclude-dir=build", "--exclude-dir=dist",
               "--exclude=*.o", "--exclude=*.a", "--exclude=*.so",
               "--exclude=*.dylib", "--exclude=*.pyc",
               "-m", max_matches_str,
               pattern, path, (char *)NULL);
        _exit(127);
    }

    close(pipefd[1]);
    str_t out = str_new(4096);
    char buf[NASH_PATH_MAX];
    ssize_t n;

    /* Read with timeout + output cap.
     * FIX B4: grep_max_matches controls line count (grep -m), while this
     * byte cap prevents unbounded output from long-line matches.
     * Uses shell_max_output as default; a dedicated grep_max_output config
     * could be added if finer control is needed. */
    int grep_timeout = ctx->cfg ? ctx->cfg->grep_timeout : 60;
    int grep_max = ctx->cfg ? ctx->cfg->shell_max_output : 512000;
    /* FIX #9: Use clock_gettime(CLOCK_MONOTONIC) + poll() instead of
     * time() + nanosleep(). Provides sub-second timeout accuracy and
     * avoids CPU-wasteful 10ms busy-loop polling. */
    struct timespec ts_start;
    clock_gettime(CLOCK_MONOTONIC, &ts_start);
    int timed_out = 0;
    struct pollfd pfd = { .fd = pipefd[0], .events = POLLIN };

    while (1) {
        /* Calculate remaining timeout in ms */
        struct timespec ts_now;
        clock_gettime(CLOCK_MONOTONIC, &ts_now);
        long elapsed_ms = (ts_now.tv_sec - ts_start.tv_sec) * 1000
                        + (ts_now.tv_nsec - ts_start.tv_nsec) / 1000000;
        long remaining_ms = (long)grep_timeout * 1000 - elapsed_ms;
        if (remaining_ms <= 0) { timed_out = 1; break; }
        int poll_ms = remaining_ms > 100 ? 100 : (int)remaining_ms;

        int pr = poll(&pfd, 1, poll_ms);
        if (pr > 0) {
            n = read(pipefd[0], buf, sizeof(buf));
            if (n > 0) {
                str_append(&out, buf, (size_t)n);
                if ((int)out.len >= grep_max) {
                    str_append_cstr(&out, "\n... [output truncated at limit]\n");
                    break;
                }
            } else if (n == 0) {
                break;  /* EOF */
            }
        } else if (pr == 0) {
            continue;  /* poll timeout — check elapsed */
        } else {
            break;  /* poll error */
        }
    }
    close(pipefd[0]);

    if (timed_out) kill(pid, SIGKILL);
    int status;
    waitpid(pid, &status, 0);

    int matches = count_lines(out.data);
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
    cJSON *pattern_j = cJSON_GetObjectItem(params, "pattern");
    if (!pattern_j || !pattern_j->valuestring || !pattern_j->valuestring[0])
        return tools_make_error("glob_search requires a non-empty 'pattern' string. "
                          "Use wildcards like **/*.c or src/**/*.h");

    const char *pattern = pattern_j->valuestring;
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

    /* Use fork/execvp for find — avoids shell injection via pattern/path */
    int pipefd[2];
    if (pipe(pipefd) < 0) return tools_make_error("pipe failed");

    pid_t pid = fork();
    if (pid < 0) { close(pipefd[0]); close(pipefd[1]); return tools_make_error("fork failed"); }

    if (pid == 0) {
        setsid();
        { int dn = open("/dev/null", O_RDONLY);
          if (dn >= 0) { dup2(dn, STDIN_FILENO); close(dn); } }
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);
        /* Redirect stderr to /dev/null (like 2>/dev/null in original) */
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) { dup2(devnull, STDERR_FILENO); close(devnull); }

        if (recursive) {
            execlp("find", "find", full_path,
                   "-type", "f", "-name", name,
                   "!", "-path", "*/.git/*",
                   "!", "-path", "*/node_modules/*",
                   "!", "-path", "*/__pycache__/*",
                   "!", "-name", "*.o",
                   (char *)NULL);
        } else {
            execlp("find", "find", full_path,
                   "-maxdepth", "1",
                   "-type", "f", "-name", name,
                   "!", "-path", "*/.git/*",
                   "!", "-path", "*/node_modules/*",
                   "!", "-path", "*/__pycache__/*",
                   "!", "-name", "*.o",
                   (char *)NULL);
        }
        _exit(127);
    }

    close(pipefd[1]);
    str_t out = str_new(4096);
    char buf[NASH_PATH_MAX];
    ssize_t n;
    int line_count = 0;

    while ((n = read(pipefd[0], buf, sizeof(buf))) > 0) {
        str_append(&out, buf, (size_t)n);
        /* Count lines for limit (head -200 equivalent) */
        for (ssize_t i = 0; i < n; i++)
            if (buf[i] == '\n') line_count++;
        if (line_count >= 200) break;
    }
    close(pipefd[0]);

    /* Kill find if we hit the line limit */
    if (line_count >= 200) kill(pid, SIGKILL);
    int status;
    waitpid(pid, &status, 0);

    /* Truncate to 200 lines if we overshot */
    if (line_count > 200 && out.data) {
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
