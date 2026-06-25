#ifndef REGRESSION_H
#define REGRESSION_H

/*
 * Regression testing framework for nash Self-Harness validation.
 *
 * Loads test query banks from ~/.nash/regression/ (YAML), runs each query
 * in headless mode via react_run(), evaluates results against criteria,
 * and produces pass/fail reports with scoring.
 *
 * The validation gate implements the Self-Harness paper's acceptance rule:
 *   Δ_in ≥ 0 AND Δ_ho ≥ 0 AND max(Δ_in, Δ_ho) > 0
 * to accept/reject harness changes based on behavioral evidence.
 *
 * Based on: Self-Harness [arXiv:2606.09498, Jun 2026]
 */

#include "config.h"
#include "provider.h"
#include "memory.h"
#include "store.h"

/* ── Criterion types ─────────────────────────────────── */

typedef enum {
    CRIT_STATUS,          /* exit status: "done" (result != NULL) */
    CRIT_CONTAINS,        /* result contains substring (case-insensitive) */
    CRIT_NOT_CONTAINS,    /* result does NOT contain substring */
    CRIT_REGEX,           /* result matches POSIX extended regex */
    CRIT_TOOL_USED,       /* journal shows tool was called */
    CRIT_TOOL_NOT_USED,   /* tool was NOT called */
    CRIT_MAX_STEPS,       /* completed within N steps */
    CRIT_NO_ERROR,        /* no tool errors in journal */
    CRIT_EXIT_CODE,       /* exit code matches (0=success, 1=failure) */
} criterion_type_t;

typedef struct {
    criterion_type_t type;
    char *expect;         /* expected value (interpretation depends on type) */
    double weight;        /* scoring weight (default 1.0) */
} criterion_t;

/* ── Test query ──────────────────────────────────────── */

typedef struct {
    char *id;             /* unique identifier */
    char *query;          /* the query text to send to nash */
    int   max_turns;      /* max react loop steps (0 = use default) */
    criterion_t *criteria;
    int n_criteria;
} test_query_t;

/* ── Query bank ──────────────────────────────────────── */

typedef enum {
    SPLIT_HELD_IN,
    SPLIT_HELD_OUT,
} split_t;

typedef struct {
    char *name;
    char *filepath;       /* source YAML file */
    split_t split;
    test_query_t *queries;
    int n_queries;
} query_bank_t;

/* ── Results ─────────────────────────────────────────── */

typedef struct {
    char *criterion_desc; /* human-readable description */
    int   passed;         /* 1 = pass, 0 = fail */
    char *detail;         /* failure detail (NULL on pass) */
} criterion_result_t;

typedef struct {
    char *query_id;
    int   passed;              /* all criteria passed */
    double score;              /* weighted score [0,1] */
    int    steps_used;         /* react loop steps taken */
    criterion_result_t *crit_results;
    int n_crit_results;
    char *result_text;         /* final result from react_run (for diagnostics) */
    char *trace_summary;       /* Self-Harness: compact execution trace from journal_manifest() */
} query_result_t;

typedef struct {
    char *bank_name;
    split_t split;
    double score;              /* aggregate score [0,1] */
    int    passed;             /* queries that passed all criteria */
    int    total;              /* total queries */
    query_result_t *results;
    int n_results;
} bank_result_t;

typedef struct {
    double overall_score;
    double held_in_score;      /* -1 if no held-in queries */
    double held_out_score;     /* -1 if no held-out queries */
    int    total_passed;
    int    total_queries;
    bank_result_t *bank_results;
    int n_bank_results;
} regression_report_t;

/* Validation gate result */
typedef enum {
    VALIDATE_ACCEPTED     = 0,  /* harness change accepted */
    VALIDATE_REJECTED     = 1,  /* harness change rejected */
    VALIDATE_INCONCLUSIVE = 2,  /* no baseline or insufficient data */
} validate_result_t;

/* ── API ─────────────────────────────────────────────── */

/* Load all query banks from a directory (e.g., ~/.nash/regression/).
 * Returns array of banks (caller frees with regression_free_banks).
 * *count set to number of banks loaded. */
query_bank_t *regression_load_banks(const char *dir, int *count);

/* Free loaded query banks */
void regression_free_banks(query_bank_t *banks, int count);

/* Run regression tests.
 * split_filter: SPLIT_HELD_IN, SPLIT_HELD_OUT, or -1 for all.
 * Returns a report (caller frees with regression_free_report). */
regression_report_t *regression_run(query_bank_t *banks, int n_banks,
                                     int split_filter,
                                     provider_t *provider,
                                     config_t *cfg,
                                     memory_t *memory,
                                     store_t *store,
                                     const char *nash_dir);

/* Print a human-readable report to stderr */
void regression_print_report(const regression_report_t *report);

/* Save report to a JSON file (for baseline comparison).
 * Returns 0 on success. */
int regression_save_report(const regression_report_t *report, const char *path);

/* Load a previously saved report from JSON.
 * Returns NULL on failure. Caller frees with regression_free_report. */
regression_report_t *regression_load_report(const char *path);

/* Compare two reports (baseline vs current).
 * Implements: Δ_in ≥ 0 AND Δ_ho ≥ 0 AND max(Δ_in, Δ_ho) > 0
 * Prints comparison to stderr. Returns validation result. */
validate_result_t regression_compare(const regression_report_t *baseline,
                                      const regression_report_t *current);

/* Free a regression report */
void regression_free_report(regression_report_t *report);

/* Write seed query bank YAML files if ~/.nash/regression/ is empty.
 * Returns 0 on success. */
int regression_write_seed(const char *regression_dir);

#endif /* REGRESSION_H */
