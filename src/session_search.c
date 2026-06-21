/* session_search.c — Unified episodic search: fused semantic + lexical scoring.
 *
 * Three-phase pipeline:
 *   Phase 1: Semantic heat map (in-memory, O(sessions × chunks))
 *   Phase 2: Lexical heat map  (targeted disk I/O on top semantic hits)
 *   Phase 3: Score fusion + confidence assignment
 *
 * v4.3: Multi-match per session — collects ALL lexical matches with
 * R{loop}S{step} [tool] attribution.  Subsumes session_grep.
 *
 * Part of Nash unified memory architecture v4.3. */

#pragma GCC diagnostic ignored "-Wformat-truncation"

#include "session_search.h"
#include "nash_limits.h"
#include "journal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>    /* strcasestr */
#include <regex.h>
#include <dirent.h>
#include <time.h>
#include <math.h>

/* Maximum line length we'll read from journal.jsonl */
#define SS_JLINE_MAX     NASH_LINE_MAX

/* Maximum snippet length (matches session_grep's SNIPPET_MAX) */
#define SS_SNIPPET_MAX   4000

/* Semantic phase: max sessions to pass to lexical scan in fused mode.
 * In pure lexical mode (no query), ALL sessions are scanned. */
#define SS_FUSED_LEXICAL_BUDGET 50

/* Score thresholds for confidence tiers */
#define SS_THRESH_HIGH   0.30
#define SS_THRESH_MIN    0.10

/* Default fusion weights */
#define SS_W_SEM  0.6
#define SS_W_LEX  0.4

/* ── Internal scored session ──────────────────────────── */

typedef struct {
    char   dir[NASH_PATH_MAX];
    double timestamp;
    /* Semantic */
    double semantic;          /* [0..1] raw MaxSim */
    int    best_chunk;        /* index of best chunk, -1 if none */
    char  *chunk_preview;     /* strdup'd, NULL if none */
    int    si_entry_idx;      /* index into session_index entries (-1 if not from index) */
    /* Lexical — multi-match */
    double lexical;           /* [0..1] normalized */
    int    match_count;       /* total matches found */
    ss_match_t *matches;      /* array of individual matches */
    int    n_matches;         /* count of stored matches (≤ SS_MAX_MATCHES_PER_SESSION) */
    /* Fused */
    double recency;
    double composite;
    ss_confidence_t confidence;
    int    lexical_scanned;   /* 1 if Phase 2 ran on this session */
} scored_session_t;

/* ── Helpers ──────────────────────────────────────────── */

/* Recency score: same formula as session_index.c.
 * At 0 days: 1.0, at 30 days: ~0.59, at 365 days: ~0.28 */
static double compute_recency(double timestamp) {
    double now = (double)time(NULL);
    double age_days = (now - timestamp) / 86400.0;
    if (age_days < 0) age_days = 0;
    return 1.0 / (1.0 + log1p(age_days / 30.0));
}

/* Extract a snippet around the match position */
static char *extract_match_snippet(const char *line, const char *match_pos,
                                    size_t match_len) {
    char *buf = malloc(SS_SNIPPET_MAX + 16);
    if (!buf) return NULL;

    size_t linelen = strlen(line);
    int before = SS_SNIPPET_MAX / 2;
    int after  = SS_SNIPPET_MAX / 2;

    const char *start = match_pos - before;
    if (start < line) start = line;
    const char *end = match_pos + match_len + after;
    if (end > line + linelen) end = line + linelen;

    size_t span = (size_t)(end - start);
    if (span >= (size_t)(SS_SNIPPET_MAX - 4)) span = (size_t)(SS_SNIPPET_MAX - 5);

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

    /* Sanitize control chars */
    for (int i = 0; i < off; i++) {
        unsigned char c = (unsigned char)buf[i];
        if (c < 0x20 && c != ' ') buf[i] = ' ';
    }
    return buf;
}

/* Parse journal fields: react_loop, step, tool from a journal line.
 * Quick strstr parsing — no full JSON parse for speed. */
static void parse_journal_fields(const char *line,
                                  int *react_loop, int *step,
                                  char *tool_buf, size_t tool_buflen) {
    *react_loop = -1;
    *step = -1;
    tool_buf[0] = '\0';

    const char *p = strstr(line, "\"react_loop\":");
    if (p) *react_loop = atoi(p + 13);

    p = strstr(line, "\"step\":");
    if (p) *step = atoi(p + 6);

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

/* Free a scored_session_t's dynamically allocated fields */
static void scored_session_free_fields(scored_session_t *s) {
    free(s->chunk_preview);
    s->chunk_preview = NULL;
    if (s->matches) {
        for (int i = 0; i < s->n_matches; i++)
            free(s->matches[i].snippet);
        free(s->matches);
        s->matches = NULL;
    }
    s->n_matches = 0;
}

/* Compare scored_session_t by composite score descending */
static int cmp_scored_desc(const void *a, const void *b) {
    const scored_session_t *sa = a;
    const scored_session_t *sb = b;
    if (sb->composite > sa->composite) return 1;
    if (sb->composite < sa->composite) return -1;
    return 0;
}

/* Compare for fallback directory scan: timestamp descending */
typedef struct { char dir[NASH_PATH_MAX]; double timestamp; } dir_entry_t;

static int cmp_dir_desc(const void *a, const void *b) {
    const dir_entry_t *ea = a;
    const dir_entry_t *eb = b;
    if (ea->timestamp > eb->timestamp) return -1;
    if (ea->timestamp < eb->timestamp) return 1;
    return 0;
}


/* ══════════════════════════════════════════════════════════
 *  Phase 1: Semantic heat map
 * ══════════════════════════════════════════════════════════ */

static int phase_semantic(session_index_t *idx, const embed_vec_t *query_emb,
                           double cutoff_ts,
                           scored_session_t *scored, int cap) {
    if (!idx || !query_emb || !query_emb->data) return 0;

    pthread_mutex_lock(&idx->mtx);
    int n = 0;

    for (int i = 0; i < idx->count && n < cap; i++) {
        session_index_entry_t *e = &idx->entries[i];

        /* Age filter */
        if (cutoff_ts > 0 && e->timestamp < cutoff_ts) continue;

        float sim = 0.0f;
        int best_chunk = -1;

        if (e->has_chunks) {
            /* MaxSim across chunks */
            for (int c = 0; c < e->chunks_emb.n_chunks; c++) {
                embed_vec_t chunk_vec = {
                    .data = e->chunks_emb.data + c * e->chunks_emb.dim,
                    .dim  = e->chunks_emb.dim
                };
                float s = embed_cosine_sim(query_emb, &chunk_vec);
                if (s > sim) { sim = s; best_chunk = c; }
            }
        } else if (e->has_emb) {
            sim = embed_cosine_sim(query_emb, &e->emb);
        } else {
            continue;
        }

        if (sim < 0.10f) continue;  /* skip very low */

        scored[n].timestamp     = e->timestamp;
        scored[n].semantic      = (double)sim;
        scored[n].best_chunk    = best_chunk;
        scored[n].si_entry_idx  = i;
        scored[n].recency       = compute_recency(e->timestamp);
        scored[n].lexical       = 0;
        scored[n].match_count   = 0;
        scored[n].matches       = NULL;
        scored[n].n_matches     = 0;
        scored[n].lexical_scanned = 0;
        scored[n].chunk_preview = NULL;

        snprintf(scored[n].dir, NASH_PATH_MAX, "%s", e->session_dir);

        /* Copy chunk preview */
        if (best_chunk >= 0 && e->chunk_previews &&
            best_chunk < e->n_chunk_previews && e->chunk_previews[best_chunk]) {
            scored[n].chunk_preview = strdup(e->chunk_previews[best_chunk]);
        }

        n++;
    }

    pthread_mutex_unlock(&idx->mtx);
    return n;
}


/* ══════════════════════════════════════════════════════════
 *  Phase 2: Lexical heat map — multi-match per session
 * ══════════════════════════════════════════════════════════ */

/* Scan a single session's journal.jsonl for lexical matches.
 * Collects ALL matches (up to SS_MAX_MATCHES_PER_SESSION) with
 * R/S/tool attribution into scored->matches array.
 * Updates scored->lexical, match_count, n_matches.
 * Returns total number of matches found. */
static int scan_journal_lexical(const char *session_dir,
                                 const char *pattern, int use_regex,
                                 regex_t *compiled_re,
                                 scored_session_t *scored) {
    char jpath[NASH_PATH_MAX];
    snprintf(jpath, sizeof(jpath), "%s/journal.jsonl", session_dir);

    FILE *f = fopen(jpath, "r");
    if (!f) return 0;

    char *line_buf = malloc(SS_JLINE_MAX);
    if (!line_buf) { fclose(f); return 0; }

    int count = 0;

    /* Pre-allocate match array */
    int match_cap = 16;
    ss_match_t *matches = calloc((size_t)match_cap, sizeof(ss_match_t));

    while (fgets(line_buf, SS_JLINE_MAX, f)) {
        const char *match_pos = NULL;
        size_t match_len = 0;

        if (use_regex) {
            regmatch_t pmatch[1];
            if (regexec(compiled_re, line_buf, 1, pmatch, 0) == 0) {
                match_pos = line_buf + pmatch[0].rm_so;
                match_len = (size_t)(pmatch[0].rm_eo - pmatch[0].rm_so);
            }
        } else {
            match_pos = strcasestr(line_buf, pattern);
            if (match_pos) match_len = strlen(pattern);
        }

        if (!match_pos) continue;

        /* Parse R/S/tool attribution */
        int rl, step;
        char tool[64];
        parse_journal_fields(line_buf, &rl, &step, tool, sizeof(tool));

        /* Skip structural noise */
        if (journal_is_structural_tool(tool)) continue;

        count++;

        /* Store match if under per-session cap */
        if (matches && count <= SS_MAX_MATCHES_PER_SESSION) {
            int idx = count - 1;
            if (idx >= match_cap) {
                int new_cap = match_cap * 2;
                if (new_cap > SS_MAX_MATCHES_PER_SESSION)
                    new_cap = SS_MAX_MATCHES_PER_SESSION;
                ss_match_t *new_m = realloc(matches,
                                            (size_t)new_cap * sizeof(ss_match_t));
                if (new_m) {
                    memset(new_m + match_cap, 0,
                           (size_t)(new_cap - match_cap) * sizeof(ss_match_t));
                    matches = new_m;
                    match_cap = new_cap;
                } else {
                    /* Can't grow — stop collecting but keep counting */
                    continue;
                }
            }
            matches[idx].snippet = extract_match_snippet(line_buf, match_pos,
                                                          match_len);
            matches[idx].react_loop = rl;
            matches[idx].step = step;
            snprintf(matches[idx].tool, sizeof(matches[idx].tool), "%s", tool);
        }
    }

    fclose(f);
    free(line_buf);

    scored->match_count = count;
    scored->lexical_scanned = 1;

    if (count > 0 && matches) {
        int n_stored = count < SS_MAX_MATCHES_PER_SESSION
                       ? count : SS_MAX_MATCHES_PER_SESSION;
        scored->matches = matches;
        scored->n_matches = n_stored;
    } else {
        /* No matches — free the pre-allocated array */
        free(matches);
        scored->matches = NULL;
        scored->n_matches = 0;
    }

    /* Normalize lexical score: log scale, saturating at ~10 matches → 1.0.
     * 0 matches → 0, 1 → 0.43, 3 → 0.60, 5 → 0.70, 10 → 0.83, 20+ → ~1.0 */
    if (count > 0) {
        scored->lexical = log1p((double)count) / log1p(20.0);
        if (scored->lexical > 1.0) scored->lexical = 1.0;
    }

    return count;
}


/* Phase 2 main: run lexical scan on selected sessions.
 *
 * Strategy:
 *   - If semantic phase ran (fused mode): scan top SS_FUSED_LEXICAL_BUDGET
 *     sessions by semantic score
 *   - If no semantic phase (pure lexical): scan ALL sessions (like session_grep) */
static int phase_lexical(session_index_t *idx,
                          const char *pattern, int use_regex,
                          regex_t *compiled_re,
                          double cutoff_ts,
                          const char *sessions_dir,
                          scored_session_t *scored, int *n_scored, int cap,
                          int has_semantic) {
    if (!pattern || !pattern[0]) return 0;

    int total_matches = 0;

    if (has_semantic) {
        /* Fused mode: scan top sessions from Phase 1 (already roughly ordered
         * by semantic score via insertion order). */
        int budget = *n_scored < SS_FUSED_LEXICAL_BUDGET
                     ? *n_scored : SS_FUSED_LEXICAL_BUDGET;
        for (int i = 0; i < budget; i++) {
            total_matches += scan_journal_lexical(
                scored[i].dir, pattern, use_regex, compiled_re, &scored[i]);
        }
    } else {
        /* Pure lexical mode: build session list from index or directory scan,
         * then scan ALL of them (no budget limit). */
        int n = *n_scored;

        if (idx) {
            /* Use session index for session list */
            pthread_mutex_lock(&idx->mtx);
            for (int i = 0; i < idx->count && n < cap; i++) {
                session_index_entry_t *e = &idx->entries[i];
                if (cutoff_ts > 0 && e->timestamp < cutoff_ts) continue;

                snprintf(scored[n].dir, NASH_PATH_MAX, "%s", e->session_dir);
                scored[n].timestamp    = e->timestamp;
                scored[n].semantic     = 0;
                scored[n].best_chunk   = -1;
                scored[n].chunk_preview = NULL;
                scored[n].si_entry_idx = i;
                scored[n].recency      = compute_recency(e->timestamp);
                scored[n].lexical      = 0;
                scored[n].match_count  = 0;
                scored[n].matches      = NULL;
                scored[n].n_matches    = 0;
                scored[n].lexical_scanned = 0;
                n++;
            }
            pthread_mutex_unlock(&idx->mtx);
        } else if (sessions_dir) {
            /* Fallback: scan directory */
            DIR *d = opendir(sessions_dir);
            if (d) {
                dir_entry_t *dirs = NULL;
                int n_dirs = 0, cap_dirs = 256;
                dirs = calloc((size_t)cap_dirs, sizeof(dir_entry_t));

                struct dirent *de;
                while (dirs && (de = readdir(d)) != NULL) {
                    if (de->d_name[0] == '.') continue;
                    if (n_dirs >= cap_dirs) {
                        cap_dirs *= 2;
                        dir_entry_t *new_d = realloc(dirs, (size_t)cap_dirs * sizeof(dir_entry_t));
                        if (!new_d) break;
                        dirs = new_d;
                    }
                    snprintf(dirs[n_dirs].dir, NASH_PATH_MAX, "%s/%s",
                             sessions_dir, de->d_name);
                    dirs[n_dirs].timestamp = atof(de->d_name);
                    n_dirs++;
                }
                closedir(d);

                /* Sort newest first */
                if (dirs && n_dirs > 1)
                    qsort(dirs, (size_t)n_dirs, sizeof(dir_entry_t), cmp_dir_desc);

                /* Copy into scored array */
                for (int i = 0; dirs && i < n_dirs && n < cap; i++) {
                    if (cutoff_ts > 0 && dirs[i].timestamp < cutoff_ts) break;
                    snprintf(scored[n].dir, NASH_PATH_MAX, "%s", dirs[i].dir);
                    scored[n].timestamp    = dirs[i].timestamp;
                    scored[n].semantic     = 0;
                    scored[n].best_chunk   = -1;
                    scored[n].chunk_preview = NULL;
                    scored[n].si_entry_idx = -1;
                    scored[n].recency      = compute_recency(dirs[i].timestamp);
                    scored[n].lexical      = 0;
                    scored[n].match_count  = 0;
                    scored[n].matches      = NULL;
                    scored[n].n_matches    = 0;
                    scored[n].lexical_scanned = 0;
                    n++;
                }
                free(dirs);
            }
        }

        *n_scored = n;

        /* Scan ALL sessions — no budget limit in pure lexical mode */
        for (int i = 0; i < n; i++) {
            total_matches += scan_journal_lexical(
                scored[i].dir, pattern, use_regex, compiled_re, &scored[i]);
        }
    }

    return total_matches;
}


/* ══════════════════════════════════════════════════════════
 *  Phase 3: Fusion + confidence assignment
 * ══════════════════════════════════════════════════════════ */

static void phase_fusion(scored_session_t *scored, int n,
                          int has_semantic, int has_lexical) {
    for (int i = 0; i < n; i++) {
        double sem = scored[i].semantic;
        double lex = scored[i].lexical;
        double rec = scored[i].recency;

        if (has_semantic && has_lexical) {
            /* Fused mode: blend both signals with recency */
            scored[i].composite = (SS_W_SEM * sem + SS_W_LEX * lex) * rec;

            /* Confidence tiers */
            if (sem >= SS_THRESH_HIGH && lex >= SS_THRESH_HIGH)
                scored[i].confidence = SS_CONFIDENCE_HIGH;
            else if (sem >= SS_THRESH_HIGH || lex >= SS_THRESH_HIGH)
                scored[i].confidence = SS_CONFIDENCE_MEDIUM;
            else
                scored[i].confidence = SS_CONFIDENCE_LOW;
        } else if (has_semantic) {
            scored[i].composite = sem * rec;
            scored[i].confidence = sem >= SS_THRESH_HIGH
                ? SS_CONFIDENCE_MEDIUM : SS_CONFIDENCE_LOW;
        } else {
            /* Pure lexical */
            scored[i].composite = lex * rec;
            scored[i].confidence = lex >= SS_THRESH_HIGH
                ? SS_CONFIDENCE_MEDIUM : SS_CONFIDENCE_LOW;
        }
    }
}


/* ══════════════════════════════════════════════════════════
 *  Public API
 * ══════════════════════════════════════════════════════════ */

ss_results_t session_search(
    session_index_t *session_idx,
    embed_ctx_t *embed,
    const char *query,
    const char *pattern,
    int use_regex,
    int max_results,
    int days,
    const char *sessions_dir)
{
    ss_results_t out = {0};

    if (!query && !pattern) return out;
    if (max_results <= 0) max_results = 20;
    if (max_results > 100) max_results = 100;

    /* Age cutoff */
    double cutoff_ts = 0.0;
    if (days > 0) cutoff_ts = (double)time(NULL) - (double)days * 86400.0;

    /* Allocate scored array — conservatively large */
    int cap = 2048;
    scored_session_t *scored = calloc((size_t)cap, sizeof(scored_session_t));
    if (!scored) return out;

    int n_scored = 0;
    int has_semantic = 0;
    int has_lexical = (pattern && pattern[0]);

    /* ── Phase 1: Semantic ── */
    embed_vec_t query_emb = {0};
    if (query && query[0] && embed && session_idx) {
        query_emb = embed_text(embed, query);
        if (query_emb.data) {
            n_scored = phase_semantic(session_idx, &query_emb, cutoff_ts,
                                      scored, cap);
            has_semantic = (n_scored > 0);
        }
    }

    /* ── Phase 2: Lexical ── */
    regex_t compiled_re;
    int regex_compiled = 0;
    if (has_lexical && use_regex) {
        int rc = regcomp(&compiled_re, pattern, REG_EXTENDED | REG_ICASE | REG_NEWLINE);
        if (rc != 0) {
            /* Invalid regex — skip lexical phase */
            has_lexical = 0;
        } else {
            regex_compiled = 1;
        }
    }

    int total_matches = 0;
    if (has_lexical) {
        total_matches = phase_lexical(session_idx, pattern, use_regex,
                      regex_compiled ? &compiled_re : NULL,
                      cutoff_ts, sessions_dir,
                      scored, &n_scored, cap, has_semantic);
    }

    if (regex_compiled) regfree(&compiled_re);

    out.sessions_searched = n_scored;

    /* ── Phase 3: Fusion ── */
    phase_fusion(scored, n_scored, has_semantic, has_lexical);

    /* Sort by composite score descending */
    if (n_scored > 1)
        qsort(scored, (size_t)n_scored, sizeof(scored_session_t), cmp_scored_desc);

    /* Filter out very low scores and sessions with no signal */
    int n_valid = 0;
    for (int i = 0; i < n_scored; i++) {
        if (scored[i].composite >= SS_THRESH_MIN)
            n_valid++;
        else
            break;  /* sorted, rest are lower */
    }

    /* Take top-K */
    int n_results = n_valid < max_results ? n_valid : max_results;
    if (n_results > 0) {
        out.results = calloc((size_t)n_results, sizeof(ss_result_t));
        if (out.results) {
            for (int i = 0; i < n_results; i++) {
                scored_session_t *s = &scored[i];
                out.results[i].session_dir     = strdup(s->dir);
                out.results[i].timestamp       = s->timestamp;
                out.results[i].semantic_score   = s->semantic;
                out.results[i].best_chunk       = s->best_chunk;
                out.results[i].chunk_preview    = s->chunk_preview;
                s->chunk_preview = NULL;  /* transfer ownership */
                out.results[i].lexical_score    = s->lexical;
                out.results[i].match_count      = s->match_count;
                /* Transfer match array ownership */
                out.results[i].matches          = s->matches;
                out.results[i].n_matches        = s->n_matches;
                s->matches = NULL;  /* transfer ownership */
                s->n_matches = 0;
                out.results[i].composite_score  = s->composite;
                out.results[i].confidence       = s->confidence;
            }
            out.count = n_results;
            out.total_matches = total_matches;
        }
    }

    /* Clean up non-transferred data in scored array */
    for (int i = 0; i < n_scored; i++) {
        scored_session_free_fields(&scored[i]);
    }
    free(scored);

    embed_vec_free(&query_emb);

    return out;
}

void ss_results_free(ss_results_t *r) {
    if (!r) return;
    for (int i = 0; i < r->count; i++) {
        free(r->results[i].session_dir);
        free(r->results[i].chunk_preview);
        /* Free per-match arrays */
        if (r->results[i].matches) {
            for (int j = 0; j < r->results[i].n_matches; j++)
                free(r->results[i].matches[j].snippet);
            free(r->results[i].matches);
        }
    }
    free(r->results);
    r->results = NULL;
    r->count = 0;
}
