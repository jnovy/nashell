#ifndef MEMORY_H
#define MEMORY_H

#include "cJSON.h"

/* Long-term memory: persistent knowledge across sessions.
 * Stored as individual JSON files in .memory/ directory.
 * Types: lesson:*, strategy:*, task:*, fact:* */

typedef struct {
    char *dir;   /* .memory/ directory path */
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
} memory_entry_t;

typedef struct {
    memory_entry_t *entries;
    int count;
} memory_results_t;

/* Create/free memory store (creates .memory/ directory) */
memory_t *memory_new(const char *project_root);
void      memory_free(memory_t *m);

/* Store a memory entry (creates/overwrites .memory/<key>.json) */
int memory_store(memory_t *m, const char *key, const char *value,
                 const char **tags, int n_tags, int pinned);

/* Search memories by query (substring match on key + value + tags).
 * Returns up to max_results matches, sorted by relevance.
 * Caller must free with memory_results_free(). */
memory_results_t memory_recall(memory_t *m, const char *query, int max_results);

/* Build a compact index of all memory keys+tags for system prompt injection.
 * Format: "  key1 [tag1, tag2]\n  key2 [tag3]\n..."
 * Caller must free. Returns NULL if no memories. */
char *memory_build_index(memory_t *m);

/* Load all pinned memories and return their values concatenated.
 * Format: "[PINNED: key1]\nvalue1\n\n[PINNED: key2]\nvalue2\n..."
 * Caller must free. Returns NULL if no pinned memories. */
char *memory_load_pinned(memory_t *m);

/* Free a memory_results_t */
void memory_results_free(memory_results_t *r);

/* Prune stale, low-value memories.
 * Deletes entries older than max_age_days with access_count < min_access_count.
 * Never prunes pinned memories, strategies, or lessons.
 * Returns number of entries pruned. */
int memory_prune(memory_t *m, int max_age_days, int min_access_count);

#endif
