#ifndef POSTMORTEM_H
#define POSTMORTEM_H

/*
 * Failure pattern mining for nash Self-Harness.
 *
 * Scans session journals (journal.jsonl) to identify recurring failure
 * patterns, clusters them by mechanism, and produces evidence bundles
 * for the Self-Harness proposal stage.
 *
 * Failure signature: φ(r) = (terminal_cause, causal_status, agent_mechanism)
 *
 * Based on: Self-Harness [arXiv:2606.09498, Jun 2026] — Weakness Mining stage
 */

#include <stddef.h>

/* ── Failure types ───────────────────────────────────── */

typedef enum {
    FAIL_TOOL_ERROR,       /* tool returned an error */
    FAIL_STEP_LIMIT,       /* hit max_steps without completing */
    FAIL_NULL_RESULT,      /* react_run returned NULL */
    FAIL_CYCLING,          /* repeated identical actions */
    FAIL_EMPTY_RESULT,     /* done() called with empty result */
} fail_terminal_cause_t;

typedef enum {
    FMECH_FILE_EDIT_MISMATCH,  /* file_edit with wrong old_text */
    FMECH_UNREAD_REF,          /* tool output not read before proceeding */
    FMECH_SHELL_RETRY,         /* same shell command retried after failure */
    FMECH_CONTEXT_EVICTION,    /* key info lost to context eviction */
    FMECH_WRONG_TOOL,          /* used wrong tool for the task */
    FMECH_HALLUCINATION,       /* acted on information not in context */
    FMECH_SPEC_VIOLATION,      /* tool call with invalid parameters */
    FMECH_OTHER,               /* uncategorized */
} fail_mechanism_t;

/* A single failure instance from a session */
typedef struct {
    char *session_dir;         /* path to session directory */
    int   react_loop;          /* which react loop failed */
    int   step;                /* which step the error occurred at */
    fail_terminal_cause_t cause;
    fail_mechanism_t mechanism;
    char *tool;                /* tool that failed (or last tool called) */
    char *error_msg;           /* error message if any */
    char *context;             /* surrounding context (prev+next steps) */
} failure_instance_t;

/* A cluster of similar failures */
typedef struct {
    fail_terminal_cause_t cause;
    fail_mechanism_t mechanism;
    char *tool;                /* most common tool in this cluster */
    int   count;               /* number of instances */
    failure_instance_t *instances;  /* representative instances (up to 5) */
    int   n_instances;
    char *summary;             /* human-readable summary */
} failure_cluster_t;

/* Full postmortem report */
typedef struct {
    int total_sessions;        /* sessions analyzed */
    int total_failures;        /* total failure instances found */
    failure_cluster_t *clusters;
    int n_clusters;
    char *evidence_bundle;     /* formatted text for LLM consumption */
} postmortem_report_t;

/* ── API ─────────────────────────────────────────────── */

/* Scan recent sessions and produce a postmortem report.
 * nash_dir: base directory (~/.nash/)
 * max_sessions: maximum sessions to scan (0 = all)
 * Returns report (caller frees with postmortem_free). */
postmortem_report_t *postmortem_analyze(const char *nash_dir, int max_sessions);

/* Print human-readable postmortem report to stderr */
void postmortem_print(const postmortem_report_t *report);

/* Save evidence bundle to a file (for self-harness proposal stage) */
int postmortem_save(const postmortem_report_t *report, const char *path);

/* Free a postmortem report */
void postmortem_free(postmortem_report_t *report);

#endif /* POSTMORTEM_H */
