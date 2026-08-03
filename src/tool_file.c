#include "tools_internal.h"
#include "tool_plugin.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include <unistd.h>
#include <limits.h>

static int path_has_traversal(const char *path);

/* BUG FIX: Detect symlink-based escapes by resolving the real path
 * and verifying it stays within the current working directory.
 * Returns 1 if the path escapes, 0 if safe, -1 on error. */
static int path_escapes_cwd(const char *path) {
  char cwd[PATH_MAX];
  char real[PATH_MAX];
  if (!getcwd(cwd, sizeof(cwd))) return -1;
  if (!realpath(path, real)) {
    /* If the file doesn't exist yet (e.g. file_write), resolve parent dir */
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", path);
    char *slash = strrchr(tmp, '/');
    if (slash) {
      *slash = '\0';
      if (!realpath(tmp, real)) return -1;
    } else {
      /* File in current directory, no parent to resolve */
      return 0;
    }
  }
  size_t cwdlen = strlen(cwd);
  /* Real path must start with cwd and be followed by '/' or end */
  if (strncmp(real, cwd, cwdlen) != 0) return 1;
  if (real[cwdlen] != '\0' && real[cwdlen] != '/') return 1;
  return 0;
}

/* ── file_read ───────────────────────────────────────── */

tool_result_t tool_file_read(tool_ctx_t *ctx, cJSON *params) {
  TOOL_REQ_STR(params, "path", path);
  const char *orig_path = path; /* before resolve */

  /* Resolve aliases and store/ paths */
  char *resolved = NULL;
  char resolved_buf[NASH_PATH_MAX];
  path = tools_resolve_path(ctx, path, resolved_buf, &resolved);

  /* Reject path traversal attempts (BUG FIX: was missing, unlike file_write/file_edit) */
  if (path_has_traversal(path)) {
    free(resolved);
    return tools_make_error("file_read: path must not contain '..' components.");
  }

  /* BUG FIX: Reject symlink-based escapes outside workspace */
  if (path_escapes_cwd(path) > 0) {
    free(resolved);
    return tools_make_error("file_read: path resolves outside the workspace directory.");
  }

  /* Safety: check file type and size before reading */
  struct stat st;
  if (stat(path, &st) != 0) {
    char msg[4224];
    snprintf(msg, sizeof(msg), "cannot stat '%.4095s': %s", path, strerror(errno));
    free(resolved);
    return tools_make_error(msg);
  }
  if (!S_ISREG(st.st_mode)) {
    free(resolved);
    return tools_make_error("not a regular file (refusing to read pipes, devices, etc.)");
  }
  int max_file = ctx->cfg ? ctx->cfg->file_max_size : 52428800; /* 50MB default */
  if (st.st_size > max_file) {
    char msg[256];
    snprintf(msg, sizeof(msg), "file too large (%ld bytes, limit %d)", (long)st.st_size, max_file);
    free(resolved);
    return tools_make_error(msg);
  }

  size_t len = 0;
  char *content = slurp_file(path, &len);
  if (!content) {
    char msg[4224];
    snprintf(msg, sizeof(msg), "cannot read '%.4095s': %s", path, strerror(errno));
    free(resolved);
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
        free(resolved); /* free heap-allocated alias resolution */
        char msg[4224];
        snprintf(msg, sizeof(msg),
                 "'%.4060s' appears to be a binary file (not text). "
                 "file_read only supports text files. For images, "
                 "use image_analyze instead.",
                 path);
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
  int start_line = json_int(params, "start_line", 0);
  int end_line = json_int(params, "end_line", 0);

  char *display_content = NULL; /* content to show (with line numbers if range) */
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
      if (line_num > range_end) break; /* early exit */
    }

    display_content = str_steal(&out);
    display_len = strlen(display_content);
    display_lines = range_end - range_start + 1;
  }

  const char *store_content = display_content ? display_content : content;
  char *hash = store_save(ctx->store, store_content);
  char *alias = tool_register_alias(ctx, hash ? hash : "");

  cJSON *meta = cJSON_CreateObject();
  cJSON_AddStringToObject(meta, "path", orig_path);
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
                  ? ctx->provider->cfg.chars_per_token
                  : 3.5f;
    int ctx_chars = (int)((double)ctx->provider->cfg.context_size * cpt * ctx->cfg->file_read_context_pct / 100.0);
    if (ctx_chars > 0 && ctx_chars < max_inline)
      max_inline = ctx_chars;
  }

  /* file_read MUST return content — that's its purpose */
  if ((int)display_len <= max_inline) {
    cJSON_AddStringToObject(meta, "content", store_content);
  } else {
    char *trunc = xmalloc(max_inline + 1);
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

  /* Rec #5: Smart tool wrapper — auto-annotate with contextual hints [AHE]
     * When reading a .c file, check for companion .h file and extract function
     * signatures as structural context. This encodes the "auto-surfaces contract
     * hints from files near each command" pattern from AHE's evolved harness. */
  {
    const char *ext = strrchr(orig_path, '.');
    if (ext && strcmp(ext, ".c") == 0) {
      /* Build companion .h path */
      size_t base_len = (size_t)(ext - orig_path);
      char *h_path = xmalloc(base_len + 3);
      if (h_path) {
        memcpy(h_path, orig_path, base_len);
        strcpy(h_path + base_len, ".h");
        struct stat h_st;
        if (stat(h_path, &h_st) == 0 && S_ISREG(h_st.st_mode) &&
            h_st.st_size < 32768) {
          size_t h_len = 0;
          char *h_content = slurp_file(h_path, &h_len);
          if (h_content) {
            /* Extract function-like lines (contain '(' and end with ';') */
            str_t sigs = str_new(512);
            const char *lp = h_content;
            int sig_count = 0;
            while (*lp && sig_count < 20) {
              const char *le = strchr(lp, '\n');
              if (!le) le = lp + strlen(lp);
              int ll = (int)(le - lp);
              /* Heuristic: line has '(' and ends with ';' or '),' = likely signature */
              if (ll > 8 && ll < 300) {
                const char *paren = memchr(lp, '(', ll);
                if (paren && (lp[ll - 1] == ';' || lp[ll - 1] == ',')) {
                  /* Skip comments, #defines, typedefs */
                  if (lp[0] != '#' && lp[0] != '/' && lp[0] != '*' &&
                      !memchr(lp, '{', ll)) {
                    str_append(&sigs, lp, ll);
                    str_append_cstr(&sigs, "\n");
                    sig_count++;
                  }
                }
              }
              lp = *le ? le + 1 : le;
            }
            if (sig_count > 0) {
              cJSON_AddStringToObject(meta, "header_hint",
                                      str_cstr(&sigs));
            }
            str_free(&sigs);
            free(h_content);
          }
        }
        free(h_path);
      }
    }
  }

  tools_inject_thought(ctx, params);
  tool_journal(ctx, "file_read", params, alias,
               display_len, display_lines, NULL, NULL);

  char *ref_copy = xstrdup(alias);
  free(alias);
  free(content);
  free(display_content); /* NULL-safe: free line-range extracted content */
  free(hash);
  free(resolved); /* BUG 2 fix: free heap-allocated alias resolution */
  return tools_make_result(1, meta, ref_copy);
}

/* ── path traversal guard ────────────────────────────── */

/* Returns 1 if path contains a ".." component (path traversal). */
static int path_has_traversal(const char *path) {
  const char *p = path;
  while ((p = strstr(p, "..")) != NULL) {
    /* Check that ".." is a full component: preceded by / or start, followed by / or end */
    int at_start = (p == path || p[-1] == '/');
    int at_end = (p[2] == '\0' || p[2] == '/');
    if (at_start && at_end) return 1;
    p += 2;
  }
  return 0;
}

/* ── file_write ──────────────────────────────────────── */

tool_result_t tool_file_write(tool_ctx_t *ctx, cJSON *params) {
  TOOL_REQ_STR(params, "path", path);
  const char *content = json_str(params, "content");
  if (!content)
    return tools_make_error("file_write requires a 'content' parameter.");

  /* Reject path traversal attempts */
  if (path_has_traversal(path))
    return tools_make_error("file_write: path must not contain '..' components.");

  /* BUG FIX: Reject symlink-based escapes outside workspace */
  if (path_escapes_cwd(path) > 0)
    return tools_make_error("file_write: path resolves outside the workspace directory.");

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

  tool_result_t res = tool_result_ok();
  cJSON_AddStringToObject(res.meta, "path", path);
  cJSON_AddNumberToObject(res.meta, "bytes", (double)len);
  cJSON_AddStringToObject(res.meta, "ref", alias);

  tools_inject_thought(ctx, params);
  tool_journal(ctx, "file_write", params, alias,
               len, count_lines(content), NULL, NULL);

  res.store_ref = xstrdup(alias);
  free(alias);
  free(hash);
  return res;
}

/* ── file_edit ───────────────────────────────────────── */

tool_result_t tool_file_edit(tool_ctx_t *ctx, cJSON *params) {
  TOOL_REQ_STR(params, "path", path);
  TOOL_REQ_STR(params, "old_text", old_text);
  const char *new_text = json_str(params, "new_text");
  if (!new_text)
    return tools_make_error("file_edit requires a 'new_text' parameter.");

  /* Reject path traversal attempts */
  if (path_has_traversal(path))
    return tools_make_error("file_edit: path must not contain '..' components.");

  /* BUG FIX: Reject symlink-based escapes outside workspace */
  if (path_escapes_cwd(path) > 0)
    return tools_make_error("file_edit: path resolves outside the workspace directory.");

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
               "surrounding context to disambiguate",
               match_count, path);
      return tools_make_error(errmsg);
    }
  }

  /* Build new content */
  size_t new_len = strlen(new_text);
  size_t result_len = flen - old_len + new_len;
  char *result = xmalloc(result_len + 1);

  size_t prefix_len = (size_t)(pos - content);
  memcpy(result, content, prefix_len);
  memcpy(result + prefix_len, new_text, new_len);
  memcpy(result + prefix_len + new_len, pos + old_len, flen - prefix_len - old_len);
  result[result_len] = '\0';

  if (write_file(path, result, result_len) < 0) {
    free(content);
    free(result);
    free(pre_hash);
    free(pre_alias);
    return tools_make_error("cannot write file");
  }

  /* Smart wrapper: read-after-write verification [AHE-inspired].
     * Catches silent write failures (disk full, permissions changed mid-write,
     * NFS stale handles, etc.) at the tool level with zero token cost -- the
     * agent never needs to re-read to confirm its edit applied. */
  {
    size_t verify_len = 0;
    char *verify = slurp_file(path, &verify_len);
    if (!verify || verify_len != result_len ||
        memcmp(verify, result, result_len) != 0) {
      free(verify);
      free(content);
      free(result);
      free(pre_hash);
      free(pre_alias);
      return tools_make_error(
        "file_edit: write verification failed -- file content "
        "does not match expected result (disk full? permissions?)");
    }
    free(verify);
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
      while (scan >= content && *scan != '\n')
        scan--;
      line_start = scan + 1;
    }

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
    char *diff = xmalloc(diff_cap);
    int diff_len = 0;
    int actual_added = 0, actual_removed = 0; /* filled by diff algorithm */

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
        if (safe_realloc((void **)&diff, diff_cap)) {
          free(diff);
          free(content);
          free(result);
          free(pre_hash);
          free(pre_alias);
          free(post_hash);
          free(post_alias);
          return tools_make_error("out of memory");
        }
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
      typedef struct {
        const char *s;
        int len;
      } dline_t;
      int old_cap = 64, new_cap = 64;
      int old_cnt = 0, new_cnt = 0;
      dline_t *old_lines = xmalloc((size_t)old_cap * sizeof(dline_t));
      dline_t *new_lines = xmalloc((size_t)new_cap * sizeof(dline_t));
      if (!old_lines || !new_lines) {
        free(old_lines);
        free(new_lines);
        free(diff);
        free(content);
        free(result);
        free(pre_hash);
        free(pre_alias);
        free(post_hash);
        free(post_alias);
        return tools_make_error("out of memory");
      }

      /* Parse old_text into lines */
      const char *p = pos;
      while (p < pos + old_len) {
        const char *nl = memchr(p, '\n', old_len - (size_t)(p - pos));
        int llen = nl ? (int)(nl - p) : (int)(pos + old_len - p);
        if (old_cnt >= old_cap) {
          old_cap *= 2;
          if (safe_realloc((void **)&old_lines, (size_t)old_cap * sizeof(dline_t))) {
            free(old_lines);
            free(new_lines);
            free(diff);
            free(content);
            free(result);
            free(pre_hash);
            free(pre_alias);
            free(post_hash);
            free(post_alias);
            return tools_make_error("out of memory");
          }
        }
        old_lines[old_cnt++] = (dline_t){p, llen};
        p = nl ? nl + 1 : p + llen;
      }

      /* Parse new_text into lines */
      p = new_text;
      while (p < new_text + new_len) {
        const char *nl = memchr(p, '\n', new_len - (size_t)(p - new_text));
        int llen = nl ? (int)(nl - p) : (int)(new_text + new_len - p);
        if (new_cnt >= new_cap) {
          new_cap *= 2;
          if (safe_realloc((void **)&new_lines, (size_t)new_cap * sizeof(dline_t))) {
            free(old_lines);
            free(new_lines);
            free(diff);
            free(content);
            free(result);
            free(pre_hash);
            free(pre_alias);
            free(post_hash);
            free(post_alias);
            return tools_make_error("out of memory");
          }
        }
        new_lines[new_cnt++] = (dline_t){p, llen};
        p = nl ? nl + 1 : p + llen;
      }

      /* Extend partial first/last lines to full file lines.
             * When old_text starts or ends mid-line, the diff would show
             * only the matched fragment, making it look like the entire
             * file line is just that fragment.  Extending to full file
             * lines gives proper context. */

      /* Extend old_lines[0] backward to line_start (start of file line) */
      if (old_cnt > 0 && pos > line_start) {
        old_lines[0].len += (int)(old_lines[0].s - line_start);
        old_lines[0].s = line_start;
      }

      /* Extend old_lines[last] forward to end of file line */
      if (old_cnt > 0) {
        dline_t *last_ol = &old_lines[old_cnt - 1];
        const char *end_of_old = last_ol->s + last_ol->len;
        /* Check if old_text ends mid-line (not at \n or EOF) */
        if (end_of_old < content + flen && *end_of_old != '\n') {
          const char *eol = memchr(end_of_old, '\n',
                                   (size_t)((content + flen) - end_of_old));
          if (eol)
            last_ol->len = (int)(eol - last_ol->s);
          else
            last_ol->len = (int)((content + flen) - last_ol->s);
        }
      }

      /* Extend new_lines to full file lines using the result buffer.
             * new_text in result starts at result + prefix_len.
             * The file line containing it starts at result + (line_start - content).
             * new_lines[] pointers currently point into the new_text param;
             * we remap them into result so they include surrounding content. */
      if (new_cnt > 0) {
        const char *new_in_result = result + prefix_len;
        const char *new_file_line_start = result + (size_t)(line_start - content);

        /* Extend first new line backward */
        if (new_in_result > new_file_line_start) {
          int extra = (int)(new_in_result - new_file_line_start);
          /* We need to point into result, not new_text */
          new_lines[0].s = new_file_line_start;
          new_lines[0].len += extra;
        }

        /* Extend last new line forward to end of file line in result */
        dline_t *last_nl = &new_lines[new_cnt - 1];
        /* Map last_nl->s into result if it's still pointing at new_text */
        const char *last_nl_in_result;
        if (last_nl->s >= new_text && last_nl->s < new_text + new_len)
          last_nl_in_result = new_in_result + (last_nl->s - new_text);
        else
          last_nl_in_result = last_nl->s;
        const char *end_of_last = last_nl_in_result + last_nl->len;
        if (end_of_last < result + result_len && *end_of_last != '\n') {
          const char *eol = memchr(end_of_last, '\n',
                                   (size_t)((result + result_len) - end_of_last));
          int new_total;
          if (eol)
            new_total = (int)(eol - last_nl_in_result);
          else
            new_total = (int)((result + result_len) - last_nl_in_result);
          last_nl->s = last_nl_in_result;
          last_nl->len = new_total;
        }
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

      /* Emit common prefix as context (capped to ctx_before lines,
             * showing only the lines closest to the actual change) */
      int prefix_skip = prefix > ctx_before ? prefix - ctx_before : 0;
      /* Advance line numbers past skipped prefix lines */
      old_lnum += prefix_skip;
      new_lnum += prefix_skip;
      for (int i = prefix_skip; i < prefix; i++) {
        dline_t *dl = &new_lines[i];
        if ((size_t)(diff_len + dl->len + 16) >= diff_cap) {
          diff_cap *= 2;
          if (safe_realloc((void **)&diff, diff_cap)) {
            free(old_lines);
            free(new_lines);
            free(diff);
            free(content);
            free(result);
            free(pre_hash);
            free(pre_alias);
            free(post_hash);
            free(post_alias);
            return tools_make_error("out of memory");
          }
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
          if (safe_realloc((void **)&diff, diff_cap)) {
            free(old_lines);
            free(new_lines);
            free(diff);
            free(content);
            free(result);
            free(pre_hash);
            free(pre_alias);
            free(post_hash);
            free(post_alias);
            return tools_make_error("out of memory");
          }
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
          if (safe_realloc((void **)&diff, diff_cap)) {
            free(old_lines);
            free(new_lines);
            free(diff);
            free(content);
            free(result);
            free(pre_hash);
            free(pre_alias);
            free(post_hash);
            free(post_alias);
            return tools_make_error("out of memory");
          }
        }
        diff_len += snprintf(diff + diff_len, diff_cap - (size_t)diff_len,
                             "  %5d +%.*s\n", new_lnum, dl->len, dl->s);
        new_lnum++;
        actual_added++;
      }

      /* Emit common suffix as context (capped to ctx_before lines,
             * showing only the lines closest to the actual change) */
      int suffix_show = suffix > ctx_before ? ctx_before : suffix;
      for (int i = old_cnt - suffix; i < old_cnt - suffix + suffix_show; i++) {
        dline_t *dl = &new_lines[new_cnt - suffix + (i - (old_cnt - suffix))];
        if ((size_t)(diff_len + dl->len + 16) >= diff_cap) {
          diff_cap *= 2;
          if (safe_realloc((void **)&diff, diff_cap)) {
            free(old_lines);
            free(new_lines);
            free(diff);
            free(content);
            free(result);
            free(pre_hash);
            free(pre_alias);
            free(post_hash);
            free(post_alias);
            return tools_make_error("out of memory");
          }
        }
        diff_len += snprintf(diff + diff_len, diff_cap - (size_t)diff_len,
                             "  %5d  %.*s\n", new_lnum, dl->len, dl->s);
        old_lnum++;
        new_lnum++;
      }
      /* Advance line numbers past any remaining suffix lines we didn't show */
      int suffix_skip = suffix - suffix_show;
      old_lnum += suffix_skip;
      new_lnum += suffix_skip;

      free(old_lines);
      free(new_lines);
    }

    /* Context after — use new_lnum (post-edit line numbers).
         * If old_text ended mid-line, after_edit points mid-line too.
         * Since we extended old/new lines to cover full file lines,
         * skip to the next line to avoid duplicating the tail. */
    cl = after_edit;
    if (cl < result + result_len && cl > result) {
      /* If we're not at start of a line, skip to next line */
      if (cl[-1] != '\n' && cl[0] != '\n') {
        char *nl = memchr(cl, '\n', (size_t)((result + result_len) - cl));
        if (nl)
          cl = nl + 1;
        else
          cl = result + result_len; /* no more lines */
      } else if (cl[0] == '\n') {
        cl++; /* skip the newline itself */
      }
    }
    while (cl < ctx_end) {
      char *nl = strchr(cl, '\n');
      int llen = nl ? (int)(nl - cl) : (int)(ctx_end - cl);
      if ((size_t)(diff_len + llen + 16) >= diff_cap) {
        diff_cap *= 2;
        if (safe_realloc((void **)&diff, diff_cap)) {
          free(diff);
          free(content);
          free(result);
          free(pre_hash);
          free(pre_alias);
          free(post_hash);
          free(post_alias);
          return tools_make_error("out of memory");
        }
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
        if (safe_realloc((void **)&diff, diff_cap)) {
          free(diff);
          free(content);
          free(result);
          free(pre_hash);
          free(pre_alias);
          free(post_hash);
          free(post_alias);
          return tools_make_error("out of memory");
        }
      }
      memmove(diff + hlen, diff, (size_t)diff_len + 1); /* +1 for NUL */
      memcpy(diff, hdr, (size_t)hlen);
      diff_len += hlen;
    }

    /* Store diff as the display ref */
    char *diff_hash = store_save(ctx->store, diff);
    char *diff_alias = tool_register_alias(ctx, diff_hash ? diff_hash : "");
    free(diff);
    free(diff_hash);

    tool_result_t res = tool_result_ok();
    cJSON_AddStringToObject(res.meta, "path", path);
    cJSON_AddStringToObject(res.meta, "pre_ref", pre_alias);
    cJSON_AddStringToObject(res.meta, "post_ref", post_alias);

    tools_inject_thought(ctx, params);
    tool_journal(ctx, "file_edit", params, diff_alias,
                 diff_len, 0, NULL, NULL);

    free(content);
    free(result);
    free(pre_alias);
    free(pre_hash);
    free(post_hash);
    free(post_alias);
    res.store_ref = diff_alias;
    return res;
  }
}

/* ── Plugin registration ──────────────────────────────── */

static const tool_param_t file_read_params[] = {
  TOOL_PARAM("path", "string", "File path", 1),
  TOOL_PARAM("start_line", "integer", "First line to read (1-based, default: 1). Negative = from end (-20 = last 20 lines)", 0),
  TOOL_PARAM("end_line", "integer", "Last line to read (1-based inclusive, default: EOF)", 0),
  TOOL_PARAM_END};

static const tool_param_t file_write_params[] = {
  TOOL_PARAM("path", "string", "File path", 1),
  TOOL_PARAM("content", "string", "File content", 1),
  TOOL_PARAM_END};

static const tool_param_t file_edit_params[] = {
  TOOL_PARAM("path", "string", "File path", 1),
  TOOL_PARAM("old_text", "string", "Exact text to find (must match)", 1),
  TOOL_PARAM("new_text", "string", "Replacement text", 1),
  TOOL_PARAM_END};

static const tool_plugin_t file_plugins[] = {
  TOOL_DEF("file_read",
           "Read contents of a file. Supports line ranges to avoid reading entire large files. Use start_line/end_line for specific sections (1-based). Negative start_line reads from end (e.g., -20 = last 20 lines).",
           file_read_params, tool_file_read),
  TOOL_DEF("file_write",
           "Write content to a file (under workspace dir).",
           file_write_params, tool_file_write),
  TOOL_DEF("file_edit",
           "Edit a file by replacing exact text. Always file_read first to copy exact text.",
           file_edit_params, tool_file_edit),
};
TOOL_PLUGIN_REGISTER_ARRAY(file_plugins, 3)
