/* compress.c — Sentence-level relevance compression and deduplication.
 *
 * Implements two key mechanisms from Harness-1 (arXiv 2606.02373):
 *   §3.1 Sentence-BM25 compression: compress tool outputs to the most
 *         relevant sentences instead of blind truncation.
 *   §3.3 Content deduplication: CRC32-based near-duplicate detection
 *         to prevent injecting the same content into context twice.
 */
#include "compress.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

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

/* ── Sentence-level relevance compression ────────────────────────── */

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

/* Split text into sentences at '.', '!', '?', or '\n\n'.
 * Returns array of malloc'd sentence strings. Sets *n_sentences.
 * Caller must free each sentence and the array. */
static char **split_sentences(const char *text, int *n_sentences) {
    int cap = 64, count = 0;
    char **sents = malloc((size_t)cap * sizeof(char *));
    if (!sents) { *n_sentences = 0; return NULL; }

    const char *p = text;
    while (*p) {
        /* Skip leading whitespace */
        while (*p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
        if (!*p) break;

        const char *start = p;
        /* Find sentence end.
         * FIX MED#9: Don't treat '.' as sentence boundary when it appears
         * inside filenames (file.c), version numbers (v2.0), IP addresses
         * (127.0.0.1), or abbreviations (e.g., i.e.). A '.' is a sentence
         * boundary only if the char before it is not a digit/slash and the
         * char after it is whitespace-then-uppercase, end of string, or
         * a newline. This is critical for a coding agent where tool outputs
         * contain pervasive dotted identifiers. */
        while (*p) {
            if (*p == '!' || *p == '?') {
                p++;
                while (*p == ' ') p++;
                break;
            }
            if (*p == '.') {
                /* Check if this dot is a real sentence boundary:
                 * NOT a boundary if preceded by a digit or followed by
                 * an alphanumeric char (covers filenames, versions, IPs) */
                int prev_is_alnum = (p > start && (isalnum((unsigned char)*(p-1)) || *(p-1) == '/'));
                int next_is_alnum = (*(p+1) && isalnum((unsigned char)*(p+1)));
                if (prev_is_alnum && next_is_alnum) {
                    /* Dot inside identifier — skip */
                    p++;
                    continue;
                }
                /* Also skip single-letter abbreviations like e.g. i.e. */
                if (p > start && isalpha((unsigned char)*(p-1)) &&
                    p - start >= 1 && (p - 1 == start || !isalpha((unsigned char)*(p-2))) &&
                    *(p+1) && isalpha((unsigned char)*(p+1))) {
                    p++;
                    continue;
                }
                p++;
                /* Skip trailing dots/spaces */
                while (*p == '.' || *p == ' ') p++;
                break;
            }
            if (*p == '\n' && *(p + 1) == '\n') {
                p += 2;
                break;
            }
            p++;
        }

        int slen = (int)(p - start);
        if (slen > 5) {  /* skip trivial fragments */
            char *sent = malloc((size_t)(slen + 1));
            if (sent) {
                memcpy(sent, start, (size_t)slen);
                sent[slen] = '\0';
                if (count >= cap) {
                    cap *= 2;
                    char **tmp = realloc(sents, (size_t)cap * sizeof(char *));
                    if (!tmp) { free(sent); break; }
                    sents = tmp;
                }
                sents[count++] = sent;
            }
        }
    }
    *n_sentences = count;
    return sents;
}

/* Comparison function for sorting scored sentences by score (descending) */
typedef struct {
    int   index;
    float score;
} scored_sentence_t;

static int cmp_scored_desc(const void *a, const void *b) {
    float sa = ((const scored_sentence_t *)a)->score;
    float sb = ((const scored_sentence_t *)b)->score;
    if (sb > sa) return 1;
    if (sb < sa) return -1;
    return 0;
}

/* Comparison function for sorting by original index (ascending) — preserve order */
static int cmp_index_asc(const void *a, const void *b) {
    int ia = ((const scored_sentence_t *)a)->index;
    int ib = ((const scored_sentence_t *)b)->index;
    return ia - ib;
}

char *compress_to_relevant(const char *text, const char *query,
                           int max_sentences, int max_chars) {
    if (!text || !text[0]) return NULL;
    int tlen = (int)strlen(text);

    /* FIX #15: Short-circuit when text is already within bounds.
     * Previously required max_sentences <= 0 which is never true from
     * the eviction call site (always passes 4). Now also returns early
     * when text fits within max_chars, avoiding unnecessary sentence
     * splitting and scoring for short messages. */
    if (tlen <= max_chars) return strdup(text);

    /* Split into sentences */
    int n_sents;
    char **sents = split_sentences(text, &n_sents);
    if (!sents || n_sents == 0) {
        free(sents);
        /* Fallback: truncate */
        char *out = malloc((size_t)(max_chars + 16));
        if (!out) return NULL;
        snprintf(out, (size_t)(max_chars + 16), "%.*s...[compressed]", max_chars - 16, text);
        return out;
    }

    /* If we have few sentences, just return them all (up to char limit) */
    if (n_sents <= max_sentences) {
        size_t total = 0;
        for (int i = 0; i < n_sents; i++) total += strlen(sents[i]) + 1;
        char *out = malloc(total + 32);
        if (out) {
            out[0] = '\0';
            size_t pos = 0;
            for (int i = 0; i < n_sents; i++) {
                size_t sl = strlen(sents[i]);
                if (pos + sl + 2 > (size_t)max_chars) break;
                memcpy(out + pos, sents[i], sl);
                pos += sl;
                out[pos++] = ' ';
            }
            out[pos] = '\0';
        }
        for (int i = 0; i < n_sents; i++) free(sents[i]);
        free(sents);
        return out;
    }

    /* Tokenize query */
    int n_qwords;
    char **qwords = tokenize_words(query ? query : "", &n_qwords);

    /* Score each sentence */
    scored_sentence_t *scored = malloc((size_t)n_sents * sizeof(scored_sentence_t));
    if (!scored) {
        free_words(qwords, n_qwords);
        for (int i = 0; i < n_sents; i++) free(sents[i]);
        free(sents);
        return NULL;
    }
    for (int i = 0; i < n_sents; i++) {
        scored[i].index = i;
        scored[i].score = score_sentence(sents[i], qwords, n_qwords);
        /* Boost first and last sentences (they often contain key info) */
        if (i == 0) scored[i].score += 0.3f;
        if (i == n_sents - 1) scored[i].score += 0.15f;
    }

    /* Sort by score descending, take top N */
    qsort(scored, (size_t)n_sents, sizeof(scored_sentence_t), cmp_scored_desc);
    int keep = max_sentences < n_sents ? max_sentences : n_sents;

    /* Re-sort the kept sentences by original index to preserve order */
    qsort(scored, (size_t)keep, sizeof(scored_sentence_t), cmp_index_asc);

    /* Build output */
    size_t out_cap = (size_t)max_chars + 64;
    char *out = malloc(out_cap);
    if (!out) {
        free(scored);
        free_words(qwords, n_qwords);
        for (int i = 0; i < n_sents; i++) free(sents[i]);
        free(sents);
        return NULL;
    }
    out[0] = '\0';
    size_t pos = 0;
    for (int i = 0; i < keep; i++) {
        const char *s = sents[scored[i].index];
        size_t sl = strlen(s);
        if (pos + sl + 2 > (size_t)max_chars) break;
        memcpy(out + pos, s, sl);
        pos += sl;
        out[pos++] = ' ';
    }
    if (pos > 0 && n_sents > keep) {
        const char *tag = "[...compressed]";
        size_t tl = strlen(tag);
        if (pos + tl < out_cap) {
            memcpy(out + pos, tag, tl);
            pos += tl;
        }
    }
    out[pos] = '\0';

    /* Cleanup */
    free(scored);
    free_words(qwords, n_qwords);
    for (int i = 0; i < n_sents; i++) free(sents[i]);
    free(sents);
    return out;
}
