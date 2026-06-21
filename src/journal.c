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
        if (!inner || !cJSON_IsString(inner) || !inner->valuestring) {
            /* No thought field or not a string — the input is a JSON object
             * (e.g. a full action echo) with no extractable thought. */
            cJSON_Delete(nested);
            free(current);
            return NULL;
        }
        if (inner->valuestring[0] == '\0') {
            /* Nested thought is empty — the model echoed the full action
             * JSON as the thought field but left thought="".  Return NULL
             * so the display layer treats this as "no thought". */
            cJSON_Delete(nested);
            free(current);
            return NULL;
        }
        char *next = strdup(inner->valuestring);
        cJSON_Delete(nested);
        free(current);
        if (next[0] != '{') return next;  /* fully unwrapped — plain text */
        current = next;  /* still JSON, continue unwrapping */
    }

    /* If we exhausted depth limit, the value is still JSON.
     * Return NULL since it's not meaningful thought text. */
    free(current);
    return NULL;
}

journal_t *journal_new(const char *session_dir) {
    journal_t *j = calloc(1, sizeof(*j));
    if (!j) return NULL;
    pthread_mutex_init(&j->mtx, NULL);  /* FIX CRIT2: thread-safe journal */
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
    pthread_mutex_init(&j->mtx, NULL);  /* FIX CRIT2: thread-safe journal */
    j->nash_dir = strdup(nash_dir);
    j->lazy_created = 0;
    return j;
}

void journal_free(journal_t *j) {
    if (!j) return;
    pthread_mutex_destroy(&j->mtx);  /* FIX CRIT2 */
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
    /* FIX CRIT2: mutex protects all journal state (lazy_created, path, file I/O)
     * against concurrent calls from inference thread and nash_log(). */
    pthread_mutex_lock(&j->mtx);

    /* Lazy session creation: create directory on first write */
    if (j->nash_dir && !j->lazy_created) {
        journal_create_lazy_session(j);
    }
    if (!j->path) { pthread_mutex_unlock(&j->mtx); return -1; }
    FILE *f = fopen(j->path, "a");
    if (!f) { pthread_mutex_unlock(&j->mtx); return -1; }

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
    pthread_mutex_unlock(&j->mtx);  /* FIX CRIT2 */
    return 0;
}

/* ── B1 FIX: Shared compaction parameter extraction ──── */

void journal_parse_compaction_stats(cJSON *params, journal_compaction_stats_t *s) {
    s->before_msgs = 0;
    s->after_msgs = 0;
    s->before_pct = 0;
    s->after_pct = 0;
    if (!params) return;
    /* S3 FIX: Macro-driven extraction eliminates 4× repeated pattern. */
    #define PARSE_INT_FIELD(name) do { \
        cJSON *_j = cJSON_GetObjectItem(params, #name); \
        if (_j) s->name = (int)_j->valuedouble; \
    } while (0)
    PARSE_INT_FIELD(before_msgs);
    PARSE_INT_FIELD(after_msgs);
    PARSE_INT_FIELD(before_pct);
    PARSE_INT_FIELD(after_pct);
    #undef PARSE_INT_FIELD
}

/* ── B3 FIX: Shared structural tool skip list ────────── */

int journal_is_structural_tool(const char *tool) {
    if (!tool) return 0;
    return strcmp(tool, "system") == 0 ||
           strcmp(tool, "query") == 0 ||
           strcmp(tool, "context") == 0 ||
           strcmp(tool, "spec") == 0 ||
           strcmp(tool, "memory_context") == 0 ||
           strcmp(tool, "log") == 0 ||
           strcmp(tool, "compaction") == 0;
}

/* DUP1 FIX: Extract shared evicted-count flush logic.
 * Appends an eviction summary line to `out` and resets counters. */
static void journal_flush_evicted(str_t *out, int *evicted_count,
                                  int *evicted_compact_step,
                                  const journal_compaction_stats_t *cs) {
    if (*evicted_count <= 0) return;
    if (*evicted_compact_step >= 0)
        str_appendf(out,
            "    ... [%d steps compacted at step %d: %d\xe2\x86\x92%d msgs, %d%%\xe2\x86\x92%d%% usage]\n",
            *evicted_count, *evicted_compact_step,
            cs->before_msgs, cs->after_msgs,
            cs->before_pct, cs->after_pct);
    else
        str_appendf(out, "    ... [%d earlier steps evicted from context]\n",
                    *evicted_count);
    *evicted_count = 0;
    *evicted_compact_step = -1;
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
    /* FIX CRIT2: mutex protects j->path from concurrent lazy creation */
    pthread_mutex_lock(&j->mtx);
    if (!j->path) {
        pthread_mutex_unlock(&j->mtx);
        return strdup("Session history: (empty — new session)");
    }
    FILE *f = fopen(j->path, "r");
    pthread_mutex_unlock(&j->mtx);  /* path is stable after lazy init */
    if (!f) return strdup("Session history: (empty — new session)");
    flock(fileno(f), LOCK_SH);

    str_t out = str_new(2048);
    str_append_cstr(&out, "Session history:\n");

    char line[NASH_LINE_MAX];
    int count = 0;
    int current_loop = -1;
    int evicted_count = 0;  /* count of evicted steps in target_loop */
    int evicted_compact_step = -1;  /* step of last compaction in evicted range */
    journal_compaction_stats_t evicted_cs = {0}; /* B1 FIX */

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
            /* B2 FIX: Flush evicted count from previous loop */
            journal_flush_evicted(&out, &evicted_count,
                                  &evicted_compact_step, &evicted_cs);
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

        /* FIX D8: For steps in the target loop that are below min_step,
         * just count them — they were evicted from context */
        if (loop == target_loop && step > 0 && step < min_step) {
            /* B1 FIX: Track compaction events within evicted range */
            if (tool && strcmp(tool, "compaction") == 0 && params) {
                evicted_compact_step = step;
                journal_parse_compaction_stats(params, &evicted_cs);
            }
            evicted_count++;
            cJSON_Delete(entry);
            count++;
            continue;
        }

        /* B1 FIX: Render compaction entries as a compact separator line */
        if (tool && strcmp(tool, "compaction") == 0 && params) {
            journal_compaction_stats_t cs;
            journal_parse_compaction_stats(params, &cs);
            str_appendf(&out,
                "    \xe2\x9c\x82 context compacted at step %d: %d\xe2\x86\x92%d msgs, %d%%\xe2\x86\x92%d%%\n",
                step, cs.before_msgs, cs.after_msgs, cs.before_pct, cs.after_pct);
            cJSON_Delete(entry);
            count++;
            continue;
        }

        /* B3 FIX: Skip structural entries (shown in header / redundant) */
        if (journal_is_structural_tool(tool)) {
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
                if (unwrapped2)
                    thought = unwrapped2;
                else if (th->valuestring[0] != '{')
                    thought = th->valuestring; /* plain text, use as-is */
                /* else: JSON with no extractable thought — skip */
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
    journal_flush_evicted(&out, &evicted_count,
                          &evicted_compact_step, &evicted_cs);

    fclose(f);
    return str_steal(&out);
}

/* ── Chunk extraction for session-level RAG ─────────── */

void journal_chunks_free(journal_chunks_t *jc) {
    if (!jc) return;
    for (int i = 0; i < jc->n_chunks; i++)
        free(jc->texts[i]);
    free(jc->texts);
    jc->texts = NULL;
    jc->n_chunks = 0;
}

/* Read first N bytes of a file, return heap string (NULL on error) */
static char *read_file_head(const char *path, int max_bytes) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    char *buf = malloc((size_t)max_bytes + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, (size_t)max_bytes, f);
    fclose(f);
    buf[n] = '\0';
    /* Clamp to valid UTF-8 boundary */
    while (n > 0 && ((unsigned char)buf[n] & 0xC0) == 0x80) n--;
    buf[n] = '\0';
    return buf;
}

journal_chunks_t journal_extract_chunks(const char *session_dir,
                                        int max_chars_per_chunk,
                                        int max_chunks) {
    journal_chunks_t result = {0};
    if (!session_dir) return result;
    if (max_chars_per_chunk <= 0) max_chars_per_chunk = 900;
    if (max_chunks <= 0) max_chunks = 50;

    char jpath[NASH_PATH_MAX];
    snprintf(jpath, sizeof(jpath), "%s/journal.jsonl", session_dir);

    FILE *f = fopen(jpath, "r");
    if (!f) return result;
    flock(fileno(f), LOCK_SH);

    /* Pass 1: extract per-step semantic text segments */
    typedef struct { char *text; } seg_t;
    int seg_count = 0, seg_cap = 64;
    seg_t *segs = calloc((size_t)seg_cap, sizeof(seg_t));
    if (!segs) { fclose(f); return result; }

    /* Track query text for chunk prefixes */
    char query_text[201] = {0};

    char line[NASH_LINE_MAX];
    while (fgets(line, sizeof(line), f)) {
        cJSON *entry = cJSON_Parse(line);
        if (!entry) continue;

        const char *tool = cJSON_GetStringValue(
            cJSON_GetObjectItem(entry, "tool"));
        cJSON *params = cJSON_GetObjectItem(entry, "params");
        const char *ref = cJSON_GetStringValue(
            cJSON_GetObjectItem(entry, "ref"));

        if (!tool) { cJSON_Delete(entry); continue; }

        /* Capture query text for chunk context prefix */
        if (strcmp(tool, "query") == 0 && params && !query_text[0]) {
            cJSON *qt = cJSON_GetObjectItem(params, "text");
            if (qt && qt->valuestring) {
                utf8_truncate(query_text, qt->valuestring, 200);
            }
            cJSON_Delete(entry);
            continue;
        }

        /* B3 FIX: Skip structural entries — no semantic value for RAG */
        if (journal_is_structural_tool(tool)) {
            cJSON_Delete(entry);
            continue;
        }

        str_t seg = str_new(512);

        /* Extract thought — highest semantic value */
        if (params) {
            cJSON *th = cJSON_GetObjectItem(params, "thought");
            if (th && th->valuestring && th->valuestring[0]) {
                char *unwrapped = unwrap_thought(th->valuestring);
                const char *thought = unwrapped ? unwrapped
                    : (th->valuestring[0] != '{' ? th->valuestring : NULL);
                if (thought && strlen(thought) > 5) {
                    char trunc[401];
                    utf8_truncate(trunc, thought, 400);
                    str_appendf(&seg, "%s\n", trunc);
                }
                free(unwrapped);
            }
        }

        /* Tool-specific content extraction */
        if (strcmp(tool, "done") == 0 && params) {
            cJSON *res = cJSON_GetObjectItem(params, "result");
            if (res && res->valuestring && strlen(res->valuestring) > 5) {
                char trunc[501];
                utf8_truncate(trunc, res->valuestring, 500);
                str_appendf(&seg, "Result: %s\n", trunc);
            }
        } else if (strcmp(tool, "memory_store") == 0 && params) {
            cJSON *key_j = cJSON_GetObjectItem(params, "key");
            cJSON *val_j = cJSON_GetObjectItem(params, "value");
            if (key_j && key_j->valuestring)
                str_appendf(&seg, "Stored: %s\n", key_j->valuestring);
            if (val_j && val_j->valuestring) {
                char trunc[301];
                utf8_truncate(trunc, val_j->valuestring, 300);
                str_appendf(&seg, "%s\n", trunc);
            }
        } else if (strcmp(tool, "memory_recall") == 0 && params) {
            cJSON *q = cJSON_GetObjectItem(params, "query");
            if (q && q->valuestring)
                str_appendf(&seg, "Recalled: %s\n", q->valuestring);
        } else if ((strcmp(tool, "file_read") == 0 ||
                    strcmp(tool, "grep_search") == 0 ||
                    strcmp(tool, "shell_exec") == 0 ||
                    strcmp(tool, "file_edit") == 0 ||
                    strcmp(tool, "web_fetch") == 0) && ref) {
            /* Read first N chars of referenced content */
            char ref_path[NASH_PATH_MAX];
            snprintf(ref_path, sizeof(ref_path), "%s/%s", session_dir, ref);
            int content_limit = (strcmp(tool, "shell_exec") == 0) ? 300 : 400;
            char *content = read_file_head(ref_path, content_limit);
            if (content && strlen(content) > 10) {
                /* Add tool context */
                if (params) {
                    cJSON *p = cJSON_GetObjectItem(params, "path");
                    cJSON *cmd = cJSON_GetObjectItem(params, "command");
                    cJSON *pat = cJSON_GetObjectItem(params, "pattern");
                    if (p && p->valuestring)
                        str_appendf(&seg, "%s %s: ", tool, p->valuestring);
                    else if (cmd && cmd->valuestring) {
                        char ct[101];
                        utf8_truncate(ct, cmd->valuestring, 100);
                        str_appendf(&seg, "shell: %s\n", ct);
                    } else if (pat && pat->valuestring)
                        str_appendf(&seg, "%s '%s': ", tool, pat->valuestring);
                    else
                        str_appendf(&seg, "%s: ", tool);
                }
                str_appendf(&seg, "%s\n", content);
            }
            free(content);
        }

        /* Only keep segments with meaningful content */
        if (seg.len > 15) {
            if (seg_count >= seg_cap) {
                seg_cap *= 2;
                seg_t *new_segs = realloc(segs, (size_t)seg_cap * sizeof(seg_t));
                if (!new_segs) { str_free(&seg); break; }
                segs = new_segs;
            }
            segs[seg_count].text = str_steal(&seg);
            seg_count++;
        } else {
            str_free(&seg);
        }

        cJSON_Delete(entry);
    }
    fclose(f);

    if (seg_count == 0) {
        free(segs);
        return result;
    }

    /* Pass 2: Group segments into chunks of ~max_chars_per_chunk */
    /* Build a session context prefix */
    char prefix[256];
    {
        const char *base = strrchr(session_dir, '/');
        if (base) base++; else base = session_dir;
        double ts = atof(base);
        time_t ts_t = (time_t)ts;
        struct tm *tm = localtime(&ts_t);
        char date_buf[32];
        strftime(date_buf, sizeof(date_buf), "%Y-%m-%d", tm);
        if (query_text[0])
            snprintf(prefix, sizeof(prefix), "Session %s | Query: %s\n",
                     date_buf, query_text);
        else
            snprintf(prefix, sizeof(prefix), "Session %s\n", date_buf);
    }
    int prefix_len = (int)strlen(prefix);
    int content_budget = max_chars_per_chunk - prefix_len;
    if (content_budget < 200) content_budget = 200;

    /* Allocate chunks array (upper bound = seg_count) */
    int chunk_cap = seg_count < max_chunks ? seg_count : max_chunks;
    result.texts = calloc((size_t)chunk_cap + 1, sizeof(char *));
    if (!result.texts) {
        for (int i = 0; i < seg_count; i++) free(segs[i].text);
        free(segs);
        return result;
    }

    str_t cur_chunk = str_new((size_t)max_chars_per_chunk + 128);
    str_append_cstr(&cur_chunk, prefix);
    int chunk_content_len = 0;

    for (int i = 0; i < seg_count; i++) {
        int seg_len = (int)strlen(segs[i].text);

        /* Would adding this segment exceed budget? Start new chunk. */
        if (chunk_content_len > 0 &&
            chunk_content_len + seg_len > content_budget) {
            /* Save current chunk */
            if (result.n_chunks < chunk_cap) {
                result.texts[result.n_chunks] = str_steal(&cur_chunk);
                result.n_chunks++;
            } else {
                str_free(&cur_chunk);
            }
            /* Start new chunk with prefix */
            cur_chunk = str_new((size_t)max_chars_per_chunk + 128);
            str_append_cstr(&cur_chunk, prefix);
            chunk_content_len = 0;

            if (result.n_chunks >= max_chunks) {
                /* Hit chunk cap — discard remaining segments */
                for (int j = i; j < seg_count; j++) free(segs[j].text);
                break;
            }
        }

        /* Append segment to current chunk */
        if (seg_len > content_budget) {
            /* Single oversized segment: truncate (UTF-8 safe) */
            size_t safe_len = utf8_clamp(segs[i].text, (size_t)content_budget);
            str_append(&cur_chunk, segs[i].text, safe_len);
            chunk_content_len += content_budget;
        } else {
            str_append_cstr(&cur_chunk, segs[i].text);
            chunk_content_len += seg_len;
        }
        free(segs[i].text);
        segs[i].text = NULL;
    }

    /* Save final chunk if it has content */
    if (chunk_content_len > 0 && result.n_chunks < max_chunks) {
        result.texts[result.n_chunks] = str_steal(&cur_chunk);
        result.n_chunks++;
    } else {
        str_free(&cur_chunk);
    }

    free(segs);
    return result;
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
