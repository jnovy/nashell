#ifndef PROMPT_OPTIMIZE_H
#define PROMPT_OPTIMIZE_H

/*
 * GEPA-inspired prompt optimization for Nash.
 *
 * Implements an iterative optimization loop that:
 *   1. Runs the regression test suite to score the current prompt
 *   2. Formats failures as textual feedback
 *   3. Sends feedback to a reflection LM to propose improved instructions
 *   4. Scores the candidate and keeps the best
 *
 * Based on: DSPy GEPA optimizer (Stanford NLP, 2025-2026)
 *   Key insight: use a strong reflection LM to optimize prompts for
 *   a (possibly weaker) student LM, guided by metric feedback.
 *
 * Nash-specific advantages:
 *   - regression.c IS the metric function (typed criteria with weights)
 *   - held-in/held-out split prevents overfitting
 *   - system_prompt_extra IS the optimization target
 *   - model profiles make results portable per model
 */

#include "config.h"
#include "provider.h"
#include "memory.h"
#include "store.h"
#include "regression.h"

/* ── Budget presets ─────────────────────────────────── */

typedef enum {
    OPTIMIZE_LIGHT  = 3,   /* ~3 rounds, ~4 regression runs */
    OPTIMIZE_MEDIUM = 6,   /* ~6 rounds, ~7 regression runs */
    OPTIMIZE_HEAVY  = 10,  /* ~10 rounds, ~11 regression runs */
} optimize_budget_t;

/* ── Candidate prompt ───────────────────────────────── */

typedef struct {
    char  *prompt_text;    /* the candidate prompt text */
    double score;          /* overall regression score [0,1] */
    double held_in_score;  /* held-in split score (-1 if N/A) */
    double held_out_score; /* held-out split score (-1 if N/A) */
    int    total_passed;   /* number of queries passed */
    int    total_queries;  /* total queries */
    int    round;          /* which round produced this */
} prompt_candidate_t;

/* ── Optimization configuration ─────────────────────── */

typedef struct {
    int         max_rounds;       /* budget: number of reflection rounds */
    provider_t *student;          /* model being optimized */
    provider_t *reflection;       /* model doing the reflecting (can be same as student) */
    const char *profile_path;     /* model profile .toml to update (NULL = don't write) */
    int         split_filter;     /* -1=all, SPLIT_HELD_IN, SPLIT_HELD_OUT */
    int         verbose;          /* print detailed progress */
} optimize_config_t;

/* ── API ─────────────────────────────────────────────── */

/* Format failure feedback from a regression report.
 * Returns malloc'd string summarizing all failures with criterion details.
 * Caller must free. Returns NULL if no failures. */
char *optimize_format_feedback(const regression_report_t *report);

/* Ask the reflection LM to propose an improved prompt.
 * Returns malloc'd string with the new candidate prompt text.
 * Caller must free. Returns NULL on failure. */
char *optimize_reflect(provider_t *reflection_lm,
                       const char *current_prompt,
                       const char *feedback_summary,
                       int round, int max_rounds);

/* Run the full optimization loop.
 * Returns the best candidate found. Caller must free candidate.prompt_text.
 * If profile_path is set, writes the winning prompt to the model profile. */
prompt_candidate_t optimize_run(optimize_config_t *opt,
                                query_bank_t *banks, int n_banks,
                                config_t *cfg,
                                memory_t *memory,
                                store_t *store,
                                const char *nash_dir);

/* Parse a budget string ("light", "medium", "heavy", or integer).
 * Returns the number of rounds, or -1 on invalid input. */
int optimize_parse_budget(const char *budget_str);

/* Free prompt_candidate_t internals */
void optimize_free_candidate(prompt_candidate_t *c);

#endif /* PROMPT_OPTIMIZE_H */
