#ifndef WORKSPACE_H
#define WORKSPACE_H

#include "memory.h"

/* Workspace: two-layer memory segregation.
 *
 * Global layer (~/.nash/.memory/): universal skills, lessons, strategies.
 * Workspace layer (~/.nash/workspaces/<name>/.memory/): project-specific facts.
 *
 * When no workspace is active, behaves identically to the old single-memory
 * architecture — all operations go to global. */

typedef struct {
    memory_t *global;        /* ~/.nash/.memory/ — always non-NULL */
    memory_t *workspace;     /* ~/.nash/workspaces/<name>/.memory/ — NULL if none */
    char     *name;          /* workspace name (NULL = global-only mode) */
    int       isolated;      /* if true, skip global on recall */
    double    global_weight; /* score multiplier for global results (default 0.8) */
} workspace_t;

/* Create a workspace.
 * nash_dir:      base directory (e.g. ~/.nash)
 * ws_name:       workspace name (NULL = global-only mode)
 * isolated:      1 = skip global on recall
 * global_weight: score discount for global results (0.0–1.0, default 0.8)
 *
 * Always creates the global memory_t. Creates workspace memory_t only
 * if ws_name is non-NULL and non-empty. */
workspace_t *workspace_new(const char *nash_dir, const char *ws_name,
                           int isolated, double global_weight);

/* Free a workspace and both memory_t instances. */
void workspace_free(workspace_t *ws);

/* Initialize embeddings on all memory_t instances in the workspace. */
int workspace_init_embeddings(workspace_t *ws, const char *type,
                              const char *model, const char *api_base,
                              const char *model_path, int dimension,
                              int max_input_chars);

/* Set recall config on all memory_t instances in the workspace. */
void workspace_set_recall_config(workspace_t *ws, double min_score,
                                 float blend_semantic, float blend_substring,
                                 float vscore_exp);

/* Recall: search workspace memory first, then merge global results
 * (with global_weight discount). Returns unified sorted results.
 * If no workspace is active, just searches global. */
memory_results_t workspace_recall(workspace_t *ws, const char *query,
                                  int max_results);

/* Store: routes to workspace memory by default, global if force_global
 * is set or no workspace is active. */
int workspace_store(workspace_t *ws, const char *key, const char *value,
                    int pinned, const char *journal_ref,
                    const char **refs, int n_refs,
                    const char **triggers, int n_triggers,
                    int force_global);

/* Pin/unpin: operates on the memory that contains the key.
 * Searches workspace first, then global. */
int workspace_pin(workspace_t *ws, const char *key);
int workspace_unpin(workspace_t *ws, const char *key);

/* Delete: removes from whichever memory contains the key.
 * Searches workspace first, then global. */
int workspace_delete(workspace_t *ws, const char *key);

/* Build combined memory index summary (counts from both layers). */
char *workspace_build_index(workspace_t *ws);

/* Build combined listing of memory keys, labeled by layer.
 * type_filter: NULL = all types. */
char *workspace_build_listing(workspace_t *ws, const char *type_filter);

/* Load pinned memories from both layers (workspace + global). */
char *workspace_load_pinned(workspace_t *ws);

/* Prune low-value memories from both layers. Returns total pruned. */
int workspace_prune(workspace_t *ws, double min_score, int min_evidence);

/* Increment hits/misses: searches workspace first, then global. */
int workspace_increment_hits(workspace_t *ws, const char *key);
int workspace_increment_misses(workspace_t *ws, const char *key);
int workspace_increment_access(workspace_t *ws, const char *key);

/* Set supersedes on the memory that contains new_key. */
int workspace_set_supersedes(workspace_t *ws, const char *new_key,
                             const char *old_key);

/* Set belief entropy on the memory that contains key. */
int workspace_set_belief_entropy(workspace_t *ws, const char *key, double h_be);

/* Deferred git: defer/flush on both layers. */
void workspace_git_defer(workspace_t *ws);
void workspace_git_flush(workspace_t *ws, const char *msg);

/* Promote: move a key from workspace → global.
 * Returns 0 on success, -1 if key not found in workspace. */
int workspace_promote(workspace_t *ws, const char *key);

/* Demote: move a key from global → workspace.
 * Returns 0 on success, -1 if key not found or no workspace active. */
int workspace_demote(workspace_t *ws, const char *key);

/* Find which memory_t contains a key.
 * Searches workspace first, then global. Returns NULL if not found. */
memory_t *workspace_find_memory(workspace_t *ws, const char *key);

#endif
