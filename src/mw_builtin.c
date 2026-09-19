/* mw_builtin.c - Built-in middleware hooks for Nash tool dispatch.
 *
 * Four hooks registered at startup:
 *
 *  1. mw_destructive_guard  (pre-hook,  priority=0)
 *     Blocks shell_exec with dangerous patterns (rm -rf /, mkfs, dd, etc.)
 *
 *  2. mw_read_tracker       (post-hook, priority=5)
 *     Records file paths from successful file_read/grep_search into
 *     ctx->read_files[] so the read-before-edit enforcer can check them.
 *
 *  3. mw_read_before_edit   (pre-hook,  priority=10)
 *     Blocks file_edit on files that haven't been read first.
 *     Paper 12 (AHE, arXiv 2604.25850): encoding behavior in tools gives
 *     +3.3pp vs system-prompt-only which REGRESSES by -2.3pp.
 *
 *  4. mw_context_hints      (post-hook, priority=50)
 *     After successful file_read on source files, surfaces related unread
 *     files (header, test file) as an annotation on the result.
 *     Paper 12: auto-surfacing contract hints is the +3.3pp tool improvement.
 */

#include "tools_internal.h"
#include "mw_builtin.h"
#include <string.h>
#include <unistd.h>

/* ---- helpers ---------------------------------------------------------- */

/* Record a file path in the read-files tracker (dedup, bounded). */
static void track_read_file(tool_ctx_t *ctx, const char *path) {
  if (!path || !path[0]) return;
  /* Dedup: skip if already tracked */
  for (int i = 0; i < ctx->n_read_files; i++) {
    if (strcmp(ctx->read_files[i], path) == 0) return;
  }
  if (ctx->n_read_files >= READ_FILES_MAX) return; /* full - silently allow */
  ctx->read_files[ctx->n_read_files++] = xstrdup(path);
}

/* Check if a path has been read (is in the read-files tracker). */
static int was_file_read(tool_ctx_t *ctx, const char *path) {
  if (!path) return 0;
  for (int i = 0; i < ctx->n_read_files; i++) {
    if (strcmp(ctx->read_files[i], path) == 0) return 1;
  }
  return 0;
}

/* Check if a path has been modified this session (file_write created it). */
static int was_file_modified(tool_ctx_t *ctx, const char *path) {
  if (!path) return 0;
  for (int i = 0; i < ctx->n_modified_files; i++) {
    if (ctx->modified_files[i].path &&
        strcmp(ctx->modified_files[i].path, path) == 0)
      return 1;
  }
  return 0;
}

/* Get file extension from path (returns pointer into path, or ""). */
static const char *get_ext(const char *path) {
  const char *dot = strrchr(path, '.');
  const char *slash = strrchr(path, '/');
  if (!dot) return "";
  if (slash && dot < slash) return ""; /* dot in directory name */
  return dot;
}

/* ---- Hook 1: Destructive Operation Guard ------------------------------ */

static int is_path_char(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
}

enum { DG_SUBSTR = 0, DG_PATH_BOUNDARY = 1, DG_NO_SUBDIR = 2 };

static int mw_destructive_guard_pre(struct tool_ctx_struct *ctx,
                                    const char *action, cJSON *params,
                                    char **block_msg) {
  (void)ctx;
  if (strcmp(action, "shell_exec") != 0) return 1; /* allow */

  const char *cmd = json_str(params, "command");
  if (!cmd) return 1;

  static const struct {
    const char *pattern;
    int mode;
  } dangerous[] = {
    {"rm -rf /",        DG_PATH_BOUNDARY},
    {"rm -rf ~",        DG_NO_SUBDIR},
    {"rm -rf $HOME",    DG_NO_SUBDIR},
    {"mkfs.",           DG_SUBSTR},
    {"dd if=/dev/",     DG_SUBSTR},
    {"> /dev/sd",       DG_SUBSTR},
    {"> /dev/nvme",     DG_SUBSTR},
    {":(){ :|:& };:",   DG_SUBSTR},
    {"chmod -R 777 /",  DG_PATH_BOUNDARY},
    {NULL, 0}
  };

  for (int i = 0; dangerous[i].pattern; i++) {
    const char *hit = strstr(cmd, dangerous[i].pattern);
    if (!hit) continue;
    char next = hit[strlen(dangerous[i].pattern)];
    if (dangerous[i].mode == DG_PATH_BOUNDARY && is_path_char(next))
      continue;
    if (dangerous[i].mode == DG_NO_SUBDIR && next == '/')
      continue;

    char buf[256];
    snprintf(buf, sizeof(buf),
             "Blocked: command matches destructive pattern '%s'. "
             "If this is intentional, break it into safer steps.",
             dangerous[i].pattern);
    *block_msg = xstrdup(buf);
    return 0; /* block */
  }
  return 1; /* allow */
}

/* ---- Hook 2: Read Tracker --------------------------------------------- */

static char *mw_read_tracker_post(struct tool_ctx_struct *ctx,
                                  const char *action, cJSON *params,
                                  tool_result_t *result) {
  if (!result->success) return NULL;

  if (strcmp(action, "file_read") == 0) {
    const char *path = json_str(params, "path");
    if (path) track_read_file(ctx, path);
  } else if (strcmp(action, "grep_search") == 0) {
    /* grep_search's "path" param is the search target (file or dir).
     * If it's a file (not a directory), track it as read. */
    const char *path = json_str(params, "path");
    if (path && access(path, F_OK) == 0) {
      /* Check if it's a regular file (not a directory) */
      struct stat st;
      if (stat(path, &st) == 0 && S_ISREG(st.st_mode))
        track_read_file(ctx, path);
    }
  } else if (strcmp(action, "file_write") == 0) {
    /* file_write creates a file - count as "known" for edit purposes */
    const char *path = json_str(params, "path");
    if (path) track_read_file(ctx, path);
  }
  return NULL; /* no annotation */
}

/* ---- Hook 3: Read-Before-Edit Enforcer -------------------------------- */

static int mw_read_before_edit_pre(struct tool_ctx_struct *ctx,
                                   const char *action, cJSON *params,
                                   char **block_msg) {
  if (strcmp(action, "file_edit") != 0) return 1; /* allow */

  const char *path = json_str(params, "path");
  if (!path || !path[0]) return 1; /* let param validation handle it */

  /* Allow if the file was read via file_read or grep_search */
  if (was_file_read(ctx, path)) return 1;

  /* Allow if the file was written/modified this session */
  if (was_file_modified(ctx, path)) return 1;

  /* Allow if the read_files tracker is full (fail open, not closed) */
  if (ctx->n_read_files >= READ_FILES_MAX) return 1;

  *block_msg = xstrdup(
    "You must read the file before editing it. "
    "Use file_read first to see the exact text you want to change.");
  return 0; /* block */
}

/* ---- Hook 4: Context Hint Injection ----------------------------------- */

static char *mw_context_hints_post(struct tool_ctx_struct *ctx,
                                   const char *action, cJSON *params,
                                   tool_result_t *result) {
  if (!result->success) return NULL;
  if (strcmp(action, "file_read") != 0) return NULL;

  const char *path = json_str(params, "path");
  if (!path || !path[0]) return NULL;

  const char *ext = get_ext(path);
  /* Only suggest hints for source files */
  if (strcmp(ext, ".c") != 0 && strcmp(ext, ".h") != 0 &&
      strcmp(ext, ".py") != 0 && strcmp(ext, ".rs") != 0 &&
      strcmp(ext, ".go") != 0 && strcmp(ext, ".ts") != 0 &&
      strcmp(ext, ".js") != 0)
    return NULL;

  /* Build candidate related paths */
  char candidates[8][512];
  int n_candidates = 0;

  /* Extract directory and basename without extension */
  char dir[512] = "";
  char base[256] = "";
  const char *last_slash = strrchr(path, '/');
  if (last_slash) {
    int dir_len = (int)(last_slash - path);
    snprintf(dir, sizeof(dir), "%.*s", dir_len, path);
    snprintf(base, sizeof(base), "%s", last_slash + 1);
  } else {
    snprintf(base, sizeof(base), "%s", path);
  }

  /* Strip extension from base */
  char base_no_ext[256];
  snprintf(base_no_ext, sizeof(base_no_ext), "%s", base);
  char *dot = strrchr(base_no_ext, '.');
  if (dot) *dot = '\0';

  if (strcmp(ext, ".c") == 0) {
    /* foo.c -> foo.h */
    if (dir[0])
      snprintf(candidates[n_candidates], sizeof(candidates[0]),
               "%s/%s.h", dir, base_no_ext);
    else
      snprintf(candidates[n_candidates], sizeof(candidates[0]),
               "%s.h", base_no_ext);
    n_candidates++;

    /* foo.c -> test_foo.c */
    if (dir[0])
      snprintf(candidates[n_candidates], sizeof(candidates[0]),
               "%s/test_%s.c", dir, base_no_ext);
    else
      snprintf(candidates[n_candidates], sizeof(candidates[0]),
               "test_%s.c", base_no_ext);
    n_candidates++;
  } else if (strcmp(ext, ".h") == 0) {
    /* foo.h -> foo.c */
    if (dir[0])
      snprintf(candidates[n_candidates], sizeof(candidates[0]),
               "%s/%s.c", dir, base_no_ext);
    else
      snprintf(candidates[n_candidates], sizeof(candidates[0]),
               "%s.c", base_no_ext);
    n_candidates++;
  } else if (strcmp(ext, ".py") == 0) {
    /* foo.py -> test_foo.py */
    if (dir[0])
      snprintf(candidates[n_candidates], sizeof(candidates[0]),
               "%s/test_%s.py", dir, base_no_ext);
    else
      snprintf(candidates[n_candidates], sizeof(candidates[0]),
               "test_%s.py", base_no_ext);
    n_candidates++;
  }

  /* Filter: exists AND not already read */
  char hints[2048] = "";
  int hint_count = 0;
  for (int i = 0; i < n_candidates; i++) {
    if (access(candidates[i], F_OK) != 0) continue;
    if (was_file_read(ctx, candidates[i])) continue;
    if (hint_count > 0) strncat(hints, ", ", sizeof(hints) - strlen(hints) - 1);
    strncat(hints, candidates[i], sizeof(hints) - strlen(hints) - 1);
    hint_count++;
  }

  if (hint_count == 0) return NULL;

  char *result_str = xmalloc(strlen(hints) + 64);
  snprintf(result_str, strlen(hints) + 64,
           "Related unread files: %s", hints);
  return result_str;
}

/* ---- Registration ----------------------------------------------------- */

static int mw_initialized = 0;

void mw_builtin_init(void) {
  if (mw_initialized) return;
  mw_initialized = 1;

  /* Priority 0: destructive guard runs first (safety) */
  tools_middleware_register(&(tool_middleware_t){
    .name = "destructive_guard",
    .pre_hook = mw_destructive_guard_pre,
    .post_hook = NULL,
    .priority = 0
  });

  /* Priority 5: read tracker runs early to populate ctx->read_files
   * before any other post-hook might need it */
  tools_middleware_register(&(tool_middleware_t){
    .name = "read_tracker",
    .pre_hook = NULL,
    .post_hook = mw_read_tracker_post,
    .priority = 5
  });

  /* Priority 10: read-before-edit enforcer */
  tools_middleware_register(&(tool_middleware_t){
    .name = "read_before_edit",
    .pre_hook = mw_read_before_edit_pre,
    .post_hook = NULL,
    .priority = 10
  });

  /* Priority 50: context hints run late (informational, not blocking) */
  tools_middleware_register(&(tool_middleware_t){
    .name = "context_hints",
    .pre_hook = NULL,
    .post_hook = mw_context_hints_post,
    .priority = 50
  });

  nash_log("[mw_builtin] registered 4 built-in middleware hooks");
}
