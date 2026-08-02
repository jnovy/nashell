/* workspace.c — Two-layer memory segregation.
 *
 * Provides a thin routing layer over two memory_t instances:
 *   - global:    ~/.nash/memory/     (universal knowledge)
 *   - workspace: ~/.nash/workspaces/<name>/memory/  (project-specific)
 *
 * When no workspace is active, all operations go to global — backward
 * compatible with the pre-workspace architecture.
 *
 * Design rationale: memory_t is already self-contained (its own index,
 * mutex, git repo, embedding cache). We instantiate two and add routing. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include "workspace.h"
#include "nash_limits.h"
#include "str.h"

/* ── helpers ─────────────────────────────────────────── */

/* Reject workspace names containing path traversal sequences. */
static int ws_name_is_safe(const char *name) {
    if (!name || !*name || *name == '/') return 0;
    if (strstr(name, "..")) return 0;
    if (strchr(name, '\\')) return 0;
    return 1;
}

/* Recursive mkdir -p.  Creates all intermediate directories. */


/* Check if a key exists in a memory_t's index.
 * Uses memory_has_key() for O(1) hash lookup under the mutex,
 * replacing the old stat()-based approach that had filesystem I/O
 * and a race condition (no mutex protection). */
static int mem_has_key(memory_t *m, const char *key) {
    return memory_has_key(m, key);
}

/* ── lifecycle ───────────────────────────────────────── */

workspace_t *workspace_new(const char *nash_dir, const char *ws_name,
                           int isolated, double global_weight) {
    if (!nash_dir) return NULL;

    workspace_t *ws = calloc(1, sizeof(*ws));
    if (!ws) return NULL;

    ws->global_weight = (global_weight > 0.0) ? global_weight : 0.8;
    ws->isolated = isolated;

    /* Always create global memory */
    ws->global = memory_new(nash_dir);
    if (!ws->global) {
        free(ws);
        return NULL;
    }

    /* Create workspace memory if name is provided and safe */
    if (ws_name && ws_name[0] && ws_name_is_safe(ws_name)) {
        ws->name = strdup(ws_name);

        /* Build workspace root: ~/.nash/workspaces/<name>/ */
        char ws_root[NASH_PATH_MAX];
        snprintf(ws_root, sizeof(ws_root), "%s/workspaces/%s",
                 nash_dir, ws_name);
        mkdir_p(ws_root, 0755);

        ws->workspace = memory_new(ws_root);
        /* workspace memory is optional — if it fails, we still have global */
    }

    return ws;
}

void workspace_free(workspace_t *ws) {
    if (!ws) return;
    /* Free workspace first: its embed_ctx may share the ONNX session
     * owned by global's embed_ctx, so the borrower must go first. */
    memory_free(ws->workspace);
    memory_free(ws->global);
    free(ws->name);
    free(ws);
}

/* ── configuration ───────────────────────────────────── */

int workspace_init_embeddings(workspace_t *ws, const char *type,
                              const char *model, const char *api_base,
                              const char *model_path, int dimension,
                              int max_input_chars) {
    if (!ws) return 0;
    int r = 0;
    if (ws->global)
        r = memory_init_embeddings(ws->global, type, model, api_base,
                                   model_path, dimension, max_input_chars);
    if (ws->workspace) {
        /* Share the embedding backend from global memory instead of loading
         * the ONNX model (or probing the HTTP service) a second time. */
        embed_ctx_t *global_embed = r ? memory_embed_ctx(ws->global) : NULL;
        if (global_embed) {
            embed_ctx_t *shared = embed_share(global_embed);
            if (shared)
                memory_set_embed(ws->workspace, shared);
        } else {
            memory_init_embeddings(ws->workspace, type, model, api_base,
                                   model_path, dimension, max_input_chars);
        }
    }
    return r;
}

void workspace_set_recall_config(workspace_t *ws, double min_score,
                                 float blend_semantic, float blend_substring,
                                 float vscore_exp) {
    if (!ws) return;
    if (ws->global)
        memory_set_recall_config(ws->global, min_score,
                                 blend_semantic, blend_substring, vscore_exp);
    if (ws->workspace)
        memory_set_recall_config(ws->workspace, min_score,
                                 blend_semantic, blend_substring, vscore_exp);
}

/* ── recall (the core merging operation) ─────────────── */

/* Compare by descending relevance for qsort. */
static int cmp_relevance_desc(const void *a, const void *b) {
    const memory_entry_t *ea = (const memory_entry_t *)a;
    const memory_entry_t *eb = (const memory_entry_t *)b;
    if (eb->relevance > ea->relevance) return 1;
    if (eb->relevance < ea->relevance) return -1;
    return 0;
}

memory_results_t workspace_recall(workspace_t *ws, const char *query,
                                  int max_results) {
    memory_results_t merged = {0};
    if (!ws) return merged;

    /* If no workspace, just search global */
    if (!ws->workspace) {
        return memory_query(ws->global, query, max_results);
    }

    /* 1. Recall from workspace */
    memory_results_t ws_results = memory_query(ws->workspace, query, max_results);

    /* 2. If isolated, return workspace results only */
    if (ws->isolated) {
        return ws_results;
    }

    /* 3. Recall from global */
    memory_results_t gl_results = memory_query(ws->global, query, max_results);

    /* Apply weight discount to global results */
    for (int i = 0; i < gl_results.count; i++)
        gl_results.entries[i].relevance *= ws->global_weight;

    /* 4. Merge: combine into one array, sort by relevance, keep top max_results */
    int total = ws_results.count + gl_results.count;
    if (total == 0) {
        memory_results_free(&ws_results);
        memory_results_free(&gl_results);
        return merged;
    }

    merged.entries = calloc(total, sizeof(memory_entry_t));
    if (!merged.entries) {
        memory_results_free(&ws_results);
        memory_results_free(&gl_results);
        return merged;
    }

    /* Copy workspace results (they own the strings) */
    if (ws_results.count > 0) {
        memcpy(merged.entries, ws_results.entries,
               ws_results.count * sizeof(memory_entry_t));
    }
    /* Copy global results */
    if (gl_results.count > 0) {
        memcpy(merged.entries + ws_results.count, gl_results.entries,
               gl_results.count * sizeof(memory_entry_t));
    }
    merged.count = total;

    /* Null out the source arrays so memory_results_free doesn't double-free
     * the strings we just moved. The entries themselves are shallow-copied,
     * so the new merged array owns all the pointers. */
    free(ws_results.entries);
    ws_results.entries = NULL;
    ws_results.count = 0;
    free(gl_results.entries);
    gl_results.entries = NULL;
    gl_results.count = 0;

    /* Sort by relevance descending */
    qsort(merged.entries, merged.count, sizeof(memory_entry_t),
          cmp_relevance_desc);

    /* Deduplicate by key -- if the same key exists in both workspace and
     * global, both entries appear in the merged list. Keep the higher-scored
     * one (first after sort) and remove duplicates. */
    for (int i = 0; i < merged.count; i++) {
        if (!merged.entries[i].key) continue;
        for (int j = i + 1; j < merged.count; j++) {
            if (!merged.entries[j].key) continue;
            if (strcmp(merged.entries[i].key, merged.entries[j].key) == 0) {
                /* Free the duplicate (lower-scored) entry */
                free(merged.entries[j].key);
                free(merged.entries[j].value);
                free(merged.entries[j].description);
                free(merged.entries[j].journal_ref);
                free(merged.entries[j].supersedes);
                for (int r = 0; r < merged.entries[j].n_refs; r++)
                    free(merged.entries[j].refs[r]);
                free(merged.entries[j].refs);
                for (int t = 0; t < merged.entries[j].n_triggers; t++)
                    free(merged.entries[j].triggers[t]);
                free(merged.entries[j].triggers);
                /* Shift remaining entries down */
                memmove(&merged.entries[j], &merged.entries[j + 1],
                        (size_t)(merged.count - j - 1) * sizeof(memory_entry_t));
                merged.count--;
                j--;  /* re-check this index */
            }
        }
    }

    /* Cross-layer ref-boost: if a recalled entry has refs pointing to
     * another entry in the results, boost the referenced entry's score.
     * This was previously impossible because workspace and global were
     * scored in separate memory_query() calls. */
    for (int i = 0; i < merged.count; i++) {
        if (merged.entries[i].n_refs <= 0) continue;
        for (int ri = 0; ri < merged.entries[i].n_refs; ri++) {
            const char *ref_key = merged.entries[i].refs[ri];
            if (!ref_key) continue;
            for (int j = 0; j < merged.count; j++) {
                if (j == i || !merged.entries[j].key) continue;
                if (strcmp(merged.entries[j].key, ref_key) == 0) {
                    double boost = 0.3 * merged.entries[i].relevance;
                    merged.entries[j].relevance += boost;
                    if (merged.entries[j].relevance > 1.0)
                        merged.entries[j].relevance = 1.0;
                    break;
                }
            }
        }
    }
    /* Re-sort after ref-boost */
    qsort(merged.entries, merged.count, sizeof(memory_entry_t),
          cmp_relevance_desc);

    /* Truncate to max_results */
    if (merged.count > max_results) {
        /* Free excess entries */
        for (int i = max_results; i < merged.count; i++) {
            free(merged.entries[i].key);
            free(merged.entries[i].value);
            free(merged.entries[i].description);
            free(merged.entries[i].journal_ref);
            free(merged.entries[i].supersedes);
            if (merged.entries[i].tags) {
                for (int t = 0; t < merged.entries[i].n_tags; t++)
                    free(merged.entries[i].tags[t]);
                free(merged.entries[i].tags);
            }
            if (merged.entries[i].refs) {
                for (int r = 0; r < merged.entries[i].n_refs; r++)
                    free(merged.entries[i].refs[r]);
                free(merged.entries[i].refs);
            }
            if (merged.entries[i].triggers) {
                for (int t = 0; t < merged.entries[i].n_triggers; t++)
                    free(merged.entries[i].triggers[t]);
                free(merged.entries[i].triggers);
            }
        }
        merged.count = max_results;
    }

    return merged;
}

/* ── store routing ───────────────────────────────────── */

int workspace_store(workspace_t *ws, const char *key, const char *value,
                    int pinned, const char *journal_ref,
                    const char **refs, int n_refs,
                    const char **triggers, int n_triggers,
                    int force_global) {
    if (!ws) return -1;
    memory_t *target;
    if (force_global || !ws->workspace) {
        target = ws->global;
    } else {
        /* Route to existing layer if key already exists there, to prevent
         * cross-layer duplicates (e.g., updating a global lesson from a
         * workspace session would previously create a stale duplicate). */
        memory_t *existing = workspace_find_memory(ws, key);
        target = existing ? existing : ws->workspace;
    }
    return memory_store(target, key, value, pinned, journal_ref, refs, n_refs,
                        triggers, n_triggers);
}

/* ── find which memory contains a key ────────────────── */

memory_t *workspace_find_memory(workspace_t *ws, const char *key) {
    if (!ws || !key) return NULL;
    /* Check workspace first (more specific), then global */
    if (ws->workspace && mem_has_key(ws->workspace, key))
        return ws->workspace;
    if (ws->global && mem_has_key(ws->global, key))
        return ws->global;
    return NULL;
}

/* ── pin/unpin ───────────────────────────────────────── */

int workspace_pin(workspace_t *ws, const char *key) {
    memory_t *m = workspace_find_memory(ws, key);
    if (!m) return -1;
    return memory_pin(m, key);
}

int workspace_unpin(workspace_t *ws, const char *key) {
    memory_t *m = workspace_find_memory(ws, key);
    if (!m) return -1;
    return memory_unpin(m, key);
}

/* ── delete ──────────────────────────────────────────── */

int workspace_delete(workspace_t *ws, const char *key) {
    memory_t *m = workspace_find_memory(ws, key);
    if (!m) return -1;
    return memory_delete(m, key);
}

/* ── index & listing ─────────────────────────────────── */

char *workspace_build_index(workspace_t *ws) {
    if (!ws) return NULL;

    /* If no workspace, just build global index */
    if (!ws->workspace) {
        return memory_build_index(ws->global);
    }

    /* Build both and combine */
    char *g_idx = memory_build_index(ws->global);
    char *w_idx = memory_build_index(ws->workspace);

    if (!g_idx && !w_idx) return NULL;
    if (!w_idx) return g_idx;
    if (!g_idx) {
        /* Prefix with workspace label */
        str_t s = str_new(512);
        str_appendf(&s, "Workspace '%s': %s", ws->name ? ws->name : "default", w_idx);
        free(w_idx);
        return str_steal(&s);
    }

    str_t s = str_new(1024);
    str_appendf(&s, "%s\nWorkspace '%s': %s",
                g_idx, ws->name ? ws->name : "default", w_idx);
    free(g_idx);
    free(w_idx);
    return str_steal(&s);
}

char *workspace_build_listing(workspace_t *ws, const char *type_filter) {
    if (!ws) return NULL;

    if (!ws->workspace) {
        return memory_build_listing(ws->global, type_filter);
    }

    char *g_list = memory_build_listing(ws->global, type_filter);
    char *w_list = memory_build_listing(ws->workspace, type_filter);

    if (!g_list && !w_list) return NULL;

    str_t s = str_new(2048);
    if (g_list) {
        str_appendf(&s, "## Global Memory\n%s\n", g_list);
        free(g_list);
    }
    if (w_list) {
        str_appendf(&s, "## Workspace '%s'\n%s\n",
                    ws->name ? ws->name : "default", w_list);
        free(w_list);
    }
    return str_steal(&s);
}

/* ── pinned ──────────────────────────────────────────── */

char *workspace_load_pinned(workspace_t *ws) {
    if (!ws) return NULL;

    char *g_pinned = memory_load_pinned(ws->global);

    if (!ws->workspace) return g_pinned;

    char *w_pinned = memory_load_pinned(ws->workspace);

    if (!g_pinned && !w_pinned) return NULL;
    if (!w_pinned) return g_pinned;
    if (!g_pinned) return w_pinned;

    /* Concatenate: global pinned + workspace pinned */
    str_t s = str_new(strlen(g_pinned) + strlen(w_pinned) + 64);
    str_append_cstr(&s, g_pinned);
    str_append_cstr(&s, "\n");
    str_append_cstr(&s, w_pinned);
    free(g_pinned);
    free(w_pinned);
    return str_steal(&s);
}

/* ── prune ───────────────────────────────────────────── */

int workspace_prune(workspace_t *ws, double min_score, int min_evidence) {
    if (!ws) return 0;
    int total = 0;
    if (ws->global)
        total += memory_prune(ws->global, min_score, min_evidence);
    if (ws->workspace)
        total += memory_prune(ws->workspace, min_score, min_evidence);
    return total;
}

/* ── validation scoring ──────────────────────────────── */

int workspace_increment_hits(workspace_t *ws, const char *key) {
    memory_t *m = workspace_find_memory(ws, key);
    if (!m) return -1;
    return memory_increment_hits(m, key);
}

int workspace_increment_misses(workspace_t *ws, const char *key) {
    memory_t *m = workspace_find_memory(ws, key);
    if (!m) return -1;
    return memory_increment_misses(m, key);
}

int workspace_increment_access(workspace_t *ws, const char *key) {
    memory_t *m = workspace_find_memory(ws, key);
    if (!m) return -1;
    return memory_increment_access(m, key);
}

/* ── supersedes ──────────────────────────────────────── */

int workspace_set_supersedes(workspace_t *ws, const char *new_key,
                             const char *old_key) {
    memory_t *m = workspace_find_memory(ws, new_key);
    if (!m) return -1;
    return memory_set_supersedes(m, new_key, old_key);
}

/* ── belief entropy ──────────────────────────────────── */

int workspace_set_belief_entropy(workspace_t *ws, const char *key, double h_be) {
    memory_t *m = workspace_find_memory(ws, key);
    if (!m) return -1;
    return memory_set_belief_entropy(m, key, h_be);
}

/* ── git defer/flush ─────────────────────────────────── */

void workspace_git_defer(workspace_t *ws) {
    if (!ws) return;
    if (ws->global)    memory_git_defer(ws->global);
    if (ws->workspace) memory_git_defer(ws->workspace);
}

void workspace_git_flush(workspace_t *ws, const char *msg) {
    if (!ws) return;
    if (ws->global)    memory_git_flush(ws->global, msg);
    if (ws->workspace) memory_git_flush(ws->workspace, msg);
}

/* ── promote / demote ────────────────────────────────── */

/* Copy a JSON file and its .emb file from one memory dir to another,
 * then store the entry in the destination memory_t and delete from source. */
static int transfer_entry(memory_t *src, memory_t *dst, const char *key) {
    if (!src || !dst || !key) return -1;

    /* Read the full JSON entry from source */
    char fname[512];
    key_to_path(key, ".json", fname, sizeof(fname));

    char src_path[NASH_PATH_MAX], dst_path[NASH_PATH_MAX];
    snprintf(src_path, sizeof(src_path), "%s/%s", src->dir, fname);
    snprintf(dst_path, sizeof(dst_path), "%s/%s", dst->dir, fname);

    /* Read source JSON file and write to destination */
    size_t sz = 0;
    char *data = slurp_file(src_path, &sz);
    if (!data) return -1;
    if (write_file(dst_path, data, sz) != 0) { free(data); return -1; }
    free(data);

    /* Copy .emb file if it exists */
    char emb_fname[512];
    key_to_path(key, ".emb", emb_fname, sizeof(emb_fname));
    char src_emb[NASH_PATH_MAX], dst_emb[NASH_PATH_MAX];
    snprintf(src_emb, sizeof(src_emb), "%s/%s", src->dir, emb_fname);
    snprintf(dst_emb, sizeof(dst_emb), "%s/%s", dst->dir, emb_fname);

    size_t esz = 0;
    void *edata = slurp_file_binary(src_emb, &esz);
    if (edata) {
        FILE *ef = fopen(dst_emb, "wb");
        if (ef) {
            fwrite(edata, 1, esz, ef);
            fclose(ef);
        }
        free(edata);
    }

    /* Delete from source (updates source index + git) */
    memory_delete(src, key);

    /* FIX #8: Use memory_reindex_entry() instead of memory_store().
     * memory_store() would overwrite the copied JSON (losing created_at,
     * recall_hits, etc.) and regenerate the embedding we already copied.
     * memory_reindex_entry() just reads the file and updates the index. */
    memory_reindex_entry(dst, key);

    return 0;
}

int workspace_promote(workspace_t *ws, const char *key) {
    if (!ws || !ws->workspace || !key) return -1;
    if (!mem_has_key(ws->workspace, key)) return -1;
    return transfer_entry(ws->workspace, ws->global, key);
}

int workspace_demote(workspace_t *ws, const char *key) {
    if (!ws || !ws->workspace || !key) return -1;
    if (!mem_has_key(ws->global, key)) return -1;
    return transfer_entry(ws->global, ws->workspace, key);
}

/* ── workspace listing ──────────────────────────────── */

/* Recursively scan a directory for workspaces (dirs containing memory/).
 * prefix is the relative path from the workspaces root (empty string at top). */
static void list_workspaces_recurse(const char *base, const char *prefix,
                                    int *count) {
    char dirpath[NASH_PATH_MAX];
    if (prefix[0])
        snprintf(dirpath, sizeof(dirpath), "%s/%s", base, prefix);
    else
        snprintf(dirpath, sizeof(dirpath), "%s", base);

    DIR *d = opendir(dirpath);
    if (!d) return;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;

        char fullpath[NASH_PATH_MAX];
        if (snprintf(fullpath, sizeof(fullpath), "%s/%s", dirpath, ent->d_name)
                >= (int)sizeof(fullpath))
            continue;

        struct stat st;
        if (stat(fullpath, &st) != 0 || !S_ISDIR(st.st_mode))
            continue;

        /* Skip internal subdirs - only descend into potential workspace trees */
        if (strcmp(ent->d_name, "memory") == 0 ||
            strcmp(ent->d_name, "sessions") == 0)
            continue;

        /* Build relative name */
        char relname[NASH_PATH_MAX];
        if (prefix[0])
            snprintf(relname, sizeof(relname), "%s/%s", prefix, ent->d_name);
        else
            snprintf(relname, sizeof(relname), "%s", ent->d_name);

        /* Check if this dir has a memory/ subdirectory - that makes it a workspace */
        char mempath[NASH_PATH_MAX];
        if (snprintf(mempath, sizeof(mempath), "%s/memory", fullpath)
                < (int)sizeof(mempath)) {
            struct stat mst;
            if (stat(mempath, &mst) == 0 && S_ISDIR(mst.st_mode)) {
                printf("  %s\n", relname);
                (*count)++;
            }
        }

        /* Recurse into subdirectories for nested workspaces (e.g. rh/container-tools) */
        list_workspaces_recurse(base, relname, count);
    }
    closedir(d);
}

void workspace_list_all(const char *nash_dir) {
    if (!nash_dir) {
        fprintf(stderr, "error: nash data directory not set\n");
        return;
    }

    char ws_root[NASH_PATH_MAX];
    snprintf(ws_root, sizeof(ws_root), "%s/workspaces", nash_dir);

    struct stat st;
    if (stat(ws_root, &st) != 0 || !S_ISDIR(st.st_mode)) {
        printf("No workspaces found.\n");
        return;
    }

    printf("Available workspaces:\n");
    int count = 0;
    list_workspaces_recurse(ws_root, "", &count);

    if (count == 0)
        printf("  (none)\n");
    printf("\nTotal: %d workspace%s\n", count, count == 1 ? "" : "s");
    printf("Use: nash -w NAME to activate a workspace\n");
}
