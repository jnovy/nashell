/* harness_metrics.c - Cross-session prediction accuracy tracking.
 * See harness_metrics.h for API documentation. */

#include "harness_metrics.h"
#include "cJSON.h"
#include "nash_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/* EMA blending factor for component scores */
#define EMA_ALPHA 0.1

/* Accuracy threshold below which a recommendation is generated */
#define ACCURACY_LOW_THRESHOLD 0.6

/* ── Lifecycle ── */

harness_metrics_t *harness_metrics_load(const char *nash_dir)
{
    harness_metrics_t *m = calloc(1, sizeof(*m));
    if (!m) return NULL;
    m->version = 1;

    if (!nash_dir) return m;

    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/harness_metrics.json", nash_dir);

    FILE *f = fopen(path, "r");
    if (!f) return m; /* missing file = defaults */

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (sz <= 0 || sz > 1024 * 1024) {
        fclose(f);
        return m;
    }

    char *buf = malloc(sz + 1);
    if (!buf) { fclose(f); return m; }
    size_t nr = fread(buf, 1, sz, f);
    fclose(f);
    buf[nr] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        nash_log("harness_metrics: corrupt JSON in %s, using defaults", path);
        return m;
    }

    cJSON *v = cJSON_GetObjectItem(root, "version");
    if (v) m->version = v->valueint;

    cJSON *sc = cJSON_GetObjectItem(root, "sessions_counted");
    if (sc) m->sessions_counted = sc->valueint;

    cJSON *acc = cJSON_GetObjectItem(root, "accuracy");
    if (acc) {
        for (int t = 0; t < PREDICT_COUNT; t++) {
            cJSON *entry = cJSON_GetObjectItem(acc, predict_type_name((predict_type_t)t));
            if (!entry) continue;
            cJSON *c = cJSON_GetObjectItem(entry, "confirmed");
            cJSON *r = cJSON_GetObjectItem(entry, "refuted");
            cJSON *e = cJSON_GetObjectItem(entry, "expired");
            if (c) m->types[t].confirmed = c->valueint;
            if (r) m->types[t].refuted = r->valueint;
            if (e) m->types[t].expired = e->valueint;
        }
    }

    cJSON *cs = cJSON_GetObjectItem(root, "component_scores");
    if (cs) {
        for (int t = 0; t < PREDICT_COUNT; t++) {
            cJSON *s = cJSON_GetObjectItem(cs, predict_type_name((predict_type_t)t));
            if (s) m->component_scores[t] = s->valuedouble;
        }
    }

    cJSON *pa = cJSON_GetObjectItem(root, "prev_accuracy");
    if (pa) {
        for (int t = 0; t < PREDICT_COUNT; t++) {
            cJSON *p = cJSON_GetObjectItem(pa, predict_type_name((predict_type_t)t));
            if (p) m->prev_accuracy[t] = p->valuedouble;
        }
    }

    cJSON *tr = cJSON_GetObjectItem(root, "trend");
    if (tr) {
        for (int t = 0; t < PREDICT_COUNT; t++) {
            cJSON *d = cJSON_GetObjectItem(tr, predict_type_name((predict_type_t)t));
            if (d) m->delta[t] = d->valuedouble;
        }
    }

    cJSON_Delete(root);
    return m;
}

int harness_metrics_save(const harness_metrics_t *m, const char *nash_dir)
{
    if (!m || !nash_dir) return -1;

    char path[PATH_MAX], tmp_path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/harness_metrics.json", nash_dir);
    snprintf(tmp_path, sizeof(tmp_path), "%s/harness_metrics.json.tmp", nash_dir);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "version", m->version);
    cJSON_AddNumberToObject(root, "sessions_counted", m->sessions_counted);

    /* accuracy */
    cJSON *acc = cJSON_CreateObject();
    for (int t = 0; t < PREDICT_COUNT; t++) {
        cJSON *entry = cJSON_CreateObject();
        cJSON_AddNumberToObject(entry, "confirmed", m->types[t].confirmed);
        cJSON_AddNumberToObject(entry, "refuted", m->types[t].refuted);
        cJSON_AddNumberToObject(entry, "expired", m->types[t].expired);
        int verifiable = m->types[t].confirmed + m->types[t].refuted;
        if (verifiable > 0)
            cJSON_AddNumberToObject(entry, "accuracy",
                                    (double)m->types[t].confirmed / verifiable);
        cJSON_AddItemToObject(acc, predict_type_name((predict_type_t)t), entry);
    }
    cJSON_AddItemToObject(root, "accuracy", acc);

    /* component_scores */
    cJSON *cs = cJSON_CreateObject();
    for (int t = 0; t < PREDICT_COUNT; t++)
        cJSON_AddNumberToObject(cs, predict_type_name((predict_type_t)t),
                                m->component_scores[t]);
    cJSON_AddItemToObject(root, "component_scores", cs);

    /* prev_accuracy */
    cJSON *pa = cJSON_CreateObject();
    for (int t = 0; t < PREDICT_COUNT; t++)
        cJSON_AddNumberToObject(pa, predict_type_name((predict_type_t)t),
                                m->prev_accuracy[t]);
    cJSON_AddItemToObject(root, "prev_accuracy", pa);

    /* trend */
    cJSON *tr = cJSON_CreateObject();
    for (int t = 0; t < PREDICT_COUNT; t++)
        cJSON_AddNumberToObject(tr, predict_type_name((predict_type_t)t),
                                m->delta[t]);
    cJSON_AddItemToObject(root, "trend", tr);

    /* Atomic write: .tmp -> rename */
    char *json = cJSON_Print(root);
    cJSON_Delete(root);
    if (!json) return -1;

    FILE *f = fopen(tmp_path, "w");
    if (!f) {
        nash_log("harness_metrics: can't write %s: %s", tmp_path, strerror(errno));
        free(json);
        return -1;
    }
    fputs(json, f);
    fclose(f);
    free(json);

    if (rename(tmp_path, path) != 0) {
        nash_log("harness_metrics: rename %s -> %s failed: %s",
                 tmp_path, path, strerror(errno));
        return -1;
    }

    return 0;
}

void harness_metrics_free(harness_metrics_t *m)
{
    free(m);
}

/* ── Update ── */

void harness_metrics_update(harness_metrics_t *m, const predict_tracker_t *pt)
{
    if (!m || !pt) return;

    m->sessions_counted++;

    for (int t = 0; t < PREDICT_COUNT; t++) {
        /* Accumulate raw counts */
        m->types[t].confirmed += pt->confirmed[t];
        m->types[t].refuted   += pt->refuted[t];
        m->types[t].expired   += pt->expired[t];

        /* Compute this session's accuracy for this type */
        int verifiable = pt->confirmed[t] + pt->refuted[t];
        double session_acc = -1.0;
        if (verifiable > 0)
            session_acc = (double)pt->confirmed[t] / verifiable;

        /* Update trend */
        if (session_acc >= 0.0) {
            m->delta[t] = session_acc - m->prev_accuracy[t];
            m->prev_accuracy[t] = session_acc;

            /* Update component score via EMA */
            if (m->component_scores[t] == 0.0 && m->sessions_counted <= 1) {
                /* First session: initialize directly */
                m->component_scores[t] = session_acc;
            } else {
                m->component_scores[t] = EMA_ALPHA * session_acc +
                                          (1.0 - EMA_ALPHA) * m->component_scores[t];
            }
        }
    }
}

/* ── Query ── */

double harness_metrics_accuracy(const harness_metrics_t *m, predict_type_t type)
{
    if (!m || type < 0 || type >= PREDICT_COUNT) return -1.0;
    int verifiable = m->types[type].confirmed + m->types[type].refuted;
    if (verifiable == 0) return -1.0;
    return (double)m->types[type].confirmed / verifiable;
}

int harness_metrics_recommend(const harness_metrics_t *m,
                              harness_recommendation_t *out, int max_recs)
{
    if (!m || !out || max_recs <= 0) return 0;

    int n = 0;
    for (int t = 0; t < PREDICT_COUNT && n < max_recs; t++) {
        int verifiable = m->types[t].confirmed + m->types[t].refuted;
        if (verifiable < 10) continue; /* not enough data */

        double acc = (double)m->types[t].confirmed / verifiable;

        if (acc < ACCURACY_LOW_THRESHOLD) {
            out[n].type = (predict_type_t)t;
            out[n].current_accuracy = acc;
            out[n].trend = m->delta[t];
            out[n].sample_size = verifiable;
            out[n].recommendation = "decrease_threshold";
            n++;
        } else if (acc > 0.95 && m->delta[t] >= 0.0) {
            /* Very high accuracy + stable/improving = could be more aggressive */
            out[n].type = (predict_type_t)t;
            out[n].current_accuracy = acc;
            out[n].trend = m->delta[t];
            out[n].sample_size = verifiable;
            out[n].recommendation = "increase_threshold";
            n++;
        }
    }

    return n;
}
