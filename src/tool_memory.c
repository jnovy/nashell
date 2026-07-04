#include "tools_internal.h"
#include "memory.h"
#include "workspace.h"
#include "embedding.h"
#include "session_index.h"
#include "session_search.h"
#include "provider.h"
#include "llm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

/* ── LLM-based memory consolidation (GDN-2 P2) ──────── */
/* After storing a memory, check for semantically similar existing memories.
 * If found, concatenate both and call the LLM to produce a consolidated
 * version. This implements the "subtract-before-write" principle from GDN-2:
 * related content is merged rather than duplicated. */


/* Carry forward validation scores from a deleted/old entry to the surviving
 * entry.  When consolidation merges or replaces entries, the old entry's
 * recall evidence (hits/misses) must be summed into the survivor to prevent
 * a well-tested memory from losing its credibility after consolidation.
 * Without this, a merged entry starts at vscore=0.50 (uninformed prior)
 * and becomes vulnerable to immediate pruning. */
static void consolidation_carry_scores(memory_t *m,
                                       const char *survivor_key,
                                       int old_hits, int old_misses) {
    if (!m || !survivor_key || (old_hits == 0 && old_misses == 0)) return;

    /* memory_update_scores() handles both disk persistence (load JSON,
     * add counters, write back) and in-memory index update in one call.
     * Previously this function did its own load/modify/write cycle first,
     * then called memory_update_scores which did the same — resulting in
     * double-counting (old_hits added twice to disk). */
    memory_update_scores(m, survivor_key, old_hits, old_misses);
}

/* FIX D4: Returns strdup'd key to delete (caller collects for batch),
 * or NULL if no deletion needed.  Previously called memory_delete()
 * inline, causing O(N) gc_refs scan per delete.  Now the caller
 * batches all deletions into a single memory_delete_batch() call. */
char *tools_memory_try_consolidate(tool_ctx_t *ctx, const char *new_key,
                                   const char *new_value, memory_t *target) {
    if (!ctx->provider || !target) return NULL;
    if (!memory_has_embeddings(target)) return NULL;

    /* Load the multi-vec embedding for the new entry (just stored by
     * memory_embed_entry, which already produced chunked embeddings). */
    /* FIX #9: Use ".emb" extension directly instead of empty extension
     * + manual .emb append, which was fragile and inconsistent. */
    char new_emb_fname[512];
    key_to_path(new_key, ".emb", new_emb_fname, sizeof(new_emb_fname));
    char new_emb_path[NASH_PATH_MAX];
    snprintf(new_emb_path, sizeof(new_emb_path), "%s/%s",
             memory_dir(target), new_emb_fname);
    embed_multi_vec_t new_emb = embed_multi_vec_load(new_emb_path);
    if (!new_emb.data) return NULL;

    /* Scan in-memory index for high similarity (FIX P1-3).
     *
     * Previous: for_each_dir_entry() scanned .emb files on disk, creating
     * a consistency window where deleted-but-not-flushed entries could match,
     * or newly-stored entries without embeddings would be missed.
     *
     * Now: iterate over the authoritative in-memory index (m->idx), using
     * cached embeddings when available, falling back to disk load.
     *
     * Consolidation threshold: cosine similarity above which two memories
     * are considered near-duplicates and merged. 0.82 is conservative —
     * only genuinely redundant entries trigger consolidation.
     * Configurable via config.toml [limits] consolidation_threshold.
     *
     * Research basis: IR literature places "semantically equivalent"
     * text at cosine similarity 0.80-0.90 depending on embedding model.
     * See also: MemForest [arXiv:2605.23986] for temporal dedup. */
    float cons_threshold = ctx->cfg ? ctx->cfg->consolidation_threshold : 0.82f;
    char best_key[512] = {0};
    char best_path[NASH_PATH_MAX] = {0};
    float best_sim = 0.0f;

    /* FIX CRITICAL #1: Snapshot index keys and paths under the mutex,
     * then iterate the snapshot without holding the lock.  Previously
     * iterated m->idx.entries[] directly without the mutex — a concurrent
     * memory_delete (swap-remove) could cause use-after-free or OOB. */
    memory_t *m = target;
    typedef struct { char *key; char *path; int has_emb; embed_multi_vec_t emb; } consol_snap_t;
    int snap_count = 0;
    consol_snap_t *snap = NULL;
    {
        /* Build snapshot under memory mutex */
        pthread_mutex_lock(&m->mtx);
        snap_count = m->idx.count;
        if (snap_count > 0) {
            snap = calloc((size_t)snap_count, sizeof(consol_snap_t));
            if (snap) {
                for (int i = 0; i < snap_count; i++) {
                    mem_index_entry_t *e = &m->idx.entries[i];
                    snap[i].key = e->key ? strdup(e->key) : NULL;
                    snap[i].path = e->path ? strdup(e->path) : NULL;
                    snap[i].has_emb = e->has_emb;
                    /* Deep-copy embedding data for thread-safe access */
                    if (e->has_emb && e->emb.data) {
                        snap[i].emb.dim = e->emb.dim;
                        snap[i].emb.n_chunks = e->emb.n_chunks;
                        size_t emb_bytes = sizeof(float) * (size_t)e->emb.dim * (size_t)e->emb.n_chunks;
                        snap[i].emb.data = malloc(emb_bytes);
                        if (snap[i].emb.data)
                            memcpy(snap[i].emb.data, e->emb.data, emb_bytes);
                        else
                            snap[i].has_emb = 0;
                    }
                }
            }
        }
        pthread_mutex_unlock(&m->mtx);
    }
    if (!snap) { embed_multi_vec_free(&new_emb); return NULL; }

    for (int i = 0; i < snap_count; i++) {
        if (!snap[i].key) continue;

        /* Skip self — the entry we just stored */
        if (strcmp(snap[i].key, new_key) == 0) continue;

        /* Get embedding: prefer cached snapshot, fall back to disk */
        embed_multi_vec_t *emb_ptr = NULL;
        embed_multi_vec_t loaded_emb = {0};
        if (snap[i].has_emb && snap[i].emb.data) {
            emb_ptr = &snap[i].emb;
        } else if (snap[i].path) {
            char emb_path[NASH_PATH_MAX];
            snprintf(emb_path, sizeof(emb_path), "%s", snap[i].path);
            size_t plen = strlen(emb_path);
            if (plen >= 5 && strcmp(emb_path + plen - 5, ".json") == 0)
                strcpy(emb_path + plen - 5, ".emb");
            loaded_emb = embed_multi_vec_load(emb_path);
            if (loaded_emb.data) emb_ptr = &loaded_emb;
        }
        if (!emb_ptr) continue;

        /* Dimension check: skip stale embeddings from a different model */
        if (emb_ptr->dim != new_emb.dim) {
            if (loaded_emb.data) {
                /* Delete stale .emb file so memory_embed_all() regenerates it */
                char emb_path[NASH_PATH_MAX];
                snprintf(emb_path, sizeof(emb_path), "%s", snap[i].path);
                size_t plen = strlen(emb_path);
                if (plen >= 5 && strcmp(emb_path + plen - 5, ".json") == 0)
                    strcpy(emb_path + plen - 5, ".emb");
                unlink(emb_path);
                embed_multi_vec_free(&loaded_emb);
            }
            continue;
        }

        /* MaxSim across all chunk pairs */
        float sim = embed_cosine_sim_multi_multi(&new_emb, emb_ptr);
        if (loaded_emb.data) embed_multi_vec_free(&loaded_emb);

        if (sim > best_sim && sim > cons_threshold) {
            best_sim = sim;
            snprintf(best_key, sizeof(best_key), "%s", snap[i].key);
            if (snap[i].path)
                snprintf(best_path, sizeof(best_path), "%s", snap[i].path);
        }
    }
    /* Free snapshot */
    for (int i = 0; i < snap_count; i++) {
        free(snap[i].key);
        free(snap[i].path);
        if (snap[i].emb.data) free(snap[i].emb.data);
    }
    free(snap);

    embed_multi_vec_free(&new_emb);

    if (best_key[0] == '\0') return NULL;  /* no similar memory found */

    /* Load the similar memory's value */
    cJSON *old_entry = slurp_json(best_path);
    if (!old_entry) return NULL;

    cJSON *old_key_j = cJSON_GetObjectItem(old_entry, "key");
    cJSON *old_val_j = cJSON_GetObjectItem(old_entry, "value");
    if (!old_key_j || !old_val_j ||
        !old_key_j->valuestring || !old_val_j->valuestring) {
        cJSON_Delete(old_entry);
        return NULL;
    }

    const char *old_key = old_key_j->valuestring;
    const char *old_value = old_val_j->valuestring;

    /* Don't consolidate pinned memories */
    cJSON *pinned_j = cJSON_GetObjectItem(old_entry, "pinned");
    if (pinned_j && cJSON_IsTrue(pinned_j)) {
        cJSON_Delete(old_entry);
        return NULL;
    }

    /* Extract old entry's validation scores BEFORE any branch deletes it.
     * These will be carried forward to the surviving entry. */
    cJSON *old_rh = cJSON_GetObjectItem(old_entry, "recall_hits");
    cJSON *old_rm = cJSON_GetObjectItem(old_entry, "recall_misses");
    int old_hits = old_rh ? (int)cJSON_GetNumberValue(old_rh) : 0;
    int old_misses = old_rm ? (int)cJSON_GetNumberValue(old_rm) : 0;

    /* Build classify-then-act prompt.
     * Instead of blindly merging, ask the LLM to classify the relationship:
     * - SUPERSEDES: new entry corrects/updates old → delete old, keep new
     * - COMPLEMENTARY: entries cover different aspects → keep both
     * - REDUNDANT: entries say the same thing → merge into one
     * This prevents contradictory entries from being merged into incoherent
     * mush, and allows corrective insights to properly replace outdated ones.
     *
     * FIX B10: Include key lengths in allocation — the format string
     * interpolates new_key and old_key too, which could be up to 256
     * chars each. */
    size_t prompt_sz = strlen(new_value) + strlen(old_value) +
                     strlen(new_key) + strlen(old_key) + 2048;
    char *prompt = malloc(prompt_sz);
    if (!prompt) { cJSON_Delete(old_entry); return NULL; }
    snprintf(prompt, prompt_sz,
        "Two memory entries are semantically similar. Classify their relationship "
        "and act accordingly.\n\n"
        "--- NEW entry (key: %s) ---\n%s\n\n"
        "--- EXISTING entry (key: %s) ---\n%s\n\n"
        "First, classify the relationship as exactly one of:\n"
        "SUPERSEDES — the NEW entry corrects, updates, or invalidates the EXISTING entry "
        "(e.g. opposite advice, updated procedure, refined understanding)\n"
        "COMPLEMENTARY — the entries cover different aspects of the same topic "
        "(e.g. different failure modes, different contexts, different techniques)\n"
        "REDUNDANT — the entries say essentially the same thing with different wording\n\n"
        "Output format:\n"
        "Line 1: SUPERSEDES or COMPLEMENTARY or REDUNDANT\n"
        "Line 2+: If REDUNDANT, output the merged text (concise, preserve unique info). "
        "If SUPERSEDES or COMPLEMENTARY, output nothing more.",
        new_key, new_value, old_key, old_value);

    /* Call LLM for classification + optional merge */
    llm_chat_t *chat = llm_chat_new();
    llm_chat_add(chat, "system",
        "You are a memory consistency assistant. Classify the relationship between "
        "two memory entries and, if redundant, merge them. Be precise: entries that "
        "give OPPOSITE advice for the same situation are SUPERSEDES, not REDUNDANT.");
    llm_chat_add(chat, "user", prompt);
    free(prompt);

    llm_stats_t stats = {0};
    char *response = NULL;

    /* FIX CRIT#2: Deep-copy string fields to prevent dangling pointers if
     * the original provider is freed/modified concurrently (e.g. model switch
     * during playbook pass). provider_create() strdup's its input, but the
     * input itself must be valid at the time of the call.
     * FIX MED#10: Inherit llm_timeout to prevent indefinite blocking. */
    provider_config_t cons_cfg = ctx->provider->cfg;
    cons_cfg.model_id    = cons_cfg.model_id    ? strdup(cons_cfg.model_id)    : NULL;
    cons_cfg.api_base    = cons_cfg.api_base    ? strdup(cons_cfg.api_base)    : NULL;
    cons_cfg.api_key_env = cons_cfg.api_key_env ? strdup(cons_cfg.api_key_env) : NULL;
    cons_cfg.project_id  = cons_cfg.project_id  ? strdup(cons_cfg.project_id)  : NULL;
    cons_cfg.region      = cons_cfg.region      ? strdup(cons_cfg.region)      : NULL;
    cons_cfg.max_tokens = 2048;
    cons_cfg.temperature = 0.1f;
    cons_cfg.enable_thinking = 0;
    cons_cfg.thinking_budget = 0;
    if (cons_cfg.llm_timeout == 0) cons_cfg.llm_timeout = 120; /* FIX MED#10: default 2min timeout */
    provider_t *cons_provider = provider_create(&cons_cfg);
    /* Free our temporary strdup'd copies (provider_create strdup's again) */
    free((void *)cons_cfg.model_id);
    free((void *)cons_cfg.api_base);
    free((void *)cons_cfg.api_key_env);
    free((void *)cons_cfg.project_id);
    free((void *)cons_cfg.region);
    if (cons_provider) {
        response = provider_complete(cons_provider, chat, &stats);
        provider_free(cons_provider);
    }
    llm_chat_free(chat);

    if (!response || strlen(response) < 5) {
        free(response);
        cJSON_Delete(old_entry);
        return NULL;
    }

    /* Parse classification from first line of response */
    if (strncmp(response, "COMPLEMENTARY", 13) == 0) {
        /* Entries cover different aspects — keep both, do nothing */
        free(response);
        cJSON_Delete(old_entry);
        return NULL;
    }

    if (strncmp(response, "SUPERSEDES", 10) == 0) {
        /* New entry corrects/updates old — delete old, keep new as-is.
         * FIX D4: Return key for batch deletion instead of inline delete. */
        char *del_key = (strcmp(old_key, new_key) != 0) ? strdup(old_key) : NULL;
        /* Carry forward old entry's validation evidence to the new entry.
         * The new insight earned the old one's credibility by replacing it. */
        consolidation_carry_scores(target, new_key,
                                   old_hits, old_misses);
        free(response);
        cJSON_Delete(old_entry);
        return del_key;
    }

    /* Default: REDUNDANT — merge (extract text after first newline) */
    {
        char *merged = NULL;
        char *newline = strchr(response, '\n');
        if (newline) {
            /* Skip the classification line and any leading whitespace */
            newline++;
            while (*newline == '\n' || *newline == '\r' || *newline == ' ')
                newline++;
            if (strlen(newline) >= 20) {
                merged = strdup(newline);
            }
        }
        if (!merged) {
            /* Couldn't extract merged text — skip */
            free(response);
            cJSON_Delete(old_entry);
            return NULL;
        }
        free(response);

        /* Preserve journal_ref provenance from the new entry.
         * FIX B1: Without this, consolidated memories lose provenance. */
        char new_fname[512];
        key_to_path(new_key, ".json", new_fname, sizeof(new_fname));
        char new_json_path[NASH_PATH_MAX];
        snprintf(new_json_path, sizeof(new_json_path), "%s/%s",
                 memory_dir(target), new_fname);

        const char *new_jref = NULL;
        cJSON *new_entry_json = slurp_json(new_json_path);
        if (new_entry_json) {
            cJSON *jr = cJSON_GetObjectItem(new_entry_json, "journal_ref");
            if (jr && jr->valuestring) new_jref = jr->valuestring;
        }

        /* Store merged version under the new key */
        memory_store(target, new_key, merged,
                     0, new_jref, NULL, 0);

        cJSON_Delete(new_entry_json);

        /* FIX D4: Return key for batch deletion instead of inline delete. */
        char *del_key = (strcmp(old_key, new_key) != 0) ? strdup(old_key) : NULL;

        /* Carry forward old entry's validation evidence to the merged result.
         * memory_store() above preserved the new entry's counters (same key
         * overwrite), but the old entry's counters were lost via delete.
         * Sum both entries' evidence into the survivor. */
        consolidation_carry_scores(target, new_key,
                                   old_hits, old_misses);

        free(merged);
        cJSON_Delete(old_entry);
        return del_key;
    }
    /* FIX BUG#3: removed unreachable cJSON_Delete that was after the
     * unconditional return in the REDUNDANT block above. */
    return NULL;  /* unreachable — silences compiler warning */
}

/* ── memory_store ──────────────────────────────────────── */

tool_result_t tool_memory_store(tool_ctx_t *ctx, cJSON *params) {
    cJSON *key_j = cJSON_GetObjectItem(params, "key");
    cJSON *val_j = cJSON_GetObjectItem(params, "value");
    if (!key_j || !key_j->valuestring || !key_j->valuestring[0])
        return tools_make_error("memory_store requires a non-empty 'key' string. "
                          "Use format 'type:descriptive-name' (e.g. lesson:config-sentinel-values).");
    if (!val_j || !val_j->valuestring || !val_j->valuestring[0])
        return tools_make_error("memory_store requires a non-empty 'value' string. "
                          "Provide the knowledge to store.");

    const char *key = key_j->valuestring;
    const char *value = val_j->valuestring;

    int pinned = 0;
    cJSON *pin_j = cJSON_GetObjectItem(params, "pinned");
    if (pin_j && cJSON_IsTrue(pin_j)) pinned = 1;

    /* Build journal provenance reference: "session_dir/journal.jsonl:R<loop>" */
    char jref[NASH_PATH_MAX];
    if (ctx->session_dir && ctx->journal) {
        snprintf(jref, sizeof(jref), "%s/journal.jsonl:R%d",
                 ctx->session_dir, ctx->react_loop);
    } else {
        jref[0] = '\0';
    }

    /* Parse refs (comma-separated string of related memory keys).
     * Inter-memory relationships for "see also" links.
     * Research: MemForest [arXiv:2605.23986], ActiveGraph [arXiv:2605.21997],
     * MemIR [arXiv:2605.25869] — all validate graph-structured memory. */
    const char *refs_arr[32];
    int n_refs = 0;
    cJSON *refs_j = cJSON_GetObjectItem(params, "refs");
    char *refs_copy = NULL;
    if (refs_j) {
        if (cJSON_IsArray(refs_j)) {
            /* Handle JSON array format: ["key1", "key2"] */
            cJSON *item;
            cJSON_ArrayForEach(item, refs_j) {
                if (item->valuestring && n_refs < 32)
                    refs_arr[n_refs++] = item->valuestring;
            }
        } else if (refs_j->valuestring) {
            /* Handle legacy comma-separated string format */
            refs_copy = strdup(refs_j->valuestring);
            char *saveptr = NULL;
            char *tok = strtok_r(refs_copy, ",", &saveptr);
            while (tok && n_refs < 32) {
                while (*tok == ' ') tok++;  /* trim leading space */
                refs_arr[n_refs++] = tok;
                tok = strtok_r(NULL, ",", &saveptr);
            }
        }
    }

    /* Workspace routing: 'global' parameter forces store to global memory */
    int force_global = 0;
    cJSON *glob_j = cJSON_GetObjectItem(params, "global");
    if (glob_j && cJSON_IsTrue(glob_j)) force_global = 1;

    int rc;
    if (ctx->ws) {
        rc = workspace_store(ctx->ws, key, value, pinned,
                             jref[0] ? jref : NULL,
                             n_refs > 0 ? refs_arr : NULL, n_refs,
                             force_global);
    } else {
        rc = memory_store(ctx->memory, key, value, pinned,
                          jref[0] ? jref : NULL,
                          n_refs > 0 ? refs_arr : NULL, n_refs);
    }
    free(refs_copy);

    if (rc != 0) return tools_make_error("failed to store memory");

    /* P2: Lesson lineage — if 'supersedes' is provided, set the lineage chain.
     * Self-Harness [arXiv:2606.09498] — harness lineage h₀→h₁→h₂. */
    cJSON *sup_j = cJSON_GetObjectItem(params, "supersedes");
    if (sup_j && sup_j->valuestring && sup_j->valuestring[0]) {
        if (ctx->ws)
            workspace_set_supersedes(ctx->ws, key, sup_j->valuestring);
        else
            memory_set_supersedes(ctx->memory, key, sup_j->valuestring);
    }

    /* Belief Entropy probe — compute ℋ_BE for the new memory entry.
     * Only runs when enabled in config AND provider is local (has /completion).
     * The probe is lightweight (~30 tokens) and non-blocking on failure. */
    if (ctx->cfg->belief_entropy.enabled && ctx->provider &&
        ctx->provider->type == PROVIDER_LOCAL) {
        belief_entropy_config_t *bec = &ctx->cfg->belief_entropy;
        belief_entropy_result_t be = llm_belief_entropy_probe(
            ctx->provider->cfg.api_base,
            value,  /* memory content as context */
            bec->anchor_question,
            bec->probe_tokens,
            bec->probe_n_probs,
            bec->probe_temperature);
        if (be.ok) {
            if (ctx->ws)
                workspace_set_belief_entropy(ctx->ws, key, (double)be.h_mean);
            else
                memory_set_belief_entropy(ctx->memory, key, (double)be.h_mean);
        }
    }

    /* FIX CRIT1: Defer consolidation to post-task instead of blocking inline.
     * Previously, memory_try_consolidate() ran a synchronous LLM call here,
     * adding 5-30s latency on the hot path. Now we queue the key+value pair
     * and process them all in tool_flush_deferred_consolidations() after
     * the react loop completes. */
    /* Determine which memory_t the entry was stored in, for consolidation. */
    memory_t *store_target = ctx->ws
        ? workspace_find_memory(ctx->ws, key) : ctx->memory;
    if (!store_target) store_target = ctx->memory;

    if (!pinned && store_target &&
        !atomic_load(&store_target->consolidating)) {
        /* FIX BUG#13: Cap deferred queue at 64 entries to bound memory usage.
         * Oldest entries are dropped if the queue is full -- they'll be
         * consolidated on the next session anyway via memory_embed_all. */
        #define DEFERRED_CONSOL_MAX 64
        if (ctx->n_deferred_consol < DEFERRED_CONSOL_MAX) {
            /* Grow deferred queue if needed */
            if (ctx->n_deferred_consol >= ctx->cap_deferred_consol) {
                int new_cap = ctx->cap_deferred_consol ? ctx->cap_deferred_consol * 2 : 16;
                if (new_cap > DEFERRED_CONSOL_MAX) new_cap = DEFERRED_CONSOL_MAX;
                void *tmp = realloc(ctx->deferred_consol,
                                    (size_t)new_cap * sizeof(ctx->deferred_consol[0]));
                if (tmp) {
                    ctx->deferred_consol = tmp;
                    ctx->cap_deferred_consol = new_cap;
                }
            }
            if (ctx->n_deferred_consol < ctx->cap_deferred_consol) {
                ctx->deferred_consol[ctx->n_deferred_consol].key = strdup(key);
                ctx->deferred_consol[ctx->n_deferred_consol].value = strdup(value);
                ctx->deferred_consol[ctx->n_deferred_consol].target = store_target;
                ctx->n_deferred_consol++;
            }
        }
    }

    /* Store for audit */
    char *hash = store_save(ctx->store, value);
    char *alias = tool_register_alias(ctx, hash ? hash : "");

    cJSON *meta = cJSON_CreateObject();
    cJSON_AddStringToObject(meta, "status", "ok");
    cJSON_AddStringToObject(meta, "key", key);
    if (alias) cJSON_AddStringToObject(meta, "ref", alias);

    tools_inject_thought(ctx, params);
    tool_journal(ctx, "memory_store",
                   params, alias, strlen(value), 0, NULL, NULL);

    char *ref_copy = alias ? strdup(alias) : NULL;
    free(alias);
    free(hash);
    return tools_make_result(1, meta, ref_copy);
}

/* ── memory_search (v4.4 unified: curated memory + session history) ── */
/* Single search tool that replaces both memory_query and session_search.
 * Searches curated L4 memory (lessons, skills, strategies, facts) and
 * L3 session history (journal.jsonl), ranks all results by relevance,
 * and returns an interleaved result set labeled by source. */

tool_result_t tool_memory_search(tool_ctx_t *ctx, cJSON *params) {
    /* Extract parameters */
    const char *query = NULL;
    cJSON *query_j = cJSON_GetObjectItem(params, "query");
    if (query_j && query_j->valuestring && query_j->valuestring[0])
        query = query_j->valuestring;

    const char *key = NULL;
    cJSON *key_j = cJSON_GetObjectItem(params, "key");
    if (key_j && key_j->valuestring && key_j->valuestring[0])
        key = key_j->valuestring;

    const char *pattern = NULL;
    cJSON *pattern_j = cJSON_GetObjectItem(params, "pattern");
    if (pattern_j && pattern_j->valuestring && pattern_j->valuestring[0])
        pattern = pattern_j->valuestring;

    if (!query && !key && !pattern)
        return tools_make_error(
            "memory_search requires at least one of: 'query' (semantic search), "
            "'key' (exact memory key), or 'pattern' (lexical/regex search).");

    int use_regex = 0;
    cJSON *regex_j = cJSON_GetObjectItem(params, "regex");
    if (regex_j && cJSON_IsTrue(regex_j))
        use_regex = 1;

    int max_results = 0;  /* 0 = use defaults per source */
    cJSON *max_j = cJSON_GetObjectItem(params, "max_results");
    if (max_j && cJSON_IsNumber(max_j)) {
        max_results = max_j->valueint;
        if (max_results < 1) max_results = 1;
        if (max_results > 100) max_results = 100;
    }

    int days = 0;
    cJSON *days_j = cJSON_GetObjectItem(params, "days");
    if (days_j && cJSON_IsNumber(days_j))
        days = days_j->valueint > 0 ? days_j->valueint : 0;

    str_t out = str_new(4096);
    int mem_count = 0, ses_count = 0, total_lexical_matches = 0;

    /* ── Exact key lookup (bypasses scoring) ──────── */
    memory_results_t mem_results = {0};
    if (key && !query && !pattern) {
        /* Direct key recall -- use memory_find() to bypass scoring entirely.
         * Previously used memory_query() which applied min_score filtering,
         * so entries with bad vscore could fail exact key lookup. */
        if (ctx->memory || ctx->ws) {
            memory_t *target_mem = ctx->ws
                ? workspace_find_memory(ctx->ws, key)
                : ctx->memory;
            mem_index_entry_t *ie = target_mem
                ? memory_find(target_mem, key) : NULL;
            if (ie && ie->value) {
                str_appendf(&out, "[MEMORY -- %s]\n%s\n\n", ie->key, ie->value);
                tool_track_recalled_key(ctx, ie->key);
                mem_count = 1;
            }
            memory_find_free(ie);
        }
        if (mem_count == 0)
            str_appendf(&out, "(no memory found for key \"%s\")\n", key);
        goto finish;
    }

    /* ── L4: Curated memory search ──────────────── */
    if (query && (ctx->memory || ctx->ws)) {
        int mem_max = max_results > 0 ? max_results : 5;
        mem_results = ctx->ws
            ? workspace_recall(ctx->ws, query, mem_max)
            : memory_query(ctx->memory, query, mem_max);
        mem_count = mem_results.count;
    }

    /* ── L3: Session history search ─────────────── */
    ss_results_t ses_results = {0};
    if (query || pattern) {
        /* Derive sessions_dir from session_dir (parent) */
        char sessions_dir[NASH_PATH_MAX] = {0};
        if (ctx->session_dir) {
            snprintf(sessions_dir, sizeof(sessions_dir), "%s", ctx->session_dir);
            char *last_slash = strrchr(sessions_dir, '/');
            if (last_slash) *last_slash = '\0';
        }

        embed_ctx_t *embed = NULL;
        if (ctx->memory && memory_has_embeddings(ctx->memory))
            embed = memory_embed_ctx(ctx->memory);

        /* Adjust session count based on memory results */
        int ses_max = max_results > 0 ? max_results : 5;
        if (!pattern && mem_count >= 3 && ses_max > 2) ses_max = 2;

        ses_results = session_search(
            ctx->session_idx, embed, query, pattern,
            use_regex, ses_max, days,
            sessions_dir[0] ? sessions_dir : NULL);

        ses_count = ses_results.count;
        total_lexical_matches = ses_results.total_matches;
    }

    /* ── Interleave results by score ────────────── */
    /* Build a merged index: memory entries have relevance [0,1],
     * session entries have composite_score [0,1]. Walk both arrays
     * in descending score order. */
    {
        int mi = 0, si = 0;
        int total_emitted = 0;
        int emit_limit = max_results > 0 ? max_results : 10;  /* default: mem(5) + ses(5) */

        static const char *conf_labels[] = {"LOW", "MEDIUM", "HIGH"};

        while (total_emitted < emit_limit && (mi < mem_count || si < ses_count)) {
            double mem_score = (mi < mem_count)
                ? mem_results.entries[mi].relevance : -1.0;
            double ses_score = (si < ses_count)
                ? ses_results.results[si].composite_score : -1.0;

            if (mem_score >= ses_score && mi < mem_count) {
                /* Emit memory result (>= means memory wins ties — intentional:
                 * curated memory is higher quality than raw session logs) */
                memory_entry_t *e = &mem_results.entries[mi];
                str_appendf(&out, "[MEMORY — %s]\n%s\n\n", e->key, e->value);
                tool_track_recalled_key(ctx, e->key);
                mi++;
            } else if (si < ses_count) {
                /* Emit session result */
                ss_result_t *r = &ses_results.results[si];
                time_t ts = (time_t)r->timestamp;
                struct tm tm_buf;
                struct tm *tm = gmtime_r(&ts, &tm_buf);
                char ts_buf[32];
                if (tm) strftime(ts_buf, sizeof(ts_buf), "%Y-%m-%d %H:%M", tm);
                else    snprintf(ts_buf, sizeof(ts_buf), "%.0f", r->timestamp);

                /* Determine output detail level based on whether we have
                 * lexical matches (pattern-driven) or just semantic */
                int has_lex_matches = (r->n_matches > 0);
                int has_semantic = (r->semantic_score > 0.01);

                if (has_lex_matches || has_semantic) {
                    str_appendf(&out, "[SESSION — %s  %s  score=%.3f",
                                ts_buf, conf_labels[r->confidence],
                                r->composite_score);
                    if (has_semantic)
                        str_appendf(&out, " sem=%.2f", r->semantic_score);
                    if (has_lex_matches)
                        str_appendf(&out, " lex=%.2f matches=%d",
                                    r->lexical_score, r->match_count);
                    str_appendf(&out, "]\n");
                    str_appendf(&out, "    %s\n",
                                r->session_dir ? r->session_dir : "");

                    /* Show chunk preview (semantic) */
                    if (r->chunk_preview && r->chunk_preview[0]) {
                        str_appendf(&out, "  %s\n", r->chunk_preview);
                    }

                    /* Show per-line lexical matches */
                    for (int j = 0; j < r->n_matches; j++) {
                        ss_match_t *m = &r->matches[j];
                        str_appendf(&out, "  R%dS%d [%s]: %s\n",
                                    m->react_loop, m->step, m->tool,
                                    m->snippet ? m->snippet : "");
                    }
                    if (r->match_count > r->n_matches) {
                        str_appendf(&out, "  ... and %d more match%s\n",
                                    r->match_count - r->n_matches,
                                    (r->match_count - r->n_matches) == 1 ? "" : "es");
                    }
                } else {
                    str_appendf(&out,
                        "[SESSION — %s]\n    %s\n"
                        "  -> file_read %s/journal.jsonl for details\n",
                        ts_buf,
                        r->session_dir ? r->session_dir : "",
                        r->session_dir ? r->session_dir : "");
                }
                str_appendf(&out, "\n");
                si++;
            }
            total_emitted++;
        }

        if (total_emitted == 0)
            str_appendf(&out, "(no matches)\n");
    }

finish:;
    int total_results = mem_count + ses_count;

    /* Store result */
    char *hash = store_save(ctx->store,
                            out.len > 0 ? out.data : "(no matches)");
    char *alias = tool_register_alias(ctx, hash ? hash : "");

    cJSON *meta = cJSON_CreateObject();
    cJSON_AddNumberToObject(meta, "matches", total_results);
    if (mem_count > 0)
        cJSON_AddNumberToObject(meta, "memories", mem_count);
    if (ses_count > 0)
        cJSON_AddNumberToObject(meta, "sessions", ses_count);
    if (total_lexical_matches > 0)
        cJSON_AddNumberToObject(meta, "lexical_matches", total_lexical_matches);
    cJSON_AddStringToObject(meta, "ref", alias);

    tools_inject_thought(ctx, params);
    tool_journal(ctx, "memory_search",
                   params, alias, out.len, total_results, NULL, NULL);

    memory_results_free(&mem_results);
    if (ses_count > 0) ss_results_free(&ses_results);
    char *ref_copy = strdup(alias);
    free(alias);
    free(hash);
    str_free(&out);
    return tools_make_result(1, meta, ref_copy);
}


/* ── memory_pin / memory_unpin / memory_delete ─────────── */

tool_result_t tool_memory_pin(tool_ctx_t *ctx, cJSON *params) {
    return tools_memory_key_op(ctx, params, "memory_pin",
        "memory_pin", "pinned",
        "Pinned memories alter system prompt for all future sessions. "
        "Validate with: nash --regression --validate-harness compare",
        workspace_pin, memory_pin);
}

tool_result_t tool_memory_unpin(tool_ctx_t *ctx, cJSON *params) {
    return tools_memory_key_op(ctx, params, "memory_unpin",
        "memory_unpin", "unpinned", NULL,
        workspace_unpin, memory_unpin);
}

tool_result_t tool_memory_delete(tool_ctx_t *ctx, cJSON *params) {
    return tools_memory_key_op(ctx, params, "memory_delete",
        "memory_delete", "deleted", NULL,
        workspace_delete, memory_delete);
}


