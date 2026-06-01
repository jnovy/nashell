#include "journal.h"
#include "nash_limits.h"
#include "cJSON.h"
#include "str.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <sys/file.h>  /* flock */

/* Recursively unwrap nested JSON in a thought string.
 * The LLM sometimes echoes its own previous response as a thought,
 * producing double- or triple-nested JSON like:
 *   {"thought":"{\"thought\":\"{\"thought\":\"deep\"}\"}"}
 * This function keeps unwrapping while the result is still JSON with a "thought" key.
 * Returns a heap-allocated clean thought, or NULL if no unwrapping was needed.
 * Caller must free() the result. */
char *unwrap_thought(const char *thought) {
    if (!thought || thought[0] != '{') return NULL;  /* nothing to unwrap */

    char *current = strdup(thought);
    if (!current) return NULL;

    for (int depth = 0; depth < 5; depth++) {
        cJSON *nested = cJSON_Parse(current);
        if (!nested) break;
        cJSON *inner = cJSON_GetObjectItemCaseSensitive(nested, "thought");
        if (!inner || !cJSON_IsString(inner) || !inner->valuestring || inner->valuestring[0] == '\0') {
            cJSON_Delete(nested);
            break;
        }
        char *next = strdup(inner->valuestring);
        cJSON_Delete(nested);
        free(current);
        if (next[0] != '{') return next;  /* fully unwrapped — plain text */
        current = next;  /* still JSON, continue unwrapping */
    }

    /* If we exhausted depth limit or broke out, return the last unwrapped value
     * (it may still be JSON, but we can't unwrap further). */
    return current;
}

journal_t *journal_new(const char *session_dir) {
    journal_t *j = calloc(1, sizeof(*j));
    if (!j) return NULL;
    j->session_dir = strdup(session_dir);
    if (!j->session_dir) { free(j); return NULL; }
    char path[NASH_PATH_MAX];
    snprintf(path, sizeof(path), "%s/journal.jsonl", session_dir);
    j->path = strdup(path);
    j->lazy_created = 1;
    if (!j->path) { free(j->session_dir); free(j); return NULL; }
    return j;
}

/* Lazy journal: session directory is not created until first journal_append().
 * If program exits without any append, no session directory exists. */
journal_t *journal_new_lazy(const char *nash_dir) {
    journal_t *j = calloc(1, sizeof(*j));
    if (!j) return NULL;
    j->nash_dir = strdup(nash_dir);
    j->lazy_created = 0;
    return j;
}

void journal_free(journal_t *j) {
    if (!j) return;
    free(j->path);
    free(j->session_dir);
    free(j->nash_dir);
    free(j);
}

const char *journal_session_dir(journal_t *j) {
    if (!j) return NULL;
    return j->session_dir;
}

/* Create the session directory lazily. Called from journal_append on first write. */
static int journal_create_lazy_session(journal_t *j) {
    if (!j || !j->nash_dir || j->lazy_created) return 0;

    struct timespec tp;
    clock_gettime(CLOCK_REALTIME, &tp);

    char sessions_base[1024];
    snprintf(sessions_base, sizeof(sessions_base), "%s/sessions", j->nash_dir);
    mkdir(sessions_base, 0755);

    char path[1088];
    snprintf(path, sizeof(path), "%s/%ld.%05ld",
             sessions_base, (long)tp.tv_sec, tp.tv_nsec / 10000);
    mkdir(path, 0755);

    j->session_dir = strdup(path);
    char jpath[NASH_PATH_MAX];
    snprintf(jpath, sizeof(jpath), "%s/journal.jsonl", path);
    j->path = strdup(jpath);
    j->lazy_created = 1;
    return 0;
}

int journal_append(journal_t *j, int react_loop, int step, const char *tool,
                   cJSON *params, const char *ref,
                   size_t size, int lines, const char *error,
                   const char *tool_call_id) {
    /* Lazy session creation: create directory on first write */
    if (j->nash_dir && !j->lazy_created) {
        journal_create_lazy_session(j);
    }
    if (!j->path) return -1;
    FILE *f = fopen(j->path, "a");
    if (!f) return -1;

    /* Exclusive lock for writes — prevents torn reads from TUI thread */
    flock(fileno(f), LOCK_EX);

    /* Unix epoch timestamp with microsecond precision */
    struct timespec tp;
    clock_gettime(CLOCK_REALTIME, &tp);
    char ts[32];
    snprintf(ts, sizeof(ts), "%ld.%05ld", (long)tp.tv_sec, tp.tv_nsec / 10000);

    cJSON *entry = cJSON_CreateObject();
    cJSON_AddNumberToObject(entry, "react_loop", react_loop);
    cJSON_AddNumberToObject(entry, "step", step);
    cJSON_AddStringToObject(entry, "ts", ts);
    cJSON_AddStringToObject(entry, "tool", tool);
    if (params) cJSON_AddItemToObject(entry, "params", cJSON_Duplicate(params, 1));
    if (ref) cJSON_AddStringToObject(entry, "ref", ref);
    cJSON_AddNumberToObject(entry, "size", (double)size);
    cJSON_AddNumberToObject(entry, "lines", lines);
    cJSON_AddBoolToObject(entry, "failed", error != NULL);
    if (error) cJSON_AddStringToObject(entry, "error", error);
    if (tool_call_id) cJSON_AddStringToObject(entry, "tc_id", tool_call_id);

    char *json = cJSON_PrintUnformatted(entry);
    fprintf(f, "%s\n", json);
    free(json);
    cJSON_Delete(entry);
    fclose(f);
    return 0;
}

char *journal_manifest(journal_t *j, int max_steps) {
    /* Delegate to the filtered version with no eviction filtering.
     * target_loop=-1 ensures the eviction check never matches. */
    return journal_manifest_filtered(j, max_steps, -1, 0);
}

/* FIX D8: Build a manifest that collapses evicted steps into a summary.
 * Steps in the target react_loop with step < min_step are shown as a
 * single "[N earlier steps evicted from context]" line. This prevents
 * the model from trying to reference detailed step info that was evicted. */
char *journal_manifest_filtered(journal_t *j, int max_steps,
                                 int target_loop, int min_step) {
    FILE *f = fopen(j->path, "r");
    if (!f) return strdup("Session history: (empty — new session)");
    flock(fileno(f), LOCK_SH);

    str_t out = str_new(2048);
    str_append_cstr(&out, "Session history:\n");

    char line[NASH_LINE_MAX];
    int count = 0;
    int current_loop = -1;
    int evicted_count = 0;  /* count of evicted steps in target_loop */

    while (fgets(line, sizeof(line), f) && count < max_steps) {
        cJSON *entry = cJSON_Parse(line);
        if (!entry) continue;

        int loop = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(entry, "react_loop"));
        int step = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(entry, "step"));
        const char *tool = cJSON_GetStringValue(cJSON_GetObjectItem(entry, "tool"));
        const char *ref = cJSON_GetStringValue(cJSON_GetObjectItem(entry, "ref"));
        double sz = cJSON_GetNumberValue(cJSON_GetObjectItem(entry, "size"));
        cJSON *params = cJSON_GetObjectItem(entry, "params");

        /* New react loop — show header with query text */
        if (loop != current_loop) {
            /* Flush evicted count from previous loop */
            if (evicted_count > 0) {
                str_appendf(&out, "    ... [%d earlier steps evicted from context]\n",
                            evicted_count);
                evicted_count = 0;
            }
            current_loop = loop;
            if (loop > 0) str_append_cstr(&out, "\n");
            str_appendf(&out, "  [Query R%d]", loop);

            if (tool && strcmp(tool, "query") == 0 && params) {
                cJSON *text = cJSON_GetObjectItem(params, "text");
                if (text && text->valuestring) {
                    char truncated[101];
                    utf8_truncate(truncated, text->valuestring, 100);
                    str_appendf(&out, " \"%s\"", truncated);
                }
            }
            str_append_cstr(&out, "\n");
        }

        /* Skip system and query entries (already shown in header) */
        if (tool && (strcmp(tool, "system") == 0 || strcmp(tool, "query") == 0)) {
            cJSON_Delete(entry);
            count++;
            continue;
        }

        /* FIX D8: For steps in the target loop that are below min_step,
         * just count them — they were evicted from context */
        if (loop == target_loop && step > 0 && step < min_step) {
            evicted_count++;
            cJSON_Delete(entry);
            count++;
            continue;
        }

        /* Extract thought and key param for display */
        const char *key_param = "";
        const char *thought = NULL;
        char *unwrapped2 = NULL;
        if (params) {
            cJSON *th = cJSON_GetObjectItem(params, "thought");
            if (th && th->valuestring && th->valuestring[0]) {
                unwrapped2 = unwrap_thought(th->valuestring);
                thought = unwrapped2 ? unwrapped2 : th->valuestring;
            }
            cJSON *cmd = cJSON_GetObjectItem(params, "command");
            cJSON *p = cJSON_GetObjectItem(params, "path");
            cJSON *pat = cJSON_GetObjectItem(params, "pattern");
            cJSON *res = cJSON_GetObjectItem(params, "result");
            if (cmd && cmd->valuestring) key_param = cmd->valuestring;
            else if (p && p->valuestring) key_param = p->valuestring;
            else if (pat && pat->valuestring) key_param = pat->valuestring;
            else if (res && res->valuestring) key_param = res->valuestring;
        }

        /* Show thought above the step line (truncated for manifest) */
        if (thought) {
            char th_trunc[121];
            utf8_truncate(th_trunc, thought, 120);
            str_appendf(&out, "  \xf0\x9f\x92\xad %s\n", th_trunc);
        }

        char buf[512];
        cJSON *failed_j = cJSON_GetObjectItem(entry, "failed");
        int failed = (failed_j && cJSON_IsTrue(failed_j));
        const char *mark = failed ? "x" : "+";

        char kp[101];
        utf8_truncate(kp, key_param, 80);

        if (tool && strcmp(tool, "done") == 0) {
            char kp_done[101];
            utf8_truncate(kp_done, key_param, 100);
            snprintf(buf, sizeof(buf), "    %s %s: -> \"%s\"",
                     mark, ref ? ref : "?", kp_done);
        } else if (failed) {
            snprintf(buf, sizeof(buf), "    %s %s: %s \"%s\"",
                     mark, ref ? ref : "?", tool ? tool : "?", kp);
        } else {
            snprintf(buf, sizeof(buf), "    %s %s: %s \"%s\" -> %d chars",
                     mark, ref ? ref : "?", tool ? tool : "?", kp, (int)sz);
        }
        str_append_cstr(&out, buf);
        str_append_cstr(&out, "\n");

        free(unwrapped2);
        unwrapped2 = NULL;
        cJSON_Delete(entry);
        count++;
    }

    /* Flush final evicted count */
    if (evicted_count > 0) {
        str_appendf(&out, "    ... [%d earlier steps evicted from context]\n",
                    evicted_count);
    }

    fclose(f);
    return str_steal(&out);
}

/* Scan journal.jsonl and return the highest react_loop value found.
 * Returns -1 if the journal is empty or doesn't exist. */
int journal_max_react_loop(journal_t *j) {
    if (!j || !j->path) return -1;

    FILE *f = fopen(j->path, "r");
    if (!f) return -1;
    flock(fileno(f), LOCK_SH);  /* shared lock for reading */

    int max_loop = -1;
    char line[NASH_LINE_MAX];
    while (fgets(line, sizeof(line), f)) {
        cJSON *entry = cJSON_Parse(line);
        if (!entry) continue;
        int loop = (int)cJSON_GetNumberValue(
            cJSON_GetObjectItem(entry, "react_loop"));
        if (loop > max_loop) max_loop = loop;
        cJSON_Delete(entry);
    }
    fclose(f);
    return max_loop;
}
