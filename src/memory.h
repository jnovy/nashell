#ifndef MEMORY_H
#define MEMORY_H

#include <pthread.h>
#include <stdatomic.h>
#include "cJSON.h"
#include "embedding.h"

/* Long-term memory: persistent knowledge across sessions.
 * Stored as individual JSON files in .memory/ directory.
 * Types: lesson:*, strategy:*, task:*, fact:* */

/* P1: In-memory index cache — eliminates O(n) file I/O per recall.
 *
 * Research basis:
 *   AutoMEM [arXiv:2606.04315, Jun 2026] — cross-scenario evaluation
 *     showed self-managed memory with active control beats all passive
 *     retrieval pipelines. Index enables the agent to browse memory
 *     structure without I/O.
 *   MRAgent [arXiv:2606.06036, ICML 2026] — Cue-Tag-Content graph
 *     with associative tags. Our index serves as the "cue" layer,
 *     enabling fast navigation before loading full content.
 *   Letta Context Repositories [May 2026] — progressive disclosure
 *     via filetree structure always in system prompt. Index provides
 *     the equivalent navigational signal.
 *
 * Loaded once at memory_new(), updated incrementally on store/delete.
 * memory_recall() iterates the in-memory array instead of scanning
 * the filesystem, reducing recall from O(n) file reads to O(n) array
 * scan + O(k) file reads for top-k results only. */
typedef struct {
    char  *key;           /* memory key (owned) */
    char  *description;   /* first sentence/line of value (owned, ≤250 chars) */
    char  *value;         /* full value text (owned) */
    int    pinned;
    int    access_count;
    int    recall_hits;
    int    recall_misses;
    double belief_entropy;
    double created_at;
    char **refs;          /* inter-memory ref keys (owned) */
    int    n_refs;
    embed_multi_vec_t emb; /* cached embedding (loaded once) */
    int    has_emb;        /* 1 if emb is valid */
    char  *path;           /* full path to .json file (owned) */
} mem_index_entry_t;

/* FIX 2a: Hash map for O(1) key→index lookup (open-addressing, linear probing).
 * Without this, mem_index_find() was O(n) per call, making batch operations
 * during dream consolidation O(n²). */
typedef struct {
    int *slots;       /* maps hash bucket → entries[] index, -1 = empty */
    int cap;          /* capacity (always power of 2) */
} mem_index_map_t;

typedef struct {
    mem_index_entry_t *entries;
    int count;
    int cap;
    mem_index_map_t map;
} mem_index_t;

typedef struct {
    char *dir;          /* .memory/ directory path */
    char *model;        /* model name for commit signoff (e.g. "claude-sonnet-4-20250514") */
    embed_ctx_t *embed; /* embedding context for semantic matching (NULL = disabled) */

    /* Recall tuning parameters — set once via memory_set_recall_config().
     * Centralizes the config→memory sync (was 4 manual copies in main.c). */
    double recall_min_score;       /* min composite score for injection (default 0.25) */
    float recall_blend_semantic;   /* semantic weight (default 0.7) */
    float recall_blend_substring;  /* substring weight (default 0.3) */
    float vscore_exponent;         /* Bayesian vscore exponent (default 0.3, 0.0=disabled) */

    /* Guard against recursive consolidation — set during
     * memory_try_consolidate to prevent consolidation→store→consolidation loops.
     * FIX B2: atomic to prevent data race between consolidation and store. */
    atomic_int consolidating;

    /* Deferred git commits — when git_deferred > 0, memory_git_commit()
     * is skipped and git_deferred_count is incremented. Call
     * memory_git_flush() to batch-commit all deferred changes. */
    int git_deferred;
    int git_deferred_count;

    /* Thread safety: protects idx and all index-dependent operations.
     * The inference thread (react_run → tools) and the main thread
     * (/memory_recall command) can both access memory concurrently.
     * All public memory_*() functions acquire this lock internally. */
    pthread_mutex_t mtx;

    /* P1: In-memory index — populated by memory_new(), updated by
     * memory_store()/memory_delete(). Used by memory_recall() and
     * memory_build_index() to avoid filesystem scans. */
    mem_index_t idx;
} memory_t;

typedef struct {
    char  *key;           /* e.g. "lesson:redis-v7-changes" */
    char  *value;         /* the knowledge text */
    /* P6: Auto-generated description — first sentence or first 150 chars
     * of value. Enables progressive disclosure (P2) where the agent sees
     * key + description in the memory index without loading full content.
     *
     * Research basis:
     *   Letta Context Repositories [May 2026] — each memory file includes
     *     frontmatter with a description, similar to YAML frontmatter in
     *     Anthropic's SKILL.md files.
     *   Claude Code Auto Memory [2026] — MEMORY.md index file with topic
     *     descriptions enabling selective loading.
     *   AutoMEM [arXiv:2606.04315, Jun 2026] — self-managed flat text-file
     *     storage via tool calls achieves best cross-task ranking when
     *     agents can browse descriptions before loading full content. */
    char  *description;   /* first sentence/line of value (≤250 chars) */
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
    double belief_entropy; /* ℋ_BE — forward-looking quality signal (MMPO). -1 = not computed */

    /* P2: Lesson lineage tracking — Self-Harness harness lineage h₀→h₁→h₂.
     * When a new lesson supersedes an old one, record the superseded key.
     * This creates a chain: the agent can trace how its understanding evolved.
     * Based on: Self-Harness [arXiv:2606.09498, Jun 2026] — harness lineage
     *
     * Example: lesson:file-edit-v2 supersedes lesson:file-edit-v1
     * The old entry is NOT deleted — it becomes inactive (low relevance via
     * validation scoring) while the new one takes over. The chain preserves
     * the full evolution history for retrospective analysis. */
    char  *supersedes;    /* key of the memory this entry supersedes (NULL = none) */
    int    version;       /* lineage version number (1 = original, 2+ = superseding) */
} memory_entry_t;

typedef struct {
    memory_entry_t *entries;
    int count;
} memory_results_t;

/* Create/free memory store (creates .memory/ directory) */
memory_t *memory_new(const char *project_root);
void      memory_free(memory_t *m);

/* Set recall tuning parameters from config. Centralizes the config→memory
 * sync — call once after config_apply_profile() and config_set_defaults(). */
void memory_set_recall_config(memory_t *m, double min_score,
                              float blend_semantic, float blend_substring,
                              float vscore_exp);

/* Convert a memory key to a filesystem path component.
 * Replaces ':' and '/' with '_', appends ext (e.g. ".json").
 * Result is written to out (max out_sz bytes). */
void key_to_path(const char *key, const char *ext, char *out, size_t out_sz);

/* Store a memory entry (creates/overwrites .memory/<key>.json).
 * journal_ref: provenance pointer to the session journal where this memory
 * was created (e.g. "/path/to/session/journal.jsonl:R5"). NULL = no ref.
 * The dreaming LLM can read this journal to understand the original context. */
int memory_store(memory_t *m, const char *key, const char *value,
                 int pinned, const char *journal_ref,
                 const char **refs, int n_refs);

/* Pin an existing memory (set pinned=true). Returns 0 on success, -1 if not found. */
int memory_pin(memory_t *m, const char *key);

/* Unpin an existing memory (set pinned=false). Returns 0 on success, -1 if not found. */
int memory_unpin(memory_t *m, const char *key);

/* Search memories by query (substring match on key + value).
 * Returns up to max_results matches, sorted by relevance.
 * Caller must free with memory_results_free(). */
memory_results_t memory_recall(memory_t *m, const char *query, int max_results);

/* Build a compact memory summary (counts by type only).
 * Format: "Memory: N entries (X lessons, Y strategies, Z skills, ...)"
 * Caller must free. Returns NULL if no memories. */
char *memory_build_index(memory_t *m);

/* Build a full listing of all memory keys grouped by type.
 * type_filter: if non-NULL/non-empty, only show entries matching that type
 * (e.g. "lesson", "strategy", "skill", "fact", "task", "other").
 * Caller must free. Returns NULL if no matching memories. */
char *memory_build_listing(memory_t *m, const char *type_filter);

/* Load all pinned memories and return their values concatenated.
 * Format: "[PINNED: key1]\nvalue1\n\n[PINNED: key2]\nvalue2\n..."
 * Caller must free. Returns NULL if no pinned memories. */
char *memory_load_pinned(memory_t *m);

/* Delete a memory entry by key. Removes .json and .emb files.
 * Returns 0 on success, -1 if not found. */
int memory_delete(memory_t *m, const char *key);

/* Batch delete: delete multiple keys with a single gc_refs pass and
 * a single git commit.  Reduces O(K×N) to O(K+N) for K deletes across
 * N remaining entries.  Used by memory_prune() and playbook fact cleanup.
 * Returns the number of entries actually deleted (keys that existed). */
int memory_delete_batch(memory_t *m, const char **keys, int n_keys);

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

/* P2: Set supersedes field on a memory entry.
 * Creates a lineage chain: new_key supersedes old_key.
 * Sets version = old_version + 1 on the new entry.
 * Returns 0 on success, -1 if new_key not found. */
int memory_set_supersedes(memory_t *m, const char *new_key, const char *old_key);

/* Add validation evidence to a memory's in-memory index entry.
 * Used by consolidation to carry forward recall_hits/misses from
 * deleted entries to their survivors, so merged entries don't lose
 * credibility.  Updates ONLY the in-memory index (disk JSON is
 * updated separately by consolidation_carry_scores).
 * Returns 0 on success, -1 if key not found. */
int memory_update_scores(memory_t *m, const char *key,
                         int add_hits, int add_misses);

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
int memory_embed_entry(memory_t *m, const char *key, const char *value);

/* Re-embed all memory entries that don't have .emb files.
 * Useful after enabling embeddings on an existing memory store.
 * Returns number of entries embedded. */
int memory_embed_all(memory_t *m);

/* Deferred git commits for batch operations (e.g., dreaming/consolidation).
 * Call memory_git_defer(m) before a batch, then memory_git_flush(m) after.
 * While deferred, individual store/delete/pin operations skip git commits.
 * memory_git_flush() does a single `git add -A && git commit` for all changes. */
void memory_git_defer(memory_t *m);
void memory_git_flush(memory_t *m, const char *msg);

#endif
