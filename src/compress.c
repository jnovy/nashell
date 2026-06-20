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
#define COMPRESS_MIN_CHARS  800   /* minimum output budget */
#define COMPRESS_MIN_UNITS  12    /* minimum chunks to keep */
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

int compress_is_duplicate(uint32_t hash, const uint32_t *hash_buf,
                          int hash_count) {
    for (int i = 0; i < hash_count; i++) {
        if (hash_buf[i] == hash) return 1;
    }
    return 0;
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
        /* Skip non-alpha */
        while (*p && !isalpha((unsigned char)*p)) p++;
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

/* Simple BM25-like term overlap score between a sentence and query terms.
 * Not full BM25 (no IDF, no doc length normalization) but captures the
 * key signal: how many query terms appear in this sentence.
 * FIX #10: Also does substring matching to handle code-centric queries
 * where query words appear as substrings in identifiers (e.g., "evict"
 * matches "evict_mark", "n_evictable"). Without this, all sentences
 * score 0 for code content and only position bonuses differentiate. */
static float score_sentence(const char *sentence, char **query_words, int n_query) {
    if (!sentence || !query_words || n_query == 0) return 0.0f;

    int n_sent;
    char **sent_words = tokenize_words(sentence, &n_sent);
    if (!sent_words) return 0.0f;

    float score = 0.0f;
    for (int qi = 0; qi < n_query; qi++) {
        int found_exact = 0;
        int found_partial = 0;
        int qlen = (int)strlen(query_words[qi]);
        for (int si = 0; si < n_sent; si++) {
            if (strcmp(query_words[qi], sent_words[si]) == 0) {
                found_exact = 1;
                break;
            }
            /* FIX #10: Substring match for code identifiers.
             * Only match query words >= 4 chars to avoid false
             * positives on short common words like "in", "to". */
            if (!found_partial && qlen >= 4 &&
                strstr(sent_words[si], query_words[qi])) {
                found_partial = 1;
            }
        }
        if (found_exact)
            score += 1.0f;
        else if (found_partial)
            score += 0.5f;  /* partial credit for substring match */
    }
    /* Normalize by query length to get overlap ratio */
    score /= (float)n_query;

    /* Bonus for longer sentences (they carry more info) */
    int slen = (int)strlen(sentence);
    if (slen > 80) score += 0.1f;
    if (slen > 200) score += 0.1f;

    free_words(sent_words, n_sent);
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
    /* Accumulator for merging short lines */
    char merge_buf[512];
    int  merge_len = 0;

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
            /* Flush merge buffer as its own chunk */
            if (merge_len > 0) {
                char *chunk = malloc((size_t)(merge_len + 1));
                if (chunk) {
                    memcpy(chunk, merge_buf, (size_t)merge_len);
                    chunk[merge_len] = '\0';
                    if (count >= cap) {
                        cap *= 2;
                        char **tmp = realloc(chunks, (size_t)cap * sizeof(char *));
                        if (!tmp) { free(chunk); break; }
                        chunks = tmp;
                    }
                    chunks[count++] = chunk;
                }
                merge_len = 0;
            }
            p = *eol ? eol + 1 : eol;
            continue;
        }

        /* Decide: merge into accumulator or emit as own chunk.
         * Merge if: line is short AND doesn't start with whitespace
         * (indentation = code structure, don't merge across indent levels). */
        int starts_with_ws = (ll > 0 && (p[0] == ' ' || p[0] == '\t'));
        if (ll < COMPRESS_MERGE_LEN && !starts_with_ws &&
            merge_len + ll + 1 < (int)sizeof(merge_buf)) {
            if (merge_len > 0) merge_buf[merge_len++] = ' ';
            memcpy(merge_buf + merge_len, p, (size_t)ll);
            merge_len += ll;
        } else {
            /* Flush any pending merge buffer first */
            if (merge_len > 0) {
                char *chunk = malloc((size_t)(merge_len + 1));
                if (chunk) {
                    memcpy(chunk, merge_buf, (size_t)merge_len);
                    chunk[merge_len] = '\0';
                    if (count >= cap) {
                        cap *= 2;
                        char **tmp = realloc(chunks, (size_t)cap * sizeof(char *));
                        if (!tmp) { free(chunk); break; }
                        chunks = tmp;
                    }
                    chunks[count++] = chunk;
                }
                merge_len = 0;
            }
            /* Emit current line as its own chunk */
            char *chunk = malloc((size_t)(ll + 1));
            if (chunk) {
                memcpy(chunk, p, (size_t)ll);
                chunk[ll] = '\0';
                if (count >= cap) {
                    cap *= 2;
                    char **tmp = realloc(chunks, (size_t)cap * sizeof(char *));
                    if (!tmp) { free(chunk); break; }
                    chunks = tmp;
                }
                chunks[count++] = chunk;
            }
        }

        p = *eol ? eol + 1 : eol;
    }

    /* Flush trailing merge buffer */
    if (merge_len > 0) {
        char *chunk = malloc((size_t)(merge_len + 1));
        if (chunk) {
            memcpy(chunk, merge_buf, (size_t)merge_len);
            chunk[merge_len] = '\0';
            if (count >= cap) {
                cap *= 2;
                char **tmp = realloc(chunks, (size_t)cap * sizeof(char *));
                if (!tmp) { free(chunk); goto done; }
                chunks = tmp;
            }
            chunks[count++] = chunk;
        }
    }
done:
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
    return ia - ib;
}

char *compress_to_relevant(const char *text, const char *query,
                           int max_units, int max_chars) {
    if (!text || !text[0]) return NULL;
    int tlen = (int)strlen(text);

    /* Enforce minimum budgets so compression is never pathologically tight */
    if (max_units < COMPRESS_MIN_UNITS) max_units = COMPRESS_MIN_UNITS;
    if (max_chars < COMPRESS_MIN_CHARS) max_chars = COMPRESS_MIN_CHARS;

    /* Short-circuit when text already fits within budget */
    if (tlen <= max_chars) return strdup(text);

    /* Split into content-agnostic chunks (lines with short-line merging) */
    int n_chunks;
    char **chunks = split_chunks(text, &n_chunks);
    if (!chunks || n_chunks == 0) {
        free(chunks);
        /* Fallback: hard truncate */
        char *out = malloc((size_t)(max_chars + COMPRESS_TAG_LEN + 1));
        if (!out) return NULL;
        snprintf(out, (size_t)(max_chars + COMPRESS_TAG_LEN + 1),
                 "%.*s" COMPRESS_TAG, max_chars - COMPRESS_TAG_LEN, text);
        return out;
    }

    /* If few enough chunks, just emit them all up to the char limit */
    if (n_chunks <= max_units) {
        size_t total = 0;
        for (int i = 0; i < n_chunks; i++) total += strlen(chunks[i]) + 1;
        char *out = malloc(total + COMPRESS_TAG_LEN + 1);
        if (out) {
            size_t pos = 0;
            int truncated = 0;
            for (int i = 0; i < n_chunks; i++) {
                size_t sl = strlen(chunks[i]);
                if (pos + sl + 2 > (size_t)max_chars) {
                    truncated = 1;
                    break;
                }
                memcpy(out + pos, chunks[i], sl);
                pos += sl;
                out[pos++] = '\n';
            }
            if (truncated && pos + COMPRESS_TAG_LEN < total + COMPRESS_TAG_LEN + 1) {
                memcpy(out + pos, COMPRESS_TAG, COMPRESS_TAG_LEN);
                pos += COMPRESS_TAG_LEN;
            }
            out[pos] = '\0';
        }
        for (int i = 0; i < n_chunks; i++) free(chunks[i]);
        free(chunks);
        return out;
    }

    /* Tokenize query for BM25-like scoring */
    int n_qwords;
    char **qwords = tokenize_words(query ? query : "", &n_qwords);

    /* Score each chunk by query relevance */
    scored_chunk_t *scored = malloc((size_t)n_chunks * sizeof(scored_chunk_t));
    if (!scored) {
        free_words(qwords, n_qwords);
        for (int i = 0; i < n_chunks; i++) free(chunks[i]);
        free(chunks);
        return NULL;
    }
    for (int i = 0; i < n_chunks; i++) {
        scored[i].index = i;
        scored[i].score = score_sentence(chunks[i], qwords, n_qwords);
        /* Boost first few and last few chunks (often contain key info) */
        if (i < 3) scored[i].score += 0.3f - i * 0.08f;
        if (i >= n_chunks - 2) scored[i].score += 0.15f;
    }

    /* Sort by score descending, keep top-N */
    qsort(scored, (size_t)n_chunks, sizeof(scored_chunk_t), cmp_scored_desc);
    int keep = max_units < n_chunks ? max_units : n_chunks;

    /* Re-sort the kept chunks by original index to preserve order */
    qsort(scored, (size_t)keep, sizeof(scored_chunk_t), cmp_index_asc);

    /* Build output */
    size_t out_cap = (size_t)max_chars + COMPRESS_TAG_LEN + 1;
    char *out = malloc(out_cap);
    if (!out) {
        free(scored);
        free_words(qwords, n_qwords);
        for (int i = 0; i < n_chunks; i++) free(chunks[i]);
        free(chunks);
        return NULL;
    }
    size_t pos = 0;
    for (int i = 0; i < keep; i++) {
        const char *s = chunks[scored[i].index];
        size_t sl = strlen(s);
        if (pos + sl + 2 > (size_t)max_chars) break;
        memcpy(out + pos, s, sl);
        pos += sl;
        out[pos++] = '\n';
    }
    if (pos > 0 && n_chunks > keep) {
        if (pos + COMPRESS_TAG_LEN < out_cap) {
            memcpy(out + pos, COMPRESS_TAG, COMPRESS_TAG_LEN);
            pos += COMPRESS_TAG_LEN;
        }
    }
    out[pos] = '\0';

    /* Cleanup */
    free(scored);
    free_words(qwords, n_qwords);
    for (int i = 0; i < n_chunks; i++) free(chunks[i]);
    free(chunks);
    return out;
}
