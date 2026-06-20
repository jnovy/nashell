/* tool_session.c — session_grep: exact/substring search across past session
 * journals.  Complements memory_recall's semantic search with precise
 * pattern matching on function names, error codes, file paths, commands,
 * and other identifiers that embedding-based search handles poorly.
 *
 * Iterates sessions newest-first via the in-memory session_index (if
 * available) or by scanning the sessions directory directly.  For each
 * session, reads journal.jsonl line by line and applies case-insensitive
 * substring matching against the raw JSON text (which contains thoughts,
 * tool params, commands, file paths, etc.).
 *
 * Part of Nash unified memory architecture v4.1. */

#include "tools_internal.h"
#include "session_index.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>    /* strcasestr */
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#include <math.h>

/* Maximum line length we'll read from journal.jsonl */
#define JLINE_MAX 65536

/* Maximum context snippet length around a match.
 * Large enough to show full context — do NOT truncate session_grep lines. */
#define SNIPPET_MAX 4000

/* Default + absolute max for max_results */
#define DEFAULT_MAX_RESULTS 20
#define ABSOLUTE_MAX_RESULTS 100


/* Extract a readable context snippet around the match position.
 * Writes into buf (size buflen).  Returns buf. */
static char *extract_snippet(const char *line, const char *match_pos,
                              const char *pattern, char *buf, size_t buflen) {
    size_t patlen = strlen(pattern);
    size_t linelen = strlen(line);

    /* Determine a window around the match — use full line width */
    int before = 2000;
    int after  = 2000;

    const char *start = match_pos - before;
    if (start < line) start = line;
    const char *end = match_pos + patlen + after;
    if (end > line + linelen) end = line + linelen;

    /* Copy the window, trimming to buflen-1 */
    size_t span = (size_t)(end - start);
    if (span >= buflen - 4) span = buflen - 5;

    int off = 0;
    if (start > line) {
        buf[0] = '.'; buf[1] = '.'; buf[2] = '.';
        off = 3;
    }
    memcpy(buf + off, start, span);
    off += (int)span;
    if (end < line + linelen) {
        buf[off++] = '.';
        buf[off++] = '.';
        buf[off++] = '.';
    }
    buf[off] = '\0';

    /* Sanitize: replace control chars, raw JSON escapes */
    for (int i = 0; i < off; i++) {
        unsigned char c = (unsigned char)buf[i];
        if (c < 0x20 && c != ' ') buf[i] = ' ';
    }

    return buf;
}

/* Format a timestamp into "YYYY-MM-DD HH:MM" */
static void format_ts(double ts, char *buf, size_t buflen) {
    time_t t = (time_t)ts;
    struct tm tm_buf;
    struct tm *tm = gmtime_r(&t, &tm_buf);
    if (tm) {
        strftime(buf, buflen, "%Y-%m-%d %H:%M", tm);
    } else {
        snprintf(buf, buflen, "%.0f", ts);
    }
}

/* Parse tool and step info from a journal line (quick JSON extract).
 * Avoids full cJSON parse for speed — extracts key fields via strstr. */
static void parse_journal_fields(const char *line,
                                  int *react_loop, int *step,
                                  char *tool_buf, size_t tool_buflen) {
    *react_loop = -1;
    *step = -1;
    tool_buf[0] = '\0';

    /* "react_loop":N */
    const char *p = strstr(line, "\"react_loop\":");
    if (p) *react_loop = atoi(p + 13);

    /* "step":N */
    p = strstr(line, "\"step\":");
    if (p) *step = atoi(p + 6);

    /* "tool":"xxx" */
    p = strstr(line, "\"tool\":\"");
    if (p) {
        p += 8;
        const char *end = strchr(p, '"');
        if (end) {
            size_t len = (size_t)(end - p);
            if (len >= tool_buflen) len = tool_buflen - 1;
            memcpy(tool_buf, p, len);
            tool_buf[len] = '\0';
        }
    }
}

/* Compare scan_entry_t by timestamp for qsort (descending = newest first).
 * Must match the scan_entry_t typedef inside tool_session_grep(). */
typedef struct {
    char   dir_[NASH_PATH_MAX];
    double timestamp_;
} scan_entry_cmp_t;

static int cmp_scan_entry_desc(const void *a, const void *b) {
    const scan_entry_cmp_t *ea = a;
    const scan_entry_cmp_t *eb = b;
    if (ea->timestamp_ > eb->timestamp_) return -1;
    if (ea->timestamp_ < eb->timestamp_) return 1;
    return 0;
}

/* ── session_grep implementation ─────────────────────── */

tool_result_t tool_session_grep(tool_ctx_t *ctx, cJSON *params) {
    cJSON *pattern_j = cJSON_GetObjectItem(params, "pattern");
    if (!pattern_j || !pattern_j->valuestring || !pattern_j->valuestring[0])
        return tools_make_error("session_grep requires a non-empty 'pattern' string.");

    const char *pattern = pattern_j->valuestring;

    /* Optional max_results */
    int max_results = DEFAULT_MAX_RESULTS;
    cJSON *max_j = cJSON_GetObjectItem(params, "max_results");
    if (max_j && cJSON_IsNumber(max_j)) {
        max_results = max_j->valueint;
        if (max_results < 1) max_results = 1;
        if (max_results > ABSOLUTE_MAX_RESULTS) max_results = ABSOLUTE_MAX_RESULTS;
    }

    /* Optional days — limit search to sessions within N days */
    int days = 0;  /* 0 = no limit (search all) */
    double cutoff_ts = 0.0;
    cJSON *days_j = cJSON_GetObjectItem(params, "days");
    if (days_j && cJSON_IsNumber(days_j)) {
        days = days_j->valueint;
        if (days < 1) days = 1;
        cutoff_ts = (double)time(NULL) - (double)days * 86400.0;
    }

    /* Collect session directories to search (newest first).
     * Prefer the in-memory session_index (already sorted by timestamp desc). */
    typedef struct {
        char   dir[NASH_PATH_MAX];
        double timestamp;
    } scan_entry_t;

    scan_entry_t *sessions = NULL;
    int n_sessions = 0;

    if (ctx->session_idx) {
        pthread_mutex_lock(&ctx->session_idx->mtx);
        n_sessions = ctx->session_idx->count;
        sessions = calloc((size_t)n_sessions, sizeof(scan_entry_t));
        if (sessions) {
            for (int i = 0; i < n_sessions; i++) {
                snprintf(sessions[i].dir, NASH_PATH_MAX, "%s",
                         ctx->session_idx->entries[i].session_dir);
                sessions[i].timestamp = ctx->session_idx->entries[i].timestamp;
            }
        }
        pthread_mutex_unlock(&ctx->session_idx->mtx);
    }

    /* Fallback: scan sessions directory if no index */
    if (!sessions || n_sessions == 0) {
        /* Determine sessions directory from current session dir.
         * session_dir is like /home/user/.nash/sessions/1750000123.456
         * parent is the sessions/ directory. */
        if (!ctx->session_dir) {
            free(sessions);
            return tools_make_error("no session directory available for session_grep");
        }
        char sessions_dir[NASH_PATH_MAX];
        snprintf(sessions_dir, sizeof(sessions_dir), "%s", ctx->session_dir);
        char *last_slash = strrchr(sessions_dir, '/');
        if (last_slash) *last_slash = '\0';

        DIR *d = opendir(sessions_dir);
        if (!d) {
            free(sessions);
            return tools_make_error("cannot open sessions directory");
        }

        int cap = 256;
        sessions = calloc((size_t)cap, sizeof(scan_entry_t));
        n_sessions = 0;

        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            if (de->d_name[0] == '.') continue;
            if (n_sessions >= cap) {
                cap *= 2;
                scan_entry_t *new_s = realloc(sessions, (size_t)cap * sizeof(scan_entry_t));
                if (!new_s) break;
                sessions = new_s;
            }
            snprintf(sessions[n_sessions].dir, NASH_PATH_MAX, "%s/%s",
                     sessions_dir, de->d_name);
            sessions[n_sessions].timestamp = atof(de->d_name);
            n_sessions++;
        }
        closedir(d);

        /* Sort newest first */
        if (n_sessions > 1) {
            qsort(sessions, (size_t)n_sessions, sizeof(scan_entry_t),
                  cmp_scan_entry_desc);
        }
    }

    /* Search each session's journal.jsonl */
    str_t out = str_new(4096);
    int total_matches = 0;
    int sessions_matched = 0;

    char *line_buf = malloc(JLINE_MAX);
    if (!line_buf) {
        free(sessions);
        str_free(&out);
        return tools_make_error("out of memory");
    }

    int sessions_scanned = 0;  /* track how many we actually looked at (vs skipped by age) */

    for (int si = 0; si < n_sessions && total_matches < max_results; si++) {
        /* Age filter: sessions are sorted newest-first, so once we
         * hit one older than the cutoff, all remaining are older too. */
        if (days > 0 && sessions[si].timestamp < cutoff_ts)
            break;

        sessions_scanned++;

        char jpath[NASH_PATH_MAX];
        snprintf(jpath, sizeof(jpath), "%s/journal.jsonl", sessions[si].dir);

        FILE *f = fopen(jpath, "r");
        if (!f) continue;

        char ts_buf[32];
        format_ts(sessions[si].timestamp, ts_buf, sizeof(ts_buf));

        int session_printed = 0;

        while (fgets(line_buf, JLINE_MAX, f) && total_matches < max_results) {
            /* Case-insensitive substring search on the raw line */
            const char *match = strcasestr(line_buf, pattern);
            if (!match) continue;

            /* Skip structural noise (system, query, context, memory_context, spec) */
            int rl, step;
            char tool[64];
            parse_journal_fields(line_buf, &rl, &step, tool, sizeof(tool));

            /* B3 FIX: Use shared structural tool skip list */
            if (journal_is_structural_tool(tool))
                continue;

            /* Print session header on first match */
            if (!session_printed) {
                if (sessions_matched > 0) str_appendf(&out, "\n");
                str_appendf(&out, "=== Session %s ===\n", ts_buf);
                str_appendf(&out, "    %s\n", sessions[si].dir);
                session_printed = 1;
                sessions_matched++;
            }

            /* Extract snippet around match */
            char snippet[SNIPPET_MAX + 16];
            extract_snippet(line_buf, match, pattern, snippet, sizeof(snippet));

            str_appendf(&out, "  R%dS%d [%s]: %s\n", rl, step, tool, snippet);
            total_matches++;
        }

        fclose(f);
    }

    free(line_buf);
    free(sessions);

    /* Summary header */
    str_t result = str_new(out.len + 256);
    if (days > 0) {
        str_appendf(&result,
            "session_grep: %d match%s across %d session%s "
            "(searched %d sessions within %d day%s, pattern: \"%s\")\n\n",
            total_matches, total_matches == 1 ? "" : "es",
            sessions_matched, sessions_matched == 1 ? "" : "s",
            sessions_scanned, days, days == 1 ? "" : "s",
            pattern);
    } else {
        str_appendf(&result,
            "session_grep: %d match%s across %d session%s "
            "(searched %d sessions, pattern: \"%s\")\n\n",
            total_matches, total_matches == 1 ? "" : "es",
            sessions_matched, sessions_matched == 1 ? "" : "s",
            sessions_scanned,
            pattern);
    }

    if (total_matches > 0) {
        str_append(&result, out.data, out.len);
    } else {
        str_appendf(&result, "No matches found for \"%s\" in recent sessions.\n", pattern);
    }

    str_free(&out);

    /* Store result */
    char *hash = store_save(ctx->store, result.data);
    char *alias = tool_register_alias(ctx, hash ? hash : "");

    cJSON *meta = cJSON_CreateObject();
    cJSON_AddStringToObject(meta, "pattern", pattern);
    cJSON_AddNumberToObject(meta, "matches", total_matches);
    cJSON_AddNumberToObject(meta, "sessions_matched", sessions_matched);
    cJSON_AddNumberToObject(meta, "sessions_searched", sessions_scanned);
    if (days > 0)
        cJSON_AddNumberToObject(meta, "days", days);
    if (max_results != DEFAULT_MAX_RESULTS)
        cJSON_AddNumberToObject(meta, "max_results", max_results);
    cJSON_AddNumberToObject(meta, "chars", (double)result.len);
    cJSON_AddStringToObject(meta, "ref", alias);

    /* Include preview for small results */
    if (result.len > 0 && result.len < 500) {
        cJSON_AddStringToObject(meta, "preview", result.data);
    } else if (result.len >= 500) {
        char preview[256];
        size_t copy_len = 200;
        if (copy_len > result.len) copy_len = result.len;
        memcpy(preview, result.data, copy_len);
        preview[copy_len] = '\0';
        strcat(preview, "...");
        cJSON_AddStringToObject(meta, "preview", preview);
    }

    tools_inject_thought(ctx, params);
    journal_append(ctx->journal, ctx->react_loop, ctx->step, "session_grep",
                   params, alias, result.len, total_matches, NULL, NULL);

    char *ref_copy = strdup(alias);
    free(alias);
    free(hash);
    str_free(&result);
    return tools_make_result(1, meta, ref_copy);
}
