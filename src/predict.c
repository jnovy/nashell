/* predict.c - Decision observability for harness evolution.
 *
 * Ring buffer of predictions with real-time verification.
 * See predict.h for design rationale. */

#include "predict.h"
#include "journal.h"
#include "compress.h"
#include "cJSON.h"
#include "nash_log.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ── Type/outcome name tables ── */

static const char *type_names[] = {
    "eviction", "injection", "compression", "cycling",
    "importance", "dedup", "error_recall", "nudge"
};

static const char *outcome_names[] = {
    "pending", "confirmed", "refuted", "expired"
};

const char *predict_type_name(predict_type_t type)
{
    if (type >= 0 && type < PREDICT_COUNT) return type_names[type];
    return "unknown";
}

const char *predict_outcome_name(predict_outcome_t outcome)
{
    if (outcome >= 0 && outcome <= PREDICT_EXPIRED) return outcome_names[outcome];
    return "unknown";
}

/* ── Lifecycle ── */

predict_tracker_t *predict_tracker_new(void)
{
    predict_tracker_t *pt = calloc(1, sizeof(*pt));
    return pt;
}

void predict_tracker_free(predict_tracker_t *pt)
{
    if (!pt) return;
    for (int i = 0; i < pt->n_injected; i++)
        free(pt->injected_keys[i]);
    free(pt);
}

/* ── Internal helpers ── */

static double now_ts(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* Get prediction from ring by decision_id. Returns NULL if not in ring. */
static prediction_t *ring_find(predict_tracker_t *pt, int decision_id)
{
    for (int i = 0; i < pt->count; i++) {
        int idx = (pt->head - pt->count + i + PREDICT_RING_SIZE) % PREDICT_RING_SIZE;
        if (pt->ring[idx].decision_id == decision_id)
            return &pt->ring[idx];
    }
    return NULL;
}

/* Update aggregate counters when an outcome changes. */
static void update_counters(predict_tracker_t *pt, predict_type_t type,
                            predict_outcome_t old_outcome,
                            predict_outcome_t new_outcome)
{
    /* Decrement old counter (only if not PENDING - pending is implicit) */
    switch (old_outcome) {
    case PREDICT_CONFIRMED: pt->confirmed[type]--; break;
    case PREDICT_REFUTED:   pt->refuted[type]--;   break;
    case PREDICT_EXPIRED:   pt->expired[type]--;   break;
    default: break;
    }
    /* Increment new counter */
    switch (new_outcome) {
    case PREDICT_CONFIRMED: pt->confirmed[type]++; break;
    case PREDICT_REFUTED:   pt->refuted[type]++;   break;
    case PREDICT_EXPIRED:   pt->expired[type]++;   break;
    default: break;
    }
}

/* ── Recording ── */

int predict_record(predict_tracker_t *pt, predict_type_t type, int step,
                   const char *subject, const char *claim, double confidence)
{
    if (!pt) return -1;

    /* If overwriting a pending entry, auto-expire it */
    if (pt->count == PREDICT_RING_SIZE) {
        prediction_t *oldest = &pt->ring[pt->head];
        if (oldest->outcome == PREDICT_PENDING) {
            oldest->outcome = PREDICT_EXPIRED;
            pt->expired[oldest->type]++;
        }
    }

    prediction_t *p = &pt->ring[pt->head];
    memset(p, 0, sizeof(*p));
    p->type = type;
    p->step = step;
    p->ts = now_ts();
    if (subject) snprintf(p->subject, sizeof(p->subject), "%s", subject);
    if (claim)   snprintf(p->claim, sizeof(p->claim), "%s", claim);
    p->outcome = PREDICT_PENDING;
    p->verified_step = -1;
    p->confidence = confidence;
    p->decision_id = pt->next_id++;

    pt->head = (pt->head + 1) % PREDICT_RING_SIZE;
    if (pt->count < PREDICT_RING_SIZE) pt->count++;
    pt->totals[type]++;

    return p->decision_id;
}

/* ── Verification ── */

void predict_verify(predict_tracker_t *pt, int decision_id,
                    predict_outcome_t outcome, int verified_step)
{
    if (!pt) return;
    prediction_t *p = ring_find(pt, decision_id);
    if (!p) return;
    if (p->outcome != PREDICT_PENDING) return; /* already verified */

    update_counters(pt, p->type, p->outcome, outcome);
    p->outcome = outcome;
    p->verified_step = verified_step;
}

void predict_verify_by_subject(predict_tracker_t *pt, predict_type_t type,
                               const char *subject, predict_outcome_t outcome,
                               int verified_step)
{
    if (!pt || !subject) return;
    for (int i = 0; i < pt->count; i++) {
        int idx = (pt->head - pt->count + i + PREDICT_RING_SIZE) % PREDICT_RING_SIZE;
        prediction_t *p = &pt->ring[idx];
        if (p->type == type && p->outcome == PREDICT_PENDING &&
            strcmp(p->subject, subject) == 0) {
            update_counters(pt, type, p->outcome, outcome);
            p->outcome = outcome;
            p->verified_step = verified_step;
            return; /* verify first match only */
        }
    }
}

/* ── Finalization ── */

/* Default outcomes when trigger never fires */
static const predict_outcome_t default_outcomes[PREDICT_COUNT] = {
    PREDICT_CONFIRMED,  /* EVICTION: no re-read = correct eviction */
    PREDICT_EXPIRED,    /* INJECTION: can't tell if it helped */
    PREDICT_CONFIRMED,  /* COMPRESSION: no re-read = good compression */
    PREDICT_EXPIRED,    /* CYCLING: ambiguous if session just ended */
    PREDICT_CONFIRMED,  /* IMPORTANCE: not re-requested = correct */
    PREDICT_CONFIRMED,  /* DEDUP: CRC match is reliable */
    PREDICT_EXPIRED,    /* ERROR_RECALL: ambiguous */
    PREDICT_REFUTED     /* NUDGE: no notes() call = nudge failed */
};

void predict_finalize(predict_tracker_t *pt)
{
    if (!pt) return;
    for (int i = 0; i < pt->count; i++) {
        int idx = (pt->head - pt->count + i + PREDICT_RING_SIZE) % PREDICT_RING_SIZE;
        prediction_t *p = &pt->ring[idx];
        if (p->outcome == PREDICT_PENDING) {
            predict_outcome_t final = default_outcomes[p->type];
            update_counters(pt, p->type, PREDICT_PENDING, final);
            p->outcome = final;
        }
    }
}

/* ── Journal flush ── */

void predict_journal_flush(predict_tracker_t *pt, journal_t *j, int react_loop)
{
    if (!pt || !j) return;

    for (int i = 0; i < pt->count; i++) {
        int idx = (pt->head - pt->count + i + PREDICT_RING_SIZE) % PREDICT_RING_SIZE;
        prediction_t *p = &pt->ring[idx];

        cJSON *params = cJSON_CreateObject();
        cJSON_AddNumberToObject(params, "decision_id", p->decision_id);
        cJSON_AddStringToObject(params, "type", predict_type_name(p->type));
        cJSON_AddStringToObject(params, "subject", p->subject);
        cJSON_AddStringToObject(params, "claim", p->claim);
        cJSON_AddNumberToObject(params, "confidence", p->confidence);
        cJSON_AddStringToObject(params, "outcome", predict_outcome_name(p->outcome));
        if (p->verified_step >= 0)
            cJSON_AddNumberToObject(params, "verified_step", p->verified_step);

        journal_append(j, react_loop, p->step, "prediction", params,
                       NULL, 0, 0, NULL, NULL, p->ts);
        cJSON_Delete(params);
    }

    /* Flush aggregate summary */
    cJSON *summary = cJSON_CreateObject();
    for (int t = 0; t < PREDICT_COUNT; t++) {
        if (pt->totals[t] == 0) continue;
        cJSON *entry = cJSON_CreateObject();
        cJSON_AddNumberToObject(entry, "total", pt->totals[t]);
        cJSON_AddNumberToObject(entry, "confirmed", pt->confirmed[t]);
        cJSON_AddNumberToObject(entry, "refuted", pt->refuted[t]);
        cJSON_AddNumberToObject(entry, "expired", pt->expired[t]);
        int verifiable = pt->confirmed[t] + pt->refuted[t];
        if (verifiable > 0)
            cJSON_AddNumberToObject(entry, "accuracy",
                                    (double)pt->confirmed[t] / verifiable);
        cJSON_AddItemToObject(summary, predict_type_name((predict_type_t)t), entry);
    }
    journal_append(j, react_loop, -1, "prediction_summary", summary,
                   NULL, 0, 0, NULL, NULL, now_ts());
    cJSON_Delete(summary);
}

/* ── Deferred verification state ── */

void predict_store_evicted_crc(predict_tracker_t *pt, uint32_t crc,
                               uint32_t len, int step)
{
    if (!pt || pt->n_evicted >= PREDICT_MAX_EVICTED) return;
    pt->evicted_crcs[pt->n_evicted] = crc;
    pt->evicted_lens[pt->n_evicted] = len;
    pt->evicted_steps[pt->n_evicted] = step;
    pt->n_evicted++;
}

void predict_store_injected_key(predict_tracker_t *pt, const char *key)
{
    if (!pt || !key || pt->n_injected >= PREDICT_MAX_INJECTED) return;
    pt->injected_keys[pt->n_injected] = strdup(key);
    pt->n_injected++;
}

/* ── Real-time verification triggers ── */

void predict_check_triggers(predict_tracker_t *pt, int step,
                            const char *tool_name, const char *tool_path,
                            const char *content, size_t content_len,
                            int tool_failed)
{
    if (!pt) return;

    /* Check EVICTION refutation: agent re-read content matching evicted CRC */
    if (tool_name && (strcmp(tool_name, "file_read") == 0 ||
                      strcmp(tool_name, "grep_search") == 0) &&
        content && content_len > 0) {
        uint32_t crc = compress_crc32(content, content_len);
        for (int e = 0; e < pt->n_evicted; e++) {
            if (pt->evicted_crcs[e] == crc &&
                pt->evicted_lens[e] == (uint32_t)content_len) {
                /* Agent re-read evicted content - eviction was wrong */
                char subject[256];
                snprintf(subject, sizeof(subject), "msg[%d]",
                         pt->evicted_steps[e]);
                predict_verify_by_subject(pt, PREDICT_EVICTION, subject,
                                          PREDICT_REFUTED, step);
            }
        }
        /* Also check COMPRESSION refutation: re-read same file */
        if (tool_path) {
            char subject[256];
            /* We use tool_path as subject for compression predictions */
            snprintf(subject, sizeof(subject), "%s", tool_path);
            predict_verify_by_subject(pt, PREDICT_COMPRESSION, subject,
                                      PREDICT_REFUTED, step);
        }
        /* Also check IMPORTANCE refutation: LOW-imp content re-requested */
        if (tool_path) {
            char subject[256];
            snprintf(subject, sizeof(subject), "%s", tool_path);
            predict_verify_by_subject(pt, PREDICT_IMPORTANCE, subject,
                                      PREDICT_REFUTED, step);
        }
    }

    /* Check NUDGE confirmation: notes() called within window */
    if (tool_name && strcmp(tool_name, "notes") == 0) {
        for (int i = 0; i < pt->count; i++) {
            int idx = (pt->head - pt->count + i + PREDICT_RING_SIZE) % PREDICT_RING_SIZE;
            prediction_t *p = &pt->ring[idx];
            if (p->type == PREDICT_NUDGE && p->outcome == PREDICT_PENDING &&
                (step - p->step) <= 3) {
                update_counters(pt, PREDICT_NUDGE, PREDICT_PENDING, PREDICT_CONFIRMED);
                p->outcome = PREDICT_CONFIRMED;
                p->verified_step = step;
            }
        }
    }

    /* Check NUDGE expiry: 3 steps passed without notes() */
    for (int i = 0; i < pt->count; i++) {
        int idx = (pt->head - pt->count + i + PREDICT_RING_SIZE) % PREDICT_RING_SIZE;
        prediction_t *p = &pt->ring[idx];
        if (p->type == PREDICT_NUDGE && p->outcome == PREDICT_PENDING &&
            (step - p->step) > 3) {
            update_counters(pt, PREDICT_NUDGE, PREDICT_PENDING, PREDICT_REFUTED);
            p->outcome = PREDICT_REFUTED;
            p->verified_step = step;
        }
    }

    /* Check CYCLING confirmation: productive step within 3 steps of break */
    if (tool_name && !tool_failed) {
        for (int i = 0; i < pt->count; i++) {
            int idx = (pt->head - pt->count + i + PREDICT_RING_SIZE) % PREDICT_RING_SIZE;
            prediction_t *p = &pt->ring[idx];
            if (p->type == PREDICT_CYCLING && p->outcome == PREDICT_PENDING &&
                (step - p->step) <= 3 && (step - p->step) > 0) {
                /* Productive = non-trivial tool call that succeeded */
                if (strcmp(tool_name, "file_edit") == 0 ||
                    strcmp(tool_name, "file_write") == 0 ||
                    strcmp(tool_name, "shell_exec") == 0 ||
                    strcmp(tool_name, "done") == 0) {
                    update_counters(pt, PREDICT_CYCLING, PREDICT_PENDING,
                                    PREDICT_CONFIRMED);
                    p->outcome = PREDICT_CONFIRMED;
                    p->verified_step = step;
                }
            }
        }
    }

    /* Check CYCLING expiry: 3 steps passed without productive step */
    for (int i = 0; i < pt->count; i++) {
        int idx = (pt->head - pt->count + i + PREDICT_RING_SIZE) % PREDICT_RING_SIZE;
        prediction_t *p = &pt->ring[idx];
        if (p->type == PREDICT_CYCLING && p->outcome == PREDICT_PENDING &&
            (step - p->step) > 3) {
            update_counters(pt, PREDICT_CYCLING, PREDICT_PENDING, PREDICT_REFUTED);
            p->outcome = PREDICT_REFUTED;
            p->verified_step = step;
        }
    }

    /* Check ERROR_RECALL: if same error recurs within 5 steps, refute */
    if (tool_failed && tool_name) {
        for (int i = 0; i < pt->count; i++) {
            int idx = (pt->head - pt->count + i + PREDICT_RING_SIZE) % PREDICT_RING_SIZE;
            prediction_t *p = &pt->ring[idx];
            if (p->type == PREDICT_ERROR_RECALL && p->outcome == PREDICT_PENDING &&
                (step - p->step) <= 5) {
                update_counters(pt, PREDICT_ERROR_RECALL, PREDICT_PENDING,
                                PREDICT_REFUTED);
                p->outcome = PREDICT_REFUTED;
                p->verified_step = step;
            }
        }
    }

    /* Check ERROR_RECALL confirmation: 5 steps without recurrence */
    for (int i = 0; i < pt->count; i++) {
        int idx = (pt->head - pt->count + i + PREDICT_RING_SIZE) % PREDICT_RING_SIZE;
        prediction_t *p = &pt->ring[idx];
        if (p->type == PREDICT_ERROR_RECALL && p->outcome == PREDICT_PENDING &&
            (step - p->step) > 5) {
            update_counters(pt, PREDICT_ERROR_RECALL, PREDICT_PENDING,
                            PREDICT_CONFIRMED);
            p->outcome = PREDICT_CONFIRMED;
            p->verified_step = step;
        }
    }

    /* Check INJECTION: if agent references an injected key */
    if (tool_name && strcmp(tool_name, "memory_search") == 0 && tool_path) {
        predict_verify_by_subject(pt, PREDICT_INJECTION, tool_path,
                                  PREDICT_CONFIRMED, step);
    }
}
