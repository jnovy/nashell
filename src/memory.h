#ifndef MEMORY_H
#define MEMORY_H

#include "cJSON.h"
#include "embedding.h"

/* Long-term memory: persistent knowledge across sessions.
 * Stored as individual JSON files in .memory/ directory.
 * Types: lesson:*, strategy:*, task:*, fact:* */

typedef struct {
    char *dir;          /* .memory/ directory path */
    char *model;        /* model name for commit signoff (e.g. "claude-sonnet-4-20250514") */
    embed_ctx_t *embed; /* embedding context for semantic matching (NULL = disabled) */

    /* P0: Score threshold for recall injection (abstention gate).
     * Memories with composite score below this value are excluded from
     * recall results, implementing the "abstention" pattern from:
     *   Mem-π [arXiv:2605.21463] — learned abstention yields +22% avg
     *   MemFail [arXiv:2605.26667] — weak injection hurts performance
     * Default: 0.15 (set from config.recall_min_score) */
    double recall_min_score;
} memory_t;

typedef struct {
    char  *key;           /* e.g. "lesson:redis-v7-changes" */
    char  *value;         /* the knowledge text */
    char **tags;          /* array of tag strings */
    int    n_tags;
    int    pinned;        /* 1 = always inject into system prompt */
    double created_at;    /* unix epoch */
    double last_accessed; /* unix epoch */
    int    access_count;
    int    recall_hits;   /* recalled during tasks that SUCCEEDED */
    int    recall_misses; /* recalled during tasks that FAILED */
    char  *journal_ref;   /* provenance: "session/journal.jsonl:R5" */

    /* Inter-memory relationships — "see also" links between related memories.
     * Populated by dreaming's SYNTHESIZE pass to create a lightweight graph
     * structure without requiring a full graph database.
     *
     * Research basis:
     *   MemForest [arXiv:2605.23986, May 2026] — hierarchical temporal trees
     *     with parent-child relationships between memory nodes.
     *   ActiveGraph [arXiv:2605.21997, May 2026] — typed edges between nodes
     *     in a reactive graph where relationships ARE the memory structure.
     *   MemIR [arXiv:2605.25869, May 2026] — provenance chains linking raw
     *     evidence to claims via typed intermediate representation.
     *
     * During recall, if a high-scoring memory has refs, the ref'd memories
     * receive a score boost (+0.5), implementing associative retrieval. */
    char **refs;          /* array of related memory keys */
    int    n_refs;
    double relevance;     /* final composite score from last memory_recall */
    double raw_relevance; /* semantic+substring blend [0,1] before importance/vscore */
    double importance;    /* log access frequency [0,1] */
} memory_entry_t;

typedef struct {
    memory_entry_t *entries;
    int count;
} memory_results_t;

/* Create/free memory store (creates .memory/ directory) */
memory_t *memory_new(const char *project_root);
void      memory_free(memory_t *m);

/* Store a memory entry (creates/overwrites .memory/<key>.json).
 * journal_ref: provenance pointer to the session journal where this memory
 * was created (e.g. "/path/to/session/journal.jsonl:R5"). NULL = no ref.
 * The dreaming LLM can read this journal to understand the original context. */
int memory_store(memory_t *m, const char *key, const char *value,
                 const char **tags, int n_tags, int pinned,
                 const char *journal_ref,
                 const char **refs, int n_refs);

/* Pin an existing memory (set pinned=true). Returns 0 on success, -1 if not found. */
int memory_pin(memory_t *m, const char *key);

/* Unpin an existing memory (set pinned=false). Returns 0 on success, -1 if not found. */
int memory_unpin(memory_t *m, const char *key);

/* Search memories by query (substring match on key + value + tags).
 * Returns up to max_results matches, sorted by relevance.
 * Caller must free with memory_results_free(). */
memory_results_t memory_recall(memory_t *m, const char *query, int max_results);

/* Build a compact index of all memory keys+tags for system prompt injection.
 * Format: "  key1 [tag1, tag2]\n  key2 [tag3]\n..."
 * Caller must free. Returns NULL if no memories. */
char *memory_build_index(memory_t *m, int max_entries);

/* Load all pinned memories and return their values concatenated.
 * Format: "[PINNED: key1]\nvalue1\n\n[PINNED: key2]\nvalue2\n..."
 * Caller must free. Returns NULL if no pinned memories. */
char *memory_load_pinned(memory_t *m);

/* Delete a memory entry by key. Removes .json and .emb files.
 * Returns 0 on success, -1 if not found. */
int memory_delete(memory_t *m, const char *key);

/* Free a memory_results_t */
void memory_results_free(memory_results_t *r);

/* Prune low-value memories based on Bayesian validation scoring.
 * Deletes entries with validation score < 0.35 and sufficient evidence (≥3
 * recalls). Age is NOT a pruning criterion — a year-old lesson with no
 * evidence is unknown (score 0.50), not worthless.
 * Pinned memories are never pruned.
 * Returns number of entries pruned. */
int memory_prune(memory_t *m, double min_score, int min_evidence);

/* Increment recall_hits (task succeeded) or recall_misses (task failed)
 * for a memory entry identified by key. Returns 0 on success, -1 if not found.
 * Validation score = (hits+1)/(hits+misses+2) — Beta posterior mean. */
int memory_increment_hits(memory_t *m, const char *key);
int memory_increment_misses(memory_t *m, const char *key);

/* Initialize embedding context for semantic memory matching.
 * Call after memory_new(). Probes the embedding backend and sets
 * m->embed if available. No-op if cfg->type is "none" or NULL.
 * For ONNX: model_path = directory with onnx/model.onnx + vocab.txt.
 * For HTTP backends: model = model name, api_base = server URL.
 * Returns 1 if embeddings are available, 0 otherwise. */
int memory_init_embeddings(memory_t *m, const char *type,
                           const char *model, const char *api_base,
                           const char *model_path, int dimension,
                           int max_input_chars);

/* Generate and save embedding for a memory entry.
 * Called automatically by memory_store when embeddings are enabled.
 * Saves to .memory/<key>.emb alongside the .json file.
 * Returns 0 on success, -1 on failure. */
int memory_embed_entry(memory_t *m, const char *key, const char *value,
                       const char **tags, int n_tags);

/* Re-embed all memory entries that don't have .emb files.
 * Useful after enabling embeddings on an existing memory store.
 * Returns number of entries embedded. */
int memory_embed_all(memory_t *m);

#endif
