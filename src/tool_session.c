/* tool_session.c — session_search: unified episodic search across past session
 * journals.  Fuses semantic search (query via embeddings) with lexical/regex
 * search (pattern) into a single ranked result set with confidence tiers.
 *
 * v4.3: Subsumes session_grep — returns per-line match attribution
 * (R{loop}S{step} [tool]: snippet) for every lexical hit, not just one
 * "best" snippet per session.
 *
 * Part of Nash unified memory architecture v4.3. */

/* Suppress -Wformat-truncation for PATH_MAX path construction.
 * snprintf with sizeof(buf) handles truncation safely. */
#pragma GCC diagnostic ignored "-Wformat-truncation"

#include "tools_internal.h"
#include "session_search.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Default + absolute max for max_results */
#define DEFAULT_MAX_RESULTS 20
#define ABSOLUTE_MAX_RESULTS 100

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


/* ── session_search: unified episodic search (semantic + lexical fusion) ── */

tool_result_t tool_session_search(tool_ctx_t *ctx, cJSON *params) {
    /* Extract parameters */
    const char *query = NULL;
    cJSON *query_j = cJSON_GetObjectItem(params, "query");
    if (query_j && query_j->valuestring && query_j->valuestring[0])
        query = query_j->valuestring;

    const char *pattern = NULL;
    cJSON *pattern_j = cJSON_GetObjectItem(params, "pattern");
    if (pattern_j && pattern_j->valuestring && pattern_j->valuestring[0])
        pattern = pattern_j->valuestring;

    if (!query && !pattern)
        return tools_make_error(
            "session_search requires at least one of 'query' (semantic) "
            "or 'pattern' (lexical). Provide both for fused search.");

    int max_results = DEFAULT_MAX_RESULTS;
    cJSON *max_j = cJSON_GetObjectItem(params, "max_results");
    if (max_j && cJSON_IsNumber(max_j)) {
        max_results = max_j->valueint;
        if (max_results < 1) max_results = 1;
        if (max_results > ABSOLUTE_MAX_RESULTS) max_results = ABSOLUTE_MAX_RESULTS;
    }

    int days = 0;
    cJSON *days_j = cJSON_GetObjectItem(params, "days");
    if (days_j && cJSON_IsNumber(days_j)) {
        days = days_j->valueint;
        if (days < 1) days = 1;
    }

    int use_regex = 0;
    cJSON *regex_j = cJSON_GetObjectItem(params, "regex");
    if (regex_j && cJSON_IsTrue(regex_j))
        use_regex = 1;

    /* Derive sessions_dir from session_dir (parent directory) */
    char sessions_dir[NASH_PATH_MAX] = {0};
    if (ctx->session_dir) {
        snprintf(sessions_dir, sizeof(sessions_dir), "%s", ctx->session_dir);
        char *last_slash = strrchr(sessions_dir, '/');
        if (last_slash) *last_slash = '\0';
    }

    /* Get embedding context */
    embed_ctx_t *embed = NULL;
    if (ctx->memory && memory_has_embeddings(ctx->memory))
        embed = memory_embed_ctx(ctx->memory);

    /* Run unified search */
    ss_results_t results = session_search(
        ctx->session_idx,
        embed,
        query,
        pattern,
        use_regex,
        max_results,
        days,
        sessions_dir[0] ? sessions_dir : NULL
    );

    /* Determine mode for output formatting */
    int has_semantic = (query != NULL);
    int has_lexical  = (pattern != NULL);
    int is_pure_lexical = (has_lexical && !has_semantic);

    static const char *conf_labels[] = {"LOW", "MEDIUM", "HIGH"};
    str_t out = str_new(4096);

    /* ── Summary header ── */
    if (is_pure_lexical) {
        /* Pure lexical mode: match session_grep-style header with total match count */
        const char *mode_label = use_regex ? "regex" : "pattern";
        if (days > 0) {
            str_appendf(&out,
                "session_search: %d match%s across %d session%s "
                "(searched %d sessions within %d day%s, %s: \"%s\")\n\n",
                results.total_matches,
                results.total_matches == 1 ? "" : "es",
                results.count, results.count == 1 ? "" : "s",
                results.sessions_searched,
                days, days == 1 ? "" : "s",
                mode_label, pattern);
        } else {
            str_appendf(&out,
                "session_search: %d match%s across %d session%s "
                "(searched %d sessions, %s: \"%s\")\n\n",
                results.total_matches,
                results.total_matches == 1 ? "" : "es",
                results.count, results.count == 1 ? "" : "s",
                results.sessions_searched,
                mode_label, pattern);
        }
    } else {
        /* Semantic or fused mode: richer header */
        str_appendf(&out,
            "session_search: %d result%s (searched %d sessions",
            results.count, results.count == 1 ? "" : "s",
            results.sessions_searched);
        if (query) str_appendf(&out, ", query: \"%s\"", query);
        if (pattern) str_appendf(&out, ", pattern: \"%s\"", pattern);
        if (days > 0) str_appendf(&out, ", within %d day%s", days, days == 1 ? "" : "s");
        str_appendf(&out, ")\n");
    }

    /* ── Per-session results ── */
    for (int i = 0; i < results.count; i++) {
        ss_result_t *r = &results.results[i];

        char ts_buf[32];
        format_ts(r->timestamp, ts_buf, sizeof(ts_buf));

        if (is_pure_lexical) {
            /* Pure lexical: session_grep-style output (no scores) */
            if (i > 0) str_appendf(&out, "\n");
            str_appendf(&out, "=== Session %s ===\n", ts_buf);
            str_appendf(&out, "    %s\n", r->session_dir ? r->session_dir : "");

            /* Show all per-line matches */
            for (int j = 0; j < r->n_matches; j++) {
                ss_match_t *m = &r->matches[j];
                str_appendf(&out, "  R%dS%d [%s]: %s\n",
                            m->react_loop, m->step, m->tool,
                            m->snippet ? m->snippet : "");
            }
            if (r->match_count > r->n_matches) {
                str_appendf(&out, "  ... and %d more match%s\n",
                            r->match_count - r->n_matches,
                            (r->match_count - r->n_matches) == 1 ? "" : "es");
            }
        } else {
            /* Semantic or fused mode: scored output with confidence */
            str_appendf(&out, "\n─── #%d  %s  [%s]  composite=%.3f "
                        "(sem=%.3f lex=%.3f matches=%d) ───\n",
                        i + 1, ts_buf,
                        conf_labels[r->confidence],
                        r->composite_score,
                        r->semantic_score, r->lexical_score,
                        r->match_count);
            str_appendf(&out, "    %s\n", r->session_dir ? r->session_dir : "");

            /* Show chunk preview (semantic signal) */
            if (r->chunk_preview && r->chunk_preview[0]) {
                str_appendf(&out, "  [semantic match — chunk %d]\n%s\n",
                            r->best_chunk, r->chunk_preview);
            }

            /* Show per-line lexical matches */
            if (r->n_matches > 0) {
                str_appendf(&out, "  [lexical — %d hit%s]\n",
                            r->match_count,
                            r->match_count == 1 ? "" : "s");
                for (int j = 0; j < r->n_matches; j++) {
                    ss_match_t *m = &r->matches[j];
                    str_appendf(&out, "  R%dS%d [%s]: %s\n",
                                m->react_loop, m->step, m->tool,
                                m->snippet ? m->snippet : "");
                }
                if (r->match_count > r->n_matches) {
                    str_appendf(&out, "  ... and %d more match%s\n",
                                r->match_count - r->n_matches,
                                (r->match_count - r->n_matches) == 1 ? "" : "es");
                }
            }
        }
    }

    if (results.count == 0) {
        if (is_pure_lexical) {
            str_appendf(&out, "No matches found for \"%s\" in recent sessions.\n",
                        pattern);
        } else {
            str_appendf(&out, "\nNo matching sessions found.\n");
        }
    }

    /* Save counts before free */
    int result_count = results.count;
    int sessions_searched = results.sessions_searched;
    int total_matches = results.total_matches;
    ss_results_free(&results);

    /* Store result */
    char *hash = store_save(ctx->store, out.data);
    char *alias = tool_register_alias(ctx, hash ? hash : "");

    cJSON *meta = cJSON_CreateObject();
    if (is_pure_lexical) {
        cJSON_AddNumberToObject(meta, "matches", total_matches);
        cJSON_AddNumberToObject(meta, "sessions_matched", result_count);
    } else {
        cJSON_AddNumberToObject(meta, "matches", result_count);
    }
    cJSON_AddNumberToObject(meta, "sessions_searched", sessions_searched);
    if (query) cJSON_AddStringToObject(meta, "query", query);
    if (pattern) cJSON_AddStringToObject(meta, "pattern", pattern);
    if (use_regex) cJSON_AddBoolToObject(meta, "regex", 1);
    if (days > 0) cJSON_AddNumberToObject(meta, "days", days);
    cJSON_AddNumberToObject(meta, "chars", (double)out.len);
    cJSON_AddStringToObject(meta, "ref", alias);

    /* Include preview for small results */
    if (out.len > 0 && out.len < 500) {
        cJSON_AddStringToObject(meta, "preview", out.data);
    } else if (out.len >= 500) {
        char preview[256];
        size_t copy_len = 200;
        if (copy_len > out.len) copy_len = out.len;
        memcpy(preview, out.data, copy_len);
        preview[copy_len] = '\0';
        strcat(preview, "...");
        cJSON_AddStringToObject(meta, "preview", preview);
    }

    tools_inject_thought(ctx, params);
    journal_append(ctx->journal, ctx->react_loop, ctx->step, "session_search",
                   params, alias, out.len, total_matches > 0 ? total_matches : result_count,
                   NULL, NULL);

    char *ref_copy = strdup(alias);
    free(alias);
    free(hash);
    str_free(&out);
    return tools_make_result(1, meta, ref_copy);
}
