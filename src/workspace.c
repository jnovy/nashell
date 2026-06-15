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
#include <sys/stat.h>
#include "workspace.h"
#include "str.h"

/* ── helpers ─────────────────────────────────────────── */

/* Recursive mkdir -p.  Creates all intermediate directories. */
static void mkdirp(const char *path) {
    char tmp[4096];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
}

/* Check if a key exists in a memory_t's index. */
static int mem_has_key(memory_t *m, const char *key) {
    if (!m || !key) return 0;
    /* Use memory_recall with max_results=1 as a quick existence check?
     * No — we need exact key match.  Check the index directly via
     * a zero-length recall that just checks the hash map.
     * Actually, the simplest approach: try to load the entry JSON. */
    char fname[512];
    key_to_path(key, ".json", fname, sizeof(fname));
    char path[4096];
    snprintf(path, sizeof(path), "%s/%s", m->dir, fname);
    struct stat st;
    return (stat(path, &st) == 0);
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

    /* Create workspace memory if name is provided */
    if (ws_name && ws_name[0]) {
        ws->name = strdup(ws_name);

        /* Build workspace root: ~/.nash/workspaces/<name>/ */
        char ws_root[4096];
        snprintf(ws_root, sizeof(ws_root), "%s/workspaces/%s",
                 nash_dir, ws_name);
        mkdirp(ws_root);

        ws->workspace = memory_new(ws_root);
        /* workspace memory is optional — if it fails, we still have global */
    }

    return ws;
}

void workspace_free(workspace_t *ws) {
    if (!ws) return;
    memory_free(ws->global);
    memory_free(ws->workspace);
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
    if (ws->workspace)
        memory_init_embeddings(ws->workspace, type, model, api_base,
                               model_path, dimension, max_input_chars);
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
        return memory_recall(ws->global, query, max_results);
    }

    /* 1. Recall from workspace */
    memory_results_t ws_results = memory_recall(ws->workspace, query, max_results);

    /* 2. If isolated, return workspace results only */
    if (ws->isolated) {
        return ws_results;
    }

    /* 3. Recall from global */
    memory_results_t gl_results = memory_recall(ws->global, query, max_results);

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
        }
        merged.count = max_results;
    }

    return merged;
}

/* ── store routing ───────────────────────────────────── */

int workspace_store(workspace_t *ws, const char *key, const char *value,
                    int pinned, const char *journal_ref,
                    const char **refs, int n_refs, int force_global) {
    if (!ws) return -1;
    memory_t *target = (force_global || !ws->workspace)
                       ? ws->global : ws->workspace;
    return memory_store(target, key, value, pinned, journal_ref, refs, n_refs);
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

/* ── supersedes ──────────────────────────────────────── */

int workspace_set_supersedes(workspace_t *ws, const char *new_key,
                             const char *old_key) {
    memory_t *m = workspace_find_memory(ws, new_key);
    if (!m) return -1;
    return memory_set_supersedes(m, new_key, old_key);
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

    char src_path[4096], dst_path[4096];
    snprintf(src_path, sizeof(src_path), "%s/%s", src->dir, fname);
    snprintf(dst_path, sizeof(dst_path), "%s/%s", dst->dir, fname);

    /* Read source JSON file */
    FILE *f = fopen(src_path, "r");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *data = malloc(sz + 1);
    if (!data) { fclose(f); return -1; }
    fread(data, 1, sz, f);
    data[sz] = '\0';
    fclose(f);

    /* Write to destination */
    f = fopen(dst_path, "w");
    if (!f) { free(data); return -1; }
    fwrite(data, 1, sz, f);
    fclose(f);
    free(data);

    /* Copy .emb file if it exists */
    char emb_fname[512];
    key_to_path(key, ".emb", emb_fname, sizeof(emb_fname));
    char src_emb[4096], dst_emb[4096];
    snprintf(src_emb, sizeof(src_emb), "%s/%s", src->dir, emb_fname);
    snprintf(dst_emb, sizeof(dst_emb), "%s/%s", dst->dir, emb_fname);

    f = fopen(src_emb, "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        long esz = ftell(f);
        fseek(f, 0, SEEK_SET);
        char *edata = malloc(esz);
        if (edata) {
            fread(edata, 1, esz, f);
            fclose(f);
            FILE *ef = fopen(dst_emb, "wb");
            if (ef) {
                fwrite(edata, 1, esz, ef);
                fclose(ef);
            }
            free(edata);
        } else {
            fclose(f);
        }
    }

    /* Delete from source (updates source index + git) */
    memory_delete(src, key);

    /* Rebuild destination index entry by re-reading the JSON.
     * The simplest way: call memory_store with the entry's value,
     * but that would overwrite metadata.  Instead, we force a
     * re-index by freeing and re-creating the destination memory.
     * Actually, we can just do a targeted re-index:
     * Since we wrote the JSON file directly, we need to update
     * the destination's in-memory index. The cleanest way is to
     * read the value and store through the normal API. */

    /* Read the value from the copied JSON */
    cJSON *entry = NULL;
    {
        FILE *jf = fopen(dst_path, "r");
        if (jf) {
            fseek(jf, 0, SEEK_END);
            long jsz = ftell(jf);
            fseek(jf, 0, SEEK_SET);
            char *jdata = malloc(jsz + 1);
            if (jdata) {
                fread(jdata, 1, jsz, jf);
                jdata[jsz] = '\0';
                entry = cJSON_Parse(jdata);
                free(jdata);
            }
            fclose(jf);
        }
    }

    if (entry) {
        /* Extract value and pinned from JSON, store via API.
         * This properly updates the in-memory index. */
        cJSON *val_j = cJSON_GetObjectItem(entry, "value");
        cJSON *pin_j = cJSON_GetObjectItem(entry, "pinned");
        const char *value = val_j ? val_j->valuestring : "";
        int pinned = (pin_j && cJSON_IsTrue(pin_j)) ? 1 : 0;

        /* Extract refs */
        cJSON *refs_j = cJSON_GetObjectItem(entry, "refs");
        int n_refs = 0;
        const char **refs = NULL;
        if (refs_j && cJSON_IsArray(refs_j)) {
            n_refs = cJSON_GetArraySize(refs_j);
            if (n_refs > 0) {
                refs = malloc(n_refs * sizeof(char *));
                for (int i = 0; i < n_refs; i++) {
                    cJSON *r = cJSON_GetArrayItem(refs_j, i);
                    refs[i] = r ? r->valuestring : "";
                }
            }
        }

        memory_store(dst, key, value, pinned, NULL, refs, n_refs);
        free(refs);
        cJSON_Delete(entry);
    }

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
