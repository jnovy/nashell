#ifndef SUBPROCESS_H
#define SUBPROCESS_H

#include <stdatomic.h>
#include "str.h"

/* ── Unified subprocess execution ─────────────────────────────────
 * Replaces 6+ copy-pasted fork/setsid/pipe/poll/kill patterns with
 * two helpers that cover every use case in the codebase:
 *
 *   subprocess_run()        — capture stdout (and optionally stderr)
 *   subprocess_run_silent() — no output, just exit code + timeout
 *
 * Child isolation (always applied):
 *   - setsid()         — new session + process group
 *   - stdin from /dev/null
 *   - kill(-pid, SIGKILL) on timeout — kills entire process group
 * ────────────────────────────────────────────────────────────────── */

/* Flags for subprocess_run() */
#define SUBPROCESS_PIPE_STDERR (1 << 0) /* pipe stderr too (default: /dev/null) */

typedef struct {
  int exit_code;     /* >=0 from WEXITSTATUS, -1 on error, -2 on timeout */
  int timed_out;     /* 1 if killed by timeout */
  int output_capped; /* 1 if hit max_bytes or max_lines */
  int aborted;       /* 1 if killed by external abort_flag */
  int line_count;    /* number of newlines in output */
} subprocess_result_t;

/* Run a command, capture output into *out.
 *
 * argv:        NULL-terminated argument vector (passed to execvp)
 * workdir:     if non-NULL, child does chdir() before exec
 * timeout_sec: wall-clock limit (0 = no limit)
 * max_bytes:   output byte cap (0 = no limit)
 * max_lines:   output line cap (0 = no limit)
 * flags:       SUBPROCESS_PIPE_STDERR or 0
 * out:         must point to an initialized str_t; output is appended
 * abort_flag:  if non-NULL, checked every 100ms; when set to non-zero
 *              the child is killed and result.aborted=1 (NULL = ignore)
 *
 * Returns a subprocess_result_t with exit_code, timed_out, etc. */
subprocess_result_t subprocess_run(char *const argv[],
                                   const char *workdir,
                                   int timeout_sec,
                                   int max_bytes,
                                   int max_lines,
                                   unsigned flags,
                                   str_t *out,
                                   atomic_int *abort_flag);

/* Run a command silently (no output capture).
 *
 * argv:        NULL-terminated argument vector
 * workdir:     if non-NULL, child does chdir() before exec
 * timeout_sec: wall-clock limit (0 = no limit)
 *
 * Returns exit code (>=0), -1 on error, -2 on timeout. */
int subprocess_run_silent(char *const argv[],
                          const char *workdir,
                          int timeout_sec);

#endif /* SUBPROCESS_H */
