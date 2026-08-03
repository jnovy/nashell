/* session_search.h — Unified episodic search: fused semantic + lexical scoring.
 *
 * Applies the same hybrid scoring pattern proven in memory.c's
 * score_entry_hybrid() to session/episodic search. Produces a per-session
 * heat map with both semantic (MaxSim over chunk embeddings) and lexical
 * (substring/regex match count) signals, blended into a composite score
 * with confidence tiers (HIGH = both agree, MEDIUM = one strong, LOW).
 *
 * v4.3: Multi-match per session — collects ALL lexical matches with
 * R{loop}S{step} [tool] attribution, not just one "best" snippet.
 * This subsumes session_grep entirely.
 *
 * Part of Nash unified memory architecture v4.3. */

#ifndef SESSION_SEARCH_H
#define SESSION_SEARCH_H

#include "session_index.h"
#include "embedding.h"

/* Maximum matches to keep per session */
#define SS_MAX_MATCHES_PER_SESSION 50

/* Confidence tiers based on signal agreement */
typedef enum {
  SS_CONFIDENCE_LOW,    /* neither signal strong */
  SS_CONFIDENCE_MEDIUM, /* one strong signal */
  SS_CONFIDENCE_HIGH    /* both signals agree (high confidence) */
} ss_confidence_t;

/* Per-match entry within a session */
typedef struct {
  char *snippet;  /* match context (up to ~4000 chars) */
  int react_loop; /* R{loop} attribution */
  int step;       /* S{step} attribution */
  char tool[64];  /* tool name */
} ss_match_t;

/* Per-session search result */
typedef struct {
  char *session_dir; /* full path to sessions/<ts>/ */
  double timestamp;  /* session timestamp */
  /* Semantic signal */
  double semantic_score; /* raw MaxSim score [0..1] */
  int best_chunk;        /* index of best-matching chunk (-1 if none) */
  char *chunk_preview;   /* best-matching chunk text (NULL if none) */
  /* Lexical signal — with full match list */
  double lexical_score; /* normalized lexical score [0..1] */
  int match_count;      /* total lexical matches in this session */
  ss_match_t *matches;  /* array of individual matches */
  int n_matches;        /* number of matches stored (≤ SS_MAX_MATCHES_PER_SESSION) */
  /* Fused */
  double composite_score;     /* blended final score */
  ss_confidence_t confidence; /* agreement-based confidence tier */
} ss_result_t;

typedef struct {
  ss_result_t *results;
  int count;
  int total_matches;     /* sum of all match_count across results */
  int sessions_searched; /* how many sessions were examined */
} ss_results_t;

/* ── Main search API ──────────────────────────────────── */

/* Unified episodic search.  At least one of query/pattern must be non-NULL.
 *
 * query:   semantic search text (NULL = skip semantic phase)
 * pattern: lexical pattern (NULL = skip lexical phase)
 * use_regex: 1 = POSIX Extended Regex, 0 = case-insensitive substring
 * max_results: maximum results to return (capped at 100)
 * days: 0 = no age limit; >0 = only sessions within N days
 *
 * If both query AND pattern are provided, scores are fused:
 *   composite = w_sem * semantic + w_lex * lexical (+ recency decay)
 *   confidence = HIGH if both scores > threshold
 *
 * If only query: pure semantic (equivalent to session_index_search)
 * If only pattern: pure lexical with per-line match attribution
 *
 * session_idx: in-memory session index (required for semantic; NULL ok for
 *              pure lexical, in which case sessions_dir is scanned)
 * embed: embedding context (required for semantic; NULL ok for pure lexical)
 * sessions_dir: parent directory containing session dirs (for lexical scan;
 *               if NULL, derived from session_idx entries)
 *
 * Caller must free with ss_results_free(). */
ss_results_t session_search(
  session_index_t *session_idx,
  embed_ctx_t *embed,
  const char *query,
  const char *pattern,
  int use_regex,
  int max_results,
  int days,
  const char *sessions_dir);

/* Free search results (including per-match arrays). */
void ss_results_free(ss_results_t *r);

#endif /* SESSION_SEARCH_H */
