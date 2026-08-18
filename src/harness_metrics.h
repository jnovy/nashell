/* harness_metrics.h - Cross-session prediction accuracy tracking.
 *
 * Persists decision accuracy to ~/.nash/harness_metrics.json.
 * Enables data-driven tuning of eviction, injection, compression,
 * and cycling parameters. */

#ifndef HARNESS_METRICS_H
#define HARNESS_METRICS_H

#include "predict.h"

/* Per-type accuracy record */
typedef struct {
    int confirmed;
    int refuted;
    int expired;
} hm_type_record_t;

/* Cross-session metrics */
typedef struct {
    int              version;        /* format version (currently 1) */
    int              sessions_counted;
    hm_type_record_t types[PREDICT_COUNT];

    /* Component effectiveness scores (EMA, alpha=0.1) */
    double           component_scores[PREDICT_COUNT];

    /* Trend tracking (current - previous session accuracy) */
    double           prev_accuracy[PREDICT_COUNT];
    double           delta[PREDICT_COUNT];
} harness_metrics_t;

/* Recommendation from metrics analysis */
typedef struct {
    predict_type_t type;
    double         current_accuracy;
    double         trend;
    int            sample_size;
    const char    *recommendation; /* "increase_threshold" | "decrease_threshold" | "no_change" */
} harness_recommendation_t;

/* ── API ── */

/* Load metrics from nash_dir/harness_metrics.json.
 * Returns heap-allocated struct (defaults if file missing/corrupt). */
harness_metrics_t *harness_metrics_load(const char *nash_dir);

/* Save metrics to nash_dir/harness_metrics.json (atomic write). */
int harness_metrics_save(const harness_metrics_t *m, const char *nash_dir);

/* Update metrics from a completed session's prediction tracker. */
void harness_metrics_update(harness_metrics_t *m, const predict_tracker_t *pt);

/* Get recommendations for parameter tuning.
 * Returns number of recommendations written (up to max_recs). */
int harness_metrics_recommend(const harness_metrics_t *m,
                              harness_recommendation_t *out, int max_recs);

/* Free a harness_metrics_t. */
void harness_metrics_free(harness_metrics_t *m);

/* Compute accuracy for a type: confirmed / (confirmed + refuted).
 * Returns -1.0 if no verifiable predictions. */
double harness_metrics_accuracy(const harness_metrics_t *m, predict_type_t type);

#endif /* HARNESS_METRICS_H */
