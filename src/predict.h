/* predict.h - Decision observability for harness evolution.
 *
 * Records predictions about harness decisions (eviction, injection,
 * compression, cycling, etc.) and verifies them against outcomes.
 * Tracks accuracy per decision type across sessions to enable
 * data-driven harness tuning.
 *
 * Inspired by AHE (arXiv 2604.25850) observability-driven evolution. */

#ifndef PREDICT_H
#define PREDICT_H

#include <stdint.h>
#include <stddef.h>
#include "journal.h"

/* ── Decision categories ── */
typedef enum {
    PREDICT_EVICTION,       /* "msg N won't be needed again" */
    PREDICT_INJECTION,      /* "memory key X will help this task" */
    PREDICT_COMPRESSION,    /* "BM25 kept essential content from msg N" */
    PREDICT_CYCLING,        /* "breaking cycle will restore progress" */
    PREDICT_IMPORTANCE,     /* "msg N is LOW/NORMAL importance" */
    PREDICT_DEDUP,          /* "content is duplicate of step N" */
    PREDICT_ERROR_RECALL,   /* "memory retrieval will help resolve error" */
    PREDICT_NUDGE,          /* "nudge will improve agent behavior" */
    PREDICT_COUNT
} predict_type_t;

/* ── Outcome states ── */
typedef enum {
    PREDICT_PENDING,        /* not yet verified */
    PREDICT_CONFIRMED,      /* prediction was correct */
    PREDICT_REFUTED,        /* prediction was wrong */
    PREDICT_EXPIRED         /* session ended before verification possible */
} predict_outcome_t;

/* ── Single prediction record ── */
typedef struct {
    predict_type_t    type;
    int               step;           /* step when prediction was made */
    double            ts;             /* timestamp (epoch) */
    char              subject[256];   /* what was acted on (msg index, key, path) */
    char              claim[512];     /* human-readable prediction text (ASCII) */
    predict_outcome_t outcome;
    int               verified_step;  /* step when outcome determined (-1 = pending) */
    double            confidence;     /* harness confidence 0.0-1.0 */
    int               decision_id;    /* monotonic ID for cross-referencing */
} prediction_t;

/* ── Per-session prediction tracker (ring buffer) ── */
#define PREDICT_RING_SIZE 128
#define PREDICT_MAX_EVICTED 64
#define PREDICT_MAX_INJECTED 32

typedef struct predict_tracker_t {
    prediction_t ring[PREDICT_RING_SIZE];
    int          head;              /* next write position */
    int          count;             /* entries in ring (max PREDICT_RING_SIZE) */
    int          next_id;           /* monotonic decision ID */

    /* Aggregate counters (lifetime, survive ring overflow) */
    int          totals[PREDICT_COUNT];
    int          confirmed[PREDICT_COUNT];
    int          refuted[PREDICT_COUNT];
    int          expired[PREDICT_COUNT];

    /* Deferred verification state */
    uint32_t     evicted_crcs[PREDICT_MAX_EVICTED];
    uint32_t     evicted_lens[PREDICT_MAX_EVICTED];
    int          evicted_steps[PREDICT_MAX_EVICTED];
    int          n_evicted;

    char        *injected_keys[PREDICT_MAX_INJECTED];
    int          n_injected;
} predict_tracker_t;

/* ── API ── */

/* Lifecycle */
predict_tracker_t *predict_tracker_new(void);
void predict_tracker_free(predict_tracker_t *pt);

/* Record a prediction. Returns decision_id for later verification. */
int predict_record(predict_tracker_t *pt, predict_type_t type, int step,
                   const char *subject, const char *claim, double confidence);

/* Verify a prediction by decision_id. */
void predict_verify(predict_tracker_t *pt, int decision_id,
                    predict_outcome_t outcome, int verified_step);

/* Verify by type+subject (for deferred checks). */
void predict_verify_by_subject(predict_tracker_t *pt, predict_type_t type,
                               const char *subject, predict_outcome_t outcome,
                               int verified_step);

/* Expire all pending predictions (call at session end). */
void predict_finalize(predict_tracker_t *pt);

/* Emit predictions to journal with store-backed refs for clickable links. */
void predict_journal_flush(predict_tracker_t *pt, void *tool_ctx);

/* Real-time verification: check pending predictions after each tool exec. */
void predict_check_triggers(predict_tracker_t *pt, int step,
                            const char *tool_name, const char *tool_path,
                            const char *content, size_t content_len,
                            int tool_failed);

/* Store an evicted content CRC for later refutation detection. */
void predict_store_evicted_crc(predict_tracker_t *pt, uint32_t crc,
                               uint32_t len, int step);

/* Store an injected memory key for later verification. */
void predict_store_injected_key(predict_tracker_t *pt, const char *key);

/* Type name for logging. */
const char *predict_type_name(predict_type_t type);

/* Outcome name for logging. */
const char *predict_outcome_name(predict_outcome_t outcome);

#endif /* PREDICT_H */
