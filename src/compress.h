#ifndef COMPRESS_H
#define COMPRESS_H

#include <stddef.h>
#include <stdint.h>

/* Line-level relevance compression — Harness-1 §3.1 (Line-BM25).
 * Splits text into content-agnostic chunks (lines with short-line
 * merging), scores each by term overlap with the query, and returns
 * the top-N most relevant chunks concatenated.  Works for code,
 * prose, JSON, YAML, logs, diffs — no content-type detection needed.
 *
 * See: arXiv 2606.02373 "Harness-1: RL for Search Agents with
 * State-Externalizing Harnesses" — §3.1 compression.
 *
 * text:       input text to compress
 * query:      relevance query (user query + current thought)
 * max_units:  maximum chunks to retain (floor: 12)
 * max_chars:  hard character limit on output (floor: 800)
 * Returns:    malloc'd compressed string (caller frees), or NULL
 */
char *compress_to_relevant(const char *text, const char *query,
                           int max_units, int max_chars);

/* CRC32 hash for content deduplication (Harness-1 §3.3).
 * Simple, fast, good enough for near-duplicate detection. */
uint32_t compress_crc32(const char *data, size_t len);

/* Check if two CRC32 hashes represent likely-duplicate content.
 * Returns 1 if duplicate, 0 otherwise. */
int compress_is_duplicate(uint32_t hash, const uint32_t *hash_buf,
                          int hash_count);

#endif /* COMPRESS_H */
