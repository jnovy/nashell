/*
 * Failure pattern mining for nash Self-Harness.
 *
 * Scans session journals to identify recurring failure patterns,
 * clusters them by mechanism signature φ(r) = (cause, status, mechanism),
 * and produces evidence bundles for the proposal stage.
 *
 * Based on: Self-Harness [arXiv:2606.09498, Jun 2026] — Weakness Mining
 */

#include "postmortem.h"
#include "cJSON.h"
#include "str.h"
#include "nash_limits.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>

/* ── Helpers ─────────────────────────────────────────── */

static const char *cause_str(fail_terminal_cause_t c) {
    switch (c) {
        case FAIL_TOOL_ERROR:   return "tool_error";
        case FAIL_STEP_LIMIT:   return "step_limit";
        case FAIL_NULL_RESULT:  return "null_result";
        case FAIL_CYCLING:      return "cycling";
        case FAIL_EMPTY_RESULT: return "empty_result";
    }
    return "unknown";
}

static const char *mechanism_str(fail_mechanism_t m) {
    switch (m) {
        case FMECH_FILE_EDIT_MISMATCH: return "file_edit_mismatch";
        case FMECH_UNREAD_REF:         return "unread_ref";
        case FMECH_SHELL_RETRY:        return "shell_retry";
        case FMECH_CONTEXT_EVICTION:   return "context_eviction";
        case FMECH_WRONG_TOOL:         return "wrong_tool";
        case FMECH_HALLUCINATION:      return "hallucination";
        case FMECH_SPEC_VIOLATION:     return "spec_violation";
        case FMECH_OTHER:              return "other";
    }
    return "unknown";
}

static fail_mechanism_t classify_mechanism(const char *tool, const char *error,
                                           cJSON *params) {
    if (!tool) return FMECH_OTHER;

    /* Tool name concatenation: LLM emitted two tool names joined together
     * e.g. "web_fetchweb_search", "memory_recallshell_exec" */
    if (strcmp(tool, "unknown_tool") == 0 && error) {
        if (strstr(error, "unknown tool:"))
            return FMECH_WRONG_TOOL;
    }

    /* file_edit with wrong old_text */
    if (strcmp(tool, "file_edit") == 0) {
        if (error && (strstr(error, "not found") || strstr(error, "does not match") ||
                      strstr(error, "old_text")))
            return FMECH_FILE_EDIT_MISMATCH;
    }

    /* shell_exec — detect retries of the same command */
    if (strcmp(tool, "shell_exec") == 0) {
        /* This is a heuristic — full retry detection requires comparing
         * with previous steps, handled during journal traversal */
        if (error && (strstr(error, "timed out") || strstr(error, "timeout")))
            return FMECH_SHELL_RETRY;
    }

    /* Invalid tool parameters */
    if (error && (strstr(error, "missing") || strstr(error, "required") ||
                  strstr(error, "parameter")))
        return FMECH_SPEC_VIOLATION;

    (void)params;  /* used for future heuristics */
    return FMECH_OTHER;
}

/* ── Session scanning ────────────────────────────────── */

typedef struct {
    failure_instance_t *failures;
    int n_failures;
    int cap;
} failure_list_t;

static void add_failure(failure_list_t *list, failure_instance_t fi) {
    if (list->n_failures >= list->cap) {
        list->cap = list->cap ? list->cap * 2 : 32;
        list->failures = realloc(list->failures,
                                  list->cap * sizeof(failure_instance_t));
    }
    list->failures[list->n_failures++] = fi;
}

static void scan_session(const char *session_dir, failure_list_t *out) {
    char jpath[NASH_PATH_MAX];
    snprintf(jpath, sizeof(jpath), "%s/journal.jsonl", session_dir);

    char *data = slurp_file(jpath, NULL);
    if (!data) return;

    /* Track per-react-loop state for failure detection */
    int max_loop = -1;
    int last_step = -1;
    char *prev_shell_cmd = NULL;
    int prev_had_ref_read = 1;  /* start as true (no pending read) */

    /* First pass: find max react_loop and detect failures */
    char *line = data;
    while (line && *line) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';

        cJSON *entry = cJSON_Parse(line);
        if (entry) {
            int rl = cJSON_GetObjectItem(entry, "react_loop")
                     ? cJSON_GetObjectItem(entry, "react_loop")->valueint : -1;
            int step = cJSON_GetObjectItem(entry, "step")
                       ? cJSON_GetObjectItem(entry, "step")->valueint : -1;
            const char *tool = cJSON_GetStringValue(cJSON_GetObjectItem(entry, "tool"));

            if (rl > max_loop) max_loop = rl;
            if (step > last_step) last_step = step;

            /* Skip system/query entries */
            if (tool && strcmp(tool, "system") != 0 && strcmp(tool, "query") != 0) {
                cJSON *failed = cJSON_GetObjectItem(entry, "failed");
                cJSON *err_node = cJSON_GetObjectItem(entry, "error");
                const char *err_str = err_node && cJSON_IsString(err_node)
                                      ? err_node->valuestring : NULL;
                int is_error = (failed && cJSON_IsTrue(failed)) ||
                               (err_str && err_str[0]);

                if (is_error) {
                    failure_instance_t fi = {0};
                    fi.session_dir = strdup(session_dir);
                    fi.react_loop = rl;
                    fi.step = step;
                    fi.cause = FAIL_TOOL_ERROR;
                    fi.tool = strdup(tool);
                    fi.error_msg = err_str ? strdup(err_str) : strdup("unknown error");
                    fi.mechanism = classify_mechanism(tool, err_str,
                                      cJSON_GetObjectItem(entry, "params"));

                    add_failure(out, fi);
                }

                /* Detect unread refs: if a tool produced a ref and the next
                 * tool is NOT file_read of that ref, it's a potential issue */
                const char *ref = cJSON_GetStringValue(cJSON_GetObjectItem(entry, "ref"));
                if (ref && ref[0]) {
                    prev_had_ref_read = 0;  /* pending read of this ref */
                } else if (strcmp(tool, "file_read") == 0 && !prev_had_ref_read) {
                    prev_had_ref_read = 1;  /* read occurred */
                }

                /* Detect shell retry: same command repeated */
                if (strcmp(tool, "shell_exec") == 0) {
                    cJSON *params = cJSON_GetObjectItem(entry, "params");
                    const char *cmd = cJSON_GetStringValue(
                        cJSON_GetObjectItem(params, "command"));
                    if (cmd && prev_shell_cmd && strcmp(cmd, prev_shell_cmd) == 0) {
                        failure_instance_t fi = {0};
                        fi.session_dir = strdup(session_dir);
                        fi.react_loop = rl;
                        fi.step = step;
                        fi.cause = FAIL_CYCLING;
                        fi.mechanism = FMECH_SHELL_RETRY;
                        fi.tool = strdup(tool);
                        fi.error_msg = strdup("repeated shell command");
                        add_failure(out, fi);
                    }
                    free(prev_shell_cmd);
                    prev_shell_cmd = cmd ? strdup(cmd) : NULL;
                }

                /* Detect done with empty result */
                if (strcmp(tool, "done") == 0) {
                    cJSON *params = cJSON_GetObjectItem(entry, "params");
                    const char *result = cJSON_GetStringValue(
                        cJSON_GetObjectItem(params, "result"));
                    if (!result || !result[0]) {
                        failure_instance_t fi = {0};
                        fi.session_dir = strdup(session_dir);
                        fi.react_loop = rl;
                        fi.step = step;
                        fi.cause = FAIL_EMPTY_RESULT;
                        fi.mechanism = FMECH_OTHER;
                        fi.tool = strdup(tool);
                        fi.error_msg = strdup("done() called with empty result");
                        add_failure(out, fi);
                    }
                }
            }

            cJSON_Delete(entry);
        }

        if (nl) line = nl + 1;
        else break;
    }

    free(prev_shell_cmd);

    /* Check for step limit exhaustion: if max react loop has many steps
     * and no "done" tool was called, likely hit the limit */
    /* This is a heuristic — the actual step limit check would require
     * comparing with config.max_react_steps */
    if (last_step >= 29) {  /* default max is 30, so step 29 = hit limit */
        failure_instance_t fi = {0};
        fi.session_dir = strdup(session_dir);
        fi.react_loop = max_loop;
        fi.step = last_step;
        fi.cause = FAIL_STEP_LIMIT;
        fi.mechanism = FMECH_OTHER;
        fi.tool = strdup("react_loop");
        fi.error_msg = strdup("likely hit step limit");
        add_failure(out, fi);
    }

    free(data);
}

/* ── Clustering ──────────────────────────────────────── */

static void cluster_failures(failure_list_t *list,
                              failure_cluster_t **out_clusters,
                              int *out_n) {
    /* Group by (cause, mechanism) pair */
    int max_clusters = 64;
    failure_cluster_t *clusters = calloc(max_clusters, sizeof(failure_cluster_t));
    int n_clusters = 0;

    /* Per-cluster session tracking — dynamic arrays freed after loop */
    char ***cluster_sessions = NULL;  /* cluster_sessions[i] = array of session_dir strings */
    int *cluster_session_caps = NULL;

    for (int i = 0; i < list->n_failures; i++) {
        failure_instance_t *fi = &list->failures[i];

        /* Find existing cluster */
        int found = -1;
        for (int j = 0; j < n_clusters; j++) {
            if (clusters[j].cause == fi->cause &&
                clusters[j].mechanism == fi->mechanism) {
                found = j;
                break;
            }
        }

        if (found < 0) {
            /* Create new cluster */
            if (n_clusters >= max_clusters) {
                max_clusters *= 2;
                clusters = realloc(clusters, max_clusters * sizeof(failure_cluster_t));
                memset(&clusters[n_clusters], 0,
                       (max_clusters - n_clusters) * sizeof(failure_cluster_t));
            }
            found = n_clusters++;
            clusters[found].cause = fi->cause;
            clusters[found].mechanism = fi->mechanism;
            clusters[found].tool = fi->tool ? strdup(fi->tool) : NULL;
            clusters[found].instances = calloc(5, sizeof(failure_instance_t));

            /* Grow session tracking arrays */
            cluster_sessions = realloc(cluster_sessions,
                                       n_clusters * sizeof(char **));
            cluster_session_caps = realloc(cluster_session_caps,
                                           n_clusters * sizeof(int));
            cluster_sessions[found] = NULL;
            cluster_session_caps[found] = 0;
        }

        clusters[found].count++;

        /* Track unique sessions: scan the full session list for this cluster */
        if (fi->session_dir) {
            int seen = 0;
            for (int k = 0; k < clusters[found].n_sessions; k++) {
                if (cluster_sessions[found][k] &&
                    strcmp(cluster_sessions[found][k], fi->session_dir) == 0) {
                    seen = 1;
                    break;
                }
            }
            if (!seen) {
                if (clusters[found].n_sessions >= cluster_session_caps[found]) {
                    cluster_session_caps[found] = cluster_session_caps[found]
                                                 ? cluster_session_caps[found] * 2 : 8;
                    cluster_sessions[found] = realloc(
                        cluster_sessions[found],
                        cluster_session_caps[found] * sizeof(char *));
                }
                cluster_sessions[found][clusters[found].n_sessions] =
                    strdup(fi->session_dir);
                clusters[found].n_sessions++;
            }
        }

        /* Keep up to 5 representative instances */
        if (clusters[found].n_instances < 5) {
            failure_instance_t *inst = &clusters[found].instances[clusters[found].n_instances++];
            inst->session_dir = fi->session_dir ? strdup(fi->session_dir) : NULL;
            inst->react_loop = fi->react_loop;
            inst->step = fi->step;
            inst->cause = fi->cause;
            inst->mechanism = fi->mechanism;
            inst->tool = fi->tool ? strdup(fi->tool) : NULL;
            inst->error_msg = fi->error_msg ? strdup(fi->error_msg) : NULL;
        }
    }

    /* Free session tracking arrays */
    for (int i = 0; i < n_clusters; i++) {
        for (int j = 0; j < clusters[i].n_sessions; j++)
            free(cluster_sessions[i][j]);
        free(cluster_sessions[i]);
    }
    free(cluster_sessions);
    free(cluster_session_caps);

    /* Generate summaries */
    for (int i = 0; i < n_clusters; i++) {
        char buf[512];
        snprintf(buf, sizeof(buf), "%s/%s via %s (%d occurrences across %d session%s)",
                 cause_str(clusters[i].cause),
                 mechanism_str(clusters[i].mechanism),
                 clusters[i].tool ? clusters[i].tool : "unknown",
                 clusters[i].count,
                 clusters[i].n_sessions,
                 clusters[i].n_sessions == 1 ? "" : "s");
        clusters[i].summary = strdup(buf);
    }

    /* Sort by count (descending) */
    for (int i = 0; i < n_clusters - 1; i++) {
        for (int j = i + 1; j < n_clusters; j++) {
            if (clusters[j].count > clusters[i].count) {
                failure_cluster_t tmp = clusters[i];
                clusters[i] = clusters[j];
                clusters[j] = tmp;
            }
        }
    }

    *out_clusters = clusters;
    *out_n = n_clusters;
}

/* ── Public API ──────────────────────────────────────── */

postmortem_report_t *postmortem_analyze(const char *nash_dir, int max_sessions) {
    postmortem_report_t *report = calloc(1, sizeof(postmortem_report_t));

    char sessions_dir[NASH_PATH_MAX];
    snprintf(sessions_dir, sizeof(sessions_dir), "%s/sessions", nash_dir);

    DIR *d = opendir(sessions_dir);
    if (!d) return report;

    /* Collect session dirs, sorted by name (timestamp) descending */
    int dirs_cap = 64;
    char **dirs = calloc(dirs_cap, sizeof(char *));
    int n_dirs = 0;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        /* Skip names that would overflow the path buffer */
        size_t name_len = strlen(ent->d_name);
        if (strlen(sessions_dir) + name_len + 20 >= NASH_PATH_MAX)
            continue;

        /* Build paths — overflow guarded above */
        char path[NASH_PATH_MAX + 256];
        snprintf(path, sizeof(path), "%s/%s/journal.jsonl", sessions_dir, ent->d_name);

        /* Only include directories that have a journal */
        struct stat st;
        if (stat(path, &st) != 0) continue;

        if (n_dirs >= dirs_cap) {
            dirs_cap *= 2;
            dirs = realloc(dirs, dirs_cap * sizeof(char *));
        }

        snprintf(path, sizeof(path), "%s/%s", sessions_dir, ent->d_name);
        dirs[n_dirs++] = strdup(path);
    }
    closedir(d);

    /* Sort descending by name (newest first) */
    for (int i = 0; i < n_dirs - 1; i++) {
        for (int j = i + 1; j < n_dirs; j++) {
            if (strcmp(dirs[j], dirs[i]) > 0) {
                char *tmp = dirs[i];
                dirs[i] = dirs[j];
                dirs[j] = tmp;
            }
        }
    }

    /* Limit sessions to scan */
    int scan_count = n_dirs;
    if (max_sessions > 0 && max_sessions < scan_count)
        scan_count = max_sessions;

    /* Scan sessions */
    failure_list_t failures = {0};
    for (int i = 0; i < scan_count; i++) {
        scan_session(dirs[i], &failures);
        report->total_sessions++;
    }

    report->total_failures = failures.n_failures;

    /* Cluster failures */
    cluster_failures(&failures, &report->clusters, &report->n_clusters);

    /* Build evidence bundle (formatted text for LLM consumption) */
    str_t bundle = str_new(4096);
    str_appendf(&bundle, "# Failure Pattern Analysis\n\n");
    str_appendf(&bundle, "Sessions analyzed: %d\n", report->total_sessions);
    str_appendf(&bundle, "Total failures found: %d\n", report->total_failures);
    str_appendf(&bundle, "Distinct failure patterns: %d\n\n", report->n_clusters);

    for (int i = 0; i < report->n_clusters; i++) {
        failure_cluster_t *c = &report->clusters[i];
        str_appendf(&bundle, "## Pattern %d: %s\n", i + 1, c->summary);
        str_appendf(&bundle, "Cause: %s\n", cause_str(c->cause));
        str_appendf(&bundle, "Mechanism: %s\n", mechanism_str(c->mechanism));
        str_appendf(&bundle, "Tool: %s\n", c->tool ? c->tool : "N/A");
        str_appendf(&bundle, "Count: %d (across %d session%s)\n\n",
                    c->count, c->n_sessions,
                    c->n_sessions == 1 ? "" : "s");

        str_appendf(&bundle, "Representative instances:\n");
        for (int j = 0; j < c->n_instances; j++) {
            failure_instance_t *fi = &c->instances[j];
            str_appendf(&bundle, "  - session: %s, loop: %d, step: %d\n",
                        fi->session_dir ? fi->session_dir : "?",
                        fi->react_loop, fi->step);
            if (fi->error_msg)
                str_appendf(&bundle, "    error: %s\n", fi->error_msg);
        }
        str_appendf(&bundle, "\n");
    }

    report->evidence_bundle = str_steal(&bundle);

    /* Free temporary data */
    for (int i = 0; i < failures.n_failures; i++) {
        free(failures.failures[i].session_dir);
        free(failures.failures[i].tool);
        free(failures.failures[i].error_msg);
        free(failures.failures[i].context);
    }
    free(failures.failures);

    for (int i = 0; i < n_dirs; i++)
        free(dirs[i]);
    free(dirs);

    return report;
}

void postmortem_print(const postmortem_report_t *report) {
    fprintf(stderr, "\n╔══════════════════════════════════════════╗\n");
    fprintf(stderr, "║     Nash Failure Pattern Analysis        ║\n");
    fprintf(stderr, "╚══════════════════════════════════════════╝\n\n");

    fprintf(stderr, "  Sessions analyzed: %d\n", report->total_sessions);
    fprintf(stderr, "  Total failures: %d\n", report->total_failures);
    fprintf(stderr, "  Failure patterns: %d\n\n", report->n_clusters);

    for (int i = 0; i < report->n_clusters; i++) {
        failure_cluster_t *c = &report->clusters[i];
        fprintf(stderr, "  %d. %s\n", i + 1, c->summary);

        for (int j = 0; j < c->n_instances && j < 3; j++) {
            failure_instance_t *fi = &c->instances[j];
            fprintf(stderr, "     └ %s loop:%d step:%d",
                    fi->session_dir ? fi->session_dir : "?",
                    fi->react_loop, fi->step);
            if (fi->error_msg)
                fprintf(stderr, " — %s", fi->error_msg);
            fprintf(stderr, "\n");
        }

        if (c->n_instances > 3)
            fprintf(stderr, "     └ ... and %d more\n", c->n_instances - 3);

        fprintf(stderr, "\n");
    }

    if (report->n_clusters == 0)
        fprintf(stderr, "  No failure patterns detected.\n\n");
}

int postmortem_save(const postmortem_report_t *report, const char *path) {
    if (!report->evidence_bundle) return -1;
    return write_file(path, report->evidence_bundle,
                      strlen(report->evidence_bundle));
}

void postmortem_free(postmortem_report_t *report) {
    if (!report) return;

    for (int i = 0; i < report->n_clusters; i++) {
        failure_cluster_t *c = &report->clusters[i];
        free(c->tool);
        free(c->summary);
        for (int j = 0; j < c->n_instances; j++) {
            free(c->instances[j].session_dir);
            free(c->instances[j].tool);
            free(c->instances[j].error_msg);
            free(c->instances[j].context);
        }
        free(c->instances);
    }
    free(report->clusters);
    free(report->evidence_bundle);
    free(report);
}
