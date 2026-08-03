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
  FAIL_TOOL_ERROR,   /* tool returned an error */
  FAIL_STEP_LIMIT,   /* hit max_steps without completing */
  FAIL_NULL_RESULT,  /* react_run returned NULL */
  FAIL_CYCLING,      /* repeated identical actions */
  FAIL_EMPTY_RESULT, /* done() called with empty result */
} fail_terminal_cause_t;

typedef enum {
  FMECH_FILE_EDIT_MISMATCH, /* file_edit with wrong old_text */
  FMECH_UNREAD_REF,         /* tool output not read before proceeding */
  FMECH_SHELL_RETRY,        /* same shell command retried after failure */
  FMECH_CONTEXT_EVICTION,   /* key info lost to context eviction */
  FMECH_WRONG_TOOL,         /* used wrong tool for the task */
  FMECH_HALLUCINATION,      /* acted on information not in context */
  FMECH_SPEC_VIOLATION,     /* tool call with invalid parameters */
  FMECH_OTHER,              /* uncategorized */
} fail_mechanism_t;

/* A single failure instance from a session */
typedef struct {
  char *session_dir; /* path to session directory */
  int react_loop;    /* which react loop failed */
  int step;          /* which step the error occurred at */
  fail_terminal_cause_t cause;
  fail_mechanism_t mechanism;
  char *tool;      /* tool that failed (or last tool called) */
  char *error_msg; /* error message if any */
  char *context;   /* surrounding context (prev+next steps) */
} failure_instance_t;

/* A cluster of similar failures */
typedef struct {
  fail_terminal_cause_t cause;
  fail_mechanism_t mechanism;
  char *tool;                    /* most common tool in this cluster */
  int count;                     /* number of instances */
  int n_sessions;                /* number of unique sessions affected */
  failure_instance_t *instances; /* representative instances (up to 5) */
  int n_instances;
  char *summary; /* human-readable summary */
} failure_cluster_t;

/* ── SWE-Shepherd: Step-level scoring [arXiv:2604.10493] ── */

/* Step productivity score — heuristic classification of each tool call.
 * Based on SWE-Shepherd's step-level supervision for code agents. */
typedef enum {
  STEP_SPINNING = -2,  /* repeated same action (cycling) */
  STEP_HARMFUL = -1,   /* tool failed, or caused a retry/correction */
  STEP_WASTEFUL = 0,   /* tool succeeded but output was never referenced */
  STEP_NEUTRAL = 1,    /* tool succeeded, result usage unclear */
  STEP_PRODUCTIVE = 2, /* tool succeeded, result was used later */
} step_score_t;

/* Per-session trajectory quality metrics */
typedef struct {
  int n_steps;
  int n_productive;
  int n_wasteful;
  int n_harmful;
  int n_spinning;
  float efficiency;  /* productive / total */
  float waste_ratio; /* wasteful / total */
  int longest_productive_streak;
  int longest_harmful_streak;
  int causal_step;   /* earliest step that caused failure (-1 if N/A) */
  char *causal_tool; /* tool at causal step (NULL if N/A) */
} trajectory_score_t;

/* Full postmortem report */
typedef struct {
  int total_sessions; /* sessions analyzed */
  int total_failures; /* total failure instances found */
  failure_cluster_t *clusters;
  int n_clusters;
  char *evidence_bundle; /* formatted text for LLM consumption */
  /* SWE-Shepherd: aggregate trajectory quality */
  int trajectory_sessions;          /* sessions with trajectory data */
  float avg_efficiency;             /* mean efficiency across sessions */
  float avg_waste_ratio;            /* mean waste ratio across sessions */
  trajectory_score_t *trajectories; /* per-session trajectory data */
  int n_trajectories;
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
