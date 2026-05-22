#include "tools.h"
#include "str.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <dirent.h>
#include <regex.h>
#include <errno.h>

/* ── helpers ─────────────────────────────────────────────── */

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

/* ── run_command: fork/execve helper ─────────────────────── */

static int run_command_argv(char *const argv[], str_t *out) {
    int pipefd[2];
    if (pipe(pipefd) < 0) return -1;

    pid_t pid = fork();
    if (pid < 0) { close(pipefd[0]); close(pipefd[1]); return -1; }

    if (pid == 0) {
        /* child: redirect stdout+stderr to pipe */
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        execvp(argv[0], argv);
        _exit(127);
    }

    /* parent: read from pipe */
    close(pipefd[1]);
    char buf[4096];
    ssize_t n;
    while ((n = read(pipefd[0], buf, sizeof(buf))) > 0)
        str_append(out, buf, (size_t)n);
    close(pipefd[0]);

    int status;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* ── shell_exec ──────────────────────────────────────────── */

static tool_result_t tool_shell_exec(tool_ctx_t *ctx, cJSON *params) {
    cJSON *cmd_j = cJSON_GetObjectItem(params, "command");
    if (!cmd_j || !cmd_j->valuestring)
        return make_error("missing 'command' parameter");

    const char *command = cmd_j->valuestring;

    /* run via fork/execve — no shell string interpolation */
    str_t out = str_new(4096);
    char *argv[] = { "/bin/sh", "-c", (char *)command, NULL };
    int exit_code = run_command_argv(argv, &out);

    /* store full output */
    char *ref = store_save(ctx->store, out.data, "txt");

    /* build metadata */
    cJSON *meta = cJSON_CreateObject();
    cJSON_AddNumberToObject(meta, "exit_code", exit_code);
    cJSON_AddNumberToObject(meta, "chars", (double)out.len);
    cJSON_AddNumberToObject(meta, "lines", count_lines(out.data));
    if (ref) cJSON_AddStringToObject(meta, "ref", ref);

    /* journal */
    { char *_pj = cJSON_PrintUnformatted(params); journal_append(ctx->journal, ctx->step, "shell_exec", _pj, ref,
                   out.len, count_lines(out.data), exit_code == 0 ? NULL : "non-zero exit"); free(_pj); }

    str_free(&out);
    return make_result(exit_code == 0, meta, ref);
}

/* ── file_read ───────────────────────────────────────────── */

static tool_result_t tool_file_read(tool_ctx_t *ctx, cJSON *params) {
    cJSON *path_j = cJSON_GetObjectItem(params, "path");
    if (!path_j || !path_j->valuestring)
        return make_error("missing 'path' parameter");

    const char *path = path_j->valuestring;
    char resolved[4096];

    /* Resolve store/ paths relative to session directory */
    if (strncmp(path, "store/", 6) == 0) {
        snprintf(resolved, sizeof(resolved), "%s/%s", ctx->session_dir, path);
        path = resolved;
    }

    size_t len = 0;
    char *content = read_file_contents(path, &len);
    if (!content) {
        char msg[512];
        snprintf(msg, sizeof(msg), "cannot read '%s': %s", path, strerror(errno));
        return make_error(msg);
    }

    int lines = count_lines(content);
    char *ref = store_save(ctx->store, content, "txt");

    cJSON *meta = cJSON_CreateObject();
    cJSON_AddStringToObject(meta, "path", path_j->valuestring);
    cJSON_AddNumberToObject(meta, "lines", lines);
    cJSON_AddNumberToObject(meta, "chars", (double)len);
    if (ref) cJSON_AddStringToObject(meta, "ref", ref);

    /* file_read MUST return content — that's its purpose.
     * The model calls file_read specifically to SEE content.
     * Truncate at 50K to prevent context explosion on huge files. */
    if (len <= 50000) {
        cJSON_AddStringToObject(meta, "content", content);
    } else {
        char *trunc = malloc(50001);
        memcpy(trunc, content, 50000);
        trunc[50000] = '\0';
        cJSON_AddStringToObject(meta, "content", trunc);
        cJSON_AddBoolToObject(meta, "truncated", 1);
        free(trunc);
    }

    { char *_pj = cJSON_PrintUnformatted(params); journal_append(ctx->journal, ctx->step, "file_read", _pj, ref,
                   len, lines, NULL); free(_pj); }

    free(content);
    return make_result(1, meta, ref);
}

/* ── file_write ──────────────────────────────────────────── */

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

    cJSON *meta = cJSON_CreateObject();
    cJSON_AddStringToObject(meta, "status", "ok");
    cJSON_AddStringToObject(meta, "path", path);
    cJSON_AddNumberToObject(meta, "bytes", (double)len);

    { char *_pj = cJSON_PrintUnformatted(params); journal_append(ctx->journal, ctx->step, "file_write", _pj, NULL,
                   len, count_lines(content), NULL); free(_pj); }

    return make_result(1, meta, NULL);
}

/* ── file_edit ───────────────────────────────────────────── */

static tool_result_t tool_file_edit(tool_ctx_t *ctx, cJSON *params) {
    cJSON *path_j = cJSON_GetObjectItem(params, "path");
    cJSON *old_j  = cJSON_GetObjectItem(params, "old_text");
    cJSON *new_j  = cJSON_GetObjectItem(params, "new_text");
    if (!path_j || !old_j || !new_j ||
        !path_j->valuestring || !old_j->valuestring || !new_j->valuestring)
        return make_error("missing path/old_text/new_text");

    const char *path = path_j->valuestring;
    const char *old_text = old_j->valuestring;
    const char *new_text = new_j->valuestring;

    size_t flen = 0;
    char *content = read_file_contents(path, &flen);
    if (!content) return make_error("cannot read file for editing");

    /* store pre-edit content */
    char *pre_ref = store_save(ctx->store, content, "txt");

    /* find and replace (first occurrence) */
    char *pos = strstr(content, old_text);
    if (!pos) {
        free(content);
        return make_error("old_text not found in file");
    }

    size_t old_len = strlen(old_text);
    size_t new_len = strlen(new_text);
    size_t result_len = flen - old_len + new_len;
    char *result = malloc(result_len + 1);
    size_t prefix_len = (size_t)(pos - content);
    memcpy(result, content, prefix_len);
    memcpy(result + prefix_len, new_text, new_len);
    memcpy(result + prefix_len + new_len, pos + old_len, flen - prefix_len - old_len);
    result[result_len] = '\0';

    FILE *f = fopen(path, "w");
    if (!f) { free(content); free(result); return make_error("cannot write file"); }
    fwrite(result, 1, result_len, f);
    fclose(f);

    /* store diff info */
    cJSON *meta = cJSON_CreateObject();
    cJSON_AddStringToObject(meta, "status", "ok");
    cJSON_AddStringToObject(meta, "path", path);
    cJSON_AddNumberToObject(meta, "old_chars", (double)old_len);
    cJSON_AddNumberToObject(meta, "new_chars", (double)new_len);
    if (pre_ref) cJSON_AddStringToObject(meta, "pre_ref", pre_ref);

    { char *_pj = cJSON_PrintUnformatted(params); journal_append(ctx->journal, ctx->step, "file_edit", _pj, pre_ref,
                   result_len, count_lines(result), NULL); free(_pj); }

    free(content);
    free(result);
    return make_result(1, meta, NULL);
}

/* ── grep_search ─────────────────────────────────────────── */

static tool_result_t tool_grep_search(tool_ctx_t *ctx, cJSON *params) {
    cJSON *pattern_j = cJSON_GetObjectItem(params, "pattern");
    cJSON *path_j    = cJSON_GetObjectItem(params, "path");
    if (!pattern_j || !pattern_j->valuestring)
        return make_error("missing 'pattern' parameter");

    const char *pattern = pattern_j->valuestring;
    const char *path = path_j && path_j->valuestring ? path_j->valuestring : ".";

    /* Resolve store/ paths relative to session directory */
    char resolved_path[4096];
    if (strncmp(path, "store/", 6) == 0) {
        snprintf(resolved_path, sizeof(resolved_path), "%s/%s", ctx->session_dir, path);
        path = resolved_path;
    }

    /* use fork/execvp to avoid shell injection via pattern/path */
    int pipefd[2];
    if (pipe(pipefd) < 0) return make_error("pipe failed");

    pid_t pid = fork();
    if (pid < 0) { close(pipefd[0]); close(pipefd[1]); return make_error("fork failed"); }

    if (pid == 0) {
        /* child: exec grep with proper argv (no shell interpolation) */
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
               "-m", "50",  /* max 50 matches (replaces | head -50) */
               pattern, path, (char *)NULL);
        _exit(127);
    }

    /* parent: read output */
    close(pipefd[1]);
    str_t out = str_new(4096);
    char buf[4096];
    ssize_t n;
    while ((n = read(pipefd[0], buf, sizeof(buf))) > 0)
        str_append(&out, buf, (size_t)n);
    close(pipefd[0]);

    int status;
    waitpid(pid, &status, 0);

    int matches = count_lines(out.data);
    char *ref = store_save(ctx->store, out.data, "txt");

    cJSON *meta = cJSON_CreateObject();
    cJSON_AddStringToObject(meta, "pattern", pattern);
    cJSON_AddStringToObject(meta, "path", path);
    cJSON_AddNumberToObject(meta, "matches", matches);
    cJSON_AddNumberToObject(meta, "chars", (double)out.len);
    if (ref) cJSON_AddStringToObject(meta, "ref", ref);

    { char *_pj = cJSON_PrintUnformatted(params); journal_append(ctx->journal, ctx->step, "grep_search", _pj, ref,
                   out.len, matches, NULL); free(_pj); }

    str_free(&out);
    return make_result(1, meta, ref);
}

/* ── notes (scratchpad) ──────────────────────────────────── */

static tool_result_t tool_notes(tool_ctx_t *ctx, cJSON *params) {
    cJSON *content_j = cJSON_GetObjectItem(params, "content");
    if (!content_j || !content_j->valuestring)
        return make_error("missing 'content' parameter");

    /* update scratchpad */
    free(ctx->scratchpad);
    ctx->scratchpad = strdup(content_j->valuestring);

    /* persist to disk */
    char path[1024];
    snprintf(path, sizeof(path), "%s/scratchpad.md", ctx->session_dir);
    FILE *f = fopen(path, "w");
    if (f) {
        fputs(ctx->scratchpad, f);
        fclose(f);
    }

    cJSON *meta = cJSON_CreateObject();
    cJSON_AddStringToObject(meta, "status", "ok");

    { char *_pj = cJSON_PrintUnformatted(params); journal_append(ctx->journal, ctx->step, "notes", _pj, NULL,
                   strlen(ctx->scratchpad), 0, NULL); free(_pj); }

    return make_result(1, meta, NULL);
}

/* ── done ────────────────────────────────────────────────── */

static tool_result_t tool_done(tool_ctx_t *ctx, cJSON *params) {
    cJSON *result_j = cJSON_GetObjectItem(params, "result");
    const char *result = result_j && result_j->valuestring
                         ? result_j->valuestring : "(no result)";

    cJSON *meta = cJSON_CreateObject();
    cJSON_AddStringToObject(meta, "result", result);

    { char *_pj = cJSON_PrintUnformatted(params); journal_append(ctx->journal, ctx->step, "done", _pj, NULL,
                   strlen(result), 0, NULL); free(_pj); }

    return make_result(1, meta, NULL);
}

/* ── dispatcher ──────────────────────────────────────────── */

tool_result_t tool_execute(tool_ctx_t *ctx, const char *action, cJSON *params) {
    if (!params) params = cJSON_CreateObject();

    if (strcmp(action, "shell_exec")  == 0) return tool_shell_exec(ctx, params);
    if (strcmp(action, "file_read")   == 0) return tool_file_read(ctx, params);
    if (strcmp(action, "file_write")  == 0) return tool_file_write(ctx, params);
    if (strcmp(action, "file_edit")   == 0) return tool_file_edit(ctx, params);
    if (strcmp(action, "grep_search") == 0) return tool_grep_search(ctx, params);
    if (strcmp(action, "notes")       == 0) return tool_notes(ctx, params);
    if (strcmp(action, "done")        == 0) return tool_done(ctx, params);

    char msg[256];
    snprintf(msg, sizeof(msg), "unknown tool: %s", action);
    return make_error(msg);
}

void tool_result_free(tool_result_t *r) {
    if (r->meta)      { cJSON_Delete(r->meta); r->meta = NULL; }
    if (r->store_ref) { free(r->store_ref); r->store_ref = NULL; }
}

/* ── system prompt ───────────────────────────────────────── */

const char *tools_system_prompt(void) {
    return
    "You are an autonomous coding agent. Solve the user's task step by step.\n"
    "\n"
    "Respond with exactly one JSON object per turn:\n"
    "{\"thought\": \"your reasoning\", \"action\": \"tool_name\", \"param1\": \"value1\", ...}\n"
    "\n"
    "Available tools:\n"
    "\n"
    "shell_exec(command) - Execute a shell command. Output is stored; you see metadata.\n"
    "  Returns: {exit_code, chars, lines, ref, preview}\n"
    "\n"
    "file_read(path) - Read a file. Content is stored; you see metadata.\n"
    "  Returns: {path, lines, chars, ref}\n"
    "\n"
    "file_write(path, content) - Write content to a file.\n"
    "  Returns: {status, path, bytes}\n"
    "\n"
    "file_edit(path, old_text, new_text) - Replace exact text in a file.\n"
    "  Always file_read first to get exact text. Returns: {status, path}\n"
    "\n"
    "grep_search(pattern, path) - Search files with regex. Results stored.\n"
    "  Returns: {pattern, path, matches, chars, ref}\n"
    "\n"
    "notes(content) - Save persistent scratchpad. Survives context resets.\n"
    "  Returns: {status}\n"
    "\n"
    "done(result) - Signal task completion with final answer.\n"
    "  Returns: {result}\n"
    "\n"
    "Rules:\n"
    "- Tool outputs are stored to disk. You see ONLY metadata (exit_code, path, lines, chars, ref).\n"
    "- To see actual content, call file_read with the ref path (e.g. file_read(path=\"store/abc123.txt\")).\n"
    "- You MUST file_read the ref if you need to see what a command output or file contains.\n"
    "- Never guess tool results. Wait for actual output.\n"
    "- file_edit: old_text must exactly match. Always file_read first.\n"
    "- Record key findings in notes after each discovery.\n"
    "- Call done when finished.\n";
}
