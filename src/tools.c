#include "tools.h"
#include "memory.h"
#include "str.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <dirent.h>
#include <curl/curl.h>
#include <regex.h>
#include <errno.h>
#include <signal.h>
#include <poll.h>
#include <fcntl.h>

/* ── helpers ─────────────────────────────────────────── */

static char *read_file_contents(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return NULL; }
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, (size_t)sz, f);
    buf[n] = '\0';
    fclose(f);
    if (out_len) *out_len = n;
    return buf;
}

static int count_lines(const char *s) {
    int n = 0;
    for (; *s; s++) if (*s == '\n') n++;
    return n;
}

static tool_result_t make_result(int success, cJSON *meta, char *ref) {
    return (tool_result_t){ .meta = meta, .store_ref = ref, .success = success };
}

static tool_result_t make_error(const char *msg) {
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "error", msg);
    return make_result(0, m, NULL);
}

/* ── alias management ──────────────────────────────────── */

const char *tool_register_alias(tool_ctx_t *ctx, const char *hash) {
    if (ctx->alias_count >= MAX_ALIASES) return "R?S?";
    alias_entry_t *a = &ctx->aliases[ctx->alias_count];
    snprintf(a->alias, sizeof(a->alias), "R%dS%d", ctx->react_loop, ctx->alias_count);
    snprintf(a->hash, sizeof(a->hash), "%s", hash ? hash : "");
    ctx->alias_count++;

    /* Create symlink in session directory: R1S0 → ../store/hash */
    if (ctx->session_dir && hash && hash[0]) {
        char link_path[4096];
        char target[4096];
        snprintf(link_path, sizeof(link_path), "%s/%s", ctx->session_dir, a->alias);
        snprintf(target, sizeof(target), "../../store/%s", hash);
        symlink(target, link_path);  /* ignore EEXIST */
    }

    return a->alias;
}

const char *tool_resolve_alias(tool_ctx_t *ctx, const char *alias) {
    /* Check if it looks like an alias: R1S1, R1S2, R2S1, ... */
    if (!alias || alias[0] != 'R')
        return NULL;
    for (int i = 0; i < ctx->alias_count; i++) {
        if (strcmp(ctx->aliases[i].alias, alias) == 0) {
            /* Return the full store path */
            return store_resolve(ctx->store, ctx->aliases[i].hash);
        }
    }
    return NULL;
}

/* ── run_command: fork/execve helper ─────────────────── */

/* Run a command with timeout and output cap.
 * timeout_sec: max wall-clock seconds (0 = no limit)
 * max_output:  max bytes to capture (0 = no limit)
 * Returns exit code, or -1 on error, -2 on timeout. */
static int run_command_argv_limited(char *const argv[], str_t *out,
                                    int timeout_sec, int max_output) {
    int pipefd[2];
    if (pipe(pipefd) < 0) return -1;

    pid_t pid = fork();
    if (pid < 0) { close(pipefd[0]); close(pipefd[1]); return -1; }

    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        execvp(argv[0], argv);
        _exit(127);
    }

    close(pipefd[1]);

    /* Set pipe to non-blocking for poll-based reading */
    int flags = fcntl(pipefd[0], F_GETFL, 0);
    fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK);

    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    int timed_out = 0;
    int output_capped = 0;

    while (1) {
        /* Check timeout */
        if (timeout_sec > 0) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            double elapsed = (now.tv_sec - start.tv_sec) +
                             (now.tv_nsec - start.tv_nsec) / 1e9;
            if (elapsed > timeout_sec) {
                timed_out = 1;
                break;
            }
        }

        /* Check output cap */
        if (max_output > 0 && (int)out->len >= max_output) {
            output_capped = 1;
            break;
        }

        /* Poll for data with 100ms timeout */
        struct pollfd pfd = { .fd = pipefd[0], .events = POLLIN };
        int pr = poll(&pfd, 1, 100);
        if (pr > 0 && (pfd.revents & POLLIN)) {
            char buf[4096];
            ssize_t n = read(pipefd[0], buf, sizeof(buf));
            if (n <= 0) break;  /* EOF or error */
            /* Respect output cap */
            if (max_output > 0 && (int)(out->len + (size_t)n) > max_output) {
                size_t remaining = (size_t)max_output - out->len;
                if (remaining > 0) str_append(out, buf, remaining);
                output_capped = 1;
                break;
            }
            str_append(out, buf, (size_t)n);
        } else if (pr > 0 && (pfd.revents & (POLLHUP | POLLERR))) {
            /* Pipe closed (child exited) or error — drain any remaining data */
            char buf[4096];
            ssize_t n;
            while ((n = read(pipefd[0], buf, sizeof(buf))) > 0) {
                if (max_output > 0 && (int)(out->len + (size_t)n) > max_output) {
                    size_t remaining = (size_t)max_output - out->len;
                    if (remaining > 0) str_append(out, buf, remaining);
                    break;
                }
                str_append(out, buf, (size_t)n);
            }
            break;
        } else if (pr == 0) {
            /* Timeout on poll — check if child exited */
            int status;
            pid_t w = waitpid(pid, &status, WNOHANG);
            if (w > 0) {
                /* Child exited — drain remaining output */
                char buf[4096];
                ssize_t n;
                while ((n = read(pipefd[0], buf, sizeof(buf))) > 0) {
                    if (max_output > 0 && (int)(out->len + (size_t)n) > max_output) {
                        size_t remaining = (size_t)max_output - out->len;
                        if (remaining > 0) str_append(out, buf, remaining);
                        break;
                    }
                    str_append(out, buf, (size_t)n);
                }
                close(pipefd[0]);
                return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            }
        } else if (pr < 0 && errno != EINTR) {
            break;  /* poll error */
        }
    }

    close(pipefd[0]);

    /* Kill the child if we broke out early */
    if (timed_out || output_capped) {
        kill(pid, SIGKILL);
        int status;
        waitpid(pid, &status, 0);
        if (timed_out) {
            str_appendf(out, "\n[TIMEOUT: killed after %ds]\n", timeout_sec);
            return -2;
        }
        if (output_capped) {
            str_appendf(out, "\n[OUTPUT CAPPED at %d bytes]\n", max_output);
        }
        return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }

    int status;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* ── shell_exec ──────────────────────────────────────── */

static tool_result_t tool_shell_exec(tool_ctx_t *ctx, cJSON *params) {
    cJSON *cmd_j = cJSON_GetObjectItem(params, "command");
    if (!cmd_j || !cmd_j->valuestring)
        return make_error("missing 'command' parameter");

    const char *command = cmd_j->valuestring;

    str_t out = str_new(4096);
    char *argv[] = { "sh", "-c", (char *)command, NULL };
    int timeout = ctx->cfg ? ctx->cfg->shell_timeout : 300;
    int max_out = ctx->cfg ? ctx->cfg->shell_max_output : 512000;
    int exit_code = run_command_argv_limited(argv, &out, timeout, max_out);

    /* Store to shared store */
    char *hash = store_save(ctx->store, out.data);

    /* Register alias */
    const char *alias = tool_register_alias(ctx, hash ? hash : "");

    cJSON *meta = cJSON_CreateObject();
    cJSON_AddNumberToObject(meta, "exit_code", exit_code);
    cJSON_AddNumberToObject(meta, "chars", (double)out.len);
    cJSON_AddNumberToObject(meta, "lines", count_lines(out.data));
    cJSON_AddStringToObject(meta, "ref", alias);

    journal_append(ctx->journal, ctx->react_loop, ctx->step, "shell_exec", params, alias,
                   out.len, count_lines(out.data), exit_code == 0 ? NULL : "non-zero exit", NULL);

    char *ref_copy = strdup(alias);
    str_free(&out);
    free(hash);
    return make_result(exit_code == 0, meta, ref_copy);
}

/* ── file_read ───────────────────────────────────────── */

static tool_result_t tool_file_read(tool_ctx_t *ctx, cJSON *params) {
    cJSON *path_j = cJSON_GetObjectItem(params, "path");
    if (!path_j || !path_j->valuestring)
        return make_error("missing 'path' parameter");

    const char *path = path_j->valuestring;

    /* Resolve step aliases (S0, S1, S2...) */
    const char *resolved = tool_resolve_alias(ctx, path);
    if (resolved) path = resolved;

    /* Resolve store/ paths relative to session directory (legacy) */
    char resolved_buf[4096];
    if (strncmp(path, "store/", 6) == 0) {
        snprintf(resolved_buf, sizeof(resolved_buf), "%s/%s", ctx->session_dir, path);
        path = resolved_buf;
    }

    /* Safety: check file type and size before reading */
    struct stat st;
    if (stat(path, &st) != 0) {
        char msg[4224];
        snprintf(msg, sizeof(msg), "cannot stat '%.4095s': %s", path, strerror(errno));
        return make_error(msg);
    }
    if (!S_ISREG(st.st_mode)) {
        return make_error("not a regular file (refusing to read pipes, devices, etc.)");
    }
    int max_file = ctx->cfg ? ctx->cfg->file_max_size : 52428800;  /* 50MB default */
    if (st.st_size > max_file) {
        char msg[256];
        snprintf(msg, sizeof(msg), "file too large (%ld bytes, limit %d)", (long)st.st_size, max_file);
        return make_error(msg);
    }

    size_t len = 0;
    char *content = read_file_contents(path, &len);
    if (!content) {
        char msg[4224];
        snprintf(msg, sizeof(msg), "cannot read '%.4095s': %s", path, strerror(errno));
        return make_error(msg);
    }

    int lines = count_lines(content);
    char *hash = store_save(ctx->store, content);
    const char *alias = tool_register_alias(ctx, hash ? hash : "");

    cJSON *meta = cJSON_CreateObject();
    cJSON_AddStringToObject(meta, "path", path_j->valuestring);
    cJSON_AddNumberToObject(meta, "lines", lines);
    cJSON_AddNumberToObject(meta, "chars", (double)len);
    cJSON_AddStringToObject(meta, "ref", alias);

    /* file_read MUST return content — that's its purpose */
    if (len <= 50000) {
        cJSON_AddStringToObject(meta, "content", content);
    } else {
        char *trunc = malloc(50001);
        if (trunc) {
            memcpy(trunc, content, 50000);
            trunc[50000] = '\0';
            cJSON_AddStringToObject(meta, "content", trunc);
            cJSON_AddBoolToObject(meta, "truncated", 1);
            free(trunc);
        }
    }

    journal_append(ctx->journal, ctx->react_loop, ctx->step, "file_read", params, alias,
                   len, lines, NULL, NULL);

    char *ref_copy = strdup(alias);
    free(content);
    free(hash);
    return make_result(1, meta, ref_copy);
}

/* ── file_write ──────────────────────────────────────── */

static tool_result_t tool_file_write(tool_ctx_t *ctx, cJSON *params) {
    cJSON *path_j = cJSON_GetObjectItem(params, "path");
    cJSON *content_j = cJSON_GetObjectItem(params, "content");
    if (!path_j || !path_j->valuestring || !content_j || !content_j->valuestring)
        return make_error("missing 'path' or 'content' parameter");

    const char *path = path_j->valuestring;
    const char *content = content_j->valuestring;

    FILE *f = fopen(path, "w");
    if (!f) {
        char msg[512];
        snprintf(msg, sizeof(msg), "cannot write '%s': %s", path, strerror(errno));
        return make_error(msg);
    }

    size_t len = strlen(content);
    fwrite(content, 1, len, f);
    fclose(f);

    /* Store written content for full audit trail */
    char *hash = store_save(ctx->store, content);
    const char *alias = tool_register_alias(ctx, hash ? hash : "");

    cJSON *meta = cJSON_CreateObject();
    cJSON_AddStringToObject(meta, "status", "ok");
    cJSON_AddStringToObject(meta, "path", path);
    cJSON_AddNumberToObject(meta, "bytes", (double)len);
    cJSON_AddStringToObject(meta, "ref", alias);

    journal_append(ctx->journal, ctx->react_loop, ctx->step, "file_write", params, alias,
                   len, count_lines(content), NULL, NULL);

    char *ref_copy = strdup(alias);
    free(hash);
    return make_result(1, meta, ref_copy);
}

/* ── file_edit ───────────────────────────────────────── */

static tool_result_t tool_file_edit(tool_ctx_t *ctx, cJSON *params) {
    cJSON *path_j     = cJSON_GetObjectItem(params, "path");
    cJSON *old_text_j = cJSON_GetObjectItem(params, "old_text");
    cJSON *new_text_j = cJSON_GetObjectItem(params, "new_text");
    if (!path_j || !path_j->valuestring ||
        !old_text_j || !old_text_j->valuestring ||
        !new_text_j || !new_text_j->valuestring)
        return make_error("missing 'path', 'old_text', or 'new_text' parameter");

    const char *path = path_j->valuestring;
    const char *old_text = old_text_j->valuestring;
    const char *new_text = new_text_j->valuestring;

    size_t flen = 0;
    char *content = read_file_contents(path, &flen);
    if (!content) {
        char msg[512];
        snprintf(msg, sizeof(msg), "cannot read '%s': %s", path, strerror(errno));
        return make_error(msg);
    }

    /* Store pre-edit content */
    char *pre_hash = store_save(ctx->store, content);
    const char *pre_alias = tool_register_alias(ctx, pre_hash ? pre_hash : "");

    char *pos = strstr(content, old_text);
    if (!pos) {
        free(content);
        free(pre_hash);
        return make_error("old_text not found in file");
    }

    /* Build new content */
    size_t old_len = strlen(old_text);
    size_t new_len = strlen(new_text);
    size_t result_len = flen - old_len + new_len;
    char *result = malloc(result_len + 1);
    if (!result) { free(content); free(pre_hash); return make_error("malloc failed"); }

    size_t prefix_len = (size_t)(pos - content);
    memcpy(result, content, prefix_len);
    memcpy(result + prefix_len, new_text, new_len);
    memcpy(result + prefix_len + new_len, pos + old_len, flen - prefix_len - old_len);
    result[result_len] = '\0';

    FILE *f = fopen(path, "w");
    if (!f) { free(content); free(result); free(pre_hash); return make_error("cannot write file"); }
    fwrite(result, 1, result_len, f);
    fclose(f);

    cJSON *meta = cJSON_CreateObject();
    cJSON_AddStringToObject(meta, "status", "ok");
    cJSON_AddStringToObject(meta, "path", path);
    cJSON_AddStringToObject(meta, "pre_ref", pre_alias);

    journal_append(ctx->journal, ctx->react_loop, ctx->step, "file_edit", params, pre_alias,
                   result_len, count_lines(result), NULL, NULL);

    free(content);
    free(result);
    free(pre_hash);
    return make_result(1, meta, NULL);
}

/* ── grep_search ─────────────────────────────────────── */

static tool_result_t tool_grep_search(tool_ctx_t *ctx, cJSON *params) {
    cJSON *pattern_j = cJSON_GetObjectItem(params, "pattern");
    cJSON *path_j    = cJSON_GetObjectItem(params, "path");
    if (!pattern_j || !pattern_j->valuestring)
        return make_error("missing 'pattern' parameter");

    const char *pattern = pattern_j->valuestring;
    const char *path = path_j && path_j->valuestring ? path_j->valuestring : ".";

    /* Resolve step aliases */
    const char *resolved = tool_resolve_alias(ctx, path);
    if (resolved) path = resolved;

    /* Resolve store/ paths (legacy) */
    char resolved_path[4096];
    if (strncmp(path, "store/", 6) == 0) {
        snprintf(resolved_path, sizeof(resolved_path), "%s/%s", ctx->session_dir, path);
        path = resolved_path;
    }

    /* use fork/execvp to avoid shell injection */
    int pipefd[2];
    if (pipe(pipefd) < 0) return make_error("pipe failed");

    pid_t pid = fork();
    if (pid < 0) { close(pipefd[0]); close(pipefd[1]); return make_error("fork failed"); }

    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        execlp("grep", "grep", "-rn",
               "--include=*.c", "--include=*.h", "--include=*.py",
               "--include=*.js", "--include=*.json", "--include=*.yaml",
               "--include=*.yml", "--include=*.md", "--include=*.txt",
               "--include=*.sh", "--include=*.go", "--include=*.rs",
               "--include=*.toml", "--include=*.xml", "--include=*.html",
               "--include=*.java", "--include=*.rb", "--include=*.php",
               "--include=*.cfg", "--include=*.ini", "--include=*.conf",
               "-m", "50",
               pattern, path, (char *)NULL);
        _exit(127);
    }

    close(pipefd[1]);
    str_t out = str_new(4096);
    char buf[4096];
    ssize_t n;

    /* Read with timeout + output cap */
    int grep_timeout = ctx->cfg ? ctx->cfg->grep_timeout : 60;
    int grep_max = ctx->cfg ? ctx->cfg->shell_max_output : 512000;
    time_t start = time(NULL);
    int timed_out = 0;

    /* Set pipe to non-blocking for timeout support */
    int flags = fcntl(pipefd[0], F_GETFL, 0);
    fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK);

    while (1) {
        n = read(pipefd[0], buf, sizeof(buf));
        if (n > 0) {
            str_append(&out, buf, (size_t)n);
            if ((int)out.len >= grep_max) {
                str_append_cstr(&out, "\n... [output truncated at limit]\n");
                break;
            }
        } else if (n == 0) {
            break;  /* EOF */
        } else {
            /* EAGAIN — no data yet */
            if (time(NULL) - start >= grep_timeout) {
                timed_out = 1;
                break;
            }
            { struct timespec ts = {0, 10000000}; nanosleep(&ts, NULL); }  /* 10ms poll */
        }
    }
    close(pipefd[0]);

    if (timed_out) kill(pid, SIGKILL);
    int status;
    waitpid(pid, &status, 0);

    int matches = count_lines(out.data);
    char *hash = store_save(ctx->store, out.data);
    const char *alias = tool_register_alias(ctx, hash ? hash : "");

    cJSON *meta = cJSON_CreateObject();
    cJSON_AddStringToObject(meta, "pattern", pattern);
    cJSON_AddStringToObject(meta, "path", path_j && path_j->valuestring ? path_j->valuestring : ".");
    cJSON_AddNumberToObject(meta, "matches", matches);
    cJSON_AddNumberToObject(meta, "chars", (double)out.len);
    cJSON_AddStringToObject(meta, "ref", alias);

    journal_append(ctx->journal, ctx->react_loop, ctx->step, "grep_search", params, alias,
                   out.len, matches, NULL, NULL);

    char *ref_copy = strdup(alias);
    str_free(&out);
    free(hash);
    return make_result(1, meta, ref_copy);
}

/* ── notes (scratchpad) ──────────────────────────────── */

static tool_result_t tool_notes(tool_ctx_t *ctx, cJSON *params) {
    cJSON *content_j = cJSON_GetObjectItem(params, "content");
    if (!content_j || !content_j->valuestring)
        return make_error("missing 'content' parameter");

    /* Update scratchpad */
    if (ctx->scratchpad) free(ctx->scratchpad);
    ctx->scratchpad = strdup(content_j->valuestring);

    /* Persist to disk */
    char scratch_path[512];
    snprintf(scratch_path, sizeof(scratch_path), "%s/scratchpad.md", ctx->session_dir);
    FILE *f = fopen(scratch_path, "w");
    if (f) {
        fputs(ctx->scratchpad, f);
        fclose(f);
    }

    /* Store for audit */
    char *hash = store_save(ctx->store, ctx->scratchpad);
    const char *alias = tool_register_alias(ctx, hash ? hash : "");

    cJSON *meta = cJSON_CreateObject();
    cJSON_AddStringToObject(meta, "status", "ok");
    cJSON_AddStringToObject(meta, "ref", alias);

    journal_append(ctx->journal, ctx->react_loop, ctx->step, "notes", params, alias,
                   strlen(ctx->scratchpad), 0, NULL, NULL);

    char *ref_copy = strdup(alias);
    free(hash);
    return make_result(1, meta, ref_copy);
}

/* ── done ────────────────────────────────────────────── */

static tool_result_t tool_done(tool_ctx_t *ctx, cJSON *params) {
    cJSON *result_j = cJSON_GetObjectItem(params, "result");
    const char *result = result_j && result_j->valuestring
                         ? result_j->valuestring : "(no result)";

    /* Store result for full audit */
    char *hash = store_save(ctx->store, result);
    const char *alias = tool_register_alias(ctx, hash ? hash : "");

    cJSON *meta = cJSON_CreateObject();
    cJSON_AddStringToObject(meta, "result", result);
    cJSON_AddStringToObject(meta, "ref", alias);

    journal_append(ctx->journal, ctx->react_loop, ctx->step, "done", params, alias,
                   strlen(result), 0, NULL, NULL);

    char *ref_copy = strdup(alias);
    free(hash);
    return make_result(1, meta, ref_copy);
}

/* ── dispatcher ──────────────────────────────────────── */

/* ── memory_store ──────────────────────────────────────── */

static tool_result_t tool_memory_store(tool_ctx_t *ctx, cJSON *params) {
    cJSON *key_j = cJSON_GetObjectItem(params, "key");
    cJSON *val_j = cJSON_GetObjectItem(params, "value");
    if (!key_j || !key_j->valuestring || !val_j || !val_j->valuestring)
        return make_error("missing 'key' or 'value' parameter");

    const char *key = key_j->valuestring;
    const char *value = val_j->valuestring;

    /* Parse tags (comma-separated string) */
    const char *tags_arr[32];
    int n_tags = 0;
    cJSON *tags_j = cJSON_GetObjectItem(params, "tags");
    char *tags_copy = NULL;
    if (tags_j && tags_j->valuestring) {
        tags_copy = strdup(tags_j->valuestring);
        char *tok = strtok(tags_copy, ",");
        while (tok && n_tags < 32) {
            while (*tok == ' ') tok++;  /* trim leading space */
            tags_arr[n_tags++] = tok;
            tok = strtok(NULL, ",");
        }
    }

    int pinned = 0;
    cJSON *pin_j = cJSON_GetObjectItem(params, "pinned");
    if (pin_j && cJSON_IsTrue(pin_j)) pinned = 1;

    int rc = memory_store(ctx->memory, key, value, tags_arr, n_tags, pinned);
    free(tags_copy);

    if (rc != 0) return make_error("failed to store memory");

    /* Store for audit */
    char *hash = store_save(ctx->store, value);
    const char *alias = tool_register_alias(ctx, hash ? hash : "");

    cJSON *meta = cJSON_CreateObject();
    cJSON_AddStringToObject(meta, "status", "ok");
    cJSON_AddStringToObject(meta, "key", key);
    if (alias) cJSON_AddStringToObject(meta, "ref", alias);

    journal_append(ctx->journal, ctx->react_loop, ctx->step, "memory_store",
                   params, alias, strlen(value), 0, NULL, NULL);

    free(hash);
    return make_result(1, meta, alias ? strdup(alias) : NULL);
}

/* ── memory_recall ─────────────────────────────────────── */

static tool_result_t tool_memory_recall(tool_ctx_t *ctx, cJSON *params) {
    cJSON *query_j = cJSON_GetObjectItem(params, "query");
    if (!query_j || !query_j->valuestring)
        return make_error("missing 'query' parameter");

    const char *query = query_j->valuestring;
    memory_results_t results = memory_recall(ctx->memory, query, 5);

    /* Build result string */
    str_t out = str_new(1024);
    for (int i = 0; i < results.count; i++) {
        memory_entry_t *e = &results.entries[i];
        str_appendf(&out, "--- %s ---\n%s\n\n", e->key, e->value);
    }

    char *hash = NULL;
    const char *alias = NULL;
    if (out.len > 0) {
        hash = store_save(ctx->store, out.data);
        alias = tool_register_alias(ctx, hash ? hash : "");
    }

    cJSON *meta = cJSON_CreateObject();
    cJSON_AddNumberToObject(meta, "matches", results.count);
    /* Content stored to .store/ — model reads via file_read(ref) */
    if (alias) cJSON_AddStringToObject(meta, "ref", alias);

    journal_append(ctx->journal, ctx->react_loop, ctx->step, "memory_recall",
                   params, alias, out.len, results.count, NULL, NULL);

    memory_results_free(&results);
    free(hash);
    char *ref_copy = alias ? strdup(alias) : NULL;
    str_free(&out);
    return make_result(1, meta, ref_copy);
}



/* ── memory_pin ─────────────────────────────────────────── */

static tool_result_t tool_memory_pin(tool_ctx_t *ctx, cJSON *params) {
    cJSON *key_j = cJSON_GetObjectItem(params, "key");
    if (!key_j || !key_j->valuestring)
        return make_error("missing 'key' parameter");

    int rc = memory_pin(ctx->memory, key_j->valuestring);
    if (rc != 0) return make_error("memory entry not found");

    cJSON *meta = cJSON_CreateObject();
    cJSON_AddStringToObject(meta, "status", "pinned");
    cJSON_AddStringToObject(meta, "key", key_j->valuestring);

    journal_append(ctx->journal, ctx->react_loop, ctx->step, "memory_pin",
                   params, NULL, 0, 0, NULL, NULL);

    return make_result(1, meta, NULL);
}

/* ── memory_unpin ───────────────────────────────────────── */

static tool_result_t tool_memory_unpin(tool_ctx_t *ctx, cJSON *params) {
    cJSON *key_j = cJSON_GetObjectItem(params, "key");
    if (!key_j || !key_j->valuestring)
        return make_error("missing 'key' parameter");

    int rc = memory_unpin(ctx->memory, key_j->valuestring);
    if (rc != 0) return make_error("memory entry not found");

    cJSON *meta = cJSON_CreateObject();
    cJSON_AddStringToObject(meta, "status", "unpinned");
    cJSON_AddStringToObject(meta, "key", key_j->valuestring);

    journal_append(ctx->journal, ctx->react_loop, ctx->step, "memory_unpin",
                   params, NULL, 0, 0, NULL, NULL);

    return make_result(1, meta, NULL);
}

/* ── web_fetch ──────────────────────────────────────────── */

static size_t web_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    str_t *buf = userdata;
    size_t total = size * nmemb;
    /* Cap at 500KB to prevent memory explosion */
    if (buf->len + total > 512000) {
        size_t remaining = 512000 - buf->len;
        if (remaining > 0) str_append(buf, ptr, remaining);
        return total;  /* tell curl we consumed it all */
    }
    str_append(buf, ptr, total);
    return total;
}

static tool_result_t tool_web_fetch(tool_ctx_t *ctx, cJSON *params) {
    cJSON *url_j = cJSON_GetObjectItem(params, "url");
    if (!url_j || !url_j->valuestring)
        return make_error("missing 'url' parameter");

    const char *url = url_j->valuestring;

    CURL *curl = curl_easy_init();
    if (!curl) return make_error("curl_easy_init failed");

    str_t body = str_new(8192);

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, web_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "nash/1.0");

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    char *content_type = NULL;
    char *ct = NULL;
    curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &ct);
    if (ct) content_type = strdup(ct);

    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        char msg[512];
        snprintf(msg, sizeof(msg), "fetch failed: %s", curl_easy_strerror(res));
        str_free(&body);
        free(content_type);
        return make_error(msg);
    }

    /* Store to shared store */
    char *hash = store_save(ctx->store, body.data);
    const char *alias = tool_register_alias(ctx, hash ? hash : "");

    /* Build metadata */
    cJSON *meta = cJSON_CreateObject();
    cJSON_AddStringToObject(meta, "url", url);
    cJSON_AddNumberToObject(meta, "http_code", http_code);
    cJSON_AddNumberToObject(meta, "chars", (double)body.len);
    cJSON_AddNumberToObject(meta, "lines", count_lines(body.data));
    if (content_type) cJSON_AddStringToObject(meta, "content_type", content_type);
    if (alias) cJSON_AddStringToObject(meta, "ref", alias);

    /* Content stored to .store/ — model reads via file_read(ref) */

    journal_append(ctx->journal, ctx->react_loop, ctx->step, "web_fetch",
                   params, alias, body.len, count_lines(body.data), NULL, NULL);

    free(hash);
    free(content_type);
    char *ref_copy = alias ? strdup(alias) : NULL;
    str_free(&body);
    return make_result(http_code >= 200 && http_code < 400, meta, ref_copy);
}

/* ── web_search ────────────────────────────────────────── */

static tool_result_t tool_web_search(tool_ctx_t *ctx, cJSON *params) {
    cJSON *query_j = cJSON_GetObjectItem(params, "query");
    if (!query_j || !query_j->valuestring)
        return make_error("missing 'query' parameter");

    const char *query = query_j->valuestring;

    /* Use DuckDuckGo lite HTML search */
    /* URL-encode the query */
    CURL *curl = curl_easy_init();
    if (!curl) return make_error("curl_easy_init failed");

    char *encoded_q = curl_easy_escape(curl, query, 0);
    char url[2048];
    snprintf(url, sizeof(url), "https://lite.duckduckgo.com/lite/?q=%s", encoded_q);
    curl_free(encoded_q);

    str_t body = str_new(32768);

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, web_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "nash/1.0");

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        char msg[512];
        snprintf(msg, sizeof(msg), "search failed: %s", curl_easy_strerror(res));
        str_free(&body);
        return make_error(msg);
    }

    /* Parse DuckDuckGo lite HTML results — extract links and snippets */
    str_t results = str_new(4096);
    int result_count = 0;

    /* Simple HTML parsing: find result links in <a class="result-link"> or <a> tags with http */
    const char *p = body.data;
    while (p && result_count < 10) {
        /* Look for result links — DDG lite uses <a rel="nofollow" href="..."> */
        const char *href = strstr(p, "href=\"http");
        if (!href) break;
        href += 6;  /* skip href=" */
        const char *end = strchr(href, '"');
        if (!end || end - href > 500) { p = href; continue; }

        /* Extract URL */
        char link[512];
        size_t link_len = (size_t)(end - href);
        if (link_len >= sizeof(link)) link_len = sizeof(link) - 1;
        memcpy(link, href, link_len);
        link[link_len] = '\0';

        /* Skip DDG internal links */
        if (strstr(link, "duckduckgo.com") || strstr(link, "duck.co")) {
            p = end;
            continue;
        }

        /* Try to find a title — look for text between > and < after the <a> tag */
        const char *tag_end = strchr(end, '>');
        char title[256] = "";
        if (tag_end) {
            tag_end++;
            const char *title_end = strchr(tag_end, '<');
            if (title_end && title_end - tag_end > 0 && title_end - tag_end < 250) {
                size_t tlen = (size_t)(title_end - tag_end);
                memcpy(title, tag_end, tlen);
                title[tlen] = '\0';
            }
        }

        if (title[0]) {
            str_appendf(&results, "%d. %s\n   %s\n\n", result_count + 1, title, link);
        } else {
            str_appendf(&results, "%d. %s\n\n", result_count + 1, link);
        }
        result_count++;
        p = end;
    }

    if (result_count == 0) {
        /* No results = explicit failure — don't store useless content,
         * record as error in journal so reflection can see it */
        char errmsg[512];
        snprintf(errmsg, sizeof(errmsg),
                 "no results found for query: %s", query);

        journal_append(ctx->journal, ctx->react_loop, ctx->step, "web_search",
                       params, NULL, 0, 0, errmsg, NULL);

        str_free(&results);
        str_free(&body);
        return make_error(errmsg);
    }

    /* Store results */
    char *hash = store_save(ctx->store, results.data);
    const char *alias = tool_register_alias(ctx, hash ? hash : "");

    cJSON *meta = cJSON_CreateObject();
    cJSON_AddStringToObject(meta, "query", query);
    cJSON_AddNumberToObject(meta, "results", result_count);
    cJSON_AddNumberToObject(meta, "chars", (double)results.len);
    if (alias) cJSON_AddStringToObject(meta, "ref", alias);

    /* Content stored to .store/ — model reads via file_read(ref) */

    journal_append(ctx->journal, ctx->react_loop, ctx->step, "web_search",
                   params, alias, results.len, result_count, NULL, NULL);

    free(hash);
    char *ref_copy = alias ? strdup(alias) : NULL;
    str_free(&results);
    str_free(&body);
    return make_result(1, meta, ref_copy);
}


tool_result_t tool_execute(tool_ctx_t *ctx, const char *action, cJSON *params) {
    if (strcmp(action, "shell_exec")  == 0) return tool_shell_exec(ctx, params);
    if (strcmp(action, "file_read")   == 0) return tool_file_read(ctx, params);
    if (strcmp(action, "file_write")  == 0) return tool_file_write(ctx, params);
    if (strcmp(action, "file_edit")   == 0) return tool_file_edit(ctx, params);
    if (strcmp(action, "grep_search") == 0) return tool_grep_search(ctx, params);
    if (strcmp(action, "web_fetch")   == 0) return tool_web_fetch(ctx, params);
    if (strcmp(action, "web_search")  == 0) return tool_web_search(ctx, params);
    if (strcmp(action, "notes")       == 0) return tool_notes(ctx, params);
    if (strcmp(action, "done")        == 0) return tool_done(ctx, params);
    if (strcmp(action, "memory_store") == 0) return tool_memory_store(ctx, params);
    if (strcmp(action, "memory_recall")== 0) return tool_memory_recall(ctx, params);
    if (strcmp(action, "memory_pin")  == 0) return tool_memory_pin(ctx, params);
    if (strcmp(action, "memory_unpin")== 0) return tool_memory_unpin(ctx, params);

    char msg[256];
    snprintf(msg, sizeof(msg), "unknown tool: %s", action);
    return make_error(msg);
}

void tool_result_free(tool_result_t *r) {
    if (r->meta) cJSON_Delete(r->meta);
    free(r->store_ref);
    r->meta = NULL;
    r->store_ref = NULL;
}

/* ── system prompt ───────────────────────────────────── */

const char *tools_system_prompt(void) {
    return
    "You are an autonomous coding agent. Solve the user's task step by step "
    "using the available tools.\n"
    "\n"
    "Store-and-reference pattern:\n"
    "- Most tool outputs are stored to disk. You see only metadata with a ref "
    "alias (R0S1, R0S2, etc.).\n"
    "- To read the actual content, call file_read(path=\"R0S1\").\n"
    "- You MUST file_read the ref if you need to see what a command produced "
    "or what a file contains.\n"
    "\n"
    "Rules:\n"
    "- Never invoke tools speculatively. Every tool call must have a clear reason "
    "and you MUST read the result (file_read the ref) before proceeding.\n"
    "- Never guess tool results. Wait for actual output.\n"
    "- file_edit: old_text must match exactly. Always file_read first.\n"
    "- Record key findings in notes — they survive context eviction.\n"
    "- Call done with the final answer when finished.\n";
}
