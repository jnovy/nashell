#ifndef PROMPT_OPTIMIZE_H
#define PROMPT_OPTIMIZE_H

/*
 * Self-Harness prompt optimization for Nash.
 *
 * Implements the Self-Harness iterative loop [arXiv:2606.09498]:
 *   1. Evaluate current harness on held-in/held-out splits
 *   2. Weakness Mining: cluster failures by signature (cause, status, mechanism)
 *   3. Harness Proposal: generate K diverse, minimal candidate edits
 *   4. Proposal Validation: accept edits only if Δ_in ≥ 0, Δ_ho ≥ 0, max > 0
 *   5. Merge accepted edits into next harness version
 *
 * Key design from paper:
 *   - Same model proposes its own harness improvements (self-improvement)
 *   - Failure signatures ground proposals in behavioral evidence
 *   - Parallel proposals (width K) explore diverse improvements per round
 *   - Accepted edits are merged; rejected edits logged for future rounds
 *   - Minimality: each edit targets one failure mechanism
 *
 * Nash-specific mapping:
 *   - regression.c IS the evaluator (typed criteria with weights)
 *   - held-in/held-out splits prevent overfitting
 *   - system_prompt_extra IS the editable harness surface
 *   - model profiles make results portable per model
 *   - journal_manifest() provides execution traces for weakness mining
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

/* ── Self-Harness failure signature [§3.2] ──────────── */

typedef enum {
    SH_CAUSE_NO_DONE,        /* agent did not call done */
    SH_CAUSE_WRONG_RESULT,   /* done called but result incorrect */
    SH_CAUSE_WRONG_TOOL,     /* used wrong tool / didn't use required tool */
    SH_CAUSE_TOO_MANY_STEPS, /* exceeded step budget */
    SH_CAUSE_ERRORS,         /* tool errors in execution */
    SH_CAUSE_OTHER,          /* unclassifiable */
} sh_cause_t;

typedef enum {
    SH_STATUS_CAUSAL,        /* agent behavior directly caused failure */
    SH_STATUS_CONTRIBUTING,  /* agent behavior contributed to failure */
    SH_STATUS_UNKNOWN,       /* causality unclear */
} sh_status_t;

typedef enum {
    SH_MECH_TOOL_CHOICE,     /* wrong tool selection */
    SH_MECH_TASK_ABANDON,    /* stopped without completing task */
    SH_MECH_LOOP,            /* repeated unproductive actions */
    SH_MECH_INCOMPLETE,      /* partial result missing required content */
    SH_MECH_ERROR_CASCADE,   /* unrecovered errors */
    SH_MECH_OVER_EXPLORE,    /* excessive exploration without action */
    SH_MECH_OTHER,           /* unclassifiable */
} sh_mechanism_t;

/* Failure signature φ(r_i) = (cause, status, mechanism) */
typedef struct {
    sh_cause_t     cause;
    sh_status_t    status;
    sh_mechanism_t mechanism;
} sh_signature_t;

/* A cluster of failures sharing the same signature */
typedef struct {
    sh_signature_t  sig;
    int             count;          /* number of failures in this cluster */
    char          **query_ids;      /* query IDs in this cluster */
    char          **trace_excerpts; /* truncated trace per failure (for evidence) */
    char          **crit_details;   /* criterion failure descriptions */
    int             n_entries;
} sh_cluster_t;

/* Evidence bundle B_t — structured output of weakness mining */
typedef struct {
    sh_cluster_t *clusters;
    int           n_clusters;
    int           total_failures;
    int           total_passes;
    char         *passing_summary;  /* brief summary of what works */
} sh_evidence_t;

/* ── Candidate prompt ───────────────────────────────── */

typedef struct {
    char  *prompt_text;    /* the candidate prompt text */
    double score;          /* overall regression score [0,1] */
    double held_in_score;  /* held-in split score (-1 if N/A) */
    double held_out_score; /* held-out split score (-1 if N/A) */
    int    total_passed;   /* number of queries passed */
    int    total_queries;  /* total queries */
    int    round;          /* which round produced this */
    char  *audit;          /* Self-Harness: targeted failure pattern + rationale */
} prompt_candidate_t;

/* ── Rejected proposal (for cross-round memory) ─────── */

typedef struct {
    char *prompt_text;       /* the rejected prompt text */
    char *audit;             /* what it targeted and why it was rejected */
    int   round;
    double delta_in;
    double delta_out;
} rejected_proposal_t;

/* ── Optimization configuration ─────────────────────── */

typedef struct {
    int         max_rounds;       /* budget: number of reflection rounds per epoch (T) */
    int         proposal_width;   /* candidates per round (K), default 2 */
    provider_t *student;          /* model being optimized */
    provider_t *reflection;       /* model doing the reflecting (can be same) */
    const char *profile_path;     /* model profile .toml to update (NULL = don't write) */
    int         split_filter;     /* -1=all, SPLIT_HELD_IN, SPLIT_HELD_OUT */
    int         verbose;          /* print detailed progress */
    /* SkillOpt extensions [arXiv:2605.23904v2] */
    int         n_epochs;         /* training epochs (0/1 = GEPA mode, >1 = SkillOpt) */
    int         edit_budget_init; /* L_0: max edits per step (0 = unlimited, default 4) */
    int         edit_budget_floor;/* L_min: min edits at end of epoch (default 2) */
    int         minibatch_size;   /* failures per reflection minibatch (0 = all-at-once) */
} optimize_config_t;

/* ── API ─────────────────────────────────────────────── */

/* Stage 1: Weakness Mining [§3.2]
 * Analyze regression report to build a structured evidence bundle.
 * Classifies each failure into a signature, clusters by exact agreement,
 * orders by support count, and includes execution traces.
 * Caller must free with optimize_free_evidence_bundle(). */
sh_evidence_t *optimize_build_evidence_bundle(const regression_report_t *report);

/* Format an evidence bundle as text for the proposer LM.
 * Returns malloc'd string. Caller must free. */
char *optimize_format_evidence(const sh_evidence_t *bundle,
                               const rejected_proposal_t *rejected,
                               int n_rejected);

/* Legacy: format simple feedback (backwards compatible).
 * Returns malloc'd string. Caller must free. */
char *optimize_format_feedback(const regression_report_t *report);

/* Stage 2: Harness Proposal [§3.3]
 * Ask the reflection LM to propose an improved prompt targeting a specific
 * failure cluster. Each call produces one candidate.
 * target_cluster: index into evidence bundle clusters (-1 = let LM choose).
 * edit_budget: max add/delete/replace ops this round (0 = unlimited).
 * slow_guidance: cross-epoch longitudinal guidance (NULL if epoch 0).
 * Returns malloc'd string. Caller must free. */
char *optimize_reflect(provider_t *reflection_lm,
                       const char *current_prompt,
                       const char *evidence_text,
                       int round, int max_rounds,
                       int proposal_idx, int proposal_width,
                       int edit_budget,
                       const char *slow_guidance);

/* Stage 3+: Full Self-Harness loop [Algorithm 1]
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

/* Free functions */
void optimize_free_candidate(prompt_candidate_t *c);
void optimize_free_evidence_bundle(sh_evidence_t *b);

#endif /* PROMPT_OPTIMIZE_H */
