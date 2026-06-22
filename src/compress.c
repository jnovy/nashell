/* compress.c — Line-level relevance compression and deduplication.
 *
 * Implements two key mechanisms from Harness-1 (arXiv 2606.02373):
 *   §3.1 Line-BM25 compression: compress tool outputs to the most
 *         relevant lines instead of blind truncation.  Content-type
 *         agnostic — works for code, prose, JSON, YAML, logs, etc.
 *   §3.3 Content deduplication: CRC32-based near-duplicate detection
 *         to prevent injecting the same content into context twice.
 */
#include "compress.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

/* ── Constants ──────────────────────────────────────────────────── */

#define COMPRESS_TAG        "[...compressed]"
#define COMPRESS_TAG_LEN    15
#define COMPRESS_MERGE_LEN  40    /* merge lines shorter than this */

/* ── CRC32 ──────────────────────────────────────────────────────── */

/* CRC32 lookup table (IEEE polynomial, same as zlib).
 * FIX 3a: Initialize at program startup via constructor attribute to avoid
 * a data race when two threads call compress_crc32() simultaneously.
 * Previously used a non-atomic flag check. */
static uint32_t crc32_table[256];

__attribute__((constructor))
static void crc32_init_table(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int j = 0; j < 8; j++)
            c = (c & 1) ? (0xEDB88320 ^ (c >> 1)) : (c >> 1);
        crc32_table[i] = c;
    }
}

uint32_t compress_crc32(const char *data, size_t len) {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++)
        crc = crc32_table[(crc ^ (uint8_t)data[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFF;
}

/* ── Line-level relevance compression ────────────────────────────── */

/* Tokenize a string into lowercase words for BM25-like scoring.
 * Returns array of malloc'd word strings. Sets *n_words.
 * Caller must free each word and the array. */
static char **tokenize_words(const char *text, int *n_words) {
    int cap = 64, count = 0;
    char **words = malloc((size_t)cap * sizeof(char *));
    if (!words) { *n_words = 0; return NULL; }

    const char *p = text;
    while (*p) {
        /* Skip non-alphanumeric (FLAW 3 FIX: was isalpha — skipped digit-starting
         * tokens like error codes 404, IPs 192.168.1.1, versions, hex values) */
        while (*p && !isalnum((unsigned char)*p)) p++;
        if (!*p) break;
        const char *start = p;
        while (*p && (isalnum((unsigned char)*p) || *p == '_')) p++;
        int wlen = (int)(p - start);
        if (wlen > 1 && wlen < 64) {  /* skip single chars */
            char *w = malloc((size_t)(wlen + 1));
            if (w) {
                for (int i = 0; i < wlen; i++)
                    w[i] = (char)tolower((unsigned char)start[i]);
                w[wlen] = '\0';
                if (count >= cap) {
                    cap *= 2;
                    char **tmp = realloc(words, (size_t)cap * sizeof(char *));
                    if (!tmp) { free(w); break; }
                    words = tmp;
                }
                words[count++] = w;
            }
        }
    }
    *n_words = count;
    return words;
}

static void free_words(char **words, int n) {
    if (!words) return;
    for (int i = 0; i < n; i++) free(words[i]);
    free(words);
}

/* DEDUP 1 FIX: Extracted chunk-freeing pattern (was 4 identical copies). */
static void free_chunks(char **chunks, int n) {
    if (!chunks) return;
    for (int i = 0; i < n; i++) free(chunks[i]);
    free(chunks);
}

/* DEDUP 2 FIX: Extracted grow-and-add pattern (was duplicated in FLUSH_MERGE
 * macro and emit-as-own-chunk block). Returns 0 on success, -1 on OOM. */
static int push_chunk(char ***chunks, int *count, int *cap, char *chunk) {
    if (*count >= *cap) {
        int new_cap = *cap * 2;
        char **tmp = realloc(*chunks, (size_t)new_cap * sizeof(char *));
        if (!tmp) return -1;
        *chunks = tmp;
        *cap = new_cap;
    }
    (*chunks)[(*count)++] = chunk;
    return 0;
}

/* D6 FIX: Stopword filter for BM25-like scoring. Without IDF, all query
 * terms are weighted equally. Filtering common English words and programming
 * keywords prevents them from dominating the score and drowning out the
 * rare, semantically meaningful terms. */
static int is_stopword(const char *word) {
    /* Top English stopwords + common programming terms.
     * All lowercase — query words are already lowercased by tokenize_words. */
    static const char *stopwords[] = {
        /* English */
        "the", "be", "to", "of", "and", "in", "that", "have", "it", "for",
        "not", "on", "with", "he", "as", "you", "do", "at", "this", "but",
        "his", "by", "from", "they", "we", "say", "her", "she", "or", "an",
        "will", "my", "one", "all", "would", "there", "their", "what",
        "so", "up", "out", "if", "about", "who", "get", "which", "go",
        "when", "can", "no", "just", "than", "been", "its", "also", "is",
        "was", "are", "were", "has", "had", "did", "does", "am",
        /* Programming ("if" and "for" already in English list above) */
        "int", "char", "void", "const", "return", "else",
        "while", "struct", "null", "true", "false", "static",
        "function", "var", "let", "new", "class", "string",
        NULL
    };
    for (int i = 0; stopwords[i]; i++) {
        if (strcmp(word, stopwords[i]) == 0)
            return 1;
    }
    return 0;
}

/* Count words in a chunk using the same tokenization as score_sentence:
 * alphanumeric+underscore runs of length 2..63. Needed for BM25 document
 * length (dl) and average document length (avgdl). */
static int count_words(const char *text) {
    int count = 0;
    const char *p = text;
    while (*p) {
        while (*p && !isalnum((unsigned char)*p)) p++;
        if (!*p) break;
        const char *wstart = p;
        while (*p && (isalnum((unsigned char)*p) || *p == '_')) p++;
        int wlen = (int)(p - wstart);
        if (wlen >= 2 && wlen < 64) count++;
    }
    return count;
}

/* BM25 TF-saturation scoring for chunks against a query.
 *
 * Replaces the previous binary scoring (exact=1.0, partial=0.5) with proper
 * BM25 term frequency saturation: repeated query term matches in a chunk
 * increase the score with diminishing returns, and longer chunks are
 * penalized via length normalization.
 *
 * Formula per query term: tf*(k1+1) / (tf + k1*(1 - b + b*dl/avgdl))
 *   k1=1.2 (saturation speed), b=0.75 (length normalization strength)
 *   tf = exact_matches + 0.5*partial_matches (substring hits count as half)
 *
 * D6 FIX: Stopwords still filtered — without IDF, they'd match everywhere.
 * FIX #10: Substring matching for code identifiers still supported.
 * SIMP 3 FIX: sentence_len parameter avoids redundant strlen. */
static float score_sentence(const char *sentence, int sentence_len,
                            char **query_words, int n_query,
                            int doc_wordcount, float avgdl) {
    if (!sentence || !query_words || n_query == 0) return 0.0f;

    /* BM25 parameters */
    const float k1 = 1.2f;
    const float b = 0.75f;

    /* D6 FIX: Count non-stopword query terms for normalization. */
    int n_effective = 0;
    for (int qi = 0; qi < n_query; qi++) {
        if (!is_stopword(query_words[qi]))
            n_effective++;
    }
    if (n_effective == 0) return 0.0f;

    float dl = (float)doc_wordcount;
    /* Guard against degenerate avgdl (empty chunks) */
    float safe_avgdl = avgdl > 0.0f ? avgdl : 1.0f;

    float score = 0.0f;
    for (int qi = 0; qi < n_query; qi++) {
        /* D6 FIX: Skip stopwords — they match everywhere and add noise */
        if (is_stopword(query_words[qi])) continue;
        int tf_exact = 0;
        int tf_partial = 0;
        int qlen = (int)strlen(query_words[qi]);

        /* Scan ALL sentence words — count frequencies, don't stop at first */
        const char *p = sentence;
        while (*p) {
            while (*p && !isalnum((unsigned char)*p)) p++;
            if (!*p) break;
            const char *wstart = p;
            while (*p && (isalnum((unsigned char)*p) || *p == '_')) p++;
            int wlen = (int)(p - wstart);
            if (wlen <= 1 || wlen >= 64) continue;

            /* Exact match */
            if (wlen == qlen) {
                int match = 1;
                for (int k = 0; k < wlen; k++) {
                    if ((char)tolower((unsigned char)wstart[k]) != query_words[qi][k]) {
                        match = 0; break;
                    }
                }
                if (match) { tf_exact++; continue; }
            }
            /* FIX #10: Substring match for code identifiers (>= 4 chars) */
            if (qlen >= 4 && wlen >= qlen) {
                for (int off = 0; off <= wlen - qlen; off++) {
                    int match = 1;
                    for (int k = 0; k < qlen; k++) {
                        if ((char)tolower((unsigned char)wstart[off + k]) != query_words[qi][k]) {
                            match = 0; break;
                        }
                    }
                    if (match) { tf_partial++; break; }
                }
            }
        }

        /* Effective TF: exact matches count full, partial (substring) as half */
        float tf = (float)tf_exact + 0.5f * (float)tf_partial;
        if (tf > 0.0f) {
            /* BM25 TF saturation with length normalization */
            score += tf * (k1 + 1.0f) /
                     (tf + k1 * (1.0f - b + b * dl / safe_avgdl));
        }
    }
    /* Normalize by effective (non-stopword) query term count */
    score /= (float)n_effective;

    /* SIMP 3 FIX: Length bonuses for substantial chunks */
    if (sentence_len > 80) score += 0.1f;
    if (sentence_len > 200) score += 0.1f;

    return score;
}

/* Split text into logical chunks by lines, merging short consecutive
 * non-indented lines into larger units.  Content-type agnostic: works
 * for code, prose, JSON, YAML, logs, diffs, etc.
 *
 * Returns array of malloc'd chunk strings.  Sets *n_chunks.
 * Caller must free each chunk and the array. */
static char **split_chunks(const char *text, int *n_chunks) {
    int cap = 128, count = 0;
    char **chunks = malloc((size_t)cap * sizeof(char *));
    if (!chunks) { *n_chunks = 0; return NULL; }

    const char *p = text;
    /* Dynamic merge buffer — grows to accommodate arbitrarily many
     * consecutive short lines (e.g., bullet lists, short log entries).
     * Previously a fixed 512-byte stack buffer that caused artificial
     * chunk boundaries in long bullet lists. */
    int merge_cap = 512;
    char *merge_buf = malloc((size_t)merge_cap);
    int  merge_len = 0;
    if (!merge_buf) { free(chunks); *n_chunks = 0; return NULL; }

    /* SIMP 2 FIX: Replaced FLUSH_MERGE macro (16-line macro with goto control
     * flow) with flush_merge inline helper using push_chunk (DEDUP 2). */
    #define FLUSH_MERGE() do { \
        if (merge_len > 0) { \
            char *_chunk = malloc((size_t)(merge_len + 1)); \
            if (_chunk) { \
                memcpy(_chunk, merge_buf, (size_t)merge_len); \
                _chunk[merge_len] = '\0'; \
                if (push_chunk(&chunks, &count, &cap, _chunk) < 0) \
                    { free(_chunk); goto done; } \
            } \
            merge_len = 0; \
        } \
    } while(0)

    while (*p) {
        /* Extract one line */
        const char *eol = strchr(p, '\n');
        if (!eol) eol = p + strlen(p);
        int ll = (int)(eol - p);

        /* Skip blank lines — they're separators, not content */
        int blank = 1;
        for (int i = 0; i < ll; i++) {
            if (p[i] != ' ' && p[i] != '\t' && p[i] != '\r') {
                blank = 0; break;
            }
        }
        if (blank) {
            FLUSH_MERGE();
            p = *eol ? eol + 1 : eol;
            continue;
        }

        /* Decide: merge into accumulator or emit as own chunk.
         * Merge if: line is short AND doesn't start with whitespace
         * (indentation = code structure, don't merge across indent levels). */
        int starts_with_ws = (ll > 0 && (p[0] == ' ' || p[0] == '\t'));
        if (ll < COMPRESS_MERGE_LEN && !starts_with_ws) {
            /* Grow merge buffer if needed */
            int need = merge_len + ll + 2;
            if (need > merge_cap) {
                int new_cap = merge_cap;
                while (new_cap < need) new_cap *= 2;
                char *tmp = realloc(merge_buf, (size_t)new_cap);
                if (!tmp) goto done;
                merge_buf = tmp;
                merge_cap = new_cap;
            }
            if (merge_len > 0) merge_buf[merge_len++] = ' ';
            memcpy(merge_buf + merge_len, p, (size_t)ll);
            merge_len += ll;
        } else {
            FLUSH_MERGE();
            /* DEDUP 2 FIX: Use push_chunk instead of inline grow-and-add */
            char *chunk = malloc((size_t)(ll + 1));
            if (chunk) {
                memcpy(chunk, p, (size_t)ll);
                chunk[ll] = '\0';
                if (push_chunk(&chunks, &count, &cap, chunk) < 0)
                    { free(chunk); goto done; }
            }
        }

        p = *eol ? eol + 1 : eol;
    }

    FLUSH_MERGE();
    #undef FLUSH_MERGE

done:
    free(merge_buf);
    *n_chunks = count;
    return chunks;
}

/* Comparison function for sorting scored chunks by score (descending) */
typedef struct {
    int   index;
    float score;
} scored_chunk_t;

static int cmp_scored_desc(const void *a, const void *b) {
    float sa = ((const scored_chunk_t *)a)->score;
    float sb = ((const scored_chunk_t *)b)->score;
    if (sb > sa) return 1;
    if (sb < sa) return -1;
    return 0;
}

/* Comparison function for sorting by original index (ascending) — preserve order */
static int cmp_index_asc(const void *a, const void *b) {
    int ia = ((const scored_chunk_t *)a)->index;
    int ib = ((const scored_chunk_t *)b)->index;
    /* SIMP 4 FIX: safe comparison (ia - ib can overflow for extreme values) */
    return (ia > ib) - (ia < ib);
}

/* B6 FIX: Shared chunk assembly helper. Writes chunks into `out` buffer up to
 * max_chars, appending COMPRESS_TAG if truncated. `indices` maps iteration order
 * to chunk[] indices (NULL = direct 0..count-1). Returns bytes written. */
static size_t emit_chunks(char *out, size_t out_cap, char **chunks,
                          const int *indices, int count, int max_chars,
                          int total_chunks) {
    size_t pos = 0;
    int emitted = 0;
    for (int i = 0; i < count; i++) {
        const char *s = chunks[indices ? indices[i] : i];
        size_t sl = strlen(s);
        /* FLAW 6 FIX: was +2 (reserved space for NUL which isn't part of
         * output length), causing the last fitting chunk to be skipped */
        if (pos + sl + 1 > (size_t)max_chars) break;
        memcpy(out + pos, s, sl);
        pos += sl;
        out[pos++] = '\n';
        emitted++;
    }
    if (emitted < total_chunks && pos + COMPRESS_TAG_LEN < out_cap) {
        memcpy(out + pos, COMPRESS_TAG, COMPRESS_TAG_LEN);
        pos += COMPRESS_TAG_LEN;
    }
    out[pos] = '\0';
    return pos;
}

char *compress_to_relevant(const char *text, const char *query,
                           int max_units, int max_chars) {
    if (!text || !text[0]) return NULL;
    int tlen = (int)strlen(text);

    /* Sane minimums — at least 1 chunk and 1 char */
    if (max_units < 1) max_units = 1;
    if (max_chars < 1) max_chars = 1;

    /* Short-circuit when text already fits within budget */
    if (tlen <= max_chars) return strdup(text);

    /* Split into content-agnostic chunks (lines with short-line merging) */
    int n_chunks;
    char **chunks = split_chunks(text, &n_chunks);
    if (!chunks || n_chunks == 0) {
        free(chunks);
        /* Fallback: hard truncate */
        /* D2 FIX: Guard against negative precision if max_chars < tag length */
        if (max_chars < COMPRESS_TAG_LEN + 1)
            max_chars = COMPRESS_TAG_LEN + 1;
        char *out = malloc((size_t)(max_chars + COMPRESS_TAG_LEN + 1));
        if (!out) return NULL;
        snprintf(out, (size_t)(max_chars + COMPRESS_TAG_LEN + 1),
                 "%.*s" COMPRESS_TAG, max_chars - COMPRESS_TAG_LEN, text);
        return out;
    }

    /* B6 FIX: If few enough chunks, emit them all via shared helper */
    if (n_chunks <= max_units) {
        size_t total = 0;
        for (int i = 0; i < n_chunks; i++) total += strlen(chunks[i]) + 1;
        size_t out_cap = total + COMPRESS_TAG_LEN + 1;
        char *out = malloc(out_cap);
        if (out)
            emit_chunks(out, out_cap, chunks, NULL, n_chunks, max_chars,
                        n_chunks);
        free_chunks(chunks, n_chunks);
        return out;
    }

    /* Tokenize query for BM25 scoring */
    int n_qwords;
    char **qwords = tokenize_words(query ? query : "", &n_qwords);

    /* Score each chunk by query relevance using BM25 TF saturation */
    scored_chunk_t *scored = malloc((size_t)n_chunks * sizeof(scored_chunk_t));
    if (!scored) {
        free_words(qwords, n_qwords);
        free_chunks(chunks, n_chunks);
        return NULL;
    }

    /* BM25 needs average document length (avgdl) for length normalization.
     * Pre-compute word count per chunk and derive avgdl. */
    int *wordcounts = malloc((size_t)n_chunks * sizeof(int));
    if (!wordcounts) {
        free(scored);
        free_words(qwords, n_qwords);
        free_chunks(chunks, n_chunks);
        return NULL;
    }
    int total_words = 0;
    for (int i = 0; i < n_chunks; i++) {
        wordcounts[i] = count_words(chunks[i]);
        total_words += wordcounts[i];
    }
    float avgdl = (float)total_words / (float)n_chunks;

    /* Compute max content-based score across all chunks to calibrate
     * position bonuses. When query terms match well, position bonuses
     * are secondary tiebreakers. When no terms match (short/empty query),
     * position bonuses dominate — which is the best we can do. */
    float max_content_score = 0.0f;
    for (int i = 0; i < n_chunks; i++) {
        scored[i].index = i;
        scored[i].score = score_sentence(chunks[i], (int)strlen(chunks[i]),
                                          qwords, n_qwords,
                                          wordcounts[i], avgdl);
        if (scored[i].score > max_content_score)
            max_content_score = scored[i].score;
    }
    free(wordcounts);
    /* Position bonus scales down when content scoring is effective.
     * At max_content_score=0 (no query matches), bonus_scale=0.3 (full).
     * At max_content_score>=0.5 (good matches), bonus_scale→0.06 (minimal). */
    float bonus_scale = 0.3f / (1.0f + max_content_score * 4.0f);
    for (int i = 0; i < n_chunks; i++) {
        /* Boost first few and last few chunks (often contain key info) */
        if (i == 0)      scored[i].score += bonus_scale;
        else if (i == 1) scored[i].score += bonus_scale * 0.73f;
        else if (i == 2) scored[i].score += bonus_scale * 0.47f;
        /* FLAW 1 FIX: Guard tail bonus against overlap with head region.
         * Previously when n_chunks<=4, middle chunks got BOTH head+tail
         * bonuses, inverting the intended ranking. */
        if (i >= n_chunks - 2 && i > 2) scored[i].score += bonus_scale * 0.5f;
    }

    /* Sort by score descending, keep top-N */
    qsort(scored, (size_t)n_chunks, sizeof(scored_chunk_t), cmp_scored_desc);
    int keep = max_units < n_chunks ? max_units : n_chunks;

    /* Re-sort the kept chunks by original index to preserve order */
    qsort(scored, (size_t)keep, sizeof(scored_chunk_t), cmp_index_asc);

    /* B6 FIX: Build output via shared emit helper */
    size_t out_cap = (size_t)max_chars + COMPRESS_TAG_LEN + 1;
    char *out = malloc(out_cap);
    if (!out) {
        free(scored);
        free_words(qwords, n_qwords);
        free_chunks(chunks, n_chunks);
        return NULL;
    }
    int *indices = malloc((size_t)keep * sizeof(int));
    if (indices) {
        for (int i = 0; i < keep; i++) indices[i] = scored[i].index;
        emit_chunks(out, out_cap, chunks, indices, keep, max_chars, n_chunks);
        free(indices);
    } else {
        out[0] = '\0';
    }

    /* Cleanup */
    free(scored);
    free_words(qwords, n_qwords);
    free_chunks(chunks, n_chunks);
    return out;
}
